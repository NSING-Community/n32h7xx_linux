/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * camera-matrix.c - capture one frame for every format x size combination.
 *
 * The camera is an ordinary V4L2 capture device and this board's DVP port has
 * neither a scaler nor a format converter, so what the sensor is programmed
 * for is exactly what lands in the buffer.  This example walks the whole
 * matrix in one run, saves one frame per combination to a file, and checks
 * each frame before calling that combination good:
 *
 *   - the driver has to accept the requested pixel format and report it back;
 *     a format the device does not support is skipped, not written out;
 *   - a frame the driver flagged with V4L2_BUF_FLAG_ERROR is never counted as
 *     a capture, however many bytes it produced;
 *   - an uncompressed frame must be exactly bytes-per-line * height bytes,
 *     which is what catches a frame truncated by the port;
 *   - a JPEG frame must begin with SOI (FF D8) and end with EOI (FF D9),
 *     which is what catches a compressed stream cut short.
 *
 * Requested and granted size can differ: the port will not hand out a frame
 * bigger than the memory it has to put it in and falls back to a smaller
 * size.  Both are printed, and the file name carries the size really used.
 *
 * Build and install (see doc/camera.md):
 *
 *   make -C sdk/example camera-matrix
 *   adb push sdk/example/camera-matrix /system/bin/
 *
 * Usage:
 *
 * By default it walks the combinations this board delivers: one frame each,
 * so a failure here is the port losing a frame in front of you rather than
 * anything the sensor mode cannot do.  -a walks every mode the sensor has
 * instead, which is the bring-up view: some of those combinations are past
 * the port's line-length limit and are expected to fail.
 *
 *   camera-matrix                                 the supported combinations
 *   camera-matrix -a                              every sensor mode
 *   camera-matrix -f GREY -s 160x120,320x240      just the small grayscale ones
 *   camera-matrix -f JPEG -c test_pattern=1       JPEG of the ISP colour bars
 *   camera-matrix -o /mnt/sd/cam -q 1             write the frames to the SD card
 *
 * Every frame is written as a raw dump: the uncompressed formats have no file
 * header, and the JPEG files are single complete JPEG images.  Frame size,
 * byte count and frame time are reported per combination, and the exit status
 * is non-zero if any combination failed, so the summary can be used in a
 * script.
 *
 * The reported frame time is the gap between two frames of the same run.
 * The wait for the first frame after a mode change is much longer, because
 * the sensor has to settle into the new mode, so it is left out; the column
 * is what the camera can sustain, not what the first frame costs.
 *
 * The frame that is saved is not simply the one after -w frames: the sensor
 * restarts its auto exposure and auto white balance on every STREAMON and
 * takes a few frames to converge, and a frame grabbed from that ramp comes out
 * dark, blown out or with a green cast.  Frames are dropped until the exposure
 * and gain the sensor reports have held still; see frame_usable().
 *
 * Memory: each combination reserves and then releases its own physically
 * contiguous buffers, because a buffer is exactly one frame of the size in
 * use.  Writing the frames to the SD card (-o /mnt/sd/...) rather than to
 * ramfs and keeping the buffer count low (-q) leaves the most room for a
 * large frame.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define MAX_FORMATS	8
#define MAX_SIZES	16
#define MAX_CTRLS	8

/*
 * How long to wait for one frame.  A 2592x1944 frame takes on the order of
 * 100 ms; the margin is for the sensor's own settling time after a mode
 * change.  Without a bound, a port that stops delivering would leave DQBUF
 * waiting forever, so every wait goes through poll() first.
 */
#define FRAME_TIMEOUT_MS 5000

/*
 * How far back from the end of a JPEG frame to look for its EOI marker.  The
 * port pads the last bus word, so a handful of bytes can follow it.
 */
#define JPEG_EOI_WINDOW 64

struct buffer {
	void	*start;
	size_t	length;
};

struct format {
	const char	*name;		/* as typed on the command line */
	uint32_t	fourcc;
	const char	*ext;		/* file name suffix */
	unsigned int	bits;		/* bits per pixel, 0 = compressed */
	const char	*sizes;		/* the sizes this board delivers reliably */
};

