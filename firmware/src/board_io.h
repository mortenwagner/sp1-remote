/* Hardware access for the SP-1 panel.
 *
 * Everything Zephyr-specific and everything register-level lives behind this
 * header, so the logic modules (controls, buttons, profile, txqueue, presets)
 * stay pure C and host-testable.
 *
 * The implementation is transplanted from chattock/sp1-tape-looper (MIT),
 * whose constants are verified on real hardware. Each block in board_io.c
 * carries the source line range it came from. */
#ifndef SP1_BOARD_IO_H
#define SP1_BOARD_IO_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_NUM_FADERS 4
#define BOARD_NUM_LEDS   4

/* Brings up the LED renderer, the button rail and ADC, the watchdog, the
 * charger, and the USB console. Safe to call once at boot; never blocks
 * waiting for a USB host. */
void board_io_init(void);

/* Feed the watchdog. The bootloader arms a 4 s window and there is no reset
 * pin on this hardware, so every path that can loop must call this. */
void board_io_feed_wdt(void);

/* Raw 12-bit SAADC codes. Negative means the read failed and the caller
 * should hold its last value rather than treat it as a reading. */
int  board_io_read_fader(int idx);
int  board_io_read_track_ladder(void);

/* -1 none, 0..3 track buttons, 4 play. Thresholds are calibrated to the ADC
 * configuration in the board files: see the comment in board_io.c. */
int  board_io_decode_track_button(int raw);

bool board_io_function_held(void);

/* Charger telemetry, straight off the BQ24232's open-drain status pins.
 * Both are active LOW at the pin; these return the logical sense. */
bool board_io_usb_present(void);   /* nPGOOD low: bus power is there   */
bool board_io_charging(void);      /* nCHG   low: actively charging    */

/* Raw battery voltage from the on-board divider (AIN4). Relative only:
 * useful for watching it climb, not for a state-of-charge percentage. */
int  board_io_read_battery(void);

/* SYSTEM_OFF. Never returns. Clears RESETREAS first, and leaves the power
 * button armed as the wake source, so the bootloader gets control on the
 * next press. */
void board_io_power_off(void);

/* FAILSAFE: reset into the bootloader from a running app, so the puck can
 * always be reflashed even if this firmware is wedged. Never returns. */
void board_io_enter_dfu(void);

/* Centre-row LEDs.
 *
 * IMPORTANT: the transplanted renderer is a shadow mask plus a global dim
 * setting, NOT per-LED PWM, so this quantises to THREE levels: 0 is off,
 * 1..127 is the "ghost" duty (roughly a fifth of dim), 128..255 is on. The
 * 0..255 signature exists so the Phase 6 code reads naturally and so a
 * future per-LED renderer can be dropped in without touching callers. Do not
 * design a UI that needs more than three levels here without extending the
 * renderer first. */
void board_io_led_set(int idx, uint8_t level);

/* Track-row LEDs, above the four track buttons. On/off only. */
void board_io_track_led_set(int idx, bool on);

/* Reset reason latched at boot, before it was cleared. Diagnostic only. */
uint32_t board_io_resetreas(void);

#endif /* SP1_BOARD_IO_H */
