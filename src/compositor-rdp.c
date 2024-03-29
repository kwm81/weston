/*
 * Copyright © 2013 Hardening <rdp.effort@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>

#if HAVE_FREERDP_VERSION_H
#include <freerdp/version.h>
#else
/* assume it's a early 1.1 version */
#define FREERDP_VERSION_MAJOR 1
#define FREERDP_VERSION_MINOR 1
#define FREERDP_VERSION_REVISION 0
#endif

#define FREERDP_VERSION_NUMBER ((FREERDP_VERSION_MAJOR * 0x10000) + \
		(FREERDP_VERSION_MINOR * 0x100) + FREERDP_VERSION_REVISION)

#if FREERDP_VERSION_NUMBER >= 0x10201
#define HAVE_SKIP_COMPRESSION
#endif

#if FREERDP_VERSION_NUMBER < 0x10202
#define FREERDP_CB_RET_TYPE void
#define FREERDP_CB_RETURN(V) return
#else
#define HAVE_NSC_RESET
#define FREERDP_CB_RET_TYPE BOOL
#define FREERDP_CB_RETURN(V) return TRUE
#endif

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/update.h>
#include <freerdp/input.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/locale/keyboard.h>
#include <winpr/input.h>

#include "shared/helpers.h"
#include "compositor.h"
#include "pixman-renderer.h"

#define MAX_FREERDP_FDS 32
#define DEFAULT_AXIS_STEP_DISTANCE wl_fixed_from_int(10)
#define RDP_MODE_FREQ 60 * 1000

struct rdp_backend_config {
	int width;
	int height;
	char *bind_address;
	int port;
	char *rdp_key;
	char *server_cert;
	char *server_key;
	int env_socket;
	int no_clients_resize;
};

struct rdp_output;

struct rdp_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	freerdp_listener *listener;
	struct wl_event_source *listener_events[MAX_FREERDP_FDS];
	struct rdp_output *output;

	char *server_cert;
	char *server_key;
	char *rdp_key;
	int tls_enabled;
	int no_clients_resize;
};

enum peer_item_flags {
	RDP_PEER_ACTIVATED      = (1 << 0),
	RDP_PEER_OUTPUT_ENABLED = (1 << 1),
};

struct rdp_peers_item {
	int flags;
	freerdp_peer *peer;
	struct weston_seat seat;

	struct wl_list link;
};

struct rdp_output {
	struct weston_output base;
	struct wl_event_source *finish_frame_timer;
	pixman_image_t *shadow_surface;

	struct wl_list peers;
};

struct rdp_peer_context {
	rdpContext _p;

	struct rdp_backend *rdpBackend;
	struct wl_event_source *events[MAX_FREERDP_FDS];
	RFX_CONTEXT *rfx_context;
	wStream *encode_stream;
	RFX_RECT *rfx_rects;
	NSC_CONTEXT *nsc_context;

	struct rdp_peers_item item;
};
typedef struct rdp_peer_context RdpPeerContext;

static void
rdp_backend_config_init(struct rdp_backend_config *config)
{
	config->width = 640;
	config->height = 480;
	config->bind_address = NULL;
	config->port = 3389;
	config->rdp_key = NULL;
	config->server_cert = NULL;
	config->server_key = NULL;
	config->env_socket = 0;
	config->no_clients_resize = 0;
}