struct size {
	unsigned int	width;
	unsigned int	height;
};

struct ctrl_opt {
	char		name[64];
	long long	value;
};

static const char *progname = "camera-matrix";

/*
 * The pixel formats this board can deliver without conversion, each with the
 * resolutions it holds up at.  The port writes a line as fast as it arrives
 * and has no scaler, so how long a line is decides whether a mode is stable.
 * Every list here was measured over repeated captures of a real scene: below
 * the line-length cliff the port loses a frame only once in a few hundred at
 * any size, above it, it loses them in bursts.  -a walks the sensor's whole
 * mode list instead, including the modes past the cliff.
 */
static const struct format formats[] = {
	{ "GREY",   V4L2_PIX_FMT_GREY,   "gray",   8,
	  "160x120,176x144,320x240,640x480,720x480,720x576" },
	{ "YUYV",   V4L2_PIX_FMT_YUYV,   "yuyv",  16,
	  "160x120,176x144,320x240" },
	{ "RGB565", V4L2_PIX_FMT_RGB565, "rgb565", 16,
	  "160x120,176x144,320x240" },
	{ "JPEG",   V4L2_PIX_FMT_JPEG,   "jpg",     0,
	  "160x120,176x144,320x240,640x480,720x480,720x576,1024x768,"
	  "1280x720,1920x1080,2592x1944" },
};

/*
 * Every mode the OV5640 has, largest last.  These are the sensor's own mode
 * list, so each one is a mode the sensor really has rather than an arbitrary
 * rectangle the driver would have to approximate; -a walks this list.
 */
static const struct size all_sizes[] = {
	{  160,  120 },	/* QQVGA */
	{  176,  144 },	/* QCIF  */
	{  320,  240 },	/* QVGA  */
	{  640,  480 },	/* VGA   */
	{  720,  480 },	/* NTSC  */
	{  720,  576 },	/* PAL   */
	{ 1024,  768 },	/* XGA   */
	{ 1280,  720 },	/* 720p  */
	{ 1920, 1080 },	/* 1080p */
	{ 2592, 1944 },	/* QSXGA */
};

static void usage(void)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -d DEV     video device (default /dev/video0)\n"
		"  -f LIST    comma separated formats, from GREY, YUYV, RGB565,\n"
		"             JPEG (default all of them)\n"
		"  -s LIST    comma separated sizes, e.g. 160x120,320x240\n"
		"             (default: what each format delivers reliably)\n"
		"  -a         every mode the sensor has, not just the\n"
		"             reliable ones (some are expected to fail)\n"
		"  -o DIR     directory for the frames (default .)\n"
		"  -q N       capture buffers per combination (default 2)\n"
		"  -w N       drop at least N frames before the saved one\n"
		"             (default 2).  More are dropped if the sensor's auto\n"
		"             exposure needs longer to settle\n"
		"  -c N=V     set control N to V before capturing (repeatable).\n"
		"             Names ignore case, spaces and underscores, so\n"
		"             test_pattern=1 or \"Test Pattern\"=1 both work.\n"
		"  -l         list the device's formats and what would be\n"
		"             captured, then exit\n",
		progname);
}

