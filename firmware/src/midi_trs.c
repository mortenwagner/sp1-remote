/* TRS MIDI transmit on the SP-1 sync jack. TRS Type A, 31250 baud, TX only.
 *
 * Three pins make the jack's MIDI output (2026-11-06):
 *
 *   RING  current source: a PNP whose base is P0.23, LOW = source on.
 *   TIP   data, sunk by TWO pins wired in parallel: P0.20 and P0.17.
 *
 * A Type A receiver is an optocoupler whose LED hangs between our ring and
 * our tip, so a MIDI "0" is current flowing ring -> opto -> tip -> our pins.
 *
 * What it took to get here, all measured on pop:
 *   1. The first version drove the tip only (P0.20, following feldd's data
 *      pin). The ring floated, no opto input ever lit: not the Midihub's,
 *      not the Tiliqua's. The 2026-08-08 "TRS proven" capture went into an
 *      interface that evidently senses the tip as logic.
 *   2. With the ring source on (feldd drives P0.23 low at init, midi_out.c),
 *      the Midihub received everything (664 messages, all six CCs). The
 *      Tiliqua's H11L1 (behind 220R) still saw nothing, while a 5 V Pro-2
 *      lit it at once: not enough current. High drive on P0.20 alone did
 *      not fix it.
 *   3. So both tip pins sink together, as the hardware's parallel wiring
 *      intends (feldd: "A second tip pin (P0.17, a parallel current copy) is
 *      only needed if one pin cannot drive the MIDI opto").
 *
 * The two pins must carry one waveform: if one were high while the other
 * was low, they would fight through their resistors and the tip would sit
 * mid-rail. Each pin therefore gets its own UARTE (UARTE1 on P0.20, UARTE0
 * on P0.17), programmed here at register level with identical settings, and
 * both STARTTX tasks are written back to back with interrupts locked: a few
 * tens of nanoseconds of skew against a 32 us bit. Both are disabled in the
 * devicetree so no Zephyr driver owns them; UARTE0 is the Bluetooth module's
 * UART, which this firmware never uses.
 *
 * The UARTEs also keep what the hardware UART bought earlier: DMA, so flash
 * writes and USB interrupts cannot stretch a bit.
 *
 * midi_trs_release() turns the ring source off before SYSTEM_OFF, which
 * would otherwise latch it on and leak through a cable left plugged in.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <soc.h>

#include "midi_trs.h"

#define RING_SRC_PIN  23u   /* P0.23, PNP base, LOW = ring sources current */
#define TIP_A_PIN     20u   /* P0.20, UARTE1 TXD */
#define TIP_B_PIN     17u   /* P0.17, UARTE0 TXD, the parallel current copy */

/* Set to 1 to drive the tip from P0.20 alone (the feldd configuration). */
#define TRS_TIP_PINS  2

/* A 3-byte CC is ~0.96 ms on the wire; give up well after that so a wedged
 * peripheral can never hang the drain thread past the 4 s watchdog. */
#define TX_TIMEOUT_US 5000

static uint8_t tx_buf[3];   /* DMA source: must be in RAM, hence static */
static bool trs_ready;

static void pin_output_high(uint32_t pin, bool high_drive)
{
	NRF_P0->OUTSET = (1u << pin);   /* idle = mark = high, set before DIR */
	NRF_P0->PIN_CNF[pin] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
		(GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
		((high_drive ? GPIO_PIN_CNF_DRIVE_H0H1 : GPIO_PIN_CNF_DRIVE_S0S1)
		 << GPIO_PIN_CNF_DRIVE_Pos);
}

static void ring_source(bool on)
{
	if (on) {
		NRF_P0->OUTCLR = (1u << RING_SRC_PIN);
	} else {
		NRF_P0->OUTSET = (1u << RING_SRC_PIN);
	}
	NRF_P0->PIN_CNF[RING_SRC_PIN] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
		(GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
}

static void uarte_setup(NRF_UARTE_Type *u, uint32_t tx_pin)
{
	u->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
	u->PSEL.RXD = 0xFFFFFFFFu;
	u->PSEL.RTS = 0xFFFFFFFFu;
	u->PSEL.CTS = 0xFFFFFFFFu;
	u->PSEL.TXD = tx_pin;                       /* port 0 */
	u->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud31250;
	u->CONFIG = 0;                              /* 8N1, no flow control */
	u->INTENCLR = 0xFFFFFFFFu;                  /* polled, no interrupts */
	u->ENABLE = UARTE_ENABLE_ENABLE_Enabled;
}

void midi_trs_init(void)
{
	pin_output_high(TIP_A_PIN, true);
	uarte_setup(NRF_UARTE1, TIP_A_PIN);
#if TRS_TIP_PINS == 2
	pin_output_high(TIP_B_PIN, true);
	uarte_setup(NRF_UARTE0, TIP_B_PIN);
#endif
	ring_source(true);
	trs_ready = true;
}

static void trs_send(const uint8_t *bytes, uint32_t n)
{
	if (!trs_ready || n == 0u || n > sizeof(tx_buf)) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		tx_buf[i] = bytes[i];
	}

	NRF_UARTE1->TXD.PTR = (uint32_t)tx_buf;
	NRF_UARTE1->TXD.MAXCNT = n;
	NRF_UARTE1->EVENTS_ENDTX = 0;
	NRF_UARTE1->EVENTS_TXSTOPPED = 0;
#if TRS_TIP_PINS == 2
	NRF_UARTE0->TXD.PTR = (uint32_t)tx_buf;
	NRF_UARTE0->TXD.MAXCNT = n;
	NRF_UARTE0->EVENTS_ENDTX = 0;
	NRF_UARTE0->EVENTS_TXSTOPPED = 0;
#endif

	unsigned int key = irq_lock();
	NRF_UARTE1->TASKS_STARTTX = 1;
#if TRS_TIP_PINS == 2
	NRF_UARTE0->TASKS_STARTTX = 1;
#endif
	irq_unlock(key);

	for (uint32_t waited = 0; waited < TX_TIMEOUT_US; waited += 20u) {
		bool done = NRF_UARTE1->EVENTS_ENDTX != 0;
#if TRS_TIP_PINS == 2
		done = done && (NRF_UARTE0->EVENTS_ENDTX != 0);
#endif
		if (done) {
			break;
		}
		k_busy_wait(20);
	}

	/* ENDTX = last byte transmitted (nRF52840 PS). STOPTX then releases
	 * the peripheral's clock until the next message. */
	NRF_UARTE1->TASKS_STOPTX = 1;
#if TRS_TIP_PINS == 2
	NRF_UARTE0->TASKS_STOPTX = 1;
#endif
	for (uint32_t waited = 0; waited < 1000u; waited += 10u) {
		bool stopped = NRF_UARTE1->EVENTS_TXSTOPPED != 0;
#if TRS_TIP_PINS == 2
		stopped = stopped && (NRF_UARTE0->EVENTS_TXSTOPPED != 0);
#endif
		if (stopped) {
			break;
		}
		k_busy_wait(10);
	}
}

void midi_trs_send_byte(uint8_t b)
{
	trs_send(&b, 1u);
}

void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
	const uint8_t msg[3] = {
		(uint8_t)(0xB0u | (channel & 0x0Fu)),
		(uint8_t)(cc & 0x7Fu),
		(uint8_t)(value & 0x7Fu),
	};

	trs_send(msg, 3u);
}

void midi_trs_release(void)
{
	ring_source(false);
}