static void
rdp_peer_refresh_rfx(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height, nrects, i;
	pixman_box32_t *region, *rects;
	uint32_t *ptr;
	RFX_RECT *rfxRect;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND *cmd = &update->surface_bits_command;
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	Stream_Clear(context->encode_stream);
	Stream_SetPosition(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

#ifdef HAVE_SKIP_COMPRESSION
	cmd->skipCompression = TRUE;
#else
	memset(cmd, 0, sizeof(*cmd));
#endif
	cmd->destLeft = damage->extents.x1;
	cmd->destTop = damage->extents.y1;
	cmd->destRight = damage->extents.x2;
	cmd->destBottom = damage->extents.y2;
	cmd->bpp = 32;
	cmd->codecID = peer->settings->RemoteFxCodecId;
	cmd->width = width;
	cmd->height = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * (pixman_image_get_stride(image) / sizeof(uint32_t));

	rects = pixman_region32_rectangles(damage, &nrects);
	context->rfx_rects = realloc(context->rfx_rects, nrects * sizeof *rfxRect);

	for (i = 0; i < nrects; i++) {
		region = &rects[i];
		rfxRect = &context->rfx_rects[i];

		rfxRect->x = (region->x1 - damage->extents.x1);
		rfxRect->y = (region->y1 - damage->extents.y1);
		rfxRect->width = (region->x2 - region->x1);
		rfxRect->height = (region->y2 - region->y1);
	}

	rfx_compose_message(context->rfx_context, context->encode_stream, context->rfx_rects, nrects,
			(BYTE *)ptr, width, height,
			pixman_image_get_stride(image)
	);

	cmd->bitmapDataLength = Stream_GetPosition(context->encode_stream);
	cmd->bitmapData = Stream_Buffer(context->encode_stream);

	update->SurfaceBits(update->context, cmd);
}


static void
rdp_peer_refresh_nsc(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height;
	uint32_t *ptr;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND *cmd = &update->surface_bits_command;
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	Stream_Clear(context->encode_stream);
	Stream_SetPosition(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

#ifdef HAVE_SKIP_COMPRESSION
	cmd->skipCompression = TRUE;
#else
	memset(cmd, 0, sizeof(*cmd));
#endif
	cmd->destLeft = damage->extents.x1;
	cmd->destTop = damage->extents.y1;
	cmd->destRight = damage->extents.x2;
	cmd->destBottom = damage->extents.y2;
	cmd->bpp = 32;
	cmd->codecID = peer->settings->NSCodecId;
	cmd->width = width;
	cmd->height = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * (pixman_image_get_stride(image) / sizeof(uint32_t));

	nsc_compose_message(context->nsc_context, context->encode_stream, (BYTE *)ptr,
			cmd->width,	cmd->height,
			pixman_image_get_stride(image));
	cmd->bitmapDataLength = Stream_GetPosition(context->encode_stream);
	cmd->bitmapData = Stream_Buffer(context->encode_stream);
	update->SurfaceBits(update->context, cmd);
}

static void
pixman_image_flipped_subrect(const pixman_box32_t *rect, pixman_image_t *img, BYTE *dest)
{
	int stride = pixman_image_get_stride(img);
	int h;
	int toCopy = (rect->x2 - rect->x1) * 4;
	int height = (rect->y2 - rect->y1);
	const BYTE *src = (const BYTE *)pixman_image_get_data(img);
	src += ((rect->y2-1) * stride) + (rect->x1 * 4);

	for (h = 0; h < height; h++, src -= stride, dest += toCopy)
		   memcpy(dest, src, toCopy);
}

static void
rdp_peer_refresh_raw(pixman_region32_t *region, pixman_image_t *image, freerdp_peer *peer)
{
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND *cmd = &update->surface_bits_command;
	SURFACE_FRAME_MARKER *marker = &update->surface_frame_marker;
	pixman_box32_t *rect, subrect;
	int nrects, i;
	int heightIncrement, remainingHeight, top;

	rect = pixman_region32_rectangles(region, &nrects);
	if (!nrects)
		return;

	marker->frameId++;
	marker->frameAction = SURFACECMD_FRAMEACTION_BEGIN;
	update->SurfaceFrameMarker(peer->context, marker);

	memset(cmd, 0, sizeof(*cmd));
	cmd->bpp = 32;
	cmd->codecID = 0;

	for (i = 0; i < nrects; i++, rect++) {
		/*weston_log("rect(%d,%d, %d,%d)\n", rect->x1, rect->y1, rect->x2, rect->y2);*/
		cmd->destLeft = rect->x1;
		cmd->destRight = rect->x2;
		cmd->width = rect->x2 - rect->x1;

		heightIncrement = peer->settings->MultifragMaxRequestSize / (16 + cmd->width * 4);
		remainingHeight = rect->y2 - rect->y1;
		top = rect->y1;

		subrect.x1 = rect->x1;
		subrect.x2 = rect->x2;

		while (remainingHeight) {
			   cmd->height = (remainingHeight > heightIncrement) ? heightIncrement : remainingHeight;
			   cmd->destTop = top;
			   cmd->destBottom = top + cmd->height;
			   cmd->bitmapDataLength = cmd->width * cmd->height * 4;
			   cmd->bitmapData = (BYTE *)realloc(cmd->bitmapData, cmd->bitmapDataLength);

			   subrect.y1 = top;
			   subrect.y2 = top + cmd->height;
			   pixman_image_flipped_subrect(&subrect, image, cmd->bitmapData);

			   /*weston_log("*  sending (%d,%d, %d,%d)\n", subrect.x1, subrect.y1, subrect.x2, subrect.y2); */
			   update->SurfaceBits(peer->context, cmd);

			   remainingHeight -= cmd->height;
			   top += cmd->height;
		}
	}

	marker->frameAction = SURFACECMD_FRAMEACTION_END;
	update->SurfaceFrameMarker(peer->context, marker);
}

static void
rdp_peer_refresh_region(pixman_region32_t *region, freerdp_peer *peer)
{
	RdpPeerContext *context = (RdpPeerContext *)peer->context;
	struct rdp_output *output = context->rdpBackend->output;
	rdpSettings *settings = peer->settings;

	if (settings->RemoteFxCodec)
		rdp_peer_refresh_rfx(region, output->shadow_surface, peer);
	else if (settings->NSCodec)
		rdp_peer_refresh_nsc(region, output->shadow_surface, peer);
	else
		rdp_peer_refresh_raw(region, output->shadow_surface, peer);
}

static void
rdp_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, PRESENTATION_FEEDBACK_INVALID);
}

static int
rdp_output_repaint(struct weston_output *output_base, pixman_region32_t *damage)
{
	struct rdp_output *output = container_of(output_base, struct rdp_output, base);
	struct weston_compositor *ec = output->base.compositor;
	struct rdp_peers_item *outputPeer;

	pixman_renderer_output_set_buffer(output_base, output->shadow_surface);
	ec->renderer->repaint_output(&output->base, damage);

	if (pixman_region32_not_empty(damage)) {
		wl_list_for_each(outputPeer, &output->peers, link) {
			if ((outputPeer->flags & RDP_PEER_ACTIVATED) &&
					(outputPeer->flags & RDP_PEER_OUTPUT_ENABLED))
			{
				rdp_peer_refresh_region(damage, outputPeer->peer);
			}
		}
	}

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	wl_event_source_timer_update(output->finish_frame_timer, 16);
	return 0;
}

static void
rdp_output_destroy(struct weston_output *output_base)
{
	struct rdp_output *output = (struct rdp_output *)output_base;

	wl_event_source_remove(output->finish_frame_timer);
	free(output);
}

