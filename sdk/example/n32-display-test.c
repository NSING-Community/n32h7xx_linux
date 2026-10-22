// SPDX-License-Identifier: BSD-3-Clause
/* Board-side framebuffer, GPU, PRIME scanout and page-flip checks.
 * No libdrm dependency.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <drm/n32h7xx_drm.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int expect_invalid(int fd, struct drm_n32h7xx_fill *fill)
{
	if (ioctl(fd, DRM_IOCTL_N32H7XX_FILL, fill) == -1 && errno == EINVAL)
		return 0;
	fprintf(stderr, "GPU accepted an invalid rectangle or returned wrong errno\n");
	return -1;
}

/*
 * Report which step failed: "the display is off" and "this program cannot
 * talk to the display driver" need very different answers from the user.
 */
static int crtc_state(int fd, uint32_t *crtc_id, uint32_t *fb_id,
		      unsigned int *width, unsigned int *height)
{
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	uint32_t crtcs[4];

	res.count_crtcs = 4;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
		fprintf(stderr, "Cannot read the display resources: %s\n",
			strerror(errno));
		return -1;
	}
	if (!res.count_crtcs) {
		fprintf(stderr, "The display driver reports no CRTC; "
			"is this the LCDC node?\n");
		return -1;
	}
	crtc.crtc_id = crtcs[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc)) {
		fprintf(stderr, "Cannot read the display mode: %s\n",
			strerror(errno));
		return -1;
	}
	if (!crtc.mode_valid || !crtc.fb_id) {
		fprintf(stderr, "The display is not in a usable mode "
			"(mode_valid %u, fb %u)\n",
			crtc.mode_valid, crtc.fb_id);
		return -1;
	}
	*crtc_id = crtc.crtc_id;
	*fb_id = crtc.fb_id;
	*width = crtc.mode.hdisplay;
	*height = crtc.mode.vdisplay;
	return 0;
}

static int read_flip_event(int fd, unsigned int *sequence)
{
	struct drm_event_vblank ev;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	ssize_t len;

	/* A flip latches within a frame; a missing event means the path broke. */
	if (poll(&pfd, 1, 2000) != 1) {
		fprintf(stderr, "Timed out waiting for a flip-complete event\n");
		return -1;
	}
	len = read(fd, &ev, sizeof(ev));
	if (len != sizeof(ev) || ev.base.type != DRM_EVENT_FLIP_COMPLETE) {
		fprintf(stderr, "No flip-complete event (read %zd)\n", len);
		return -1;
	}
	*sequence = ev.sequence;
	return 0;
}

static int flip_wait(int fd, uint32_t crtc_id, uint32_t fb_id,
		     unsigned int *sequence)
{
	struct drm_mode_crtc_page_flip flip = {
		.crtc_id = crtc_id,
		.fb_id = fb_id,
		.flags = DRM_MODE_PAGE_FLIP_EVENT,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip)) {
		perror("page flip");
		return -1;
	}
	return read_flip_event(fd, sequence);
}

/* Fill a full-size RGB565 frame with the GPU, then scan it out via the LCDC. */
static int gpu_scanout_test(int gpu, const char *node, unsigned int hold)
{
	struct drm_n32h7xx_create big = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_prime_handle prime = { 0 };
	struct drm_mode_fb_cmd2 fbc = { 0 };
	struct drm_gem_close close_bo = { 0 };
	uint32_t crtc_id, old_fb, fb_id = 0, width, height, pitch;
	unsigned int seq;
	int card, master = 0, imported = 0, ret = -1;

	card = open(node, O_RDWR | O_CLOEXEC);
	if (card < 0) {
		perror(node);
		return -1;
	}
	if (crtc_state(card, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		goto out;
	}
	pitch = width * 2;
	big.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &big)) {
		perror("GPU frame allocation");
		goto out;
	}
	fill.handle = big.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = 0x001f;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("GPU background fill");
		goto out_bo;
	}
	fill.color = 0xffff;
	fill.width = width;
	fill.height = 2;
	fill.offset = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
		goto border;
	fill.offset = (height - 2) * pitch;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
		goto border;
	fill.width = 2;
	fill.height = height;
	fill.offset = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
		goto border;
	fill.offset = pitch - 4;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
		goto border;

	prime.handle = big.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(gpu, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
		perror("PRIME export");
		goto out_bo;
	}
	if (ioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) {
		perror("PRIME import into LCDC");
		close(prime.fd);
		goto out_bo;
	}
	close(prime.fd);
	imported = 1;
	fbc.width = width;
	fbc.height = height;
	fbc.pixel_format = DRM_FORMAT_RGB565;
	fbc.handles[0] = prime.handle;
	fbc.pitches[0] = pitch;
	if (ioctl(card, DRM_IOCTL_MODE_ADDFB2, &fbc)) {
		perror("add framebuffer");
		goto out_import;
	}
	fb_id = fbc.fb_id;
	master = ioctl(card, DRM_IOCTL_SET_MASTER, 0) == 0;
	if (!master)
		fprintf(stderr, "Cannot become DRM master; flip may be rejected\n");
	if (flip_wait(card, crtc_id, fb_id, &seq))
		goto out_fb;
	printf("GPU-rendered %ux%u frame shown at vblank %u\n", width, height, seq);
	if (hold) {
		puts("Inspect LCD: blue field, white border. Restoring the");
		printf("desktop framebuffer after %u seconds.\n", hold);
		sleep(hold);
	}
	if (flip_wait(card, crtc_id, old_fb, &seq))
		goto out_fb;
	printf("GPU to LCDC PRIME scanout PASS, restored at vblank %u\n", seq);
	ret = 0;
out_fb:
	if (ret)
		flip_wait(card, crtc_id, old_fb, &seq);
	ioctl(card, DRM_IOCTL_MODE_RMFB, &fb_id);
out_import:
	if (imported) {
		close_bo.handle = prime.handle;
		ioctl(card, DRM_IOCTL_GEM_CLOSE, &close_bo);
	}
out_bo:
	close_bo.handle = big.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
out:
	if (master)
		ioctl(card, DRM_IOCTL_DROP_MASTER, 0);
	close(card);
	return ret;
border:
	perror("GPU border fill");
	goto out_bo;
}

static int gpu_test(int fd, const char *display, unsigned int hold)
{
	struct drm_n32h7xx_create create = { .size = 8192 };
	struct drm_n32h7xx_fill fill, bad;
	struct drm_gem_close close_bo = { 0 };
	struct drm_version version = { 0 };
	char name[32] = { 0 };
	uint8_t *pixels;
	int ret = -1, cpp;
	unsigned int i;

	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &version) || strcmp(name, "n32h7xx-gpu")) {
		fprintf(stderr, "Select an n32h7xx-gpu DRM node\n");
		return -1;
	}
	if (ioctl(fd, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	close_bo.handle = create.handle;
	pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fd, create.mmap_offset);
	if (pixels == MAP_FAILED) {
		perror("GPU buffer mmap");
		goto close;
	}
	for (cpp = 2; cpp <= 4; cpp += 2) {
		uint32_t color = cpp == 2 ? 0x39e7 : 0xff397be7;

		memset(pixels, 0xa5, create.size);
		memset(&fill, 0, sizeof(fill));
		fill.handle = create.handle;
		fill.format = cpp == 2 ? DRM_FORMAT_RGB565 : DRM_FORMAT_ARGB8888;
		fill.offset = 32;
		fill.pitch = 128;
		fill.width = 17;
		fill.height = 11;
		fill.color = color;
		if (ioctl(fd, DRM_IOCTL_N32H7XX_FILL, &fill)) {
			perror("GPU fill");
			goto unmap;
		}
		for (i = 0; i < create.size; i++) {
			unsigned int offset = i >= fill.offset ? i - fill.offset : ~0U;
			uint8_t expected = 0xa5;

			if (i >= fill.offset && offset / fill.pitch < fill.height &&
			    offset % fill.pitch < fill.width * cpp)
				expected = color >> (8 * ((offset % fill.pitch) % cpp));
			if (pixels[i] != expected) {
				fprintf(stderr, "GPU readback mismatch at %u: %02x != %02x\n",
					i, pixels[i], expected);
				goto unmap;
			}
		}
		bad = fill;
		bad.width = 0;
		if (expect_invalid(fd, &bad))
			goto unmap;
		bad = fill;
		bad.offset = UINT32_MAX - 3;
		if (expect_invalid(fd, &bad))
			goto unmap;
		bad = fill;
		bad.pitch = 32768;
		if (expect_invalid(fd, &bad))
			goto unmap;
		bad = fill;
		bad.reserved = 1;
		if (expect_invalid(fd, &bad))
			goto unmap;
		printf("GPU %d-bit fill: pixels, row padding and buffer guards PASS\n",
		       cpp * 8);
	}
	if (display && gpu_scanout_test(fd, display, hold))
		goto unmap;
	ret = 0;
unmap:
	munmap(pixels, create.size);
close:
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

static int wait_vblank(int fd, unsigned int sequence, unsigned int *count,
		       long *sec, long *usec)
{
	union drm_wait_vblank vbl = { 0 };

	vbl.request.type = _DRM_VBLANK_RELATIVE;
	vbl.request.sequence = sequence;
	if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl)) {
		perror("wait for vblank");
		return -1;
	}
	*count = vbl.reply.sequence;
	*sec = vbl.reply.tval_sec;
	*usec = vbl.reply.tval_usec;
	return 0;
}

/*
 * Blue field with a white border and diagonal. A wrong pitch shears the
 * diagonal and clips the border, so the picture also checks the stride.
 */
static void draw_pattern(uint16_t *base, unsigned int pitch_px,
			 unsigned int width, unsigned int height)
{
	unsigned int x, y;

	for (y = 0; y < height; y++) {
		uint16_t *row = base + (size_t)y * pitch_px;

		for (x = 0; x < width; x++)
			row[x] = 0x001f;
		row[0] = row[1] = row[width - 2] = row[width - 1] = 0xffff;
		row[(unsigned int)((uint64_t)y * (width - 1) / (height - 1))] =
			0xffff;
	}
	for (x = 0; x < width; x++)
		base[x] = 0xffff;
}

static int flip_test(int fd, unsigned int cycles, unsigned int hold)
{
	struct drm_mode_create_dumb dumb = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_fb_cmd2 fbc = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	uint32_t old_fb, crtc_id, fb_id = 0, shown;
	unsigned int count, first, seq = 0, cycle;
	long sec, usec, sec0, usec0, elapsed;
	uint8_t *mapped = MAP_FAILED;
	unsigned int width, height;
	int master, ret = -1;

	master = ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0;
	if (!master)
		fprintf(stderr, "Cannot become DRM master; flips may be rejected\n");

	if (wait_vblank(fd, 8, &count, &sec, &usec))
		goto out_master;
	first = count;
	sec0 = sec;
	usec0 = usec;
	if (wait_vblank(fd, 16, &count, &sec, &usec))
		goto out_master;
	elapsed = (sec - sec0) * 1000000L + (usec - usec0);
	if (count < first + 16 || elapsed <= 0) {
		fprintf(stderr, "Vblank did not advance: %u frames in %ld us\n",
			count - first, elapsed);
		goto out_master;
	}
	printf("Vblank: %u frames in %ld us (%.1f Hz), sequence %u -> %u PASS\n",
	       count - first, elapsed, (count - first) * 1.0e6 / elapsed,
	       first, count);

	if (crtc_state(fd, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		goto out_master;
	}
	shown = old_fb;

	dumb.width = width;
	dumb.height = height;
	dumb.bpp = 16;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) {
		perror("create dumb buffer");
		goto out_master;
	}
	if (dumb.pitch != width * 2) {
		fprintf(stderr, "Unexpected pitch %u for %ux%u RGB565\n",
			dumb.pitch, width, height);
		goto out_dumb;
	}
	map.handle = dumb.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
		perror("map dumb buffer");
		goto out_dumb;
	}
	mapped = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      map.offset);
	if (mapped == MAP_FAILED) {
		perror("dumb mmap");
		goto out_dumb;
	}
	draw_pattern((uint16_t *)mapped, dumb.pitch / 2, width, height);

	fbc.width = width;
	fbc.height = height;
	fbc.pixel_format = DRM_FORMAT_RGB565;
	fbc.handles[0] = dumb.handle;
	fbc.pitches[0] = dumb.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fbc)) {
		perror("add framebuffer");
		goto out_unmap;
	}
	fb_id = fbc.fb_id;
	for (cycle = 0; cycle < cycles; cycle++) {
		if (flip_wait(fd, crtc_id, fb_id, &seq))
			goto out_fb;
		shown = fb_id;
		if (cycle) {
			if (seq <= first)
				goto no_progress;
		} else {
			printf("Page flip to the new buffer completed at vblank %u\n",
			       seq);
			puts("Inspect LCD: blue field, white border, top-left to");
			printf("bottom-right diagonal, held for %u seconds.\n", hold);
			sleep(hold);
		}
		first = seq;
		if (flip_wait(fd, crtc_id, old_fb, &seq))
			goto out_fb;
		shown = old_fb;
		if (seq <= first) {
no_progress:
			fprintf(stderr, "Flip event did not advance past %u\n", first);
			goto out_fb;
		}
		first = seq;
	}
	printf("%u page-flip pairs completed, last event at vblank %u PASS\n",
	       cycles, seq);
	ret = 0;
out_fb:
	if (shown != old_fb && flip_wait(fd, crtc_id, old_fb, &seq))
		fprintf(stderr, "Warning: could not restore the original framebuffer\n");
	ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
out_unmap:
	if (mapped != MAP_FAILED)
		munmap(mapped, dumb.size);
out_dumb:
	destroy.handle = dumb.handle;
	ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
out_master:
	if (master)
		ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	return ret;
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Panel refresh: consecutive single-frame waits report the real cadence. */
static int perf_vblank(int fd, unsigned int frames)
{
	unsigned int i, first = 0, last = 0, skipped;
	double prev = 0, first_t = 0, last_t = 0, min = 0, max = 0, sum = 0;

	for (i = 0; i < frames; i++) {
		/* The reply overwrites the union, so re-arm every iteration. */
		union drm_wait_vblank vbl = { 0 };
		double t;

		vbl.request.type = _DRM_VBLANK_RELATIVE;
		vbl.request.sequence = 1;
		if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl)) {
			perror("wait for vblank");
			return -1;
		}
		t = (double)vbl.reply.tval_sec + (double)vbl.reply.tval_usec / 1e6;
		if (!i) {
			first = vbl.reply.sequence;
			first_t = t;
		} else {
			double delta = t - prev;

			if (i == 1 || delta < min)
				min = delta;
			if (i == 1 || delta > max)
				max = delta;
			sum += delta;
		}
		prev = t;
		last = vbl.reply.sequence;
		last_t = t;
	}
	/* Sequence numbers count real vblanks even if a wakeup was late. */
	skipped = last - first - (frames - 1);
	printf("Panel refresh: %u waits, seq %u..%u (%u vblank(s) missed while "
	       "sampling), interval min/avg/max %.3f/%.3f/%.3f ms, %.2f fps\n",
	       frames, first, last, skipped, min * 1000.0,
	       sum / (frames - 1) * 1000.0, max * 1000.0,
	       (last - first) / (last_t - first_t));
	return 0;
}

/* Maximum rate the kernel will retire legacy page flips onto the panel. */
static int perf_flip(int fd, unsigned int flips)
{
	struct drm_mode_create_dumb dumb[2] = { { 0 }, { 0 } };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_fb_cmd2 fbc = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	uint32_t crtc_id, old_fb, fb_id[2] = { 0, 0 };
	uint32_t width, height, i;
	void *map_addr[2] = { MAP_FAILED, MAP_FAILED };
	double t0, min = 0, max = 0, sum = 0;
	unsigned int seq = 0, frame, delay;
	int ret = -1;

	if (crtc_state(fd, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		return -1;
	}
	for (i = 0; i < 2; i++) {
		dumb[i].width = width;
		dumb[i].height = height;
		dumb[i].bpp = 16;
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb[i])) {
			perror("create dumb buffer");
			goto out;
		}
		map.handle = dumb[i].handle;
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
			perror("map dumb buffer");
			goto out;
		}
		map_addr[i] = mmap(NULL, dumb[i].size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd, map.offset);
		if (map_addr[i] == MAP_FAILED) {
			perror("dumb mmap");
			goto out;
		}
		memset(map_addr[i], i ? 0xf8 : 0x1f, dumb[i].size);
		fbc.width = width;
		fbc.height = height;
		fbc.pixel_format = DRM_FORMAT_RGB565;
		fbc.handles[0] = dumb[i].handle;
		fbc.pitches[0] = dumb[i].pitch;
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fbc)) {
			perror("add framebuffer");
			goto out;
		}
		fb_id[i] = fbc.fb_id;
		fbc.fb_id = 0;
	}
	t0 = now_sec();
	for (frame = 0; frame < flips; frame++) {
		double start = now_sec(), done;

		if (flip_wait(fd, crtc_id, fb_id[frame & 1], &seq))
			goto out_restore;
		done = now_sec();
		if (frame) {
			double delta = done - start;

			if (frame == 1 || delta < min)
				min = delta;
			if (frame == 1 || delta > max)
				max = delta;
			sum += delta;
		}
	}
	printf("Page flip: %u flips in %.3f s, %.2f fps, latency "
	       "min/avg/max %.3f/%.3f/%.3f ms, last vblank %u\n",
	       flips, now_sec() - t0, flips / (now_sec() - t0), min * 1000.0,
	       sum / (flips - 1) * 1000.0, max * 1000.0, seq);

	/* How late in a frame a flip can still latch at the next vblank. */
	for (delay = 0; delay <= 14; delay += 2) {
		unsigned int one = 0, two = 0, count, sample;
		long sec, usec;

		for (sample = 0; sample < 4; sample++) {
			double start, d;

			if (wait_vblank(fd, 1, &count, &sec, &usec))
				goto out_restore;
			usleep(delay * 1000);
			start = now_sec();
			if (flip_wait(fd, crtc_id, fb_id[sample & 1], &seq))
				goto out_restore;
			d = now_sec() - start;
			if (d < 0.023)
				one++;
			else
				two++;
		}
		printf("Flip %2u ms after vblank: %u/4 latched at next vblank, "
		       "%u/4 one frame late\n", delay, one, two);
	}
	ret = 0;
out_restore:
	if (flip_wait(fd, crtc_id, old_fb, &seq))
		fprintf(stderr, "Warning: could not restore the original framebuffer\n");
out:
	for (i = 0; i < 2; i++) {
		if (map_addr[i] != MAP_FAILED)
			munmap(map_addr[i], dumb[i].size);
		if (fb_id[i])
			ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id[i]);
		if (dumb[i].handle) {
			destroy.handle = dumb[i].handle;
			ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		}
	}
	return ret;
}

/* How fast the CPU can rewrite one RGB565 frame through the dumb mapping. */
static int perf_cpu_fill(int fd, unsigned int frames)
{
	struct drm_mode_create_dumb dumb = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	uint32_t crtc_id, old_fb, width, height, n;
	volatile uint32_t *pixels;
	double t0, t1;
	unsigned int i;

	if (crtc_state(fd, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		return -1;
	}
	dumb.width = width;
	dumb.height = height;
	dumb.bpp = 16;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) {
		perror("create dumb buffer");
		return -1;
	}
	map.handle = dumb.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
		perror("map dumb buffer");
		goto out;
	}
	pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      map.offset);
	if (pixels == MAP_FAILED) {
		perror("dumb mmap");
		goto out;
	}
	t0 = now_sec();
	for (i = 0; i < frames; i++)
		for (n = 0; n < dumb.size / 4; n++)
			pixels[n] = 0x001f001f;
	t1 = now_sec();
	printf("CPU fill: %ux%u RGB565, %u frames in %.3f s, %.1f MB/s, "
	       "%.1f fps\n", width, height, frames, t1 - t0,
	       frames * (double)dumb.size / 1e6 / (t1 - t0),
	       frames / (t1 - t0));
	munmap((void *)pixels, dumb.size);
out:
	destroy.handle = dumb.handle;
	ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	return 0;
}

static int perf_gpu_fill(int gpu, uint32_t width, uint32_t height,
			 unsigned int frames)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	double t0, t1;
	unsigned int i;

	create.size = (uint64_t)width * height * 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU frame allocation");
		return -1;
	}
	fill.handle = create.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = width * 2;
	fill.width = width;
	fill.height = height;
	t0 = now_sec();
	for (i = 0; i < frames; i++) {
		fill.color = 0x1f << (i % 11);
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
			perror("GPU fill");
			close_bo.handle = create.handle;
			ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
			return -1;
		}
	}
	t1 = now_sec();
	printf("GPU fill: %ux%u RGB565, %u frames in %.3f s, %.1f Mpixel/s, "
	       "%.1f fps\n", width, height, frames, t1 - t0,
	       frames * (double)width * height / 1e6 / (t1 - t0),
	       frames / (t1 - t0));
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return 0;
}

