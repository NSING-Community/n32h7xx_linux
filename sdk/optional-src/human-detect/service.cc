/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * human-detect owns /dev/video0 while it runs.
 *
 * The position detector consumes 320x240 GREY frames. /preview.mjpg republishes
 * those exact frames with detection boxes, while /snapshot.jpg temporarily
 * switches the camera to JPEG for a full-colour still. Keeping one process as
 * the only V4L2 client avoids /dev/video0 races.
 */
#include "detector.h"
#include "dashboard.h"
#include "n32jpeg.h"
#include "preview-gray8.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr int kDetectWidth = 320;
constexpr int kDetectHeight = 240;
constexpr int kFrameTimeoutMs = 3000;
constexpr uint64_t kCameraStallMs = 5000;
constexpr int kSnapshotMinWarmup = 3;
constexpr int kSnapshotMaxFrames = 20;
constexpr int kSnapshotSettleFrames = 3;
constexpr int kPreviewDefaultFps = 5;
constexpr int kPreviewMinFps = 3;
constexpr int kPreviewMaxFps = 5;
constexpr int kPreviewDefaultSeconds = 0; /* until the client disconnects */
constexpr int kPreviewMaxSeconds = 60;
constexpr uint64_t kPreviewWriteTimeoutMs = 3000;
constexpr size_t kPreviewJpegMax = 128 * 1024;
constexpr size_t kPreviewGrayBytes =
	sizeof(struct human_detect_gray_header) +
	HUMAN_DETECT_PREVIEW_WIDTH * HUMAN_DETECT_PREVIEW_HEIGHT;
constexpr size_t kHttpRequestMax = 4096;
constexpr const char *kDefaultSocket = "/run/human-detect.sock";
constexpr const char *kDefaultTaskSocket = "/run/weniclaw/control.sock";
constexpr const char *kDefaultEventImage =
	"/system/weniclaw/workspace/latest-human.jpg";
constexpr uint64_t kTaskDedupeWindowMs = 5 * 60 * 1000;
constexpr uint64_t kTaskRetryMs = 30 * 1000;
constexpr int kTaskConnectTimeoutMs = 100;
constexpr int kTaskResponseTimeoutMs = 300;

struct ImageSize {
	uint32_t width;
	uint32_t height;
};

constexpr ImageSize kSnapshotSizes[] = {
	{ 160, 120 },   { 176, 144 },   { 320, 240 },
	{ 640, 480 },   { 720, 480 },   { 720, 576 },
	{ 1024, 768 },  { 1280, 720 },  { 1920, 1080 },
	{ 2592, 1944 },
};

constexpr ImageSize kDefaultSnapshot = { 1024, 768 };
constexpr ImageSize kEventSnapshotSize = { 1024, 768 };
constexpr ImageSize kPreviewSize = { kDetectWidth, kDetectHeight };
constexpr ImageSize kPreviewSizes[] = { kPreviewSize };

static_assert(sizeof(struct human_detect_preview_box) == 10,
	      "gray8 preview box ABI changed");
static_assert(sizeof(struct human_detect_gray_header) ==
		      HUMAN_DETECT_GRAY_HEADER_SIZE,
	      "gray8 preview header ABI changed");

enum class PreviewMode {
	None,
	Mjpeg,
	Gray8,
};

struct Config {
	const char *device = "/dev/video0";
	const char *socket_path = kDefaultSocket;
	const char *task_socket_path = kDefaultTaskSocket;
	const char *event_image_path = kDefaultEventImage;
	const char *json_path = nullptr;
	unsigned int threshold = 102;
	uint16_t http_port = 0;
};

struct Camera {
	int fd = -1;
	bool streaming = false;
	void *mapping = nullptr;
	size_t mapping_length = 0;
	struct v4l2_format format;
};

struct ServiceState {
	bool camera_ready = false;
	bool camera_stream_synced = false;
	bool human_present = false;
	bool task_pending = false;
	bool event_image_ready = false;
	bool have_result = false;
	bool preview_active = false;
	uint64_t frames = 0;
	uint64_t camera_errors = 0;
	uint64_t inference_errors = 0;
	uint64_t preview_errors = 0;
	uint64_t snapshots = 0;
	uint64_t previews = 0;
	uint64_t preview_frames = 0;
	uint64_t task_submissions = 0;
	uint64_t task_submit_errors = 0;
	uint64_t rate_window_frames = 0;
	uint64_t rate_window_started_ms = 0;
	uint64_t last_frame_ms = 0;
	uint64_t last_human_change_ms = 0;
	uint64_t last_task_submit_ms = 0;
	uint64_t next_task_submit_ms = 0;
	uint64_t task_event_epoch_s = 0;
	uint64_t last_snapshot_ms = 0;
	uint64_t last_preview_ms = 0;
	uint32_t snapshot_ms = 0;
	uint32_t preview_ms = 0;
	uint32_t prepare_us = 0;
	uint32_t inference_us = 0;
	uint32_t inference_fps_x100 = 0;
	struct human_detect_result result = {};
	alignas(16) uint8_t gray_frame[kDetectWidth * kDetectHeight];
};

struct PreviewClient {
	int fd = -1;
	int jpeg_fd = -1;
	PreviewMode mode = PreviewMode::None;
	uint64_t started_ms = 0;
	uint64_t deadline_ms = 0;
	uint64_t next_frame_ms = 0;
	uint64_t last_io_ms = 0;
	unsigned int fps = kPreviewDefaultFps;
	uint8_t *jpeg = nullptr;
	uint8_t *gray_packet = nullptr;
	char prefix[192];
	size_t prefix_length = 0;
	size_t prefix_offset = 0;
	size_t jpeg_length = 0;
	size_t jpeg_offset = 0;
	size_t gray_length = 0;
	size_t gray_offset = 0;
	size_t suffix_offset = 0;
	bool pending = false;
};

struct HttpRequest {
	char method[16];
	char target[512];
};

void preview_close(PreviewClient *preview, ServiceState *state,
		   const char *reason);
bool save_event_snapshot(Camera *camera, ServiceState *state,
			 PreviewClient *preview, const char *path);

volatile sig_atomic_t g_stop;

uint64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return static_cast<uint64_t>(now.tv_sec) * 1000 +
	       static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

uint64_t monotonic_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return static_cast<uint64_t>(now.tv_sec) * 1000000 +
	       static_cast<uint64_t>(now.tv_nsec) / 1000;
}

void on_signal(int)
{
	g_stop = 1;
}

void log_msg(const char *format, ...)
{
	va_list args;

	fprintf(stderr, "human-detect: ");
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

const char *fourcc_string(uint32_t fourcc, char text[5])
{
	text[0] = static_cast<char>(fourcc & 0xff);
	text[1] = static_cast<char>((fourcc >> 8) & 0xff);
	text[2] = static_cast<char>((fourcc >> 16) & 0xff);
	text[3] = static_cast<char>((fourcc >> 24) & 0xff);
	text[4] = '\0';
	return text;
}

void camera_stop(Camera *camera)
{
	if (camera->fd >= 0 && camera->streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (ioctl(camera->fd, VIDIOC_STREAMOFF, &type) < 0)
			log_msg("VIDIOC_STREAMOFF: %s", strerror(errno));
	}
	camera->streaming = false;

	if (camera->mapping && camera->mapping != MAP_FAILED) {
		munmap(camera->mapping, camera->mapping_length);
		camera->mapping = nullptr;
		camera->mapping_length = 0;
	}

	if (camera->fd >= 0) {
		struct v4l2_requestbuffers request;

		memset(&request, 0, sizeof(request));
		request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		request.memory = V4L2_MEMORY_MMAP;
		request.count = 0;
		if (ioctl(camera->fd, VIDIOC_REQBUFS, &request) < 0 &&
		    errno != EINVAL)
			log_msg("VIDIOC_REQBUFS(0): %s", strerror(errno));
	}
}

void camera_close(Camera *camera)
{
	camera_stop(camera);
	if (camera->fd >= 0)
		close(camera->fd);
	camera->fd = -1;
}

int camera_open(Camera *camera, const char *device)
{
	camera_close(camera);
	camera->fd = open(device, O_RDWR | O_NONBLOCK);
	if (camera->fd < 0) {
		log_msg("cannot open %s: %s", device, strerror(errno));
		return -1;
	}
	return 0;
}

int camera_start(Camera *camera, uint32_t pixel_format, uint32_t width,
		 uint32_t height)
{
	struct v4l2_requestbuffers request;
	struct v4l2_buffer buffer;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	char code[5];

	camera_stop(camera);
	if (camera->fd < 0)
		return -1;

	memset(&camera->format, 0, sizeof(camera->format));
	camera->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	camera->format.fmt.pix.width = width;
	camera->format.fmt.pix.height = height;
	camera->format.fmt.pix.pixelformat = pixel_format;
	camera->format.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(camera->fd, VIDIOC_S_FMT, &camera->format) < 0) {
		log_msg("VIDIOC_S_FMT: %s", strerror(errno));
		return -1;
	}
	if (camera->format.fmt.pix.pixelformat != pixel_format ||
	    camera->format.fmt.pix.width != width ||
	    camera->format.fmt.pix.height != height) {
		log_msg("camera chose %s %ux%u instead of requested %s %ux%u",
			fourcc_string(camera->format.fmt.pix.pixelformat, code),
			camera->format.fmt.pix.width,
			camera->format.fmt.pix.height,
			fourcc_string(pixel_format, code), width, height);
		return -1;
	}

	memset(&request, 0, sizeof(request));
	request.count = 1;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;
	if (ioctl(camera->fd, VIDIOC_REQBUFS, &request) < 0) {
		log_msg("VIDIOC_REQBUFS: %s", strerror(errno));
		return -1;
	}
	if (request.count < 1) {
		log_msg("camera allocated no buffers");
		return -1;
	}

	memset(&buffer, 0, sizeof(buffer));
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = 0;
	if (ioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
		log_msg("VIDIOC_QUERYBUF: %s", strerror(errno));
		camera_stop(camera);
		return -1;
	}
	camera->mapping_length = buffer.length;
	camera->mapping = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
			       MAP_SHARED, camera->fd, buffer.m.offset);
	if (camera->mapping == MAP_FAILED) {
		camera->mapping = nullptr;
		camera->mapping_length = 0;
		log_msg("mmap: %s", strerror(errno));
		camera_stop(camera);
		return -1;
	}

	memset(&buffer, 0, sizeof(buffer));
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = 0;
	if (ioctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) {
		log_msg("VIDIOC_QBUF: %s", strerror(errno));
		camera_stop(camera);
		return -1;
	}
	if (ioctl(camera->fd, VIDIOC_STREAMON, &type) < 0) {
		log_msg("VIDIOC_STREAMON: %s", strerror(errno));
		camera_stop(camera);
		return -1;
	}
	camera->streaming = true;
	return 0;
}