static int
finish_frame_handler(void *data)
{
	struct rdp_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static struct weston_mode *
rdp_insert_new_mode(struct weston_output *output, int width, int height, int rate)
{
	struct weston_mode *ret;
	ret = zalloc(sizeof *ret);
	if (!ret)
		return NULL;
	ret->width = width;
	ret->height = height;
	ret->refresh = rate;
	wl_list_insert(&output->mode_list, &ret->link);
	return ret;
}

static struct weston_mode *
ensure_matching_mode(struct weston_output *output, struct weston_mode *target)
{
	struct weston_mode *local;

	wl_list_for_each(local, &output->mode_list, link) {
		if ((local->width == target->width) && (local->height == target->height))
			return local;
	}

	return rdp_insert_new_mode(output, target->width, target->height, RDP_MODE_FREQ);
}

static int
rdp_switch_mode(struct weston_output *output, struct weston_mode *target_mode)
{
	struct rdp_output *rdpOutput = container_of(output, struct rdp_output, base);
	struct rdp_peers_item *rdpPeer;
	rdpSettings *settings;
	pixman_image_t *new_shadow_buffer;
	struct weston_mode *local_mode;

	local_mode = ensure_matching_mode(output, target_mode);
	if (!local_mode) {
		weston_log("mode %dx%d not available\n", target_mode->width, target_mode->height);
		return -ENOENT;
	}

	if (local_mode == output->current_mode)
		return 0;

	output->current_mode->flags &= ~WL_OUTPUT_MODE_CURRENT;

	output->current_mode = local_mode;
	output->current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	pixman_renderer_output_destroy(output);
	pixman_renderer_output_create(output);

	new_shadow_buffer = pixman_image_create_bits(PIXMAN_x8r8g8b8, target_mode->width,
			target_mode->height, 0, target_mode->width * 4);
	pixman_image_composite32(PIXMAN_OP_SRC, rdpOutput->shadow_surface, 0, new_shadow_buffer,
			0, 0, 0, 0, 0, 0, target_mode->width, target_mode->height);
	pixman_image_unref(rdpOutput->shadow_surface);
	rdpOutput->shadow_surface = new_shadow_buffer;

	wl_list_for_each(rdpPeer, &rdpOutput->peers, link) {
		settings = rdpPeer->peer->settings;
		if (settings->DesktopWidth == (UINT32)target_mode->width &&
				settings->DesktopHeight == (UINT32)target_mode->height)
			continue;

		if (!settings->DesktopResize) {
			/* too bad this peer does not support desktop resize */
			rdpPeer->peer->Close(rdpPeer->peer);
		} else {
			settings->DesktopWidth = target_mode->width;
			settings->DesktopHeight = target_mode->height;
			rdpPeer->peer->update->DesktopResize(rdpPeer->peer->context);
		}
	}
	return 0;
}

static int
rdp_backend_create_output(struct rdp_backend *b, int width, int height)
{
	struct rdp_output *output;
	struct wl_event_loop *loop;
	struct weston_mode *currentMode;
	struct weston_mode initMode;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	wl_list_init(&output->peers);
	wl_list_init(&output->base.mode_list);

	initMode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	initMode.width = width;
	initMode.height = height;
	initMode.refresh = RDP_MODE_FREQ;

	currentMode = ensure_matching_mode(&output->base, &initMode);
	if (!currentMode)
		goto out_free_output;

	output->base.current_mode = output->base.native_mode = currentMode;
	weston_output_init(&output->base, b->compositor, 0, 0, width, height,
			   WL_OUTPUT_TRANSFORM_NORMAL, 1);

	output->base.make = "weston";
	output->base.model = "rdp";
	output->shadow_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
			width, height,
		    NULL,
		    width * 4);
	if (output->shadow_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		goto out_output;
	}

	if (pixman_renderer_output_create(&output->base) < 0)
		goto out_shadow_surface;

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer = wl_event_loop_add_timer(loop, finish_frame_handler, output);

	output->base.start_repaint_loop = rdp_output_start_repaint_loop;
	output->base.repaint = rdp_output_repaint;
	output->base.destroy = rdp_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = rdp_switch_mode;
	b->output = output;

	weston_compositor_add_output(b->compositor, &output->base);
	return 0;

out_shadow_surface:
	pixman_image_unref(output->shadow_surface);
out_output:
	weston_output_destroy(&output->base);
out_free_output:
	free(output);
	return -1;
}

static void
rdp_restore(struct weston_compositor *ec)
{
}

static void
rdp_destroy(struct weston_compositor *ec)
{
	struct rdp_backend *b = (struct rdp_backend *) ec->backend;
	int i;

	weston_compositor_shutdown(ec);
	for (i = 0; i < MAX_FREERDP_FDS; i++)
		if (b->listener_events[i])
			wl_event_source_remove(b->listener_events[i]);

	freerdp_listener_free(b->listener);

	free(b->server_cert);
	free(b->server_key);
	free(b->rdp_key);
	free(b);
}

static
int rdp_listener_activity(int fd, uint32_t mask, void *data)
{
	freerdp_listener* instance = (freerdp_listener *)data;

	if (!(mask & WL_EVENT_READABLE))
		return 0;
	if (!instance->CheckFileDescriptor(instance)) {
		weston_log("failed to check FreeRDP file descriptor\n");
		return -1;
	}
	return 0;
}

