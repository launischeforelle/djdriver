/* SPDX-License-Identifier: MIT */
/*
 * CoreMIDI backend (macOS): publishes a virtual MIDI source and destination
 * that DJ software sees like a normal USB MIDI controller.
 *
 * The MIDI 1.0 packet-list API used here is deprecated since macOS 11 in
 * favour of the UMP event-list API, but remains fully functional and keeps
 * this backend simple.
 */

#include <stdio.h>
#include <stdlib.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include "port.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Fixed unique IDs so DJ software recognises the port again after a restart */
#define UNIQUE_ID_SOURCE	0x52444A32	/* 'RDJ2' */
#define UNIQUE_ID_DEST		0x52444A33

struct midi_port {
	MIDIClientRef client;
	MIDIEndpointRef source;
	MIDIEndpointRef dest;
	midi_port_rx_fn rx;
	void *user;
};

static void read_proc(const MIDIPacketList *list, void *ref, void *conn_ref)
{
	struct midi_port *port = ref;
	const MIDIPacket *pkt = &list->packet[0];
	UInt32 i;

	(void)conn_ref;
	for (i = 0; i < list->numPackets; i++) {
		port->rx(port->user, pkt->data, pkt->length);
		pkt = MIDIPacketNext(pkt);
	}
}

static void set_props(MIDIEndpointRef ep, SInt32 unique_id)
{
	/* may fail if the ID is taken; CoreMIDI then keeps its random one */
	MIDIObjectSetIntegerProperty(ep, kMIDIPropertyUniqueID, unique_id);
	MIDIObjectSetStringProperty(ep, kMIDIPropertyManufacturer, CFSTR("Reloop"));
	MIDIObjectSetStringProperty(ep, kMIDIPropertyModel, CFSTR("Digital Jockey 2 Master Edition"));
}

struct midi_port *midi_port_open(const char *name, midi_port_rx_fn rx, void *user)
{
	struct midi_port *port;
	CFStringRef cfname;
	OSStatus err;

	port = calloc(1, sizeof(*port));
	if (!port)
		return NULL;
	port->rx = rx;
	port->user = user;

	cfname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	if (!cfname)
		goto fail;

	err = MIDIClientCreate(cfname, NULL, NULL, &port->client);
	if (err) {
		fprintf(stderr, "MIDIClientCreate failed: %d\n", (int)err);
		goto fail_name;
	}

	err = MIDISourceCreate(port->client, cfname, &port->source);
	if (err) {
		fprintf(stderr, "MIDISourceCreate failed: %d\n", (int)err);
		goto fail_client;
	}
	set_props(port->source, UNIQUE_ID_SOURCE);

	err = MIDIDestinationCreate(port->client, cfname, read_proc, port, &port->dest);
	if (err) {
		fprintf(stderr, "MIDIDestinationCreate failed: %d\n", (int)err);
		goto fail_source;
	}
	set_props(port->dest, UNIQUE_ID_DEST);

	CFRelease(cfname);
	return port;

fail_source:
	MIDIEndpointDispose(port->source);
fail_client:
	MIDIClientDispose(port->client);
fail_name:
	CFRelease(cfname);
fail:
	free(port);
	return NULL;
}

void midi_port_send(struct midi_port *port, const uint8_t *msg, size_t len)
{
	union {
		MIDIPacketList list;	/* keeps the required alignment */
		Byte raw[64];
	} storage;
	MIDIPacketList *list = &storage.list;
	MIDIPacket *pkt;

	pkt = MIDIPacketListInit(list);
	pkt = MIDIPacketListAdd(list, sizeof(storage), pkt, 0, len, msg);
	if (pkt)
		MIDIReceived(port->source, list);
}

void midi_port_close(struct midi_port *port)
{
	if (!port)
		return;
	MIDIEndpointDispose(port->dest);
	MIDIEndpointDispose(port->source);
	MIDIClientDispose(port->client);
	free(port);
}