/* Full pipeline: GPU render, PRIME scanout and one flip per frame. */
static int perf_gpu_flip(int gpu, int card, unsigned int frames)
{
	struct drm_n32h7xx_create create[2] = { { 0 }, { 0 } };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_prime_handle prime = { 0 };
	struct drm_mode_fb_cmd2 fbc = { 0 };
	struct drm_gem_close close_bo = { 0 };
	uint32_t crtc_id, old_fb, fb_id[2] = { 0, 0 };
	uint32_t width, height, i;
	double t0, t1, fill_time = 0, submit_time = 0, wait_time = 0, f0;
	double fill_min = 0, fill_max = 0, last_fill = 0;
	unsigned int seq = 0, prev_seq = 0, frame, drops = 0, slow = 0;
	unsigned int drops_slow = 0, drops_fast = 0;
	int ret = -1;

	if (crtc_state(card, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		return -1;
	}
	for (i = 0; i < 2; i++) {
		create[i].size = (uint64_t)width * height * 2;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create[i])) {
			perror("GPU frame allocation");
			goto out;
		}
	}
	t0 = now_sec();
	for (frame = 0; frame < frames; frame++) {
		i = frame & 1;
		fill.handle = create[i].handle;
		fill.format = DRM_FORMAT_RGB565;
		fill.pitch = width * 2;
		fill.width = width;
		fill.height = height;
		fill.color = 0x1f << (frame % 11);
		f0 = now_sec();
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
			perror("GPU fill");
			goto out;
		}
		{
			double d = now_sec() - f0;

			fill_time += d;
			if (!frame || d < fill_min)
				fill_min = d;
			if (!frame || d > fill_max)
				fill_max = d;
			if (d > 0.0145)
				slow++;
			last_fill = d;
		}
		if (!fb_id[i]) {
			prime.handle = create[i].handle;
			prime.flags = DRM_CLOEXEC | DRM_RDWR;
			if (ioctl(gpu, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
				perror("PRIME export");
				goto out;
			}
			if (ioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) {
				perror("PRIME import");
				close(prime.fd);
				goto out;
			}
			close(prime.fd);
			fbc.width = width;
			fbc.height = height;
			fbc.pixel_format = DRM_FORMAT_RGB565;
			fbc.handles[0] = prime.handle;
			fbc.pitches[0] = width * 2;
			if (ioctl(card, DRM_IOCTL_MODE_ADDFB2, &fbc)) {
				perror("add framebuffer");
				goto out;
			}
			fb_id[i] = fbc.fb_id;
		}
		{
			struct drm_mode_crtc_page_flip flip = {
				.crtc_id = crtc_id,
				.fb_id = fb_id[i],
				.flags = DRM_MODE_PAGE_FLIP_EVENT,
			};

			f0 = now_sec();
			if (ioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &flip)) {
				perror("page flip");
				goto out;
			}
			submit_time += now_sec() - f0;
			f0 = now_sec();
			if (read_flip_event(card, &seq)) {
				fprintf(stderr, "Frame %u flip failed\n", frame);
				goto out;
			}
			wait_time += now_sec() - f0;
			if (frame && seq > prev_seq + 1) {
				unsigned int lost = seq - prev_seq - 1;

				drops += lost;
				if (last_fill > 0.012)
					drops_slow += lost;
				else
					drops_fast += lost;
			}
			prev_seq = seq;
		}
	}
	t1 = now_sec();
	printf("GPU render + PRIME scanout: %u frames in %.3f s, %.2f fps "
	       "(last vblank %u)\n", frames, t1 - t0, frames / (t1 - t0), seq);
	printf("  fill %.2f ms/frame (min %.2f, max %.2f), submit %.2f ms, "
	       "vblank wait %.2f ms, %u slow fill(s), %u frame(s) not updated "
	       "(%u after a fill over 12 ms, %u after a short one)\n",
	       fill_time / frames * 1000.0, fill_min * 1000.0,
	       fill_max * 1000.0, submit_time / frames * 1000.0,
	       wait_time / frames * 1000.0, slow, drops, drops_slow,
	       drops_fast);
	ret = 0;
out:
	if (flip_wait(card, crtc_id, old_fb, &seq))
		fprintf(stderr, "Warning: could not restore the original framebuffer\n");
	for (i = 0; i < 2; i++) {
		if (fb_id[i])
			ioctl(card, DRM_IOCTL_MODE_RMFB, &fb_id[i]);
		if (create[i].handle) {
			close_bo.handle = create[i].handle;
			ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		}
	}
	return ret;
}

static int perf_test(int fd, const char *gpu_node, unsigned int frames)
{
	struct drm_version version = { 0 };
	uint32_t crtc_id, old_fb, width, height;
	char name[32] = { 0 };
	int gpu, master;

	if (crtc_state(fd, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		return -1;
	}
	master = ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0;

	if (perf_vblank(fd, frames ? frames : 120))
		goto out;
	if (perf_flip(fd, frames ? frames : 120))
		goto out;
	perf_cpu_fill(fd, frames ? frames / 4 + 1 : 30);

	if (!gpu_node)
		goto out;
	gpu = open(gpu_node, O_RDWR | O_CLOEXEC);
	if (gpu < 0) {
		perror(gpu_node);
		goto out;
	}
	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(gpu, DRM_IOCTL_VERSION, &version) ||
	    strcmp(name, "n32h7xx-gpu")) {
		fprintf(stderr, "%s is not an n32h7xx-gpu node\n", gpu_node);
		close(gpu);
		goto out;
	}
	perf_gpu_fill(gpu, width, height, frames ? frames / 4 + 1 : 30);
	perf_gpu_flip(gpu, fd, frames ? frames : 120);
	close(gpu);
out:
	if (master)
		ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	return 0;
}

static int framebuffer_test(int fd, unsigned int hold)
{
	static const uint16_t colors[] = {
		0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0,
	};
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	uint8_t *pixels, *saved;
	size_t len;
	unsigned int x, y;

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
		perror("framebuffer information");
		return -1;
	}
	if (var.xres != 800 || var.yres != 480 || var.bits_per_pixel != 16 ||
	    var.xoffset || var.yoffset || fix.line_length < 1600 ||
	    var.red.offset != 11 || var.green.offset != 5 || var.blue.offset) {
		fprintf(stderr, "Expected an 800x480 RGB565 framebuffer\n");
		return -1;
	}
	len = (size_t)fix.line_length * var.yres;
	if (len > fix.smem_len)
		return -1;
	pixels = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		perror("framebuffer mmap");
		return -1;
	}
	saved = malloc(len);
	if (!saved) {
		munmap(pixels, fix.smem_len);
		return -1;
	}
	memcpy(saved, pixels, len);
	for (y = 0; y < var.yres; y++) {
		uint16_t *row = (uint16_t *)(pixels + y * fix.line_length);

		for (x = 0; x < var.xres; x++)
			row[x] = colors[x * 8 / var.xres];
	}
	puts("Inspect LCD bars: white yellow cyan green magenta red blue black.");
	printf("Holding the bars for %u seconds, then restoring.\n", hold);
	sleep(hold);
	memcpy(pixels, saved, len);
	free(saved);
	munmap(pixels, fix.smem_len);
	puts("Framebuffer writes completed; screen appearance requires visual confirmation.");
	return 0;
}

/* ---------------- GPU throughput matrix ---------------- */

static unsigned int compat_fails;

static void compat_check(int ok, const char *what, const char *detail)
{
	printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
	       detail && *detail ? "  " : "", detail ? detail : "");
	if (!ok)
		compat_fails++;
}

static void compat_expect_ok(const char *what, int ret)
{
	char detail[80] = "";

	if (ret)
		snprintf(detail, sizeof(detail), "-> errno %d (%s)",
			 errno, strerror(errno));
	compat_check(ret == 0, what, detail);
}

static void compat_expect_errno(const char *what, int ret, int err, int want)
{
	char detail[80] = "";

	if (ret != -1)
		snprintf(detail, sizeof(detail), "-> accepted, expected failure");
	else if (err != want)
		snprintf(detail, sizeof(detail), "-> errno %d (%s), expected %d",
			 err, strerror(err), want);
	compat_check(ret == -1 && err == want, what, detail);
}

static const char *fourcc_name(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
		return "RGB565";
	case DRM_FORMAT_XRGB8888:
		return "XRGB8888";
	case DRM_FORMAT_ARGB8888:
		return "ARGB8888";
	case DRM_FORMAT_RGBA8888:
		return "RGBA8888";
	case DRM_FORMAT_ARGB4444:
		return "ARGB4444";
	case DRM_FORMAT_RGBA4444:
		return "RGBA4444";
	case DRM_FORMAT_ARGB1555:
		return "ARGB1555";
	case DRM_FORMAT_RGBA5551:
		return "RGBA5551";
	default:
		return "unknown";
	}
}

static unsigned int fourcc_cpp(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGBA5551:
		return 2;
	default:
		return 4;
	}
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;

	return (x > y) - (x < y);
}

/*
 * One fill shape, measured call by call. The first and last tenth are reported
 * separately so a long run exposes drift that an average would hide.
 */
static int bench_case(int gpu, uint32_t format, unsigned int width,
		      unsigned int height, unsigned int pitch,
		      unsigned int iters)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int edge = iters / 10 ? iters / 10 : 1;
	double first = 0, last = 0, sum = 0, bytes;
	double *sample;
	unsigned int i;
	int ret = -1;

	sample = malloc(iters * sizeof(*sample));
	if (!sample)
		return -1;
	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		free(sample);
		return -1;
	}
	fill.handle = create.handle;
	fill.format = format;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	for (i = 0; i < iters; i++) {
		double t0;

		fill.color = 0x1f << (i % 11);
		t0 = now_sec();
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
			perror("GPU fill");
			goto out;
		}
		sample[i] = now_sec() - t0;
	}
	for (i = 0; i < edge; i++) {
		first += sample[i];
		last += sample[iters - 1 - i];
	}
	qsort(sample, iters, sizeof(*sample), cmp_double);
	for (i = 0; i < iters; i++)
		sum += sample[i];
	bytes = (double)width * height * fourcc_cpp(format);
	printf("  %-9s %4ux%-4u pitch %5u %6.3f ms avg  p50 %6.3f  p99 %6.3f"
	       "  max %7.3f  %6.1f Mpixel/s  %6.1f MB/s  first/last %5.2f/%5.2f\n",
	       fourcc_name(format), width, height, pitch, sum / iters * 1000.0,
	       sample[iters / 2] * 1000.0,
	       sample[iters - 1 - iters / 100] * 1000.0,
	       sample[iters - 1] * 1000.0,
	       (double)width * height / 1e6 / (sum / iters),
	       bytes / 1e6 / (sum / iters),
	       first / edge * 1000.0, last / edge * 1000.0);
	ret = 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	free(sample);
	return ret;
}

/*
 * Measure the same surface as bench_case, but submit it as a sequence of
 * bounded-height fills.  This distinguishes area alone from the duration of a
 * single uninterrupted GPU job, which is important while LCDC is scanning.
 */
static int bench_tiled_case(int gpu, uint32_t format, unsigned int width,
			    unsigned int height, unsigned int pitch,
			    unsigned int iters, unsigned int tile_height,
			    unsigned int gap_us)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int edge = iters / 10 ? iters / 10 : 1;
	unsigned int tiles = (height + tile_height - 1) / tile_height;
	double first = 0, last = 0, sum = 0, bytes;
	double *sample;
	unsigned int i;
	int ret = -1;

	if (!tile_height || tile_height > height)
		return -1;
	sample = malloc(iters * sizeof(*sample));
	if (!sample)
		return -1;
	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		free(sample);
		return -1;
	}
	fill.handle = create.handle;
	fill.format = format;
	fill.pitch = pitch;
	fill.width = width;
	for (i = 0; i < iters; i++) {
		unsigned int row;
		double t0 = now_sec();

		fill.color = 0x1f << (i % 11);
		for (row = 0; row < height; row += tile_height) {
			unsigned int rows = height - row;

			if (rows > tile_height)
				rows = tile_height;
			fill.offset = row * pitch;
			fill.height = rows;
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
				perror("GPU tiled fill");
				goto out;
			}
			if (gap_us && row + rows < height)
				usleep(gap_us);
		}
		sample[i] = now_sec() - t0;
	}
	for (i = 0; i < edge; i++) {
		first += sample[i];
		last += sample[iters - 1 - i];
	}
	qsort(sample, iters, sizeof(*sample), cmp_double);
	for (i = 0; i < iters; i++)
		sum += sample[i];
	bytes = (double)width * height * fourcc_cpp(format);
	printf("  %-9s %4ux%-4u tile %3u gap %4u us (%u jobs) %6.3f ms avg  p50 %6.3f"
	       "  p99 %6.3f  max %7.3f  %6.1f MB/s  first/last %5.2f/%5.2f\n",
	       fourcc_name(format), width, height, tile_height, gap_us, tiles,
	       sum / iters * 1000.0, sample[iters / 2] * 1000.0,
	       sample[iters - 1 - iters / 100] * 1000.0,
	       sample[iters - 1] * 1000.0,
	       bytes / 1e6 / (sum / iters),
	       first / edge * 1000.0, last / edge * 1000.0);
	ret = 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	free(sample);
	return ret;
}

/* Apples-to-apples CPU fill into the same coherent GEM mapping used by LVGL. */
static int bench_cpu_case(int gpu, unsigned int width, unsigned int height,
			  unsigned int iters)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_gem_close close_bo = { 0 };
	volatile uint32_t *pixels;
	uint64_t words;
	double t0, t1;
	unsigned int i;
	void *map;
	int ret = -1;

	create.size = (uint64_t)width * height * 2;
	words = create.size / sizeof(uint32_t);
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("GPU buffer mmap");
		goto out;
	}
	pixels = map;
	t0 = now_sec();
	for (i = 0; i < iters; i++) {
		uint32_t color = 0x001f001f | ((i % 11) << 21);
		uint64_t word;

		for (word = 0; word < words; word++)
			pixels[word] = color;
	}
	t1 = now_sec();
	printf("  CPU       %4ux%-4u %6.3f ms avg  %6.1f MB/s  (%u frames)\n",
	       width, height, (t1 - t0) * 1000.0 / iters,
	       iters * (double)create.size / 1e6 / (t1 - t0), iters);
	munmap(map, create.size);
	ret = 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

static int set_crtc(int card, uint32_t crtc_id, uint32_t fb_id,
		    const struct drm_mode_modeinfo *mode, uint32_t connector_id);

/* Fill a buffer and read every pixel back, to prove a timing is real work. */
static int verify_test(int gpu, unsigned int width, unsigned int height,
		       uint32_t format)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int cpp = fourcc_cpp(format), pitch = width * cpp;
	unsigned int bad = 0, first_bad = ~0U, x, y;
	uint32_t color = format == DRM_FORMAT_RGB565 ? 0x1234 : 0x00112233;
	void *map;
	int ret = -1;

	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	/* Dirty every byte first: stale content must not look like a fill. */
	memset(map, 0x5a, create.size);
	fill.handle = create.handle;
	fill.format = format;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = color;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("GPU fill");
		munmap(map, create.size);
		goto out;
	}
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			uint32_t got;

			if (cpp == 2)
				got = ((uint16_t *)map)[(y * width) + x];
			else
				got = ((uint32_t *)map)[(y * width) + x];
			if (got != color) {
				if (first_bad == ~0U)
					first_bad = (y * width) + x;
				bad++;
			}
		}
	}
	printf("%ux%u %s: %u of %u pixels wrong", width, height,
	       fourcc_name(format), bad, width * height);
	if (bad)
		printf(" (first at pixel %u, row %u)", first_bad,
		       first_bad / width);
	putchar('\n');
	munmap(map, create.size);
	ret = bad ? -1 : 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/*
 * Draw one solid rectangle with the 2D pixel pipeline and check that exactly
 * the requested pixels changed.  The buffer is dirtied first, so neither a
 * missed write nor a write outside the rectangle can pass unnoticed.
 */
static int rect_test(int gpu, unsigned int width, unsigned int height,
		     unsigned int iters)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_rect rect = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	const uint32_t color = 0xff1020c0, want = 0x1118;
	const unsigned int x0 = 8, y0 = 16, w = width / 2, h = height / 2;
	unsigned int pitch = width * 2, bad_in = 0, bad_out = 0, i;
	struct timespec t0, t1;
	double rect_ms, fill_ms;
	uint16_t *px;
	void *map;
	int ret = -1;

	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	px = map;
	memset(map, 0x5a, create.size);

	rect.handle = create.handle;
	rect.format = DRM_FORMAT_RGB565;
	rect.offset = (y0 * pitch) + (x0 * 2);
	rect.pitch = pitch;
	rect.width = w;
	rect.height = h;
	rect.color = color;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect)) {
		perror("GPU rect");
		munmap(map, create.size);
		goto out;
	}
	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++) {
			int inside = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
			uint16_t v = px[(y * width) + x];

			if (inside) {
				if (v != want)
					bad_in++;
			} else if (v != 0x5a5a) {
				bad_out++;
			}
		}
	printf("rect %ux%u at %u,%u: %u of %u pixels wrong inside, "
	       "%u changed outside\n", w, h, x0, y0, bad_in, w * h, bad_out);

	rect.offset = 0;
	rect.width = width;
	rect.height = height;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++)
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect))
			break;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	rect_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		  (t1.tv_nsec - t0.tv_nsec) / 1e6;
	rect_ms /= iters;

	fill.handle = create.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = 0x1234;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++)
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
			break;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fill_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		  (t1.tv_nsec - t0.tv_nsec) / 1e6;
	fill_ms /= iters;

	printf("full %ux%u RGB565: rect %.2f ms (%.1f MB/s), "
	       "fill %.2f ms (%.1f MB/s)\n", width, height, rect_ms,
	       width * height * 2 / rect_ms / 1000.0, fill_ms,
	       width * height * 2 / fill_ms / 1000.0);
	munmap(map, create.size);
	ret = (bad_in || bad_out) ? -1 : 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/* ---------------- batch rectangle submission ---------------- */

static uint32_t argb565(uint32_t argb);

static void batch_build(struct drm_n32h7xx_rect *rect, unsigned int count,
		       uint32_t handle, unsigned int pitch, unsigned int width,
		       unsigned int height, unsigned int rw, unsigned int rh)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		memset(&rect[i], 0, sizeof(rect[i]));
		rect[i].handle = handle;
		rect[i].format = DRM_FORMAT_RGB565;
		rect[i].pitch = pitch;
		rect[i].width = rw;
		rect[i].height = rh;
		rect[i].offset = (height > rh ? (i * 37) % (height - rh) : 0) *
				 pitch +
				 (width > rw ? (i * 53) % (width - rw) : 0) * 2;
		rect[i].color = 0xff000000 | (i * 2654435761u);
	}
}

static int batch_test(int gpu, unsigned int width, unsigned int height,
		      unsigned int rw, unsigned int rh)
{
	static const unsigned int counts[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
	const unsigned int pitch = width * 2;
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_rect rect[DRM_N32H7XX_BATCH_MAX];
	struct drm_n32h7xx_rect_batch batch = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int i, bad = 0;
	uint16_t *px;
	void *map;
	int ret = -1;

	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	px = map;

	batch_build(rect, DRM_N32H7XX_BATCH_MAX, create.handle, pitch,
		    width, height, rw, rh);
	batch.handle = create.handle;
	batch.count = 1;
	batch.rects = (uint64_t)(uintptr_t)rect;

	/* Argument checks: none of these may reach the hardware. */
	batch.count = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch) != -1 ||
	    errno != EINVAL)
		bad++;
	batch.count = DRM_N32H7XX_BATCH_MAX + 1;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch) != -1 ||
	    errno != EINVAL)
		bad++;
	batch.count = 4;
	batch.rects = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch) != -1 ||
	    errno != EINVAL)
		bad++;
	batch.rects = (uint64_t)(uintptr_t)rect;
	batch.reserved = 1;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch) != -1 ||
	    errno != EINVAL)
		bad++;
	batch.reserved = 0;
	rect[1].handle = create.handle + 1;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch) != -1 ||
	    errno != EINVAL)
		bad++;
	rect[1].handle = create.handle;
	printf("batch rejects: %u of 5 malformed requests refused\n", 5 - bad);
	fflush(stdout);

	/* Every rectangle must land exactly where it was asked to. */
	fill.handle = create.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = 0x1234;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
		perror("batch background");

	batch_build(rect, DRM_N32H7XX_BATCH_MAX, create.handle, pitch,
		    width, height, rw, rh);
	batch.count = DRM_N32H7XX_BATCH_MAX;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch)) {
		printf("batch draw (count %u): %s\n", batch.count,
		       strerror(errno));
		bad++;
	} else {
		unsigned int n;

		for (n = 0; n < DRM_N32H7XX_BATCH_MAX; n++) {
			unsigned int x, y, x0 = (rect[n].offset % pitch) / 2;
			unsigned int y0 = rect[n].offset / pitch;
			uint16_t want = argb565(rect[n].color);

			for (y = y0; y < y0 + rh; y++)
				for (x = x0; x < x0 + rw; x++)
					if (px[y * width + x] != want) {
						bad++;
						y = y0 + rh;
						break;
					}
			for (y = y0; y < y0 + rh; y++) {
				if (x0 && px[y * width + x0 - 1] != 0x1234)
					bad++;
				if (x0 + rw < width &&
				    px[y * width + x0 + rw] != 0x1234)
					bad++;
			}
		}
	}

	/* Per-call cost with a growing number of rectangles in one job. */
	for (i = 0; i < ARRAY_SIZE(counts); i++) {
		static double sample[4096];
		unsigned int count = counts[i];
		unsigned int iters = 4096 / count;
		unsigned int n;

		if (iters < 64)
			iters = 64;
		batch_build(rect, count, create.handle, pitch, width, height,
			    rw, rh);
		batch.count = count;
		for (n = 0; n < iters; n++) {
			struct timespec a, b;

			clock_gettime(CLOCK_MONOTONIC, &a);
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch)) {
				printf("batch bench count %u: %s\n", count,
				       strerror(errno));
				fflush(stdout);
				bad++;
				iters = 0;
				break;
			}
			clock_gettime(CLOCK_MONOTONIC, &b);
			sample[n] = (b.tv_sec - a.tv_sec) * 1e6 +
				    (b.tv_nsec - a.tv_nsec) / 1e3;
		}
		if (!iters)
			break;
		qsort(sample, iters, sizeof(sample[0]), cmp_double);
		printf("batch %3u x %ux%u: p50 %7.1f us/call, "
		       "%5.2f us/rect (p99 %7.1f, max %7.1f)\n",
		       count, rw, rh, sample[iters / 2],
		       sample[iters / 2] / count,
		       sample[iters - 1 - iters / 100],
		       sample[iters - 1]);
	}

	printf("batch: %s\n", bad ? "FAIL" : "PASS");
	munmap(map, create.size);
	ret = bad ? -1 : 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

