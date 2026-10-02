/* SPDX-License-Identifier: MIT */
/*
 * djdriver - userspace MIDI driver for the Reloop Digital Jockey 2 Master
 * Edition.
 *
 * Talks to the controller over libusb and publishes a virtual MIDI port
 * ("Reloop DJ2 ME") that DJ software can use like any class-compliant
 * controller. Waits for the controller and reconnects automatically.
 */

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb.h>

#include "device.h"
#include "midi.h"
#include "ploytec.h"
#include "port.h"

#define DEFAULT_PORT_NAME	"Reloop DJ2 ME"
#define DJDRIVER_VERSION	"0.1.0"

static volatile sig_atomic_t g_stop;

/*
 * Last value sent by the DJ software for every note / controller, so LEDs
 * can be restored when the controller is replugged while the software keeps
 * running (most DJ software only sends LED states when they change).
 */
#define CACHE_UNSET 0xFF

struct app {
	struct dj_config cfg;
	struct midi_port *port;
	struct midi_out_queue out_queue;
	struct midi_parser out_parser;	/* only used from the MIDI backend thread */
	pthread_mutex_t cache_lock;
	uint8_t note_cache[16][128];
	uint8_t cc_cache[16][128];
	int monitor;
};

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void sleep_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void print_msg(const char *dir, const uint8_t *msg, size_t len)
{
	size_t i;

	printf("%s", dir);
	for (i = 0; i < len; i++)
		printf(" %02X", msg[i]);
	printf("\n");
	fflush(stdout);
}

static void cache_update(struct app *app, const uint8_t *msg)
{
	uint8_t ch = msg[0] & 0x0F;

	pthread_mutex_lock(&app->cache_lock);
	switch (msg[0] & 0xF0) {
	case 0x80:
		app->note_cache[ch][msg[1]] = 0;
		break;
	case 0x90:
		app->note_cache[ch][msg[1]] = msg[2];
		break;
	case 0xB0:
		app->cc_cache[ch][msg[1]] = msg[2];
		break;
	}
	pthread_mutex_unlock(&app->cache_lock);
}

static void cache_replay(struct app *app)
{
	uint8_t msg[3];
	int ch, n;

	pthread_mutex_lock(&app->cache_lock);
	for (ch = 0; ch < 16; ch++) {
		for (n = 0; n < 128; n++) {
			if (app->note_cache[ch][n] != CACHE_UNSET) {
				msg[0] = (uint8_t)(0x90 | ch);
				msg[1] = (uint8_t)n;
				msg[2] = app->note_cache[ch][n];
				midi_out_queue_push_msg(&app->out_queue, msg, 3);
			}
			if (app->cc_cache[ch][n] != CACHE_UNSET) {
				msg[0] = (uint8_t)(0xB0 | ch);
				msg[1] = (uint8_t)n;
				msg[2] = app->cc_cache[ch][n];
				midi_out_queue_push_msg(&app->out_queue, msg, 3);
			}
		}
	}
	pthread_mutex_unlock(&app->cache_lock);
}

/* DJ software -> controller (called on a CoreMIDI thread) */
static void on_port_rx(void *user, const uint8_t *data, size_t len)
{
	struct app *app = user;
	size_t i;
	int n;

	for (i = 0; i < len; i++) {
		n = midi_parser_feed(&app->out_parser, data[i]);
		if (n <= 0)
			continue;
		if (app->monitor)
			print_msg("to controller  ", app->out_parser.msg, (size_t)n);
		cache_update(app, app->out_parser.msg);
		midi_out_queue_push_msg(&app->out_queue, app->out_parser.msg, (size_t)n);
	}
}

/* controller -> DJ software (called on the USB event thread) */
static void on_midi_in(void *user, const uint8_t *msg, size_t len)
{
	struct app *app = user;

	if (app->monitor)
		print_msg("from controller", msg, len);
	midi_port_send(app->port, msg, len);
}

/* Light every note 0..127 on channel 1 one after another */
static void led_test(libusb_context *ctx, struct app *app, struct dj_device *dev)
{
	struct timeval tv = { 0, 10000 };
	uint8_t on[3] = { 0x90, 0, 0x7F }, off[3] = { 0x80, 0, 0 };
	int note, i;

	printf("LED test: every note on MIDI channel 1 is lit for 0.3 s.\n");
	for (note = 0; note < 128 && !g_stop && dj_alive(dev); note++) {
		on[1] = off[1] = (uint8_t)note;
		printf("  note %3d (0x%02X)\n", note, note);
		fflush(stdout);
		midi_out_queue_push_msg(&app->out_queue, on, 3);
		for (i = 0; i < 30; i++)
			libusb_handle_events_timeout_completed(ctx, &tv, NULL);
		midi_out_queue_push_msg(&app->out_queue, off, 3);
	}
	for (i = 0; i < 20; i++)
		libusb_handle_events_timeout_completed(ctx, &tv, NULL);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"djdriver %s - driver for the Reloop Digital Jockey 2 Master Edition\n"
		"\n"
		"Usage: %s [command] [options]\n"
		"\n"
		"Commands:\n"
		"  run        run the driver (default)\n"
		"  monitor    run the driver and print all MIDI messages\n"
		"  probe      print USB descriptors and device status, then exit\n"
		"  ledtest    light note 0..127 on channel 1 one by one, then exit\n"
		"\n"
		"Options:\n"
		"  -n, --name NAME     name of the virtual MIDI port (default \"%s\")\n"
		"  -r, --rate HZ       sample rate to program (default 44100)\n"
		"  -v, --verbose       verbose log output\n"
		"      --vid HEX       USB vendor ID  (default %04x)\n"
		"      --pid HEX       USB product ID (default %04x)\n"
		"      --ep-out HEX    override output endpoint (default 0x05)\n"
		"      --ep-midi HEX   override MIDI input endpoint (default 0x83)\n"
		"      --ep-pcm HEX    override audio input endpoint (default 0x86)\n"
		"  -h, --help          this help\n",
		DJDRIVER_VERSION, prog, DEFAULT_PORT_NAME,
		DJ2ME_VENDOR_ID, DJ2ME_PRODUCT_ID);
}

