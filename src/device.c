/* SPDX-License-Identifier: MIT */
/*
 * USB side of the driver: Ploytec handshake and streaming via libusb.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "device.h"
#include "ploytec.h"

#define CTRL_TIMEOUT_MS		2000
#define NUM_OUT_XFERS		4	/* output transfers kept in flight */
#define UNITS_PER_OUT_XFER	2	/* 80 audio frames per transfer */
#define NUM_MIDI_IN_XFERS	2
#define NUM_PCM_IN_XFERS	2
#define MAX_CONSEC_ERRORS	50

#define LOG(dev, ...)	do { fprintf(stderr, __VA_ARGS__); } while (0)
#define DBG(dev, ...)	do { if ((dev)->cfg.verbose) fprintf(stderr, __VA_ARGS__); } while (0)

enum xfer_kind {
	XFER_OUT,
	XFER_MIDI_IN,
	XFER_PCM_IN,
};

struct endpoint {
	uint8_t addr;		/* 0 = not present */
	uint8_t type;		/* LIBUSB_TRANSFER_TYPE_BULK / _INTERRUPT */
	int max_packet;
};

struct dj_xfer {
	struct dj_device *dev;
	struct libusb_transfer *t;
	enum xfer_kind kind;
	uint8_t *buf;
};

struct dj_device {
	struct dj_config cfg;
	libusb_context *ctx;
	libusb_device_handle *h;
	int claimed[PT_NUM_INTERFACES];

	struct endpoint ep_out;
	struct endpoint ep_midi_in;
	struct endpoint ep_pcm_in;
	enum pt_mode mode;

	struct midi_out_queue *out_queue;
	struct midi_parser in_parser;
	dj_midi_in_fn midi_in;
	void *user;

	struct dj_xfer xfers[NUM_OUT_XFERS + NUM_MIDI_IN_XFERS + NUM_PCM_IN_XFERS];
	int num_xfers;
	int active;		/* transfers currently submitted */
	int stopping;
	int failed;
	struct timespec t0;	/* start of the handshake */
	const char *step;	/* current handshake step, for error messages */
	int seen_data[3];	/* per xfer_kind: first completion logged */
	int consec_errors;
};

static void sleep_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Milliseconds since the handshake started, for the verbose log */
static double elapsed_ms(const struct dj_device *dev)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)(ts.tv_sec - dev->t0.tv_sec) * 1000.0 +
	       (double)(ts.tv_nsec - dev->t0.tv_nsec) / 1e6;
}

void dj_config_defaults(struct dj_config *cfg)
{
	cfg->vid = DJ2ME_VENDOR_ID;
	cfg->pid = DJ2ME_PRODUCT_ID;
	cfg->rate = 44100;
	cfg->ep_out = -1;
	cfg->ep_midi_in = -1;
	cfg->ep_pcm_in = -1;
	cfg->capture = 0;
	cfg->leds = 0;
	cfg->verbose = 0;
}

static libusb_device *find_device(libusb_context *ctx, const struct dj_config *cfg)
{
	libusb_device **list, *found = NULL;
	struct libusb_device_descriptor desc;
	ssize_t n, i;

	n = libusb_get_device_list(ctx, &list);
	if (n < 0)
		return NULL;
	for (i = 0; i < n && !found; i++) {
		if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
		    desc.idVendor == cfg->vid && desc.idProduct == cfg->pid)
			found = libusb_ref_device(list[i]);
	}
	libusb_free_device_list(list, 1);
	return found;
}

bool dj_present(libusb_context *ctx, const struct dj_config *cfg)
{
	libusb_device *d = find_device(ctx, cfg);

	if (!d)
		return false;
	libusb_unref_device(d);
	return true;
}

static const char *type_name(uint8_t attr)
{
	switch (attr & LIBUSB_TRANSFER_TYPE_MASK) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:	return "control";
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:	return "isochronous";
	case LIBUSB_TRANSFER_TYPE_BULK:		return "bulk";
	default:				return "interrupt";
	}
}

/* ---------------------------------------------------------------------- */
/* Control requests                                                        */
/* ---------------------------------------------------------------------- */

static int read_firmware(libusb_device_handle *h, uint8_t *buf, int len)
{
	return libusb_control_transfer(h, PT_REQ_FIRMWARE_TYPE, PT_REQ_FIRMWARE,
				       0, 0, buf, (uint16_t)len, CTRL_TIMEOUT_MS);
}

