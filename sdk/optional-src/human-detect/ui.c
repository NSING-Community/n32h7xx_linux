/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Local LVGL monitor for human-detect.
 *
 * The detector already owns /dev/video0. This client receives its untouched
 * 320x240 L8 frames over the local Unix socket, avoiding JPEG encode/decode in
 * the display path. Detection boxes and status are rendered by LVGL.
 */

#include "preview-gray8.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "lvgl-gpu-backend.h"

#define FRAME_WIDTH HUMAN_DETECT_PREVIEW_WIDTH
#define FRAME_HEIGHT HUMAN_DETECT_PREVIEW_HEIGHT
#define HTTP_BUFFER_SIZE 512
#define CONNECT_RETRY_MS 2000

enum stream_stage {
	STREAM_DISCONNECTED,
	STREAM_CONNECTING,
	STREAM_REQUEST,
	STREAM_HTTP,
	STREAM_HEADER,
	STREAM_BODY,
};

static volatile sig_atomic_t running = 1;
static _Alignas(16) uint8_t gray_frame[FRAME_WIDTH * FRAME_HEIGHT];
static lvgl_gpu_backend_t *gpu_backend;
static lv_display_t *display;

static lv_obj_t *canvas;
static lv_obj_t *no_signal;
static lv_obj_t *pause_badge;
static lv_obj_t *connection_label;
static lv_obj_t *person_label;
static lv_obj_t *fps_label;
static lv_obj_t *timing_label;
static lv_obj_t *boxes_label;
static lv_obj_t *frame_label;
static lv_obj_t *errors_label;
static lv_obj_t *pause_button_label;
static lv_obj_t *boxes_button_label;
static lv_obj_t *detection_boxes[HUMAN_DETECT_PREVIEW_BOXES];

static const char *socket_path = "/run/human-detect.sock";
static int preview_fps = 3;
static bool paused;
static bool boxes_visible = true;
static bool have_frame;

static int stream_fd = -1;
static enum stream_stage stream_stage = STREAM_DISCONNECTED;
static uint64_t retry_at_ms;
static char http_request[192];
static size_t http_request_length;
static size_t http_request_offset;
static char http_response[HTTP_BUFFER_SIZE];
static size_t http_response_length;
static uint8_t packet_header[HUMAN_DETECT_GRAY_HEADER_SIZE];
static size_t packet_header_length;
static size_t frame_length;
static uint64_t received_packets;
static char last_reported_status[24];

static void begin_request(uint64_t now);

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static uint64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000ULL +
	       (uint64_t)now.tv_nsec / 1000000ULL;
}

static void set_connection(const char *text, uint32_t color)
{
	const char *previous;

	if (!connection_label)
		return;
	previous = lv_label_get_text(connection_label);
	if ((!previous || strcmp(previous, text) != 0) &&
	    strcmp(text, "CONNECTING") != 0 && strcmp(text, "SYNCING") != 0 &&
	    strcmp(last_reported_status, text) != 0) {
		printf("human-detect-ui: stream %s\n", text);
		fflush(stdout);
		snprintf(last_reported_status, sizeof(last_reported_status), "%s", text);
	}
	lv_label_set_text(connection_label, text);
	lv_obj_set_style_text_color(connection_label, lv_color_hex(color), 0);
}

static void stream_disconnect(const char *status, uint32_t color)
{
	if (stream_fd >= 0)
		close(stream_fd);
	stream_fd = -1;
	stream_stage = STREAM_DISCONNECTED;
	http_request_length = http_request_offset = 0;
	http_response_length = 0;
	packet_header_length = 0;
	frame_length = 0;
	retry_at_ms = monotonic_ms() + CONNECT_RETRY_MS;
	set_connection(status, color);
}

static int stream_connect(uint64_t now)
{
	struct sockaddr_un address;
	int flags;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
	    fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		close(fd);
		return -1;
	}

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
	if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
		stream_fd = fd;
		begin_request(now);
		return 0;
	} else if (errno == EINPROGRESS || errno == EAGAIN) {
		stream_stage = STREAM_CONNECTING;
	} else {
		close(fd);
		set_connection("SERVICE OFFLINE", 0xf87171);
		retry_at_ms = now + CONNECT_RETRY_MS;
		return -1;
	}

	stream_fd = fd;
	set_connection("CONNECTING", 0xfbbf24);
	return 0;
}