static long parse_num(const char *s, int base)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, base);
	if (errno || *end || v < 0) {
		fprintf(stderr, "Invalid number: %s\n", s);
		exit(2);
	}
	return v;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "name",    required_argument, NULL, 'n' },
		{ "rate",    required_argument, NULL, 'r' },
		{ "verbose", no_argument,       NULL, 'v' },
		{ "vid",     required_argument, NULL, 1 },
		{ "pid",     required_argument, NULL, 2 },
		{ "ep-out",  required_argument, NULL, 3 },
		{ "ep-midi", required_argument, NULL, 4 },
		{ "ep-pcm",  required_argument, NULL, 5 },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	const char *name = DEFAULT_PORT_NAME;
	const char *cmd = "run";
	libusb_context *ctx = NULL;
	struct dj_device *dev;
	struct app *app;
	int c, r, waiting = 0, ret = 0;

	app = calloc(1, sizeof(*app));
	if (!app)
		return 1;
	dj_config_defaults(&app->cfg);

	while ((c = getopt_long(argc, argv, "n:r:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'n': name = optarg; break;
		case 'r': app->cfg.rate = (uint32_t)parse_num(optarg, 10); break;
		case 'v': app->cfg.verbose = 1; break;
		case 1:   app->cfg.vid = (uint16_t)parse_num(optarg, 16); break;
		case 2:   app->cfg.pid = (uint16_t)parse_num(optarg, 16); break;
		case 3:   app->cfg.ep_out = (int)parse_num(optarg, 16); break;
		case 4:   app->cfg.ep_midi_in = (int)parse_num(optarg, 16); break;
		case 5:   app->cfg.ep_pcm_in = (int)parse_num(optarg, 16); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}
	if (optind < argc)
		cmd = argv[optind++];
	if (optind < argc || (strcmp(cmd, "run") && strcmp(cmd, "monitor") &&
			      strcmp(cmd, "probe") && strcmp(cmd, "ledtest"))) {
		usage(argv[0]);
		return 2;
	}

	r = libusb_init(&ctx);
	if (r) {
		fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(r));
		return 1;
	}

	if (!strcmp(cmd, "probe")) {
		ret = dj_probe(ctx, &app->cfg);
		libusb_exit(ctx);
		return ret;
	}

	app->monitor = !strcmp(cmd, "monitor");
	midi_out_queue_init(&app->out_queue);
	midi_parser_reset(&app->out_parser);
	pthread_mutex_init(&app->cache_lock, NULL);
	memset(app->note_cache, CACHE_UNSET, sizeof(app->note_cache));
	memset(app->cc_cache, CACHE_UNSET, sizeof(app->cc_cache));

	app->port = midi_port_open(name, on_port_rx, app);
	if (!app->port) {
		fprintf(stderr, "Cannot create virtual MIDI port\n");
		libusb_exit(ctx);
		return 1;
	}
	fprintf(stderr, "Virtual MIDI port \"%s\" created\n", name);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while (!g_stop) {
		if (!dj_present(ctx, &app->cfg)) {
			if (!waiting)
				fprintf(stderr, "Waiting for controller %04x:%04x ...\n",
					app->cfg.vid, app->cfg.pid);
			waiting = 1;
			sleep_ms(1000);
			continue;
		}
		waiting = 0;

		midi_out_queue_clear(&app->out_queue);
		dev = dj_open(ctx, &app->cfg, &app->out_queue, on_midi_in, app);
		if (!dev) {
			fprintf(stderr, "Initialising the controller failed, retrying ...\n");
			sleep_ms(3000);
			continue;
		}
		cache_replay(app);

		if (!strcmp(cmd, "ledtest")) {
			led_test(ctx, app, dev);
			dj_close(dev);
			break;
		}

		while (!g_stop && dj_alive(dev)) {
			struct timeval tv = { 0, 100000 };

			libusb_handle_events_timeout_completed(ctx, &tv, NULL);
		}
		dj_close(dev);
		if (!g_stop)
			sleep_ms(500);
	}

	fprintf(stderr, "Shutting down\n");
	midi_port_close(app->port);
	libusb_exit(ctx);
	midi_out_queue_destroy(&app->out_queue);
	pthread_mutex_destroy(&app->cache_lock);
	free(app);
	return ret;
}