int camera_start_grey(Camera *camera)
{
	return camera_start(camera, V4L2_PIX_FMT_GREY, kDetectWidth,
			    kDetectHeight);
}

int camera_recover_grey(Camera *camera, const char *device)
{
	camera_close(camera);
	if (camera_open(camera, device) < 0)
		return -1;
	if (camera_start_grey(camera) < 0) {
		camera_close(camera);
		return -1;
	}
	return 0;
}

int camera_wait(Camera *camera)
{
	struct pollfd descriptor;
	int result;

	descriptor.fd = camera->fd;
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	result = poll(&descriptor, 1, kFrameTimeoutMs);
	if (result == 0)
		return 0;
	if (result < 0) {
		if (errno == EINTR)
			return 0;
		return -1;
	}
	if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -1;
	return (descriptor.revents & POLLIN) ? 1 : 0;
}

int camera_dequeue(Camera *camera, struct v4l2_buffer *buffer)
{
	int ready = camera_wait(camera);

	if (ready <= 0)
		return ready;

	memset(buffer, 0, sizeof(*buffer));
	buffer->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer->memory = V4L2_MEMORY_MMAP;
	if (ioctl(camera->fd, VIDIOC_DQBUF, buffer) < 0) {
		if (errno == EAGAIN)
			return 0;
		log_msg("VIDIOC_DQBUF: %s", strerror(errno));
		return -1;
	}
	return 1;
}

