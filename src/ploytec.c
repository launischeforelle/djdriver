/* SPDX-License-Identifier: MIT */
/*
 * Ploytec USB protocol helpers (pure computation, no I/O).
 */

#include <string.h>

#include "ploytec.h"

static const size_t pt_int_midi_slots[] = { 432, 914, 1396, 1878 };

size_t pt_out_unit_size(enum pt_mode mode)
{
	return mode == PT_MODE_BULK ? PT_BULK_UNIT_SIZE : PT_INT_UNIT_SIZE;
}

size_t pt_out_midi_offset(enum pt_mode mode)
{
	return mode == PT_MODE_BULK ? PT_BULK_MIDI_OFFSET : pt_int_midi_slots[0];
}

void pt_out_init(uint8_t *buf, size_t units, enum pt_mode mode)
{
	size_t unit_size = pt_out_unit_size(mode);
	size_t u, i;

	/* All-zero audio is silence in the bit-scattered wire format, too */
	memset(buf, 0, units * unit_size);

	for (u = 0; u < units; u++) {
		uint8_t *unit = buf + u * unit_size;

		if (mode == PT_MODE_BULK) {
			for (i = 0; i < PT_BULK_SUBPKTS_PER_UNIT; i++) {
				uint8_t *sub = unit + i * PT_BULK_SUBPKT_SIZE;

				sub[PT_BULK_MIDI_OFFSET] = PT_MIDI_IDLE_BYTE;
				sub[PT_BULK_SYNC_OFFSET] = PT_SYNC_BYTE;
				/* bytes 482..511 stay zero: filling them with 0xFD
				 * overflows the firmware's parser */
			}
		} else {
			for (i = 0; i < sizeof(pt_int_midi_slots) / sizeof(pt_int_midi_slots[0]); i++) {
				unit[pt_int_midi_slots[i]] = PT_MIDI_IDLE_BYTE;
				unit[pt_int_midi_slots[i] + 1] = PT_MIDI_IDLE_BYTE;
			}
		}
	}
}

void pt_out_set_midi(uint8_t *buf, size_t unit, enum pt_mode mode, uint8_t midi_byte)
{
	buf[unit * pt_out_unit_size(mode) + pt_out_midi_offset(mode)] = midi_byte;
}

uint16_t pt_status_confirm_wvalue(uint8_t status)
{
	int8_t modified = (int8_t)(status | PT_STATUS_STREAMING);

	return (uint16_t)(int16_t)modified;
}

void pt_encode_rate(uint32_t rate, uint8_t out[3])
{
	out[0] = rate & 0xFF;
	out[1] = (rate >> 8) & 0xFF;
	out[2] = (rate >> 16) & 0xFF;
}

uint32_t pt_decode_rate(const uint8_t in[3])
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16);
}

size_t pt_midi_in_strip(uint8_t *buf, size_t len)
{
	size_t i, n = 0;

	for (i = 0; i < len; i++)
		if (buf[i] < 0xF0)
			buf[n++] = buf[i];
	return n;
}