static uint32_t argb565(uint32_t argb)
{
	unsigned int r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff;
	unsigned int b = argb & 0xff;

	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void expand565(uint32_t v, unsigned int *r, unsigned int *g,
		      unsigned int *b)
{
	*r = (v >> 11) & 0x1f;
	*g = (v >> 5) & 0x3f;
	*b = v & 0x1f;
	*r = (*r << 3) | (*r >> 2);
	*g = (*g << 2) | (*g >> 4);
	*b = (*b << 3) | (*b >> 2);
}

/* Reference implementation of the KMS default pixel blend mode. */
static uint16_t blend_ref565(uint16_t dst, uint32_t src, unsigned int alpha)
{
	unsigned int dr, dg, db, r, g, b;

	expand565(dst, &dr, &dg, &db);
	r = (((src >> 16) & 0xff) * alpha + dr * (255 - alpha) + 127) / 255;
	g = (((src >> 8) & 0xff) * alpha + dg * (255 - alpha) + 127) / 255;
	b = ((src & 0xff) * alpha + db * (255 - alpha) + 127) / 255;
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/*
 * Largest difference between the two colours in RGB565 code steps.  The
 * hardware rounds while the reference truncates, so one step per field is
 * expected and anything beyond it is a real mismatch.
 */
static unsigned int channel_delta(uint16_t a, uint16_t b)
{
	unsigned int dr, dg, db;

	dr = ((a >> 11) & 0x1f) > ((b >> 11) & 0x1f) ?
	     ((a >> 11) & 0x1f) - ((b >> 11) & 0x1f) :
	     ((b >> 11) & 0x1f) - ((a >> 11) & 0x1f);
	dg = ((a >> 5) & 0x3f) > ((b >> 5) & 0x3f) ?
	     ((a >> 5) & 0x3f) - ((b >> 5) & 0x3f) :
	     ((b >> 5) & 0x3f) - ((a >> 5) & 0x3f);
	db = (a & 0x1f) > (b & 0x1f) ? (a & 0x1f) - (b & 0x1f) :
				       (b & 0x1f) - (a & 0x1f);
	return dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
}

/*
 * ARGB4444 and ARGB1555 keep alpha in a narrow field.  Blend over a surface
 * whose destination alpha is zero and check the field: alpha 0 must leave the
 * surface untouched, alpha 255 must write the packed source colour, and alpha
 * 128 must land between the two.  This is the only test that exercises a
 * destination alpha other than the 8-bit ARGB8888 case.
 */
static unsigned int alpha_smoke(int gpu, uint32_t handle, void *map,
				uint32_t format, unsigned int width,
				unsigned int height)
{
	const unsigned int cpp = 2, pitch = width * cpp;
	const uint32_t color = 0x80c04020;
	const unsigned int abits = format == DRM_FORMAT_ARGB1555 ? 1 : 4;
	const unsigned int ashift = format == DRM_FORMAT_ARGB1555 ? 15 : 12;
	const unsigned int rshift = format == DRM_FORMAT_ARGB1555 ? 10 : 8;
	const unsigned int gshift = format == DRM_FORMAT_ARGB1555 ? 5 : 4;
	const unsigned int rb = format == DRM_FORMAT_ARGB1555 ? 5 : 4;
	const unsigned int gb = rb;
	const unsigned int bb = rb;
	const unsigned int amask = (1u << abits) - 1;
	const uint16_t src_px = (uint16_t)((0xc0u >> (8 - rb)) << rshift |
					   (0x40u >> (8 - gb)) << gshift |
					   (0x20u >> (8 - bb)) | (amask << ashift));
	static const unsigned int alphas[] = { 0, 0x80, 0xff };
	struct drm_n32h7xx_blend blend = { 0 };
	unsigned int a, k, bad = 0;
	uint16_t *px = map;

	blend.handle = handle;
	blend.format = format;
	blend.pitch = pitch;
	blend.width = width;
	blend.height = height;
	for (a = 0; a < ARRAY_SIZE(alphas); a++) {
		unsigned int alpha = alphas[a];
		unsigned int want_a = alpha >> (8 - abits);

		for (k = 0; k < width * height; k++)
			px[k] = 0x0234;	/* alpha field is zero here */
		blend.color = (alpha << 24) | (color & 0xffffff);
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLEND, &blend)) {
			printf("alpha smoke %08x alpha %u: %s\n", format, alpha,
			       strerror(errno));
			return ~0u;
		}
		for (k = 0; k < width * height; k++) {
			unsigned int got = (px[k] >> ashift) & amask;
			unsigned int rgb = px[k] & ~(amask << ashift);
			unsigned int want_rgb = 0x0234;
			unsigned int rgb_bad;

			if (alpha == 0xff)
				want_rgb = src_px & ~(amask << ashift);
			rgb_bad = (alpha == 0 || alpha == 0xff) &&
				  rgb != want_rgb;
			if (got != want_a || rgb_bad) {
				bad++;
				if (bad < 4)
					printf("alpha smoke %08x alpha %u: "
					       "pixel %u got %04x want alpha %x\n",
					       format, alpha, k, px[k], want_a);
			}
		}
	}
	return bad;
}

/*
 * Blend solid rectangles over a gradient with every alpha the driver accepts
 * and compare each pixel against the software source-over reference.  A
 * missing destination read shows up immediately, because the result then
 * depends only on the source colour.
 */
static unsigned int blend_matrix(int gpu, uint32_t handle, void *map,
				 unsigned int width, unsigned int height,
				 uint32_t format, unsigned int *checked)
{
	static const unsigned int alphas[] = { 0, 1, 32, 64, 128, 192, 254, 255 };
	static const uint32_t colors[] = {
		0x1020c0, 0xff8000, 0x00ff40, 0x808080, 0xffffff, 0x000000,
	};
	const unsigned int x0 = 4, y0 = 8, w = width - 8, h = height - 16;
	unsigned int cpp = fourcc_cpp(format), pitch = width * cpp;
	struct drm_n32h7xx_blend blend = { 0 };
	unsigned int bad = 0, worst = 0, i, a, c;
	uint16_t *px16 = map;
	uint32_t *px32 = map;

	blend.handle = handle;
	blend.format = format;
	blend.offset = (y0 * pitch) + (x0 * cpp);
	blend.pitch = pitch;
	blend.width = w;
	blend.height = h;

	for (a = 0; a < ARRAY_SIZE(alphas); a++) {
		for (c = 0; c < ARRAY_SIZE(colors); c++) {
			unsigned int alpha = alphas[a];
			unsigned int has_alpha =
				format == DRM_FORMAT_ARGB8888;

			blend.color = (alpha << 24) | colors[c];
			for (i = 0; i < width * height; i++) {
				unsigned int x = i % width, y = i / width;
				uint32_t v = ((y * 0x010101u) ^
					      (x * 0x000101u)) & 0xffffff;

				if (cpp == 2)
					px16[i] = argb565(v);
				else
					px32[i] = v | (has_alpha ?
						((uint32_t)((x * 7 + y * 11) & 0xff)
						 << 24) : 0);
			}
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLEND, &blend)) {
				perror("GPU blend");
				return ~0u;
			}
			for (i = 0; i < width * height; i++) {
				unsigned int x = i % width, y = i / width;
				uint32_t src = ((y * 0x010101u) ^
						(x * 0x000101u)) & 0xffffff;

				if (x >= x0 && x < x0 + w && y >= y0 &&
				    y < y0 + h) {
					unsigned int d;

					if (cpp == 2) {
						uint16_t want =
							blend_ref565(argb565(src),
								     blend.color,
								     alpha);

						(*checked)++;
						d = channel_delta(px16[i], want);
					} else {
						unsigned int sr = (blend.color >> 16) & 0xff;
						unsigned int sg = (blend.color >> 8) & 0xff;
						unsigned int sb = blend.color & 0xff;
						unsigned int dr = (src >> 16) & 0xff;
						unsigned int dg = (src >> 8) & 0xff;
						unsigned int db = src & 0xff;
						unsigned int da = has_alpha ?
							((x * 7 + y * 11) & 0xff) : 0;
						unsigned int want_r, want_g, want_b;

						(*checked)++;
						want_r = (sr * alpha +
							  dr * (255 - alpha) +
							  127) / 255;
						want_g = (sg * alpha +
							  dg * (255 - alpha) +
							  127) / 255;
						want_b = (sb * alpha +
							  db * (255 - alpha) +
							  127) / 255;
						d = abs((int)((px32[i] >> 16) & 0xff) -
							(int)want_r);
						if (abs((int)((px32[i] >> 8) & 0xff) -
							(int)want_g) > (int)d)
							d = abs((int)((px32[i] >> 8) & 0xff) -
								(int)want_g);
						if (abs((int)(px32[i] & 0xff) -
							(int)want_b) > (int)d)
							d = abs((int)(px32[i] & 0xff) -
								(int)want_b);
						if (has_alpha) {
							unsigned int want_a =
								(alpha * 255 +
								 da * (255 - alpha) +
								 127) / 255;
							unsigned int got_a =
								(px32[i] >> 24) & 0xff;

							if (abs((int)got_a -
								(int)want_a) > (int)d)
								d = abs((int)got_a -
									(int)want_a);
						}
					}
					if (d > worst)
						worst = d;
					if (d > 1)
						bad++;
				} else if (cpp == 2) {
					if (px16[i] != argb565(src))
						bad++;
				} else {
					uint32_t want = src | (has_alpha ?
						((uint32_t)((x * 7 + y * 11) & 0xff)
						 << 24) : 0);

					/* XRGB8888 has no alpha channel; its top
					 * byte is padding and may be rewritten.
					 */
					if (has_alpha ? px32[i] != want :
					    (px32[i] & 0xffffff) != src)
						bad++;
				}
			}
			if (bad) {
				printf("blend %08x alpha %u colour %06x: "
				       "%u wrong, worst delta %u\n", format,
				       alpha, colors[c], bad, worst);
				return bad;
			}
		}
	}
	printf("blend %08x %ux%u: %u of %u pixels off, worst delta %u "
	       "(%u alpha/colour cases)\n", format, w, h, bad, *checked, worst,
	       (unsigned int)ARRAY_SIZE(alphas) * ARRAY_SIZE(colors));
	return bad;
}

/*
 * Blend solid rectangles over a gradient with every alpha the driver accepts
 * and compare each pixel against the software source-over reference.  A
 * missing destination read shows up immediately, because the result then
 * depends only on the source colour.
 */
static int blend_test(int gpu, unsigned int width, unsigned int height,
		      unsigned int iters)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_blend blend = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int checked = 0, bad = 0, i;
	struct timespec t0, t1;
	double blend_ms;
	void *map;
	int ret = -1;

	create.size = (uint64_t)width * height * 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}

	bad = blend_matrix(gpu, create.handle, map, width, height,
			   DRM_FORMAT_RGB565, &checked);
	bad |= alpha_smoke(gpu, create.handle, map, DRM_FORMAT_ARGB4444,
			   width, height);
	bad |= alpha_smoke(gpu, create.handle, map, DRM_FORMAT_ARGB1555,
			   width, height);

	blend.handle = create.handle;
	blend.format = DRM_FORMAT_RGB565;
	blend.pitch = width * 2;
	blend.width = width;
	blend.height = height;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++)
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLEND, &blend))
			break;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	blend_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		   (t1.tv_nsec - t0.tv_nsec) / 1e6;
	blend_ms /= iters;
	printf("full %ux%u RGB565 blend: %.2f ms (%.1f MB/s read+write)\n",
	       width, height, blend_ms,
	       width * height * 2 * 2 / blend_ms / 1000.0);
	munmap(map, create.size);
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);

	/*
	 * A full-size 32-bit buffer needs a 1.5 MB contiguous shmem backing,
	 * which is at the order-10 limit; half size keeps the XRGB8888 pass
	 * well inside what the board can allocate.
	 */
	memset(&create, 0, sizeof(create));
	create.size = (uint64_t)(width / 2) * (height / 2) * 4;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	bad |= blend_matrix(gpu, create.handle, map, width / 2, height / 2,
			    DRM_FORMAT_XRGB8888, &checked);
	bad |= blend_matrix(gpu, create.handle, map, width / 2, height / 2,
			    DRM_FORMAT_ARGB8888, &checked);
	munmap(map, create.size);
	ret = bad ? -1 : 0;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/*
 * Convex polygon cases for the POLY ioctl, laid out in a fixed 640x360
 * workspace so the shapes keep their shape whatever surface is used.
 */
struct poly_case {
	const char *name;
	unsigned int count;
	double x[6], y[6];
	uint32_t color;
	uint32_t flags;
};

/* One side of a polygon, normalised so coverage falls out of a dot product. */
struct poly_side {
	double nx, ny, x0, y0, bias;
};

struct poly_ref {
	struct poly_side side[6];
	unsigned int count;
};

static double poly_hypot(double x, double y)
{
	double v = x * x + y * y, r;
	unsigned int i;

	if (v <= 0)
		return 0;
	/*
	 * Newton on t -> (t + v/t) / 2; the test binary does not link libm.
	 * Starting at v itself converges in a handful of steps for the edge
	 * lengths seen here, and this only runs once per side.
	 */
	r = v > 1 ? v : 1;
	for (i = 0; i < 40; i++)
		r = (r + v / r) * 0.5;
	return r;
}

/*
 * The hardware evaluates v = n . (p - p0) / |n| at the top-left corner of
 * each pixel and clamps it to 0..1 for the coverage.  The pixel's far corner
 * along the inward normal is what a side actually cuts, so its offset,
 * max(nx, 0) + max(ny, 0) in unit normal terms, has to be added: otherwise a
 * side whose interior lies right or below lands one pixel short.
 */
static void poly_ref_build(struct poly_ref *ref, const struct poly_case *c)
{
	double cx = 0, cy = 0;
	unsigned int i;

	for (i = 0; i < c->count; i++) {
		cx += c->x[i];
		cy += c->y[i];
	}
	cx /= c->count;
	cy /= c->count;
	ref->count = c->count;
	for (i = 0; i < c->count; i++) {
		double x0 = c->x[i], y0 = c->y[i];
		double ex = c->x[(i + 1) % c->count] - x0;
		double ey = c->y[(i + 1) % c->count] - y0;
		double nx = -ey, ny = ex, len = poly_hypot(ex, ey);

		/* The interior is the side holding the centroid. */
		if (nx * (cx - x0) + ny * (cy - y0) < 0) {
			nx = -nx;
			ny = -ny;
		}
		ref->side[i].nx = nx / len;
		ref->side[i].ny = ny / len;
		ref->side[i].x0 = x0;
		ref->side[i].y0 = y0;
		ref->side[i].bias = (nx > 0 ? nx / len : 0) +
				    (ny > 0 ? ny / len : 0);
	}
}

static double poly_distance(const struct poly_side *s, double x, double y)
{
	return s->nx * (x - s->x0) + s->ny * (y - s->y0);
}

/*
 * Hardware coverage of the pixel whose top-left corner is (x, y): every side
 * yields 0..1 and the two smallest values are multiplied, which is what the
 * pixel selection unit documents.  Without the ramp a side is a threshold at
 * zero and the far corner offset is not part of the limiter value.
 */
static double poly_cov(const struct poly_ref *ref, double x, double y,
		       int no_aa)
{
	double lo1 = 1, lo2 = 1;
	unsigned int i;

	for (i = 0; i < ref->count; i++) {
		double v = poly_distance(&ref->side[i], x, y);

		if (no_aa)
			v = v >= 0 ? 1 : 0;
		else {
			v += ref->side[i].bias;
			v = v < 0 ? 0 : v > 1 ? 1 : v;
		}
		if (v < lo1) {
			lo2 = lo1;
			lo1 = v;
		} else if (v < lo2) {
			lo2 = v;
		}
	}
	return lo1 * lo2;
}

/*
 * Distance of the pixel corner from the closest side.  Without the ramp the
 * hardware decides on the sign of that value, so a pixel that sits on a side
 * (within the fixed point rounding of the limiter registers) may fall either
 * way and cannot be held against it.
 */
static double poly_margin(const struct poly_ref *ref, double x, double y)
{
	double best = 1e9;
	unsigned int i;

	for (i = 0; i < ref->count; i++) {
		double v = fabs(poly_distance(&ref->side[i], x, y));

		if (v < best)
			best = v;
	}
	return best;
}

/* True area coverage by point sampling the pixel square, for reference. */
static double poly_area(const struct poly_ref *ref, unsigned int x,
			unsigned int y)
{
	unsigned int sx, sy, i, hits = 0;

	for (sy = 0; sy < 4; sy++)
		for (sx = 0; sx < 4; sx++) {
			double px = x + (sx + 0.5) / 4.0;
			double py = y + (sy + 0.5) / 4.0;

			for (i = 0; i < ref->count; i++)
				if (poly_distance(&ref->side[i], px, py) < 0)
					break;
			if (i == ref->count)
				hits++;
		}
	return hits / 16.0;
}

static uint32_t blend_ref32(uint32_t dst, uint32_t src, unsigned int alpha)
{
	unsigned int o = 0, sh;

	for (sh = 16; sh < 25; sh -= 8) {
		unsigned int s = (src >> sh) & 0xff, d = (dst >> sh) & 0xff;

		o |= ((s * alpha + d * (255 - alpha) + 127) / 255) << sh;
	}
	return o;
}

static unsigned int channel_delta32(uint32_t a, uint32_t b)
{
	unsigned int worst = 0, sh;

	for (sh = 16; sh < 25; sh -= 8) {
		unsigned int x = (a >> sh) & 0xff, y = (b >> sh) & 0xff;
		unsigned int d = x > y ? x - y : y - x;

		if (d > worst)
			worst = d;
	}
	return worst;
}

/*
 * Paint the destination pattern, draw one polygon, then compare every pixel
 * against the coverage model.  Pixels the model leaves at zero coverage must
 * come back untouched, which catches a ramp that is one pixel wide or a
 * limiter that is not participating.
 */
static unsigned int poly_verify(int gpu, uint32_t handle, void *map,
				unsigned int width, unsigned int height,
				uint32_t format, const struct poly_case *c,
				unsigned int *checked, unsigned int *worst,
				unsigned int *worst_area)
{
	unsigned int cpp = fourcc_cpp(format), pitch = width * cpp;
	unsigned int alpha = (c->color >> 24) & 0xff, bad = 0, ties = 0, i;
	struct drm_n32h7xx_poly poly = { 0 };
	struct poly_ref ref;
	uint16_t *px16 = map;
	uint32_t *px32 = map;

	poly_ref_build(&ref, c);
	for (i = 0; i < width * height; i++) {
		unsigned int x = i % width, y = i / width;
		uint32_t v = ((y * 0x010101u) ^ (x * 0x000101u)) & 0xffffff;

		if (cpp == 2)
			px16[i] = argb565(v);
		else
			px32[i] = v;
	}
	poly.handle = handle;
	poly.format = format;
	poly.pitch = pitch;
	poly.color = c->color;
	poly.flags = c->flags;
	poly.vertex_count = c->count;
	for (i = 0; i < c->count; i++) {
		poly.x[i] = (int32_t)(c->x[i] * 65536);
		poly.y[i] = (int32_t)(c->y[i] * 65536);
	}
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly)) {
		perror("GPU poly");
		return ~0u;
	}
	for (i = 0; i < width * height; i++) {
		unsigned int x = i % width, y = i / width;
		double cov = poly_cov(&ref, x, y, c->flags & 1);
		unsigned int a = (unsigned int)(cov * alpha + 0.5);
		uint32_t base = ((y * 0x010101u) ^ (x * 0x000101u)) & 0xffffff;
		unsigned int d, area;

		(*checked)++;
		/*
		 * Without the ramp a pixel is in or out by the sign of the
		 * limiter value only, so one sitting on a side can go either
		 * way once the 2.18 arithmetic rounds.  Such a pixel must be
		 * fully covered or untouched; anything else is a real error.
		 */
		if ((c->flags & 1) && poly_margin(&ref, x, y) < 0.005) {
			ties++;
			continue;
		}
		if (cpp == 2) {
			uint16_t want = blend_ref565(argb565(base), c->color, a);

			d = channel_delta(px16[i], want);
		} else {
			d = channel_delta32(px32[i] & 0xffffff,
					    blend_ref32(base, c->color, a));
		}
		if (d > *worst)
			*worst = d;
		if (d > 1)
			bad++;
		/* How far the hardware's linear ramp is from the real area. */
		if (cov > 0 && cov < 1) {
			area = (unsigned int)(poly_area(&ref, x, y) * 255 + 0.5);
			d = area > 0 ? area : 0;
			d = d > (unsigned int)(cov * 255) ?
			    d - (unsigned int)(cov * 255) :
			    (unsigned int)(cov * 255) - d;
			if (d > *worst_area)
				*worst_area = d;
		}
	}
	if (ties)
		printf("poly %s: %u pixels on a side, sign decided by rounding\n",
		       c->name, ties);
	return bad;
}

