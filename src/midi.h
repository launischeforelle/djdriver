/* SPDX-License-Identifier: MIT */
/*
 * Small MIDI 1.0 helpers: a byte-stream parser for the device -> host
 * direction and a thread-safe byte queue for the host -> device direction.
 */

#ifndef DJDRIVER_MIDI_H
#define DJDRIVER_MIDI_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Number of data bytes that follow a channel voice status byte */
int midi_data_len(uint8_t status);

/*
 * Assembles complete channel voice messages from a raw byte stream, expanding
 * running status so that every message handed to the host carries its
 * status byte. Bytes >= 0xF0 must already have been removed.
 */
struct midi_parser {
	uint8_t msg[3];
	uint8_t len;
	uint8_t need;
};

void midi_parser_reset(struct midi_parser *p);

/*
 * Feed one byte. Returns the length (2 or 3) of a completed message, which
 * is then available in p->msg, or 0 if no message is complete yet.
 */
int midi_parser_feed(struct midi_parser *p, uint8_t byte);

/*
 * Queue of bytes waiting to be embedded into the output stream. The firmware
 * does not understand running status, so only complete channel voice
 * messages (with status byte) are accepted, and messages are queued
 * atomically: either the whole message fits or it is dropped.
 */
#define MIDI_OUT_QUEUE_SIZE 4096	/* must be a power of two */

struct midi_out_queue {
	pthread_mutex_t lock;
	uint8_t buf[MIDI_OUT_QUEUE_SIZE];
	size_t head;	/* next byte to read */
	size_t tail;	/* next byte to write */
	unsigned long dropped;
};

void midi_out_queue_init(struct midi_out_queue *q);
void midi_out_queue_destroy(struct midi_out_queue *q);
void midi_out_queue_clear(struct midi_out_queue *q);

/* Push one complete message (status + data bytes); returns false if dropped */
bool midi_out_queue_push_msg(struct midi_out_queue *q, const uint8_t *msg, size_t len);

/* Pop one byte; returns false if the queue is empty */
bool midi_out_queue_pop(struct midi_out_queue *q, uint8_t *byte);

size_t midi_out_queue_len(struct midi_out_queue *q);

#endif /* DJDRIVER_MIDI_H */
