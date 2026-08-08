/* SP-1 Remote: a 4-fader / 4-button TRS MIDI controller.
 *
 * Phase 2 skeleton: boots, shows life, charges, powers down cleanly, and can
 * always be recovered. No MIDI yet, deliberately. Nothing is worth debugging
 * on top of an uncertain bring-up.
 *
 * BOOTLOADER SAFETY (the SP-1 "BIG FIVE", inherited from sp1-tape-looper):
 *   the app lives at 0x20000; the watchdog is fed every control pass;
 *   bootloader-owned clocks and peripherals are not re-initialised;
 *   SYSTEM_OFF is the only power-down path; RESETREAS is cleared at boot and
 *   again before SYSTEM_OFF. There is no reset pin on this hardware.
 *
 * The escape hatch below is the reason this firmware can be flashed onto
 * scarce hardware with a straight face: holding Track 1 + Track 4 resets
 * into the bootloader even if everything else here is wedged.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include "board_io.h"
#include "midi_tx.h"
#include "controls.h"
#include "profile.h"
#include "buttons.h"
#include "presets.h"
#include "config_console.h"

#define SP1_DIAG 1

#define CONTROL_PERIOD_MS 5
#define POWER_HOLD_MS     2500

/* Track 1 + Track 4 read as their own ADC band, ~1325, which sits INSIDE
 * track 4's decode range (950..1500). It must therefore be tested BEFORE
 * decoding buttons, or the recovery gesture reads as a track-4 press.
 * Source: sp1-tape-looper main.c:6964-6981. */
#define COMBO14_LO       1280
#define COMBO14_HI       1390
#define COMBO14_HOLD_MS  1200

/* Consecutive failed ADC reads tolerated before the ladder reports "nothing
 * pressed". Holding the last value forever would turn a transient failure
 * into a phantom press. */
#define LADDER_ERR_LIMIT 10

profile_t              g_profile;      /* the live, editable mapping */
static fader_state_t   g_fader[PROFILE_NUM_FADERS];
static btn_state_t     g_btn[PROFILE_NUM_BUTTONS];
static button_engine_t g_eng;
static int             g_last_raw;      /* for the diagnostic line only */
static uint32_t        g_save_blink_until;
static bool            g_save_ok;

/* Read the track ladder, handle the recovery combo, and debounce.
 *
 * The Track 1 + Track 4 combo reads ~1325, which sits INSIDE track 4's
 * decode band (950..1500), so it MUST be tested before decoding or the
 * rescue gesture fires a preset save instead of reaching the bootloader.
 * Measured on pop: an isolated track-4 press peaks at 1234 and the combo
 * lands at 1322-1329, so the 1280 boundary has 46 counts of margin.
 *
 * Debounce: the buttons share one noisy resistor ladder, so a single read
 * at a band edge can name the wrong button. Commit only after three
 * agreeing passes, as the looper does (main.c:7513-7519).
 *
 * ADC errors are tolerated briefly, then reported as "nothing pressed":
 * holding the last value forever would turn a transient failure into a
 * phantom press, which for a toggle means freeze flipping on its own. */
/* Snapshot what the puck last SENT, which is all it knows: MIDI here is
 * one-way, so anything changed from Push 3 or the synth's own menu is not
 * captured. Accepted in the design. Freeze is excluded by the profile's
 * capture list, since a scene should not re-trigger a live gesture.
 *
 * Every write is bounds-checked individually, not just the outer loop: a
 * generic profile may map one CC to two faders, and then a single capture
 * entry produces two messages. */
static int snapshot_surface(const profile_t *prof, cc_msg_t *out, int out_max)
{
	int n = 0;

	for (int c = 0; c < prof->preset_capture_len; c++) {
		uint8_t cc = prof->preset_capture[c];

		for (int f = 0; f < PROFILE_NUM_FADERS && n < out_max; f++) {
			if (prof->fader[f].cc == cc && g_fader[f].have_sent) {
				out[n].channel = prof->fader[f].channel;
				out[n].cc      = cc;
				out[n].value   = g_fader[f].last_sent;
				n++;
			}
		}
		for (int b = 0; b < PROFILE_NUM_BUTTONS && n < out_max; b++) {
			const button_cfg_t *cfg = &prof->button[b];

			if (cfg->mode == BTN_MODE_CYCLE && cfg->cc == cc) {
				out[n].channel = cfg->channel;
				out[n].cc      = cc;
				out[n].value   =
					cfg->steps[button_engine_step_index(&g_eng, b)];
				n++;
			}
		}
		if (n >= out_max) {
			break;
		}
	}
	return n;
}

/* preset_store_save takes BOTH slots, and the engine holds them separately,
 * so the bank has to be assembled. Saving one slot alone would blank the
 * other. */