static void begin_request(uint64_t now)
{
	http_request_length = (size_t)snprintf(
		http_request, sizeof(http_request),
		"GET /preview.gray8?fps=%d HTTP/1.0\r\n"
		"Host: localhost\r\n\r\n",
		preview_fps);
	http_request_offset = 0;
	http_response_length = 0;
	packet_header_length = 0;
	frame_length = 0;
	stream_stage = STREAM_REQUEST;
	retry_at_ms = now + CONNECT_RETRY_MS;
	set_connection("SYNCING", 0x38bdff);
}

static void configure_box(lv_obj_t *box)
{
	lv_obj_set_scrollable(box, false);
	lv_obj_set_clickable(box, false);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_color(box, lv_color_hex(0xf59e0b), 0);
	lv_obj_set_style_border_width(box, 3, 0);
	lv_obj_set_style_radius(box, 2, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_hidden(box, true);
}

static void update_detection_boxes(const struct human_detect_gray_header *header)
{
	uint32_t shown = header->box_count > HUMAN_DETECT_PREVIEW_BOXES ?
		HUMAN_DETECT_PREVIEW_BOXES : header->box_count;

	for (uint32_t i = 0; i < HUMAN_DETECT_PREVIEW_BOXES; i++) {
		const struct human_detect_preview_box *box;
		int32_t left;
		int32_t top;
		int32_t right;
		int32_t bottom;

		if (!boxes_visible || i >= shown) {
			lv_obj_set_hidden(detection_boxes[i], true);
			continue;
		}
		box = &header->boxes[i];
		left = 8 + (int32_t)((uint32_t)box->x0 * (FRAME_WIDTH - 1) / 65535U);
		top = 8 + (int32_t)((uint32_t)box->y0 * (FRAME_HEIGHT - 1) / 65535U);
		right = 8 + (int32_t)((uint32_t)box->x1 * (FRAME_WIDTH - 1) / 65535U);
		bottom = 8 + (int32_t)((uint32_t)box->y1 * (FRAME_HEIGHT - 1) / 65535U);
		if (right - left < 3)
			right = left + 3;
		if (bottom - top < 3)
			bottom = top + 3;
		lv_obj_set_pos(detection_boxes[i], left, top);
		lv_obj_set_size(detection_boxes[i], right - left + 1, bottom - top + 1);
		lv_obj_set_hidden(detection_boxes[i], false);
		lv_obj_move_foreground(detection_boxes[i]);
	}
}

static void update_metrics(const struct human_detect_gray_header *header)
{
	uint32_t boxes = header->box_count > HUMAN_DETECT_PREVIEW_BOXES ?
		HUMAN_DETECT_PREVIEW_BOXES : header->box_count;

	lv_label_set_text(person_label,
			  header->human_present ? "PERSON DETECTED" : "NO PERSON");
	lv_obj_set_style_text_color(person_label,
		lv_color_hex(header->human_present ? 0xfbbf24 : 0x4ade80), 0);
	lv_label_set_text_fmt(fps_label, "%u.%02u FPS",
			      header->inference_fps_x100 / 100,
			      header->inference_fps_x100 % 100);
	lv_label_set_text_fmt(timing_label, "%u ms prep  /  %u ms infer",
			      (header->prepare_us + 500) / 1000,
			      (header->inference_us + 500) / 1000);
	lv_label_set_text_fmt(boxes_label, "%u DETECTION%s", boxes,
			      boxes == 1 ? "" : "S");
	lv_label_set_text_fmt(frame_label, "%llu  /  age %u ms",
			      (unsigned long long)header->frames,
			      header->frame_age_ms);
	lv_label_set_text_fmt(errors_label, "CAM %u   INFER %u",
			      header->camera_errors, header->inference_errors);
	update_detection_boxes(header);
}

static int finish_packet(void)
{
	const struct human_detect_gray_header *header =
		(const struct human_detect_gray_header *)packet_header;

	if (header->magic != HUMAN_DETECT_GRAY_MAGIC ||
	    header->version != HUMAN_DETECT_GRAY_VERSION ||
	    header->header_size != HUMAN_DETECT_GRAY_HEADER_SIZE ||
	    header->width != FRAME_WIDTH || header->height != FRAME_HEIGHT ||
	    header->stride != FRAME_WIDTH || !header->camera_ready) {
		stream_disconnect("BAD STREAM", 0xf87171);
		return -1;
	}
	have_frame = true;
	received_packets++;
	lv_obj_set_hidden(no_signal, true);
	update_metrics(header);
	lv_obj_invalidate(canvas);
	set_connection(paused ? "PAUSED" : "LIVE", paused ? 0xfbbf24 : 0x4ade80);
	packet_header_length = 0;
	frame_length = 0;
	stream_stage = STREAM_HEADER;
	return 0;
}

static int consume_stream_bytes(const uint8_t *data, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		size_t available = length - offset;

		if (stream_stage == STREAM_HTTP) {
			if (http_response_length >= sizeof(http_response) - 1) {
				stream_disconnect("BAD RESPONSE", 0xf87171);
				return -1;
			}
			http_response[http_response_length++] = (char)data[offset++];
			if (http_response_length < 4 ||
			    memcmp(http_response + http_response_length - 4,
				   "\r\n\r\n", 4) != 0)
				continue;

			http_response[http_response_length] = '\0';
			if (strncmp(http_response, "HTTP/1.1 200", 12) != 0) {
				stream_disconnect(strstr(http_response, "503") ?
						  "CAMERA OFFLINE" : "STREAM ERROR",
						  0xf87171);
				return -1;
			}
			http_response_length = 0;
			packet_header_length = 0;
			frame_length = 0;
			stream_stage = STREAM_HEADER;
		} else if (stream_stage == STREAM_HEADER) {
			size_t room = sizeof(packet_header) - packet_header_length;
			size_t take = available < room ? available : room;

			memcpy(packet_header + packet_header_length, data + offset,
			       take);
			packet_header_length += take;
			offset += take;
			if (packet_header_length == sizeof(packet_header))
				stream_stage = STREAM_BODY;
		} else if (stream_stage == STREAM_BODY) {
			size_t room = sizeof(gray_frame) - frame_length;
			size_t take = available < room ? available : room;

			memcpy(gray_frame + frame_length, data + offset, take);
			frame_length += take;
			offset += take;
			if (frame_length == sizeof(gray_frame) && finish_packet() < 0)
				return -1;
		} else {
			return -1;
		}
	}
	return 0;
}

