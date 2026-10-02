/* SPDX-License-Identifier: MIT */
/*
 * Unit tests for the protocol and MIDI helpers (no hardware needed).
 */

#include <stdio.h>
#include <string.h>

#include "../src/midi.h"
#include "../src/ploytec.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void test_bulk_layout(void)
{
	static uint8_t buf[PT_BULK_UNIT_SIZE * 2];
	size_t i, s;

	memset(buf, 0xAA, sizeof(buf));
	pt_out_init(buf, 2, PT_MODE_BULK);

	CHECK(pt_out_unit_size(PT_MODE_BULK) == 2048);
	for (s = 0; s < 8; s++) {
		uint8_t *sub = buf + s * 512;

		for (i = 0; i < 480; i++)
			CHECK(sub[i] == 0);
		CHECK(sub[480] == 0xFD);
		CHECK(sub[481] == 0xFF);
		for (i = 482; i < 512; i++)
			CHECK(sub[i] == 0);
	}

	pt_out_set_midi(buf, 1, PT_MODE_BULK, 0x90);
	CHECK(buf[2048 + 480] == 0x90);
	CHECK(buf[480] == 0xFD);
	CHECK(buf[2048 + 512 + 480] == 0xFD);	/* only one byte per unit */
}

static void test_interrupt_layout(void)
{
	static uint8_t buf[PT_INT_UNIT_SIZE];
	const size_t slots[] = { 432, 914, 1396, 1878 };
	size_t i, j, nonzero = 0;

	pt_out_init(buf, 1, PT_MODE_INTERRUPT);
	CHECK(pt_out_unit_size(PT_MODE_INTERRUPT) == 1928);
	for (i = 0; i < 4; i++) {
		CHECK(buf[slots[i]] == 0xFD);
		CHECK(buf[slots[i] + 1] == 0xFD);
	}
	for (j = 0; j < sizeof(buf); j++)
		nonzero += buf[j] != 0;
	CHECK(nonzero == 8);	/* audio frames are silent, no sync bytes */

	pt_out_set_midi(buf, 0, PT_MODE_INTERRUPT, 0x42);
	CHECK(buf[432] == 0x42);
	CHECK(buf[433] == 0xFD);
}

static void test_status_and_rate(void)
{
	uint8_t r[3];

	CHECK(pt_status_confirm_wvalue(0x00) == 0x0020);
	CHECK(pt_status_confirm_wvalue(0x12) == 0x0032);
	CHECK(pt_status_confirm_wvalue(0x82) == 0xFFA2);	/* sign-extended */

	pt_encode_rate(44100, r);
	CHECK(r[0] == 0x44 && r[1] == 0xAC && r[2] == 0x00);
	CHECK(pt_decode_rate(r) == 44100);
	pt_encode_rate(96000, r);
	CHECK(pt_decode_rate(r) == 96000);
}

static void test_midi_in_strip(void)
{
	uint8_t buf[] = { 0xFD, 0x90, 0xFD, 0x10, 0xF9, 0x7F, 0xFB, 0xFF, 0xB0, 0x01, 0x40 };
	const uint8_t want[] = { 0x90, 0x10, 0x7F, 0xB0, 0x01, 0x40 };
	size_t n = pt_midi_in_strip(buf, sizeof(buf));

	CHECK(n == sizeof(want));
	CHECK(memcmp(buf, want, sizeof(want)) == 0);
}

static void test_parser(void)
{
	struct midi_parser p;
	/* note on, running-status note on, CC, program change, pitch bend */
	const uint8_t in[] = { 0x90, 0x3C, 0x7F, 0x3C, 0x00, 0xB1, 0x07, 0x64,
			       0xC2, 0x05, 0x06, 0xE0, 0x00, 0x40 };
	const uint8_t want[][3] = {
		{ 0x90, 0x3C, 0x7F }, { 0x90, 0x3C, 0x00 }, { 0xB1, 0x07, 0x64 },
		{ 0xC2, 0x05, 0 }, { 0xC2, 0x06, 0 }, { 0xE0, 0x00, 0x40 },
	};
	const int want_len[] = { 3, 3, 3, 2, 2, 3 };
	size_t i;
	int n, got = 0;

	midi_parser_reset(&p);
	for (i = 0; i < sizeof(in); i++) {
		n = midi_parser_feed(&p, in[i]);
		if (n > 0) {
			CHECK(got < 6);
			if (got >= 6)
				break;
			CHECK(n == want_len[got]);
			CHECK(memcmp(p.msg, want[got], (size_t)n) == 0);
			got++;
		}
	}
	CHECK(got == 6);

	/* data without status is dropped, SysEx and real-time are ignored */
	midi_parser_reset(&p);
	CHECK(midi_parser_feed(&p, 0x40) == 0);
	CHECK(midi_parser_feed(&p, 0xF0) == 0);
	CHECK(midi_parser_feed(&p, 0x01) == 0);
	CHECK(midi_parser_feed(&p, 0xF7) == 0);
	CHECK(midi_parser_feed(&p, 0x80) == 0);
	CHECK(midi_parser_feed(&p, 0xF8) == 0);	/* clock in the middle of a message */
	CHECK(midi_parser_feed(&p, 0x10) == 0);
	CHECK(midi_parser_feed(&p, 0x00) == 3);
	CHECK(p.msg[0] == 0x80 && p.msg[1] == 0x10 && p.msg[2] == 0x00);
}

static void test_out_queue(void)
{
	struct midi_out_queue q;
	const uint8_t msg[3] = { 0x90, 0x01, 0x7F };
	const uint8_t bad[2] = { 0x90, 0x01 };
	uint8_t b;
	int i, accepted = 0;

	midi_out_queue_init(&q);
	CHECK(!midi_out_queue_pop(&q, &b));
	CHECK(!midi_out_queue_push_msg(&q, bad, 2));	/* wrong length */
	CHECK(midi_out_queue_push_msg(&q, msg, 3));
	CHECK(midi_out_queue_len(&q) == 3);
	CHECK(midi_out_queue_pop(&q, &b) && b == 0x90);
	CHECK(midi_out_queue_pop(&q, &b) && b == 0x01);
	CHECK(midi_out_queue_pop(&q, &b) && b == 0x7F);
	CHECK(!midi_out_queue_pop(&q, &b));

	/* fill up: messages are never split */
	for (i = 0; i < MIDI_OUT_QUEUE_SIZE; i++)
		accepted += midi_out_queue_push_msg(&q, msg, 3);
	CHECK(accepted == (MIDI_OUT_QUEUE_SIZE - 1) / 3);
	CHECK(midi_out_queue_len(&q) % 3 == 0);
	CHECK(q.dropped > 0);
	midi_out_queue_clear(&q);
	CHECK(midi_out_queue_len(&q) == 0);
	midi_out_queue_destroy(&q);
}

int main(void)
{
	test_bulk_layout();
	test_interrupt_layout();
	test_status_and_rate();
	test_midi_in_strip();
	test_parser();
	test_out_queue();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("All tests passed\n");
	return 0;
}
