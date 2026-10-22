/* SPDX-License-Identifier: MIT */
/*
 * LVGL touchscreen coordinate demo for the N32H787 HMI panel.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "lvgl-gpu-backend.h"

#define TRAIL_CHUNK_POINTS 12
#define TRAIL_MAX_CHUNKS 96
#define TRAIL_LINE_WIDTH 4
#define TRAIL_MARGIN 4

typedef struct {
    lv_obj_t *obj;
    lv_point_precise_t points[TRAIL_CHUNK_POINTS];
    lv_point_precise_t global_points[TRAIL_CHUNK_POINTS];
    uint32_t count;
} trail_chunk_t;

static volatile sig_atomic_t running = 1;
static lv_obj_t *coordinate_label;
static lv_obj_t *state_label;
static lv_obj_t *touch_marker;
static lv_obj_t *touch_dot;
static trail_chunk_t trail_chunks[TRAIL_MAX_CHUNKS];
static uint32_t trail_chunk_count;
static uint32_t trail_current_chunk;
static int32_t trail_last_x;
static int32_t trail_last_y;
static bool trail_has_last;
static int32_t last_x;
static int32_t last_y;
static int32_t press_x;
static int32_t press_y;
static bool touch_down;
static bool dragging;
static lvgl_gpu_backend_t *gpu_backend;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
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
    lv_obj_set_pos(touch_marker, x - 24, y - 24);
    lv_obj_set_hidden(touch_marker, false);
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
}

static trail_chunk_t *new_trail_chunk(int32_t x, int32_t y)
{
    trail_chunk_t *chunk;

    if (trail_chunk_count >= TRAIL_MAX_CHUNKS)
        return NULL;

    chunk = &trail_chunks[trail_chunk_count++];
    trail_current_chunk = trail_chunk_count - 1;
    chunk->count = 0;

    if (trail_has_last) {
        chunk->global_points[chunk->count].x = trail_last_x;
        chunk->global_points[chunk->count].y = trail_last_y;
        chunk->count++;
    }

    chunk->global_points[chunk->count].x = x;
    chunk->global_points[chunk->count].y = y;
    chunk->count++;
    trail_last_x = x;
    trail_last_y = y;
    trail_has_last = true;
    update_trail_chunk(chunk);
    return chunk;
}

static void append_trail_point(int32_t x, int32_t y)
{
    trail_chunk_t *chunk;
    int32_t dx;
    int32_t dy;

    if (trail_has_last) {
        dx = x - trail_last_x;
        dy = y - trail_last_y;

        if (abs(dx) <= 1 && abs(dy) <= 1)
            return;
    }

    chunk = &trail_chunks[trail_current_chunk];
    if (!trail_has_last || chunk->count == TRAIL_CHUNK_POINTS) {
        new_trail_chunk(x, y);
        return;
    }

    chunk->global_points[chunk->count].x = x;
    chunk->global_points[chunk->count].y = y;
    chunk->count++;
    trail_last_x = x;
    trail_last_y = y;
    update_trail_chunk(chunk);
}

static void reset_trail(void)
{
    uint32_t i;

    for (i = 0; i < trail_chunk_count; i++) {
        trail_chunks[i].count = 0;
        lv_line_set_points_mutable(trail_chunks[i].obj, trail_chunks[i].points, 0);
        lv_obj_set_hidden(trail_chunks[i].obj, true);
    }

    trail_chunk_count = 0;
    trail_current_chunk = 0;
    trail_has_last = false;
}

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;

    if (indev == NULL)
        return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &point);
        touch_down = true;
        dragging = false;
        press_x = point.x;
        press_y = point.y;
        reset_trail();
        append_trail_point(point.x, point.y);
        update_touch_position(point.x, point.y);
        set_touch_state("PRESSED", 0x4ade80);
        printf("touch-demo: pressed %ld,%ld\n",
               (long)point.x, (long)point.y);
        fflush(stdout);
    }
    else if (code == LV_EVENT_PRESSING && touch_down) {
        lv_indev_get_point(indev, &point);
        append_trail_point(point.x, point.y);
        update_touch_position(point.x, point.y);
        if (!dragging && (abs(point.x - press_x) > 2 ||
                          abs(point.y - press_y) > 2)) {
            dragging = true;
            set_touch_state("DRAGGING", 0xf59e0b);
            printf("touch-demo: drag started\n");
            fflush(stdout);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_indev_get_point(indev, &point);
        append_trail_point(point.x, point.y);
        touch_down = false;
        dragging = false;
        update_touch_position(point.x, point.y);
        set_touch_state("RELEASED", 0x8b98a5);
        printf("touch-demo: released %ld,%ld\n",
               (long)point.x, (long)point.y);
        fflush(stdout);
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
    lv_obj_t *touch_surface;
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

    label = make_label(panel, "Drag anywhere to draw", &lv_font_montserrat_14,
                       0x536273);
    lv_obj_set_pos(label, 24, 120);

    label = make_label(screen, "LAST TOUCH", &lv_font_montserrat_14, 0x8b98a5);
    lv_obj_set_pos(label, 24, 292);

    /* A transparent input surface captures the complete gesture, including
     * motion outside the labels and panels. */
    touch_surface = lv_obj_create(screen);
    lv_obj_set_pos(touch_surface, 0, 0);
    lv_obj_set_size(touch_surface, 800, 480);
    lv_obj_set_style_bg_opa(touch_surface, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(touch_surface, 0, 0);
    lv_obj_set_style_radius(touch_surface, 0, 0);
    lv_obj_set_style_pad_all(touch_surface, 0, 0);
    lv_obj_set_scrollable(touch_surface, false);
    lv_obj_set_clickable(touch_surface, true);
    lv_obj_add_event_cb(touch_surface, touch_event_cb, LV_EVENT_ALL, NULL);

    for (uint32_t i = 0; i < TRAIL_MAX_CHUNKS; i++) {
        lv_obj_t *chunk = lv_line_create(screen);

        trail_chunks[i].obj = chunk;
        trail_chunks[i].count = 0;
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
    lv_obj_set_style_radius(touch_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(touch_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(touch_marker, lv_color_hex(0x536273), 0);
    lv_obj_set_style_border_width(touch_marker, 3, 0);
    lv_obj_set_style_pad_all(touch_marker, 0, 0);
    lv_obj_set_scrollable(touch_marker, false);
    lv_obj_set_clickable(touch_marker, false);
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
    uint32_t start;
    lv_display_t *display;
    lv_indev_t *indev = NULL;

    while ((opt = getopt(argc, argv, "d:g:i:r:t:h")) != -1) {
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
        indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_device);
        if (indev == NULL) {
            fprintf(stderr, "touch-demo: cannot open %s\n", input_device);
            return 1;
        }
        lv_indev_set_display(indev, display);
        lv_timer_set_period(lv_indev_get_read_timer(indev), period_ms);
    }
    lv_timer_set_period(lv_display_get_refr_timer(display), 16);

    build_ui();
    start = lv_tick_get();
    printf("touch-demo: display=%s gpu=%s input=%s touch=%dms display=16ms\n",
           gpu_backend ? lvgl_gpu_lcdc_device(gpu_backend) : display_device,
           gpu_backend ? lvgl_gpu_gpu_device(gpu_backend) : "software",
           input_device, period_ms);
    fflush(stdout);

    while (running) {
        lv_timer_handler();
        usleep(1000);
        if (seconds > 0 && (uint32_t)(lv_tick_get() - start) >=
                           (uint32_t)seconds * 1000U)
            break;
    }

    printf("touch-demo: last position %ld,%ld\n", (long)last_x, (long)last_y);
    if (gpu_backend)
        lvgl_gpu_display_destroy(gpu_backend);
    return 0;
}