static bool persist_all_slots(void)
{
	preset_bank_t bank;

	memset(&bank, 0, sizeof(bank));
	for (int sl = 0; sl < BUTTON_MAX_PRESET_SLOTS; sl++) {
		const preset_slot_t *ps = button_engine_get_preset(&g_eng, sl);

		if (ps != NULL) {
			bank.slot[sl] = *ps;
		}
	}
	return preset_store_save(&bank);
}

/* A recall moves the synth out from under the faders and the cycle button.
 * Without this the next ADC pass sees a fader where it was, decides that
 * differs, and instantly undoes the scene; and shimmer's LED would lie. */
static void resync_after_recall(const profile_t *prof,
				const cc_msg_t *msgs, int n)
{
	for (int m = 0; m < n; m++) {
		for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
			if (prof->fader[f].cc == msgs[m].cc &&
			    prof->fader[f].channel == msgs[m].channel) {
				fader_arm_pickup(&g_fader[f], msgs[m].value);
			}
		}
		for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
			if (prof->button[b].mode == BTN_MODE_CYCLE &&
			    prof->button[b].cc == msgs[m].cc &&
			    prof->button[b].channel == msgs[m].channel) {
				button_engine_sync_cycle(&g_eng, b, msgs[m].value);
			}
		}
	}
}

static int debounced_track_button(void)
{
	static int     committed = -1, candidate = -1, count;
	static int64_t combo14_t = -1;
	static int     err_run;

	int raw = board_io_read_track_ladder();

	g_last_raw = raw;

	if (raw < 0) {
		if (++err_run < LADDER_ERR_LIMIT) {
			return committed;
		}
		committed = candidate = -1;
		count = 0;
		combo14_t = -1;
		return committed;
	}
	err_run = 0;

	if (raw >= COMBO14_LO && raw <= COMBO14_HI) {
		if (combo14_t < 0) {
			combo14_t = k_uptime_get();
		} else if (k_uptime_get() - combo14_t >= COMBO14_HOLD_MS) {
			board_io_enter_dfu();          /* does not return */
		}
		committed = candidate = -1;        /* never a button in-band */
		count = 0;
		return committed;
	}
	combo14_t = -1;

	int b = board_io_decode_track_button(raw);

	if (b == committed) {
		count = 0;
	} else if (b == candidate) {
		if (++count >= 3) {
			committed = b;
			count = 0;
		}
	} else {
		candidate = b;
		count = 1;
	}
	return committed;
}

int main(void)
{
	/* The live profile is a RAM copy, so it can be edited over the console
	 * and persisted. The compiled-in table is always the fallback: if flash
	 * holds nothing valid, or a stored record is rejected, the puck comes
	 * up with the shipped mapping rather than nothing. */
	g_profile = profile_popgoblin_default;
	(void)profile_store_load(&g_profile);

	const profile_t *prof = &g_profile;

	board_io_init();
	midi_tx_init();
	config_console_init();
	button_engine_init(&g_eng, prof);

	/* Load stored scenes. Silent: loading must not transmit, the puck
	 * says nothing until a control is touched. */
	{
		preset_bank_t bank;

		if (preset_store_load(&bank)) {
			for (int sl = 0; sl < BUTTON_MAX_PRESET_SLOTS; sl++) {
				button_engine_set_preset(&g_eng, sl,
							 bank.slot[sl].msg,
							 bank.slot[sl].len);
			}
		}
	}

	uint32_t held_since = 0u;

	for (;;) {
		board_io_feed_wdt();
		config_console_poll();

		int pressed = debounced_track_button();

		/* ---- power off on a long function-button hold ----
		 * Timed from uptime, not by adding CONTROL_PERIOD_MS per pass:
		 * the loop's real period is work plus 5 ms, and a diagnostic
		 * pass, a page dump or a flash write all make it longer, so
		 * counting iterations would stretch a nominal 2.5 s hold. */
		if (board_io_function_held()) {
			uint32_t now_ms = (uint32_t)k_uptime_get();

			if (held_since == 0u) {
				held_since = now_ms ? now_ms : 1u;
			} else if ((uint32_t)(now_ms - held_since) >= POWER_HOLD_MS) {
				board_io_power_off();       /* does not return */
			}
		} else {
			held_since = 0u;
		}

		/* ---- panel readout ----
		 * These LEDs show the PUCK's own state. MIDI here is one-way and
		 * there is no readback, so nothing on this panel can prove the
		 * synth agrees: it equals the synth only while the puck is the
		 * authoritative controller for these CCs.
		 *
		 * The side column mirrors each fader's last sent value. The
		 * renderer is a shadow mask with a global dim, not per-LED PWM,
		 * so board_io_led_set quantises to three levels: off, ghost, on.
		 * A fader in soft pickup blinks instead, since it is not in
		 * control yet and the performer needs to know. */
		{
			uint32_t now_ms = (uint32_t)k_uptime_get();
			bool     blink  = ((now_ms / 200) & 1) != 0;

			for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
				uint8_t level;

				if (fader_pickup_armed(&g_fader[i])) {
					level = blink ? 255u : 0u;
				} else if (!g_fader[i].have_sent) {
					level = 0u;
				} else {
					level = (uint8_t)((g_fader[i].last_sent * 255) / 127);
				}
				board_io_led_set(i, level);
			}

			/* Track row: freeze latched, shimmer off or not, and whether
			 * each preset slot holds a scene. */
			/* A save confirms itself: three fast blinks across the
			 * track row when it worked, a single long one when the
			 * write failed. A silent failure here would be invisible
			 * until the scene did not come back. */
			if (now_ms < g_save_blink_until) {
				bool on = g_save_ok ? (((now_ms / 100) & 1) != 0)
						    : true;

				for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
					board_io_track_led_set(b, on);
				}
			} else {
			board_io_track_led_set(0, button_engine_is_latched(&g_eng, 0));
			board_io_track_led_set(1, button_engine_step_index(&g_eng, 1) != 0);
			for (int b = 2; b < PROFILE_NUM_BUTTONS; b++) {
				const preset_slot_t *ps =
					button_engine_get_preset(&g_eng,
								 prof->button[b].preset_slot);

				board_io_track_led_set(b, ps != NULL && ps->len > 0);
			}
			}
		}