static int read_status(libusb_device_handle *h, uint8_t *status)
{
	int r = libusb_control_transfer(h, PT_REQ_STATUS_READ_TYPE, PT_REQ_STATUS,
					0, 0, status, 1, CTRL_TIMEOUT_MS);

	return r == 1 ? 0 : (r < 0 ? r : LIBUSB_ERROR_IO);
}

static int read_rate(libusb_device_handle *h, uint16_t index, uint32_t *rate)
{
	uint8_t buf[3];
	int r = libusb_control_transfer(h, PT_REQ_GET_RATE_TYPE, PT_REQ_GET_RATE,
					PT_RATE_WVALUE, index, buf, 3, CTRL_TIMEOUT_MS);

	if (r != 3)
		return r < 0 ? r : LIBUSB_ERROR_IO;
	*rate = pt_decode_rate(buf);
	return 0;
}

static int write_rate(libusb_device_handle *h, uint16_t index, uint32_t rate)
{
	uint8_t buf[3];
	int r;

	pt_encode_rate(rate, buf);
	r = libusb_control_transfer(h, PT_REQ_SET_RATE_TYPE, PT_REQ_SET_RATE,
				    PT_RATE_WVALUE, index, buf, 3, CTRL_TIMEOUT_MS);
	return r == 3 ? 0 : (r < 0 ? r : LIBUSB_ERROR_IO);
}

static void print_firmware(libusb_device_handle *h)
{
	uint8_t fw[15];
	int r, i;

	r = read_firmware(h, fw, sizeof(fw));
	if (r < 0) {
		printf("  Firmware:  read failed (%s)\n", libusb_error_name(r));
		return;
	}
	printf("  Firmware:  ");
	for (i = 0; i < r; i++)
		printf("%02X ", fw[i]);
	printf("\n");
}

/* ---------------------------------------------------------------------- */
/* Probe                                                                   */
/* ---------------------------------------------------------------------- */

int dj_probe(libusb_context *ctx, const struct dj_config *cfg)
{
	struct libusb_device_descriptor desc;
	struct libusb_config_descriptor *conf;
	libusb_device_handle *h;
	libusb_device *d;
	uint8_t status;
	int i, a, e, r;

	d = find_device(ctx, cfg);
	if (!d) {
		printf("No device %04x:%04x found.\n", cfg->vid, cfg->pid);
		return 1;
	}

	libusb_get_device_descriptor(d, &desc);
	printf("Device %04x:%04x on bus %u, address %u, speed %d (1=low 2=full 3=high 4=super)\n",
	       desc.idVendor, desc.idProduct, libusb_get_bus_number(d),
	       libusb_get_device_address(d), libusb_get_device_speed(d));
	printf("  bcdUSB %04x, bcdDevice %04x, class %02x, %u configuration(s)\n",
	       desc.bcdUSB, desc.bcdDevice, desc.bDeviceClass, desc.bNumConfigurations);

	for (i = 0; i < desc.bNumConfigurations; i++) {
		if (libusb_get_config_descriptor(d, (uint8_t)i, &conf) != 0)
			continue;
		printf("  Configuration %u: %u interface(s)\n",
		       conf->bConfigurationValue, conf->bNumInterfaces);
		for (a = 0; a < conf->bNumInterfaces; a++) {
			const struct libusb_interface *itf = &conf->interface[a];
			int s;

			for (s = 0; s < itf->num_altsetting; s++) {
				const struct libusb_interface_descriptor *alt = &itf->altsetting[s];

				printf("    Interface %u alt %u: class %02x/%02x/%02x, %u endpoint(s)\n",
				       alt->bInterfaceNumber, alt->bAlternateSetting,
				       alt->bInterfaceClass, alt->bInterfaceSubClass,
				       alt->bInterfaceProtocol, alt->bNumEndpoints);
				for (e = 0; e < alt->bNumEndpoints; e++) {
					const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];

					printf("      EP 0x%02x %-3s %-11s maxpacket %u interval %u\n",
					       ep->bEndpointAddress,
					       (ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
					       type_name(ep->bmAttributes),
					       ep->wMaxPacketSize, ep->bInterval);
				}
			}
		}
		libusb_free_config_descriptor(conf);
	}

	r = libusb_open(d, &h);
	libusb_unref_device(d);
	if (r) {
		printf("  Cannot open device: %s\n", libusb_error_name(r));
		return 1;
	}
	print_firmware(h);
	r = read_status(h, &status);
	if (r)
		printf("  Status:    read failed (%s)\n", libusb_error_name(r));
	else
		printf("  Status:    0x%02X\n", status);
	/* no rate query here: the DJ2 ME firmware crashes on it */
	libusb_close(h);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Step-by-step diagnosis                                                  */
/* ---------------------------------------------------------------------- */

struct diag_ctx {
	libusb_device_handle *h;
	struct timespec t0;
	unsigned int pause_ms;
	struct libusb_transfer *xfers[3];
	int nxfers;
	int out_done, midi_done, pcm_done, xfer_err;
	int midi_bytes;
	uint8_t first_midi[16];
	int first_midi_len;
	struct midi_parser parser;
	int live;		/* print MIDI messages as they arrive */
	int raw_bytes;		/* all bytes seen on EP 0x83, filler included */
};

static double diag_ms(const struct diag_ctx *d)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)(ts.tv_sec - d->t0.tv_sec) * 1000.0 +
	       (double)(ts.tv_nsec - d->t0.tv_nsec) / 1e6;
}

