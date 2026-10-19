/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HUMAN_DETECT_DETECTOR_H
#define HUMAN_DETECT_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUMAN_DETECT_MAX_BOXES 6

struct human_detect_box {
	uint16_t x0;
	uint16_t y0;
	uint16_t x1;
	uint16_t y1;
	uint8_t score;
};

struct human_detect_result {
	uint8_t count;
	struct human_detect_box boxes[HUMAN_DETECT_MAX_BOXES];
};

int human_detector_init(void);
int human_detector_prepare_gray8(const uint8_t *gray8, size_t stride);
int human_detector_invoke(struct human_detect_result *result);
int human_detector_set_threshold(unsigned int threshold);
size_t human_detector_arena_bytes(void);
const char *human_detector_error_string(void);

#ifdef __cplusplus
}
#endif

#endif
