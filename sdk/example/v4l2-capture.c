/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * v4l2-capture.c - grab frames from the OV5640 through the N32H7xx DVP port.
 *
 * The camera is a normal V4L2 capture device.  Load the stack first (it comes
 * up at boot unless CAMERA=off is set in /etc/defaults.conf), then:
 *
 *   v4l2-capture -l                  list the formats and the active one
 *   v4l2-capture -o gray.raw                 Y8 grayscale, the default size
 *   v4l2-capture -f GREY -s 640x480 -o g.raw
 *   v4l2-capture -f RGB565 -s 320x240 -o rgb.raw
 *   v4l2-capture -f JPEG -s 1280x720 -n 3 -o shot.jpg
 *   v4l2-capture -f JPEG -o -        one JPEG frame on stdout
 *   v4l2-capture -L                  list the sensor's controls
 *   v4l2-capture -c test_pattern=1 -o bars.raw    color bars, no lighting
 *
 * The formats are the four the DVP hands to userspace as a straight byte
 * stream off the parallel bus, so they need no conversion here:
 *
 *   GREY    V4L2_PIX_FMT_GREY    1 byte/pixel, one Y sample per pixel
 *   YUYV    V4L2_PIX_FMT_YUYV    2 bytes/pixel, Y0 U Y1 V
 *   RGB565  V4L2_PIX_FMT_RGB565  2 bytes/pixel, little-endian in memory
 *   JPEG    V4L2_PIX_FMT_JPEG    compressed, variable length per frame
 *
 * Frames are captured with mmap streaming: the driver has no format
 * conversion, so every buffer is already the format that was asked for.
 *
 * The sensor restarts its auto exposure and auto white balance on every
 * STREAMON, and the frames that follow are not pictures of the scene
 * yet.  Capturing drops frames until the sensor has settled before it saves
 * anything -- see frame_usable() below.  -w sets the minimum number of frames
 * to drop; more are dropped if that is what it takes.
 *
 * The controls belong to the sensor, not to this driver.  The bridge lends
 * the sensor's control handler to /dev/videoN, so the ordinary control ioctls
 * work here: exposure, gain, white balance, the flips and the ISP test
 * pattern.  A control is written on the wire only once the sensor is
 * streaming, so -c is applied before STREAMON and the value set is what the
 * first frame uses.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define MAX_CTRLS 8

struct buffer {
	void	*start;
	size_t	length;
};

struct ctrl_opt {
	char		name[64];
	long long	value;
};

static const char *progname = "v4l2-capture";

struct format {
	const char	*name;
	uint32_t	fourcc;
};

/* The pixel formats the DVP driver can deliver without conversion. */
static const struct format formats[] = {
	{ "GREY",   V4L2_PIX_FMT_GREY },
	{ "YUYV",   V4L2_PIX_FMT_YUYV },
	{ "RGB565", V4L2_PIX_FMT_RGB565 },
	{ "JPEG",   V4L2_PIX_FMT_JPEG },
};

/*
 * Largest physically contiguous free block, in bytes, from /proc/buddyinfo.
 *
 * Every buffer this driver hands out is physically contiguous: the kernel has
 * no MMU, so vmalloc() there is a kmalloc() and a frame cannot be scattered
 * across pages.  What limits a large capture is therefore the biggest single
 * free block, not the total free memory.  Sizing the request to that block
 * keeps REQBUFS from asking for a frame the allocator cannot place and then
 * having to fall back (which prints a page-allocation failure on the way).
 */
