/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HUMAN_DETECT_PREVIEW_GRAY8_H
#define HUMAN_DETECT_PREVIEW_GRAY8_H

#include <stdint.h>

#define HUMAN_DETECT_GRAY_MAGIC 0x38474448u /* "HDG8" on little-endian */
#define HUMAN_DETECT_GRAY_VERSION 1u
#define HUMAN_DETECT_GRAY_HEADER_SIZE 128u
#define HUMAN_DETECT_PREVIEW_WIDTH 320u
#define HUMAN_DETECT_PREVIEW_HEIGHT 240u
#define HUMAN_DETECT_PREVIEW_BOXES 6u

struct human_detect_preview_box {
	uint16_t x0;
	uint16_t y0;
	uint16_t x1;
	uint16_t y1;
	uint8_t score;
	uint8_t reserved;
} __attribute__((packed));

struct human_detect_gray_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint16_t width;
	uint16_t height;
	uint32_t stride;
	uint64_t sequence;
	uint64_t frames;
	uint32_t camera_errors;
	uint32_t inference_errors;
	uint32_t inference_fps_x100;
	uint32_t prepare_us;
	uint32_t inference_us;
	uint32_t frame_age_ms;
	uint8_t human_present;
	uint8_t box_count;
	uint8_t camera_ready;
	uint8_t reserved0;
	struct human_detect_preview_box boxes[HUMAN_DETECT_PREVIEW_BOXES];
	uint8_t reserved[8];
} __attribute__((packed));

#endif