static void fourcc_to_str(uint32_t fourcc, char out[5])
{
	out[0] = fourcc & 0xff;
	out[1] = (fourcc >> 8) & 0xff;
	out[2] = (fourcc >> 16) & 0xff;
	out[3] = (fourcc >> 24) & 0xff;
	out[4] = '\0';
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

static int apply_ctrl(int fd, const struct ctrl_opt *opt)
{
	struct v4l2_control c;
	int id = find_ctrl(fd, opt->name);

	if (id < 0) {
		fprintf(stderr, "%s: no control named '%s'\n", progname,
			opt->name);
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

	if (ioctl(fd, VIDIOC_G_CTRL, &c) == 0)
		printf("control %s = %d\n", opt->name, c.value);

	return 0;
}

/* Does the device itself advertise this pixel format? */
static int device_has_format(int fd, uint32_t fourcc)
{
	struct v4l2_fmtdesc desc;
	unsigned int n = 0;

	memset(&desc, 0, sizeof(desc));
	desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (;;) {
		desc.index = n;
		if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0)
			return 0;
		if (desc.pixelformat == fourcc)
			return 1;
		n++;
	}
}

static void list_formats(int fd)
{
	struct v4l2_fmtdesc desc;
	struct v4l2_format fmt;
	char code[5];
	unsigned int n = 0;

	memset(&desc, 0, sizeof(desc));
	desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	printf("device formats:\n");
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
	if (!n)
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

/* "5038848 (4.8 MiB)" - the byte counts here are not human-sized on purpose. */
static void human_bytes(unsigned int bytes, char *out, size_t outlen)
{
	if (bytes >= 1024 * 1024)
		snprintf(out, outlen, "%u (%.1f MiB)", bytes,
			 (double)bytes / (1024 * 1024));
	else if (bytes >= 1024)
		snprintf(out, outlen, "%u (%.1f KiB)", bytes,
			 (double)bytes / 1024);
	else
		snprintf(out, outlen, "%u", bytes);
}

static double now_ms(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	return (double)t.tv_sec * 1000.0 + (double)t.tv_usec / 1000.0;
}

/*
 * Statistics for an 8-bit luminance frame: enough to tell a real picture from
 * a black or blank one, which is the first thing to check before feeding a
 * grayscale frame to anything.
 */
static void gray_stats(const unsigned char *p, unsigned int n, unsigned int *min,
		       unsigned int *max, unsigned int *mean)
{
	unsigned long sum = 0;
	unsigned int lo = 255, hi = 0;
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned char v = p[i];

		if (v < lo)
			lo = v;
		if (v > hi)
			hi = v;
		sum += v;
	}

	*min = lo;
	*max = hi;
	*mean = n ? (unsigned int)(sum / n) : 0;
}

/*
 * Offset of the last EOI marker in a JPEG frame, or -1.  Only the tail is
 * searched: a complete frame always ends with one, and scanning a
 * multi-megabyte frame from the front would cost more than the check is
 * worth.  The window has to be more than a couple of bytes wide because the
 * port writes whole bus words, so a few bytes can follow the marker.
 */
static int jpeg_last_eoi(const unsigned char *p, unsigned int bytes)
{
	int i, from;

	if (bytes < 4)
		return -1;

	i = (int)bytes - 2;
	from = (int)bytes - JPEG_EOI_WINDOW;
	if (from < 2)
		from = 2;

	for (; i >= from; i--)
		if (p[i] == 0xff && p[i + 1] == 0xd9)
			return i;

	return -1;
}

/*
 * Validate the frame in buf and write it out.  Returns 0 when the frame is
 * good.  The file is written either way, so a failing combination still
 * leaves something to look at.  *written comes back as the number of bytes
 * the file holds, which is the frame's real length for every format.
 */
static int save_frame(const struct format *fmt, const struct buffer *buf,
		      unsigned int bytes, unsigned int expected,
		      const char *out_dir, const char *file,
		      unsigned int *written, char *note, size_t note_len)
{
	char path[256];
	unsigned int stored = bytes;
	int out_fd;
	ssize_t put;
	int bad = 0;

	if (fmt->bits == 0) {
		/*
		 * JPEG is variable length, so the markers are the only
		 * in-band way to tell a complete frame from a cut one.  The
		 * file gets everything up to and including the EOI: the bytes
		 * the port counted past it are its own word padding, and a
		 * JPEG with trailing bytes trips some decoders.
		 */
		const unsigned char *p = buf->start;
		int eoi;

		if (bytes < 4 || p[0] != 0xff || p[1] != 0xd8) {
			bad = 1;
			snprintf(note, note_len, "no JPEG SOI marker");
		} else if ((eoi = jpeg_last_eoi(p, bytes)) < 0) {
			bad = 1;
			snprintf(note, note_len, "no JPEG EOI marker");
		} else {
			unsigned int pad = bytes - (unsigned int)eoi - 2;

			stored = (unsigned int)eoi + 2;
			if (bytes - stored > JPEG_EOI_WINDOW)
				snprintf(note, note_len,
					 "SOI..EOI ok, %u trailing bytes",
					 pad);
			else
				snprintf(note, note_len, "SOI..EOI ok");
		}
	} else if (bytes != expected) {
		bad = 1;
		snprintf(note, note_len, "wrote %u bytes, expected %u", bytes,
			 expected);
	} else if (fmt->fourcc == V4L2_PIX_FMT_GREY) {
		unsigned int lo, hi, mean;

		gray_stats(buf->start, bytes, &lo, &hi, &mean);
		snprintf(note, note_len, "min %u max %u mean %u", lo, hi, mean);
	}

	snprintf(path, sizeof(path), "%s/%s", out_dir, file);
	out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out_fd < 0) {
		snprintf(note, note_len, "cannot create %s: %s", file,
			 strerror(errno));
		return -1;
	}

	put = write(out_fd, buf->start, stored);
	close(out_fd);
	if (put != (ssize_t)stored) {
		snprintf(note, note_len, "short write to %s", file);
		return -1;
	}

	*written = stored;
	return bad ? -1 : 0;
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
 * Should the frame that was just dequeued be kept?
 *
 * The OV5640 runs its own auto exposure and auto white balance and restarts
 * both every time streaming starts, so the first frames after STREAMON are
 * not pictures of the scene yet: they run from far too dark through
 * overexposed, and the white balance drifts across a strong green cast on the
 * way.  A frame saved from that ramp is the usual reason a capture looks
 * wrong.
 *
 * The sensor has no "converged" flag, but the driver marks the exposure and
 * the analogue gain volatile and reads them back out of the sensor on
 * VIDIOC_G_CTRL, so the numbers the AEC is working with are visible from
 * userspace.  The frames stop being wrong when those stop changing.
 *
 * min_frames are dropped whatever happens.  Past that, frames are dropped
 * until the pair has held still for SETTLE_FRAMES reads, or until WARMUP_MAX
 * more frames have gone by, so a sensor mode that never settles still
 * produces a frame.
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

	return s->still >= SETTLE_FRAMES || j + 1 >= min_frames + WARMUP_MAX;
}