static
int rdp_implant_listener(struct rdp_backend *b, freerdp_listener* instance)
{
	int i, fd;
	int rcount = 0;
	void* rfds[MAX_FREERDP_FDS];
	struct wl_event_loop *loop;

	if (!instance->GetFileDescriptor(instance, rfds, &rcount)) {
		weston_log("Failed to get FreeRDP file descriptor\n");
		return -1;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);
		b->listener_events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_listener_activity, instance);
	}

	for ( ; i < MAX_FREERDP_FDS; i++)
		b->listener_events[i] = 0;
	return 0;
}


static void
rdp_peer_context_new(freerdp_peer* client, RdpPeerContext* context)
{
	context->item.peer = client;
	context->item.flags = RDP_PEER_OUTPUT_ENABLED;

#if FREERDP_VERSION_MAJOR == 1 && FREERDP_VERSION_MINOR == 1
	context->rfx_context = rfx_context_new();
#else
	context->rfx_context = rfx_context_new(TRUE);
#endif
	context->rfx_context->mode = RLGR3;
	context->rfx_context->width = client->settings->DesktopWidth;
	context->rfx_context->height = client->settings->DesktopHeight;
	rfx_context_set_pixel_format(context->rfx_context, RDP_PIXEL_FORMAT_B8G8R8A8);

	context->nsc_context = nsc_context_new();
	nsc_context_set_pixel_format(context->nsc_context, RDP_PIXEL_FORMAT_B8G8R8A8);

	context->encode_stream = Stream_New(NULL, 65536);
}

static void
rdp_peer_context_free(freerdp_peer* client, RdpPeerContext* context)
{
	int i;
	if (!context)
		return;

	wl_list_remove(&context->item.link);
	for (i = 0; i < MAX_FREERDP_FDS; i++) {
		if (context->events[i])
			wl_event_source_remove(context->events[i]);
	}

	if (context->item.flags & RDP_PEER_ACTIVATED) {
		weston_seat_release_keyboard(&context->item.seat);
		weston_seat_release_pointer(&context->item.seat);
		weston_seat_release(&context->item.seat);
	}

	Stream_Free(context->encode_stream, TRUE);
	nsc_context_free(context->nsc_context);
	rfx_context_free(context->rfx_context);
	free(context->rfx_rects);
}


static int
rdp_client_activity(int fd, uint32_t mask, void *data)
{
	freerdp_peer* client = (freerdp_peer *)data;

	if (!client->CheckFileDescriptor(client)) {
		weston_log("unable to checkDescriptor for %p\n", client);
		goto out_clean;
	}
	return 0;

out_clean:
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);
	return 0;
}

static BOOL
xf_peer_capabilities(freerdp_peer* client)
{
	return TRUE;
}

struct rdp_to_xkb_keyboard_layout {
	UINT32 rdpLayoutCode;
	const char *xkbLayout;
	const char *xkbVariant;
};

/* table reversed from
	https://github.com/awakecoding/FreeRDP/blob/master/libfreerdp/locale/xkb_layout_ids.c#L811 */
