/* SPDX-License-Identifier: MIT */
/*
 * LVGL touchscreen coordinate demo for the N32H787 HMI panel.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "lvgl.h"
#include "lvgl-gpu-backend.h"

#define TRAIL_CHUNK_POINTS 24
#define TRAIL_MAX_CHUNKS 96
#define TRAIL_LINE_WIDTH 4
#define TRAIL_MARGIN 4
#define TOUCH_QUEUE_CAPACITY 2048
#define TOUCH_DRAIN_BATCH 64
#define TOUCH_THREAD_STACK (48 * 1024)
#define TOUCH_MAX_SLOTS 10
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

typedef struct {
    lv_obj_t *obj;
    lv_point_precise_t points[TRAIL_CHUNK_POINTS];
    lv_point_precise_t global_points[TRAIL_CHUNK_POINTS];
    uint32_t count;
    uint8_t slot;
    bool dirty;
} trail_chunk_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t slot;
    uint8_t pressed;
} touch_sample_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t press_x;
    int32_t press_y;
    uint32_t current_chunk;
    uint32_t sample_count;
    uint32_t start_tick;
    bool down;
    bool dragging;
    bool has_last;
} touch_slot_state_t;

static volatile sig_atomic_t running = 1;
static lv_obj_t *coordinate_label;
static lv_obj_t *state_label;
static lv_obj_t *touch_marker;
static lv_obj_t *touch_dot;
static lv_obj_t *touch_slot_markers[TOUCH_MAX_SLOTS];
static lv_obj_t *touch_slot_dots[TOUCH_MAX_SLOTS];
static trail_chunk_t trail_chunks[TRAIL_MAX_CHUNKS];
static uint32_t trail_chunk_count;
static touch_slot_state_t touch_slots[TOUCH_MAX_SLOTS];
static uint32_t active_touch_count;
static int32_t last_x;
static int32_t last_y;
static lvgl_gpu_backend_t *gpu_backend;
static lv_display_flush_cb_t downstream_flush_cb;
static uint32_t consumed_sample_count;
static uint32_t displayed_frame_count;
static bool verbose_stats;
static touch_sample_t touch_queue[TOUCH_QUEUE_CAPACITY];
static pthread_mutex_t touch_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t touch_thread;
static bool touch_thread_started;
static uint32_t touch_queue_head;
static uint32_t touch_queue_tail;
static uint32_t touch_sample_count;
static uint32_t touch_sample_dropped;
static int touch_input_fd = -1;
static int touch_abs_min_x;
static int touch_abs_max_x = DISPLAY_WIDTH - 1;
static int touch_abs_min_y;
static int touch_abs_max_y = DISPLAY_HEIGHT - 1;
static int touch_supported_slots;
static lv_point_t latest_touch_point;
static lv_indev_state_t latest_touch_state = LV_INDEV_STATE_RELEASED;

static const uint32_t touch_slot_colors[TOUCH_MAX_SLOTS] = {
    0x38bdff, 0xf59e0b, 0x4ade80, 0xf472b6, 0xa78bfa,
    0x22d3ee, 0xf87171, 0xfacc15, 0x2dd4bf, 0xfb923c,
};

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static uint16_t scale_touch_axis(int value, int min, int max, int output_max)
{
    if (max <= min)
        return (uint16_t)LV_CLAMP(0, value, output_max);

    value = LV_CLAMP(min, value, max);
    return (uint16_t)((int64_t)(value - min) * output_max / (max - min));
}

static void queue_touch_sample(uint8_t slot, uint16_t x, uint16_t y,
                               bool pressed)
{
    uint32_t next;

    pthread_mutex_lock(&touch_queue_lock);
    next = (touch_queue_head + 1U) % TOUCH_QUEUE_CAPACITY;
    if (next == touch_queue_tail) {
        touch_queue_tail = (touch_queue_tail + 1U) % TOUCH_QUEUE_CAPACITY;
        touch_sample_dropped++;
    }

    touch_queue[touch_queue_head].x = x;
    touch_queue[touch_queue_head].y = y;
    touch_queue[touch_queue_head].slot = slot;
    touch_queue[touch_queue_head].pressed = pressed;
    touch_queue_head = next;
    touch_sample_count++;
    pthread_mutex_unlock(&touch_queue_lock);
}

static void *touch_reader(void *arg)
{
    struct input_event events[32];
    int raw_x[TOUCH_MAX_SLOTS] = { 0 };
    int raw_y[TOUCH_MAX_SLOTS] = { 0 };
    int tracking_id[TOUCH_MAX_SLOTS];
    int previous_x[TOUCH_MAX_SLOTS] = { 0 };
    int previous_y[TOUCH_MAX_SLOTS] = { 0 };
    bool previous_pressed[TOUCH_MAX_SLOTS] = { false };
    int current_slot = 0;

    for (uint32_t i = 0; i < TOUCH_MAX_SLOTS; i++)
        tracking_id[i] = -1;

    (void)arg;

    while (running) {
        struct pollfd pfd = {
            .fd = touch_input_fd,
            .events = POLLIN,
        };
        int ready = poll(&pfd, 1, 100);

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (!(pfd.revents & POLLIN))
            continue;

        for (;;) {
            ssize_t bytes = read(touch_input_fd, events, sizeof(events));
            size_t count;
            size_t i;

            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    break;
                running = 0;
                break;
            }
            if (bytes == 0)
                break;

            count = (size_t)bytes / sizeof(events[0]);
            for (i = 0; i < count; i++) {
                const struct input_event *event = &events[i];

                if (event->type == EV_ABS) {
                    if (event->code == ABS_MT_SLOT) {
                        current_slot = LV_CLAMP(0, event->value,
                                                TOUCH_MAX_SLOTS - 1);
                    }
                    else if (event->code == ABS_MT_POSITION_X ||
                             event->code == ABS_X) {
                        int slot = event->code == ABS_X ? 0 : current_slot;

                        raw_x[slot] = event->value;
                    }
                    else if (event->code == ABS_MT_POSITION_Y ||
                             event->code == ABS_Y) {
                        int slot = event->code == ABS_Y ? 0 : current_slot;

                        raw_y[slot] = event->value;
                    }
                    else if (event->code == ABS_MT_TRACKING_ID) {
                        tracking_id[current_slot] = event->value;
                    }
                }
                else if (event->type == EV_SYN &&
                         event->code == SYN_REPORT) {
                    for (uint32_t slot = 0; slot < TOUCH_MAX_SLOTS; slot++) {
                        bool pressed = tracking_id[slot] >= 0;
                        bool moved = raw_x[slot] != previous_x[slot] ||
                                     raw_y[slot] != previous_y[slot];

                        if (pressed != previous_pressed[slot] ||
                            (pressed && moved)) {
                            uint16_t x = scale_touch_axis(raw_x[slot],
                                                          touch_abs_min_x,
                                                          touch_abs_max_x,
                                                          DISPLAY_WIDTH - 1);
                            uint16_t y = scale_touch_axis(raw_y[slot],
                                                          touch_abs_min_y,
                                                          touch_abs_max_y,
                                                          DISPLAY_HEIGHT - 1);

                            queue_touch_sample((uint8_t)slot, x, y, pressed);
                            previous_x[slot] = raw_x[slot];
                            previous_y[slot] = raw_y[slot];
                            previous_pressed[slot] = pressed;
                        }
                    }
                }
            }
        }
    }

    return NULL;
}

static uint32_t drain_touch_samples(touch_sample_t *samples, uint32_t capacity)
{
    uint32_t count = 0;

    pthread_mutex_lock(&touch_queue_lock);
    while (touch_queue_tail != touch_queue_head && count < capacity) {
        samples[count++] = touch_queue[touch_queue_tail];
        touch_queue_tail = (touch_queue_tail + 1U) % TOUCH_QUEUE_CAPACITY;
    }
    pthread_mutex_unlock(&touch_queue_lock);
    return count;
}

static void buffered_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = latest_touch_point;
    data->state = latest_touch_state;
}

static void counted_flush_cb(lv_display_t *display, const lv_area_t *area,
                             uint8_t *color_p)
{
    bool last = lv_display_flush_is_last(display);

    downstream_flush_cb(display, area, color_p);
    if (last)
        displayed_frame_count++;
}

static void enable_display_stats(lv_display_t *display)
{
    downstream_flush_cb = lv_display_get_flush_cb(display);
    if (downstream_flush_cb != NULL)
        lv_display_set_flush_cb(display, counted_flush_cb);
}

static void set_touch_state(const char *state, uint32_t color)
{
    lv_label_set_text(state_label, state);
    lv_obj_set_style_text_color(state_label, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(touch_marker, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(color), 0);
}

static void update_touch_position(int32_t x, int32_t y)
{
    last_x = x;
    last_y = y;

    lv_label_set_text_fmt(coordinate_label, "X  %ld     Y  %ld",
                          (long)x, (long)y);
}

static void update_slot_markers(int primary)
{
    for (uint32_t slot = 0; slot < TOUCH_MAX_SLOTS; slot++) {
        lv_obj_t *marker = touch_slot_markers[slot];
        touch_slot_state_t *state = &touch_slots[slot];
        uint32_t color = touch_slot_colors[slot];

        if (marker == NULL)
            continue;

        if (!state->down || (int)slot == primary) {
            lv_obj_set_hidden(marker, true);
            continue;
        }

        lv_obj_set_style_border_color(marker, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(touch_slot_dots[slot],
                                  lv_color_hex(color), 0);
        lv_obj_set_pos(marker, state->x, state->y);
        lv_obj_set_hidden(marker, false);
    }
}

static void update_trail_chunk(trail_chunk_t *chunk)
{
    int32_t min_x = chunk->global_points[0].x;
    int32_t min_y = chunk->global_points[0].y;
    int32_t max_x = min_x;
    int32_t max_y = min_y;
    uint32_t i;

    for (i = 1; i < chunk->count; i++) {
        min_x = LV_MIN(min_x, (int32_t)chunk->global_points[i].x);
        min_y = LV_MIN(min_y, (int32_t)chunk->global_points[i].y);
        max_x = LV_MAX(max_x, (int32_t)chunk->global_points[i].x);
        max_y = LV_MAX(max_y, (int32_t)chunk->global_points[i].y);
    }

    min_x -= TRAIL_MARGIN;
    min_y -= TRAIL_MARGIN;
    max_x += TRAIL_MARGIN;
    max_y += TRAIL_MARGIN;

    for (i = 0; i < chunk->count; i++) {
        chunk->points[i].x = chunk->global_points[i].x - min_x;
        chunk->points[i].y = chunk->global_points[i].y - min_y;
    }

    lv_obj_set_pos(chunk->obj, min_x, min_y);
    lv_obj_set_size(chunk->obj, max_x - min_x + 1, max_y - min_y + 1);
    lv_line_set_points_mutable(chunk->obj, chunk->points, chunk->count);
    lv_obj_set_hidden(chunk->obj, false);
    chunk->dirty = false;
}

static trail_chunk_t *new_trail_chunk(uint8_t slot, int32_t x, int32_t y)
{
    touch_slot_state_t *state = &touch_slots[slot];
    trail_chunk_t *chunk;

    if (trail_chunk_count >= TRAIL_MAX_CHUNKS)
        return NULL;

    chunk = &trail_chunks[trail_chunk_count++];
    state->current_chunk = trail_chunk_count - 1;
    chunk->count = 0;
    chunk->slot = slot;

    if (state->has_last) {
        chunk->global_points[chunk->count].x = state->x;
        chunk->global_points[chunk->count].y = state->y;
        chunk->count++;
    }

    chunk->global_points[chunk->count].x = x;
    chunk->global_points[chunk->count].y = y;
    chunk->count++;
    state->x = x;
    state->y = y;
    state->has_last = true;
    lv_obj_set_style_line_color(chunk->obj,
                                lv_color_hex(touch_slot_colors[slot]), 0);
    chunk->dirty = true;
    return chunk;
}

static void append_trail_point(uint8_t slot, int32_t x, int32_t y)
{
    touch_slot_state_t *state = &touch_slots[slot];
    trail_chunk_t *chunk;
    int32_t dx;
    int32_t dy;

    if (state->has_last) {
        dx = x - state->x;
        dy = y - state->y;

        if (abs(dx) <= 1 && abs(dy) <= 1)
            return;
    }

    if (!state->has_last || state->current_chunk >= trail_chunk_count ||
        trail_chunks[state->current_chunk].count == TRAIL_CHUNK_POINTS) {
        new_trail_chunk(slot, x, y);
        return;
    }

    chunk = &trail_chunks[state->current_chunk];
    chunk->global_points[chunk->count].x = x;
    chunk->global_points[chunk->count].y = y;
    chunk->count++;
    state->x = x;
    state->y = y;
    chunk->dirty = true;
}

static void flush_trail_updates(void)
{
    uint32_t i;

    for (i = 0; i < trail_chunk_count; i++) {
        if (trail_chunks[i].dirty)
            update_trail_chunk(&trail_chunks[i]);
    }
}

static void reset_trail(void)
{
    uint32_t i;

    for (i = 0; i < trail_chunk_count; i++) {
        trail_chunks[i].count = 0;
        trail_chunks[i].dirty = false;
        lv_line_set_points_mutable(trail_chunks[i].obj, trail_chunks[i].points, 0);
        lv_obj_set_hidden(trail_chunks[i].obj, true);
    }

    trail_chunk_count = 0;
    for (i = 0; i < TOUCH_MAX_SLOTS; i++) {
        touch_slots[i].current_chunk = UINT32_MAX;
        touch_slots[i].has_last = false;
    }
}

static int primary_touch_slot(void)
{
    for (int i = 0; i < TOUCH_MAX_SLOTS; i++) {
        if (touch_slots[i].down)
            return i;
    }

    return -1;
}

static void process_touch_samples(void)
{
    touch_sample_t samples[TOUCH_DRAIN_BATCH];
    uint32_t count;
    bool changed = false;

    while ((count = drain_touch_samples(samples, TOUCH_DRAIN_BATCH)) != 0) {
        uint32_t i;

        for (i = 0; i < count; i++) {
            uint8_t slot = samples[i].slot;
            touch_slot_state_t *state;
            int32_t x = samples[i].x;
            int32_t y = samples[i].y;
            bool pressed = samples[i].pressed;

            if (slot >= TOUCH_MAX_SLOTS)
                continue;

            state = &touch_slots[slot];
            consumed_sample_count++;
            latest_touch_point.x = x;
            latest_touch_point.y = y;
            latest_touch_state = pressed ? LV_INDEV_STATE_PRESSED :
                                           LV_INDEV_STATE_RELEASED;
            changed = true;

            if (pressed) {
                if (!state->down) {
                    if (active_touch_count == 0)
                        reset_trail();

                    state->down = true;
                    state->dragging = false;
                    state->press_x = x;
                    state->press_y = y;
                    state->sample_count = 1;
                    state->start_tick = lv_tick_get();
                    state->current_chunk = UINT32_MAX;
                    state->has_last = false;
                    active_touch_count++;
                    append_trail_point(slot, x, y);
                    printf("touch-demo: slot %u pressed %ld,%ld fingers=%lu\n",
                           slot, (long)x, (long)y,
                           (unsigned long)active_touch_count);
                    fflush(stdout);
                }
                else {
                    state->sample_count++;
                    append_trail_point(slot, x, y);
                    if (!state->dragging &&
                        (abs(x - state->press_x) > 2 ||
                         abs(y - state->press_y) > 2)) {
                        state->dragging = true;
                        printf("touch-demo: slot %u drag started\n", slot);
                        fflush(stdout);
                    }
                }
            }
            else if (state->down) {
                state->sample_count++;
                append_trail_point(slot, x, y);
                state->down = false;
                state->dragging = false;
                state->current_chunk = UINT32_MAX;
                state->has_last = false;
                if (active_touch_count > 0)
                    active_touch_count--;
                printf("touch-demo: slot %u released %ld,%ld "
                       "samples=%lu duration=%lums fingers=%lu\n",
                       slot, (long)x, (long)y,
                       (unsigned long)state->sample_count,
                       (unsigned long)lv_tick_elaps(state->start_tick),
                       (unsigned long)active_touch_count);
                fflush(stdout);
            }

            last_x = x;
            last_y = y;
        }
    }

    if (changed) {
        int primary = primary_touch_slot();

        flush_trail_updates();
        if (primary >= 0) {
            touch_slot_state_t *state = &touch_slots[primary];
            uint32_t color = touch_slot_colors[primary];

            latest_touch_point.x = state->x;
            latest_touch_point.y = state->y;
            latest_touch_state = LV_INDEV_STATE_PRESSED;
            update_touch_position(state->x, state->y);
            lv_obj_set_hidden(touch_marker, false);
            if (active_touch_count > 1) {
                lv_label_set_text_fmt(state_label, "%lu POINTS",
                                      (unsigned long)active_touch_count);
                lv_obj_set_style_text_color(state_label,
                                            lv_color_hex(color), 0);
            }
            else if (state->dragging) {
                set_touch_state("DRAGGING", color);
            }
            else {
                set_touch_state("PRESSED", color);
            }
            lv_obj_set_style_border_color(touch_marker, lv_color_hex(color), 0);
            lv_obj_set_style_bg_color(touch_dot, lv_color_hex(color), 0);
        }
        else {
            latest_touch_state = LV_INDEV_STATE_RELEASED;
            set_touch_state("RELEASED", 0x8b98a5);
            lv_obj_set_hidden(touch_marker, true);
        }
        update_slot_markers(primary);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void build_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *header;
    lv_obj_t *panel;
    lv_obj_t *label;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x081018), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    header = lv_obj_create(screen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 800, 72);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111b27), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollable(header, false);

    label = make_label(header, "TOUCH INPUT", &lv_font_montserrat_24, 0xe6edf3);
    lv_obj_set_pos(label, 24, 20);

    label = make_label(header, "N32H787  /  800 x 480", &lv_font_montserrat_14,
                       0x8b98a5);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -24, 0);

    panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 24, 96);
    lv_obj_set_size(panel, 752, 170);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111b27), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x263548), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_scrollable(panel, false);

    label = make_label(panel, "POSITION", &lv_font_montserrat_14, 0x8b98a5);
    lv_obj_set_pos(label, 24, 18);

    state_label = make_label(panel, "RELEASED", &lv_font_montserrat_14,
                             0x8b98a5);
    lv_obj_align(state_label, LV_ALIGN_TOP_RIGHT, -24, 18);

    coordinate_label = make_label(panel, "X  --     Y  --",
                                  &lv_font_montserrat_26, 0x38bdff);
    lv_obj_set_pos(coordinate_label, 24, 56);

    label = make_label(panel, "Draw with one or more fingers",
                       &lv_font_montserrat_14,
                       0x536273);
    lv_obj_set_pos(label, 24, 120);

    label = make_label(screen, "LAST TOUCH", &lv_font_montserrat_14, 0x8b98a5);
    lv_obj_set_pos(label, 24, 292);

    for (uint32_t i = 0; i < TRAIL_MAX_CHUNKS; i++) {
        lv_obj_t *chunk = lv_line_create(screen);

        trail_chunks[i].obj = chunk;
        trail_chunks[i].count = 0;
        trail_chunks[i].dirty = false;
        lv_obj_set_pos(chunk, 0, 0);
        lv_obj_set_size(chunk, 1, 1);
        lv_obj_set_style_bg_opa(chunk, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chunk, 0, 0);
        lv_obj_set_style_pad_all(chunk, 0, 0);
        lv_obj_set_style_line_width(chunk, TRAIL_LINE_WIDTH, 0);
        lv_obj_set_style_line_color(chunk, lv_color_hex(0x38bdff), 0);
        lv_obj_set_style_line_rounded(chunk, true, 0);
        lv_obj_set_scrollable(chunk, false);
        lv_obj_set_clickable(chunk, false);
        lv_obj_set_hidden(chunk, true);
    }

    touch_marker = lv_obj_create(screen);
    lv_obj_set_size(touch_marker, 48, 48);
    lv_obj_set_style_translate_x(touch_marker, -24, 0);
    lv_obj_set_style_translate_y(touch_marker, -24, 0);
    lv_obj_set_style_radius(touch_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(touch_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(touch_marker, lv_color_hex(0x536273), 0);
    lv_obj_set_style_border_width(touch_marker, 3, 0);
    lv_obj_set_style_pad_all(touch_marker, 0, 0);
    lv_obj_set_scrollable(touch_marker, false);
    lv_obj_set_clickable(touch_marker, false);
    lv_obj_set_ignore_layout(touch_marker, true);
    lv_obj_set_floating(touch_marker, true);
    lv_obj_set_hidden(touch_marker, true);
    lv_obj_move_foreground(touch_marker);

    touch_dot = lv_obj_create(touch_marker);
    lv_obj_set_size(touch_dot, 10, 10);
    lv_obj_center(touch_dot);
    lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(0x536273), 0);
    lv_obj_set_style_bg_opa(touch_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(touch_dot, 0, 0);
    lv_obj_set_style_pad_all(touch_dot, 0, 0);
    lv_obj_set_scrollable(touch_dot, false);
    lv_obj_set_clickable(touch_dot, false);

    lv_obj_t *system_layer = lv_display_get_layer_sys(lv_obj_get_display(screen));

    for (uint32_t slot = 0; slot < TOUCH_MAX_SLOTS; slot++) {
        lv_obj_t *marker = lv_obj_create(system_layer);
        lv_obj_t *dot;

        touch_slot_markers[slot] = marker;
        lv_obj_set_size(marker, 44, 44);
        lv_obj_set_style_translate_x(marker, -22, 0);
        lv_obj_set_style_translate_y(marker, -22, 0);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(marker, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(marker,
                                      lv_color_hex(touch_slot_colors[slot]), 0);
        lv_obj_set_style_border_width(marker, 3, 0);
        lv_obj_set_style_pad_all(marker, 0, 0);
        lv_obj_set_scrollable(marker, false);
        lv_obj_set_clickable(marker, false);
        lv_obj_set_ignore_layout(marker, true);
        lv_obj_set_floating(marker, true);
        lv_obj_set_hidden(marker, true);

        dot = lv_obj_create(marker);
        touch_slot_dots[slot] = dot;
        lv_obj_set_size(dot, 8, 8);
        lv_obj_center(dot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(touch_slot_colors[slot]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_scrollable(dot, false);
        lv_obj_set_clickable(dot, false);
    }
}

static void print_usage(const char *program)
{
    printf("usage: %s [-d /dev/fb0|/dev/dri/cardN]"
           " [-g auto|none|/dev/dri/cardN]"
           " [-i /dev/input/eventN|none] [-r touch-ms] [-t seconds]\n",
           program);
}

static int is_drm_path(const char *device)
{
    return strncmp(device, "/dev/dri/", 9) == 0;
}

int main(int argc, char **argv)
{
    const char *requested_device = NULL;
    const char *requested_gpu = "auto";
    const char *display_device;
    const char *input_device = "/dev/input/event0";
    char found_lcdc[64] = "";
    char found_gpu[64] = "";
    int period_ms = 5;
    int seconds = 0;
    int opt;
    int exit_status = 0;
    uint32_t start;
    lv_display_t *display;
    lv_indev_t *indev = NULL;

    while ((opt = getopt(argc, argv, "d:g:i:r:t:hv")) != -1) {
        switch (opt) {
        case 'd':
            requested_device = optarg;
            break;
        case 'g':
            requested_gpu = optarg;
            break;
        case 'i':
            input_device = optarg;
            break;
        case 'r':
            period_ms = atoi(optarg);
            break;
        case 't':
            seconds = atoi(optarg);
            break;
        case 'v':
            verbose_stats = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (period_ms < 1 || period_ms > 100) {
        fprintf(stderr, "touch-demo: touch period must be 1..100 ms\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    lv_init();

    display_device = requested_device ? requested_device : "/dev/fb0";
    if (strcmp(requested_gpu, "none") &&
        (!requested_device || is_drm_path(requested_device))) {
        if (!lvgl_gpu_discover(found_lcdc, sizeof(found_lcdc),
                               found_gpu, sizeof(found_gpu))) {
            const char *lcdc = requested_device ? requested_device : found_lcdc;
            const char *gpu = strcmp(requested_gpu, "auto") ?
                              requested_gpu : found_gpu;

            gpu_backend = lvgl_gpu_display_create(&display, lcdc, gpu);
            if (!gpu_backend)
                fprintf(stderr,
                        "touch-demo: GPU unavailable; falling back to KMS\n");
        }
        if (!requested_device && found_lcdc[0])
            display_device = found_lcdc;
    }

    if (gpu_backend) {
        /* The GPU backend owns the display. */
    }
    else if (is_drm_path(display_device)) {
        display = lv_linux_drm_create();
        if (display == NULL || lv_linux_drm_set_file(display, display_device, -1) != LV_RESULT_OK) {
            fprintf(stderr, "touch-demo: cannot initialize %s\n", display_device);
            return 1;
        }
    }
    else {
        display = lv_linux_fbdev_create();
        if (display == NULL || lv_linux_fbdev_set_file(display, display_device) != LV_RESULT_OK) {
            fprintf(stderr, "touch-demo: cannot initialize %s\n", display_device);
            return 1;
        }
    }

    if (strcmp(input_device, "none") != 0) {
        struct input_absinfo absinfo;
        pthread_attr_t attr;
        bool attr_initialized = false;
        bool have_attr = false;
        int flags;

        touch_input_fd = open(input_device, O_RDONLY | O_NOCTTY | O_CLOEXEC);
        if (touch_input_fd < 0) {
            fprintf(stderr, "touch-demo: cannot open %s\n", input_device);
            return 1;
        }

        if (ioctl(touch_input_fd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) == 0) {
            touch_abs_min_x = absinfo.minimum;
            touch_abs_max_x = absinfo.maximum;
        }
        else if (ioctl(touch_input_fd, EVIOCGABS(ABS_X), &absinfo) == 0) {
            touch_abs_min_x = absinfo.minimum;
            touch_abs_max_x = absinfo.maximum;
        }
        if (ioctl(touch_input_fd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) == 0) {
            touch_abs_min_y = absinfo.minimum;
            touch_abs_max_y = absinfo.maximum;
        }
        else if (ioctl(touch_input_fd, EVIOCGABS(ABS_Y), &absinfo) == 0) {
            touch_abs_min_y = absinfo.minimum;
            touch_abs_max_y = absinfo.maximum;
        }
        if (ioctl(touch_input_fd, EVIOCGABS(ABS_MT_SLOT), &absinfo) == 0)
            touch_supported_slots = LV_CLAMP(1, absinfo.maximum + 1,
                                             TOUCH_MAX_SLOTS);
        else
            touch_supported_slots = 1;

        flags = fcntl(touch_input_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(touch_input_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            fprintf(stderr, "touch-demo: cannot make %s nonblocking: %s\n",
                    input_device, strerror(errno));
            close(touch_input_fd);
            touch_input_fd = -1;
            return 1;
        }

        if (pthread_attr_init(&attr) == 0) {
            attr_initialized = true;
            if (pthread_attr_setstacksize(&attr, TOUCH_THREAD_STACK) == 0)
                have_attr = true;
        }
        if (pthread_create(&touch_thread, have_attr ? &attr : NULL,
                           touch_reader, NULL) != 0) {
            fprintf(stderr, "touch-demo: cannot start input reader\n");
            if (attr_initialized)
                pthread_attr_destroy(&attr);
            close(touch_input_fd);
            touch_input_fd = -1;
            return 1;
        }
        if (attr_initialized)
            pthread_attr_destroy(&attr);
        touch_thread_started = true;

        indev = lv_indev_create();
        if (indev == NULL) {
            fprintf(stderr, "touch-demo: cannot create LVGL input device\n");
            running = 0;
            pthread_join(touch_thread, NULL);
            touch_thread_started = false;
            close(touch_input_fd);
            touch_input_fd = -1;
            return 1;
        }
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, buffered_touch_read_cb);
        lv_indev_set_display(indev, display);
        lv_timer_set_period(lv_indev_get_read_timer(indev), period_ms);
    }
    lv_timer_set_period(lv_display_get_refr_timer(display), 16);
    enable_display_stats(display);

    build_ui();
    if (indev != NULL)
        lv_indev_set_cursor(indev, touch_marker);
    start = lv_tick_get();
    printf("touch-demo: display=%s gpu=%s input=%s touch=%dms display=16ms "
           "slots=%d\n",
           gpu_backend ? lvgl_gpu_lcdc_device(gpu_backend) : display_device,
           gpu_backend ? lvgl_gpu_gpu_device(gpu_backend) : "software",
           input_device, period_ms, touch_supported_slots);
    fflush(stdout);

    uint32_t stats_tick = lv_tick_get();
    uint32_t last_sample_count = 0;
    uint32_t last_frame_count = 0;

    while (running) {
        process_touch_samples();
        lv_timer_handler();
        if (lvgl_gpu_error(gpu_backend)) {
            exit_status = 1;
            break;
        }
        usleep(1000);

        if (verbose_stats && lv_tick_elaps(stats_tick) >= 1000) {
            uint32_t elapsed = lv_tick_elaps(stats_tick);
            uint32_t captured;
            uint32_t dropped;
            uint32_t pending;
            uint32_t frames = displayed_frame_count - last_frame_count;

            pthread_mutex_lock(&touch_queue_lock);
            captured = touch_sample_count;
            dropped = touch_sample_dropped;
            pending = (touch_queue_head + TOUCH_QUEUE_CAPACITY -
                       touch_queue_tail) % TOUCH_QUEUE_CAPACITY;
            pthread_mutex_unlock(&touch_queue_lock);

            printf("touch-demo: capture=%.1fHz consume=%.1fHz "
                   "display=%.1fHz pending=%lu dropped=%lu\n",
                   (captured - last_sample_count) * 1000.0 / elapsed,
                   consumed_sample_count * 1000.0 / elapsed,
                   frames * 1000.0 / elapsed,
                   (unsigned long)pending, (unsigned long)dropped);
            fflush(stdout);
            consumed_sample_count = 0;
            last_sample_count = captured;
            last_frame_count = displayed_frame_count;
            stats_tick = lv_tick_get();
        }
        if (seconds > 0 && (uint32_t)(lv_tick_get() - start) >=
                           (uint32_t)seconds * 1000U)
            break;
    }

    running = 0;
    if (touch_thread_started) {
        pthread_join(touch_thread, NULL);
        touch_thread_started = false;
    }
    if (touch_input_fd >= 0) {
        close(touch_input_fd);
        touch_input_fd = -1;
    }
    printf("touch-demo: last position %ld,%ld\n", (long)last_x, (long)last_y);
    if (gpu_backend)
        lvgl_gpu_display_destroy(gpu_backend);
    return exit_status;
}