static void poly_batch_build(struct drm_n32h7xx_poly *polys,
			     unsigned int count, uint32_t handle,
			     unsigned int pitch, unsigned int width,
			     unsigned int height)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		unsigned int x = 16 + (i % 16) * 32;
		unsigned int y = 16 + (i / 16) * 64;

		memset(&polys[i], 0, sizeof(polys[i]));
		polys[i].handle = handle;
		polys[i].format = DRM_FORMAT_RGB565;
		polys[i].offset = y * pitch + x * 2;
		polys[i].pitch = pitch;
		polys[i].color = 0xff000000 | (i * 2654435761u);
		polys[i].vertex_count = 3;
		polys[i].x[0] = 1 << 16;
		polys[i].y[0] = 1 << 16;
		polys[i].x[1] = 14 << 16;
		polys[i].y[1] = 1 << 16;
		polys[i].x[2] = 1 << 16;
		polys[i].y[2] = 14 << 16;
		if (x + 14 >= width || y + 14 >= height)
			polys[i].vertex_count = 0;
	}
}

static int poly_test(int gpu, unsigned int width, unsigned int height,
		     unsigned int iters)
{
	static const struct poly_case cases[] = {
		{ .name = "square on pixel edges", .count = 4,
		  .x = { 16, 116, 116, 16 }, .y = { 16, 16, 116, 116 },
		  .color = 0xffff0000 },
		{ .name = "square on pixel centres", .count = 4,
		  .x = { 140.5, 240.5, 240.5, 140.5 },
		  .y = { 16.5, 16.5, 116.5, 116.5 }, .color = 0xff00ff00 },
		{ .name = "triangle 45 degrees", .count = 3,
		  .x = { 270, 370, 270 }, .y = { 16, 116, 116 },
		  .color = 0xff0000ff },
		{ .name = "skewed quad", .count = 4,
		  .x = { 16, 126, 136, 26 }, .y = { 150, 160, 280, 270 },
		  .color = 0xff00ffff },
		{ .name = "hexagon", .count = 6,
		  .x = { 300, 260, 180, 140, 180, 260 },
		  .y = { 220, 289.2820323, 289.2820323, 220, 150.7179677,
			 150.7179677 },
		  .color = 0xffffa000 },
		{ .name = "skewed quad, no AA", .count = 4,
		  .x = { 396, 506, 516, 406 }, .y = { 150, 160, 280, 270 },
		  .color = 0xff00ffff, .flags = 1 },
		{ .name = "triangle alpha 0", .count = 3,
		  .x = { 500, 600, 600 }, .y = { 16, 16, 116 },
		  .color = 0x00ff8800 },
		{ .name = "triangle alpha 64", .count = 3,
		  .x = { 500, 600, 600 }, .y = { 16, 16, 116 },
		  .color = 0x40ff8800 },
		{ .name = "triangle alpha 128", .count = 3,
		  .x = { 500, 600, 600 }, .y = { 16, 16, 116 },
		  .color = 0x80ff8800 },
		{ .name = "triangle alpha 255", .count = 3,
		  .x = { 500, 600, 600 }, .y = { 16, 16, 116 },
		  .color = 0xffff8800 },
	};
	static const struct poly_case hex_small = {
		.name = "triangle xrgb8888", .count = 3,
		.x = { 20, 120, 20 }, .y = { 20, 120, 120 },
		.color = 0xff20c040,
	};
	struct drm_n32h7xx_poly poly = { 0 };
	struct drm_n32h7xx_poly polys[DRM_N32H7XX_POLY_BATCH_MAX];
	struct drm_n32h7xx_poly_batch poly_batch = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_n32h7xx_create create = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int checked = 0, bad = 0, worst = 0, worst_area = 0, i;
	unsigned int rejects = 0, batch_rejects = 0;
	struct timespec t0, t1;
	double poly_ms;
	void *map;
	int ret = -1;

	if (width < 640 || height < 360) {
		fprintf(stderr, "poly needs at least a 640x360 surface\n");
		return -1;
	}
	create.size = (uint64_t)width * height * 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}

	/* Argument checks: none of these may reach the hardware. */
	poly.handle = create.handle;
	poly.format = DRM_FORMAT_RGB565;
	poly.pitch = width * 2;
	poly.color = 0xffffffff;
	poly.vertex_count = 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly) == 0 || errno != EINVAL)
		rejects++;
	poly.vertex_count = 7;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly) == 0 || errno != EINVAL)
		rejects++;
	poly.vertex_count = 3;
	poly.flags = 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly) == 0 || errno != EINVAL)
		rejects++;
	poly.flags = 0;
	poly.format = DRM_FORMAT_ARGB8888;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly) == 0 || errno != EINVAL)
		rejects++;
	poly.format = DRM_FORMAT_RGB565;
	poly.x[0] = (int32_t)((width + 8) * 65536);
	poly.y[0] = 16 << 16;
	poly.x[1] = poly.x[2] = 16 << 16;
	poly.y[1] = 116 << 16;
	poly.y[2] = 16 << 16;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly) == 0 || errno != EINVAL)
		rejects++;
	printf("poly rejects: %u of 5 malformed requests refused\n", 5 - rejects);
	bad += rejects;

	poly_batch_build(polys, DRM_N32H7XX_POLY_BATCH_MAX, create.handle,
			 width * 2, width, height);
	poly_batch.handle = create.handle;
	poly_batch.count = 1;
	poly_batch.polys = (uint64_t)(uintptr_t)polys;
	poly_batch.count = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH, &poly_batch) != -1 ||
	    errno != EINVAL)
		batch_rejects++;
	poly_batch.count = DRM_N32H7XX_POLY_BATCH_MAX + 1;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH, &poly_batch) != -1 ||
	    errno != EINVAL)
		batch_rejects++;
	poly_batch.count = 4;
	poly_batch.polys = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH, &poly_batch) != -1 ||
	    errno != EINVAL)
		batch_rejects++;
	poly_batch.polys = (uint64_t)(uintptr_t)polys;
	poly_batch.reserved = 1;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH, &poly_batch) != -1 ||
	    errno != EINVAL)
		batch_rejects++;
	poly_batch.reserved = 0;
	polys[1].handle = create.handle + 1;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH, &poly_batch) != -1 ||
	    errno != EINVAL)
		batch_rejects++;
	polys[1].handle = create.handle;
	printf("poly batch rejects: %u of 5 malformed requests refused\n",
	       5 - batch_rejects);
	bad += batch_rejects;

	fill.handle = create.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = width * 2;
	fill.width = width;
	fill.height = height;
	fill.color = 0x1234;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("poly batch background");
		bad++;
	}
	poly_batch.count = DRM_N32H7XX_POLY_BATCH_MAX;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH, &poly_batch)) {
		printf("poly batch draw: %s\n", strerror(errno));
		bad++;
	} else {
		uint16_t *px = map;

		for (i = 0; i < DRM_N32H7XX_POLY_BATCH_MAX; i++) {
			unsigned int x = 16 + (i % 16) * 32;
			unsigned int y = 16 + (i / 16) * 64;

			if (px[(y + 5) * width + x + 5] !=
			    argb565(polys[i].color))
				bad++;
			if (px[(y + 13) * width + x + 13] != 0x1234)
				bad++;
		}
	}

	{
		static const unsigned int counts[] = { 1, 2, 4, 8, 16, 32, 64 };

		for (i = 0; i < ARRAY_SIZE(counts); i++) {
			static double sample[1024];
			unsigned int count = counts[i];
			unsigned int runs = 512 / count;
			unsigned int n;

			if (runs < 32)
				runs = 32;
			poly_batch.count = count;
			for (n = 0; n < runs; n++) {
				struct timespec a, b;

				clock_gettime(CLOCK_MONOTONIC, &a);
				if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY_BATCH,
					  &poly_batch)) {
					printf("poly batch %u: %s\n", count,
					       strerror(errno));
					bad++;
					runs = 0;
					break;
				}
				clock_gettime(CLOCK_MONOTONIC, &b);
				sample[n] = (b.tv_sec - a.tv_sec) * 1e6 +
					    (b.tv_nsec - a.tv_nsec) / 1e3;
			}
			if (!runs)
				break;
			qsort(sample, runs, sizeof(sample[0]), cmp_double);
			printf("poly batch %2u: p50 %7.1f us/call, "
			       "%5.2f us/poly\n", count, sample[runs / 2],
			       sample[runs / 2] / count);
		}
	}

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int n;

		n = poly_verify(gpu, create.handle, map, width, height,
				DRM_FORMAT_RGB565, &cases[i], &checked, &worst,
				&worst_area);
		if (n)
			printf("poly %s: %u of %u pixels off\n", cases[i].name,
			       n, width * height);
		bad += n;
	}

	/*
	 * A hexagon that covers the whole surface: every pixel is enumerated
	 * and blended, which is the realistic cost of a filled vector shape.
	 */
	{
		struct poly_case big = { .name = "full", .count = 6,
			.color = 0xff204080 };

		big.x[0] = 0;		big.y[0] = height / 2.0;
		big.x[1] = width / 4.0;	big.y[1] = 0;
		big.x[2] = width * 3 / 4.0; big.y[2] = 0;
		big.x[3] = width;	big.y[3] = height / 2.0;
		big.x[4] = width * 3 / 4.0; big.y[4] = height;
		big.x[5] = width / 4.0;	big.y[5] = height;
		memset(&poly, 0, sizeof(poly));
		poly.handle = create.handle;
		poly.format = DRM_FORMAT_RGB565;
		poly.pitch = width * 2;
		poly.color = big.color;
		poly.vertex_count = big.count;
		for (i = 0; i < big.count; i++) {
			poly.x[i] = (int32_t)(big.x[i] * 65536);
			poly.y[i] = (int32_t)(big.y[i] * 65536);
		}
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < iters; i++)
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly))
				break;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		poly_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
			  (t1.tv_nsec - t0.tv_nsec) / 1e6;
		poly_ms /= iters;
		printf("full %ux%u RGB565 polygon: %.2f ms (%.1f MB/s "
		       "read+write)\n", width, height, poly_ms,
		       width * height * 2 * 2 / poly_ms / 1000.0);
		poly.flags = 1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < iters; i++)
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_POLY, &poly))
				break;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		poly_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
			  (t1.tv_nsec - t0.tv_nsec) / 1e6;
		poly_ms /= iters;
		printf("full %ux%u RGB565 polygon, no AA: %.2f ms (%.1f MB/s "
		       "read+write)\n", width, height, poly_ms,
		       width * height * 2 * 2 / poly_ms / 1000.0);
	}
	printf("poly %ux%u rgb565: %u of %u pixels off, worst delta %u, "
	       "coverage vs area within %u/255 (%u cases)\n", width, height,
	       bad, checked, worst, worst_area,
	       (unsigned int)ARRAY_SIZE(cases));
	munmap(map, create.size);
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	ret = bad ? -1 : 0;

	/* 32-bit surfaces are covered separately: they need their own buffer. */
	memset(&create, 0, sizeof(create));
	create.size = 320ull * 240 * 4;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	checked = worst = worst_area = 0;
	bad = poly_verify(gpu, create.handle, map, 320, 240,
			  DRM_FORMAT_XRGB8888, &hex_small, &checked, &worst,
			  &worst_area);
	printf("poly 320x240 xrgb8888: %u of %u pixels off, worst delta %u, "
	       "coverage vs area within %u/255\n", bad, checked, worst,
	       worst_area);
	munmap(map, create.size);
	if (bad)
		ret = -1;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/*
 * Quadratic curve (DRM_N32H7XX_CURVE).  (x0,y0) is where the tangents at the
 * two end points meet, so the drawn parabola bulges towards it; the filled
 * part is the slice of the control triangle between the parabola and the
 * chord (x1,y1)-(x2,y2).
 *
 * The hardware feeds the two leg limiters into the curve unit.  Expressed as
 * barycentric weights u and v of the end points (u = 1 at (x1,y1) and 0 on
 * the leg through (x0,y0)->(x2,y2), and the other way round for v), a pixel
 * whose top-left corner is (x, y) is covered where
 *
 *	sqrt(u) + sqrt(v) >= 1
 *
 * which is the classic implicit form of the quadratic: the curve itself is
 * the set where the two square roots add up to exactly one.
 */
struct curve_case {
	const char *name;
	double x0, y0, x1, y1, x2, y2;
	uint32_t color;
	uint32_t flags;
};

struct curve_ref {
	double ux, uy, uc;	/* u = ux*x + uy*y + uc */
	double vx, vy, vc;
	double cx, cy, cc;	/* chord side holding (x0,y0) is >= 0 */
	double area;		/* control triangle area */
};

static double curve_sqrt(double v)
{
	double r;
	unsigned int i;

	if (v <= 0)
		return 0;
	r = v > 1 ? v : 1;
	for (i = 0; i < 40; i++)
		r = (r + v / r) * 0.5;
	return r;
}

static void curve_ref_build(struct curve_ref *ref, const struct curve_case *c)
{
	double e1x = c->x1 - c->x0, e1y = c->y1 - c->y0;
	double e2x = c->x2 - c->x0, e2y = c->y2 - c->y0;
	double det = e1x * e2y - e1y * e2x;
	double dx = c->x2 - c->x1, dy = c->y2 - c->y1;
	double nx = dy, ny = -dx;

	ref->area = (det < 0 ? -det : det) * 0.5;
	ref->ux = e2y / det;
	ref->uy = -e2x / det;
	ref->vx = -e1y / det;
	ref->vy = e1x / det;
	ref->uc = -(ref->ux * c->x0 + ref->uy * c->y0);
	ref->vc = -(ref->vx * c->x0 + ref->vy * c->y0);
	if (nx * (c->x0 - c->x1) + ny * (c->y0 - c->y1) < 0) {
		nx = -nx;
		ny = -ny;
	}
	/* Normalise the chord so values are signed distances in pixels. */
	{
		double len = poly_hypot(nx, ny);

		if (len > 0) {
			nx /= len;
			ny /= len;
		}
	}
	ref->cx = nx;
	ref->cy = ny;
	ref->cc = -(nx * c->x1 + ny * c->y1);
}

/* Signed distance in pixels from the pixel corner to the parabola. */
static double curve_distance(const struct curve_ref *ref, double x, double y)
{
	double u = ref->ux * x + ref->uy * y + ref->uc;
	double v = ref->vx * x + ref->vy * y + ref->vc;
	double du = 0, dv = 0, gx, gy, g;

	if (u > 1e-9)
		du = 0.5 / curve_sqrt(u);
	if (v > 1e-9)
		dv = 0.5 / curve_sqrt(v);
	if (!du && !dv)
		return 0;
	gx = ref->ux * du + ref->vx * dv;
	gy = ref->uy * du + ref->vy * dv;
	g = poly_hypot(gx, gy);
	if (g < 1e-12)
		return 0;
	return (curve_sqrt(u) + curve_sqrt(v) - 1) / g;
}

/*
 * Euclidean distance from a point to the drawn parabola.  curve_distance()
 * divides by the gradient of sqrt(u) + sqrt(v) - 1, which degenerates where
 * the curve is tangent to a leg, so the hard-edge check needs the real
 * distance instead of that first-order estimate.
 */
static double curve_bezier_gap(const struct curve_case *c, double t,
			       double px, double py)
{
	double a = 1 - t, b = t;
	double x = a * a * c->x1 + 2 * a * b * c->x0 + b * b * c->x2;
	double y = a * a * c->y1 + 2 * a * b * c->y0 + b * b * c->y2;

	return poly_hypot(x - px, y - py);
}

static double curve_distance_exact(const struct curve_case *c, double px,
				   double py)
{
	double lo = 0, hi = 1, m1, m2;
	unsigned int i;

	for (i = 0; i < 48; i++) {
		m1 = lo + (hi - lo) / 3;
		m2 = hi - (hi - lo) / 3;
		if (curve_bezier_gap(c, m1, px, py) <
		    curve_bezier_gap(c, m2, px, py))
			hi = m2;
		else
			lo = m1;
	}
	return curve_bezier_gap(c, (lo + hi) / 2, px, py);
}

/*
 * Draw one curve on a black surface and check the result.  White is used so
 * the destination value is the coverage itself, which lets the test add the
 * whole shape up: a one pixel ramp that is placed correctly has to come out
 * at the exact area of the region, two thirds of the control triangle.
 */
static unsigned int curve_verify(int gpu, uint32_t handle, void *map,
				 unsigned int width, unsigned int height,
				 uint32_t format, const struct curve_case *c,
				 unsigned int *checked, unsigned int *ramp,
				 double *mass)
{
	unsigned int cpp = fourcc_cpp(format), pitch = width * cpp;
	unsigned int alpha = (c->color >> 24) & 0xff, bad = 0, i;
	struct drm_n32h7xx_curve curve = { 0 };
	uint16_t *px16 = map;
	uint32_t *px32 = map;
	double expect = (2.0 / 3.0) * 0, total = 0, cdec;
	struct curve_ref ref;

	curve_ref_build(&ref, c);
	/* The chord cannot ramp, so the hardware decides it at the pixel
	 * centre: a pixel is inside when chord + half >= 0, where half is the
	 * pixel's extent along the chord normal.
	 */
	cdec = 0.5 * (poly_hypot(ref.cx, ref.cy) > 0 ?
		      (ref.cx > 0 ? ref.cx : -ref.cx) +
		      (ref.cy > 0 ? ref.cy : -ref.cy) : 0);
	memset(map, 0, (size_t)width * height * cpp);
	curve.handle = handle;
	curve.format = format;
	curve.pitch = pitch;
	curve.color = c->color;
	curve.flags = c->flags;
	curve.x0 = (int32_t)(c->x0 * 65536);
	curve.y0 = (int32_t)(c->y0 * 65536);
	curve.x1 = (int32_t)(c->x1 * 65536);
	curve.y1 = (int32_t)(c->y1 * 65536);
	curve.x2 = (int32_t)(c->x2 * 65536);
	curve.y2 = (int32_t)(c->y2 * 65536);
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve)) {
		perror("GPU curve");
		return ~0u;
	}
	*mass = 0;
	*ramp = 0;
	for (i = 0; i < width * height; i++) {
		unsigned int x = i % width, y = i / width;
		double fx = x, fy = y, d, u, v, chord;
		unsigned int got, full = cpp == 2 ? 63 : 255;

		full = (full * alpha + 127) / 255;

		(*checked)++;
		if (cpp == 2)
			got = (px16[i] >> 5) & 0x3f;
		else
			got = (px32[i] >> 8) & 0xff;
		total += got;
		if (got && got < full)
			(*ramp)++;
		/*
		 * Outside the control triangle nothing is painted, however
		 * the curve itself is evaluated there: the legs clip it.
		 */
		u = ref.ux * fx + ref.uy * fy + ref.uc;
		v = ref.vx * fx + ref.vy * fy + ref.vc;
		chord = ref.cx * fx + ref.cy * fy + ref.cc;
		/*
		 * Nothing outside the legs or the chord may be painted, but
		 * the tangency vertices sit exactly on a leg (u or v comes out
		 * as -1e-16 there) and the pixel holding such a vertex is
		 * partly covered, so pixels within a pixel of the curve are
		 * exempt.
		 */
		if (u < -1e-6 || v < -1e-6 || chord + cdec < -0.02) {
			if (got && curve_distance_exact(c, fx, fy) > 0.75) {
				bad++;
				if (bad < 8)
					printf("  off (%u,%u) outside u=%.4f "
					       "v=%.4f chord=%+.2f got=%u\n",
					       x, y, u, v, chord + cdec, got);
			}
			continue;
		}
		d = curve_distance(&ref, fx, fy);
		if (c->flags & 1) {
			/* No ramp: the pixel is in or out by the sign only, so
			 * a pixel the curve crosses may go either way.
			 */
			if (curve_distance_exact(c, fx, fy) < 0.75)
				continue;
			if (chord + cdec > -0.01 && chord + cdec < 0.01)
				continue;
			if (d > 0) {
				if (got != full)
					bad++;
			} else if (got) {
				bad++;
			}
			continue;
		}
		if (d > 2 && chord > 2) {
			if (got != full)
				bad++;
		} else if (d < -2 &&
			   curve_distance_exact(c, fx, fy) > 1.25) {
			/*
			 * The distance above degenerates where the curve is
			 * tangent to a leg: sqrt(v) has an infinite slope
			 * there, so a pixel hugging the tangency point reads
			 * as several pixels outside.  Only pixels that are
			 * really a pixel clear of the curve must stay empty;
			 * the rest is the hardware's sqrt approximation.
			 */
			if (got)
				bad++;
			if (got && bad < 8)
				printf("  off (%u,%u) d=%+.2f chord=%+.2f "
				       "got=%u\n", x, y, d, chord, got);
		}
	}
	if (cpp == 2)
		expect = (2.0 / 3.0) * ref.area * alpha / 255.0 * 63.0;
	else
		expect = (2.0 / 3.0) * ref.area * alpha / 255.0 * 255.0;
	*mass = expect > 0 ? total / expect : 1.0;
	/*
	 * A one pixel ramp can only reproduce the area where the shape is much
	 * thicker than the ramp; a sliver thinner than a couple of pixels
	 * legitimately comes out lighter, so only thick shapes are held to it.
	 */
	{
		double ex = c->x2 - c->x1, ey = c->y2 - c->y1;
		double chord = poly_hypot(ex, ey);
		double thick = (2.0 / 3.0) * ref.area / (chord * 1.2 + 1);
		double hard = chord;
		/*
		 * The two legs ramp and reproduce the area exactly, but the
		 * chord is a hard edge: the bezier unit treats any antialiased
		 * limiter as another curve leg, which blanks the shape, so it
		 * has to stay hard and can only round to whole pixels.  Without
		 * the ramp the area is quantised to half a pixel per unit of
		 * hard edge length, which is what the bound below expresses.
		 */
		double slack;

		if (c->flags & 1)
			hard += poly_hypot(c->x1 - c->x0, c->y1 - c->y0) +
				poly_hypot(c->x2 - c->x0, c->y2 - c->y0);
		/* expect counts colour steps; convert the bound to mass. */
		slack = expect > 0 ?
			0.5 * hard / ((2.0 / 3.0) * ref.area) + 0.005 : 0.01;

		if (slack < 0.01)
			slack = 0.01;
		/*
		 * A scaled alpha is quantised twice on its way to the six
		 * bits of RGB565, so its mass is compared against the opaque
		 * run by the caller instead of against the exact area.
		 */
		if (alpha == 255 && expect > 0 && thick > 3 &&
		    (*mass < 1 - slack || *mass > 1 + slack)) {
			printf("curve %s: coverage mass %.3f of the exact "
			       "area\n", c->name, *mass);
			bad++;
		}
	}
	return bad;
}