/* Keep libusb (and thus any running streams) going while waiting */
static void diag_wait(libusb_context *ctx, unsigned int ms)
{
	struct timeval tv = { 0, 10000 };
	unsigned int i;

	for (i = 0; i < ms / 10; i++)
		libusb_handle_events_timeout_completed(ctx, &tv, NULL);
}

/*
 * Report the result of a step, wait, then check that the device still
 * answers a status request. Returns false if the device is gone.
 */
static bool diag_check(libusb_context *ctx, struct diag_ctx *d, const char *what, int r)
{
	uint8_t status;
	int s;

	printf("[%7.1f ms] %-34s -> %s\n", diag_ms(d), what,
	       r >= 0 ? "ok" : libusb_error_name(r));
	fflush(stdout);
	diag_wait(ctx, d->pause_ms);
	s = read_status(d->h, &status);
	if (s) {
		printf("[%7.1f ms]   device no longer answers (%s)\n", diag_ms(d),
		       libusb_error_name(s));
		return false;
	}
	printf("[%7.1f ms]   still alive, status 0x%02X\n", diag_ms(d), status);
	return true;
}

static void LIBUSB_CALL diag_cb(struct libusb_transfer *t)
{
	struct diag_ctx *d = t->user_data;

	if (t->status != LIBUSB_TRANSFER_COMPLETED) {
		if (t->status != LIBUSB_TRANSFER_CANCELLED)
			d->xfer_err = t->status;
		return;
	}
	if (t->endpoint == PT_EP_PCM_OUT) {
		d->out_done++;
	} else if (t->endpoint == PT_EP_MIDI_IN) {
		size_t n = pt_midi_in_strip(t->buffer, (size_t)t->actual_length), k;

		d->raw_bytes += t->actual_length;
		d->midi_done++;
		d->midi_bytes += (int)n;
		if (d->live) {
			int len;

			for (k = 0; k < n; k++) {
				len = midi_parser_feed(&d->parser, t->buffer[k]);
				if (len > 0) {
					printf("[%7.1f ms]   MIDI from controller:", diag_ms(d));
					for (int j = 0; j < len; j++)
						printf(" %02X", d->parser.msg[j]);
					printf("\n");
					fflush(stdout);
				}
			}
		}
		for (k = 0; k < n && d->first_midi_len < (int)sizeof(d->first_midi); k++)
			d->first_midi[d->first_midi_len++] = t->buffer[k];
	} else {
		d->pcm_done++;
	}
	if (libusb_submit_transfer(t))
		d->xfer_err = -1;
}

static int diag_stream(struct diag_ctx *d, uint8_t ep, uint8_t *buf, int len)
{
	struct libusb_transfer *t = libusb_alloc_transfer(0);
	int r;

	if (!t)
		return LIBUSB_ERROR_NO_MEM;
	libusb_fill_bulk_transfer(t, d->h, ep, buf, len, diag_cb, d, 0);
	r = libusb_submit_transfer(t);
	if (r)
		libusb_free_transfer(t);
	else
		d->xfers[d->nxfers++] = t;
	return r;
}

static void diag_stop_streams(libusb_context *ctx, struct diag_ctx *d)
{
	int i;

	for (i = 0; i < d->nxfers; i++)
		libusb_cancel_transfer(d->xfers[i]);
	diag_wait(ctx, 300);
	for (i = 0; i < d->nxfers; i++)
		libusb_free_transfer(d->xfers[i]);
	d->nxfers = 0;
}

/*
 * Input-only test: activate interface 0 (stable on the DJ2 ME) and only
 * listen on the MIDI input endpoint, without the streaming bit or any audio
 * request (those crash this controller). Then, separately, check whether
 * the output endpoint accepts data without the streaming bit.
 */