static
struct rdp_to_xkb_keyboard_layout rdp_keyboards[] = {
	{KBD_ARABIC_101, "ara", 0},
	{KBD_BULGARIAN, 0, 0},
	{KBD_CHINESE_TRADITIONAL_US, 0},
	{KBD_CZECH, "cz", 0},
	{KBD_CZECH_PROGRAMMERS, "cz", "bksl"},
	{KBD_CZECH_QWERTY, "cz", "qwerty"},
	{KBD_DANISH, "dk", 0},
	{KBD_GERMAN, "de", 0},
	{KBD_GERMAN_NEO, "de", "neo"},
	{KBD_GERMAN_IBM, "de", "qwerty"},
	{KBD_GREEK, "gr", 0},
	{KBD_GREEK_220, "gr", "simple"},
	{KBD_GREEK_319, "gr", "extended"},
	{KBD_GREEK_POLYTONIC, "gr", "polytonic"},
	{KBD_US, "us", 0},
	{KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L, "ara", "buckwalter"},
	{KBD_SPANISH, "es", 0},
	{KBD_SPANISH_VARIATION, "es", "nodeadkeys"},
	{KBD_FINNISH, "fi", 0},
	{KBD_FRENCH, "fr", 0},
	{KBD_HEBREW, "il", 0},
	{KBD_HUNGARIAN, "hu", 0},
	{KBD_HUNGARIAN_101_KEY, "hu", "standard"},
	{KBD_ICELANDIC, "is", 0},
	{KBD_ITALIAN, "it", 0},
	{KBD_ITALIAN_142, "it", "nodeadkeys"},
	{KBD_JAPANESE, "jp", 0},
	{KBD_JAPANESE_INPUT_SYSTEM_MS_IME2002, "jp", "kana"},
	{KBD_KOREAN, "kr", 0},
	{KBD_KOREAN_INPUT_SYSTEM_IME_2000, "kr", "kr104"},
	{KBD_DUTCH, "nl", 0},
	{KBD_NORWEGIAN, "no", 0},
	{KBD_POLISH_PROGRAMMERS, "pl", 0},
	{KBD_POLISH_214, "pl", "qwertz"},
	// {KBD_PORTUGUESE_BRAZILIAN_ABN0416, 0},
	{KBD_ROMANIAN, "ro", 0},
	{KBD_RUSSIAN, "ru", 0},
	{KBD_RUSSIAN_TYPEWRITER, "ru", "typewriter"},
	{KBD_CROATIAN, "hr", 0},
	{KBD_SLOVAK, "sk", 0},
	{KBD_SLOVAK_QWERTY, "sk", "qwerty"},
	{KBD_ALBANIAN, 0, 0},
	{KBD_SWEDISH, "se", 0},
	{KBD_THAI_KEDMANEE, "th", 0},
	{KBD_THAI_KEDMANEE_NON_SHIFTLOCK, "th", "tis"},
	{KBD_TURKISH_Q, "tr", 0},
	{KBD_TURKISH_F, "tr", "f"},
	{KBD_URDU, "in", "urd-phonetic3"},
	{KBD_UKRAINIAN, "ua", 0},
	{KBD_BELARUSIAN, "by", 0},
	{KBD_SLOVENIAN, "si", 0},
	{KBD_ESTONIAN, "ee", 0},
	{KBD_LATVIAN, "lv", 0},
	{KBD_LITHUANIAN_IBM, "lt", "ibm"},
	{KBD_FARSI, "af", 0},
	{KBD_VIETNAMESE, "vn", 0},
	{KBD_ARMENIAN_EASTERN, "am", 0},
	{KBD_AZERI_LATIN, 0, 0},
	{KBD_FYRO_MACEDONIAN, "mk", 0},
	{KBD_GEORGIAN, "ge", 0},
	{KBD_FAEROESE, 0, 0},
	{KBD_DEVANAGARI_INSCRIPT, 0, 0},
	{KBD_MALTESE_47_KEY, 0, 0},
	{KBD_NORWEGIAN_WITH_SAMI, "no", "smi"},
	{KBD_KAZAKH, "kz", 0},
	{KBD_KYRGYZ_CYRILLIC, "kg", "phonetic"},
	{KBD_TATAR, "ru", "tt"},
	{KBD_BENGALI, "bd", 0},
	{KBD_BENGALI_INSCRIPT, "bd", "probhat"},
	{KBD_PUNJABI, 0, 0},
	{KBD_GUJARATI, "in", "guj"},
	{KBD_TAMIL, "in", "tam"},
	{KBD_TELUGU, "in", "tel"},
	{KBD_KANNADA, "in", "kan"},
	{KBD_MALAYALAM, "in", "mal"},
	{KBD_HINDI_TRADITIONAL, "in", 0},
	{KBD_MARATHI, 0, 0},
	{KBD_MONGOLIAN_CYRILLIC, "mn", 0},
	{KBD_UNITED_KINGDOM_EXTENDED, "gb", "intl"},
	{KBD_SYRIAC, "syc", 0},
	{KBD_SYRIAC_PHONETIC, "syc", "syc_phonetic"},
	{KBD_NEPALI, "np", 0},
	{KBD_PASHTO, "af", "ps"},
	{KBD_DIVEHI_PHONETIC, 0, 0},
	{KBD_LUXEMBOURGISH, 0, 0},
	{KBD_MAORI, "mao", 0},
	{KBD_CHINESE_SIMPLIFIED_US, 0, 0},
	{KBD_SWISS_GERMAN, "ch", "de_nodeadkeys"},
	{KBD_UNITED_KINGDOM, "gb", 0},
	{KBD_LATIN_AMERICAN, "latam", 0},
	{KBD_BELGIAN_FRENCH, "be", 0},
	{KBD_BELGIAN_PERIOD, "be", "oss_sundeadkeys"},
	{KBD_PORTUGUESE, "pt", 0},
	{KBD_SERBIAN_LATIN, "rs", 0},
	{KBD_AZERI_CYRILLIC, "az", "cyrillic"},
	{KBD_SWEDISH_WITH_SAMI, "se", "smi"},
	{KBD_UZBEK_CYRILLIC, "af", "uz"},
	{KBD_INUKTITUT_LATIN, "ca", "ike"},
	{KBD_CANADIAN_FRENCH_LEGACY, "ca", "fr-legacy"},
	{KBD_SERBIAN_CYRILLIC, "rs", 0},
	{KBD_CANADIAN_FRENCH, "ca", "fr-legacy"},
	{KBD_SWISS_FRENCH, "ch", "fr"},
	{KBD_BOSNIAN, "ba", 0},
	{KBD_IRISH, 0, 0},
	{KBD_BOSNIAN_CYRILLIC, "ba", "us"},
	{KBD_UNITED_STATES_DVORAK, "us", "dvorak"},
	{KBD_PORTUGUESE_BRAZILIAN_ABNT2, "br", "nativo"},
	{KBD_CANADIAN_MULTILINGUAL_STANDARD, "ca", "multix"},
	{KBD_GAELIC, "ie", "CloGaelach"},

	{0x00000000, 0, 0},
};

/* taken from 2.2.7.1.6 Input Capability Set (TS_INPUT_CAPABILITYSET) */
static char *rdp_keyboard_types[] = {
	"",	/* 0: unused */
	"", /* 1: IBM PC/XT or compatible (83-key) keyboard */
	"", /* 2: Olivetti "ICO" (102-key) keyboard */
	"", /* 3: IBM PC/AT (84-key) or similar keyboard */
	"pc102",/* 4: IBM enhanced (101- or 102-key) keyboard */
	"", /* 5: Nokia 1050 and similar keyboards */
	"",	/* 6: Nokia 9140 and similar keyboards */
	""	/* 7: Japanese keyboard */
};