static int curve_test(int gpu, unsigned int width, unsigned int height,
		      unsigned int iters)
{
	static const struct curve_case cases[] = {
		{ .name = "corner", .x0 = 20, .y0 = 20, .x1 = 140, .y1 = 20,
		  .x2 = 20, .y2 = 140, .color = 0xffffffff },
		{ .name = "corner, no AA", .x0 = 20, .y0 = 20, .x1 = 140,
		  .y1 = 20, .x2 = 20, .y2 = 140, .color = 0xffffffff,
		  .flags = 1 },
		{ .name = "flat-ish", .x0 = 300, .y0 = 40, .x1 = 200, .y1 = 30,
		  .x2 = 400, .y2 = 26, .color = 0xffffffff },
		{ .name = "steep", .x0 = 500, .y0 = 30, .x1 = 502, .y1 = 140,
		  .x2 = 498, .y2 = 240, .color = 0xffffffff },
		{ .name = "tau", .x0 = 60, .y0 = 300, .x1 = 300, .y1 = 250,
		  .x2 = 60, .y2 = 200, .color = 0xffffffff },
	};
	struct drm_n32h7xx_curve curve = { 0 };
	struct drm_n32h7xx_create create = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int checked = 0, bad = 0, ramp = 0, rejects = 0, i;
	double mass;
	struct timespec t0, t1;
	double ms;
	void *map;
	int ret = -1;

	if (width < 640 || height < 360) {
		fprintf(stderr, "curve needs at least a 640x360 surface\n");
		return -1;
	}
	create.size = (uint64_t)width * height * 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}

	/* Malformed requests must be refused before they reach the hardware. */
	curve.handle = create.handle;
	curve.format = DRM_FORMAT_RGB565;
	curve.pitch = width * 2;
	curve.color = 0xffffffff;
	curve.x0 = 20 << 16;	curve.y0 = 20 << 16;
	curve.x1 = 140 << 16;	curve.y1 = 20 << 16;
	curve.x2 = 20 << 16;	curve.y2 = 140 << 16;
	curve.flags = 2;	/* the hardware's concave side is not exposed */
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve) == 0 || errno != EINVAL)
		rejects++;
	curve.flags = 0;
	curve.y2 = curve.y0 = 20 << 16;	/* collinear control points */
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve) == 0 || errno != EINVAL)
		rejects++;
	curve.y0 = 20 << 16;
	curve.y2 = 140 << 16;
	curve.format = DRM_FORMAT_ARGB8888;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve) == 0 || errno != EINVAL)
		rejects++;
	curve.format = DRM_FORMAT_RGB565;
	curve.x1 = (int32_t)((width + 8) << 16);
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve) == 0 || errno != EINVAL)
		rejects++;
	curve.x1 = 140 << 16;
	curve.pitch = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve) == 0 || errno != EINVAL)
		rejects++;
	curve.pitch = width * 2;
	printf("curve rejects: %u of 5 malformed requests refused\n",
	       5 - rejects);
	bad += rejects;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int n = curve_verify(gpu, create.handle, map, width,
					      height, DRM_FORMAT_RGB565,
					      &cases[i], &checked, &ramp, &mass);

		if (n)
			printf("curve %s: %u pixels off, mass %.3f\n",
			       cases[i].name, n, mass);
		else
			printf("curve %s: %u pixels of partial coverage, mass "
			       "%.3f\n", cases[i].name, ramp, mass);
		bad += n;
	}

	/* Alpha has to scale the coverage, not clip it. */
	{
		static const unsigned int a[4] = { 255, 0, 64, 128 };
		double opaque = 0;
		unsigned int k;

		for (k = 0; k < 4; k++) {
			struct curve_case c = cases[0];
			unsigned int n;

			c.color = (a[k] << 24) | 0x00ffffff;
			n = curve_verify(gpu, create.handle, map, width, height,
					 DRM_FORMAT_RGB565, &c, &checked,
					 &ramp, &mass);
			printf("curve alpha %u: mass %.3f, %u pixels off\n",
			       a[k], mass, n);
			bad += n;
			if (k == 0)
				opaque = mass;
			else if (a[k] && (mass < opaque - 0.02 ||
					  mass > opaque + 0.02)) {
				printf("curve alpha %u: mass %.3f moves away "
				       "from the opaque %.3f\n", a[k], mass,
				       opaque);
				bad++;
			}
		}
	}

	/* The whole surface at once: the cost of a realistic curve fill. */
	{
		struct curve_case big = { .name = "full" };

		big.x0 = 0;		big.y0 = 0;
		big.x1 = width;		big.y1 = 0;
		big.x2 = 0;		big.y2 = height;
		curve.x0 = 0;
		curve.y0 = 0;
		curve.x1 = width << 16;
		curve.y1 = 0;
		curve.x2 = 0;
		curve.y2 = height << 16;
		curve.flags = 0;
		curve.color = 0xff204080;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve)) {
			perror("GPU curve (full)");
			goto out;
		}
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < iters; i++)
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve))
				break;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		     (t1.tv_nsec - t0.tv_nsec) / 1e6;
		ms /= iters;
		printf("full %ux%u RGB565 curve: %.2f ms (%.1f MB/s "
		       "read+write)\n", width, height, ms,
		       (double)big.x1 * big.y2 / 2 * 2 * 2 / ms / 1000.0);
		curve.flags = 1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < iters; i++)
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_CURVE, &curve))
				break;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		     (t1.tv_nsec - t0.tv_nsec) / 1e6;
		ms /= iters;
		printf("full %ux%u RGB565 curve, no AA: %.2f ms\n", width,
		       height, ms);
	}

	printf("curve %ux%u rgb565: %u pixels off, %u of %u checked\n", width,
	       height, bad, checked, checked);
	munmap(map, create.size);
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	ret = bad ? -1 : 0;

	/* 32-bit surfaces need their own buffer. */
	memset(&create, 0, sizeof(create));
	create.size = 320ull * 240 * 4;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		   create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	{
		struct curve_case c = { .name = "xrgb8888", .x0 = 20, .y0 = 20,
			.x1 = 300, .y1 = 40, .x2 = 40, .y2 = 220,
			.color = 0xffffffff };

		checked = ramp = 0;
		bad = curve_verify(gpu, create.handle, map, 320, 240,
				   DRM_FORMAT_XRGB8888, &c, &checked, &ramp,
				   &mass);
		printf("curve 320x240 xrgb8888: %u pixels off, %u partial, "
		       "mass %.3f\n", bad, ramp, mass);
	}
	munmap(map, create.size);
	if (bad)
		ret = -1;
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/* Deterministic 565 pattern that exercises every bit position. */
static uint16_t blit_pattern(unsigned int x, unsigned int y)
{
	unsigned int v = (x * 7919u) ^ (y * 104729u);

	return (uint16_t)(v ^ (v >> 7) ^ (v >> 13));
}

static uint32_t argb_of565(uint16_t v)
{
	unsigned int r, g, b;

	expand565(v, &r, &g, &b);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
 * Source texel sampled by a destination pixel: rotation first, then the
 * reflection, then nearest-neighbour scaling at the destination pixel centre.
 * Each source axis is scaled against the destination axis it ends up on, so a
 * 90 degree rotation takes its source-y samples across the destination width.
 * This is the contract documented for DRM_N32H7XX_BLIT, written out by hand so
 * the hardware has something independent to disagree with.
 */
static void blit_src_texel(unsigned int dx, unsigned int dy, unsigned int w,
			   unsigned int h, unsigned int dw, unsigned int dh,
			   uint32_t rotation, unsigned int *sx,
			   unsigned int *sy)
{
	unsigned int x = (rotation & DRM_MODE_REFLECT_X) ? dw - 1 - dx : dx;
	unsigned int y = (rotation & DRM_MODE_REFLECT_Y) ? dh - 1 - dy : dy;
	/* Nearest sample of the axis, at the destination pixel centre. */
	unsigned int fx = ((2 * x + 1) * w) / (2 * dw);	/* x axis over width */
	unsigned int fy = ((2 * y + 1) * h) / (2 * dh);	/* y axis over height */
	unsigned int gx = ((2 * x + 1) * h) / (2 * dw);	/* y axis over width */
	unsigned int gy = ((2 * y + 1) * w) / (2 * dh);	/* x axis over height */

	switch (rotation & DRM_MODE_ROTATE_MASK) {
	case DRM_MODE_ROTATE_90:
		*sx = w - 1 - gy;
		*sy = gx;
		break;
	case DRM_MODE_ROTATE_180:
		*sx = w - 1 - fx;
		*sy = h - 1 - fy;
		break;
	case DRM_MODE_ROTATE_270:
		*sx = gy;
		*sy = h - 1 - gx;
		break;
	default:
		*sx = fx;
		*sy = fy;
	}
}

/* Nearest-equivalent pixel in the destination format. */
static uint32_t blit_convert565(uint16_t v, uint32_t format)
{
	if (format == DRM_FORMAT_RGB565)
		return v;
	return argb_of565(v);
}

struct blit_case {
	const char *name;
	unsigned int sw, sh, dw, dh;
	uint32_t rotation;
	uint32_t dst_format;
};

/*
 * Every case copies a 565 source into a destination that was first filled with
 * a marker value: a missing pixel, a shifted row and a write outside the block
 * all show up as a mismatch.
 */
static unsigned int blit_matrix(int gpu, uint32_t src_handle, void *smap,
				uint32_t dst_handle, void *dmap,
				unsigned int max_w, unsigned int max_h)
{
	static const struct blit_case cases[] = {
		{ "1:1",			64, 48,  64, 48, 0, DRM_FORMAT_RGB565 },
		{ "2x up",		64, 48, 128, 96, 0, DRM_FORMAT_RGB565 },
		{ "2x down",		64, 48,  32, 24, 0, DRM_FORMAT_RGB565 },
		{ "3:2 down",		64, 48,  43, 32, 0, DRM_FORMAT_RGB565 },
		{ "rotate 90",		64, 48,  48, 64, DRM_MODE_ROTATE_90,
		  DRM_FORMAT_RGB565 },
		{ "rotate 180",		64, 48,  64, 48, DRM_MODE_ROTATE_180,
		  DRM_FORMAT_RGB565 },
		{ "rotate 270",		64, 48,  48, 64, DRM_MODE_ROTATE_270,
		  DRM_FORMAT_RGB565 },
		{ "reflect x",		64, 48,  64, 48, DRM_MODE_REFLECT_X,
		  DRM_FORMAT_RGB565 },
		{ "reflect y",		64, 48,  64, 48, DRM_MODE_REFLECT_Y,
		  DRM_FORMAT_RGB565 },
		{ "rotate 90 + reflect x", 64, 48, 48, 64,
		  DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_X, DRM_FORMAT_RGB565 },
		{ "rotate 270 + reflect y", 64, 48, 48, 64,
		  DRM_MODE_ROTATE_270 | DRM_MODE_REFLECT_Y, DRM_FORMAT_RGB565 },
		{ "rotate 90 scaled",	64, 48,  96, 43, DRM_MODE_ROTATE_90,
		  DRM_FORMAT_RGB565 },
		{ "rotate 180 + reflect xy", 64, 48, 64, 48,
		  DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_X |
		  DRM_MODE_REFLECT_Y, DRM_FORMAT_RGB565 },
		{ "rotate 90 to xrgb8888", 64, 48, 48, 64, DRM_MODE_ROTATE_90,
		  DRM_FORMAT_XRGB8888 },
		{ "2x up to xrgb8888",	64, 48, 128, 96, 0, DRM_FORMAT_XRGB8888 },
	};
	struct drm_n32h7xx_blit blit = { 0 };
	unsigned int bad = 0, i, x, y;

	for (i = 0; i < max_w * max_h; i++)
		((uint16_t *)smap)[i] = blit_pattern(i % max_w, i / max_w);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct blit_case *c = &cases[i];
		unsigned int cpp = fourcc_cpp(c->dst_format);
		unsigned int wrong = 0;
		uint16_t *s = smap;

		memset(dmap, 0x5a, (size_t)max_w * max_h * 4);
		memset(&blit, 0, sizeof(blit));
		blit.src_handle = src_handle;
		blit.src_format = DRM_FORMAT_RGB565;
		blit.src_pitch = max_w * 2;
		blit.src_width = c->sw;
		blit.src_height = c->sh;
		blit.dst_handle = dst_handle;
		blit.dst_format = c->dst_format;
		blit.dst_pitch = max_w * cpp;
		blit.width = c->dw;
		blit.height = c->dh;
		blit.rotation = c->rotation;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
			printf("blit %-22s: %s\n", c->name, strerror(errno));
			bad++;
			continue;
		}
		for (y = 0; y < max_h; y++)
			for (x = 0; x < max_w; x++) {
				uint32_t want, got;
				unsigned int sx, sy;

				if (cpp == 2)
					got = ((uint16_t *)dmap)[y * max_w + x];
				else
					got = ((uint32_t *)dmap)[y * max_w + x];
				if (x >= c->dw || y >= c->dh) {
					if (got != (cpp == 2 ? 0x5a5a : 0x5a5a5a5a))
						wrong++;
					continue;
				}
				blit_src_texel(x, y, c->sw, c->sh, c->dw, c->dh,
					       c->rotation, &sx, &sy);
				want = blit_convert565(s[sy * max_w + sx], c->dst_format);
				if (cpp == 4)
					want |= 0xff000000u;
				else
					want &= 0xffff;
				if (got != want) {
					if (wrong < 3)
						printf("  %s: %u,%u got %08x want %08x\n",
						       c->name, x, y, got, want);
					wrong++;
				}
			}
		if (wrong)
			bad++;
		printf("blit %-22s %3ux%-3u -> %3ux%-3u rot %02x: %u wrong\n",
		       c->name, c->sw, c->sh, c->dw, c->dh, c->rotation, wrong);
	}
	return bad;
}

struct blit_blend_case {
	const char *name;
	unsigned int sw, sh, dw, dh;
	uint32_t rotation;
};

static uint32_t blit_alpha_pattern(unsigned int x, unsigned int y, unsigned int seed)
{
	unsigned int v = blit_pattern(x + seed, y + 2 * seed);

	return ((((x * 3 + y * 5 + seed * 41) & 0xff) << 24) |
		((v & 0xffff) << 8) | ((x * 2 + y * 3 + seed) & 0xff));
}

/*
 * Source-over through the texture path.  The source carries a per-pixel
 * alpha, the destination has its own alpha and colour pattern, and both are
 * checked against the software reference: a dropped texture alpha, a missing
 * destination read or a source alpha that only reaches the colour channels
 * all show up as a mismatch.  Pixels outside the block must stay untouched.
 */
static unsigned int blit_blend_verify(int gpu, uint32_t s_handle, void *smap,
				      uint32_t d_handle, void *dmap,
				      unsigned int max_w, unsigned int max_h,
				      const struct blit_blend_case *c)
{
	const unsigned int cpp = 4;
	struct drm_n32h7xx_blit blit = { 0 };
	uint32_t *s = smap, *d = dmap;
	unsigned int wrong = 0, x, y, i;

	for (i = 0; i < max_w * max_h; i++)
		s[i] = blit_alpha_pattern(i % max_w, i / max_w, 0);
	for (i = 0; i < max_w * max_h; i++)
		d[i] = blit_alpha_pattern(i % max_w, i / max_w, 1);

	blit.src_handle = s_handle;
	blit.src_format = DRM_FORMAT_ARGB8888;
	blit.src_pitch = max_w * cpp;
	blit.src_width = c->sw;
	blit.src_height = c->sh;
	blit.dst_handle = d_handle;
	blit.dst_format = DRM_FORMAT_ARGB8888;
	blit.dst_pitch = max_w * cpp;
	blit.width = c->dw;
	blit.height = c->dh;
	blit.rotation = c->rotation;
	blit.flags = DRM_N32H7XX_BLIT_BLEND;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
		printf("blit blend %-14s: %s\n", c->name, strerror(errno));
		return ~0u;
	}

	for (y = 0; y < max_h; y++)
		for (x = 0; x < max_w; x++) {
			uint32_t got = d[y * max_w + x];
			uint32_t ref = blit_alpha_pattern(x, y, 1);

			if (x >= c->dw || y >= c->dh) {
				if (got != ref)
					wrong++;
				continue;
			}
			{
				unsigned int sx, sy;
				uint32_t src;
				unsigned int sa, da, sh, bad = 0;

				blit_src_texel(x, y, c->sw, c->sh, c->dw, c->dh,
					       c->rotation, &sx, &sy);
				src = s[sy * max_w + sx];
				sa = (src >> 24) & 0xff;
				da = (ref >> 24) & 0xff;
				for (sh = 0; sh < 32; sh += 8) {
					unsigned int sc = (src >> sh) & 0xff;
					unsigned int dc = (ref >> sh) & 0xff;
					unsigned int want, g, delta;

					if (sh == 24)
						want = (sa * 255 + da * (255 - sa) + 127) / 255;
					else
						want = (sc * sa + dc * (255 - sa) + 127) / 255;
					g = (got >> sh) & 0xff;
					delta = g > want ? g - want : want - g;
					if (delta > 1)
						bad = 1;
				}
				if (bad) {
					if (wrong < 3)
						printf("  %s: %u,%u src %08x dst %08x got %08x\n",
						       c->name, x, y, src, ref, got);
					wrong++;
				}
			}
		}
	return wrong;
}

/*
 * 1:1 copies against software references.  The source is filled with a
 * pattern and the destination with a marker, so a missing pixel, a shifted
 * row and a write outside the block all show up as a mismatch.
 */
static int blit_test(int gpu, unsigned int width, unsigned int height,
		     unsigned int iters)
{
	struct drm_n32h7xx_create src = { 0 }, dst = { 0 };
	struct drm_n32h7xx_blit blit = { 0 };
	struct drm_gem_close close_bo = { 0 };
	const unsigned int rw = 100, rh = 50;
	unsigned int bad = 0, xff = 0, x00 = 0, xother = 0, n = 0, i;
	unsigned int sbad, wbad, cbad;
	struct timespec t0, t1;
	double blit_ms;
	uint16_t *s, *d;
	uint32_t *sw, *dw;
	void *smap, *dmap;
	int ret = -1;

	src.size = (uint64_t)width * height * 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &src)) {
		perror("GPU buffer allocation");
		return -1;
	}
	dst.size = src.size;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &dst)) {
		perror("GPU buffer allocation");
		goto out_src;
	}
	smap = mmap(NULL, src.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		    src.mmap_offset);
	dmap = mmap(NULL, dst.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		    dst.mmap_offset);
	if (smap == MAP_FAILED || dmap == MAP_FAILED) {
		perror("mmap");
		goto out;
	}
	s = smap;
	d = dmap;

	/* 1. RGB565 -> RGB565, full frame, must be bit exact. */
	for (i = 0; i < width * height; i++)
		s[i] = blit_pattern(i % width, i / width);
	for (i = 0; i < width * height; i++)
		d[i] = 0x5a5a;
	blit.src_handle = src.handle;
	blit.src_format = DRM_FORMAT_RGB565;
	blit.src_pitch = width * 2;
	blit.src_width = width;
	blit.src_height = height;
	blit.dst_handle = dst.handle;
	blit.dst_format = DRM_FORMAT_RGB565;
	blit.dst_pitch = width * 2;
	blit.width = width;
	blit.height = height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
		perror("GPU blit");
		goto out;
	}
	for (i = 0; i < width * height; i++)
		if (d[i] != s[i]) {
			if (!bad)
				printf("first mismatch at %u,%u: got %04x want %04x\n",
				       i % width, i / width, d[i], s[i]);
			bad++;
		}
	printf("blit rgb565->rgb565 %ux%u: %u of %u pixels wrong\n", width,
	       height, bad, width * height);

	/* 2. Sub-rectangle: both sides start at an offset in their buffer. */
	for (i = 0; i < width * height; i++)
		d[i] = 0x5a5a;
	blit.src_offset = (4 * width + 4) * 2;
	blit.dst_offset = (10 * width + 20) * 2;
	blit.width = rw;
	blit.height = rh;
	blit.src_width = rw;
	blit.src_height = rh;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
		perror("GPU blit");
		goto out;
	}
	wbad = 0;
	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++) {
			int inside = x >= 20 && x < 20 + rw && y >= 10 &&
				     y < 10 + rh;
			uint16_t want = inside ?
				s[(y - 10 + 4) * width + (x - 20 + 4)] : 0x5a5a;

			if (d[y * width + x] != want) {
				if (wbad++ < 6)
					printf("  window %u,%u: got %04x want %04x\n",
					       x, y, d[y * width + x], want);
			}
		}
	bad += wbad;
	printf("blit %ux%u window at 20,10 from 4,4: %u pixels wrong\n", rw, rh,
	       wbad);

	/* 3. Benchmark the full-frame copy. */
	blit.src_offset = 0;
	blit.dst_offset = 0;
	blit.width = width;
	blit.height = height;
	blit.src_width = width;
	blit.src_height = height;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++)
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit))
			break;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	blit_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		  (t1.tv_nsec - t0.tv_nsec) / 1e6;
	blit_ms /= iters;
	printf("full %ux%u RGB565 blit: %.2f ms (%.1f MB/s read+write)\n",
	       width, height, blit_ms,
	       width * height * 2 * 2 / blit_ms / 1000.0);

	/* 3b. The same frame rotated by 90 degrees: every destination row walks
	 *     a column of the source, which is the case the enumeration stripes
	 *     exist for.  Time it here so a regression in that path shows up.
	 */
	blit.width = height;
	blit.height = width;
	blit.dst_pitch = height * 2;
	blit.rotation = DRM_MODE_ROTATE_90;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
		perror("GPU blit rotate 90");
		bad++;
		goto skip_rot;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++)
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit))
			break;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	blit_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		  (t1.tv_nsec - t0.tv_nsec) / 1e6;
	blit_ms /= iters;
	printf("full %ux%u RGB565 rotate 90: %.2f ms (%.1f Mpixel/s)\n",
	       width, height, blit_ms, width * height / blit_ms / 1000.0);
