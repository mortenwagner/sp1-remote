#include <zephyr/kernel.h>

#include "midi_tx.h"
#include "midi_trs.h"
#include "midi_usb.h"

#define MIDI_TX_STACK 640
#define MIDI_TX_PRIO  7          /* preemptible: a burst must never starve
                                  * the control loop or the watchdog feed */

/* One message per 5 ms = 200/s, which is exactly popgoblin's drain rate:
 * its firmware pops one FIFO entry per 5 ms timer ISR (main.rs:28,100) and
 * the FIFO is 8 deep (top.py:150). The wire would carry ~1000/s, so the
 * receiver, not the baud rate, is what sets this number.
 *
 * USB does not need the pacing, but both sinks share it deliberately: what
 * you watch on the Mac is then exactly what the synth receives, which is
 * the entire point of using USB as the development view. */
#define MIDI_TX_SPACING_MS 5

static txqueue_t      tx_q;
static struct k_mutex tx_lock;
static struct k_sem   tx_wake;
static K_THREAD_STACK_DEFINE(tx_stack, MIDI_TX_STACK);
static struct k_thread tx_tcb;

void midi_tx_send(cc_msg_t m)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	bool ok = txq_push(&tx_q, m);
	k_mutex_unlock(&tx_lock);

	if (ok) {
		k_sem_give(&tx_wake);
	}
	/* A rejected push means 16 distinct (channel, cc) pairs are already
	 * pending, which this surface cannot produce: four faders plus four
	 * buttons plus a five-message preset burst is at most seven distinct
	 * CCs. Dropping is the right response anyway, since the next value
	 * for that CC would coalesce on top of it. */
}

static void midi_tx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		cc_msg_t m;

		k_mutex_lock(&tx_lock, K_FOREVER);
		bool have = txq_pop(&tx_q, &m);
		k_mutex_unlock(&tx_lock);

		if (!have) {
			k_sem_take(&tx_wake, K_FOREVER);
			continue;
		}

		midi_trs_send_cc(m.channel, m.cc, m.value);
		midi_usb_send_cc(m.channel, m.cc, m.value);
		k_msleep(MIDI_TX_SPACING_MS);
	}
}

void midi_tx_init(void)
{
	midi_trs_init();
	midi_usb_init();

	txq_init(&tx_q);
	k_mutex_init(&tx_lock);
	k_sem_init(&tx_wake, 0, 1);

	k_thread_create(&tx_tcb, tx_stack, MIDI_TX_STACK, midi_tx_thread,
			NULL, NULL, NULL, MIDI_TX_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&tx_tcb, "midi_tx");
}