static void stream_poll(uint64_t now)
{
	uint8_t receive_buffer[1024];

	if (paused)
		return;
	if (stream_stage == STREAM_DISCONNECTED) {
		if (now < retry_at_ms)
			return;
		stream_connect(now);
		return;
	}

	if (stream_stage == STREAM_CONNECTING) {
		struct pollfd descriptor = { .fd = stream_fd, .events = POLLOUT };
		int error = 0;
		socklen_t error_length = sizeof(error);

		if (poll(&descriptor, 1, 0) <= 0)
			return;
		if (getsockopt(stream_fd, SOL_SOCKET, SO_ERROR, &error,
			       &error_length) < 0 || error != 0) {
			stream_disconnect("SERVICE OFFLINE", 0xf87171);
			return;
		}
		begin_request(now);
	}

	if (stream_stage == STREAM_REQUEST) {
		while (http_request_offset < http_request_length) {
			ssize_t sent = send(stream_fd,
				http_request + http_request_offset,
				http_request_length - http_request_offset, 0);
			if (sent > 0) {
				http_request_offset += (size_t)sent;
				continue;
			}
			if (sent < 0 && errno == EINTR)
				continue;
			if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return;
			stream_disconnect("STREAM ERROR", 0xf87171);
			return;
		}
		stream_stage = STREAM_HTTP;
	}

	for (int pass = 0; pass < 8; pass++) {
		ssize_t received = recv(stream_fd, receive_buffer,
					sizeof(receive_buffer), 0);

		if (received == 0) {
			stream_disconnect("SERVICE CLOSED", 0xf87171);
			return;
		}
		if (received < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			stream_disconnect("STREAM ERROR", 0xf87171);
			return;
		}

		if (consume_stream_bytes(receive_buffer, (size_t)received) < 0)
			return;
	}
}

