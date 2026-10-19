/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef N32JPEG_H
#define N32JPEG_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define N32JPEG_ABI_VERSION	1
#define N32JPEG_DEFAULT_DEVICE	"/dev/jpeg"

enum n32jpeg_format {
	N32JPEG_FORMAT_GREY8 = 1,
	N32JPEG_FORMAT_RGB24 = 2,
	N32JPEG_FORMAT_YUV422 = 3,
};

struct n32jpeg_query {
	__u32 abi_version;
	__u32 formats;
	__u32 min_width;
	__u32 min_height;
	__u32 max_width;
	__u32 max_height;
	__u32 input_alignment;
	__u32 output_alignment;
	__u32 flags;
	__u32 reserved[7];
};

struct n32jpeg_encode {
	__u32 struct_size;
	__u32 flags;
	__u32 format;
	__u32 width;
	__u32 height;
	__u32 input_stride;
	__u32 input_size;
	__u32 quality;
	__aligned_u64 input;
	__aligned_u64 output;
	__u32 output_capacity;
	__u32 output_size;
	__u32 reserved[6];
};

struct n32jpeg_info {
	__u32 abi_version;
	__u32 formats;
	__u32 min_width;
	__u32 min_height;
	__u32 max_width;
	__u32 max_height;
	__u32 input_alignment;
	__u32 output_alignment;
};

#define N32JPEG_IOC_MAGIC	'J'
#define N32JPEG_QUERY		_IOR(N32JPEG_IOC_MAGIC, 0x00, \
				     struct n32jpeg_query)
#define N32JPEG_ENCODE		_IOWR(N32JPEG_IOC_MAGIC, 0x01, \
				     struct n32jpeg_encode)

#ifdef __cplusplus
extern "C" {
#endif

/* Returns an fd for the exclusive /dev/jpeg encoder, or -1 with errno set. */
int n32jpeg_open(const char *path);
int n32jpeg_query(int fd, struct n32jpeg_info *info);

/*
 * Encode one tightly packed GREY8 frame.  width and height must be multiples
 * of 8.  input and output must be physically contiguous for the no-MMU
 * hardware DMA path and aligned to the values reported by n32jpeg_query().
 * The driver writes output_size bytes.  Returns 0, or -1 with errno set by
 * the kernel.
 */
int n32jpeg_encode_grey8(int fd, const void *input, __u32 width,
			 __u32 height, __u32 quality, void *output,
			 __u32 output_capacity, __u32 *output_size);

/* Convenience aligned allocation for SDK users; free with n32jpeg_free. */
void *n32jpeg_alloc(__u32 size);
void n32jpeg_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* N32JPEG_H */
