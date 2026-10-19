/* SPDX-License-Identifier: BSD-3-Clause */
#include "n32jpeg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *name)
{
	fprintf(stderr,
		"usage: %s [-d device] [-s widthxheight] [-q 1..100] [-n count]\n"
		"       [-c output-capacity] [-r] [-o file]\n"
		"       default: -s 1024x768 -q 80 -o /tmp/n32jpeg-test.jpg\n",
		name);
}

static int output_name(char *dst, size_t size, const char *base,
		       unsigned int frame, unsigned int frames)
{
	int ret;

	if (frames == 1)
		ret = snprintf(dst, size, "%s", base);
	else
		ret = snprintf(dst, size, "%s.%u", base, frame);

	if (ret < 0 || (size_t)ret >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int parse_size(const char *text, unsigned int *width,
		      unsigned int *height)
{
	char tail;

	if (sscanf(text, "%ux%u%c", width, height, &tail) != 2)
		return -1;
	return 0;
}

static int write_all(int fd, const unsigned char *data, size_t size)
{
	while (size) {
		ssize_t written = write(fd, data, size);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		data += written;
		size -= (size_t)written;
	}
	return 0;
}

static void dump_failed_output(const unsigned char *data, size_t size)
{
	const char *path = getenv("N32JPEG_DUMP_RAW");
	size_t i, first = size, last = size;
	int fd;

	if (!path || !*path)
		return;

	for (i = 0; i + 1 < size; i++) {
		if (data[i] != 0xff || data[i + 1] != 0xd9)
			continue;
		if (first == size)
			first = i;
		last = i;
	}

	fprintf(stderr, "raw output: capacity=%zu first_eoi=%zd last_eoi=%zd\n",
		size, first == size ? -1 : (ssize_t)first,
		last == size ? -1 : (ssize_t)last);
	for (i = 0; i < 32 && i < size; i++)
		fprintf(stderr, "%02x%c", data[i], i == 31 ? '\n' : ' ');
	for (i = size > 32 ? size - 32 : 0; i < size; i++)
		fprintf(stderr, "%02x%c", data[i], i + 1 == size ? '\n' : ' ');

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror(path);
		return;
	}
	if (write_all(fd, data, size) < 0)
		perror("write raw failure snapshot");
	close(fd);
}

int main(int argc, char **argv)
{
	const char *device = N32JPEG_DEFAULT_DEVICE;
	const char *path = "/tmp/n32jpeg-test.jpg";
	unsigned int width = 1024, height = 768, quality = 80;
	unsigned int count = 1, frame;
	struct n32jpeg_info info;
	unsigned char *input = NULL, *output = NULL;
	unsigned int x, y;
	__u32 encoded = 0;
	size_t input_size, output_capacity, requested_capacity = 0;
	char path_buf[512];
	int recovery_test = 0;
	int fd = -1, out = -1, ret = EXIT_FAILURE, opt;

	while ((opt = getopt(argc, argv, "d:s:q:n:c:ro:h")) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 's':
			if (parse_size(optarg, &width, &height) < 0) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			break;
		case 'q':
		{
			char *end;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 0);
			if (errno || *end || value < 1 || value > 100) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			quality = (unsigned int)value;
			break;
		}
		case 'n':
		{
			char *end;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 0);
			if (errno || *end || !value || value > 1000) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			count = (unsigned int)value;
			break;
		}
		case 'c':
		{
			char *end;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 0);
			if (errno || *end || value < 4 || value > UINT32_MAX) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			requested_capacity = (size_t)value;
			break;
		}
		case 'r':
			recovery_test = 1;
			break;
		case 'o':
			path = optarg;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (!width || !height || quality < 1 || quality > 100) {
		fprintf(stderr, "invalid width, height or quality\n");
		return EXIT_FAILURE;
	}

	input_size = (size_t)width * height;
	output_capacity = input_size + 4096;
	if (requested_capacity)
		output_capacity = requested_capacity;
	if (input_size / width != height || output_capacity < 4) {
		fprintf(stderr, "frame size overflow\n");
		return EXIT_FAILURE;
	}

	fd = n32jpeg_open(device);
	if (fd < 0) {
		perror("n32jpeg_open");
		return EXIT_FAILURE;
	}
	if (n32jpeg_query(fd, &info) < 0) {
		perror("n32jpeg_query");
		goto out;
	}
	if (!(info.formats & (1U << N32JPEG_FORMAT_GREY8))) {
		fprintf(stderr, "device does not advertise GREY8\n");
		goto out;
	}
	if (width < info.min_width || width > info.max_width ||
	    height < info.min_height || height > info.max_height) {
		fprintf(stderr, "requested size is outside driver limits\n");
		goto out;
	}
	if ((width & 7) || (height & 7)) {
		fprintf(stderr, "width and height must be multiples of 8\n");
		goto out;
	}

	input = n32jpeg_alloc((__u32)input_size);
	output = n32jpeg_alloc((__u32)output_capacity);
	if (!input || !output) {
		perror("allocate frame buffers");
		goto out;
	}

	if (recovery_test) {
		int short_errno;

		/* Force a truncated DMA, then prove the same fd can encode again. */
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++)
				input[y * width + x] =
					(unsigned char)((x * 7 + y * 13) & 0xff);
		}
		errno = 0;
		if (n32jpeg_encode_grey8(fd, input, width, height, 100, output,
					 128, &encoded) == 0 || errno != ENOSPC) {
			short_errno = errno;
			fprintf(stderr,
				"short-output recovery: expected ENOSPC, errno=%d (%s), size=%u\n",
				short_errno, strerror(short_errno), encoded);
			goto out;
		}
		printf("short-output recovery: ENOSPC rejected as expected\n");
	}

	for (frame = 0; frame < count; frame++) {
		/* A changing deterministic frame catches stale or all-zero output. */
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++)
				input[y * width + x] = (unsigned char)
					((x * (3 + frame) + y * (5 + 2 * frame) +
					  frame * 29) & 0xff);
		}

		if (n32jpeg_encode_grey8(fd, input, width, height, quality,
					 output, (__u32)output_capacity,
					 &encoded) < 0) {
			dump_failed_output(output, output_capacity);
			perror("n32jpeg_encode_grey8");
			goto out;
		}
		if (encoded < 4 || output[0] != 0xff || output[1] != 0xd8 ||
		    output[encoded - 2] != 0xff || output[encoded - 1] != 0xd9) {
			fprintf(stderr, "invalid JPEG markers, size=%u\n", encoded);
			goto out;
		}
		if (output_name(path_buf, sizeof(path_buf), path, frame, count)) {
			perror("output path");
			goto out;
		}

		out = open(path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			perror(path_buf);
			goto out;
		}
		if (write_all(out, output, encoded) < 0) {
			perror("write");
			goto out;
		}
		close(out);
		out = -1;
		printf("%ux%u GREY8 q%u frame %u/%u -> %s (%u bytes)\n",
		       width, height, quality, frame + 1, count, path_buf,
		       encoded);
	}
	ret = EXIT_SUCCESS;

out:
	if (out >= 0)
		close(out);
	n32jpeg_free(output);
	n32jpeg_free(input);
	close(fd);
	return ret;
}
