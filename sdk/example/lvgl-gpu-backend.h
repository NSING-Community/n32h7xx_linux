/* SPDX-License-Identifier: MIT */
#ifndef LVGL_GPU_BACKEND_H
#define LVGL_GPU_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct lvgl_gpu_backend lvgl_gpu_backend_t;

int lvgl_gpu_discover(char *lcdc_node, size_t lcdc_size,
                      char *gpu_node, size_t gpu_size);
lvgl_gpu_backend_t *lvgl_gpu_display_create(lv_display_t **display,
                                            const char *lcdc_node,
                                            const char *gpu_node);
void lvgl_gpu_display_destroy(lvgl_gpu_backend_t *backend);
const char *lvgl_gpu_lcdc_device(const lvgl_gpu_backend_t *backend);
const char *lvgl_gpu_gpu_device(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_fill_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_batch_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_polygon_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_poly_batch_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_line_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_arc_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_triangle_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_border_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_present_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_large_fill_fallback_count(const lvgl_gpu_backend_t *backend);
uint32_t lvgl_gpu_max_fill_pixels(const lvgl_gpu_backend_t *backend);

#endif
