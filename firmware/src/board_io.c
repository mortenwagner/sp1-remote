/* SP-1 panel hardware.
 *
 * Transplanted from chattock/sp1-tape-looper firmware/src/main.c (MIT). Each
 * block below names the source line range it came from. What is NOT here, on
 * purpose: the audio path, the eMMC driver, the codecs and the looper engine.
 * This firmware has no audio, which is also why it needs no patched Zephyr.
 *
 * BOOTLOADER SAFETY (the SP-1 "BIG FIVE", main.c:40-44):
 *   the app lives at 0x20000; the watchdog is fed inside every loop;
 *   bootloader-owned clocks and peripherals are not re-initialised;
 *   SYSTEM_OFF is the only power-down path; RESETREAS is cleared at boot and
 *   again before SYSTEM_OFF. There is no reset pin on this hardware, so
 *   board_io_enter_dfu() is the escape hatch when the app itself is wedged.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>
#include <soc.h>

#include "board_io.h"

/* ---- the 4 centre-row LEDs (main.c:100-105, verified pin map) ---- */
struct led { NRF_GPIO_Type *port; uint32_t pin; };
static const struct led leds[] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

/* ---- the 4 TRACK LEDs, directly above buttons 1-4 (main.c:107-110) ---- */
static const struct led track_leds[] = {
	{ NRF_P0, 29 }, { NRF_P0, 26 }, { NRF_P1, 15 }, { NRF_P1, 14 },
};
#define NUM_TRACK_LEDS (sizeof(track_leds) / sizeof(track_leds[0]))

/* 1 = dim rendering (the looper's default), 0 = full brightness. */
static volatile uint8_t g_led_dim = 1;

/* ---- power / function button: P0.27, active-low with pull-up (main.c:121-123) ---- */
#define PWR_PORT        NRF_P0
#define PWR_PIN         27u

/* ---- BQ24232 charger control (main.c:126-129, verified pins) ---- */
#define BQ_PORT         NRF_P0
#define BQ_NCE_PIN      21u   /* charge enable, ACTIVE-LOW: drive low = charging on */
#define BQ_NCHG_PIN     22u   /* charge status, open-drain, LOW = charging now      */
#define BQ_NPGOOD_PIN   24u   /* power good,    open-drain, LOW = USB power present */

/* ---- button ladder rail (main.c:145-146) ----
 * The PLAY/track and Vol/rocker buttons are resistor ladders read on the
 * SAADC. They are only powered when BTN_COM is driven high. */
#define BTN_COM_PORT    NRF_P1
#define BTN_COM_PIN     10u

/* ---- ADC channels (main.c:143-156) ----
 * Consumed BY INDEX. The order must match firmware/app.overlay. */
static const struct adc_dt_spec adc_ladder[] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),  /* AIN0: PLAY + tracks   */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),  /* AIN1: Vol + FWD/RWD   */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),  /* AIN3: Fader 1         */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3),  /* AIN6: Fader 2         */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 4),  /* AIN2: Fader 3         */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 5),  /* AIN7: Fader 4         */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 6),  /* AIN4: battery divider */
};
#define LAD_TRACKS 0
#define LAD_VOL    1
#define LAD_FADER0 2     /* faders are ladder indices 2..5 */
#define LAD_BATT   6
#define NUM_LADDERS (sizeof(adc_ladder) / sizeof(adc_ladder[0]))

#define WDT_NODE DT_ALIAS(watchdog0)
static const struct device *const wdt = DEVICE_DT_GET(WDT_NODE);

static uint32_t g_resetreas;
static int16_t  adc_sample;

/* ======================================================================
 *  ADC
 * ====================================================================== */

/* Oversampled ladder read (main.c:192-208, which averaged 2).
 *
 * Raised to 4 after measuring pop: resting jitter was 4 to 7 counts
 * peak-to-peak, several times the plus-or-minus 1 the looper's comment
 * claims. The looper capped this at 2 because its blocking ADC reads
 * competed with an eMMC streamer and stole the margin a recording needed.
 * This firmware has no audio and no storage engine, so the CPU is free:
 * averaging 4 halves the noise for roughly 0.5 ms per control pass.
 *
 * Returns -1 on error: callers treat <0 as "hold the last value". */
static int ladder_read(const struct adc_dt_spec *spec)
{
	struct adc_sequence seq = {
		.buffer      = &adc_sample,
		.buffer_size = sizeof(adc_sample),
	};
	if (adc_sequence_init_dt(spec, &seq) < 0) {
		return -1;
	}
	int32_t acc = 0;
	for (int i = 0; i < 4; i++) {
		if (adc_read_dt(spec, &seq) < 0) {
			return -1;
		}
		acc += adc_sample;
	}
	return (int)(acc / 4);
}