int dj_diag(libusb_context *ctx, const struct dj_config *cfg)
{
	static uint8_t midi_buf[2][512], out_buf[PT_BULK_UNIT_SIZE];
	struct diag_ctx d = { .pause_ms = 300, .live = 1 };
	libusb_device *dev;
	uint8_t fw[8];
	int r, i;
	bool listen_ok = false;

	midi_parser_reset(&d.parser);
	dev = find_device(ctx, cfg);
	if (!dev) {
		printf("No device %04x:%04x found.\n", cfg->vid, cfg->pid);
		return 1;
	}
	r = libusb_open(dev, &d.h);
	libusb_unref_device(dev);
	if (r) {
		printf("Cannot open device: %s\n", libusb_error_name(r));
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &d.t0);

	r = libusb_claim_interface(d.h, 0);
	if (!diag_check(ctx, &d, "claim interface 0", r) || r)
		goto out;
	r = read_firmware(d.h, fw, sizeof(fw));
	if (!diag_check(ctx, &d, "read firmware", r))
		goto out;
	r = libusb_set_interface_alt_setting(d.h, 0, PT_ALT_SETTING);
	if (!diag_check(ctx, &d, "interface 0 -> alt 1", r))
		goto out;
	r = libusb_clear_halt(d.h, PT_EP_MIDI_IN);
	if (!diag_check(ctx, &d, "clear halt 0x83", r))
		goto out;

	printf("--- Test 1: listen on MIDI input only (no streaming, no audio) ---\n");
	r = diag_stream(&d, PT_EP_MIDI_IN, midi_buf[0], sizeof(midi_buf[0]));
	if (!r)
		r = diag_stream(&d, PT_EP_MIDI_IN, midi_buf[1], sizeof(midi_buf[1]));
	if (!diag_check(ctx, &d, "start listening on 0x83", r) || r)
		goto out;
	printf("[%7.1f ms] listening for 20 s - PRESS BUTTONS, MOVE FADERS, TURN JOG WHEELS NOW\n",
	       diag_ms(&d));
	fflush(stdout);
	for (i = 0; i < 20 && !d.xfer_err; i++) {
		diag_wait(ctx, 1000);
		if (i % 5 == 4 && !diag_check(ctx, &d, "still listening", 0))
			goto out;
	}
	printf("[%7.1f ms]   USB packets on 0x83: %d, bytes: %d, MIDI bytes: %d, transfer error %d\n",
	       diag_ms(&d), d.midi_done, d.raw_bytes, d.midi_bytes, d.xfer_err);
	listen_ok = d.midi_bytes > 0;

	printf("--- Test 2: one output packet (LEDs) without streaming bit ---\n");
	pt_out_init(out_buf, 1, PT_MODE_BULK);
	d.xfer_err = 0;
	r = diag_stream(&d, PT_EP_PCM_OUT, out_buf, sizeof(out_buf));
	if (!diag_check(ctx, &d, "submit one output packet", r))
		goto out;
	diag_wait(ctx, 1000);
	printf("[%7.1f ms]   output packets accepted: %d, transfer error %d\n",
	       diag_ms(&d), d.out_done, d.xfer_err);
	diag_check(ctx, &d, "after output test", 0);

out:
	diag_stop_streams(ctx, &d);
	libusb_close(d.h);
	if (listen_ok)
		printf("\nResult: controls WORK in input-only mode (%d MIDI bytes received)\n",
		       d.midi_bytes);
	else
		printf("\nResult: no MIDI data received\n");
	return listen_ok ? 0 : 1;
}

/* ---------------------------------------------------------------------- */
/* Endpoint discovery                                                      */
/* ---------------------------------------------------------------------- */

static void take_endpoint(struct endpoint *dst, const struct libusb_endpoint_descriptor *ep)
{
	dst->addr = ep->bEndpointAddress;
	dst->type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
	dst->max_packet = ep->wMaxPacketSize & 0x7FF;
}

/*
 * Walk the active alternate settings of interfaces 0 and 1 and pick the
 * three streaming endpoints. Overrides from the command line win; otherwise
 * the well-known Ploytec addresses are used, with a fallback to the first
 * non-isochronous endpoint in the right direction for the output stream.
 */