skip_rot:
	blit.width = width;
	blit.height = height;
	blit.dst_pitch = width * 2;
	blit.rotation = 0;
	munmap(dmap, dst.size);
	munmap(smap, src.size);
	close_bo.handle = dst.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	close_bo.handle = src.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);

	/*
	 * 4. Format conversion and the geometry matrix, at half size: two full
	 * 32-bit frames at once would need two order-10 allocations.
	 */
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	src.size = (uint64_t)(width / 2) * (height / 2) * 4;
	dst.size = (uint64_t)(width / 2) * (height / 2) * 4;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &src) ||
	    ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &dst)) {
		perror("GPU buffer allocation");
		return -1;
	}
	smap = mmap(NULL, src.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		    src.mmap_offset);
	dmap = mmap(NULL, dst.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu,
		    dst.mmap_offset);
	if (smap == MAP_FAILED || dmap == MAP_FAILED) {
		perror("mmap");
		goto out;
	}

	bad += blit_matrix(gpu, src.handle, smap, dst.handle, dmap, width / 2,
			   height / 2);

	{
		static const struct blit_blend_case bcases[] = {
			{ "blend 1:1",    100, 60, 100, 60, 0 },
			{ "blend 2x up",   60, 40, 120, 80, 0 },
			{ "blend rot 90", 120, 80,  80, 120, DRM_MODE_ROTATE_90 },
		};
		unsigned int b;

		for (b = 0; b < ARRAY_SIZE(bcases); b++) {
			unsigned int wrong = blit_blend_verify(gpu, src.handle, smap,
							       dst.handle, dmap,
							       width / 2, height / 2,
							       &bcases[b]);

			printf("blit %-22s %3ux%-3u -> %3ux%-3u rot %02x: %u wrong\n",
			       bcases[b].name, bcases[b].sw, bcases[b].sh,
			       bcases[b].dw, bcases[b].dh, bcases[b].rotation,
			       wrong);
			bad += !!wrong;
		}

		/* An opaque copy of the same ARGB source must ignore its alpha
		 * and write alpha 1.0.
		 */
		{
			uint32_t *sp = smap, *dp = dmap;
			unsigned int wrong = 0, i;

			for (i = 0; i < width / 2 * (height / 2); i++) {
				sp[i] = blit_alpha_pattern(i % (width / 2),
							   i / (width / 2), 2);
				dp[i] = 0x5a5a5a5a;
			}
			memset(&blit, 0, sizeof(blit));
			blit.src_handle = src.handle;
			blit.src_format = DRM_FORMAT_ARGB8888;
			blit.src_pitch = width / 2 * 4;
			blit.src_width = width / 2;
			blit.src_height = height / 2;
			blit.dst_handle = dst.handle;
			blit.dst_format = DRM_FORMAT_ARGB8888;
			blit.dst_pitch = width / 2 * 4;
			blit.width = width / 2;
			blit.height = height / 2;
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
				printf("blit opaque argb: %s\n", strerror(errno));
				bad++;
			} else {
				for (i = 0; i < width / 2 * (height / 2); i++)
					if (dp[i] != (sp[i] | 0xff000000u))
						wrong++;
				printf("blit opaque ARGB source: %u pixels wrong\n",
				       wrong);
				bad += !!wrong;
			}
		}

		/* Blending needs a sampled alpha: RGB565 and XRGB8888 sources
		 * must be refused rather than treated as fully transparent.
		 */
		{
			static const uint32_t noalpha[] = { DRM_FORMAT_RGB565,
							    DRM_FORMAT_XRGB8888 };
			unsigned int k;

			for (k = 0; k < ARRAY_SIZE(noalpha); k++) {
				memset(&blit, 0, sizeof(blit));
				blit.src_handle = src.handle;
				blit.src_format = noalpha[k];
				blit.src_pitch = width / 2 * 4;
				blit.src_width = 16;
				blit.src_height = 16;
				blit.dst_handle = dst.handle;
				blit.dst_format = DRM_FORMAT_ARGB8888;
				blit.dst_pitch = width / 2 * 4;
				blit.width = 16;
				blit.height = 16;
				blit.flags = DRM_N32H7XX_BLIT_BLEND;
				if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 ||
				    errno != EINVAL) {
					printf("blit blend %08x source: not refused\n",
					       noalpha[k]);
					bad++;
				}
			}
			printf("blit blend rejects sources without alpha: ok\n");
		}

		/* Timing: the same ARGB8888 frame copied opaquely and composited.
		 * The difference is the destination read the blend needs.
		 */
		{
			unsigned int it, n = 20;
			double t_opaque, t_blend;
			struct timespec t0, t1;

			memset(&blit, 0, sizeof(blit));
			blit.src_handle = src.handle;
			blit.src_format = DRM_FORMAT_ARGB8888;
			blit.src_pitch = width / 2 * 4;
			blit.src_width = width / 2;
			blit.src_height = height / 2;
			blit.dst_handle = dst.handle;
			blit.dst_format = DRM_FORMAT_ARGB8888;
			blit.dst_pitch = width / 2 * 4;
			blit.width = width / 2;
			blit.height = height / 2;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			for (it = 0; it < n; it++)
				ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			t_opaque = ((t1.tv_sec - t0.tv_sec) * 1000.0 +
				    (t1.tv_nsec - t0.tv_nsec) / 1e6) / n;
			blit.flags = DRM_N32H7XX_BLIT_BLEND;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			for (it = 0; it < n; it++)
				ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			t_blend = ((t1.tv_sec - t0.tv_sec) * 1000.0 +
				   (t1.tv_nsec - t0.tv_nsec) / 1e6) / n;
			blit.flags = 0;
			printf("full %ux%u ARGB8888 blit: opaque %.2f ms, blend %.2f ms\n",
			       width / 2, height / 2, t_opaque, t_blend);
		}
	}

	s = smap;
	dw = dmap;
	for (i = 0; i < width / 2 * (height / 2); i++)
		s[i] = blit_pattern(i % (width / 2), i / (width / 2));
	for (i = 0; i < width / 2 * (height / 2); i++)
		dw[i] = 0x5a5a5a5a;
	memset(&blit, 0, sizeof(blit));
	blit.src_handle = src.handle;
	blit.src_format = DRM_FORMAT_RGB565;
	blit.src_pitch = width / 2 * 2;
	blit.src_width = width / 2;
	blit.src_height = height / 2;
	blit.dst_handle = dst.handle;
	blit.dst_format = DRM_FORMAT_XRGB8888;
	blit.dst_pitch = width / 2 * 4;
	blit.width = width / 2;
	blit.height = height / 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
		perror("GPU blit rgb565->xrgb8888");
		goto out;
	}
	cbad = 0;
	for (i = 0; i < width / 2 * (height / 2); i++) {
		uint32_t want = argb_of565(s[i]);

		if ((dw[i] & 0xffffff) != (want & 0xffffff))
			cbad++;
		if ((dw[i] >> 24) == 0xff)
			xff++;
		else if ((dw[i] >> 24) == 0x00)
			x00++;
		else
			xother++;
		n++;
	}
	bad += cbad;
	printf("blit rgb565->xrgb8888 %ux%u: %u of %u rgb wrong, "
	       "x byte ff/00/other %u/%u/%u\n", width / 2, height / 2, cbad, n,
	       xff, x00, xother);

	/* 5. XRGB8888 -> RGB565. */
	sw = dmap;
	for (i = 0; i < width / 2 * (height / 2); i++)
		sw[i] = 0xff000000u | ((i * 37u) & 0xff) << 16 |
			((i * 91u) & 0xff) << 8 | ((i * 17u) & 0xff);
	for (i = 0; i < width / 2 * (height / 2); i++)
		s[i] = 0x5a5a;
	blit.src_handle = dst.handle;
	blit.src_format = DRM_FORMAT_XRGB8888;
	blit.src_pitch = width / 2 * 4;
	blit.dst_handle = src.handle;
	blit.dst_format = DRM_FORMAT_RGB565;
	blit.dst_pitch = width / 2 * 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
		perror("GPU blit xrgb8888->rgb565");
		goto out;
	}
	cbad = 0;
	for (i = 0; i < width / 2 * (height / 2); i++) {
		uint32_t v = sw[i];
		uint16_t want = (uint16_t)(((((v >> 16) & 0xff) >> 3) << 11) |
					   ((((v >> 8) & 0xff) >> 2) << 5) |
					   ((v & 0xff) >> 3));

		if (s[i] != want)
			cbad++;
	}
	bad += cbad;
	printf("blit xrgb8888->rgb565 %ux%u: %u pixels wrong\n", width / 2,
	       height / 2, cbad);

	/* 6. ARGB8888 both sides: the source alpha is discarded, so the copy
	 *    is opaque and the destination alpha reads back as 0xff.
	 */
	sw = dmap;
	for (i = 0; i < width / 2 * (height / 2); i++)
		sw[i] = ((i * 7u) & 0xff) << 24 | ((i * 13u) & 0xff) << 16 |
			((i * 29u) & 0xff) << 8 | ((i * 41u) & 0xff);
	{
		uint32_t *dst32 = smap;

		for (i = 0; i < width / 2 * (height / 2); i++)
			dst32[i] = 0x5a5a5a5a;
		blit.src_handle = dst.handle;
		blit.src_format = DRM_FORMAT_ARGB8888;
		blit.src_pitch = width / 2 * 4;
		blit.dst_handle = src.handle;
		blit.dst_format = DRM_FORMAT_ARGB8888;
		blit.dst_pitch = width / 2 * 4;
		blit.src_width = width / 2;
		blit.src_height = height / 2;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
			perror("GPU blit argb8888->argb8888");
			goto out;
		}
		cbad = 0;
		for (i = 0; i < width / 2 * (height / 2); i++) {
			uint32_t want = (sw[i] & 0xffffff) | 0xff000000u;

			if (dst32[i] != want)
				cbad++;
		}
		bad += cbad;
		printf("blit argb8888->argb8888 %ux%u: %u pixels wrong\n",
		       width / 2, height / 2, cbad);
	}

	/* 7. Arguments the driver must reject. */
	sbad = 0;
	blit.flags = 0x10;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	blit.flags = 0;
	blit.width = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	blit.width = width / 2;
	blit.dst_format = 0x12345678;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	blit.dst_format = DRM_FORMAT_ARGB8888;
	blit.src_handle = blit.dst_handle;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	blit.src_handle = dst.handle;
	blit.rotation = DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_180;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	blit.rotation = 1u << 6;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	blit.rotation = 0;
	blit.src_width = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit) == 0 || errno != EINVAL)
		sbad++;
	bad += sbad;
	printf("blit argument checks: %s\n", sbad ? "FAIL" : "ok");
	munmap(dmap, dst.size);
	munmap(smap, src.size);
	close_bo.handle = dst.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	close_bo.handle = src.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	printf("blit: %s\n", bad ? "FAIL" : "PASS");
	return bad ? -1 : 0;
out:
	munmap(dmap, dst.size);
	munmap(smap, src.size);
	close_bo.handle = dst.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
out_src:
	close_bo.handle = src.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/*
 * P6.1: every pixel format the driver exposes.  Shifts and widths describe
 * how a channel is packed inside the pixel word, so the same table can build
 * both the expected raw pixel and the software reference for a conversion.
 * abits is zero for formats without an alpha channel.
 */
struct fmt_case {
	uint32_t format;
	const char *name;
	unsigned int cpp;
	unsigned int rbits, gbits, bbits, abits;
	unsigned int rshift, gshift, bshift, ashift;
	uint32_t fill_raw;
};

static const struct fmt_case fmt_cases[] = {
	{ DRM_FORMAT_RGB565,   "RGB565",   2, 5, 6, 5, 0, 11,  5,  0,  0,
	  0x1234 },
	{ DRM_FORMAT_XRGB8888, "XRGB8888", 4, 8, 8, 8, 0, 16,  8,  0,  0,
	  0x00123456 },
	{ DRM_FORMAT_ARGB8888, "ARGB8888", 4, 8, 8, 8, 8, 16,  8,  0, 24,
	  0x78123456 },
	{ DRM_FORMAT_RGBA8888, "RGBA8888", 4, 8, 8, 8, 8, 24, 16,  8,  0,
	  0x78123456 },
	{ DRM_FORMAT_ARGB4444, "ARGB4444", 2, 4, 4, 4, 4,  8,  4,  0, 12,
	  0x1234 },
	{ DRM_FORMAT_RGBA4444, "RGBA4444", 2, 4, 4, 4, 4, 12,  8,  4,  0,
	  0x1234 },
	{ DRM_FORMAT_ARGB1555, "ARGB1555", 2, 5, 5, 5, 1, 10,  5,  0, 15,
	  0x1234 },
	{ DRM_FORMAT_RGBA5551, "RGBA5551", 2, 5, 5, 5, 1, 11,  6,  1,  0,
	  0x1234 },
};

/* Pack an ARGB8888 colour into a format, truncating like the hardware. */
static uint32_t fmt_pack(const struct fmt_case *c, uint32_t argb)
{
	unsigned int a = (argb >> 24) & 0xff, r = (argb >> 16) & 0xff;
	unsigned int g = (argb >> 8) & 0xff, b = argb & 0xff;
	uint32_t v = 0;

	v |= (uint32_t)(r >> (8 - c->rbits)) << c->rshift;
	v |= (uint32_t)(g >> (8 - c->gbits)) << c->gshift;
	v |= (uint32_t)(b >> (8 - c->bbits)) << c->bshift;
	if (c->abits)
		v |= (uint32_t)(a >> (8 - c->abits)) << c->ashift;
	return v;
}

/*
 * Widen a channel to eight bits the way the hardware does: replicate the
 * high bits into the low ones.  Widening and then truncating to a narrower
 * channel gives the same result as a direct bit replication, so one helper
 * covers every conversion in the test.
 */
static unsigned int fmt_expand(unsigned int v, unsigned int bits)
{
	if (bits >= 8)
		return v;
	if (2 * bits < 8)
		return v << (8 - bits);
	return (v << (8 - bits)) | (v >> (2 * bits - 8));
}

static uint32_t fmt_pixel_of(const struct fmt_case *c, unsigned int x,
			     unsigned int y)
{
	unsigned int rm = (1U << c->rbits) - 1, gm = (1U << c->gbits) - 1;
	unsigned int bm = (1U << c->bbits) - 1;
	unsigned int am = c->abits ? (1U << c->abits) - 1 : 0;
	unsigned int r = (x * 5 + y * 3) & rm;
	unsigned int g = (x * 3 + y * 7) & gm;
	unsigned int b = (x * 11 + y * 5) & bm;
	unsigned int a = am ? (am - 1) & (x + y) : 0;

	return (r << c->rshift) | (g << c->gshift) | (b << c->bshift) |
	       (a << c->ashift);
}

/* Same pixel as a 565 destination should hold once the texel is widened. */
static uint16_t fmt_to565(const struct fmt_case *c, uint32_t v)
{
	unsigned int r = (v >> c->rshift) & ((1U << c->rbits) - 1);
	unsigned int g = (v >> c->gshift) & ((1U << c->gbits) - 1);
	unsigned int b = (v >> c->bshift) & ((1U << c->bbits) - 1);

	r = fmt_expand(r, c->rbits) >> 3;
	g = fmt_expand(g, c->gbits) >> 2;
	b = fmt_expand(b, c->bbits) >> 3;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint32_t fmt_read(void *map, unsigned int cpp, unsigned int idx)
{
	return cpp == 2 ? ((uint16_t *)map)[idx] : ((uint32_t *)map)[idx];
}

#define FMT_W	128
#define FMT_H	64

static int format_test(int gpu)
{
	const unsigned int pitch = FMT_W * 4;
	const uint32_t rect_argb = 0xc0304080;	/* alpha forced opaque */
	unsigned int bad = 0, i;
	void *map;
	int ret = -1;

	{
		struct drm_n32h7xx_create create = { 0 };

		create.size = (uint64_t)pitch * FMT_H;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
			perror("GPU buffer allocation");
			return -1;
		}
		map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, gpu, create.mmap_offset);
		if (map == MAP_FAILED) {
			perror("mmap");
			return -1;
		}
		for (i = 0; i < ARRAY_SIZE(fmt_cases); i++) {
			const struct fmt_case *c = &fmt_cases[i];
			struct drm_n32h7xx_fill fill = { 0 };
			struct drm_n32h7xx_rect rect = { 0 };
			struct drm_n32h7xx_blend blend = { 0 };
			unsigned int bad_fill = 0, bad_in = 0, bad_out = 0;
			uint32_t want, want_raw;
			unsigned int x, y;

			/* 1. The clear engine writes raw pixels. */
			memset(map, 0x5a, create.size);
			fill.handle = create.handle;
			fill.format = c->format;
			fill.pitch = FMT_W * c->cpp;
			fill.width = FMT_W;
			fill.height = FMT_H;
			fill.color = c->fill_raw;
			want_raw = c->cpp == 2 ? c->fill_raw & 0xffff :
						 c->fill_raw;
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
				printf("fmt %-9s fill: %s\n", c->name,
				       strerror(errno));
				bad++;
				continue;
			}
			for (x = 0; x < FMT_W * FMT_H; x++)
				if (fmt_read(map, c->cpp, x) != want_raw)
					bad_fill++;

			/* 2. The pixel pipeline converts an ARGB8888 colour. */
			memset(map, 0x5a, create.size);
			rect.handle = create.handle;
			rect.format = c->format;
			rect.pitch = FMT_W * c->cpp;
			rect.offset = 8 * rect.pitch + 4 * c->cpp;
			rect.width = FMT_W - 16;
			rect.height = FMT_H - 16;
			rect.color = rect_argb;
			want = fmt_pack(c, rect_argb | 0xff000000u);
			/* The pipeline writes alpha 1.0, padding byte included. */
			if (!c->abits && c->cpp == 4)
				want |= 0xff000000u;
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect)) {
				printf("fmt %-9s rect: %s\n", c->name,
				       strerror(errno));
				bad++;
				continue;
			}
			for (y = 0; y < FMT_H; y++)
				for (x = 0; x < FMT_W; x++) {
					uint32_t v;

					v = fmt_read(map, c->cpp,
						     y * FMT_W + x);
					if (x >= 4 && x < 4 + rect.width &&
					    y >= 8 && y < 8 + rect.height) {
						if (v != want) {
							if (!bad_in)
								printf("  %s: %u,%u rect got %08x want %08x\n",
								       c->name, x, y, v, want);
							bad_in++;
						}
					} else if (v != (c->cpp == 2 ? 0x5a5a :
							   0x5a5a5a5a)) {
						bad_out++;
					}
				}

			/* 3. Compositing is accepted for every format,
			 *    including the ones that store alpha.  The value
			 *    written there is checked by blend_test.
			 */
			blend.handle = create.handle;
			blend.format = c->format;
			blend.pitch = FMT_W * c->cpp;
			blend.width = 16;
			blend.height = 16;
			blend.color = 0x80ff8000;
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLEND, &blend)) {
				printf("fmt %-9s blend: %s\n", c->name,
				       strerror(errno));
				bad++;
			}

			printf("fmt %-9s fill %u, rect %u in %u out, blend %s\n",
			       c->name, bad_fill, bad_in, bad_out,
			       "ok");
			if (bad_fill || bad_in || bad_out)
				bad++;
		}
		munmap(map, create.size);
		{
			struct drm_gem_close close_bo = { 0 };

			close_bo.handle = create.handle;
			ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		}
	}

	/* 4. Blit: same format round trip, then a 565 destination to prove
	 *    the texture unit reads the bits where the table says they are.
	 */
	for (i = 0; i < ARRAY_SIZE(fmt_cases); i++) {
		const struct fmt_case *c = &fmt_cases[i];
		struct drm_n32h7xx_create sa = { 0 }, da = { 0 };
		struct drm_n32h7xx_blit blit = { 0 };
		struct drm_gem_close close_bo = { 0 };
		unsigned int wrong_same = 0, wrong_565 = 0;
		unsigned int x, y, idx;
		void *smap, *dmap;

		sa.size = (uint64_t)FMT_W * c->cpp * FMT_H;
		da.size = (uint64_t)FMT_W * 4 * FMT_H;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &sa) ||
		    ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &da)) {
			perror("GPU buffer allocation");
			return -1;
		}
		smap = mmap(NULL, sa.size, PROT_READ | PROT_WRITE, MAP_SHARED,
			    gpu, sa.mmap_offset);
		dmap = mmap(NULL, da.size, PROT_READ | PROT_WRITE, MAP_SHARED,
			    gpu, da.mmap_offset);
		if (smap == MAP_FAILED || dmap == MAP_FAILED) {
			perror("mmap");
			return -1;
		}
		for (y = 0; y < FMT_H; y++)
			for (x = 0; x < FMT_W; x++) {
				uint32_t v = fmt_pixel_of(c, x, y);

				if (c->cpp == 2)
					((uint16_t *)smap)[y * FMT_W + x] = v;
				else
					((uint32_t *)smap)[y * FMT_W + x] = v;
			}

		/* 4a. same format in and out */
		memset(dmap, 0x5a, da.size);
		blit.src_handle = sa.handle;
		blit.src_format = c->format;
		blit.src_pitch = FMT_W * c->cpp;
		blit.src_width = FMT_W;
		blit.src_height = FMT_H;
		blit.dst_handle = da.handle;
		blit.dst_format = c->format;
		blit.dst_pitch = FMT_W * c->cpp;
		blit.width = FMT_W;
		blit.height = FMT_H;
		if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
			printf("fmt %-9s blit same: %s\n", c->name,
			       strerror(errno));
			bad++;
		} else {
			for (idx = 0; idx < FMT_W * FMT_H; idx++) {
				uint32_t got = fmt_read(dmap, c->cpp, idx);
				uint32_t want = fmt_read(smap, c->cpp, idx);
				/* The copy is opaque: alpha becomes all ones, and
				 * the unused X byte of XRGB8888 is set too.
				 */
				if (c->abits)
					want |= ((1U << c->abits) - 1) << c->ashift;
				else if (c->cpp == 4)
					want |= 0xff000000u;
				if (got != want)
					wrong_same++;
			}
		}

		/* 4b. source format -> RGB565 */
		if (c->format != DRM_FORMAT_RGB565) {
			memset(dmap, 0x5a, da.size);
			blit.dst_format = DRM_FORMAT_RGB565;
			blit.dst_pitch = FMT_W * 2;
			if (ioctl(gpu, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
				printf("fmt %-9s blit to 565: %s\n", c->name,
				       strerror(errno));
				bad++;
			} else {
				for (idx = 0; idx < FMT_W * FMT_H; idx++) {
					uint32_t src = fmt_read(smap, c->cpp,
								idx);
					uint16_t want = fmt_to565(c, src);

					if (((uint16_t *)dmap)[idx] != want)
						wrong_565++;
				}
			}
		}

		printf("fmt %-9s blit same %u wrong, to RGB565 %u wrong\n",
		       c->name, wrong_same, wrong_565);
		if (wrong_same || wrong_565)
			bad++;

		munmap(smap, sa.size);
		munmap(dmap, da.size);
		close_bo.handle = sa.handle;
		ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		close_bo.handle = da.handle;
		ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	}

	printf("formats: %u failing cases\n", bad);
	ret = bad ? -1 : 0;
	return ret;
}