int board_io_read_fader(int idx)
{
	if (idx < 0 || idx >= BOARD_NUM_FADERS) {
		return -1;
	}
	return ladder_read(&adc_ladder[LAD_FADER0 + idx]);
}

int board_io_read_track_ladder(void)
{
	return ladder_read(&adc_ladder[LAD_TRACKS]);
}

/* Transplanted from main.c:5098-5107. These thresholds are calibrated raw
 * values under the exact ADC configuration in the board files and
 * app.overlay: gain 1/6, 0.6 V internal reference, 20 us acquisition,
 * 12-bit. Do not change one without re-measuring the other.
 *
 * Note the Track 1 + Track 4 combo reads ~1325, INSIDE the track-4 band
 * below. Callers must check that band BEFORE calling this, or the recovery
 * gesture decodes as a track-4 press. See main.c:6964-6981. */
int board_io_decode_track_button(int v)
{
	if (v <  110) return -1;   /* none           */
	if (v <  300) return 0;    /* track 1, ~213  */
	if (v <  560) return 1;    /* track 2, ~403  */
	if (v <  950) return 2;    /* track 3, ~733  */
	if (v < 1500) return 3;    /* track 4, ~1220 */
	return 4;                  /* play,    ~1823 */
}

/* ======================================================================
 *  LEDs: soft PWM (main.c:5121-5240)
 *
 *  The panel LEDs are plain on/off GPIO with no current control, so "dim"
 *  means software PWM: writes go into a shadow mask and a TIMER3 ISR
 *  renders it at a low duty cycle. Single writer (the control loop), the
 *  ISR only reads. ~1 kHz frame is flicker-free.
 *
 *  Adapted from TechnicsOP's dimmed-LED build via the looper (MIT).
 * ====================================================================== */
#define LED_PWM_PERIOD_US   1000u   /* 1 kHz frame                        */
#define LED_PWM_ON_US         52u   /* ~5.2% duty, the track row          */
#define LED_STATUS_ON_US      66u   /* the centre row runs slightly wider */
#define LED_GHOST_FRAME_DIV    5u   /* ghost class: lit 1 frame in N      */
#define LED_PWM_TIMER      NRF_TIMER3
#define LED_PWM_TIMER_IRQn TIMER3_IRQn

/* Every LED pin on each port, for the OFF phase. Must match the two tables
 * above; kept as literals exactly as the looper had them. */
#define LED_ALL_P0 ((1u << 0) | (1u << 1) | (1u << 29) | (1u << 26))
#define LED_ALL_P1 ((1u << 13) | (1u << 12) | (1u << 15) | (1u << 14))

static volatile uint32_t g_led_p0_on, g_led_p1_on;
static volatile uint32_t g_led_p0_ghost, g_led_p1_ghost;
static uint32_t g_led_sta_p0, g_led_sta_p1;   /* centre-row pins */
static uint32_t g_led_trk_p0, g_led_trk_p1;   /* track-row pins  */

/* DIRECT ISR, required for IRQ_ZERO_LATENCY: pure register IO, no kernel
 * calls, returns 0 so it never asks for a reschedule. */
ISR_DIRECT_DECLARE(led_pwm_isr)
{
	if (LED_PWM_TIMER->EVENTS_COMPARE[1]) {          /* period wrap: render */
		LED_PWM_TIMER->EVENTS_COMPARE[1] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[1];
		static uint32_t gframe;
		uint32_t gdiv = g_led_dim ? 8u : LED_GHOST_FRAME_DIV;
		uint32_t gon  = ((++gframe % gdiv) == 0u);
		uint32_t s0 = g_led_p0_on | (gon ? (g_led_p0_ghost & ~g_led_p0_on) : 0u);
		uint32_t s1 = g_led_p1_on | (gon ? (g_led_p1_ghost & ~g_led_p1_on) : 0u);
		NRF_P0->OUTSET = s0;
		NRF_P0->OUTCLR = LED_ALL_P0 & ~s0;
		NRF_P1->OUTSET = s1;
		NRF_P1->OUTCLR = LED_ALL_P1 & ~s1;
	}
	if (LED_PWM_TIMER->EVENTS_COMPARE[0]) {          /* track-row on-time up */
		LED_PWM_TIMER->EVENTS_COMPARE[0] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[0];
		if (g_led_dim) {
			NRF_P0->OUTCLR = g_led_trk_p0;
			NRF_P1->OUTCLR = g_led_trk_p1;
		} else {
			NRF_P0->OUTCLR = g_led_p0_ghost & ~g_led_p0_on;
			NRF_P1->OUTCLR = g_led_p1_ghost & ~g_led_p1_on;
		}
	}
	if (LED_PWM_TIMER->EVENTS_COMPARE[2]) {          /* centre-row on-time up */
		LED_PWM_TIMER->EVENTS_COMPARE[2] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[2];
		if (g_led_dim) {
			NRF_P0->OUTCLR = g_led_sta_p0;
			NRF_P1->OUTCLR = g_led_sta_p1;
		}
	}
	return 0;
}