static BOOL
xf_peer_activate(freerdp_peer* client)
{
	RdpPeerContext *peerCtx;
	struct rdp_backend *b;
	struct rdp_output *output;
	rdpSettings *settings;
	rdpPointerUpdate *pointer;
	struct rdp_peers_item *peersItem;
	struct xkb_context *xkbContext;
	struct xkb_rule_names xkbRuleNames;
	struct xkb_keymap *keymap;
	int i;
	pixman_box32_t box;
	pixman_region32_t damage;
	char seat_name[50];


	peerCtx = (RdpPeerContext *)client->context;
	b = peerCtx->rdpBackend;
	peersItem = &peerCtx->item;
	output = b->output;
	settings = client->settings;

	if (!settings->SurfaceCommandsEnabled) {
		weston_log("client doesn't support required SurfaceCommands\n");
		return FALSE;
	}

	if (output->base.width != (int)settings->DesktopWidth ||
			output->base.height != (int)settings->DesktopHeight)
	{
		if (b->no_clients_resize) {
			/* RDP peers don't dictate their resolution to weston */
			if (!settings->DesktopResize) {
				/* peer does not support desktop resize */
				weston_log("%s: client doesn't support resizing, closing connection\n", __FUNCTION__);
				return FALSE;
			} else {
				settings->DesktopWidth = output->base.width;
				settings->DesktopHeight = output->base.height;
				client->update->DesktopResize(client->context);
			}
		} else {
			/* ask weston to adjust size */
			struct weston_mode new_mode;
			struct weston_mode *target_mode;
			new_mode.width = (int)settings->DesktopWidth;
			new_mode.height = (int)settings->DesktopHeight;
			target_mode = ensure_matching_mode(&output->base, &new_mode);
			if (!target_mode) {
				weston_log("client mode not found\n");
				return FALSE;
			}
			weston_output_mode_set_native(&output->base, target_mode, 1);
			output->base.width = new_mode.width;
			output->base.height = new_mode.height;
		}
	}

	rfx_context_reset(peerCtx->rfx_context);
#ifdef HAVE_NSC_RESET
	nsc_context_reset(peerCtx->nsc_context);
#endif

	if (peersItem->flags & RDP_PEER_ACTIVATED)
		return TRUE;

	/* when here it's the first reactivation, we need to setup a little more */
	weston_log("kbd_layout:0x%x kbd_type:0x%x kbd_subType:0x%x kbd_functionKeys:0x%x\n",
			settings->KeyboardLayout, settings->KeyboardType, settings->KeyboardSubType,
			settings->KeyboardFunctionKey);

	memset(&xkbRuleNames, 0, sizeof(xkbRuleNames));
	if (settings->KeyboardType <= 7)
		xkbRuleNames.model = rdp_keyboard_types[settings->KeyboardType];
	for (i = 0; rdp_keyboards[i].rdpLayoutCode; i++) {
		if (rdp_keyboards[i].rdpLayoutCode == settings->KeyboardLayout) {
			xkbRuleNames.layout = rdp_keyboards[i].xkbLayout;
			xkbRuleNames.variant = rdp_keyboards[i].xkbVariant;
			weston_log("%s: matching layout=%s variant=%s\n", __FUNCTION__,
					xkbRuleNames.layout, xkbRuleNames.variant);
			break;
		}
	}

	keymap = NULL;
	if (xkbRuleNames.layout) {
		xkbContext = xkb_context_new(0);
		if (!xkbContext) {
			weston_log("unable to create a xkb_context\n");
			return FALSE;
		}

		keymap = xkb_keymap_new_from_names(xkbContext, &xkbRuleNames, 0);
	}

	if (settings->ClientHostname)
		snprintf(seat_name, sizeof(seat_name), "RDP %s", settings->ClientHostname);
	else
		snprintf(seat_name, sizeof(seat_name), "RDP peer @%s", settings->ClientAddress);

	weston_seat_init(&peersItem->seat, b->compositor, seat_name);
	weston_seat_init_keyboard(&peersItem->seat, keymap);
	weston_seat_init_pointer(&peersItem->seat);

	peersItem->flags |= RDP_PEER_ACTIVATED;

	/* disable pointer on the client side */
	pointer = client->update->pointer;
	pointer->pointer_system.type = SYSPTR_NULL;
	pointer->PointerSystem(client->context, &pointer->pointer_system);

	/* sends a full refresh */
	box.x1 = 0;
	box.y1 = 0;
	box.x2 = output->base.width;
	box.y2 = output->base.height;
	pixman_region32_init_with_extents(&damage, &box);

	rdp_peer_refresh_region(&damage, client);

	pixman_region32_fini(&damage);

	return TRUE;
}

static BOOL xf_peer_post_connect(freerdp_peer *client)
{
	return TRUE;
}

static FREERDP_CB_RET_TYPE
xf_mouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
	wl_fixed_t wl_x, wl_y, axis;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_output *output;
	uint32_t button = 0;

	if (flags & PTR_FLAGS_MOVE) {
		output = peerContext->rdpBackend->output;
		if (x < output->base.width && y < output->base.height) {
			wl_x = wl_fixed_from_int((int)x);
			wl_y = wl_fixed_from_int((int)y);
			notify_motion_absolute(&peerContext->item.seat, weston_compositor_get_time(),
					wl_x, wl_y);
		}
	}

	if (flags & PTR_FLAGS_BUTTON1)
		button = BTN_LEFT;
	else if (flags & PTR_FLAGS_BUTTON2)
		button = BTN_RIGHT;
	else if (flags & PTR_FLAGS_BUTTON3)
		button = BTN_MIDDLE;

	if (button) {
		notify_button(&peerContext->item.seat, weston_compositor_get_time(), button,
			(flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
		);
	}

	if (flags & PTR_FLAGS_WHEEL) {
		/* DEFAULT_AXIS_STEP_DISTANCE is stolen from compositor-x11.c
		 * The RDP specs says the lower bits of flags contains the "the number of rotation
		 * units the mouse wheel was rotated".
		 *
		 * http://blogs.msdn.com/b/oldnewthing/archive/2013/01/23/10387366.aspx explains the 120 value
		 */
		axis = (DEFAULT_AXIS_STEP_DISTANCE * (flags & 0xff)) / 120;
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			axis = -axis;

		notify_axis(&peerContext->item.seat, weston_compositor_get_time(),
					    WL_POINTER_AXIS_VERTICAL_SCROLL,
					    axis);
	}

	FREERDP_CB_RETURN(TRUE);
}