/*
 * P6.2: the blend unit dithers towards 16-bit destinations.  A source red
 * value of 4 is half a 5-bit LSB, so the 4x4 matrix must turn half of the
 * pixels into red 1 and leave the rest at zero.
 */
#define DITHER_W	64
#define DITHER_H	32

static int dither_test(int gpu)
{
	const unsigned int pitch = DITHER_W * 2;
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_n32h7xx_rect rect = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int bad = 0, x, y, bx, by, dx, dy;
	uint16_t tmpl[4][4];
	uint16_t *px;
	void *map;
	int ret = -1;

	create.size = (uint64_t)pitch * DITHER_H;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   gpu, create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		close_bo.handle = create.handle;
		ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		return -1;
	}
	px = map;

	fill.handle = create.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = DITHER_W;
	fill.height = DITHER_H;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("dither clear");
		goto out;
	}

	rect.handle = create.handle;
	rect.format = DRM_FORMAT_RGB565;
	rect.pitch = pitch;
	rect.offset = 8 * pitch;
	rect.width = 32;
	rect.height = 16;
	rect.color = 0x00040000;
	rect.flags = DRM_N32H7XX_DITHER;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect)) {
		perror("dither rect");
		goto out;
	}

	for (y = 0; y < DITHER_H; y++)
		for (x = 0; x < DITHER_W; x++) {
			uint16_t v = px[y * DITHER_W + x];

			if (y >= 8 && y < 8 + rect.height && x < rect.width) {
				if (v != 0 && v != 0x0800)
					bad++;
			} else if (v) {
				bad++;
			}
		}
	printf("dither pattern values: %s\n", bad ? "FAIL" : "ok");

	for (by = 8; by < 8 + rect.height; by += 4)
		for (bx = 0; bx < rect.width; bx += 4) {
			unsigned int ones = 0;

			for (dy = 0; dy < 4; dy++)
				for (dx = 0; dx < 4; dx++)
					if (px[(by + dy) * DITHER_W + bx + dx])
						ones++;
			if (ones < 7 || ones > 9) {
				printf("  block %u,%u: %u red pixels\n",
				       bx, by, ones);
				bad++;
			}
		}
	printf("dither 4x4 blocks: %s\n", bad ? "FAIL" : "ok");

	for (dy = 0; dy < 4; dy++)
		for (dx = 0; dx < 4; dx++)
			tmpl[dy][dx] = px[(8 + dy) * DITHER_W + dx];

	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("dither clear");
		goto out;
	}
	rect.offset = 13 * pitch + 2 * 2;
	rect.width = 13;
	rect.height = 9;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect)) {
		perror("dither rect");
		goto out;
	}
	{
		unsigned int bad_phase = 0;

		for (y = 13; y < 22; y++)
			for (x = 2; x < 15; x++)
				if (px[y * DITHER_W + x] != tmpl[y & 3][x & 3])
					bad_phase++;
		printf("dither absolute phase: %u wrong\n", bad_phase);
		bad += bad_phase;
	}

	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("dither clear");
		goto out;
	}
	rect.offset = 13 * pitch + 5 * 2;
	rect.width = 9;
	rect.height = 7;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect)) {
		perror("dither rect");
		goto out;
	}
	{
		unsigned int bad_phase = 0;

		for (y = 13; y < 20; y++)
			for (x = 5; x < 14; x++)
				if (px[y * DITHER_W + x] != tmpl[y & 3][x & 3])
					bad_phase++;
		printf("dither unaligned phase: %u wrong\n", bad_phase);
		bad += bad_phase;
	}

	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("dither clear");
		goto out;
	}
	rect.offset = 8 * pitch;
	rect.width = 32;
	rect.height = 16;
	rect.flags = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect)) {
		perror("dither rect off");
		goto out;
	}
	{
		unsigned int lit = 0;

		for (y = 8; y < 24; y++)
			for (x = 0; x < 32; x++)
				if (px[y * DITHER_W + x])
					lit++;
		printf("dither disabled: %u lit pixels\n", lit);
		bad += lit;
	}

	rect.format = DRM_FORMAT_XRGB8888;
	rect.pitch = 16 * 4;
	rect.offset = 0;
	rect.width = 16;
	rect.height = 16;
	rect.flags = DRM_N32H7XX_DITHER;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect) == 0 || errno != EINVAL) {
		printf("dither accepted on XRGB8888\n");
		bad++;
	}
	rect.format = DRM_FORMAT_RGB565;
	rect.pitch = pitch;
	rect.flags = 0x80;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect) == 0 || errno != EINVAL) {
		printf("unknown flag accepted\n");
		bad++;
	}
	rect.pitch = 0;
	rect.flags = DRM_N32H7XX_DITHER;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_RECT, &rect) == 0 || errno != EINVAL) {
		printf("zero pitch accepted\n");
		bad++;
	}

	printf("dither: %s\n", bad ? "FAIL" : "PASS");
	ret = bad ? -1 : 0;
out:
	munmap(map, create.size);
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/*
 * P6.3: the four D/AVE HD performance counters are not memory mapped.  The
 * driver dumps them into its own page, so the ioctl returns plain numbers.
 * Event 9 counts visible pixels and event 35 framebuffer pixel writes, both
 * exactly width * height * iterations; event 69 counts every clock, which
 * gives the cost of one rectangle when divided by the iteration count.
 */
static int profile_test(int gpu, unsigned int width, unsigned int height,
			unsigned int iters)
{
	const unsigned int pitch = width * 2;
	const uint32_t color = 0x00c04020;
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_n32h7xx_profile prof = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned long long drawn = (unsigned long long)width * height * iters;
	unsigned int bad = 0, i, x, y;
	uint16_t want = argb565(color), *px;
	void *map;
	int ret = -1;

	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   gpu, create.mmap_offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		close_bo.handle = create.handle;
		ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		return -1;
	}
	px = map;

	fill.handle = create.handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("profile clear");
		goto out;
	}

	prof.handle = create.handle;
	prof.format = DRM_FORMAT_RGB565;
	prof.pitch = pitch;
	prof.width = width;
	prof.height = height;
	prof.color = color;
	prof.iterations = iters;
	prof.event[0] = 9;
	prof.event[1] = 35;
	prof.event[2] = 69;
	prof.event[3] = DRM_N32H7XX_EVENT_OFF;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_PROFILE, &prof)) {
		perror("profile");
		goto out;
	}
	printf("profile %ux%u x%u: pixels %u, writes %u, clocks %u "
	       "(%u per rect), off %u\n", width, height, iters,
	       prof.counter[0], prof.counter[1], prof.counter[2],
	       prof.counter[2] / iters, prof.counter[3]);
	if (prof.counter[0] != drawn) {
		printf("visible pixels %u, expected %llu\n",
		       prof.counter[0], drawn);
		bad++;
	}
	if (prof.counter[1] != drawn) {
		printf("framebuffer writes %u, expected %llu\n",
		       prof.counter[1], drawn);
		bad++;
	}
	if (prof.counter[3]) {
		printf("disabled counter reported %u\n", prof.counter[3]);
		bad++;
	}
	if (!prof.counter[2]) {
		printf("clock counter stayed at zero\n");
		bad++;
	}

	/* Profiling must still draw the rectangle the caller asked for. */
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			if (px[y * width + x] != want) {
				bad++;
				y = height;
				break;
			}
	printf("profile pixels: %s\n", bad ? "FAIL" : "ok");

	/* Every counter off: the draw still happens and reports zeros. */
	fill.color = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill))
		perror("profile clear");
	for (i = 0; i < 4; i++)
		prof.event[i] = DRM_N32H7XX_EVENT_OFF;
	prof.iterations = 2;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_PROFILE, &prof)) {
		perror("profile with no events");
		bad++;
	}
	for (i = 0; i < 4; i++)
		if (prof.counter[i]) {
			printf("counter %u reported %u with no event\n",
			       i, prof.counter[i]);
			bad++;
		}
	if (px[0] != want) {
		printf("draw skipped when no event was selected\n");
		bad++;
	}
	printf("profile counters off: %s\n", bad ? "FAIL" : "ok");

	prof.event[0] = 9;
	prof.event[1] = 35;
	prof.event[2] = 69;
	prof.iterations = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_PROFILE, &prof) == 0 ||
	    errno != EINVAL) {
		printf("zero iterations accepted\n");
		bad++;
	}
	prof.iterations = 4096;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_PROFILE, &prof) == 0 ||
	    errno != EINVAL) {
		printf("oversized iteration count accepted\n");
		bad++;
	}
	prof.iterations = 1;
	prof.event[0] = 70;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_PROFILE, &prof) == 0 ||
	    errno != EINVAL) {
		printf("event 70 accepted\n");
		bad++;
	}
	prof.event[0] = 9;
	prof.width = 0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_PROFILE, &prof) == 0 ||
	    errno != EINVAL) {
		printf("zero width accepted\n");
		bad++;
	}

	printf("profile: %s\n", bad ? "FAIL" : "PASS");
	ret = bad ? -1 : 0;
out:
	munmap(map, create.size);
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

static int bench_test(int fd, const char *gpu_node)
{
	uint32_t crtc_id, old_fb, width, height;
	int gpu, master, ret = 0;

	if (crtc_state(fd, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		return -1;
	}
	gpu = open(gpu_node, O_RDWR | O_CLOEXEC);
	if (gpu < 0) {
		perror(gpu_node);
		return -1;
	}
	master = ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0;

	puts("Panel timing:");
	if (perf_vblank(fd, 120))
		ret = -1;
	puts("CPU reference (32-bit stores into the fbdev buffer):");
	if (perf_cpu_fill(fd, 30))
		ret = -1;

	puts("GPU fill, LCDC scanning the console framebuffer throughout:");
	if (bench_case(gpu, DRM_FORMAT_RGB565, width, height, width * 2, 400))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_XRGB8888, width, height, width * 4, 200))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_ARGB8888, width, height, width * 4, 200))
		ret = -1;
	puts("GPU fill, area scaling:");
	if (bench_case(gpu, DRM_FORMAT_RGB565, width / 2, height / 2,
		       width, 800))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_RGB565, 320, 240, 640, 800))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_RGB565, 160, 120, 320, 2000))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_RGB565, 64, 64, 128, 2000))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_RGB565, 16, 16, 32, 2000))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_RGB565, 1, 1, 2, 2000))
		ret = -1;
	puts("GPU fill, same bytes in different shapes (1.5 MB):");
	if (bench_case(gpu, DRM_FORMAT_RGB565, 1600, 480, 3200, 200))
		ret = -1;
	if (bench_case(gpu, DRM_FORMAT_RGB565, 800, 960, 1600, 200))
		ret = -1;
	puts("GPU fill, pitch inside a wider buffer:");
	if (bench_case(gpu, DRM_FORMAT_RGB565, 320, 240, 1600, 800))
		ret = -1;
	puts("GPU fill, sustained full frame:");
	if (bench_case(gpu, DRM_FORMAT_RGB565, width, height, width * 2, 1500))
		ret = -1;
	{
		struct drm_mode_card_res res = { 0 };
		struct drm_mode_crtc cur = { 0 };
		uint32_t conn_ids[4];

		res.count_connectors = ARRAY_SIZE(conn_ids);
		res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
		cur.crtc_id = crtc_id;
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
		    ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cur) || !cur.mode_valid)
			goto out;
		puts("GPU fill with the panel output disabled (no scanout load):");
		if (set_crtc(fd, crtc_id, 0, NULL, conn_ids[0])) {
			fprintf(stderr, "Cannot disable the LCDC for a reference run\n");
			goto out;
		}
		bench_case(gpu, DRM_FORMAT_RGB565, width, height, width * 2, 300);
		if (set_crtc(fd, crtc_id, old_fb, &cur.mode, conn_ids[0])) {
			fprintf(stderr, "Warning: could not restore the panel\n");
			ret = -1;
		}
	}
out:
	close(gpu);
	if (master)
		ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	return ret;
}

/* Enumerate what a GPU buffer and a fill may look like, and what is refused. */
static int compat_gpu(int gpu, uint32_t width, uint32_t height)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	struct drm_mode_card_res res = { 0 };
	struct drm_version version = { 0 };
	uint32_t handle;
	char name[64] = { 0 }, date[32] = { 0 }, desc[64] = { 0 };
	const uint32_t bad_formats[] = {
		DRM_FORMAT_RGB888, DRM_FORMAT_NV12, DRM_FORMAT_XBGR8888, 0,
	};
	unsigned int i, pitch = width * 2;
	int card_node;

	puts("Driver identity and node type:");
	version.name = name;
	version.name_len = sizeof(name) - 1;
	version.date = date;
	version.date_len = sizeof(date) - 1;
	version.desc = desc;
	version.desc_len = sizeof(desc) - 1;
	compat_expect_ok("DRM_IOCTL_VERSION", ioctl(gpu, DRM_IOCTL_VERSION, &version));
	printf("       %s %u.%u.%u, date %s\n", name, version.version_major,
	       version.version_minor, version.version_patchlevel, date);
	/* The render node refuses SET_MASTER; the card node accepts it. */
	card_node = ioctl(gpu, DRM_IOCTL_SET_MASTER, 0) == 0;
	if (card_node) {
		ioctl(gpu, DRM_IOCTL_DROP_MASTER, 0);
		compat_expect_errno("MODE_GETRESOURCES on the GPU card node is refused",
				    ioctl(gpu, DRM_IOCTL_MODE_GETRESOURCES, &res),
				    errno, EOPNOTSUPP);
		compat_check(1, "SET_MASTER is available on the GPU card node", NULL);
	} else {
		compat_expect_errno("MODE_GETRESOURCES on the render node is refused",
				    ioctl(gpu, DRM_IOCTL_MODE_GETRESOURCES, &res),
				    errno, EACCES);
		compat_expect_errno("SET_MASTER on the render node is refused",
				    ioctl(gpu, DRM_IOCTL_SET_MASTER, 0), errno, EACCES);
	}

	puts("Buffer creation limits (GPU_MAX_BUFFER is 4 MiB):");
	create.size = 0;
	compat_expect_errno("size 0", ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create),
			    errno, EINVAL);
	create.size = 4UL << 20;
	{
		int ret = ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create);
		char detail[80] = "";

		if (ret && errno == ENOMEM)
			snprintf(detail, sizeof(detail),
				 "-> shared graphics pool busy");
		else if (ret)
			snprintf(detail, sizeof(detail), "-> errno %d (%s)",
				 errno, strerror(errno));
		compat_check(ret == 0 || errno == ENOMEM,
			     "size 4 MiB is accepted by validation", detail);
	}
	if (create.handle) {
		close_bo.handle = create.handle;
		ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	}
	create.size = (4UL << 20) + 1;
	compat_expect_errno("size 4 MiB + 1",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create),
			    errno, EINVAL);
	create.size = 64 * 1024;
	create.flags = 1;
	compat_expect_errno("nonzero flags",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create),
			    errno, EINVAL);

	create.flags = 0;
	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	handle = create.handle;
	fill.handle = handle;
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;

	puts("Fill argument validation:");
	fill.reserved = 1;
	compat_expect_errno("reserved must be zero",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill), errno, EINVAL);
	fill.reserved = 0;
	fill.width = 0;
	compat_expect_errno("width 0", ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill),
			    errno, EINVAL);
	fill.width = width;
	fill.height = 0;
	compat_expect_errno("height 0", ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill),
			    errno, EINVAL);
	fill.height = height;
	fill.width = 4097;
	compat_expect_errno("width 4097", ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill),
			    errno, EINVAL);
	fill.width = width;
	fill.pitch = pitch - 1;
	compat_expect_errno("pitch not aligned to the pixel size",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill), errno, EINVAL);
	fill.pitch = pitch - 2;
	compat_expect_errno("pitch smaller than one row",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill), errno, EINVAL);
	fill.pitch = pitch;
	fill.offset = 1;
	compat_expect_errno("offset not aligned to the pixel size",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill), errno, EINVAL);
	fill.offset = create.size;
	compat_expect_errno("offset past the end of the buffer",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill), errno, EINVAL);
	fill.offset = 0;
	fill.handle = 0xdead;
	compat_expect_errno("unknown handle",
			    ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill), errno, ENOENT);
	fill.handle = handle;
	for (i = 0; i < ARRAY_SIZE(bad_formats); i++) {
		char what[64];

		fill.format = bad_formats[i];
		snprintf(what, sizeof(what), "format %#x rejected",
			 bad_formats[i]);
		compat_expect_errno(what, ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill),
				    errno, EINVAL);
	}

	puts("Fill results read back through mmap:");
	fill.format = DRM_FORMAT_RGB565;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = 0xf81f;
	compat_expect_ok("RGB565 full frame",
			 ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill));
	{
		struct drm_mode_map_dumb map = { 0 };
		uint16_t *px;
		unsigned int bad = 0, x, y;

		map.handle = handle;
		if (card_node) {
			compat_check(ioctl(gpu, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0,
				     "MAP_DUMB is available on the GPU card node",
				     NULL);
		} else {
			compat_expect_errno("MAP_DUMB on the render node is refused",
					    ioctl(gpu, DRM_IOCTL_MODE_MAP_DUMB, &map),
					    errno, EACCES);
		}
		px = mmap(NULL, create.size, PROT_READ, MAP_SHARED, gpu,
			  create.mmap_offset);
		if (px == MAP_FAILED) {
			perror("mmap");
		} else {
			for (y = 0; y < height; y++)
				for (x = 0; x < width; x++)
					if (px[(y * width) + x] != 0xf81f)
						bad++;
			compat_check(!bad, "every RGB565 pixel holds the fill colour",
				     NULL);
			munmap(px, create.size);
		}
	}
	close_bo.handle = handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return 0;
}