static void led_cfg_output(const struct led *l)
{
	l->port->PIN_CNF[l->pin] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}

static void led_pwm_init(void)
{
	LED_PWM_TIMER->MODE      = TIMER_MODE_MODE_Timer;
	LED_PWM_TIMER->BITMODE   = TIMER_BITMODE_BITMODE_16Bit;
	LED_PWM_TIMER->PRESCALER = 4;                     /* 16 MHz/16 = 1 us tick */
	LED_PWM_TIMER->CC[0]     = LED_PWM_ON_US;
	LED_PWM_TIMER->CC[1]     = LED_PWM_PERIOD_US;
	LED_PWM_TIMER->CC[2]     = LED_STATUS_ON_US;
	LED_PWM_TIMER->SHORTS    = TIMER_SHORTS_COMPARE1_CLEAR_Msk;
	LED_PWM_TIMER->INTENSET  = TIMER_INTENSET_COMPARE0_Msk |
				   TIMER_INTENSET_COMPARE1_Msk |
				   TIMER_INTENSET_COMPARE2_Msk;

	for (unsigned li = 0; li < NUM_LEDS; li++) {
		led_cfg_output(&leds[li]);
		if (leds[li].port == NRF_P0) {
			g_led_sta_p0 |= (1u << leds[li].pin);
		} else {
			g_led_sta_p1 |= (1u << leds[li].pin);
		}
	}
	for (unsigned li = 0; li < NUM_TRACK_LEDS; li++) {
		led_cfg_output(&track_leds[li]);
		if (track_leds[li].port == NRF_P0) {
			g_led_trk_p0 |= (1u << track_leds[li].pin);
		} else {
			g_led_trk_p1 |= (1u << track_leds[li].pin);
		}
	}

	IRQ_DIRECT_CONNECT(LED_PWM_TIMER_IRQn, 0, led_pwm_isr, IRQ_ZERO_LATENCY);
	irq_enable(LED_PWM_TIMER_IRQn);
	LED_PWM_TIMER->TASKS_CLEAR = 1;
	LED_PWM_TIMER->TASKS_START = 1;
}

static void mask_set(const struct led *l, bool on, bool ghost)
{
	uint32_t bit = (1u << l->pin);
	volatile uint32_t *lit   = (l->port == NRF_P0) ? &g_led_p0_on    : &g_led_p1_on;
	volatile uint32_t *gh    = (l->port == NRF_P0) ? &g_led_p0_ghost : &g_led_p1_ghost;

	if (on)    { *lit |= bit;  } else { *lit &= ~bit; }
	if (ghost) { *gh  |= bit;  } else { *gh  &= ~bit; }
}

void board_io_led_set(int idx, uint8_t level)
{
	if (idx < 0 || idx >= (int)NUM_LEDS) {
		return;
	}
	/* Three levels, see the header: off / ghost / on. */
	mask_set(&leds[idx], level >= 128u, level > 0u && level < 128u);
}

void board_io_track_led_set(int idx, bool on)
{
	if (idx < 0 || idx >= (int)NUM_TRACK_LEDS) {
		return;
	}
	mask_set(&track_leds[idx], on, false);
}

/* Force every LED dark, bypassing the shadow mask. Used immediately before
 * SYSTEM_OFF, which latches the GPIO levels: clearing BOTH rows is what
 * stops lights freezing on into sleep (main.c:5316-5325). */
static void shutdown_leds(void)
{
	g_led_p0_on = g_led_p1_on = 0;
	g_led_p0_ghost = g_led_p1_ghost = 0;
	NRF_P0->OUTCLR = LED_ALL_P0;
	NRF_P1->OUTCLR = LED_ALL_P1;
}

/* ======================================================================
 *  Watchdog (feed main.c:5565-5569, install main.c:5901-5906)
 * ====================================================================== */
void board_io_feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
}

static void wdt_start(void)
{
	if (!device_is_ready(wdt)) {
		return;
	}
	struct wdt_timeout_cfg cfg = {
		.window.max = 4000,
		.callback   = NULL,
	};
	(void)wdt_install_timeout(wdt, &cfg);
	(void)wdt_setup(wdt, 0);
	board_io_feed_wdt();
}

/* ======================================================================
 *  Power button and power-off (main.c:5571-5597, 5664-5735)
 * ====================================================================== */
