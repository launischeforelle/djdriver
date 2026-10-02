/* SPDX-License-Identifier: MIT */
/*
 * Host-side MIDI port: the virtual MIDI device that DJ software (Mixxx,
 * Traktor, Serato, VirtualDJ, ...) sees.
 *
 * macOS: a CoreMIDI virtual source + destination (port_coremidi.c).
 * Other systems: a debug backend that only prints messages (port_print.c).
 */

#ifndef DJDRIVER_PORT_H
#define DJDRIVER_PORT_H

#include <stddef.h>
#include <stdint.h>

struct midi_port;

/* Called (from a backend thread) with raw MIDI bytes sent by the DJ software */
typedef void (*midi_port_rx_fn)(void *user, const uint8_t *data, size_t len);

/* Create the virtual port. Returns NULL on failure. */
struct midi_port *midi_port_open(const char *name, midi_port_rx_fn rx, void *user);

/* Deliver one complete MIDI message from the controller to the DJ software */
void midi_port_send(struct midi_port *port, const uint8_t *msg, size_t len);

void midi_port_close(struct midi_port *port);

#endif /* DJDRIVER_PORT_H */