int camera_queue(Camera *camera, struct v4l2_buffer *buffer)
{
	if (ioctl(camera->fd, VIDIOC_QBUF, buffer) < 0) {
		log_msg("VIDIOC_QBUF: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int ctrl_get(int fd, unsigned int id, long *value)
{
	struct v4l2_control control;

	memset(&control, 0, sizeof(control));
	control.id = id;
	if (ioctl(fd, VIDIOC_G_CTRL, &control) < 0)
		return -1;
	*value = control.value;
	return 0;
}

bool jpeg_is_complete(const Camera *camera, const struct v4l2_buffer &buffer)
{
	const uint8_t *data = static_cast<const uint8_t *>(camera->mapping);
	const uint32_t length = buffer.bytesused;

	if (buffer.flags & V4L2_BUF_FLAG_ERROR)
		return false;
	if (length < 4 || length > camera->mapping_length)
		return false;
	/* A frame that exactly fills the driver buffer may have been truncated. */
	if (length >= camera->format.fmt.pix.sizeimage)
		return false;
	return data[0] == 0xff && data[1] == 0xd8 &&
	       data[length - 2] == 0xff && data[length - 1] == 0xd9;
}

template <size_t N>
bool supported_size(const ImageSize (&sizes)[N], uint32_t width,
		    uint32_t height)
{
	for (const ImageSize &size : sizes) {
		if (size.width == width && size.height == height)
			return true;
	}
	return false;
}

bool parse_dimension(const char *text, uint32_t *value)
{
	char *end = nullptr;
	unsigned long parsed;

	if (!text || !*text)
		return false;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !end || *end || parsed == 0 || parsed > 8192)
		return false;
	*value = static_cast<uint32_t>(parsed);
	return true;
}

template <size_t N>
bool parse_image_options(char *query, const ImageSize &fallback,
			 const ImageSize (&sizes)[N], ImageSize *size,
			 unsigned int *fps, unsigned int *seconds)
{
	bool have_width = false;
	bool have_height = false;
	uint32_t width = fallback.width;
	uint32_t height = fallback.height;
	char *save = nullptr;

	if (query) {
		for (char *item = strtok_r(query, "&", &save); item;
		     item = strtok_r(nullptr, "&", &save)) {
			if (!strncmp(item, "size=", 5)) {
				char *x = strchr(item + 5, 'x');

				if (!x)
					return false;
				*x++ = '\0';
				if (!parse_dimension(item + 5, &width) ||
				    !parse_dimension(x, &height))
					return false;
				have_width = true;
				have_height = true;
			} else if (!strncmp(item, "w=", 2)) {
				if (!parse_dimension(item + 2, &width))
					return false;
				have_width = true;
			} else if (!strncmp(item, "h=", 2)) {
				if (!parse_dimension(item + 2, &height))
					return false;
				have_height = true;
			} else if (fps && !strncmp(item, "fps=", 4)) {
				uint32_t parsed;

				if (!parse_dimension(item + 4, &parsed) ||
				    parsed < kPreviewMinFps ||
				    parsed > kPreviewMaxFps)
					return false;
				*fps = parsed;
			} else if (seconds && !strncmp(item, "duration=", 9)) {
				uint32_t parsed;

				if (!parse_dimension(item + 9, &parsed) ||
				    parsed > kPreviewMaxSeconds)
					return false;
				*seconds = parsed;
			}
		}
	}

	if (have_width != have_height)
		return false;
	if (!supported_size(sizes, width, height))
		return false;
	size->width = width;
	size->height = height;
	return true;
}

int send_all(int fd, const void *data, size_t length)
{
	const uint8_t *cursor = static_cast<const uint8_t *>(data);

	while (length) {
		ssize_t sent = send(fd, cursor, length, 0);

		if (sent > 0) {
			cursor += sent;
			length -= static_cast<size_t>(sent);
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd write_fd;

			write_fd.fd = fd;
			write_fd.events = POLLOUT;
			write_fd.revents = 0;
			if (poll(&write_fd, 1, 5000) <= 0)
				return -1;
			continue;
		}
		return -1;
	}
	return 0;
}

bool write_file_atomic(const char *path, const void *data, size_t length)
{
	char temporary[512];
	const uint8_t *cursor = static_cast<const uint8_t *>(data);
	size_t remaining = length;
	int written;
	int fd;

	if (!path || !*path || strlen(path) + 16 >= sizeof(temporary))
		return false;
	written = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
	if (written < 0 || static_cast<size_t>(written) >= sizeof(temporary))
		return false;
	fd = mkstemp(temporary);
	if (fd < 0)
		return false;
	if (fchmod(fd, 0600) < 0)
		goto failed;
	while (remaining) {
		ssize_t result = write(fd, cursor, remaining);

		if (result > 0) {
			cursor += result;
			remaining -= static_cast<size_t>(result);
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		goto failed;
	}
	if (fsync(fd) < 0)
		goto failed;
	if (close(fd) < 0) {
		fd = -1;
		goto failed_after_close;
	}
	fd = -1;
	if (rename(temporary, path) < 0)
		goto failed;
	return true;

failed:
	if (fd >= 0)
		close(fd);
failed_after_close:
	unlink(temporary);
	return false;
}

int send_http_headers(int fd, int status, const char *reason,
		      const char *content_type, size_t content_length)
{
	char headers[512];
	int length = snprintf(headers, sizeof(headers),
			      "HTTP/1.1 %d %s\r\n"
			      "Content-Type: %s\r\n"
			      "Content-Length: %zu\r\n"
			      "Cache-Control: no-store\r\n"
			      "Connection: close\r\n\r\n",
			      status, reason, content_type, content_length);

	if (length < 0 || static_cast<size_t>(length) >= sizeof(headers))
		return -1;
	return send_all(fd, headers, static_cast<size_t>(length));
}

void send_http_text(int fd, int status, const char *reason, const char *body)
{
	size_t length = strlen(body);

	if (send_http_headers(fd, status, reason, "text/plain; charset=utf-8",
			      length) == 0)
		send_all(fd, body, length);
}

bool append_text(char *buffer, size_t capacity, size_t *used,
		 const char *format, ...)
{
	va_list args;
	int written;

	if (*used >= capacity)
		return false;
	va_start(args, format);
	written = vsnprintf(buffer + *used, capacity - *used, format, args);
	va_end(args);
	if (written < 0 || static_cast<size_t>(written) >= capacity - *used)
		return false;
	*used += static_cast<size_t>(written);
	return true;
}

size_t build_status_json(char *buffer, size_t capacity,
			 const ServiceState *state)
{
	size_t used = 0;
	uint64_t now = monotonic_ms();

	if (!append_text(
		    buffer, capacity, &used,
		    "{\"type\":\"human_detector.status\",\"version\":1,"
		    "\"model_type\":\"object_detection\",\"input\":\"320x240 GREY\","
		    "\"camera_ready\":%s,\"have_result\":%s,\"frames\":%llu,"
		    "\"camera_errors\":%llu,\"inference_errors\":%llu,"
		    "\"preview_errors\":%llu,\"snapshots\":%llu,\"previews\":%llu,"
			    "\"preview_active\":%s,\"preview_frames\":%llu,"
			    "\"event_image_ready\":%s,"
		    "\"last_frame_ms\":%llu,\"last_snapshot_ms\":%llu,"
		    "\"snapshot_ms\":%u,\"last_preview_ms\":%llu,\"preview_ms\":%u,"
		    "\"uptime_ms\":%llu,\"prepare_us\":%u,\"inference_us\":%u,"
		    "\"inference_fps\":%u.%02u,"
		    "\"human_detected\":%s,\"human_changed_ms\":%llu,"
			    "\"task_pending\":%s,\"task_submissions\":%llu,"
			    "\"task_submit_errors\":%llu,\"last_task_submit_ms\":%llu,"
		    "\"preview_default\":\"%ux%u\",\"preview_fps\":%d,"
		    "\"preview_max_seconds\":%d,\"count\":%u,\"boxes\":[",
		    state->camera_ready ? "true" : "false",
		    state->have_result ? "true" : "false",
		    static_cast<unsigned long long>(state->frames),
		    static_cast<unsigned long long>(state->camera_errors),
		    static_cast<unsigned long long>(state->inference_errors),
		    static_cast<unsigned long long>(state->preview_errors),
		    static_cast<unsigned long long>(state->snapshots),
		    static_cast<unsigned long long>(state->previews),
		    state->preview_active ? "true" : "false",
		    static_cast<unsigned long long>(state->preview_frames),
			    state->event_image_ready ? "true" : "false",
		    static_cast<unsigned long long>(state->last_frame_ms),
		    static_cast<unsigned long long>(state->last_snapshot_ms),
		    state->snapshot_ms,
		    static_cast<unsigned long long>(state->last_preview_ms),
		    state->preview_ms, static_cast<unsigned long long>(now),
		    state->prepare_us, state->inference_us,
		    state->inference_fps_x100 / 100,
		    state->inference_fps_x100 % 100,
		    state->human_present ? "true" : "false",
		    static_cast<unsigned long long>(state->last_human_change_ms),
			    state->task_pending ? "true" : "false",
			    static_cast<unsigned long long>(state->task_submissions),
			    static_cast<unsigned long long>(state->task_submit_errors),
			    static_cast<unsigned long long>(state->last_task_submit_ms),
		    kPreviewSize.width, kPreviewSize.height,
		    kPreviewDefaultFps, kPreviewMaxSeconds, state->result.count))
		return 0;

	for (unsigned int i = 0; i < state->result.count; ++i) {
		const struct human_detect_box &box = state->result.boxes[i];

		if (!append_text(buffer, capacity, &used,
				 "%s{\"x0\":%u,\"y0\":%u,\"x1\":%u,"
				 "\"y1\":%u,\"score\":%u}",
				 i ? "," : "", box.x0, box.y0, box.x1, box.y1,
				 box.score))
			return 0;
	}
	if (!append_text(buffer, capacity, &used, "]}\n"))
		return 0;
	return used;
}

void write_detection_json(FILE *file, const ServiceState *state)
{
	char json[1800];
	size_t length;

	if (!file)
		return;
	length = build_status_json(json, sizeof(json), state);
	if (!length)
		return;
	fwrite(json, 1, length, file);
	fflush(file);
}

bool read_http_request(int fd, HttpRequest *request)
{
	char buffer[kHttpRequestMax];
	size_t used = 0;

	memset(buffer, 0, sizeof(buffer));
	while (used + 1 < sizeof(buffer)) {
		struct pollfd descriptor;
		ssize_t got;

		descriptor.fd = fd;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		if (poll(&descriptor, 1, 1500) <= 0)
			return false;
		got = recv(fd, buffer + used, sizeof(buffer) - used - 1, 0);
		if (got <= 0)
			return false;
		used += static_cast<size_t>(got);
		buffer[used] = '\0';
		if (strstr(buffer, "\r\n\r\n") || strstr(buffer, "\n\n"))
			break;
	}

	if (sscanf(buffer, "%15s %511s", request->method, request->target) != 2)
		return false;
	return true;
}

bool detection_changed(const struct human_detect_result &old_result,
		       const struct human_detect_result &new_result)
{
	if (old_result.count != new_result.count)
		return true;
	for (unsigned int i = 0; i < new_result.count; ++i) {
		const struct human_detect_box &a = old_result.boxes[i];
		const struct human_detect_box &b = new_result.boxes[i];

		if (a.x0 != b.x0 || a.y0 != b.y0 || a.x1 != b.x1 ||
		    a.y1 != b.y1 || a.score != b.score)
			return true;
	}
	return false;
}

bool wait_for_fd(int fd, short events, int timeout_ms)
{
	uint64_t deadline = monotonic_ms() + static_cast<uint64_t>(timeout_ms);

	for (;;) {
		struct pollfd descriptor;
		uint64_t now = monotonic_ms();
		int timeout;
		int ready;

		descriptor.fd = fd;
		descriptor.events = events;
		descriptor.revents = 0;
		if (now >= deadline)
			return false;
		timeout = static_cast<int>(deadline - now);
		ready = poll(&descriptor, 1, timeout);
		if (ready > 0)
			return (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
		if (ready == 0)
			return false;
		if (errno != EINTR)
			return false;
	}
}

bool send_all_local(int fd, const char *data, size_t length)
{
	size_t sent = 0;

	while (sent < length) {
		ssize_t written = send(fd, data + sent, length - sent, 0);

		if (written > 0) {
			sent += static_cast<size_t>(written);
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
		    wait_for_fd(fd, POLLOUT, kTaskConnectTimeoutMs))
			continue;
		return false;
	}
	return true;
}

bool submit_human_task(const Config *config, const struct human_detect_result &result,
		       uint64_t *event_epoch_s, const char *image_path)
{
	static constexpr char kText[] =
		"\xE6\xA3\x80\xE6\xB5\x8B\xE5\x88\xB0\xE4\xBA\xBA\xE5\x91\x98";
	static constexpr char kImageDeliverTemplate[] =
		"\xE6\xA3\x80\xE6\xB5\x8B\xE5\x88\xB0\xE4\xBA\xBA\xE5\x91\x98\xEF\xBC\x9A"
		"{{payload.count}} \xE4\xBA\xBA\\n"
		"\xE7\xAB\xAF\xE4\xBE\xA7\xE6\xA8\xA1\xE5\x9E\x8B\xE6\xA3\x80\xE6\xB5\x8B"
		"\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C\xE5\xBD\xA9\xE8\x89\xB2\xE4\xBA\x8B"
		"\xE4\xBB\xB6\xE5\x9B\xBE\xE7\x89\x87\xE9\x9A\x8F\xE5\x90\x8E\xE5\x8F\x91"
		"\xE9\x80\x81\xE3\x80\x82";
	static constexpr char kTextDeliverTemplate[] =
		"\xE6\xA3\x80\xE6\xB5\x8B\xE5\x88\xB0\xE4\xBA\xBA\xE5\x91\x98\xEF\xBC\x9A"
		"{{payload.count}} \xE4\xBA\xBA\\n"
		"\xE4\xBA\x8B\xE4\xBB\xB6\xE5\xBD\xA9\xE8\x89\xB2\xE5\x9B\xBE\xE7\x89\x87"
		"\xE6\x8D\x95\xE8\x8E\xB7\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE8\xAF\xB7"
		"\xE6\xA3\x80\xE6\x9F\xA5\xE6\x91\x84\xE5\x83\x8F\xE5\xA4\xB4\xE7\x8A\xB6"
		"\xE6\x80\x81\xE3\x80\x82";
	static constexpr char kImageRememberTemplate[] =
		"\xE7\xAB\xAF\xE4\xBE\xA7\xE6\xA8\xA1\xE5\x9E\x8B\xE6\xA3\x80\xE6\xB5\x8B"
		"\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C\xE4\xBA\x8B\xE4\xBB\xB6\xE5\xBD\xA9"
		"\xE8\x89\xB2 JPEG \xE5\xB7\xB2\xE6\x8D\x95\xE8\x8E\xB7\xEF\xBC\x9B\xE6"
		"\xA3\x80\xE6\xB5\x8B\xE8\xBE\x93\xE5\x85\xA5\xE4\xB8\xBA 320x240 GREY\xE3"
		"\x80\x82\xE5\x9B\xBE\xE7\x89\x87\xE7\x94\xB1\xE5\x90\x8C\xE4\xB8\x80\xE6"
		"\x91\x84\xE5\x83\x8F\xE5\xA4\xB4\xE5\x9C\xA8\xE4\xBA\x8B\xE4\xBB\xB6\xE5"
		"\x8F\x91\xE7\x94\x9F\xE6\x97\xB6\xE6\x8B\x8D\xE6\x91\x84\xEF\xBC\x8C\xE8"
		"\xB7\xAF\xE5\xBE\x84\xE8\xA7\x81 payload.image\xE3\x80\x82";
	static constexpr char kTextRememberTemplate[] =
		"\xE7\xAB\xAF\xE4\xBE\xA7\xE6\xA8\xA1\xE5\x9E\x8B\xE6\xA3\x80\xE6\xB5\x8B"
		"\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C\xE4\xBD\x86\xE4\xBA\x8B\xE4\xBB\xB6"
		"\xE5\xBD\xA9\xE8\x89\xB2 JPEG \xE6\x9C\xAA\xE6\x8D\x95\xE8\x8E\xB7\xEF\xBC"
		"\x9B\xE6\xA3\x80\xE6\xB5\x8B\xE8\xBE\x93\xE5\x85\xA5\xE4\xB8\xBA 320x240 "
		"GREY\xE3\x80\x82";
	char payload[4096];
	char request[6144];
	char response[128];
	struct sockaddr_un address;
	size_t offset;
	int payload_length;
	int request_length;
	int fd;
	int flags;
	unsigned int boxes = result.count;
	bool has_image = image_path && image_path[0];

	if (boxes > HUMAN_DETECT_MAX_BOXES)
		boxes = HUMAN_DETECT_MAX_BOXES;
	if (!event_epoch_s)
		return false;
	if (!*event_epoch_s) {
		struct timespec wall;

		if (clock_gettime(CLOCK_REALTIME, &wall) < 0 || wall.tv_sec <= 0)
			return false;
		*event_epoch_s = static_cast<uint64_t>(wall.tv_sec);
	}
	if (!config->task_socket_path || strlen(config->task_socket_path) >= sizeof(address.sun_path))
		return false;
	payload_length = snprintf(
		payload, sizeof(payload),
		"{\"schema\":\"weniclaw.task.v1\",\"source\":\"human-detect\","
		"\"event\":\"human.detected\",\"payload\":{\"text\":\"%s\","
			"\"count\":%u,\"captured_at_s\":%llu,\"input\":\"320x240 GREY\","
			"\"input_width\":320,\"input_height\":240%s%s%s,\"boxes\":[",
		kText, boxes, static_cast<unsigned long long>(*event_epoch_s),
		has_image ? ",\"image\":\"" : "", has_image ? image_path : "",
		has_image ? "\"" : "");
	if (payload_length < 0 || static_cast<size_t>(payload_length) >= sizeof(payload))
		return false;
	offset = static_cast<size_t>(payload_length);
	for (unsigned int i = 0; i < boxes; ++i) {
		const struct human_detect_box &box = result.boxes[i];
		int written = snprintf(
			payload + offset, sizeof(payload) - offset,
			"%s{\"x0\":%u,\"y0\":%u,\"x1\":%u,\"y1\":%u,\"score\":%u}",
			i ? "," : "", box.x0, box.y0, box.x1, box.y1, box.score);

		if (written < 0 || static_cast<size_t>(written) >= sizeof(payload) - offset)
			return false;
		offset += static_cast<size_t>(written);
	}
	if (has_image) {
		payload_length = snprintf(
			payload + offset, sizeof(payload) - offset,
			"]},\"pipeline\":[{\"type\":\"remember\",\"template\":\"%s\"},"
			"{\"type\":\"deliver\",\"channel\":\"wechat\",\"template\":\"%s\"},"
			"{\"type\":\"media\",\"channel\":\"wechat\",\"path\":\"{{payload.image}}\"}],"
			"\"idempotency_key\":\"human-detect-%llu\","
			"\"dedupe\":{\"window_s\":300}}",
			kImageRememberTemplate, kImageDeliverTemplate,
			static_cast<unsigned long long>(*event_epoch_s));
	} else {
		payload_length = snprintf(
			payload + offset, sizeof(payload) - offset,
			"]},\"pipeline\":[{\"type\":\"remember\",\"template\":\"%s\"},"
			"{\"type\":\"deliver\",\"channel\":\"wechat\",\"template\":\"%s\"}],"
			"\"idempotency_key\":\"human-detect-%llu\","
			"\"dedupe\":{\"window_s\":300}}",
			kTextRememberTemplate, kTextDeliverTemplate,
			static_cast<unsigned long long>(*event_epoch_s));
	}
	if (payload_length < 0 || static_cast<size_t>(payload_length) >= sizeof(payload) - offset)
		return false;
	offset += static_cast<size_t>(payload_length);
	request_length = snprintf(
		request, sizeof(request),
		"POST /v1/tasks HTTP/1.1\r\nHost: local\r\n"
		"Content-Type: application/json\r\nContent-Length: %zu\r\n"
		"Connection: close\r\n\r\n%s",
		offset, payload);
	if (request_length < 0 || static_cast<size_t>(request_length) >= sizeof(request))
		return false;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
	    fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		close(fd);
		return false;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, config->task_socket_path);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
		int connect_error;
		socklen_t error_length = sizeof(connect_error);

		if (errno != EINPROGRESS || !wait_for_fd(fd, POLLOUT, kTaskConnectTimeoutMs) ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error, &error_length) < 0 ||
		    connect_error != 0) {
			close(fd);
			return false;
		}
	}
	if (!send_all_local(fd, request, static_cast<size_t>(request_length))) {
		close(fd);
		return false;
	}
	if (!wait_for_fd(fd, POLLIN, kTaskResponseTimeoutMs)) {
		close(fd);
		return false;
	}
	ssize_t received = recv(fd, response, sizeof(response), 0);
	close(fd);
	return received >= 12 && !memcmp(response, "HTTP/1.1 202", 12);
}

int process_detection_frame(Camera *camera, ServiceState *state, FILE *json,
			    PreviewClient *preview, const Config *config)
{
	struct v4l2_buffer buffer;
	struct human_detect_result result = {};
	const size_t stride = camera->format.fmt.pix.bytesperline;
	const size_t needed = stride * kDetectHeight;
	uint64_t started;
	uint64_t prepared;
	uint64_t invoked;
	int ready;

	ready = camera_dequeue(camera, &buffer);
	if (ready <= 0)
		return ready;

	if ((buffer.flags & V4L2_BUF_FLAG_ERROR) || !camera->mapping ||
	    stride < kDetectWidth || buffer.bytesused < needed) {
		/* The sensor commonly emits a few short frames immediately after
		 * STREAMON while its first VSYNC boundary settles. */
		if ((buffer.flags & V4L2_BUF_FLAG_ERROR) &&
		    state->camera_stream_synced)
			state->camera_errors++;
		return camera_queue(camera, &buffer) < 0 ? -1 : 0;
	}
	state->camera_stream_synced = true;

	started = monotonic_us();
	for (int row = 0; row < kDetectHeight; ++row) {
		const uint8_t *source =
			static_cast<const uint8_t *>(camera->mapping) + row * stride;

		memcpy(state->gray_frame + row * kDetectWidth, source,
		       kDetectWidth);
	}
	/* Return the single DMA buffer before inference. The model and optional
	 * preview use the private, tightly packed copy above. */
	if (camera_queue(camera, &buffer) < 0)
		return -1;

	if (human_detector_prepare_gray8(state->gray_frame, kDetectWidth) < 0) {
		if (state->inference_errors++ == 0)
			log_msg("detector input preparation failed");
		return 1;
	}
	prepared = monotonic_us();

	if (human_detector_invoke(&result) < 0) {
		if (state->inference_errors++ == 0)
			log_msg("detector inference failed: %s",
				human_detector_error_string());
		return 1;
	}
	invoked = monotonic_us();

	bool changed = detection_changed(state->result, result);
	bool present = result.count != 0;

	state->frames++;
	state->have_result = true;
	state->result = result;
	state->last_frame_ms = monotonic_ms();
	if (!state->rate_window_started_ms) {
		state->rate_window_started_ms = state->last_frame_ms;
		state->rate_window_frames = state->frames;
	} else {
		uint64_t elapsed = state->last_frame_ms - state->rate_window_started_ms;

		if (elapsed >= 2000) {
			uint64_t completed = state->frames - state->rate_window_frames;

			state->inference_fps_x100 = static_cast<uint32_t>(
				(completed * 100000 + elapsed / 2) / elapsed);
			state->rate_window_started_ms = state->last_frame_ms;
			state->rate_window_frames = state->frames;
		}
	}
	state->prepare_us = static_cast<uint32_t>(prepared - started);
	state->inference_us = static_cast<uint32_t>(invoked - prepared);
	if (present != state->human_present) {
		state->human_present = present;
		state->last_human_change_ms = state->last_frame_ms;
		if (!present) {
			state->task_pending = false;
			state->event_image_ready = false;
		} else if (!state->last_task_submit_ms ||
			   state->last_frame_ms - state->last_task_submit_ms >=
				   kTaskDedupeWindowMs) {
			state->task_event_epoch_s = 0;
			state->event_image_ready = false;
			state->task_pending = true;
			state->next_task_submit_ms = state->last_frame_ms;
		}
		changed = true;
	}
	if (state->task_pending && state->last_frame_ms >= state->next_task_submit_ms) {
		if (!state->event_image_ready) {
			state->event_image_ready = save_event_snapshot(
				camera, state, preview, config->event_image_path);
			log_msg(state->event_image_ready ?
					"event color JPEG saved to %s" :
					"event color JPEG unavailable; sending detector result only",
				config->event_image_path);
		}
		const char *image_path = state->event_image_ready ?
			config->event_image_path : nullptr;
		if (submit_human_task(config, result, &state->task_event_epoch_s,
				      image_path)) {
			state->task_pending = false;
			state->last_task_submit_ms = monotonic_ms();
			state->task_submissions++;
			log_msg("human event submitted to WeniClaw (%s)",
				state->event_image_ready ? "with image" : "text only");
		} else {
			state->task_submit_errors++;
			state->next_task_submit_ms = state->last_frame_ms + kTaskRetryMs;
			if (state->task_submit_errors == 1 || state->task_submit_errors % 10 == 0)
				log_msg("WeniClaw task socket unavailable; retrying in %llu seconds",
					static_cast<unsigned long long>(kTaskRetryMs / 1000));
		}
	}
	if (changed)
		write_detection_json(json, state);
	return 1;
}

void draw_rectangle(uint8_t *frame, int left, int top, int right, int bottom)
{
	constexpr int thickness = 2;

	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right >= kDetectWidth)
		right = kDetectWidth - 1;
	if (bottom >= kDetectHeight)
		bottom = kDetectHeight - 1;
	if (left > right || top > bottom)
		return;

	for (int y = top; y <= bottom; ++y) {
		for (int t = 0; t < thickness && left + t <= right; ++t) {
			frame[y * kDetectWidth + left + t] = 255;
			frame[y * kDetectWidth + right - t] = 255;
		}
	}
	for (int x = left; x <= right; ++x) {
		for (int t = 0; t < thickness && top + t <= bottom; ++t) {
			frame[(top + t) * kDetectWidth + x] = 255;
			frame[(bottom - t) * kDetectWidth + x] = 255;
		}
	}
}

