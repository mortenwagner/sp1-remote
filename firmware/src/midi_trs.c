/* TRS MIDI transmit, on the hardware UART.
 *
 * Data leaves on the TIP at P0.20 as TRS Type A, 31250 baud, TX only.
 *
 * This follows bnjreece/feldd-sp1-firmware, whose midi_out.c states the
 * path is HARDWARE-VALIDATED: feldd drives an OP-XY over this jack, Type A,
 * data on the tip P0.20. That is the only account of this jack anyone has
 * tested against real gear. The SP-1-dev pin header does not document the
 * jack at all and its wiki page for it is a Todo.
 *
 * It replaces an earlier bit-banged version transplanted from
 * sp1-tape-looper, which drove P0.23 (the ring) through an inverting PNP
 * and whose own source called it untested. Three problems disappeared with
 * it: the MIDI_INVERT polarity unknown, flash writes stalling the CPU
 * mid-byte (a word write is ~41 us against a 32 us bit), and USB interrupts
 * jittering bit edges. A UARTE with its own DMA is indifferent to all of it.
 *
 * Because it is Type A, a PLAIN TRS CABLE reaches the Tiliqua's MIDI-in and
 * a Blokas Midihub. No adapter, no tip/sleeve swap.
 *
 * Sending is blocking, one byte at a time: a three-byte CC is about 1 ms on
 * the wire, and the caller is the drain thread, which already paces
 * messages 5 ms apart. If MIDI clock output ever lands, replace this with
 * feldd's two-tier priority ring so real-time bytes can jump ahead of a
 * fader burst.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include "midi_trs.h"

static const struct device *const trs = DEVICE_DT_GET(DT_NODELABEL(uart1));

static bool trs_ready;

void midi_trs_init(void)
{
	trs_ready = device_is_ready(trs);
	if (!trs_ready) {
		printk("midi_trs: uart1 not ready, TRS output disabled\n");
	}
}

void midi_trs_send_byte(uint8_t b)
{
	if (!trs_ready) {
		return;
	}
	uart_poll_out(trs, b);
}

void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
	midi_trs_send_byte((uint8_t)(0xB0u | (channel & 0x0Fu)));
	midi_trs_send_byte(cc & 0x7Fu);
	midi_trs_send_byte(value & 0x7Fu);
}