static FREERDP_CB_RET_TYPE
xf_extendedMouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
	wl_fixed_t wl_x, wl_y;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_output *output;

	output = peerContext->rdpBackend->output;
	if (x < output->base.width && y < output->base.height) {
		wl_x = wl_fixed_from_int((int)x);
		wl_y = wl_fixed_from_int((int)y);
		notify_motion_absolute(&peerContext->item.seat, weston_compositor_get_time(),
				wl_x, wl_y);
	}

	FREERDP_CB_RETURN(TRUE);
}


static FREERDP_CB_RET_TYPE
xf_input_synchronize_event(rdpInput *input, UINT32 flags)
{
	freerdp_peer *client = input->context->peer;
	RdpPeerContext *peerCtx = (RdpPeerContext *)input->context;
	struct rdp_output *output = peerCtx->rdpBackend->output;
	pixman_box32_t box;
	pixman_region32_t damage;

	/* sends a full refresh */
	box.x1 = 0;
	box.y1 = 0;
	box.x2 = output->base.width;
	box.y2 = output->base.height;
	pixman_region32_init_with_extents(&damage, &box);

	rdp_peer_refresh_region(&damage, client);

	pixman_region32_fini(&damage);
	FREERDP_CB_RETURN(TRUE);
}


static FREERDP_CB_RET_TYPE
xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code)
{
	uint32_t scan_code, vk_code, full_code;
	enum wl_keyboard_key_state keyState;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	int notify = 0;

	if (!(peerContext->item.flags & RDP_PEER_ACTIVATED))
		FREERDP_CB_RETURN(TRUE);

	if (flags & KBD_FLAGS_DOWN) {
		keyState = WL_KEYBOARD_KEY_STATE_PRESSED;
		notify = 1;
	} else if (flags & KBD_FLAGS_RELEASE) {
		keyState = WL_KEYBOARD_KEY_STATE_RELEASED;
		notify = 1;
	}

	if (notify) {
		full_code = code;
		if (flags & KBD_FLAGS_EXTENDED)
			full_code |= KBD_FLAGS_EXTENDED;

		vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
		if (flags & KBD_FLAGS_EXTENDED)
			vk_code |= KBDEXT;

		scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);

		/*weston_log("code=%x ext=%d vk_code=%x scan_code=%x\n", code, (flags & KBD_FLAGS_EXTENDED) ? 1 : 0,
				vk_code, scan_code);*/
		notify_key(&peerContext->item.seat, weston_compositor_get_time(),
					scan_code - 8, keyState, STATE_UPDATE_AUTOMATIC);
	}

	FREERDP_CB_RETURN(TRUE);
}

static FREERDP_CB_RET_TYPE
xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code)
{
	weston_log("Client sent a unicode keyboard event (flags:0x%X code:0x%X)\n", flags, code);
	FREERDP_CB_RETURN(TRUE);
}


static FREERDP_CB_RET_TYPE
xf_suppress_output(rdpContext *context, BYTE allow, RECTANGLE_16 *area)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)context;

	if (allow)
		peerContext->item.flags |= RDP_PEER_OUTPUT_ENABLED;
	else
		peerContext->item.flags &= (~RDP_PEER_OUTPUT_ENABLED);

	FREERDP_CB_RETURN(TRUE);
}

static int
rdp_peer_init(freerdp_peer *client, struct rdp_backend *b)
{
	int rcount = 0;
	void *rfds[MAX_FREERDP_FDS];
	int i, fd;
	struct wl_event_loop *loop;
	rdpSettings	*settings;
	rdpInput *input;
	RdpPeerContext *peerCtx;

	client->ContextSize = sizeof(RdpPeerContext);
	client->ContextNew = (psPeerContextNew)rdp_peer_context_new;
	client->ContextFree = (psPeerContextFree)rdp_peer_context_free;
	freerdp_peer_context_new(client);

	peerCtx = (RdpPeerContext *) client->context;
	peerCtx->rdpBackend = b;

	settings = client->settings;
	/* configure security settings */
	if (b->rdp_key)
		settings->RdpKeyFile = strdup(b->rdp_key);
	if (b->tls_enabled) {
		settings->CertificateFile = strdup(b->server_cert);
		settings->PrivateKeyFile = strdup(b->server_key);
	} else {
		settings->TlsSecurity = FALSE;
	}
	settings->NlaSecurity = FALSE;

	if (!client->Initialize(client)) {
		weston_log("peer initialization failed\n");
		goto error_initialize;
	}

	settings->OsMajorType = OSMAJORTYPE_UNIX;
	settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
	settings->ColorDepth = 32;
	settings->RefreshRect = TRUE;
	settings->RemoteFxCodec = TRUE;
	settings->NSCodec = TRUE;
	settings->FrameMarkerCommandEnabled = TRUE;
	settings->SurfaceFrameMarkerEnabled = TRUE;

	client->Capabilities = xf_peer_capabilities;
	client->PostConnect = xf_peer_post_connect;
	client->Activate = xf_peer_activate;

	client->update->SuppressOutput = xf_suppress_output;

	input = client->input;
	input->SynchronizeEvent = xf_input_synchronize_event;
	input->MouseEvent = xf_mouseEvent;
	input->ExtendedMouseEvent = xf_extendedMouseEvent;
	input->KeyboardEvent = xf_input_keyboard_event;
	input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

	if (!client->GetFileDescriptor(client, rfds, &rcount)) {
		weston_log("unable to retrieve client fds\n");
		goto error_initialize;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);

		peerCtx->events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_client_activity, client);
	}
	for ( ; i < MAX_FREERDP_FDS; i++)
		peerCtx->events[i] = 0;

	wl_list_insert(&b->output->peers, &peerCtx->item.link);
	return 0;