static unsigned long largest_free_block(void)
{
	char line[256];
	unsigned int high = 0;
	FILE *f;

	f = fopen("/proc/buddyinfo", "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof(line), f)) {
		char *save = NULL, *tok;
		unsigned int order = 0;

		for (tok = strtok_r(line, " \n", &save); tok;
		     tok = strtok_r(NULL, " \n", &save)) {
			unsigned long v;
			char *end;

			v = strtoul(tok, &end, 10);
			if (*end || end == tok)
				continue;	/* "Node", "0,", "zone", "Normal" */
			if (v && order > high)
				high = order;
			order++;
		}
	}
	fclose(f);

	/* 4 KiB pages, so order N is 2^(N + 12) bytes. */
	return high ? 1UL << (high + 12) : 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -d DEV     video device (default /dev/video0)\n"
		"  -l         list the device's formats, then exit\n"
		"  -f FOURCC  GREY | YUYV | RGB565 | JPEG\n"
		"  -s WxH     frame size (default 320x240)\n"
		"  -n N       save N frames (default 1)\n"
		"  -w N       drop at least N frames first (default 2).  More are\n"
		"             dropped if the sensor's auto exposure needs longer\n"
		"             to settle\n"
		"  -q N       number of buffers (default 4)\n"
		"  -L         list the sensor's controls, then exit\n"
		"  -c N=V     set control N to V (repeatable).  Names are matched\n"
		"             ignoring case, spaces and underscores, so\n"
		"             test_pattern=1 or \"Test Pattern\"=1 both work.\n"
		"  -o FILE    output; \"-\" is stdout (default -).  With N > 1 the\n"
		"             index is appended to FILE.\n",
		progname);
}

/*
 * Control names come from the kernel as prose ("Test Pattern"), while people
 * type them as identifiers, so compare on a form that ignores case, spaces
 * and separators.
 */
static void normalize_name(const char *in, char *out, size_t outlen)
{
	size_t n = 0;

	for (; *in && n + 1 < outlen; in++) {
		unsigned char c = *in;

		if (c == ' ' || c == '_' || c == '-')
			continue;
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		out[n++] = c;
	}
	out[n] = '\0';
}

static int find_ctrl(int fd, const char *name)
{
	struct v4l2_query_ext_ctrl q;
	char want[64], have[64];
	char *end = NULL;
	long id;

	/* A bare number is taken as a control id. */
	id = strtol(name, &end, 0);
	if (end && end != name && *end == '\0')
		return (int)id;

	normalize_name(name, want, sizeof(want));

	memset(&q, 0, sizeof(q));
	q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
	while (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
		normalize_name(q.name, have, sizeof(have));
		if (!strcmp(want, have))
			return q.id;
		q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}

	return -1;
}

static void menu_label(int fd, unsigned int id, long long index,
		       char *out, size_t outlen)
{
	struct v4l2_querymenu qm;

	memset(&qm, 0, sizeof(qm));
	qm.id = id;
	qm.index = (unsigned int)index;
	if (ioctl(fd, VIDIOC_QUERYMENU, &qm) == 0)
		snprintf(out, outlen, "%s", qm.name);
	else
		out[0] = '\0';
}

static void list_controls(int fd)
{
	struct v4l2_query_ext_ctrl q;
	unsigned int n = 0;

	memset(&q, 0, sizeof(q));
	q.id = V4L2_CTRL_FLAG_NEXT_CTRL;

	printf("controls:\n");
	while (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
		if (q.type == V4L2_CTRL_TYPE_MENU) {
			char label[48];

			menu_label(fd, q.id, q.default_value, label,
				   sizeof(label));
			printf("  %-24s = %lld .. %lld  (menu: %s)\n", q.name,
			       (long long)q.minimum, (long long)q.maximum,
			       label);
		} else {
			printf("  %-24s = %lld .. %lld\n", q.name,
			       (long long)q.minimum, (long long)q.maximum);
		}
		n++;
		q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}
	if (!n)
		printf("  (none; is the sensor bound?)\n");
}

static int apply_ctrl(int fd, const struct ctrl_opt *opt)
{
	struct v4l2_control c;
	int id = find_ctrl(fd, opt->name);

	if (id < 0) {
		fprintf(stderr, "%s: no control named '%s' (try -L)\n",
			progname, opt->name);
		return -1;
	}

	memset(&c, 0, sizeof(c));
	c.id = id;
	c.value = (int)opt->value;
	if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) {
		fprintf(stderr, "%s: cannot set '%s': %s\n", progname,
			opt->name, strerror(errno));
		return -1;
	}

	/* Read back so the log shows what the first frame will really use.
	 * Status text stays on stderr so -o - is pure frame data. */
	if (ioctl(fd, VIDIOC_G_CTRL, &c) == 0)
		fprintf(stderr, "%s = %d\n", opt->name, c.value);

	return 0;
}