static void pause_event_cb(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_CLICKED)
		return;
	paused = !paused;
	if (paused) {
		stream_disconnect("PAUSED", 0xfbbf24);
		lv_label_set_text(pause_button_label, "RESUME");
		lv_label_set_text(pause_badge, "PAUSED");
		lv_obj_set_hidden(pause_badge, false);
	} else {
		retry_at_ms = monotonic_ms();
		lv_label_set_text(pause_button_label, "PAUSE");
		lv_obj_set_hidden(pause_badge, true);
		set_connection("CONNECTING", 0xfbbf24);
	}
	printf("human-detect-ui: %s\n", paused ? "paused" : "resumed");
	fflush(stdout);
}

static void boxes_event_cb(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_CLICKED)
		return;
	boxes_visible = !boxes_visible;
	lv_label_set_text(boxes_button_label, boxes_visible ? "BOXES ON" : "BOXES OFF");
	for (uint32_t i = 0; i < HUMAN_DETECT_PREVIEW_BOXES; i++) {
		if (!boxes_visible)
			lv_obj_set_hidden(detection_boxes[i], true);
	}
	if (boxes_visible && received_packets)
		update_detection_boxes(
			(const struct human_detect_gray_header *)packet_header);
	printf("human-detect-ui: boxes %s\n", boxes_visible ? "on" : "off");
	fflush(stdout);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
			    const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y,
			    int32_t width, int32_t height)
{
	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_set_pos(panel, x, y);
	lv_obj_set_size(panel, width, height);
	lv_obj_set_style_bg_color(panel, lv_color_hex(0x0d1822), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(panel, lv_color_hex(0x223447), 0);
	lv_obj_set_style_border_width(panel, 1, 0);
	lv_obj_set_style_radius(panel, 8, 0);
	lv_obj_set_style_pad_all(panel, 0, 0);
	lv_obj_set_scrollable(panel, false);
	return panel;
}

static lv_obj_t *make_button(lv_obj_t *parent, int32_t x, const char *text,
			     lv_event_cb_t callback, lv_obj_t **label_out)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_pos(button, x, 322);
	lv_obj_set_size(button, 180, 48);
	lv_obj_set_style_bg_color(button, lv_color_hex(0x193247), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(0x244b68),
				  LV_STATE_PRESSED);
	lv_obj_set_style_border_color(button, lv_color_hex(0x31536d), 0);
	lv_obj_set_style_border_width(button, 1, 0);
	lv_obj_set_style_radius(button, 6, 0);
	lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
	*label_out = make_label(button, text, &lv_font_montserrat_14, 0xdce8f4);
	lv_obj_center(*label_out);
	return button;
}