static int discover_endpoints(struct dj_device *dev)
{
	const struct libusb_endpoint_descriptor *fallback_out = NULL;
	struct libusb_config_descriptor *conf;
	int i, s, e, r;

	r = libusb_get_active_config_descriptor(libusb_get_device(dev->h), &conf);
	if (r)
		return r;

	for (i = 0; i < conf->bNumInterfaces && i < PT_NUM_INTERFACES; i++) {
		const struct libusb_interface *itf = &conf->interface[i];

		if (!dev->claimed[i])
			continue;

		for (s = 0; s < itf->num_altsetting; s++) {
			const struct libusb_interface_descriptor *alt = &itf->altsetting[s];

			if (itf->num_altsetting > 1 && alt->bAlternateSetting != PT_ALT_SETTING)
				continue;
			for (e = 0; e < alt->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
				uint8_t addr = ep->bEndpointAddress;
				uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;

				if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
					continue;	/* clock sync, not used here */

				if (dev->cfg.ep_out >= 0 ? addr == dev->cfg.ep_out : addr == PT_EP_PCM_OUT)
					take_endpoint(&dev->ep_out, ep);
				else if (dev->cfg.ep_midi_in >= 0 ? addr == dev->cfg.ep_midi_in : addr == PT_EP_MIDI_IN)
					take_endpoint(&dev->ep_midi_in, ep);
				else if (dev->cfg.ep_pcm_in >= 0 ? addr == dev->cfg.ep_pcm_in : addr == PT_EP_PCM_IN)
					take_endpoint(&dev->ep_pcm_in, ep);
				else if (!(addr & 0x80) && !fallback_out)
					fallback_out = ep;
			}
		}
	}

	if (!dev->ep_out.addr && fallback_out && dev->cfg.ep_out < 0) {
		LOG(dev, "Warning: no EP 0x%02x, using output endpoint 0x%02x instead\n",
		    PT_EP_PCM_OUT, fallback_out->bEndpointAddress);
		take_endpoint(&dev->ep_out, fallback_out);
	}
	libusb_free_config_descriptor(conf);

	if (!dev->ep_out.addr) {
		LOG(dev, "Error: no usable output endpoint found (run 'djdriver probe')\n");
		return LIBUSB_ERROR_NOT_FOUND;
	}
	if (!dev->ep_midi_in.addr)
		LOG(dev, "Warning: no MIDI input endpoint found, controls will not work\n");

	dev->mode = dev->ep_out.type == LIBUSB_TRANSFER_TYPE_BULK ? PT_MODE_BULK : PT_MODE_INTERRUPT;
	DBG(dev, "Endpoints: out 0x%02x (%s, %d), midi in 0x%02x, pcm in 0x%02x\n",
	    dev->ep_out.addr, dev->mode == PT_MODE_BULK ? "bulk" : "interrupt",
	    dev->ep_out.max_packet, dev->ep_midi_in.addr, dev->ep_pcm_in.addr);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Streaming                                                               */
/* ---------------------------------------------------------------------- */

/* Put the next queued MIDI bytes (at most one per unit) into an output buffer */
static void fill_midi(struct dj_device *dev, uint8_t *buf)
{
	uint8_t byte;
	int u;

	for (u = 0; u < UNITS_PER_OUT_XFER; u++) {
		if (!midi_out_queue_pop(dev->out_queue, &byte))
			byte = PT_MIDI_IDLE_BYTE;
		pt_out_set_midi(buf, (size_t)u, dev->mode, byte);
	}
}

static void handle_midi_in(struct dj_device *dev, uint8_t *buf, int len)
{
	size_t n, i;
	int msg_len;

	n = pt_midi_in_strip(buf, (size_t)len);
	for (i = 0; i < n; i++) {
		msg_len = midi_parser_feed(&dev->in_parser, buf[i]);
		if (msg_len > 0 && dev->midi_in)
			dev->midi_in(dev->user, dev->in_parser.msg, (size_t)msg_len);
	}
}

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *t)
{
	struct dj_xfer *x = t->user_data;
	struct dj_device *dev = x->dev;
	int r;

	switch (t->status) {
	case LIBUSB_TRANSFER_COMPLETED:
		dev->consec_errors = 0;
		if (!dev->seen_data[x->kind]) {
			static const char *const names[] = { "output", "MIDI input", "audio input" };

			dev->seen_data[x->kind] = 1;
			DBG(dev, "[%7.1f ms] first %s transfer done (%d bytes)\n",
			    elapsed_ms(dev), names[x->kind], t->actual_length);
		}
		if (x->kind == XFER_OUT)
			fill_midi(dev, x->buf);
		else if (x->kind == XFER_MIDI_IN)
			handle_midi_in(dev, x->buf, t->actual_length);
		/* captured audio is discarded */
		break;
	case LIBUSB_TRANSFER_CANCELLED:
		dev->active--;
		return;
	case LIBUSB_TRANSFER_NO_DEVICE:
		if (!dev->failed)
			LOG(dev, "Device disconnected (%.0f ms after start-up began)\n",
			    elapsed_ms(dev));
		dev->failed = 1;
		dev->active--;
		return;
	default:
		if (++dev->consec_errors >= MAX_CONSEC_ERRORS) {
			LOG(dev, "Too many USB errors (last: %d on EP 0x%02x), giving up\n",
			    t->status, t->endpoint);
			dev->failed = 1;
		}
		if (t->status == LIBUSB_TRANSFER_STALL)
			libusb_clear_halt(dev->h, t->endpoint);
		break;
	}

	if (dev->stopping || dev->failed) {
		dev->active--;
		return;
	}
	r = libusb_submit_transfer(t);
	if (r == LIBUSB_ERROR_PIPE && libusb_clear_halt(dev->h, t->endpoint) == 0)
		r = libusb_submit_transfer(t);
	if (r) {
		LOG(dev, "Resubmitting transfer on EP 0x%02x failed: %s\n",
		    t->endpoint, libusb_error_name(r));
		dev->failed = 1;
		dev->active--;
	}
}