#if SP1_DIAG
		static int diag_div;
		if (!g_diag_quiet && ++diag_div >= 40) {                 /* every 200 ms */
			diag_div = 0;
			printk("f0=%4d f1=%4d f2=%4d f3=%4d  lad=%4d btn=%d  "
			       "batt=%4d usb=%d chg=%d  "
			       "push=%u drain=%u usbtx=%u rdy=%d\n",
			       board_io_read_fader(0), board_io_read_fader(1),
			       board_io_read_fader(2), board_io_read_fader(3),
			       g_last_raw, board_io_decode_track_button(g_last_raw),
			       board_io_read_battery(),
			       board_io_usb_present() ? 1 : 0,
			       board_io_charging() ? 1 : 0,
			       midi_tx_pushed(), midi_tx_drained(),
			       midi_tx_usb_sent(), midi_tx_usb_ready() ? 1 : 0);
		}
#endif

		/* ---- faders -> CC, through the coalescing queue ----
		 * All four are read every pass. The looper round-robins them
		 * only because its blocking ADC reads competed with an eMMC
		 * streamer; nothing competes here. */
		for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
			uint8_t v;

			if (fader_update(&g_fader[i], board_io_read_fader(i), &v)) {
				midi_tx_send((cc_msg_t){ prof->fader[i].channel,
							 prof->fader[i].cc, v });
			}
		}

		/* ---- buttons -> CC lists ----
		 * Only one ladder button can be read at a time by construction,
		 * so the surface has no chords. Toggle and cycle act on the
		 * press; preset waits for the release, because a press alone
		 * cannot yet be told apart from the start of a hold-to-save. */
		{
			uint32_t now = (uint32_t)k_uptime_get();
			cc_msg_t msgs[PROFILE_MAX_CAPTURE];

			/* PLAY dumps the storage page (Task 7.0). Read only, and
			 * on a button rather than at boot: a CDC console discards
			 * anything printed before the host opens the port. */
			static bool play_was_down;
			bool play_down = (pressed == 4);

			if (play_down && !play_was_down) {
				(void)midi_tx_idle(200);
				preset_store_dump();
			}
			play_was_down = play_down;

			for (int i = 0; i < PROFILE_NUM_BUTTONS; i++) {
				uint8_t ev = btn_update(&g_btn[i], pressed == i, now);

				if (!ev) {
					continue;
				}

				int n = button_engine_event(&g_eng, i, ev, msgs,
							   (int)ARRAY_SIZE(msgs));

				if (n == BUTTON_ACTION_SAVE_PRESET) {
					cc_msg_t snap[PROFILE_MAX_CAPTURE];
					int slot = button_engine_pending_save_slot(&g_eng);
					int sn = snapshot_surface(prof, snap,
								  (int)ARRAY_SIZE(snap));

					button_engine_set_preset(&g_eng, slot, snap, sn);
					/* Let the wire go quiet first: a flash
					 * write stalls the CPU long enough to
					 * corrupt a bit-banged MIDI byte. */
					(void)midi_tx_idle(200);
					g_save_ok = persist_all_slots();
					g_save_blink_until = now + 900u;
					continue;
				}
				for (int k = 0; k < n; k++) {
					midi_tx_send(msgs[k]);
				}
				if (n > 0) {
					resync_after_recall(prof, msgs, n);
				}
			}
		}

		k_msleep(CONTROL_PERIOD_MS);
	}
	return 0;
}