static bool build_ui(void)
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *header;
	lv_obj_t *preview_panel;
	lv_obj_t *info_panel;
	lv_obj_t *label;
	lv_obj_t *pause_button;
	lv_obj_t *boxes_button;

	if (!screen) {
		fprintf(stderr, "human-detect-ui: no active LVGL screen\n");
		return false;
	}

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x071018), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(screen, false);

	header = make_panel(screen, 0, 0, 800, 60);
	if (!header) {
		fprintf(stderr, "human-detect-ui: cannot allocate header\n");
		return false;
	}
	lv_obj_set_style_radius(header, 0, 0);
	lv_obj_set_style_border_width(header, 0, 0);
	label = make_label(header, "HUMAN DETECTOR", &lv_font_montserrat_24,
			   0xe8f1fa);
	lv_obj_set_pos(label, 20, 15);
	connection_label = make_label(header, "CONNECTING", &lv_font_montserrat_14,
				      0xfbbf24);
	lv_obj_align(connection_label, LV_ALIGN_RIGHT_MID, -22, 0);

	preview_panel = make_panel(screen, 16, 76, 336, 256);
	if (!preview_panel) {
		fprintf(stderr, "human-detect-ui: cannot allocate preview panel\n");
		return false;
	}
	lv_obj_set_style_bg_color(preview_panel, lv_color_hex(0x020406), 0);
	lv_obj_set_style_border_color(preview_panel, lv_color_hex(0x31536d), 0);
	canvas = lv_canvas_create(preview_panel);
	if (!canvas) {
		fprintf(stderr, "human-detect-ui: cannot allocate preview canvas\n");
		return false;
	}
	lv_obj_set_pos(canvas, 8, 8);
	lv_obj_set_size(canvas, FRAME_WIDTH, FRAME_HEIGHT);
	lv_canvas_set_buffer(canvas, gray_frame, FRAME_WIDTH, FRAME_HEIGHT,
			     LV_COLOR_FORMAT_L8);
	lv_obj_set_scrollable(canvas, false);

	no_signal = make_label(preview_panel, "NO SIGNAL", &lv_font_montserrat_24,
			       0x64748b);
	lv_obj_center(no_signal);
	pause_badge = make_label(preview_panel, "PAUSED", &lv_font_montserrat_14,
				 0xfbbf24);
	lv_obj_set_pos(pause_badge, 18, 18);
	lv_obj_set_style_bg_color(pause_badge, lv_color_hex(0x071018), 0);
	lv_obj_set_style_bg_opa(pause_badge, LV_OPA_80, 0);
	lv_obj_set_style_pad_all(pause_badge, 4, 0);
	lv_obj_set_hidden(pause_badge, true);

	for (uint32_t i = 0; i < HUMAN_DETECT_PREVIEW_BOXES; i++) {
		detection_boxes[i] = lv_obj_create(preview_panel);
		configure_box(detection_boxes[i]);
	}

	info_panel = make_panel(screen, 368, 76, 416, 388);
	if (!info_panel) {
		fprintf(stderr, "human-detect-ui: cannot allocate info panel\n");
		return false;
	}
	label = make_label(info_panel, "PRESENCE", &lv_font_montserrat_14,
			   0x7f93a8);
	lv_obj_set_pos(label, 24, 20);
	person_label = make_label(info_panel, "WAITING", &lv_font_montserrat_24,
				  0x7f93a8);
	lv_obj_set_pos(person_label, 24, 44);

	label = make_label(info_panel, "INFERENCE RATE", &lv_font_montserrat_14,
			   0x7f93a8);
	lv_obj_set_pos(label, 24, 88);
	fps_label = make_label(info_panel, "0.00 FPS", &lv_font_montserrat_26,
			       0x38bdff);
	lv_obj_set_pos(fps_label, 24, 112);

	label = make_label(info_panel, "PIPELINE", &lv_font_montserrat_14,
			   0x7f93a8);
	lv_obj_set_pos(label, 220, 88);
	timing_label = make_label(info_panel, "0 ms prep / 0 ms infer",
				  &lv_font_montserrat_14, 0xc6d4e2);
	lv_obj_set_pos(timing_label, 220, 118);

	label = make_label(info_panel, "DETECTIONS", &lv_font_montserrat_14,
			   0x7f93a8);
	lv_obj_set_pos(label, 24, 166);
	boxes_label = make_label(info_panel, "0 DETECTIONS", &lv_font_montserrat_20,
				 0xe8f1fa);
	lv_obj_set_pos(boxes_label, 24, 190);

	label = make_label(info_panel, "FRAME", &lv_font_montserrat_14,
			   0x7f93a8);
	lv_obj_set_pos(label, 24, 230);
	frame_label = make_label(info_panel, "0 / age 0 ms", &lv_font_montserrat_14,
				 0xc6d4e2);
	lv_obj_set_pos(frame_label, 104, 230);

	label = make_label(info_panel, "ERRORS", &lv_font_montserrat_14,
			   0x7f93a8);
	lv_obj_set_pos(label, 24, 262);
	errors_label = make_label(info_panel, "CAM 0   INFER 0",
				  &lv_font_montserrat_14, 0x8b98a5);
	lv_obj_set_pos(errors_label, 104, 262);

	pause_button = make_button(info_panel, 16, "PAUSE", pause_event_cb,
				   &pause_button_label);
	boxes_button = make_button(info_panel, 214, "BOXES ON", boxes_event_cb,
				   &boxes_button_label);
	(void)pause_button;
	(void)boxes_button;

	label = make_label(screen, "320x240 L8 / LOCAL SOCKET / TAP TO CONTROL",
			   &lv_font_montserrat_14, 0x536273);
	lv_obj_set_pos(label, 22, 454);
	return true;
}

static void print_usage(const char *program)
{
	printf("usage: %s [-s /run/human-detect.sock]"
	       " [-d /dev/fb0|/dev/dri/cardN] [-g auto|none|/dev/dri/cardN]"
	       " [-i /dev/input/eventN|none] [-f 3..5] [-t seconds]\n",
	       program);
}

static int is_drm_path(const char *device)
{
	return strncmp(device, "/dev/dri/", 9) == 0;
}