/* GPU buffer -> PRIME export -> LCDC import -> ADDFB2 -> page flip. */
static int prime_scanout(int gpu, int card, uint32_t crtc_id, uint32_t old_fb,
			 unsigned int width, unsigned int height, uint32_t format,
			 int expect_flip)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_prime_handle prime = { 0 };
	struct drm_mode_fb_cmd2 fbc = { 0 };
	struct drm_gem_close close_bo = { 0 };
	unsigned int seq = 0, pitch = width * fourcc_cpp(format);
	int ret = -1;

	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	fill.handle = create.handle;
	fill.format = format;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = 0xffe0;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("GPU fill");
		goto out;
	}
	prime.handle = create.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(gpu, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
		perror("PRIME export");
		goto out;
	}
	if (ioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) {
		perror("PRIME import into LCDC");
		close(prime.fd);
		goto out;
	}
	close(prime.fd);
	fbc.width = width;
	fbc.height = height;
	fbc.pixel_format = format;
	fbc.handles[0] = prime.handle;
	fbc.pitches[0] = pitch;
	if (ioctl(card, DRM_IOCTL_MODE_ADDFB2, &fbc)) {
		int err = errno;

		fprintf(stderr, "%s: ADDFB2 failed, errno %d (%s)\n",
			fourcc_name(format), err, strerror(err));

		close_bo.handle = (uint32_t)prime.handle;
		ioctl(card, DRM_IOCTL_GEM_CLOSE, &close_bo);
		close_bo.handle = create.handle;
		ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		return expect_flip ? -1 : (err == EINVAL ? 0 : -1);
	}
	{
		struct drm_mode_crtc_page_flip flip = {
			.crtc_id = crtc_id,
			.fb_id = fbc.fb_id,
			.flags = DRM_MODE_PAGE_FLIP_EVENT,
		};

		if (ioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &flip)) {
			int err = errno;

			fprintf(stderr, "%s: page flip failed, errno %d (%s)\n",
				fourcc_name(format), err, strerror(err));

			ioctl(card, DRM_IOCTL_MODE_RMFB, &fbc.fb_id);
			close_bo.handle = (uint32_t)prime.handle;
			ioctl(card, DRM_IOCTL_GEM_CLOSE, &close_bo);
			close_bo.handle = create.handle;
			ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
			return expect_flip ? -1 : (err == EINVAL ? 0 : -1);
		}
	}
	if (!expect_flip) {
		/* The LCDC must not accept a format its plane cannot scan out. */
		ret = flip_wait(card, crtc_id, old_fb, &seq) ? -1 : -2;
		goto out_fb;
	}
	if (read_flip_event(card, &seq))
		goto out_fb;
	if (flip_wait(card, crtc_id, old_fb, &seq))
		goto out_fb;
	ret = 0;
out_fb:
	ioctl(card, DRM_IOCTL_MODE_RMFB, &fbc.fb_id);
	close_bo.handle = (uint32_t)prime.handle;
	ioctl(card, DRM_IOCTL_GEM_CLOSE, &close_bo);
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

static int set_crtc(int card, uint32_t crtc_id, uint32_t fb_id,
		    const struct drm_mode_modeinfo *mode, uint32_t connector_id)
{
	struct drm_mode_crtc set = { 0 };

	set.crtc_id = crtc_id;
	set.fb_id = fb_id;
	set.mode_valid = mode != NULL;
	if (mode)
		memcpy(&set.mode, mode, sizeof(set.mode));
	set.count_connectors = mode ? 1 : 0;
	set.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id;
	return ioctl(card, DRM_IOCTL_MODE_SETCRTC, &set);
}

/*
 * XRGB8888 cannot enter through the legacy page-flip ioctl: core DRM refuses a
 * format change there. SETCRTC rebuilds the whole mode, so it is the path that
 * shows whether the LCDC really scans 32-bit pixels.
 */
static int compat_xrgb_scanout(int gpu, int card, uint32_t crtc_id,
			       uint32_t old_fb, uint32_t connector_id,
			       unsigned int width, unsigned int height)
{
	struct drm_n32h7xx_create create = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_prime_handle prime = { 0 };
	struct drm_mode_fb_cmd2 fbc = { 0 };
	struct drm_gem_close close_bo = { 0 };
	struct drm_mode_crtc cur = { 0 }, now = { 0 };
	struct drm_mode_crtc_page_flip flip = { 0 };
	unsigned int pitch = width * 4, count;
	long sec, usec;
	int ret = -1;

	cur.crtc_id = crtc_id;
	if (ioctl(card, DRM_IOCTL_MODE_GETCRTC, &cur) || !cur.mode_valid)
		return -1;
	create.size = (uint64_t)pitch * height;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_CREATE, &create)) {
		perror("GPU buffer allocation");
		return -1;
	}
	fill.handle = create.handle;
	fill.format = DRM_FORMAT_XRGB8888;
	fill.pitch = pitch;
	fill.width = width;
	fill.height = height;
	fill.color = 0x00123456;
	if (ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill)) {
		perror("GPU fill");
		goto out;
	}
	prime.handle = create.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(gpu, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
		perror("PRIME export");
		goto out;
	}
	if (ioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) {
		perror("PRIME import into LCDC");
		close(prime.fd);
		goto out;
	}
	close(prime.fd);
	fbc.width = width;
	fbc.height = height;
	fbc.pixel_format = DRM_FORMAT_XRGB8888;
	fbc.handles[0] = prime.handle;
	fbc.pitches[0] = pitch;
	if (ioctl(card, DRM_IOCTL_MODE_ADDFB2, &fbc)) {
		perror("add framebuffer");
		goto out_handle;
	}
	flip.crtc_id = crtc_id;
	flip.fb_id = fbc.fb_id;
	flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
	errno = 0;
	compat_check(ioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == -1 &&
		     errno == EINVAL,
		     "page flip of a different pixel format is refused", NULL);
	if (set_crtc(card, crtc_id, fbc.fb_id, &cur.mode, connector_id)) {
		char detail[80];

		snprintf(detail, sizeof(detail), "-> errno %d (%s)", errno,
			 strerror(errno));
		compat_check(0, "SETCRTC switches to the XRGB8888 buffer", detail);
		goto out_fb;
	}
	now.crtc_id = crtc_id;
	if (ioctl(card, DRM_IOCTL_MODE_GETCRTC, &now) || now.fb_id != fbc.fb_id) {
		compat_check(0, "SETCRTC switches to the XRGB8888 buffer", NULL);
		goto out_fb;
	}
	if (wait_vblank(card, 2, &count, &sec, &usec)) {
		compat_check(0, "XRGB8888 buffer keeps the CRTC scanning", NULL);
		goto out_fb;
	}
	compat_check(1, "XRGB8888 buffer keeps the CRTC scanning", NULL);
out_fb:
	if (set_crtc(card, crtc_id, old_fb, &cur.mode, connector_id))
		fprintf(stderr, "Warning: could not restore the console framebuffer\n");
	ioctl(card, DRM_IOCTL_MODE_RMFB, &fbc.fb_id);
out_handle:
	close_bo.handle = (uint32_t)prime.handle;
	ioctl(card, DRM_IOCTL_GEM_CLOSE, &close_bo);
out:
	close_bo.handle = create.handle;
	ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
	return ret;
}

/* Toggle the output without touching the pixel format, to isolate a path. */
static double uptime_sec(void)
{
	double up = 0;
	FILE *f = fopen("/proc/uptime", "r");

	if (f) {
		if (fscanf(f, "%lf", &up) != 1)
			up = 0;
		fclose(f);
	}
	return up;
}

static int panel_test(int fd, unsigned int hold)
{
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_crtc cur = { 0 };
	uint32_t crtc_ids[4], conn_ids[4];

	res.count_crtcs = ARRAY_SIZE(crtc_ids);
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
	res.count_connectors = ARRAY_SIZE(conn_ids);
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs ||
	    !res.count_connectors)
		return -1;
	cur.crtc_id = crtc_ids[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cur) || !cur.mode_valid) {
		fprintf(stderr, "No active mode to restore after the test\n");
		return -1;
	}
	printf("[%.3f] disabling\n", uptime_sec());
	if (set_crtc(fd, cur.crtc_id, 0, NULL, conn_ids[0])) {
		perror("disable");
		return -1;
	}
	sleep(hold);
	printf("[%.3f] enabling\n", uptime_sec());
	if (set_crtc(fd, cur.crtc_id, cur.fb_id, &cur.mode, conn_ids[0])) {
		perror("enable");
		return -1;
	}
	printf("[%.3f] done\n", uptime_sec());
	return 0;
}

static int compat_test(int fd, const char *gpu_node)
{
	struct drm_mode_create_dumb dumb = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_prime_handle prime = { 0 };
	struct drm_n32h7xx_fill fill = { 0 };
	struct drm_gem_close close_bo = { 0 };
	struct drm_mode_card_res res = { 0 };
	uint32_t crtc_ids[4], conn_ids[4];
	uint32_t crtc_id, old_fb, width, height, handle = 0, connector_id = 0;
	uint16_t *dumb_px = NULL;
	int gpu, master, ret;

	if (crtc_state(fd, &crtc_id, &old_fb, &width, &height)) {
		fprintf(stderr, "Cannot read the current LCDC mode\n");
		return -1;
	}
	gpu = open(gpu_node, O_RDWR | O_CLOEXEC);
	if (gpu < 0) {
		perror(gpu_node);
		return -1;
	}
	master = ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0;
	res.count_crtcs = ARRAY_SIZE(crtc_ids);
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
	res.count_connectors = ARRAY_SIZE(conn_ids);
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	if (!ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) && res.count_connectors)
		connector_id = conn_ids[0];
	else
		fprintf(stderr, "Warning: no connector reported by the LCDC\n");

	compat_fails = 0;
	compat_gpu(gpu, width, height);

	puts("PRIME exported to the LCDC:");
	compat_check(prime_scanout(gpu, fd, crtc_id, old_fb, width, height,
				   DRM_FORMAT_RGB565, 1) == 0,
		     "RGB565 GPU buffer scans out", NULL);
	compat_xrgb_scanout(gpu, fd, crtc_id, old_fb, connector_id, width,
			    height);
	/* Only the ADDFB2 rejection is under test here.  Keep the buffer small so
	 * the check does not need a 2 MiB contiguous shmem allocation. */
	compat_check(prime_scanout(gpu, fd, crtc_id, old_fb,
				   width < 64 ? width : 64,
				   height < 64 ? height : 64,
				   DRM_FORMAT_ARGB8888, 0) == 0,
		     "ARGB8888 is refused before it reaches the panel", NULL);

	puts("LCDC framebuffer imported into the GPU:");
	dumb.width = width;
	dumb.height = height;
	dumb.bpp = 16;
	compat_expect_ok("create an LCDC dumb buffer",
			 ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb));
	map.handle = dumb.handle;
	compat_expect_ok("map the LCDC dumb buffer",
			 ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map));
	dumb_px = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       map.offset);
	compat_expect_ok("mmap the LCDC dumb buffer", dumb_px == MAP_FAILED ? -1 : 0);
	if (dumb_px != MAP_FAILED) {
		unsigned int bad = 0, i;

		for (i = 0; i < dumb.size / 2; i++)
			dumb_px[i] = 0;
		prime.handle = dumb.handle;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		compat_expect_ok("PRIME export from the LCDC",
				 ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
		if (prime.fd >= 0) {
			struct drm_prime_handle in = {
				.fd = prime.fd,
				.flags = DRM_CLOEXEC | DRM_RDWR,
			};

			compat_expect_ok("PRIME import into the GPU",
					 ioctl(gpu, DRM_IOCTL_PRIME_FD_TO_HANDLE, &in));
			handle = in.handle;
			close(prime.fd);
		}
		if (handle) {
			fill.handle = handle;
			fill.format = DRM_FORMAT_RGB565;
			fill.pitch = dumb.pitch;
			fill.width = width;
			fill.height = height;
			fill.color = 0x07e0;
			compat_expect_ok("GPU fills the LCDC buffer",
					 ioctl(gpu, DRM_IOCTL_N32H7XX_FILL, &fill));
			for (i = 0; i < (unsigned int)width * height; i++)
				if (dumb_px[i] != 0x07e0)
					bad++;
			compat_check(!bad, "every pixel holds the fill colour", NULL);
			close_bo.handle = handle;
			ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &close_bo);
		}
		munmap(dumb_px, dumb.size);
	}
	destroy.handle = dumb.handle;
	ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

	printf("Compatibility summary: %u failure(s)\n", compat_fails);
	ret = compat_fails ? -1 : 0;
	close(gpu);
	if (master)
		ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	return ret;
}

int main(int argc, char **argv)
{
	unsigned int hold;
	uint32_t format;
	int fd, ret;

	if (argc < 3 || argc > 9 ||
	    (strcmp(argv[1], "fb") && strcmp(argv[1], "gpu") &&
	     strcmp(argv[1], "flip") && strcmp(argv[1], "perf") &&
	     strcmp(argv[1], "bench") && strcmp(argv[1], "benchfill") &&
	     strcmp(argv[1], "benchfilltile") && strcmp(argv[1], "benchcpufill") &&
	     strcmp(argv[1], "verify") && strcmp(argv[1], "panel") &&
	     strcmp(argv[1], "rect") && strcmp(argv[1], "blend") &&
	     strcmp(argv[1], "blit") && strcmp(argv[1], "batch") &&
	     strcmp(argv[1], "poly") &&
	     strcmp(argv[1], "curve") && strcmp(argv[1], "formats") &&
	     strcmp(argv[1], "compat") && strcmp(argv[1], "dither") &&
	     strcmp(argv[1], "profile")) ||
	    (!strcmp(argv[1], "fb") && argc > 4) ||
	    (!strcmp(argv[1], "bench") && argc < 4) ||
	    (!strcmp(argv[1], "compat") && argc < 4) ||
	    ((!strcmp(argv[1], "benchfill") ||
	      !strcmp(argv[1], "benchfilltile")) && argc < 6) ||
	    (!strcmp(argv[1], "benchfilltile") && argc < 7) ||
	    (!strcmp(argv[1], "benchcpufill") && argc < 5) ||
	    (!strcmp(argv[1], "verify") && argc < 5) ||
	    (!strcmp(argv[1], "rect") && argc < 5) ||
	    (!strcmp(argv[1], "blit") && argc < 5) ||
	    (!strcmp(argv[1], "poly") && argc < 5) ||
	    (!strcmp(argv[1], "curve") && argc < 5) ||
	    (!strcmp(argv[1], "blend") && argc < 5)) {
		fprintf(stderr,
			"Usage: %s fb <fbdev> [hold-seconds]\n"
			"       %s gpu <GPU DRM node> [LCDC DRM node [hold-seconds]]\n"
			"       %s flip <LCDC DRM node> [pairs [hold-seconds]]\n"
			"       %s perf <LCDC DRM node> [GPU DRM node] [frames]\n"
			"       %s bench <LCDC DRM node> <GPU DRM node>\n"
			"       %s benchfill <GPU DRM node> <width> <height> <iters> [rgb565|xrgb8888|argb8888]\n"
		"       %s benchfilltile <GPU DRM node> <width> <height> <iters> <tile-height> [rgb565|xrgb8888|argb8888] [gap-us]\n"
		"       %s benchcpufill <GPU DRM node> <width> <height> <iters>\n"
			"       %s verify <GPU DRM node> <width> <height> [rgb565|xrgb8888|argb8888]\n"
			"       %s rect <GPU DRM node> <width> <height> [iters]\n"
			"       %s blend <GPU DRM node> <width> <height> [iters]\n"
			"       %s batch <GPU DRM node> [width height [rect-w rect-h]]\n"
			"       %s blit <GPU DRM node> <width> <height> [iters]\n"
			"       %s poly <GPU DRM node> <width> <height> [iters]\n"
			"       %s curve <GPU DRM node> <width> <height> [iters]\n"
"       %s formats <GPU DRM node>\n"
			"       %s dither <GPU DRM node>\n"
			"       %s profile <GPU DRM node> [width height [iterations]]\n"
			"       %s panel <LCDC DRM node> <hold-seconds>\n"
			"       %s compat <LCDC DRM node> <GPU DRM node>\n",
			argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
			argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
			argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
			argv[0], argv[0]);
		return 2;
	}
	fd = open(argv[2], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[2]);
		return 1;
	}
	if (!strcmp(argv[1], "gpu")) {
		ret = gpu_test(fd, argc > 3 ? argv[3] : NULL,
			       argc > 4 ? strtoul(argv[4], NULL, 0) : 10);
	} else if (!strcmp(argv[1], "flip")) {
		unsigned int pairs = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;

		hold = argc > 4 ? strtoul(argv[4], NULL, 0) : 5;
		ret = flip_test(fd, pairs ? pairs : 1, hold);
	} else if (!strcmp(argv[1], "perf")) {
		ret = perf_test(fd, argc > 3 ? argv[3] : NULL,
				argc > 4 ? strtoul(argv[4], NULL, 0) : 0);
	} else if (!strcmp(argv[1], "bench")) {
		ret = bench_test(fd, argv[3]);
	} else if (!strcmp(argv[1], "rect")) {
		ret = rect_test(fd, strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				argc > 5 ? strtoul(argv[5], NULL, 0) : 100);
	} else if (!strcmp(argv[1], "batch")) {
		unsigned int width = argc > 3 ? strtoul(argv[3], NULL, 0) : 800;
		unsigned int height = argc > 4 ? strtoul(argv[4], NULL, 0) : 480;
		unsigned int rw = argc > 5 ? strtoul(argv[5], NULL, 0) : 16;
		unsigned int rh = argc > 6 ? strtoul(argv[6], NULL, 0) : 16;

		if (!width || !height || !rw || !rh || rw > width || rh > height)
			return 2;
		ret = batch_test(fd, width, height, rw, rh);
	} else if (!strcmp(argv[1], "blend")) {
		ret = blend_test(fd, strtoul(argv[3], NULL, 0),
				 strtoul(argv[4], NULL, 0),
				 argc > 5 ? strtoul(argv[5], NULL, 0) : 100);
	} else if (!strcmp(argv[1], "blit")) {
		ret = blit_test(fd, strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				argc > 5 ? strtoul(argv[5], NULL, 0) : 100);
	} else if (!strcmp(argv[1], "poly")) {
		ret = poly_test(fd, strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				argc > 5 ? strtoul(argv[5], NULL, 0) : 50);
	} else if (!strcmp(argv[1], "curve")) {
		ret = curve_test(fd, strtoul(argv[3], NULL, 0),
				 strtoul(argv[4], NULL, 0),
				 argc > 5 ? strtoul(argv[5], NULL, 0) : 50);
	} else if (!strcmp(argv[1], "formats")) {
		ret = format_test(fd);
	} else if (!strcmp(argv[1], "profile")) {
		unsigned int width = argc > 4 ? strtoul(argv[3], NULL, 0) : 256;
		unsigned int height = argc > 4 ? strtoul(argv[4], NULL, 0) : 128;
		unsigned int iters = argc > 5 ? strtoul(argv[5], NULL, 0) : 4;

		ret = profile_test(fd, width ? width : 256,
				   height ? height : 128, iters ? iters : 1);
	} else if (!strcmp(argv[1], "dither")) {
		ret = dither_test(fd);
	} else if (!strcmp(argv[1], "panel")) {
		ret = panel_test(fd, argc > 3 ? strtoul(argv[3], NULL, 0) : 3);
	} else if (!strcmp(argv[1], "compat")) {
		ret = compat_test(fd, argv[3]);
	} else if (!strcmp(argv[1], "verify")) {
		format = DRM_FORMAT_RGB565;
		if (argc > 5) {
			if (!strcmp(argv[5], "xrgb8888"))
				format = DRM_FORMAT_XRGB8888;
			else if (!strcmp(argv[5], "argb8888"))
				format = DRM_FORMAT_ARGB8888;
			else if (strcmp(argv[5], "rgb565")) {
				fprintf(stderr, "Unknown format %s\n", argv[5]);
				return 2;
			}
		}
		ret = verify_test(fd, strtoul(argv[3], NULL, 0),
				  strtoul(argv[4], NULL, 0), format);
	} else if (!strcmp(argv[1], "benchfill")) {
		format = DRM_FORMAT_RGB565;
		if (argc > 6) {
			if (!strcmp(argv[6], "xrgb8888"))
				format = DRM_FORMAT_XRGB8888;
			else if (!strcmp(argv[6], "argb8888"))
				format = DRM_FORMAT_ARGB8888;
			else if (strcmp(argv[6], "rgb565")) {
				fprintf(stderr, "Unknown format %s\n", argv[6]);
				return 2;
			}
		}
		{
			uint32_t width = strtoul(argv[3], NULL, 0);
			uint32_t height = strtoul(argv[4], NULL, 0);

			ret = bench_case(fd, format, width, height,
					 width * fourcc_cpp(format),
					 strtoul(argv[5], NULL, 0));
		}
	} else if (!strcmp(argv[1], "benchfilltile")) {
		format = DRM_FORMAT_RGB565;
		if (argc > 7) {
			if (!strcmp(argv[7], "xrgb8888"))
				format = DRM_FORMAT_XRGB8888;
			else if (!strcmp(argv[7], "argb8888"))
				format = DRM_FORMAT_ARGB8888;
			else if (strcmp(argv[7], "rgb565")) {
				fprintf(stderr, "Unknown format %s\n", argv[7]);
				return 2;
			}
		}
		{
			uint32_t width = strtoul(argv[3], NULL, 0);
			uint32_t height = strtoul(argv[4], NULL, 0);

			ret = bench_tiled_case(fd, format, width, height,
					       width * fourcc_cpp(format),
					       strtoul(argv[5], NULL, 0),
					       strtoul(argv[6], NULL, 0),
					       argc > 8 ? strtoul(argv[8], NULL, 0) : 0);
		}
	} else if (!strcmp(argv[1], "benchcpufill")) {
		ret = bench_cpu_case(fd, strtoul(argv[3], NULL, 0),
				     strtoul(argv[4], NULL, 0),
				     strtoul(argv[5], NULL, 0));
	} else {
		ret = framebuffer_test(fd, argc == 4 ? strtoul(argv[3], NULL, 0) : 5);
	}
	close(fd);
	return ret ? 1 : 0;
}