error_initialize:
	client->Close(client);
	return -1;
}


static FREERDP_CB_RET_TYPE
rdp_incoming_peer(freerdp_listener *instance, freerdp_peer *client)
{
	struct rdp_backend *b = (struct rdp_backend *)instance->param4;
	if (rdp_peer_init(client, b) < 0) {
		weston_log("error when treating incoming peer\n");
		FREERDP_CB_RETURN(FALSE);
	}

	FREERDP_CB_RETURN(TRUE);
}

static struct rdp_backend *
rdp_backend_create(struct weston_compositor *compositor,
		   struct rdp_backend_config *config,
		   int *argc, char *argv[], struct weston_config *wconfig)
{
	struct rdp_backend *b;
	char *fd_str;
	int fd;

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;
	b->base.destroy = rdp_destroy;
	b->base.restore = rdp_restore;
	b->rdp_key = config->rdp_key ? strdup(config->rdp_key) : NULL;
	b->no_clients_resize = config->no_clients_resize;

	/* activate TLS only if certificate/key are available */
	if (config->server_cert && config->server_key) {
		weston_log("TLS support activated\n");
		b->server_cert = strdup(config->server_cert);
		b->server_key = strdup(config->server_key);
		if (!b->server_cert || !b->server_key)
			goto err_free_strings;
		b->tls_enabled = 1;
	}

	if (weston_compositor_set_presentation_clock_software(compositor) < 0)
		goto err_compositor;

	if (pixman_renderer_init(compositor) < 0)
		goto err_compositor;

	if (rdp_backend_create_output(b, config->width, config->height) < 0)
		goto err_compositor;

	compositor->capabilities |= WESTON_CAP_ARBITRARY_MODES;

	if (!config->env_socket) {
		b->listener = freerdp_listener_new();
		b->listener->PeerAccepted = rdp_incoming_peer;
		b->listener->param4 = b;
		if (!b->listener->Open(b->listener, config->bind_address, config->port)) {
			weston_log("unable to bind rdp socket\n");
			goto err_listener;
		}

		if (rdp_implant_listener(b, b->listener) < 0)
			goto err_compositor;
	} else {
		/* get the socket from RDP_FD var */
		fd_str = getenv("RDP_FD");
		if (!fd_str) {
			weston_log("RDP_FD env variable not set\n");
			goto err_output;
		}

		fd = strtoul(fd_str, NULL, 10);
		if (rdp_peer_init(freerdp_peer_new(fd), b))
			goto err_output;
	}

	compositor->backend = &b->base;
	return b;

err_listener:
	freerdp_listener_free(b->listener);
err_output:
	weston_output_destroy(&b->output->base);
err_compositor:
	weston_compositor_shutdown(compositor);
err_free_strings:
	free(b->rdp_key);
	free(b->server_cert);
	free(b->server_key);
	free(b);
	return NULL;
}

WL_EXPORT int
backend_init(struct weston_compositor *compositor, int *argc, char *argv[],
	     struct weston_config *wconfig,
	     struct weston_backend_config *config_base)
{
	struct rdp_backend *b;
	struct rdp_backend_config config;
	rdp_backend_config_init(&config);
	int major, minor, revision;

	freerdp_get_version(&major, &minor, &revision);
	weston_log("using FreeRDP version %d.%d.%d\n", major, minor, revision);

	const struct weston_option rdp_options[] = {
		{ WESTON_OPTION_BOOLEAN, "env-socket", 0, &config.env_socket },
		{ WESTON_OPTION_INTEGER, "width", 0, &config.width },
		{ WESTON_OPTION_INTEGER, "height", 0, &config.height },
		{ WESTON_OPTION_STRING,  "address", 0, &config.bind_address },
		{ WESTON_OPTION_INTEGER, "port", 0, &config.port },
		{ WESTON_OPTION_BOOLEAN, "no-clients-resize", 0, &config.no_clients_resize },
		{ WESTON_OPTION_STRING,  "rdp4-key", 0, &config.rdp_key },
		{ WESTON_OPTION_STRING,  "rdp-tls-cert", 0, &config.server_cert },
		{ WESTON_OPTION_STRING,  "rdp-tls-key", 0, &config.server_key }
	};

	parse_options(rdp_options, ARRAY_LENGTH(rdp_options), argc, argv);
	if (!config.rdp_key && (!config.server_cert || !config.server_key)) {
		weston_log("the RDP compositor requires keys and an optional certificate for RDP or TLS security ("
				"--rdp4-key or --rdp-tls-cert/--rdp-tls-key)\n");
		return -1;
	}

	b = rdp_backend_create(compositor, &config, argc, argv, wconfig);
	if (b == NULL)
		return -1;
	return 0;
}