static uint32_t fourcc_from_name(const char *name)
{
	size_t i;
	uint32_t fourcc;

	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
		if (!strcasecmp(name, formats[i].name))
			return formats[i].fourcc;

	/* Also accept the raw fourcc, e.g. "RGB3" or "JPEG". */
	if (strlen(name) != 4) {
		fprintf(stderr, "%s: unknown format '%s'\n", progname, name);
		exit(2);
	}
	fourcc = v4l2_fourcc(name[0], name[1], name[2], name[3]);
	return fourcc;
}

static void fourcc_to_str(uint32_t fourcc, char out[5])
{
	out[0] = fourcc & 0xff;
	out[1] = (fourcc >> 8) & 0xff;
	out[2] = (fourcc >> 16) & 0xff;
	out[3] = (fourcc >> 24) & 0xff;
	out[4] = '\0';
}

static void list_formats(int fd)
{
	struct v4l2_fmtdesc desc;
	struct v4l2_format fmt;
	char code[5];
	unsigned int n = 0;

	memset(&desc, 0, sizeof(desc));
	desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	printf("formats:\n");
	for (;;) {
		desc.index = n;
		if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) {
			if (errno != EINVAL)
				perror("VIDIOC_ENUM_FMT");
			break;
		}
		fourcc_to_str(desc.pixelformat, code);
		printf("  %-8s %s\n", code, desc.description);
		n++;
	}
	if (n == 0)
		printf("  (none)\n");

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
		fourcc_to_str(fmt.fmt.pix.pixelformat, code);
		printf("current: %s %ux%u, %u bytes/line, %u bytes/frame\n",
		       code, fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
	}
}