/*
 * One format at one size: program the sensor, capture one frame, check it.
 * Every combination gets its own buffers, because a buffer is exactly one
 * frame and the frame size changes from one combination to the next.
 */
static int capture_case(int fd, const struct format *fmt, const struct size *sz,
			unsigned int nbufs, unsigned int warmup,
			const char *out_dir, unsigned int *got_w,
			unsigned int *got_h, unsigned int *got_bytes,
			double *got_ms, char *file, size_t file_len,
			char *note, size_t note_len)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_requestbuffers req;
	struct v4l2_format f;
	struct buffer *bufs = NULL;
	double t_start, t_prev = 0.0;
	unsigned int expected = 0;
	unsigned int i, j;
	struct settle settle = { .exposure = -1, .gain = -1, .live = 1 };
	int mapped = 0;
	int streaming = 0;
	int usable = 0;
	int ret = -1;

	memset(&f, 0, sizeof(f));
	f.type = type;
	f.fmt.pix.pixelformat = fmt->fourcc;
	f.fmt.pix.width = sz->width;
	f.fmt.pix.height = sz->height;
	f.fmt.pix.field = V4L2_FIELD_ANY;

	if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
		snprintf(note, note_len, "S_FMT: %s", strerror(errno));
		return -1;
	}

	/* A device that silently substitutes another format would leave a
	 * file that cannot be decoded as what its name says it is. */
	if (f.fmt.pix.pixelformat != fmt->fourcc) {
		char code[5];

		fourcc_to_str(f.fmt.pix.pixelformat, code);
		snprintf(note, note_len, "device chose %s instead", code);
		return -1;
	}

	*got_w = f.fmt.pix.width;
	*got_h = f.fmt.pix.height;
	if (fmt->bits)
		expected = f.fmt.pix.bytesperline * f.fmt.pix.height;

	/*
	 * The file is named after the size that was asked for, not the size
	 * that came back: a run has one combination per name, so a size the
	 * port had to shrink cannot overwrite another combination's frame.
	 * The table reports the size really used.
	 */
	snprintf(file, file_len, "%s_%ux%u.%s", fmt->name, sz->width,
		 sz->height, fmt->ext);

	memset(&req, 0, sizeof(req));
	req.count = nbufs;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		snprintf(note, note_len, "REQBUFS: %s", strerror(errno));
		return -1;
	}
	if (req.count < 1) {
		snprintf(note, note_len, "no buffers available");
		goto out;
	}

	bufs = calloc(req.count, sizeof(*bufs));
	if (!bufs) {
		snprintf(note, note_len, "calloc: %s", strerror(errno));
		goto out;
	}

	for (i = 0; i < req.count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			snprintf(note, note_len, "QUERYBUF: %s",
				 strerror(errno));
			goto out;
		}
		bufs[i].length = buf.length;
		bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, buf.m.offset);
		if (bufs[i].start == MAP_FAILED) {
			snprintf(note, note_len, "mmap: %s", strerror(errno));
			goto out;
		}
		mapped = i + 1;
	}

	for (i = 0; i < req.count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			snprintf(note, note_len, "QBUF: %s", strerror(errno));
			goto out;
		}
	}

	t_start = now_ms();
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		snprintf(note, note_len, "STREAMON: %s", strerror(errno));
		goto out;
	}
	streaming = 1;

	for (j = 0; !usable; j++) {
		struct pollfd pfd;
		struct v4l2_buffer buf;
		double t_now;
		int ready;

		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		ready = poll(&pfd, 1, FRAME_TIMEOUT_MS);
		if (ready < 0) {
			snprintf(note, note_len, "poll: %s", strerror(errno));
			goto out;
		}
		if (ready == 0) {
			snprintf(note, note_len, "no frame within %d ms",
				 FRAME_TIMEOUT_MS);
			goto out;
		}

		memset(&buf, 0, sizeof(buf));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			snprintf(note, note_len, "DQBUF: %s", strerror(errno));
			goto out;
		}
		t_now = now_ms();

		/* Never trust a length the driver reports past the mapping
		 * it handed out: writing that many bytes would read someone
		 * else's memory. */
		if (buf.bytesused > bufs[buf.index].length) {
			snprintf(note, note_len,
				 "frame reports %u bytes in a %zu byte buffer",
				 buf.bytesused, bufs[buf.index].length);
			buf.bytesused = bufs[buf.index].length;
			ret = -1;
		}

		/* Hold the frame back until the sensor has settled; see
		 * frame_usable().  -w is only the floor. */
		usable = frame_usable(fd, (int)j, (int)warmup, &settle);

		if (usable) {
			/*
			 * The frame before this one is the honest measure of
			 * how fast frames arrive: the wait for the very first
			 * frame also contains the sensor's settling time after
			 * the mode change, which is not a frame period.
			 */
			*got_ms = j > 0 ? t_now - t_prev : t_now - t_start;
			/* Overwritten with the size of the file when the frame
			 * is saved; bytesused is only a fallback for the
			 * paths that never get there. */
			*got_bytes = buf.bytesused;

			if (buf.flags & V4L2_BUF_FLAG_ERROR) {
				snprintf(note, note_len,
					 "device flagged the frame bad (%u bytes)",
					 buf.bytesused);
				/* Keep the bytes so the file is still usable. */
				save_frame(fmt, &bufs[buf.index],
					   buf.bytesused, expected, out_dir,
					   file, got_bytes, note, note_len);
				ret = -1;
			} else if (buf.bytesused == 0) {
				snprintf(note, note_len,
					 "frame of zero bytes");
				ret = -1;
			} else {
				ret = save_frame(fmt, &bufs[buf.index],
						 buf.bytesused, expected,
						 out_dir, file, got_bytes,
						 note, note_len);
			}
			if (!ret && (*got_w != sz->width || *got_h != sz->height))
				snprintf(note + strlen(note),
					 note_len - strlen(note),
					 "%sfell back to %ux%u",
					 note[0] ? ", " : "", *got_w, *got_h);
		}

		t_prev = t_now;

		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			snprintf(note, note_len, "QBUF: %s", strerror(errno));
			ret = -1;
			goto out;
		}
	}