void draw_detection_boxes(uint8_t *frame,
			  const struct human_detect_result &result)
{
	for (unsigned int i = 0; i < result.count; ++i) {
		const struct human_detect_box &box = result.boxes[i];
		int left = box.x0 * (kDetectWidth - 1) / 65535;
		int top = box.y0 * (kDetectHeight - 1) / 65535;
		int right = box.x1 * (kDetectWidth - 1) / 65535;
		int bottom = box.y1 * (kDetectHeight - 1) / 65535;

		draw_rectangle(frame, left, top, right, bottom);
	}
}

int encode_gray_jpeg(PreviewClient *preview, const uint8_t *frame)
{
	__u32 encoded = 0;

	if (n32jpeg_encode_grey8(preview->jpeg_fd, frame, kDetectWidth,
				 kDetectHeight, 70, preview->jpeg,
				 kPreviewJpegMax, &encoded) < 0)
		return -1;
	preview->jpeg_length = encoded;
	return preview->jpeg_length >= 4 && preview->jpeg[0] == 0xff &&
		       preview->jpeg[1] == 0xd8 &&
		       preview->jpeg[preview->jpeg_length - 2] == 0xff &&
		       preview->jpeg[preview->jpeg_length - 1] == 0xd9
	       ? 0
	       : -1;
}

