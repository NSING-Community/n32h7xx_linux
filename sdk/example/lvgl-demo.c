/* SPDX-License-Identifier: MIT */
/*
 * LVGL v9.6.0 framebuffer smoke test for the N32H787 board.
 *
 * Creates a small animated UI on /dev/fb0 through LVGL's Linux fbdev driver
 * (partial refresh, RGB565) and prints the measured render and flush rates
 * once a second.  It is a self-contained check that the library, the display
 * backend and the board's LCDC path work together; it is not a benchmark.
 *
 * Usage: lvgl-demo [-d /dev/fb0|/dev/dri/cardN] [-g auto|none|/dev/dri/cardN]
 *                  [-i /dev/input/eventN|none]
 *                  [-t seconds] [-r refresh_ms] [-s]
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "lvgl-gpu-backend.h"

static volatile sig_atomic_t running = 1;

static lv_obj_t *fps_label;
static lv_obj_t *chart;
static lv_chart_series_t *chart_series;

static uint32_t render_count;
static uint32_t flush_count;
static uint32_t window_start;
static int stress_mode;
static lvgl_gpu_backend_t *gpu_backend;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static void display_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_READY:
        render_count++;
        break;
    case LV_EVENT_FLUSH_FINISH:
        flush_count++;
        break;
    default:
        break;
    }
}

static void stats_timer_cb(lv_timer_t *timer)
{
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - window_start;
    uint32_t render_x10;
    uint32_t flush_x10;

    if (elapsed < 1000)
        return;

    render_x10 = (render_count * 10000U + elapsed / 2U) / elapsed;
    flush_x10 = (flush_count * 10000U + elapsed / 2U) / elapsed;

    lv_label_set_text_fmt(fps_label,
                          "%u.%u fps  %u.%u flushes/s  GPU %u/%u  SW big %u",
                          render_x10 / 10U, render_x10 % 10U,
                          flush_x10 / 10U, flush_x10 % 10U,
                          gpu_backend ? lvgl_gpu_fill_count(gpu_backend) +
                                        lvgl_gpu_polygon_count(gpu_backend) : 0U,
                          gpu_backend ? lvgl_gpu_present_count(gpu_backend) : 0U,
                          gpu_backend ?
                          lvgl_gpu_large_fill_fallback_count(gpu_backend) : 0U);
    printf("lvgl: %u.%u render fps, %u.%u flushes/s, "
           "GPU fill/border/line/arc/tri/present/jobs/large-SW "
           "%u/%u/%u/%u/%u/%u/%u+%u/%u, %u ms uptime\n",
           render_x10 / 10U, render_x10 % 10U,
           flush_x10 / 10U, flush_x10 % 10U,
           gpu_backend ? lvgl_gpu_fill_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_border_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_line_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_arc_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_triangle_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_present_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_batch_count(gpu_backend) : 0U,
           gpu_backend ? lvgl_gpu_poly_batch_count(gpu_backend) : 0U,
           gpu_backend ?
           lvgl_gpu_large_fill_fallback_count(gpu_backend) : 0U, now);
    fflush(stdout);

    render_count = 0;
    flush_count = 0;
    window_start = now;

    (void)timer;
}

static void bar_anim_cb(void *var, int32_t value)
{
    lv_bar_set_value((lv_obj_t *)var, value, LV_ANIM_OFF);
}

static void chart_timer_cb(lv_timer_t *timer)
{
    static int32_t phase;

    /* Triangle wave, 0..100..0: no libm and no floating point in the loop. */
    phase = (phase + 4) % 200;
    lv_chart_set_next_value(chart, chart_series,
                            phase < 100 ? phase : 200 - phase);

    (void)timer;
}

/* Force a full-screen repaint on every tick so the counters show the cost of
 * drawing and flushing the whole panel instead of a few small widgets. */
static void stress_timer_cb(lv_timer_t *timer)
{
    lv_obj_invalidate(lv_screen_active());

    (void)timer;
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y,
                           int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x151b23), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2a3542), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_scrollable(card, false);

    return card;
}