out:
	if (streaming)
		ioctl(fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < (unsigned int)mapped; i++)
		munmap(bufs[i].start, bufs[i].length);
	free(bufs);

	/* Release the frame buffers before the next combination asks for a
	 * different size; S_FMT is refused while the queue is busy. */
	memset(&req, 0, sizeof(req));
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl(fd, VIDIOC_REQBUFS, &req);

	return ret;
}

static int parse_formats(const char *list, const struct format *out[], int max)
{
	char work[128];
	char *save = NULL, *tok;
	int n = 0;

	snprintf(work, sizeof(work), "%s", list);
	for (tok = strtok_r(work, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		size_t i;
		int found = 0;

		for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
			if (strcasecmp(tok, formats[i].name))
				continue;
			if (n >= max) {
				fprintf(stderr, "%s: too many formats\n",
					progname);
				return -1;
			}
			out[n++] = &formats[i];
			found = 1;
			break;
		}
		if (!found) {
			fprintf(stderr, "%s: unknown format '%s'\n", progname,
				tok);
			return -1;
		}
	}

	return n;
}

static int parse_sizes(const char *list, struct size *out, int max)
{
	char work[256];
	char *save = NULL, *tok;
	int n = 0;

	snprintf(work, sizeof(work), "%s", list);
	for (tok = strtok_r(work, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		unsigned int w, h;

		if (sscanf(tok, "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
			fprintf(stderr, "%s: bad size '%s'\n", progname, tok);
			return -1;
		}
		if (n >= max) {
			fprintf(stderr, "%s: too many sizes\n", progname);
			return -1;
		}
		out[n].width = w;
		out[n].height = h;
		n++;
	}

	return n;
}

/*
 * The sizes to walk for @fmt: the list the caller gave with -s if there is
 * one, every mode the sensor has with -a, and the format's own reliable set
 * otherwise.  Returns the count, or a negative value on a bad list.
 */
static int sizes_for(const struct format *fmt, const struct size *given,
		     unsigned int ngiven, int all_modes, struct size *out,
		     unsigned int max)
{
	unsigned int i, n;

	if (ngiven) {
		for (i = 0; i < ngiven && i < max; i++)
			out[i] = given[i];
		return (int)ngiven;
	}

	if (!all_modes)
		return parse_sizes(fmt->sizes, out, max);

	n = sizeof(all_sizes) / sizeof(all_sizes[0]);
	if (n > max) {
		fprintf(stderr, "%s: too many sizes\n", progname);
		return -1;
	}
	for (i = 0; i < n; i++)
		out[i] = all_sizes[i];

	return (int)n;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/video0";
	const char *out_dir = ".";
	const struct format *want[MAX_FORMATS];
	struct size sizes[MAX_SIZES];
	struct ctrl_opt ctrls[MAX_CTRLS];
	unsigned int nfmt = 0, nsize = 0, nctrl = 0;
	unsigned int nbufs = 2, warmup = 2;
	int list = 0, all_modes = 0;
	int fd, c;
	unsigned int total = 0, failed = 0;
	unsigned int i;
	unsigned int fi, si;

	while ((c = getopt(argc, argv, "d:f:s:o:q:w:c:lah")) != -1) {
		switch (c) {
		case 'd':
			device = optarg;
			break;
		case 'f': {
			int n = parse_formats(optarg, want, MAX_FORMATS);

			if (n < 0)
				return 2;
			nfmt = n;
			break;
		}
		case 's': {
			int n = parse_sizes(optarg, sizes, MAX_SIZES);

			if (n < 0)
				return 2;
			nsize = n;
			break;
		}
		case 'o':
			out_dir = optarg;
			break;
		case 'q':
			nbufs = atoi(optarg);
			if ((int)nbufs < 1) {
				fprintf(stderr, "%s: -q must be >= 1\n",
					progname);
				return 2;
			}
			break;
		case 'w':
			warmup = atoi(optarg);
			if ((int)warmup < 0) {
				fprintf(stderr, "%s: -w must be >= 0\n",
					progname);
				return 2;
			}
			break;
		case 'c': {
			char *eq = strchr(optarg, '=');
			size_t len;

			if (!eq || nctrl >= MAX_CTRLS) {
				usage();
				return 2;
			}
			len = eq - optarg;
			if (len == 0 || len >= sizeof(ctrls[0].name)) {
				fprintf(stderr, "%s: bad control '%s'\n",
					progname, optarg);
				return 2;
			}
			memcpy(ctrls[nctrl].name, optarg, len);
			ctrls[nctrl].name[len] = '\0';
			ctrls[nctrl].value = strtoll(eq + 1, NULL, 0);
			nctrl++;
			break;
		}
		case 'l':
			list = 1;
			break;
		case 'a':
			all_modes = 1;
			break;
		default:
			usage();
			return 2;
		}
	}

	if (!nfmt) {
		for (fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++)
			want[nfmt++] = &formats[fi];
	}
	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", progname, device,
			strerror(errno));
		return 1;
	}

	if (list) {
		list_formats(fd);
		printf("would capture:");
		for (fi = 0; fi < nfmt; fi++) {
			struct size fsz[MAX_SIZES];
			int n;

			n = sizes_for(want[fi], sizes, nsize, all_modes, fsz,
				      MAX_SIZES);
			if (n < 0) {
				close(fd);
				return 2;
			}
			printf(" %s at", want[fi]->name);
			for (si = 0; si < (unsigned int)n; si++)
				printf(" %ux%u", fsz[si].width, fsz[si].height);
		}
		printf("\n");
		close(fd);
		return 0;
	}

	/*
	 * The sensor keeps the value until it is asked to stream, so setting
	 * the controls once here covers every combination below.
	 */
	for (i = 0; i < nctrl; i++) {
		if (apply_ctrl(fd, &ctrls[i]) < 0) {
			close(fd);
			return 1;
		}
	}

	for (fi = 0; fi < nfmt; fi++) {
		struct size fsz[MAX_SIZES];
		int nfsz;

		if (!device_has_format(fd, want[fi]->fourcc)) {
			char code[5];

			fourcc_to_str(want[fi]->fourcc, code);
			printf("%s: the device does not offer %s, skipping\n",
			       want[fi]->name, code);
			continue;
		}

		nfsz = sizes_for(want[fi], sizes, nsize, all_modes, fsz,
				 MAX_SIZES);
		if (nfsz < 0) {
			close(fd);
			return 2;
		}

		printf("%-7s %-11s   %-11s %-18s %-9s %-24s %s\n", "format",
		       "requested", "captured", "bytes", "frame",
		       "file", "result");

		for (si = 0; si < (unsigned int)nfsz; si++) {
			unsigned int got_w = 0, got_h = 0, bytes = 0;
			char size_str[32], geom[32];
			char file[64] = "";
			double ms = 0.0;
			char note[128] = "";
			int ret;

			ret = capture_case(fd, want[fi], &fsz[si], nbufs,
					   warmup, out_dir, &got_w, &got_h,
					   &bytes, &ms, file, sizeof(file),
					   note, sizeof(note));
			total++;
			if (ret)
				failed++;

			human_bytes(bytes, size_str, sizeof(size_str));
			if (got_w && got_h)
				snprintf(geom, sizeof(geom), "%ux%u", got_w,
					 got_h);
			else
				snprintf(geom, sizeof(geom), "-");
			printf("%-7s %4ux%-5u -> %-10s %-18s %6.1f ms  %-24s %s",
			       want[fi]->name, fsz[si].width,
			       fsz[si].height, geom, size_str, ms,
			       file[0] ? file : "-", ret ? "FAIL" : "ok");
			if (note[0])
				printf("  [%s]", note);
			printf("\n");
			fflush(stdout);
		}
	}

	close(fd);

	if (failed) {
		printf("%u of %u combinations failed\n", failed, total);
		return 1;
	}

	printf("all %u combinations captured\n", total);
	return 0;
}
