/* SPDX-License-Identifier: BSD-3-Clause */
#include "n32jpeg.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

int n32jpeg_open(const char *path)
{
	return open(path ? path : N32JPEG_DEFAULT_DEVICE, O_RDWR | O_CLOEXEC);
}

int n32jpeg_query(int fd, struct n32jpeg_info *info)
{
	struct n32jpeg_query query;

	if (!info) {
		errno = EINVAL;
		return -1;
	}
	if (ioctl(fd, N32JPEG_QUERY, &query) < 0)
		return -1;
	info->abi_version = query.abi_version;
	info->formats = query.formats;
	info->min_width = query.min_width;
	info->min_height = query.min_height;
	info->max_width = query.max_width;
	info->max_height = query.max_height;
	info->input_alignment = query.input_alignment;
	info->output_alignment = query.output_alignment;
	return 0;
}

int n32jpeg_encode_grey8(int fd, const void *input, __u32 width,
			 __u32 height, __u32 quality, void *output,
			 __u32 output_capacity, __u32 *output_size)
{
	struct n32jpeg_encode encode = {
		.struct_size = sizeof(encode),
		.format = N32JPEG_FORMAT_GREY8,
		.width = width,
		.height = height,
		.input_stride = width,
		.quality = quality,
		.input = (uintptr_t)input,
		.output = (uintptr_t)output,
		.output_capacity = output_capacity,
	};
	uint64_t pixels;

	if (!input || !output || !output_capacity || !output_size ||
	    !width || !height || quality < 1 || quality > 100) {
		errno = EINVAL;
		return -1;
	}
	pixels = (uint64_t)width * height;
	if (pixels > UINT32_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	encode.input_size = (uint32_t)pixels;
	if (ioctl(fd, N32JPEG_ENCODE, &encode) < 0)
		return -1;
	*output_size = encode.output_size;
	return 0;
}

void *n32jpeg_alloc(__u32 size)
{
	if (!size) {
		errno = EINVAL;
		return NULL;
	}
	return memalign(8, size);
}

void n32jpeg_free(void *ptr)
{
	free(ptr);
}
