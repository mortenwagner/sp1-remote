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

int main(void)
{
	board_io_init();
	midi_tx_init();

	uint32_t held_ms   = 0;
	int64_t  combo14_t = -1;
	int      err_run   = 0;

	for (;;) {
		board_io_feed_wdt();

		/* ---- recovery combo, checked before anything else ---- */
		int raw = board_io_read_track_ladder();
		if (raw < 0) {
			if (++err_run >= LADDER_ERR_LIMIT) {
				combo14_t = -1;
			}
		} else {
			err_run = 0;
			if (raw >= COMBO14_LO && raw <= COMBO14_HI) {
				if (combo14_t < 0) {
					combo14_t = k_uptime_get();
				} else if (k_uptime_get() - combo14_t >= COMBO14_HOLD_MS) {
					board_io_enter_dfu();   /* does not return */
				}
			} else {
				combo14_t = -1;
			}
		}

		/* ---- power off on a long function-button hold ---- */
		if (board_io_function_held()) {
			held_ms += CONTROL_PERIOD_MS;
			if (held_ms >= POWER_HOLD_MS) {
				board_io_power_off();       /* does not return */
			}
		} else {
			held_ms = 0;
		}

		/* ---- proof of life ----
		 * A plain 1 Hz blink, on purpose. The looper animates its LEDs on
		 * boot, so "no animation, just a blink" is the fastest way to tell
		 * at a glance that this firmware is the one running. */
		board_io_led_set(0, ((k_uptime_get() / 500) & 1) ? 255 : 0);

#if SP1_DIAG
		static int diag_div;
		if (++diag_div >= 20) {                 /* every 100 ms */
			diag_div = 0;
			printk("f0=%4d f1=%4d f2=%4d f3=%4d  lad=%4d btn=%d  "
			       "batt=%4d usb=%d chg=%d\n",
			       board_io_read_fader(0), board_io_read_fader(1),
			       board_io_read_fader(2), board_io_read_fader(3),
			       raw, board_io_decode_track_button(raw),
			       board_io_read_battery(),
			       board_io_usb_present() ? 1 : 0,
			       board_io_charging() ? 1 : 0);
		}
#endif

#if SP1_DIAG
		/* Phase 3 smoke test: sweep CC 102 (the synth's filter cutoff)
		 * continuously, without touching anything. Proves the queue, the
		 * drain thread and both sinks. Faders take over in Phase 4. */
		static int ramp_div;
		static int ramp_v, ramp_dir = 1;
		if (++ramp_div >= 20) {                 /* every 100 ms */
			ramp_div = 0;
			ramp_v += ramp_dir * 4;
			if (ramp_v >= 127) { ramp_v = 127; ramp_dir = -1; }
			if (ramp_v <= 0)   { ramp_v = 0;   ramp_dir =  1; }
			midi_tx_send((cc_msg_t){ 0, 102, (uint8_t)ramp_v });
		}
#endif

		k_msleep(CONTROL_PERIOD_MS);
	}
	return 0;
}
