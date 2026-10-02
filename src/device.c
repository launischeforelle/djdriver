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
	int consec_errors;
};

static void sleep_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

void dj_config_defaults(struct dj_config *cfg)
{
	cfg->vid = DJ2ME_VENDOR_ID;
	cfg->pid = DJ2ME_PRODUCT_ID;
	cfg->rate = 44100;
	cfg->ep_out = -1;
	cfg->ep_midi_in = -1;
	cfg->ep_pcm_in = -1;
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
	uint32_t rate;
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
	r = read_rate(h, 0, &rate);
	if (r)
		printf("  Rate:      read failed (%s)\n", libusb_error_name(r));
	else
		printf("  Rate:      %u Hz\n", rate);
	libusb_close(h);
	return 0;
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
			LOG(dev, "Device disconnected\n");
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

	for (i = 0; i < NUM_OUT_XFERS && !r; i++)
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

static int handshake(struct dj_device *dev)
{
	libusb_device_handle *h = dev->h;
	uint16_t idx_in = dev->ep_pcm_in.addr ? dev->ep_pcm_in.addr : PT_EP_PCM_IN;
	uint16_t idx_out = dev->ep_out.addr;
	const uint16_t burst[] = { idx_in, idx_out, idx_in, idx_out, idx_in };
	uint8_t fw[15], status;
	uint32_t rate = 0;
	unsigned int i;
	int r;

	/* 1. firmware version: the first request the vendor drivers send */
	r = read_firmware(h, fw, sizeof(fw));
	if (r < 0)
		LOG(dev, "Warning: reading firmware version failed: %s\n", libusb_error_name(r));
	else if (r >= 3)
		LOG(dev, "Firmware: %02X %02X %02X\n", fw[0], fw[1], fw[2]);

	/* 2. activate the streaming interfaces */
	for (i = 0; i < PT_NUM_INTERFACES; i++) {
		if (!dev->claimed[i])
			continue;
		r = libusb_set_interface_alt_setting(h, (int)i, PT_ALT_SETTING);
		if (r)
			LOG(dev, "Warning: interface %u alt %d: %s\n", i, PT_ALT_SETTING,
			    libusb_error_name(r));
	}

	r = discover_endpoints(dev);
	if (r)
		return r;
	libusb_clear_halt(h, dev->ep_out.addr);
	if (dev->ep_midi_in.addr)
		libusb_clear_halt(h, dev->ep_midi_in.addr);
	if (dev->ep_pcm_in.addr)
		libusb_clear_halt(h, dev->ep_pcm_in.addr);

	/* 3. sample rate: the vendor drivers program it in a burst */
	if (read_rate(h, 0, &rate) == 0)
		DBG(dev, "Current rate: %u Hz\n", rate);
	sleep_ms(14);
	for (i = 0; i < sizeof(burst) / sizeof(burst[0]); i++) {
		r = write_rate(h, burst[i], dev->cfg.rate);
		if (r)
			LOG(dev, "Warning: setting rate on EP 0x%02x failed: %s\n",
			    burst[i], libusb_error_name(r));
	}
	if (read_rate(h, idx_in, &rate) == 0 && rate != dev->cfg.rate)
		LOG(dev, "Warning: requested %u Hz, device reports %u Hz\n", dev->cfg.rate, rate);
	sleep_ms(50);

	/* 4. start streaming: write back the status register with bit 5 set */
	r = read_status(h, &status);
	if (r) {
		LOG(dev, "Error: reading status failed: %s\n", libusb_error_name(r));
		return r;
	}
	DBG(dev, "Status: 0x%02X\n", status);
	r = libusb_control_transfer(h, PT_REQ_STATUS_WRITE_TYPE, PT_REQ_STATUS,
				    pt_status_confirm_wvalue(status), 0, NULL, 0, CTRL_TIMEOUT_MS);
	if (r < 0) {
		LOG(dev, "Error: start streaming failed: %s\n", libusb_error_name(r));
		return r;
	}
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

	/* Linux only; harmless "not supported" elsewhere */
	libusb_set_auto_detach_kernel_driver(dev->h, 1);

	if (libusb_get_configuration(dev->h, &conf) == 0 && conf == 0) {
		r = libusb_set_configuration(dev->h, 1);
		if (r)
			LOG(dev, "Warning: set configuration: %s\n", libusb_error_name(r));
	}

	for (i = 0; i < PT_NUM_INTERFACES; i++) {
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
	r = start_streaming(dev);
	if (r)
		goto fail;

	LOG(dev, "Controller ready (%s mode, %u Hz)\n",
	    dev->mode == PT_MODE_BULK ? "bulk" : "interrupt", dev->cfg.rate);
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
