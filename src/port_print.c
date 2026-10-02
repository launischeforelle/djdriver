/* SPDX-License-Identifier: MIT */
/*
 * Debug MIDI port for systems without a native backend: prints every
 * message coming from the controller. Nothing is ever sent to the device.
 */

#include <stdio.h>
#include <stdlib.h>

#include "port.h"

struct midi_port {
	int unused;
};

struct midi_port *midi_port_open(const char *name, midi_port_rx_fn rx, void *user)
{
	(void)rx;
	(void)user;
	fprintf(stderr, "Note: no native MIDI backend on this platform, "
		"messages from \"%s\" are only printed.\n", name);
	return calloc(1, sizeof(struct midi_port));
}

void midi_port_send(struct midi_port *port, const uint8_t *msg, size_t len)
{
	size_t i;

	(void)port;
	printf("MIDI IN ");
	for (i = 0; i < len; i++)
		printf(" %02X", msg[i]);
	printf("\n");
	fflush(stdout);
}

void midi_port_close(struct midi_port *port)
{
	free(port);
}