static int add_xfer(struct dj_device *dev, enum xfer_kind kind,
		    const struct endpoint *ep, int len)
{
	struct dj_xfer *x = &dev->xfers[dev->num_xfers];

	x->dev = dev;
	x->kind = kind;
	x->buf = calloc(1, (size_t)len);
	x->t = libusb_alloc_transfer(0);
	if (!x->buf || !x->t) {
		free(x->buf);
		libusb_free_transfer(x->t);
		return LIBUSB_ERROR_NO_MEM;
	}

	if (ep->type == LIBUSB_TRANSFER_TYPE_BULK)
		libusb_fill_bulk_transfer(x->t, dev->h, ep->addr, x->buf, len, xfer_cb, x, 0);
	else
		libusb_fill_interrupt_transfer(x->t, dev->h, ep->addr, x->buf, len, xfer_cb, x, 0);

	if (kind == XFER_OUT) {
		pt_out_init(x->buf, UNITS_PER_OUT_XFER, dev->mode);
		fill_midi(dev, x->buf);
	}
	dev->num_xfers++;
	return 0;
}

static int start_streaming(struct dj_device *dev)
{
	int out_len = (int)(pt_out_unit_size(dev->mode) * UNITS_PER_OUT_XFER);
	int i, r = 0;

	for (i = 0; i < NUM_OUT_XFERS && !r && dev->cfg.leds; i++)
		r = add_xfer(dev, XFER_OUT, &dev->ep_out, out_len);
	if (dev->ep_midi_in.addr)
		for (i = 0; i < NUM_MIDI_IN_XFERS && !r; i++)
			r = add_xfer(dev, XFER_MIDI_IN, &dev->ep_midi_in,
				     dev->ep_midi_in.max_packet > 0 ? dev->ep_midi_in.max_packet : 64);
	/* The capture stream is drained so that the device never stalls on a
	 * full input buffer; the audio itself is not used (yet). */
	if (dev->ep_pcm_in.addr)
		for (i = 0; i < NUM_PCM_IN_XFERS && !r; i++)
			r = add_xfer(dev, XFER_PCM_IN, &dev->ep_pcm_in, PT_IN_TRANSFER_SIZE);
	if (r)
		return r;

	for (i = 0; i < dev->num_xfers; i++) {
		r = libusb_submit_transfer(dev->xfers[i].t);
		if (r) {
			LOG(dev, "Submitting transfer on EP 0x%02x failed: %s\n",
			    dev->xfers[i].t->endpoint, libusb_error_name(r));
			return r;
		}
		dev->active++;
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Open / close                                                            */
/* ---------------------------------------------------------------------- */

static void step(struct dj_device *dev, const char *name)
{
	dev->step = name;
	DBG(dev, "[%7.1f ms] %s\n", elapsed_ms(dev), name);
}

/* Report a failed step; returns true if the handshake has to be aborted */
static bool step_failed(struct dj_device *dev, int r, bool fatal)
{
	if (r >= 0)
		return false;
	if (r == LIBUSB_ERROR_NO_DEVICE) {
		LOG(dev, "Error: the controller dropped off the USB bus during step \"%s\" "
		    "(%.0f ms into the start-up sequence)\n", dev->step, elapsed_ms(dev));
		LOG(dev, "  Hint: if the controller's own power supply is not connected, try it "
		    "(or a powered USB hub): the audio part draws more current once it is "
		    "switched on.\n");
		return true;
	}
	LOG(dev, "%s: step \"%s\" failed: %s\n", fatal ? "Error" : "Warning",
	    dev->step, libusb_error_name(r));
	return fatal;
}

/*
 * Start-up sequence, modelled step by step (including the pauses) on USB
 * captures of the vendor's macOS driver made by the alsa-jockey3 project.
 * The firmware is sensitive to requests arriving faster than that.
 */
static int handshake(struct dj_device *dev)
{
	libusb_device_handle *h = dev->h;
	uint16_t idx_in, idx_out;
	uint16_t burst[5];
	uint8_t fw[8], status;
	uint32_t rate = 0;
	unsigned int i;
	int r;

	clock_gettime(CLOCK_MONOTONIC, &dev->t0);

	step(dev, "read firmware version");
	r = read_firmware(h, fw, sizeof(fw));
	if (step_failed(dev, r, false))
		return r;
	if (r >= 3)
		LOG(dev, "Firmware: %02X %02X %02X\n", fw[0], fw[1], fw[2]);
	sleep_ms(20);

	/* activate the streaming interfaces, ~16 ms apart like the vendor driver */
	for (i = 0; i < PT_NUM_INTERFACES; i++) {
		if (!dev->claimed[i])
			continue;
		step(dev, i == 0 ? "interface 0 -> alt 1" : "interface 1 -> alt 1");
		r = libusb_set_interface_alt_setting(h, (int)i, PT_ALT_SETTING);
		if (r < 0 && step_failed(dev, r, true))
			return r;
		sleep_ms(16);
	}

	r = discover_endpoints(dev);
	if (r)
		return r;
	idx_in = dev->ep_pcm_in.addr ? dev->ep_pcm_in.addr : PT_EP_PCM_IN;
	idx_out = dev->ep_out.addr;

	step(dev, "clear endpoint halt");
	if (dev->ep_pcm_in.addr && step_failed(dev, libusb_clear_halt(h, dev->ep_pcm_in.addr), false))
		return LIBUSB_ERROR_NO_DEVICE;
	if (step_failed(dev, libusb_clear_halt(h, dev->ep_out.addr), false))
		return LIBUSB_ERROR_NO_DEVICE;
	if (dev->ep_midi_in.addr && step_failed(dev, libusb_clear_halt(h, dev->ep_midi_in.addr), false))
		return LIBUSB_ERROR_NO_DEVICE;
	sleep_ms(6);

	/*
	 * Input-only mode (default): the DJ2 ME crashes as soon as its audio
	 * engine is started (streaming bit, rate requests, interface 1), but
	 * interface 0 alone is stable. Stop here and only read EP 0x83.
	 */
	if (!dev->cfg.leds)
		return 0;

	step(dev, "read status");
	r = read_status(h, &status);
	if (step_failed(dev, r, true))
		return r;
	DBG(dev, "           status 0x%02X\n", status);

	/*
	 * The sample rate requests crash the DJ2 ME's firmware (verified on
	 * hardware), and MIDI does not depend on the rate, so they are only
	 * sent when the audio input is used.
	 */
	if (dev->cfg.capture) {
		step(dev, "read sample rate");
		r = read_rate(h, 0, &rate);
		if (step_failed(dev, r, false) && r == LIBUSB_ERROR_NO_DEVICE)
			return r;
		if (r == 0)
			DBG(dev, "           rate %u Hz\n", rate);
		sleep_ms(14);

		/*
		 * The vendor drivers always write 86 05 86 05 86, ending on the capture
		 * EP. Without the capture interface only the output EP is programmed.
		 */
		step(dev, "set sample rate");
		burst[0] = burst[2] = burst[4] = idx_in;
		burst[1] = burst[3] = idx_out;
		for (i = 0; i < sizeof(burst) / sizeof(burst[0]); i++) {
			if (!dev->ep_pcm_in.addr && i > 0)
				break;
			r = write_rate(h, dev->ep_pcm_in.addr ? burst[i] : idx_out, dev->cfg.rate);
			if (step_failed(dev, r, false) && r == LIBUSB_ERROR_NO_DEVICE)
				return r;
		}

		step(dev, "verify sample rate");
		r = read_rate(h, dev->ep_pcm_in.addr ? idx_in : idx_out, &rate);
		if (step_failed(dev, r, false) && r == LIBUSB_ERROR_NO_DEVICE)
			return r;
		if (r == 0 && rate != dev->cfg.rate)
			LOG(dev, "Warning: requested %u Hz, device reports %u Hz\n", dev->cfg.rate, rate);
		sleep_ms(50);
	}

	step(dev, "read status");
	r = read_status(h, &status);
	if (step_failed(dev, r, true))
		return r;

	/* write back the status register with bit 5 set: starts streaming */
	step(dev, "start streaming");
	if (status & PT_STATUS_STREAMING) {
		DBG(dev, "           streaming bit already set\n");
		return 0;
	}
	r = libusb_control_transfer(h, PT_REQ_STATUS_WRITE_TYPE, PT_REQ_STATUS,
				    pt_status_confirm_wvalue(status), 0, NULL, 0, CTRL_TIMEOUT_MS);
	if (step_failed(dev, r, true))
		return r;
	return 0;
}

struct dj_device *dj_open(libusb_context *ctx, const struct dj_config *cfg,
			  struct midi_out_queue *out_queue,
			  dj_midi_in_fn midi_in, void *user)
{
	struct dj_device *dev;
	libusb_device *d;
	int i, r, conf = 0;

	d = find_device(ctx, cfg);
	if (!d)
		return NULL;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		libusb_unref_device(d);
		return NULL;
	}
	dev->cfg = *cfg;
	dev->ctx = ctx;
	dev->out_queue = out_queue;
	dev->midi_in = midi_in;
	dev->user = user;
	midi_parser_reset(&dev->in_parser);

	r = libusb_open(d, &dev->h);
	libusb_unref_device(d);
	if (r) {
		LOG(dev, "Cannot open device: %s\n", libusb_error_name(r));
		free(dev);
		return NULL;
	}

#ifdef __linux__
	/* on macOS this would try to re-enumerate ("capture") the device */
	libusb_set_auto_detach_kernel_driver(dev->h, 1);
#endif

	if (libusb_get_configuration(dev->h, &conf) == 0 && conf == 0) {
		r = libusb_set_configuration(dev->h, 1);
		if (r)
			LOG(dev, "Warning: set configuration: %s\n", libusb_error_name(r));
	}

	/* interface 1 only carries the audio input; activating it without
	 * reading the stream right away crashes the DJ2 ME's firmware */
	for (i = 0; i < (cfg->capture ? PT_NUM_INTERFACES : 1); i++) {
		r = libusb_claim_interface(dev->h, i);
		if (r) {
			LOG(dev, "Cannot claim interface %d: %s%s\n", i, libusb_error_name(r),
			    r == LIBUSB_ERROR_ACCESS ?
			    " (is another driver or program using the device?)" : "");
			if (i == 0)
				goto fail;
		} else {
			dev->claimed[i] = 1;
		}
	}

	r = handshake(dev);
	if (r)
		goto fail;
	step(dev, "submit stream transfers");
	r = start_streaming(dev);
	if (r)
		goto fail;

	LOG(dev, "Controller ready (%s)\n", dev->cfg.leds ?
	    "controls + LEDs" : "controls only, LEDs off");
	return dev;

fail:
	dj_close(dev);
	return NULL;
}

bool dj_alive(const struct dj_device *dev)
{
	return dev && !dev->failed;
}

void dj_close(struct dj_device *dev)
{
	struct timeval tv = { 0, 100000 };
	int i, tries;

	if (!dev)
		return;

	dev->stopping = 1;
	for (i = 0; i < dev->num_xfers; i++)
		libusb_cancel_transfer(dev->xfers[i].t);
	/* wait (bounded) for every callback to have run */
	for (tries = 0; dev->active > 0 && tries < 50; tries++)
		libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
	if (dev->active > 0)
		LOG(dev, "Warning: %d transfer(s) did not finish\n", dev->active);

	/* only free transfers whose callbacks are guaranteed to be done */
	if (dev->active == 0) {
		for (i = 0; i < dev->num_xfers; i++) {
			libusb_free_transfer(dev->xfers[i].t);
			free(dev->xfers[i].buf);
		}
	}

	if (dev->h) {
		for (i = 0; i < PT_NUM_INTERFACES; i++) {
			if (!dev->claimed[i])
				continue;
			if (!dev->failed)
				libusb_set_interface_alt_setting(dev->h, i, 0);
			libusb_release_interface(dev->h, i);
		}
		libusb_close(dev->h);
	}
	if (dev->active == 0)
		free(dev);
	/* else: leak deliberately rather than risk a use-after-free */
}