static void pwr_btn_cfg_input(void)
{
	PWR_PORT->PIN_CNF[PWR_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}

/* Arm the button as the SYSTEM_OFF wake source (sense the low level). */
static void pwr_btn_arm_wake(void)
{
	PWR_PORT->PIN_CNF[PWR_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)|
		(GPIO_PIN_CNF_SENSE_Low     << GPIO_PIN_CNF_SENSE_Pos);
}

bool board_io_function_held(void)
{
	return (PWR_PORT->IN & (1u << PWR_PIN)) == 0u;   /* low = pressed */
}

void board_io_power_off(void)
{
	/* Shutdown sweep across both rows, then force every LED dark before
	 * SYSTEM_OFF latches the GPIO levels. */
	for (int i = (int)NUM_LEDS - 1; i >= 0; i--) {
		board_io_led_set(i, 0);
		board_io_track_led_set(i, false);
		board_io_feed_wdt();
		k_msleep(80);
	}
	shutdown_leds();

	/* Wait for the finger to leave the button, or the level-sense we are
	 * about to arm would wake us again immediately. */
	while (board_io_function_held()) {
		board_io_feed_wdt();
		k_msleep(20);
	}
	k_msleep(60);              /* debounce the release */
	shutdown_leds();           /* re-assert dark right before sleep */

	pwr_btn_arm_wake();
	board_io_feed_wdt();

	/* NOTE: the looper soft-resets instead when USB is present, because it
	 * has a charge-standby screen to land in. This firmware has none, so it
	 * always powers down. Behaviour while plugged in is a Phase 2 bench
	 * check: confirm the puck goes dark and that holding Track 1 + Track 4
	 * on the next plug still reaches the bootloader. */
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) {
		/* CPU is off; the bootloader takes over on the next press. */
	}
}

/* FAILSAFE recovery (main.c:5743-5753): reset into the bootloader so the
 * puck can ALWAYS be reflashed, including when this firmware is wedged.
 * Shows all four track LEDs as the cue, writes the UF2 magic (harmless if
 * the bootloader ignores it) and resets. The user keeps holding Track 1 +
 * Track 4 through the reset, and the bootloader's own scan enters DFU. */
void board_io_enter_dfu(void)
{
	g_led_p0_on = g_led_p1_on = 0;
	g_led_p0_ghost = g_led_p1_ghost = 0;
	for (unsigned i = 0; i < NUM_TRACK_LEDS; i++) {
		board_io_track_led_set((int)i, true);
	}
	NRF_POWER->GPREGRET = 0x57u;
	__DSB();
	NVIC_SystemReset();
	for (;;) {
	}
}

uint32_t board_io_resetreas(void)
{
	return g_resetreas;
}

/* ======================================================================
 *  Charger (main.c:126-129)
 * ====================================================================== */
static void charger_init(void)
{
	/* nCE is ACTIVE-LOW: drive it low or the puck never charges over
	 * USB-C. Easy to omit, and the symptom (a battery that quietly never
	 * fills) shows up days later. */
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);
	BQ_PORT->PIN_CNF[BQ_NCE_PIN] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);

	/* Status lines are open-drain inputs with pull-ups: LOW means active. */
	BQ_PORT->PIN_CNF[BQ_NCHG_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BQ_PORT->PIN_CNF[BQ_NPGOOD_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}

/* ======================================================================
 *  Init
 * ====================================================================== */
static void controls_init(void)
{
	/* Power the ladder rail before any ADC read (main.c:211-224). */
	BTN_COM_PORT->OUTSET = (1u << BTN_COM_PIN);
	BTN_COM_PORT->PIN_CNF[BTN_COM_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BTN_COM_PORT->OUTSET = (1u << BTN_COM_PIN);

	for (unsigned i = 0; i < NUM_LADDERS; i++) {
		if (device_is_ready(adc_ladder[i].dev)) {
			(void)adc_channel_setup_dt(&adc_ladder[i]);
		}
	}
}

/* CDC console. Nothing brings USB up by itself on the device_next stack with
 * CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n, so the application must. Never
 * blocks: a puck at the rack has no host and must still work. */
static void usb_console_start(void)
{
	struct usbd_context *usbd = sample_usbd_init_device(NULL);

	if (usbd == NULL) {
		return;
	}
	(void)usbd_enable(usbd);
}

void board_io_init(void)
{
	/* The BOOTLOADER has already armed the watchdog with a 4 s window, and
	 * it is running while we initialise. Feed it first and again after the
	 * slowest step, so a slow USB enumeration can never reboot us mid-init.
	 * This is also why the feed writes NRF_WDT->RR directly rather than
	 * going through the Zephyr API: the timeout is not ours to configure. */
	board_io_feed_wdt();

	/* BIG FIVE: latch and clear the reset reason on every boot. */
	g_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;

	pwr_btn_cfg_input();
	led_pwm_init();
	shutdown_leds();
	charger_init();
	controls_init();
	board_io_feed_wdt();

	wdt_start();
	usb_console_start();
	board_io_feed_wdt();
}
