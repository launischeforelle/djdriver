/* SPDX-License-Identifier: MIT */
/*
 * Ploytec USB protocol helpers (pure computation, no I/O).
 *
 * The Reloop Digital Jockey 2 Master Edition is not USB class compliant. Like
 * several other DJ devices of its era (Reloop Jockey 3, Allen & Heath
 * Xone:DB4/DB2/4D) it uses a USB chipset and firmware by Ploytec GmbH. The
 * protocol facts used here come from the public reverse-engineering work of
 * the alsa-jockey3 project (Frank van de Pol) and the Ozzy project (Marcel
 * Bierling); see README.md.
 *
 * In short:
 *   - Control (EP0): vendor requests to read the firmware version ('V'),
 *     read/write a status register ('I') and set the sample rate.
 *   - EP 0x05 OUT: audio playback stream. MIDI *to* the device (LEDs) is
 *     embedded as single bytes at fixed positions inside this stream, so the
 *     stream must run continuously for LEDs to work.
 *   - EP 0x86 IN: audio capture stream.
 *   - EP 0x83 IN: MIDI *from* the device (buttons, faders, jog wheels) as a
 *     raw MIDI byte stream padded with filler bytes in the 0xF0..0xFF range.
 */

#ifndef DJDRIVER_PLOYTEC_H
#define DJDRIVER_PLOYTEC_H

#include <stddef.h>
#include <stdint.h>

/* USB IDs of the Reloop Digital Jockey 2 Master Edition */
#define DJ2ME_VENDOR_ID			0x200C
#define DJ2ME_PRODUCT_ID		0x1009

/* Default endpoint addresses of Ploytec devices */
#define PT_EP_PCM_OUT			0x05
#define PT_EP_PCM_IN			0x86
#define PT_EP_MIDI_IN			0x83

/* Interfaces 0 and 1 must be switched to this alternate setting */
#define PT_NUM_INTERFACES		2
#define PT_ALT_SETTING			1

/* Control requests */
#define PT_REQ_FIRMWARE			0x56	/* 'V', bmRequestType 0xC0 */
#define PT_REQ_FIRMWARE_TYPE		0xC0
#define PT_REQ_STATUS			0x49	/* 'I', read 0xC0, write 0x40 */
#define PT_REQ_STATUS_READ_TYPE		0xC0
#define PT_REQ_STATUS_WRITE_TYPE	0x40
#define PT_REQ_SET_RATE			0x01	/* SET_CUR to an endpoint */
#define PT_REQ_SET_RATE_TYPE		0x22
#define PT_REQ_GET_RATE			0x81	/* GET_CUR from an endpoint */
#define PT_REQ_GET_RATE_TYPE		0xA2
#define PT_RATE_WVALUE			0x0100
#define PT_STATUS_STREAMING		0x20

/* Output stream framing */
#define PT_MIDI_IDLE_BYTE		0xFD	/* "no MIDI byte in this slot" */
#define PT_SYNC_BYTE			0xFF
#define PT_OUT_FRAME_SIZE		48	/* bytes per audio frame (8 ch x 24 bit, bit-scattered) */
#define PT_FRAMES_PER_UNIT		40	/* audio frames per output unit, both modes */

/*
 * Depending on the device/firmware the output endpoint is either a bulk or
 * an interrupt endpoint, and the framing differs slightly:
 *
 * Bulk: 4 sub-packets of 512 bytes per unit, each holding 10 frames
 *       (480 bytes), one MIDI byte at offset 480, a sync byte 0xFF at 481 and
 *       30 bytes of zero padding.
 *
 * Interrupt: 1928 bytes per unit, 40 frames with 2-byte MIDI slots at
 *       offsets 432, 914, 1396 and 1878 and no sync byte.
 */
enum pt_mode {
	PT_MODE_BULK,
	PT_MODE_INTERRUPT,
};

#define PT_BULK_SUBPKT_SIZE		512
#define PT_BULK_SUBPKTS_PER_UNIT	4
#define PT_BULK_MIDI_OFFSET		480
#define PT_BULK_SYNC_OFFSET		481
#define PT_BULK_UNIT_SIZE		(PT_BULK_SUBPKT_SIZE * PT_BULK_SUBPKTS_PER_UNIT)	/* 2048 */
#define PT_INT_UNIT_SIZE		1928

/* Capture stream: 10 sub-packets of 512 bytes (80 frames) per transfer */
#define PT_IN_TRANSFER_SIZE		5120

/* Size of one output unit (40 audio frames) in bytes */
size_t pt_out_unit_size(enum pt_mode mode);

/* Offset (inside a unit) of the slot that carries the next MIDI byte */
size_t pt_out_midi_offset(enum pt_mode mode);

/*
 * Initialise @units consecutive output units in @buf: silent audio, all MIDI
 * slots set to the idle byte, sync bytes in place, padding zeroed.
 */
void pt_out_init(uint8_t *buf, size_t units, enum pt_mode mode);

/* Put @midi_byte (or PT_MIDI_IDLE_BYTE) into the MIDI slot of unit @unit */
void pt_out_set_midi(uint8_t *buf, size_t unit, enum pt_mode mode, uint8_t midi_byte);

/* wValue for the "start streaming" status write: status | 0x20, sign-extended
 * to 16 bits like the vendor driver does. */
uint16_t pt_status_confirm_wvalue(uint8_t status);

/* 3-byte little-endian sample rate encoding used by SET_CUR/GET_CUR */
void pt_encode_rate(uint32_t rate, uint8_t out[3]);
uint32_t pt_decode_rate(const uint8_t in[3]);

/*
 * Remove filler bytes from a MIDI IN buffer in place. The device pads the
 * stream with bytes from 0xF0..0xFF (0xF9, 0xFB, 0xFD, 0xFF have been
 * observed) and never sends system messages, so all of those are dropped.
 * Returns the number of remaining bytes.
 */
size_t pt_midi_in_strip(uint8_t *buf, size_t len);

#endif /* DJDRIVER_PLOYTEC_H */
