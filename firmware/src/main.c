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
#include "board_io.h"
#include "midi_tx.h"
#include "controls.h"
#include "profile.h"
#include "buttons.h"

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

static fader_state_t   g_fader[PROFILE_NUM_FADERS];
static btn_state_t     g_btn[PROFILE_NUM_BUTTONS];
static button_engine_t g_eng;
static int             g_last_raw;      /* for the diagnostic line only */

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
	const profile_t *prof = &profile_popgoblin_default;

	board_io_init();
	midi_tx_init();
	button_engine_init(&g_eng, prof);

	uint32_t held_ms = 0;

	for (;;) {
		board_io_feed_wdt();

		int pressed = debounced_track_button();

		/* ---- power off on a long function-button hold ---- */
		if (board_io_function_held()) {
			held_ms += CONTROL_PERIOD_MS;
			if (held_ms >= POWER_HOLD_MS) {
				board_io_power_off();       /* does not return */
			}
		} else {
			held_ms = 0;
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
			board_io_track_led_set(0, button_engine_is_latched(&g_eng, 0));
			board_io_track_led_set(1, button_engine_step_index(&g_eng, 1) != 0);
			for (int b = 2; b < PROFILE_NUM_BUTTONS; b++) {
				const preset_slot_t *ps =
					button_engine_get_preset(&g_eng,
								 prof->button[b].preset_slot);

				board_io_track_led_set(b, ps != NULL && ps->len > 0);
			}
		}

#if SP1_DIAG
		static int diag_div;
		if (++diag_div >= 40) {                 /* every 200 ms */
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

			for (int i = 0; i < PROFILE_NUM_BUTTONS; i++) {
				uint8_t ev = btn_update(&g_btn[i], pressed == i, now);

				if (!ev) {
					continue;
				}

				int n = button_engine_event(&g_eng, i, ev, msgs,
							   (int)ARRAY_SIZE(msgs));

				if (n == BUTTON_ACTION_SAVE_PRESET) {
					/* TODO(Phase 7): snapshot and persist. */
					continue;
				}
				for (int k = 0; k < n; k++) {
					midi_tx_send(msgs[k]);
				}
			}
		}

		k_msleep(CONTROL_PERIOD_MS);
	}
	return 0;
}