static void build_ui(const char *backend_desc)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *card;
    lv_obj_t *arc;
    lv_obj_t *bar;
    lv_obj_t *label;
    lv_anim_t anim;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b0f14), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    title = lv_label_create(screen);
    lv_label_set_text(title, "LVGL 9.6.0 on N32H787");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_set_pos(title, 24, 16);

    subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, backend_desc);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8b98a5), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(subtitle, 26, 54);

    /* Left card: an arc and a bar driven by lv_anim. */
    card = make_card(screen, 24, 92, 300, 300);

    arc = lv_arc_create(card);
    lv_obj_set_size(arc, 190, 190);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x243040), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x2f81f7), LV_PART_INDICATOR);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);

    label = lv_label_create(card);
    lv_label_set_text(label, "CPU render");
    lv_obj_set_style_text_color(label, lv_color_hex(0x8b98a5), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 6);

    bar = lv_bar_create(card);
    lv_obj_set_size(bar, 240, 12);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x243040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3fb950), LV_PART_INDICATOR);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, arc);
    lv_anim_set_values(&anim, 0, 100);
    lv_anim_set_duration(&anim, 1800);
    lv_anim_set_playback_duration(&anim, 1800);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_arc_set_value);
    lv_anim_start(&anim);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, bar);
    lv_anim_set_values(&anim, 0, 100);
    lv_anim_set_duration(&anim, 2600);
    lv_anim_set_playback_duration(&anim, 600);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, bar_anim_cb);
    lv_anim_start(&anim);

    /* Right card: a live chart, updated by an lv_timer. */
    card = make_card(screen, 344, 92, 432, 300);

    label = lv_label_create(card);
    lv_label_set_text(label, "chart / lv_timer 100 ms");
    lv_obj_set_style_text_color(label, lv_color_hex(0x8b98a5), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    chart = lv_chart_create(card);
    lv_obj_set_size(chart, 400, 226);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 50);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x0f151c), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    chart_series = lv_chart_add_series(chart, lv_color_hex(0x2f81f7),
                                       LV_CHART_AXIS_PRIMARY_Y);
    lv_timer_create(chart_timer_cb, 100, NULL);

    /* Bottom status line. */
    fps_label = lv_label_create(screen);
    lv_label_set_text(fps_label, "measuring...");
    lv_obj_set_style_text_color(fps_label, lv_color_hex(0x3fb950), 0);
    lv_obj_set_style_text_font(fps_label, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(fps_label, 26, 410);
}

static int is_drm_path(const char *device)
{
    return strncmp(device, "/dev/dri/", 9) == 0;
}

static void print_memory(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long size = 0;
    unsigned long resident = 0;
    long page = sysconf(_SC_PAGESIZE);

    if (f == NULL)
        return;

    /* NOMMU /proc/self/status has no VmRSS/VmHWM fields; statm does. */
    if (fscanf(f, "%lu %lu", &size, &resident) == 2 && page > 0)
        printf("lvgl: memory %lu KiB, resident %lu KiB\n",
               size * (unsigned long)page / 1024,
               resident * (unsigned long)page / 1024);
    fclose(f);
}

