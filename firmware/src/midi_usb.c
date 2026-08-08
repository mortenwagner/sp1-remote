#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>

#include "midi_usb.h"

/* Zephyr's <zephyr/audio/midi.h> already defines UMP_GROUP as an accessor
 * that extracts the group from a packet, so ours needs its own name. */
#define SP1_UMP_GROUP 0

static const struct device *const midi_dev =
	DEVICE_DT_GET(DT_NODELABEL(usb_midi));

static atomic_t usb_midi_ready;

/* The host has opened (or closed) the MIDI interface. Until it has, sends
 * are dropped: a puck on battery at the rack has no host and must still
 * behave normally. */
static void midi_ready_cb(const struct device *dev, bool ready)
{
	ARG_UNUSED(dev);
	atomic_set(&usb_midi_ready, ready ? 1 : 0);
}

/* Nothing drives the puck over USB in v1, so received packets are ignored.
 * The callback still has to exist for the class to bind. Letting a host
 * drive the LEDs (Ableton showing state on the panel) is a natural v1.1. */
static void midi_rx_cb(const struct device *dev, const struct midi_ump ump)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ump);
}

static const struct usbd_midi_ops midi_ops = {
	.rx_packet_cb = midi_rx_cb,
	.ready_cb     = midi_ready_cb,
};

bool midi_usb_ready(void)
{
	return atomic_get(&usb_midi_ready) != 0;
}

void midi_usb_send_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
	if (!midi_usb_ready() || !device_is_ready(midi_dev)) {
		return;
	}
	const struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
		SP1_UMP_GROUP, UMP_MIDI_CONTROL_CHANGE, channel & 0x0Fu,
		cc & 0x7Fu, value & 0x7Fu);

	usbd_midi_send(midi_dev, ump);
}

void midi_usb_init(void)
{
	if (!device_is_ready(midi_dev)) {
		return;
	}
	usbd_midi_set_ops(midi_dev, &midi_ops);
}
