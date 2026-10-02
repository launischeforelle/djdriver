/* SPDX-License-Identifier: MIT */
/*
 * USB side of the driver: talks the Ploytec protocol to the controller via
 * libusb (userspace, so no kernel extension is needed on macOS).
 */

#ifndef DJDRIVER_DEVICE_H
#define DJDRIVER_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libusb.h>

#include "midi.h"

struct dj_config {
	uint16_t vid;
	uint16_t pid;
	uint32_t rate;		/* sample rate programmed into the device */
	int ep_out;		/* endpoint overrides, -1 = auto-detect */
	int ep_midi_in;
	int ep_pcm_in;
	int capture;		/* also activate interface 1 (audio input) */
	int leds;		/* start the output stream (LEDs); crashes the DJ2 ME */
	int verbose;
};

void dj_config_defaults(struct dj_config *cfg);

/* Called from the USB event thread for every message from the controller */
typedef void (*dj_midi_in_fn)(void *user, const uint8_t *msg, size_t len);

struct dj_device;

/* Returns true if a matching device is currently plugged in */
bool dj_present(libusb_context *ctx, const struct dj_config *cfg);

/* Print descriptors and status registers (for diagnosis / bug reports) */
int dj_probe(libusb_context *ctx, const struct dj_config *cfg);

/*
 * Try several start-up variants one step at a time and check after every
 * step whether the device still answers.
 */
int dj_diag(libusb_context *ctx, const struct dj_config *cfg);

/*
 * Open the device, run the Ploytec start-up handshake and start streaming.
 * MIDI for the controller is taken from @out_queue.
 */
struct dj_device *dj_open(libusb_context *ctx, const struct dj_config *cfg,
			  struct midi_out_queue *out_queue,
			  dj_midi_in_fn midi_in, void *user);

/* False once the device was unplugged or the stream failed for good */
bool dj_alive(const struct dj_device *dev);

/* Stop streaming and release the device */
void dj_close(struct dj_device *dev);

#endif /* DJDRIVER_DEVICE_H */