static void print_cpu(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    unsigned long utime = 0;
    unsigned long stime = 0;
    long hz = sysconf(_SC_CLK_TCK);

    if (f == NULL)
        return;

    /* Skip pid..cmajflt; the process name has no spaces or parentheses. */
    if (fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &utime, &stime) == 2 && hz > 0)
        printf("lvgl: cpu user %lu ms, system %lu ms\n",
               utime * 1000UL / (unsigned long)hz,
               stime * 1000UL / (unsigned long)hz);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *requested_device = NULL;
    const char *requested_gpu = "auto";
    const char *device;
    const char *input_device = "/dev/input/event0";
    char found_lcdc[64] = "";
    char found_gpu[64] = "";
    int seconds = 15;
    int refresh_ms = 0;
    char backend_desc[160];
    lv_display_t *disp;
    lv_indev_t *indev;
    lv_timer_t *refr_timer;
    int opt;
    uint32_t start;

    while ((opt = getopt(argc, argv, "d:g:i:t:r:sh")) != -1) {
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
        case 't':
            seconds = atoi(optarg);
            break;
        case 'r':
            refresh_ms = atoi(optarg);
            break;
        case 's':
            stress_mode = 1;
            break;
        case 'h':
        default:
            printf("usage: %s [-d /dev/fb0|/dev/dri/cardN]"
                   " [-g auto|none|/dev/dri/cardN]"
                   " [-i /dev/input/eventN|none]"
                   " [-t seconds] [-r refresh_ms] [-s]\n",
                   argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    lv_init();

    device = requested_device ? requested_device : "/dev/fb0";
    if (strcmp(requested_gpu, "none") &&
        (!requested_device || is_drm_path(requested_device))) {
        if (!lvgl_gpu_discover(found_lcdc, sizeof(found_lcdc),
                               found_gpu, sizeof(found_gpu))) {
            const char *lcdc = requested_device ? requested_device : found_lcdc;
            const char *gpu = strcmp(requested_gpu, "auto") ?
                              requested_gpu : found_gpu;

            gpu_backend = lvgl_gpu_display_create(&disp, lcdc, gpu);
            if (!gpu_backend) {
                fprintf(stderr,
                        "lvgl: GPU backend unavailable; falling back to KMS\n");
            }
        }
        if (!requested_device && found_lcdc[0])
            device = found_lcdc;
    }

    if (gpu_backend) {
        /* The GPU backend owns the display. */
    }
    else if (is_drm_path(device)) {
        disp = lv_linux_drm_create();
        if (disp == NULL) {
            fprintf(stderr, "lvgl: cannot create the DRM display\n");
            return 1;
        }
        if (lv_linux_drm_set_file(disp, device, -1) != LV_RESULT_OK) {
            fprintf(stderr,
                    "lvgl: cannot initialize the DRM display on %s"
                    " (KMS setup or dumb-buffer allocation failed)\n", device);
            return 1;
        }
    }
    else {
        disp = lv_linux_fbdev_create();
        if (disp == NULL) {
            fprintf(stderr, "lvgl: cannot create the fbdev display\n");
            return 1;
        }
        if (lv_linux_fbdev_set_file(disp, device) != LV_RESULT_OK) {
            fprintf(stderr, "lvgl: cannot open %s\n", device);
            return 1;
        }
    }

    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_FLUSH_FINISH, NULL);

    refr_timer = lv_display_get_refr_timer(disp);
    if (refresh_ms > 0 && refr_timer != NULL)
        lv_timer_set_period(refr_timer, (uint32_t)refresh_ms);

    indev = NULL;
    if (input_device != NULL && strcmp(input_device, "none") != 0) {
        indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_device);
        if (indev == NULL) {
            fprintf(stderr,
                    "lvgl: touchscreen %s is unavailable; display continues\n",
                    input_device);
        }
        else {
            lv_indev_set_display(indev, disp);
        }
    }

    snprintf(backend_desc, sizeof(backend_desc), "%s  %dx%d  RGB565  %s",
             gpu_backend ? "GPU GEM + draw unit" :
             (is_drm_path(device) ? "DRM dumb buffers" : "fbdev"),
             (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp),
             gpu_backend ? "CPU fallback for unsupported tasks" :
             (is_drm_path(device) ? "direct mode / two buffers"
                                  : "partial refresh"));
    build_ui(backend_desc);
    lv_timer_create(stats_timer_cb, 500, NULL);
    if (stress_mode)
        lv_timer_create(stress_timer_cb, 20, NULL);

    /* LVGL's DRM driver sets the tick source to the absolute kernel clock;
     * start the statistics window here rather than assuming a zero origin. */
    window_start = lv_tick_get();

    printf("lvgl: %s + %s, %dx%d, %d s\n",
           gpu_backend ? lvgl_gpu_lcdc_device(gpu_backend) : device,
           gpu_backend ? lvgl_gpu_gpu_device(gpu_backend) : "software",
           (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp), seconds);
    if (gpu_backend)
        printf("lvgl: GPU plain-fill limit %u..%u pixels\n",
               16384U, lvgl_gpu_max_fill_pixels(gpu_backend));
    if (indev != NULL)
        printf("lvgl: touchscreen pointer %s\n", input_device);
    if (stress_mode)
        printf("lvgl: full-screen stress mode\n");

    start = lv_tick_get();

    while (running) {
        lv_timer_handler();
        usleep(5000);
        if (seconds > 0 && (uint32_t)(lv_tick_get() - start) >= (uint32_t)seconds * 1000U)
            break;
    }

    print_memory();
    print_cpu();
    printf("lvgl: stopped after %u ms\n", lv_tick_get() - start);
    if (gpu_backend)
        lvgl_gpu_display_destroy(gpu_backend);

    return 0;
}
