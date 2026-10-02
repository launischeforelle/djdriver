/* SPDX-License-Identifier: MIT */
/*
 * Small MIDI 1.0 helpers.
 */

#include "midi.h"

int midi_data_len(uint8_t status)
{
	switch (status & 0xF0) {
	case 0xC0:	/* program change */
	case 0xD0:	/* channel pressure */
		return 1;
	case 0x80:	/* note off */
	case 0x90:	/* note on */
	case 0xA0:	/* poly pressure */
	case 0xB0:	/* control change */
	case 0xE0:	/* pitch bend */
		return 2;
	default:
		return -1;	/* not a channel voice status */
	}
}

void midi_parser_reset(struct midi_parser *p)
{
	p->msg[0] = 0;
	p->len = 0;
	p->need = 0;
}

int midi_parser_feed(struct midi_parser *p, uint8_t byte)
{
	if (byte >= 0xF8)
		return 0;	/* real-time: ignored, does not affect running status */

	if (byte >= 0xF0) {
		/* system common / SysEx: cancels running status, ignored */
		midi_parser_reset(p);
		return 0;
	}

	if (byte & 0x80) {
		p->msg[0] = byte;
		p->len = 1;
		p->need = (uint8_t)midi_data_len(byte);
		return 0;
	}

	/* data byte */
	if (p->msg[0] == 0)
		return 0;	/* no status yet: unusable */

	if (p->len == 0 || p->len > p->need)
		p->len = 1;	/* running status: reuse the stored status byte */

	p->msg[p->len++] = byte;
	if (p->len == p->need + 1) {
		int complete = p->len;

		p->len = 0;	/* keep msg[0] as running status */
		return complete;
	}
	return 0;
}

void midi_out_queue_init(struct midi_out_queue *q)
{
	pthread_mutex_init(&q->lock, NULL);
	q->head = 0;
	q->tail = 0;
	q->dropped = 0;
}

void midi_out_queue_destroy(struct midi_out_queue *q)
{
	pthread_mutex_destroy(&q->lock);
}

void midi_out_queue_clear(struct midi_out_queue *q)
{
	pthread_mutex_lock(&q->lock);
	q->head = q->tail = 0;
	pthread_mutex_unlock(&q->lock);
}

static size_t queue_used(const struct midi_out_queue *q)
{
	return (q->tail - q->head) & (MIDI_OUT_QUEUE_SIZE - 1);
}

bool midi_out_queue_push_msg(struct midi_out_queue *q, const uint8_t *msg, size_t len)
{
	size_t i;
	bool ok;

	if (len == 0 || midi_data_len(msg[0]) != (int)len - 1)
		return false;

	pthread_mutex_lock(&q->lock);
	/* one slot always stays free to tell "full" from "empty" */
	ok = MIDI_OUT_QUEUE_SIZE - 1 - queue_used(q) >= len;
	if (ok) {
		for (i = 0; i < len; i++) {
			q->buf[q->tail] = msg[i];
			q->tail = (q->tail + 1) & (MIDI_OUT_QUEUE_SIZE - 1);
		}
	} else {
		q->dropped++;
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

bool midi_out_queue_pop(struct midi_out_queue *q, uint8_t *byte)
{
	bool ok;

	pthread_mutex_lock(&q->lock);
	ok = q->head != q->tail;
	if (ok) {
		*byte = q->buf[q->head];
		q->head = (q->head + 1) & (MIDI_OUT_QUEUE_SIZE - 1);
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

size_t midi_out_queue_len(struct midi_out_queue *q)
{
	size_t n;

	pthread_mutex_lock(&q->lock);
	n = queue_used(q);
	pthread_mutex_unlock(&q->lock);
	return n;
}