static void open_output(const char *path, int index, int count,
			int *out_fd, char *resolved, size_t resolved_len)
{
	if (!strcmp(path, "-")) {
		*out_fd = STDOUT_FILENO;
		snprintf(resolved, resolved_len, "stdout");
		return;
	}

	if (count > 1)
		snprintf(resolved, resolved_len, "%s.%d", path, index);
	else
		snprintf(resolved, resolved_len, "%s", path);

	*out_fd = open(resolved, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (*out_fd < 0) {
		fprintf(stderr, "%s: cannot create %s: %s\n", progname,
			resolved, strerror(errno));
		exit(1);
	}
}

/* Read one of the sensor's controls.  Returns 0 and the value on success. */
static int ctrl_get(int fd, unsigned int id, long *value)
{
	struct v4l2_control c;

	memset(&c, 0, sizeof(c));
	c.id = id;
	if (ioctl(fd, VIDIOC_G_CTRL, &c) < 0)
		return -1;
	*value = c.value;
	return 0;
}

/*
 * How long to wait for the sensor to settle after STREAMON.  SETTLE_FRAMES
 * is how many reads of the exposure and the gain in a row have to agree, and
 * WARMUP_MAX is how many frames to keep dropping if they never do.
 */
#define SETTLE_FRAMES	3
#define WARMUP_MAX	30

struct settle {
	long	exposure;	/* last readings; -1 until the first one */
	long	gain;
	int	still;		/* reads in a row that did not move */
	int	live;		/* the driver answers G_CTRL */
};

/*
 * Should the frame that was just dequeued be saved?
 *
 * The OV5640 runs its own auto exposure and auto white balance and restarts
 * both every time streaming starts, so the first frames after STREAMON are
 * not pictures of the scene yet: they run from far too dark through
 * overexposed, and the white balance drifts across a strong green cast on the
 * way.  Saving one of those is what makes a capture look wrong -- a large
 * resolution is slow enough that the frame is usually fine by the time it is
 * saved, while a small one lands right in the middle of the ramp.
 *
 * The sensor has no "converged" flag, but the driver marks the exposure and
 * the analogue gain volatile and reads them back out of the sensor on
 * VIDIOC_G_CTRL, so the numbers the AEC is working with are visible from
 * userspace.  The frames stop being wrong when those stop changing.
 *
 * min_frames are dropped whatever happens.  Past that, frames are dropped
 * until the pair has held still for SETTLE_FRAMES reads, or until WARMUP_MAX
 * more frames have gone by, so a scene the AEC keeps hunting in still
 * produces a capture.  A driver that does not forward the controls leaves -w
 * to do the work on its own.
 */
static int frame_usable(int fd, int j, int min_frames, struct settle *s)
{
	long exposure, gain;

	if (j < min_frames)
		return 0;

	if (s->live &&
	    (ctrl_get(fd, V4L2_CID_EXPOSURE, &exposure) < 0 ||
	     ctrl_get(fd, V4L2_CID_ANALOGUE_GAIN, &gain) < 0))
		s->live = 0;
	if (!s->live)
		return 1;

	if (exposure == s->exposure && gain == s->gain)
		s->still++;
	else
		s->still = 0;
	s->exposure = exposure;
	s->gain = gain;

	if (s->still >= SETTLE_FRAMES)
		return 1;

	if (j + 1 >= min_frames + WARMUP_MAX) {
		fprintf(stderr,
			"%s: warning: the sensor's auto exposure did not settle "
			"within %d frames, using frame %d anyway\n",
			progname, min_frames + WARMUP_MAX, j);
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/video0";
	const char *out_path = "-";
	uint32_t want_fourcc = 0;
	unsigned int want_w = 0, want_h = 0;
	int save = 1, warmup = 2, nbufs = 4;
	struct settle settle = { .exposure = -1, .gain = -1, .live = 1 };
	struct buffer *buffers = NULL;
	struct v4l2_requestbuffers req;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_format fmt;
	int fd, c, i, j;
	int list = 0;
	int list_ctrls = 0, nctrls = 0;
	int ready = 0;
	struct ctrl_opt ctrls[MAX_CTRLS];
	unsigned int saved = 0;
	unsigned int bad = 0;

	while ((c = getopt(argc, argv, "d:lf:s:n:w:q:o:c:Lh")) != -1) {
		switch (c) {
		case 'd':
			device = optarg;
			break;
		case 'l':
			list = 1;
			break;
		case 'f':
			want_fourcc = fourcc_from_name(optarg);
			break;
		case 's':
			if (sscanf(optarg, "%ux%u", &want_w, &want_h) != 2 ||
			    want_w == 0 || want_h == 0) {
				fprintf(stderr, "%s: bad size '%s'\n",
					progname, optarg);
				return 2;
			}
			break;
		case 'n':
			save = atoi(optarg);
			break;
		case 'w':
			warmup = atoi(optarg);
			break;
		case 'q':
			nbufs = atoi(optarg);
			break;
		case 'o':
			out_path = optarg;
			break;
		case 'c': {
			char *eq = strchr(optarg, '=');
			size_t len;

			if (!eq || nctrls >= MAX_CTRLS) {
				usage();
				return 2;
			}
			len = eq - optarg;
			if (len == 0 || len >= sizeof(ctrls[0].name)) {
				fprintf(stderr, "%s: bad control '%s'\n",
					progname, optarg);
				return 2;
			}
			memcpy(ctrls[nctrls].name, optarg, len);
			ctrls[nctrls].name[len] = '\0';
			ctrls[nctrls].value = strtoll(eq + 1, NULL, 0);
			nctrls++;
			break;
		}
		case 'L':
			list_ctrls = 1;
			break;
		default:
			usage();
			return 2;
		}
	}

	if (save < 1 || warmup < 0 || nbufs < 1) {
		fprintf(stderr, "%s: -n must be >= 1, -w >= 0, -q >= 1\n",
			progname);
		return 2;
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", progname, device,
			strerror(errno));
		return 1;
	}

	if (list) {
		list_formats(fd);
		close(fd);
		return 0;
	}

	if (list_ctrls) {
		list_controls(fd);
		close(fd);
		return 0;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (want_fourcc)
		fmt.fmt.pix.pixelformat = want_fourcc;
	if (want_w) {
		fmt.fmt.pix.width = want_w;
		fmt.fmt.pix.height = want_h;
	}
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		return 1;
	}
	{
		char code[5];
		fourcc_to_str(fmt.fmt.pix.pixelformat, code);
		if (want_fourcc && fmt.fmt.pix.pixelformat != want_fourcc) {
			fprintf(stderr, "%s: device chose %s, not the requested "
				"format\n", progname, code);
			return 1;
		}
		/*
		 * Progress goes to stderr: with -o -, stdout carries only
		 * the raw frame bytes.
		 *
		 * sizeimage is the buffer size, not the frame size.  For the
		 * fixed-size formats it is exactly one frame, but a JPEG frame
		 * is variable length and can only ever be shorter than the
		 * buffer, so DQBUF's bytesused is the number to look at.
		 */
		fprintf(stderr, "capturing %s %ux%u, buffer %u bytes (%s)\n",
			code, fmt.fmt.pix.width, fmt.fmt.pix.height,
			fmt.fmt.pix.sizeimage,
			fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG ?
			"JPEG frames are shorter, bytesused is the frame" :
			"one frame");
	}

	for (i = 0; i < nctrls; i++) {
		if (apply_ctrl(fd, &ctrls[i]) < 0) {
			close(fd);
			return 1;
		}
	}

	/*
	 * Do not ask for more frame buffers than the largest free block can
	 * hold.  A 640x480 16-bit frame is 600 KiB and this board's biggest
	 * contiguous block is about 1 MiB, so the request drops to a single
	 * buffer there while smaller frames keep the full count.
	 */
	{
		unsigned long block = largest_free_block();
		unsigned long per_frame = fmt.fmt.pix.sizeimage;

		if (block && per_frame && (unsigned long)nbufs > block / per_frame) {
			unsigned int fit = block / per_frame;

			if (fit < 1)
				fit = 1;
			fprintf(stderr,
				"%s: largest free block %lu KiB, %lu KiB/frame: "
				"using %u of %d buffers\n",
				progname, block / 1024, per_frame / 1024, fit,
				nbufs);
			nbufs = fit;
		}
	}

	memset(&req, 0, sizeof(req));
	req.count = nbufs;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		return 1;
	}
	/*
	 * The driver may hand back fewer buffers than requested: every buffer
	 * is physically contiguous on this no-MMU target, so a large frame
	 * size can run the allocator out of room.  One buffer is a complete
	 * queue for a single-shot port, so accept whatever we were given.
	 */
	if (req.count < 1) {
		fprintf(stderr, "%s: the driver allocated no buffers\n",
			progname);
		return 1;
	}

	buffers = calloc(req.count, sizeof(*buffers));
	if (!buffers) {
		perror("calloc");
		return 1;
	}
	for (i = 0; i < (int)req.count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			return 1;
		}
		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
	}

	for (i = 0; i < (int)req.count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			return 1;
		}
	}
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		return 1;
	}

	for (j = 0; (int)saved < save; j++) {
		struct v4l2_buffer buf;
		int last;
		int out_fd = -1;
		char resolved[512];
		ssize_t written;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			perror("VIDIOC_DQBUF");
			break;
		}

		if (!ready && frame_usable(fd, j, warmup, &settle)) {
			ready = 1;
			if (j > warmup)
				fprintf(stderr,
					"%s: dropped %d frames waiting for the "
					"sensor's auto exposure to settle\n",
					progname, j);
		}
		last = ready;

		if (last) {
			/*
			 * The port flags a frame it could not deliver whole
			 * (FIFO overflow, short frame, compressed stream cut
			 * at the buffer limit).  Report it and keep going, but
			 * never let it pass as a good capture.
			 */
			if (buf.flags & V4L2_BUF_FLAG_ERROR) {
				fprintf(stderr,
					"%s: frame %d is BAD: the device reported "
					"V4L2_BUF_FLAG_ERROR after %u of %u bytes; "
					"the frame is incomplete or corrupt\n",
					progname, j, buf.bytesused,
					fmt.fmt.pix.sizeimage);
				bad++;
			}
			open_output(out_path, saved, save, &out_fd, resolved,
				    sizeof(resolved));
			written = write(out_fd, buffers[buf.index].start,
					buf.bytesused);
			if (written != (ssize_t)buf.bytesused) {
				fprintf(stderr, "%s: short write to %s\n",
					progname, resolved);
			} else {
				fprintf(stderr, "saved %s: %u bytes\n",
					resolved, buf.bytesused);
			}
			if (out_fd != STDOUT_FILENO)
				close(out_fd);
			saved++;
		}

		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			break;
		}
	}

	if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
		perror("VIDIOC_STREAMOFF");

	for (i = 0; i < (int)req.count; i++)
		munmap(buffers[i].start, buffers[i].length);
	free(buffers);
	close(fd);

	if ((unsigned int)save != saved) {
		fprintf(stderr, "%s: saved %u of %d frames\n", progname, saved,
			save);
		return 1;
	}
	if (bad) {
		fprintf(stderr,
			"%s: %u of %u saved frames were flagged bad; do not "
			"treat them as captures\n", progname, bad, saved);
		return 1;
	}
	return 0;
}