int main(int argc, char **argv)
{
	const char *requested_device = NULL;
	const char *requested_gpu = "auto";
	const char *display_device;
	const char *input_device = "/dev/input/event0";
	char found_lcdc[64] = "";
	char found_gpu[64] = "";
	int seconds = 0;
	int opt;
	uint32_t started;
	lv_indev_t *indev = NULL;

	while ((opt = getopt(argc, argv, "s:d:g:i:f:t:h")) != -1) {
		switch (opt) {
		case 's':
			socket_path = optarg;
			break;
		case 'd':
			requested_device = optarg;
			break;
		case 'g':
			requested_gpu = optarg;
			break;
		case 'i':
			input_device = optarg;
			break;
		case 'f':
			preview_fps = atoi(optarg);
			break;
		case 't':
			seconds = atoi(optarg);
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}
	if (preview_fps < 3 || preview_fps > 5 || seconds < 0) {
		fprintf(stderr, "human-detect-ui: fps must be 3..5 and time >= 0\n");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	lv_init();
	printf("human-detect-ui: lv_init done\n");

	display_device = requested_device ? requested_device : "/dev/fb0";
	if (strcmp(requested_gpu, "none") &&
	    (!requested_device || is_drm_path(requested_device))) {
		if (!lvgl_gpu_discover(found_lcdc, sizeof(found_lcdc),
				       found_gpu, sizeof(found_gpu))) {
			const char *lcdc = requested_device ? requested_device : found_lcdc;
			const char *gpu = strcmp(requested_gpu, "auto") ?
				requested_gpu : found_gpu;

			gpu_backend = lvgl_gpu_display_create(&display, lcdc, gpu);
			if (!gpu_backend)
				fprintf(stderr,
					"human-detect-ui: GPU unavailable; falling back to fbdev\n");
		}
		if (!requested_device && found_lcdc[0])
			display_device = found_lcdc;
	}

	if (!gpu_backend) {
		/* The GPU backend needs a second full-screen DMA buffer. On a busy
		 * no-MMU system that allocation may fail after boot; fbdev already
		 * owns the scanout buffer and LVGL's partial draw buffer is ~50 KiB. */
		if (!requested_device)
			display_device = "/dev/fb0";
		if (is_drm_path(display_device)) {
			display = lv_linux_drm_create();
			if (!display ||
			    lv_linux_drm_set_file(display, display_device, -1) !=
					LV_RESULT_OK) {
				fprintf(stderr, "human-detect-ui: cannot initialize %s\n",
					display_device);
				return 1;
			}
		} else {
			display = lv_linux_fbdev_create();
			if (!display ||
			    lv_linux_fbdev_set_file(display, display_device) !=
					LV_RESULT_OK) {
				fprintf(stderr, "human-detect-ui: cannot initialize %s\n",
					display_device);
				return 1;
			}
		}
	}
	printf("human-detect-ui: display ready\n");

	if (strcmp(input_device, "none") != 0) {
		indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_device);
		if (!indev) {
			fprintf(stderr, "human-detect-ui: cannot open %s\n",
				input_device);
			return 1;
		}
		lv_indev_set_display(indev, display);
		lv_timer_set_period(lv_indev_get_read_timer(indev), 10);
	}
	lv_timer_set_period(lv_display_get_refr_timer(display), 50);

	if (!build_ui())
		return 1;
	printf("human-detect-ui: controls ready\n");
	started = lv_tick_get();
	printf("human-detect-ui: display=%s gpu=%s input=%s socket=%s fps=%d\n",
	       gpu_backend ? lvgl_gpu_lcdc_device(gpu_backend) : display_device,
	       gpu_backend ? lvgl_gpu_gpu_device(gpu_backend) : "software",
	       input_device, socket_path, preview_fps);
	fflush(stdout);

	while (running) {
		stream_poll(monotonic_ms());
		lv_timer_handler();
		usleep(10000);
		if (seconds > 0 && (uint32_t)(lv_tick_get() - started) >=
				   (uint32_t)seconds * 1000U)
			break;
	}

	if (stream_fd >= 0)
		close(stream_fd);
	printf("human-detect-ui: received %llu frames\n",
	       (unsigned long long)received_packets);
	if (gpu_backend)
		lvgl_gpu_display_destroy(gpu_backend);
	return 0;
}
