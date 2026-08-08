/* Bit-banged TRS MIDI transmit.
 * Transplanted from sp1-tape-looper firmware/src/main.c:4419-4530 (MIT).
 *
 * Pins, from TimK's sync-jack schematic as quoted by the looper. These do
 * NOT appear in SP-1-dev's published pin header, and its wiki page for this
 * jack is a Todo, so treat them as good but unpublished:
 *
 *   MIDI: BC807_BASE = P0.23 -> a PNP that drives SYNC_RING. The PNP
 *         INVERTS: P0.23 LOW gives ring HIGH (MIDI idle/mark), P0.23 HIGH
 *         gives ring LOW (start bit). midi_line() compensates.
 *   PO sync: P0.20 / P0.17 -> SYNC_TIP. Deliberately NOT driven here: this
 *         firmware emits no Pocket Operator sync, and leaving the tip alone
 *         keeps it out of the MIDI current loop.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <soc.h>

#include "midi_trs.h"

#define MIDI_PIN         23u   /* P0.23 BC807 base, drives SYNC_RING */
#define MIDI_INVERT      1     /* the PNP stage inverts; flip if a receiver
                                * sees inverted data. This is the FIRST thing
                                * to try if the bench test shows garbage. */
#define MIDI_BIT_US      32u   /* 31250 baud, 8N1 */
#define MIDI_TIMER       NRF_TIMER2
#define MIDI_TIMER_IRQn  TIMER2_IRQn

static volatile uint16_t midi_tx_bits;   /* remaining frame, LSB = next bit */
static volatile uint8_t  midi_tx_left;   /* bits still to clock, 0 = done   */
static struct k_sem      midi_tx_done;   /* 1 = the line is free            */

static inline void midi_line(int mark)   /* mark = 1 is idle/high */
{
	int p = MIDI_INVERT ? !mark : mark;

	if (p) {
		NRF_P0->OUTSET = (1u << MIDI_PIN);
	} else {
		NRF_P0->OUTCLR = (1u << MIDI_PIN);
	}
}

static void midi_pins_init(void)
{
	NRF_P0->PIN_CNF[MIDI_PIN] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
	midi_line(1);                        /* idle at MARK */
}

/* One bit per ISR, interrupts left ON throughout. The looper learned this
 * the hard way: its original version held irq_lock() for the whole ~320 us
 * byte, which masked the eMMC and I2S interrupts and caused audible
 * crackle. We have no audio to starve, but the same design also keeps the
 * LED soft-PWM's edges clean, so it is kept as-is. */
static void midi_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);
	MIDI_TIMER->EVENTS_COMPARE[0] = 0;
	(void)MIDI_TIMER->EVENTS_COMPARE[0];      /* flush the clear, nRF anomaly */

	if (midi_tx_left) {
		midi_line(midi_tx_bits & 1u);
		midi_tx_bits >>= 1;
		midi_tx_left--;
	} else {
		MIDI_TIMER->TASKS_STOP = 1;
		midi_line(1);                     /* leave the line idle */
		k_sem_give(&midi_tx_done);
	}
}

static void midi_timer_init(void)
{
	MIDI_TIMER->MODE      = TIMER_MODE_MODE_Timer;
	MIDI_TIMER->BITMODE   = TIMER_BITMODE_BITMODE_16Bit;
	MIDI_TIMER->PRESCALER = 4;                   /* 16 MHz/16 = 1 us tick */
	MIDI_TIMER->CC[0]     = MIDI_BIT_US;
	MIDI_TIMER->SHORTS    = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
	MIDI_TIMER->INTENSET  = TIMER_INTENSET_COMPARE0_Msk;
	k_sem_init(&midi_tx_done, 1, 1);             /* line starts free */
	IRQ_CONNECT(MIDI_TIMER_IRQn, 2, midi_timer_isr, NULL, 0);
	irq_enable(MIDI_TIMER_IRQn);
}

void midi_trs_init(void)
{
	midi_pins_init();
	midi_timer_init();
}

void midi_trs_send_byte(uint8_t b)
{
	/* Wait for any in-flight byte. In practice it has always finished:
	 * a byte is 320 us and the drain thread paces messages 5 ms apart. */
	if (k_sem_take(&midi_tx_done, K_MSEC(5)) != 0) {
		return;                              /* stuck: drop the byte */
	}
	/* The ENTIRE 10-bit frame is timer-clocked: start(0), d0..d7 LSB
	 * first, stop(1). The start bit is the timer's FIRST event rather
	 * than driven here, so every edge is timer-paced and a thread
	 * preemption cannot stretch the start bit and corrupt framing. */
	midi_tx_bits = ((uint16_t)b << 1) | (1u << 9);
	midi_tx_left = 10;
	midi_line(1);                                /* hold mark until ISR 1 */
	MIDI_TIMER->TASKS_CLEAR = 1;
	MIDI_TIMER->TASKS_START = 1;
}

/* A three-byte CC occupies about 1 ms on the wire at 31250 baud. That is
 * the number behind the drain thread's pacing in midi_tx.c. */
void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
	midi_trs_send_byte((uint8_t)(0xB0u | (channel & 0x0Fu)));
	midi_trs_send_byte(cc & 0x7Fu);
	midi_trs_send_byte(value & 0x7Fu);
}