int preview_open_jpeg(PreviewClient *preview, const uint8_t *input)
{
	struct n32jpeg_info info;
	uint32_t input_alignment;
	uint32_t output_alignment;
	int fd;

	fd = n32jpeg_open(N32JPEG_DEFAULT_DEVICE);
	if (fd < 0) {
		log_msg("cannot open %s: %s", N32JPEG_DEFAULT_DEVICE,
			strerror(errno));
		return -1;
	}
	if (n32jpeg_query(fd, &info) < 0) {
		log_msg("hardware JPEG query failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	input_alignment = info.input_alignment ? info.input_alignment : 1;
	output_alignment = info.output_alignment ? info.output_alignment : 1;
	if (info.abi_version != N32JPEG_ABI_VERSION ||
	    !(info.formats & (1U << N32JPEG_FORMAT_GREY8)) ||
	    kDetectWidth < info.min_width ||
	    kDetectWidth > info.max_width ||
	    kDetectHeight < info.min_height ||
	    kDetectHeight > info.max_height ||
	    reinterpret_cast<uintptr_t>(input) % input_alignment) {
		log_msg("hardware JPEG encoder does not support aligned %ux%u GREY",
			kDetectWidth, kDetectHeight);
		close(fd);
		return -1;
	}
	preview->jpeg = static_cast<uint8_t *>(n32jpeg_alloc(kPreviewJpegMax));
	if (!preview->jpeg) {
		log_msg("cannot allocate hardware JPEG output: %s",
			strerror(errno));
		close(fd);
		return -1;
	}
	if (reinterpret_cast<uintptr_t>(preview->jpeg) % output_alignment) {
		log_msg("hardware JPEG output requires %u-byte alignment",
			output_alignment);
		n32jpeg_free(preview->jpeg);
		preview->jpeg = nullptr;
		close(fd);
		return -1;
	}
	preview->jpeg_fd = fd;
	return 0;
}

void preview_close(PreviewClient *preview, ServiceState *state,
		   const char *reason)
{
	if (preview->fd >= 0) {
		close(preview->fd);
		preview->fd = -1;
		if (reason)
			log_msg("preview closed: %s", reason);
	}
	if (preview->jpeg_fd >= 0) {
		close(preview->jpeg_fd);
		preview->jpeg_fd = -1;
	}
	n32jpeg_free(preview->jpeg);
	preview->jpeg = nullptr;
	n32jpeg_free(preview->gray_packet);
	preview->gray_packet = nullptr;
	preview->mode = PreviewMode::None;
	preview->pending = false;
	preview->prefix_length = preview->prefix_offset = 0;
	preview->jpeg_length = preview->jpeg_offset = 0;
	preview->gray_length = preview->gray_offset = 0;
	preview->suffix_offset = 0;
	state->preview_active = false;
	state->last_preview_ms = monotonic_ms();
	if (preview->started_ms)
		state->preview_ms = static_cast<uint32_t>(
			state->last_preview_ms - preview->started_ms);
	preview->started_ms = 0;
}

bool save_event_snapshot(Camera *camera, ServiceState *state,
			 PreviewClient *preview, const char *path)
{
	struct v4l2_buffer buffer;
	const char *failure = nullptr;
	bool controls_live = true;
	bool captured = false;
	unsigned int frames = 0;
	uint32_t last_bytes = 0;
	uint32_t last_flags = 0;
	long exposure = -1;
	long gain = -1;
	long previous_exposure = -2;
	long previous_gain = -2;
	int stable = 0;
	uint64_t started = monotonic_ms();

	if (!path || !*path)
		return false;
	if (preview && preview->fd >= 0)
		preview_close(preview, state, "event snapshot");

	camera_stop(camera);
	state->camera_ready = false;
	if (camera->fd < 0) {
		failure = "camera is closed";
		goto restore;
	}
	if (camera_start(camera, V4L2_PIX_FMT_JPEG, kEventSnapshotSize.width,
			 kEventSnapshotSize.height) < 0) {
		failure = "cannot start event JPEG capture";
		goto restore;
	}

	for (int frame = 0; frame < kSnapshotMaxFrames; ++frame) {
		bool usable;
		int ready = camera_dequeue(camera, &buffer);

		if (ready <= 0) {
			failure = ready == 0 ? "timed out waiting for an event JPEG" :
					       "event JPEG capture failed";
			break;
		}
		frames++;
		last_bytes = buffer.bytesused;
		last_flags = buffer.flags;

		usable = frame >= kSnapshotMinWarmup;
		if (controls_live) {
			if (ctrl_get(camera->fd, V4L2_CID_EXPOSURE, &exposure) < 0 ||
			    (ctrl_get(camera->fd, V4L2_CID_ANALOGUE_GAIN, &gain) < 0 &&
			     ctrl_get(camera->fd, V4L2_CID_GAIN, &gain) < 0)) {
				controls_live = false;
			} else if (exposure == previous_exposure &&
				   gain == previous_gain) {
				stable++;
			} else {
				stable = 0;
				previous_exposure = exposure;
				previous_gain = gain;
			}
		}
		if (usable && controls_live &&
		    stable < kSnapshotSettleFrames &&
		    frame + 1 < kSnapshotMaxFrames)
			usable = false;

		if (usable && jpeg_is_complete(camera, buffer)) {
			captured = write_file_atomic(path, camera->mapping,
						     buffer.bytesused);
			if (!captured)
				failure = "cannot write event JPEG";
			(void)camera_queue(camera, &buffer);
			break;
		}
		if (camera_queue(camera, &buffer) < 0) {
			failure = "cannot requeue event JPEG buffer";
			break;
		}
	}

	if (!captured && !failure) {
		if (!frames)
			failure = "camera produced no event JPEG frames";
		else
			failure = "no complete event JPEG after AEC warm-up";
	}

restore:
	camera_stop(camera);
	if (camera_start_grey(camera) == 0) {
		state->camera_ready = true;
		state->camera_stream_synced = false;
	} else {
		log_msg("event snapshot: failed to restore GREY detection");
		camera_close(camera);
		state->camera_errors++;
	}
	state->last_snapshot_ms = monotonic_ms();
	state->snapshot_ms = static_cast<uint32_t>(state->last_snapshot_ms -
						    started);
	if (captured) {
		state->snapshots++;
		return true;
	}

	state->camera_errors++;
	log_msg("event snapshot failed: %s (frames=%u last_bytes=%u flags=0x%x)",
		failure ? failure : "unknown error", frames, last_bytes, last_flags);
	return false;
}

bool preview_begin(PreviewClient *preview, ServiceState *state, int client,
		   PreviewMode mode, unsigned int fps, unsigned int seconds,
		   uint64_t now)
{
	static const char mjpeg_headers[] =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
		"Cache-Control: no-store\r\n"
		"Pragma: no-cache\r\n"
		"Connection: close\r\n\r\n";
	static const char gray8_headers[] =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/x-n32h787-gray8\r\n"
		"Cache-Control: no-store\r\n"
		"Pragma: no-cache\r\n"
		"Connection: close\r\n\r\n";
	const char *headers;
	size_t headers_length;

	if (preview->fd >= 0) {
		send_http_text(client, 409, "Conflict",
			       "a preview client is already connected\n");
		return false;
	}
	if (mode == PreviewMode::Mjpeg &&
	    !preview->jpeg && preview_open_jpeg(preview, state->gray_frame) < 0) {
		send_http_text(client, 503, "Service Unavailable",
			       "hardware JPEG is unavailable\n");
		return false;
	}
	if (mode == PreviewMode::Gray8 && !preview->gray_packet) {
		preview->gray_packet = static_cast<uint8_t *>(
			n32jpeg_alloc(kPreviewGrayBytes));
		if (!preview->gray_packet) {
			send_http_text(client, 503, "Service Unavailable",
				       "cannot allocate gray8 preview buffer\n");
			return false;
		}
	}
	preview->mode = mode;
	if (mode == PreviewMode::Gray8) {
		headers = gray8_headers;
		headers_length = sizeof(gray8_headers) - 1;
	} else {
		headers = mjpeg_headers;
		headers_length = sizeof(mjpeg_headers) - 1;
	}
	if (send_all(client, headers, headers_length) < 0) {
		preview_close(preview, state, nullptr);
		return false;
	}

	preview->fd = client;
	preview->started_ms = now;
	preview->fps = fps;
	preview->deadline_ms = seconds ?
		now + static_cast<uint64_t>(seconds) * 1000 : UINT64_MAX;
	preview->next_frame_ms = now;
	preview->last_io_ms = now;
	preview->pending = false;
	preview->prefix_length = preview->prefix_offset = 0;
	preview->jpeg_length = preview->jpeg_offset = 0;
	preview->suffix_offset = 0;
	state->preview_active = true;
	state->previews++;
	if (seconds)
		log_msg("%s preview started at %ux%u GREY, %u fps for %u s",
			mode == PreviewMode::Gray8 ? "gray8" : "MJPEG",
			kDetectWidth, kDetectHeight, fps, seconds);
	else
		log_msg("%s preview started at %ux%u GREY, %u fps until disconnect",
			mode == PreviewMode::Gray8 ? "gray8" : "MJPEG",
			kDetectWidth, kDetectHeight, fps);
	return true;
}

int send_preview_bytes(PreviewClient *preview, const uint8_t *data,
		       size_t length, size_t *offset)
{
	while (*offset < length) {
		ssize_t sent = send(preview->fd, data + *offset,
				    length - *offset, 0);

		if (sent > 0) {
			*offset += static_cast<size_t>(sent);
			preview->last_io_ms = monotonic_ms();
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
	return 1;
}

int preview_flush(PreviewClient *preview)
{
	static const uint8_t suffix[] = { '\r', '\n' };
	int result;

	if (!preview->pending)
		return 1;
	if (preview->mode == PreviewMode::Gray8) {
		result = send_preview_bytes(preview, preview->gray_packet,
					    preview->gray_length,
					    &preview->gray_offset);
		if (result <= 0)
			return result;
		preview->pending = false;
		return 1;
	}
	result = send_preview_bytes(
		preview, reinterpret_cast<const uint8_t *>(preview->prefix),
		preview->prefix_length, &preview->prefix_offset);
	if (result <= 0)
		return result;
	result = send_preview_bytes(preview, preview->jpeg, preview->jpeg_length,
				    &preview->jpeg_offset);
	if (result <= 0)
		return result;
	result = send_preview_bytes(preview, suffix, sizeof(suffix),
				    &preview->suffix_offset);
	if (result <= 0)
		return result;
	preview->pending = false;
	return 1;
}

int preview_tick(PreviewClient *preview, ServiceState *state)
{
	uint64_t now = monotonic_ms();
	struct human_detect_gray_header *header;
	uint8_t *pixels;
	int result;

	if (preview->fd < 0)
		return 0;
	if (!state->camera_ready) {
		preview_close(preview, state, "camera unavailable");
		return 0;
	}
	if (preview->deadline_ms != UINT64_MAX && now >= preview->deadline_ms) {
		preview_close(preview, state, "duration expired");
		return 0;
	}
	if (now - preview->last_io_ms >= kPreviewWriteTimeoutMs) {
		state->preview_errors++;
		preview_close(preview, state, "slow client timeout");
		return 0;
	}

	result = preview_flush(preview);
	if (result < 0) {
		state->preview_errors++;
		preview_close(preview, state, "client disconnected");
		return 0;
	}
	if (preview->pending || now < preview->next_frame_ms ||
	    !state->have_result)
		return 0;

	if (preview->mode == PreviewMode::Gray8) {
		header = reinterpret_cast<struct human_detect_gray_header *>(
			preview->gray_packet);
		pixels = preview->gray_packet + HUMAN_DETECT_GRAY_HEADER_SIZE;
		memset(header, 0, sizeof(*header));
		header->magic = HUMAN_DETECT_GRAY_MAGIC;
		header->version = HUMAN_DETECT_GRAY_VERSION;
		header->header_size = HUMAN_DETECT_GRAY_HEADER_SIZE;
		header->width = kDetectWidth;
		header->height = kDetectHeight;
		header->stride = kDetectWidth;
		header->sequence = state->preview_frames;
		header->frames = state->frames;
		header->camera_errors = state->camera_errors > UINT32_MAX ?
			UINT32_MAX : static_cast<uint32_t>(state->camera_errors);
		header->inference_errors = state->inference_errors > UINT32_MAX ?
			UINT32_MAX : static_cast<uint32_t>(state->inference_errors);
		header->inference_fps_x100 = state->inference_fps_x100;
		header->prepare_us = state->prepare_us;
		header->inference_us = state->inference_us;
		header->frame_age_ms = state->last_frame_ms && now >= state->last_frame_ms ?
			static_cast<uint32_t>(now - state->last_frame_ms) : 0;
		header->human_present = state->human_present ? 1 : 0;
		header->box_count = state->result.count > HUMAN_DETECT_PREVIEW_BOXES ?
			HUMAN_DETECT_PREVIEW_BOXES : state->result.count;
		header->camera_ready = state->camera_ready ? 1 : 0;
		for (unsigned int i = 0; i < header->box_count; ++i) {
			header->boxes[i].x0 = state->result.boxes[i].x0;
			header->boxes[i].y0 = state->result.boxes[i].y0;
			header->boxes[i].x1 = state->result.boxes[i].x1;
			header->boxes[i].y1 = state->result.boxes[i].y1;
			header->boxes[i].score = state->result.boxes[i].score;
		}
		memcpy(pixels, state->gray_frame, kDetectWidth * kDetectHeight);
		preview->gray_length = kPreviewGrayBytes;
		preview->gray_offset = 0;
	} else {
		int written;

		draw_detection_boxes(state->gray_frame, state->result);
		if (encode_gray_jpeg(preview, state->gray_frame) < 0) {
			state->preview_errors++;
			log_msg("hardware JPEG preview encode failed: %s",
				strerror(errno));
			preview_close(preview, state, "hardware JPEG encode failed");
			return 0;
		}

		written = snprintf(preview->prefix, sizeof(preview->prefix),
				   "--frame\r\nContent-Type: image/jpeg\r\n"
				   "Content-Length: %zu\r\n\r\n",
				   preview->jpeg_length);
		if (written < 0 ||
		    static_cast<size_t>(written) >= sizeof(preview->prefix)) {
			state->preview_errors++;
			preview_close(preview, state, "frame header overflow");
			return -1;
		}
		preview->prefix_length = static_cast<size_t>(written);
		preview->prefix_offset = 0;
		preview->jpeg_offset = 0;
		preview->suffix_offset = 0;
	}
	preview->pending = true;
	preview->next_frame_ms = now + (1000 + preview->fps / 2) / preview->fps;
	state->preview_frames++;

	result = preview_flush(preview);
	if (result < 0) {
		state->preview_errors++;
		preview_close(preview, state, "client disconnected");
	}
	return 0;
}

void send_snapshot_response(int client, const Camera *camera,
			    const struct v4l2_buffer &buffer)
{
	if (send_http_headers(client, 200, "OK", "image/jpeg",
			      buffer.bytesused) == 0)
		send_all(client, camera->mapping, buffer.bytesused);
}

void handle_snapshot(int client, const Config &config, Camera *camera,
		     ServiceState *state, const ImageSize &size)
{
	struct v4l2_buffer buffer;
	const char *failure = nullptr;
	bool controls_live = true;
	bool sent = false;
	unsigned int frames = 0;
	uint32_t last_bytes = 0;
	uint32_t last_flags = 0;
	long exposure = -1;
	long gain = -1;
	long previous_exposure = -2;
	long previous_gain = -2;
	int stable = 0;
	uint64_t started = monotonic_ms();

	camera_stop(camera);
	state->camera_ready = false;
	if (camera->fd < 0 && camera_open(camera, config.device) < 0) {
		failure = "cannot open camera";
		goto failed;
	}
	if (camera_start(camera, V4L2_PIX_FMT_JPEG, size.width, size.height) < 0) {
		failure = "cannot start JPEG capture";
		goto failed;
	}

	for (int frame = 0; frame < kSnapshotMaxFrames; ++frame) {
		bool usable;
		int ready = camera_dequeue(camera, &buffer);

		if (ready <= 0) {
			failure = ready == 0 ? "timed out waiting for a JPEG frame" :
					       "JPEG capture failed";
			break;
		}
		frames++;
		last_bytes = buffer.bytesused;
		last_flags = buffer.flags;

		usable = frame >= kSnapshotMinWarmup;
		if (controls_live) {
			if (ctrl_get(camera->fd, V4L2_CID_EXPOSURE, &exposure) < 0 ||
			    (ctrl_get(camera->fd, V4L2_CID_ANALOGUE_GAIN, &gain) < 0 &&
			     ctrl_get(camera->fd, V4L2_CID_GAIN, &gain) < 0)) {
				controls_live = false;
			} else if (exposure == previous_exposure &&
				   gain == previous_gain) {
				stable++;
			} else {
				stable = 0;
				previous_exposure = exposure;
				previous_gain = gain;
			}
		}
		if (usable && controls_live &&
		    stable < kSnapshotSettleFrames &&
		    frame + 1 < kSnapshotMaxFrames)
			usable = false;

		if (usable && jpeg_is_complete(camera, buffer)) {
			send_snapshot_response(client, camera, buffer);
			camera_queue(camera, &buffer);
			sent = true;
			break;
		}
		camera_queue(camera, &buffer);
	}

	if (!sent && !failure) {
		if (!frames)
			failure = "camera produced no JPEG frames";
		else
			failure = "no complete JPEG frame after AEC warm-up";
	}

	camera_stop(camera);
	if (camera_start_grey(camera) == 0) {
		state->camera_ready = true;
		state->camera_stream_synced = false;
	} else {
		log_msg("snapshot %ux%u: failed to restore GREY detection",
			size.width, size.height);
		camera_close(camera);
		state->camera_errors++;
	}
	state->last_snapshot_ms = monotonic_ms();
	state->snapshot_ms = static_cast<uint32_t>(state->last_snapshot_ms -
						    started);

	if (sent) {
		state->snapshots++;
		return;
	}

	state->camera_errors++;
	log_msg("snapshot %ux%u failed: %s (frames=%u last_bytes=%u flags=0x%x)",
		size.width, size.height, failure, frames, last_bytes, last_flags);
	send_http_text(client, 503, "Service Unavailable",
		       state->camera_ready ? failure :
					     "camera failed while taking a snapshot\n");
	return;

failed:
	camera_close(camera);
	state->camera_ready = false;
	state->camera_errors++;
	state->last_snapshot_ms = monotonic_ms();
	state->snapshot_ms = static_cast<uint32_t>(state->last_snapshot_ms -
						    started);
	log_msg("snapshot %ux%u failed: %s", size.width, size.height, failure);
	send_http_text(client, 503, "Service Unavailable", failure);
}

bool handle_http_client(int client, const Config &config, Camera *camera,
			ServiceState *state, PreviewClient *preview,
			bool local_only)
{
	HttpRequest request;
	char *query;
	ImageSize size = kDefaultSnapshot;

	if (!read_http_request(client, &request)) {
		send_http_text(client, 400, "Bad Request", "bad HTTP request\n");
		return false;
	}
	if (strcmp(request.method, "GET")) {
		send_http_text(client, 405, "Method Not Allowed",
			       "only GET is supported\n");
		return false;
	}

	query = strchr(request.target, '?');
	if (query)
		*query++ = '\0';

	if (!strcmp(request.target, "/") || !strcmp(request.target, "/dashboard")) {
		const size_t length = sizeof(kDashboardHtml) - 1;

		if (send_http_headers(client, 200, "OK",
				      "text/html; charset=utf-8", length) == 0)
			send_all(client, kDashboardHtml, length);
		return false;
	}
	if (!strcmp(request.target, "/help")) {
		char help[640];
		int length = snprintf(
			help, sizeof(help),
			"human-detect position detection service\n"
			"GET /                              HTML dashboard\n"
			"GET /status                         detector JSON\n"
			"GET /preview.mjpg[?fps=3..5&duration=1..60]\n"
			"GET /snapshot.jpg[?size=WxH]        color JPEG\n"
			"preview: 320x240 GREY with boxes, default 5 fps until disconnect\n"
			"snapshot sizes: 160x120 176x144 320x240 640x480 720x480 "
			"720x576 1024x768 1280x720 1920x1080 2592x1944\n");

		if (length < 0 || static_cast<size_t>(length) >= sizeof(help)) {
			send_http_text(client, 500, "Internal Server Error",
				       "help serialization failed\n");
		} else if (send_http_headers(client, 200, "OK",
					     "text/plain; charset=utf-8",
					     static_cast<size_t>(length)) == 0) {
			send_all(client, help, static_cast<size_t>(length));
		}
		return false;
	}
	if (!strcmp(request.target, "/status")) {
		char status[1800];
		size_t length = build_status_json(status, sizeof(status), state);

		if (!length ||
		    send_http_headers(client, 200, "OK",
				      "application/json", length) < 0)
			send_http_text(client, 500, "Internal Server Error",
				       "status serialization failed\n");
		else
			send_all(client, status, length);
		return false;
	}
	if (!strcmp(request.target, "/preview.mjpg") ||
	    !strcmp(request.target, "/preview")) {
		unsigned int fps = kPreviewDefaultFps;
		unsigned int seconds = kPreviewDefaultSeconds;

		if (!parse_image_options(query, kPreviewSize, kPreviewSizes,
					 &size, &fps, &seconds)) {
			send_http_text(client, 400, "Bad Request",
				       "preview is fixed at 320x240; fps must be 3..5\n");
			return false;
		}
		if (!state->camera_ready) {
			send_http_text(client, 503, "Service Unavailable",
				       "camera is unavailable\n");
			return false;
		}
		if (preview->fd >= 0) {
			send_http_text(client, 409, "Conflict",
				       "a preview client is already connected\n");
			return false;
		}
		return preview_begin(preview, state, client, PreviewMode::Mjpeg,
				     fps, seconds,
				     monotonic_ms());
	}
	if (!strcmp(request.target, "/preview.gray8")) {
		unsigned int fps = kPreviewDefaultFps;
		unsigned int seconds = kPreviewDefaultSeconds;

		if (!local_only) {
			send_http_text(client, 403, "Forbidden",
				       "gray8 preview is local-only\n");
			return false;
		}
		if (!parse_image_options(query, kPreviewSize, kPreviewSizes,
					 &size, &fps, &seconds)) {
			send_http_text(client, 400, "Bad Request",
				       "preview is fixed at 320x240; fps must be 3..5\n");
			return false;
		}
		if (!state->camera_ready) {
			send_http_text(client, 503, "Service Unavailable",
				       "camera is unavailable\n");
			return false;
		}
		if (preview->fd >= 0) {
			send_http_text(client, 409, "Conflict",
				       "a preview client is already connected\n");
			return false;
		}
		return preview_begin(preview, state, client, PreviewMode::Gray8,
				     fps, seconds, monotonic_ms());
	}
	if (strcmp(request.target, "/snapshot.jpg") &&
	    strcmp(request.target, "/snapshot")) {
		send_http_text(client, 404, "Not Found", "unknown endpoint\n");
		return false;
	}
	if (!parse_image_options(query, kDefaultSnapshot, kSnapshotSizes, &size,
				 nullptr, nullptr)) {
		send_http_text(client, 400, "Bad Request",
			       "unsupported image size\n");
		return false;
	}
	if (preview->fd >= 0) {
		send_http_text(client, 409, "Conflict",
			       "stop the MJPEG preview before taking a snapshot\n");
		return false;
	}
	if (!state->camera_ready) {
		send_http_text(client, 503, "Service Unavailable",
			       "camera is unavailable\n");
		return false;
	}

	handle_snapshot(client, config, camera, state, size);
	return false;
}

int socket_is_live(const char *path)
{
	struct sockaddr_un address;
	int fd;
	int flags;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&address),
		    sizeof(address)) == 0) {
		close(fd);
		return 1;
	}
	if (errno == ECONNREFUSED || errno == ENOENT) {
		close(fd);
		return 0;
	}
	close(fd);
	return 1;
}

int create_unix_socket(const char *path)
{
	struct sockaddr_un address;
	struct stat status;
	int fd;

	if (strlen(path) >= sizeof(address.sun_path)) {
		log_msg("socket path is too long: %s", path);
		return -1;
	}
	if (lstat(path, &status) == 0) {
		if (!S_ISSOCK(status.st_mode)) {
			log_msg("refusing to replace non-socket path %s", path);
			return -1;
		}
		if (socket_is_live(path) != 0) {
			log_msg("another instance is already serving %s", path);
			return -1;
		}
		if (unlink(path) < 0) {
			log_msg("cannot remove stale socket %s: %s", path,
				strerror(errno));
			return -1;
		}
	} else if (errno != ENOENT) {
		log_msg("cannot inspect %s: %s", path, strerror(errno));
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		log_msg("socket: %s", strerror(errno));
		return -1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (bind(fd, reinterpret_cast<struct sockaddr *>(&address),
		 sizeof(address)) < 0) {
		log_msg("bind %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (chmod(path, 0660) < 0)
		log_msg("chmod %s: %s", path, strerror(errno));
	if (listen(fd, 4) < 0) {
		log_msg("listen %s: %s", path, strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	return fd;
}

int create_tcp_socket(uint16_t port)
{
	struct sockaddr_in address;
	int enabled = 1;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		log_msg("TCP socket: %s", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	if (bind(fd, reinterpret_cast<struct sockaddr *>(&address),
		 sizeof(address)) < 0) {
		log_msg("TCP bind port %u: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 4) < 0) {
		log_msg("TCP listen port %u: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  --device PATH      V4L2 camera (default /dev/video0)\n"
		"  --socket PATH      Unix socket (default %s)\n"
		"  --task-socket PATH WeniClaw control socket (default %s)\n"
		"  --http-port PORT   also serve HTTP on 0.0.0.0:PORT\n"
		"  --threshold N      objectness threshold 0..255 (default 102)\n"
		"  --json FILE        append JSON when the detection changes\n"
		"  -h, --help         show this help\n",
		program, kDefaultSocket, kDefaultTaskSocket);
}

bool parse_options(int argc, char **argv, Config *config)
{
	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(argv[0]);
			exit(0);
		} else if (!strcmp(arg, "--device") && i + 1 < argc) {
			config->device = argv[++i];
		} else if (!strcmp(arg, "--socket") && i + 1 < argc) {
			config->socket_path = argv[++i];
		} else if (!strcmp(arg, "--task-socket") && i + 1 < argc) {
			config->task_socket_path = argv[++i];
		} else if (!strcmp(arg, "--http-port") && i + 1 < argc) {
			char *end = nullptr;
			unsigned long value = strtoul(argv[++i], &end, 10);

			if (!end || *end || value == 0 || value > 65535) {
				log_msg("http port must be 1..65535");
				return false;
			}
			config->http_port = static_cast<uint16_t>(value);
		} else if (!strcmp(arg, "--json") && i + 1 < argc) {
			config->json_path = argv[++i];
		} else if (!strcmp(arg, "--threshold") && i + 1 < argc) {
			char *end = nullptr;
			unsigned long value = strtoul(argv[++i], &end, 10);

			if (!end || *end || value > 255) {
				log_msg("threshold must be 0..255");
				return false;
			}
			config->threshold = static_cast<unsigned int>(value);
		} else {
			log_msg("unknown or incomplete option: %s", arg);
			return false;
		}
	}
	return true;
}

bool accept_http_client(int listener, const Config &config, Camera *camera,
			ServiceState *state, PreviewClient *preview,
			bool local_only)
{
	int client = accept(listener, nullptr, nullptr);
	int flags;
	bool adopted;

	if (client < 0)
		return false;
	flags = fcntl(client, F_GETFL, 0);
	if (flags >= 0)
		fcntl(client, F_SETFL, flags | O_NONBLOCK);
	adopted = handle_http_client(client, config, camera, state, preview,
				     local_only);
	if (!adopted)
		close(client);
	return adopted;
}

} // namespace

int main(int argc, char **argv)
{
	static ServiceState state;
	Config config;
	Camera camera;
	PreviewClient preview;
	FILE *json = nullptr;
	struct sigaction action;
	int unix_listener;
	int tcp_listener = -1;
	uint64_t retry_at = 0;
	uint64_t camera_progress_at = monotonic_ms();

	if (!parse_options(argc, argv, &config)) {
		usage(argv[0]);
		return 2;
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);
	signal(SIGPIPE, SIG_IGN);

	if (human_detector_set_threshold(config.threshold) < 0 ||
	    human_detector_init() < 0) {
		log_msg("detector initialization failed: %s",
			human_detector_error_string());
		return 1;
	}
	log_msg("position detector ready, tensor arena used: %zu bytes",
		human_detector_arena_bytes());

	if (config.json_path) {
		json = fopen(config.json_path, "a");
		if (!json) {
			log_msg("cannot append %s: %s", config.json_path,
				strerror(errno));
			return 1;
		}
		setvbuf(json, nullptr, _IOLBF, 0);
	}

	unix_listener = create_unix_socket(config.socket_path);
	if (unix_listener < 0)
		return 1;
	if (config.http_port) {
		tcp_listener = create_tcp_socket(config.http_port);
		if (tcp_listener < 0) {
			close(unix_listener);
			unlink(config.socket_path);
			return 1;
		}
	}

	if (camera_open(&camera, config.device) == 0 &&
	    camera_start_grey(&camera) == 0) {
		state.camera_ready = true;
		camera_progress_at = monotonic_ms();
		log_msg("camera ready on %s, detection %dx%d GREY", config.device,
			kDetectWidth, kDetectHeight);
	} else {
		camera_close(&camera);
		retry_at = monotonic_ms() + 1000;
		log_msg("camera unavailable; will retry on %s", config.device);
	}
	log_msg("serving Unix socket %s", config.socket_path);
	if (tcp_listener >= 0)
		log_msg("serving HTTP on 0.0.0.0:%u", config.http_port);

	while (!g_stop) {
		struct pollfd descriptors[4];
		int unix_index;
		int tcp_index = -1;
		int camera_index = -1;
		int preview_index = -1;
		int count = 0;
		uint64_t now = monotonic_ms();
		int timeout = preview.fd >= 0 ? 100 : 1000;
		int ready;

		if (state.camera_ready &&
		    now - camera_progress_at >= kCameraStallMs) {
			log_msg("camera produced no frame for %llu ms; reopening %s",
				static_cast<unsigned long long>(kCameraStallMs),
				config.device);
			state.camera_errors++;
			preview_close(&preview, &state, "camera stalled");
			camera_close(&camera);
			state.camera_ready = false;
			retry_at = now + 1000;
		}
		if (!state.camera_ready && now >= retry_at) {
			if (camera_recover_grey(&camera, config.device) == 0) {
				state.camera_ready = true;
					state.camera_stream_synced = false;
				camera_progress_at = monotonic_ms();
				log_msg("camera recovered");
			} else {
				retry_at = now + 1000;
			}
		}

		unix_index = count;
		descriptors[count].fd = unix_listener;
		descriptors[count].events = POLLIN;
		descriptors[count++].revents = 0;
		if (tcp_listener >= 0) {
			tcp_index = count;
			descriptors[count].fd = tcp_listener;
			descriptors[count].events = POLLIN;
			descriptors[count++].revents = 0;
		}
		if (state.camera_ready) {
			camera_index = count;
			descriptors[count].fd = camera.fd;
			descriptors[count].events = POLLIN;
			descriptors[count++].revents = 0;
		}
		if (preview.fd >= 0) {
			preview_index = count;
			descriptors[count].fd = preview.fd;
			descriptors[count].events = POLLOUT;
			descriptors[count++].revents = 0;
		}

		ready = poll(descriptors, count, timeout);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			log_msg("poll: %s", strerror(errno));
			break;
		}

		if (camera_index >= 0 &&
		    descriptors[camera_index].revents &
			(POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
			int processed =
						process_detection_frame(&camera, &state, json,
									&preview, &config);

			if (processed < 0) {
				log_msg("camera capture failed; reopening %s",
					config.device);
				state.camera_errors++;
				preview_close(&preview, &state,
					      "camera capture failed");
				camera_close(&camera);
				state.camera_ready = false;
				retry_at = monotonic_ms() + 1000;
			} else if (processed > 0) {
				camera_progress_at = monotonic_ms();
			}
		}

		if (preview_index >= 0 &&
		    descriptors[preview_index].revents &
			(POLLERR | POLLHUP | POLLNVAL)) {
			state.preview_errors++;
			preview_close(&preview, &state, "client disconnected");
		}

		if (descriptors[unix_index].revents & POLLIN)
			accept_http_client(unix_listener, config, &camera, &state,
					   &preview, true);
		if (tcp_index >= 0 && descriptors[tcp_index].revents & POLLIN) {
			uint64_t snapshot_before = state.last_snapshot_ms;

			accept_http_client(tcp_listener, config, &camera, &state,
						   &preview, false);
			if (state.camera_ready &&
			    state.last_snapshot_ms != snapshot_before)
				camera_progress_at = monotonic_ms();
		}

		preview_tick(&preview, &state);
	}

	preview_close(&preview, &state, nullptr);
	camera_close(&camera);
	if (tcp_listener >= 0)
		close(tcp_listener);
	close(unix_listener);
	unlink(config.socket_path);
	if (json)
		fclose(json);
	log_msg("stopped");
	return 0;
}
