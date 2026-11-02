// SPDX-License-Identifier: MIT
/*
 * LVGL display and draw-unit backend for the N32H787 D/AVE HD GPU.
 *
 * LVGL uses two GPU-accessible full-screen buffers in DIRECT mode.  The GPU draws
 * supported fills into the current buffer, then the buffer is imported into
 * LCDC through PRIME and presented with a KMS page flip.  Flush queues the
 * flip; LVGL's flush-wait callback waits before either buffer is reused.
 * Unsupported tasks remain assigned to LVGL's software draw unit.
 */

#include "lvgl-gpu-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <drm/n32h7xx_drm.h>
#include <xf86drmMode.h>

#include "include/lvgl/draw/lv_draw_arc.h"
#include "include/lvgl/draw/lv_draw_line.h"
#include "include/lvgl/draw/lv_draw_rect.h"
#include "include/lvgl/draw/lv_draw_triangle.h"
#include "src/draw/lv_draw_private.h"
#include "src/core/lv_refr_private.h"
#include "src/misc/lv_area_private.h"

#define GPU_DRAW_UNIT_ID 240
#define GPU_BUFFER_COUNT 2
#define GPU_DRM_NODE_MAX 16

/*
 * A synchronous GPU submission costs tens of microseconds even for one tiny
 * rectangle.  Keep small opaque fills in LVGL's software unit.  Very large
 * fills are also faster in the coherent CPU mapping while LCDC is scanning:
 * on the 800x480 panel a full-screen GPU fill takes about 37 ms under scanout
 * contention, versus about 23 ms for the CPU, and the long GPU transaction is
 * what triggers LCDC FIFO underruns.  Medium fills and every supported vector
 * primitive still use the GPU.
 */
#define GPU_MIN_PLAIN_FILL_PIXELS 16384
#define GPU_MAX_PLAIN_FILL_PIXELS 131072
#define GPU_SYNC_MIN_BYTES 4096
#define GPU_MIN_CLIPPED_POLY_PIXELS 4096

struct gpu_buffer {
    uint32_t handle;
    uint32_t lcdc_handle;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;
    int owns_fb;
    bool clipping;
};

enum gpu_profile_stage {
    GPU_PROFILE_REFRESH,
    GPU_PROFILE_RENDER,
    GPU_PROFILE_FILL,
    GPU_PROFILE_RECT,
    GPU_PROFILE_POLY,
    GPU_PROFILE_SYNC,
    GPU_PROFILE_FLIP_SUBMIT,
    GPU_PROFILE_FLIP_WAIT,
    GPU_PROFILE_COUNT
};

struct gpu_profile_stamp {
    uint64_t wall;
    uint64_t cpu;
};

struct gpu_profile_metric {
    uint64_t calls;
    uint64_t wall;
    uint64_t cpu;
    uint64_t max;
    uint64_t histogram[32];
};

struct lvgl_gpu_backend {
    int lcdc_fd;
    int gpu_fd;
    char lcdc_node[64];
    char gpu_node[64];
    uint32_t crtc_id;
    uint32_t old_fb;
    uint32_t connector_id;
    uint32_t width;
    uint32_t height;
    struct drm_mode_modeinfo mode;
    struct gpu_buffer buffers[GPU_BUFFER_COUNT];
    uint32_t scanout_buffer;
    int pending_buffer;
    bool master;
    uint32_t fills;
    uint32_t batches;
    uint32_t polygons;
    uint32_t poly_batches;
    uint32_t lines;
    uint32_t arcs;
    uint32_t triangles;
    uint32_t borders;
    uint32_t presents;
    uint32_t large_fill_fallbacks;
    uint32_t max_plain_fill_pixels;
    bool disable_acceleration;
    bool gpu_sync;
    bool clipping;
    int error;
    uint64_t gpu_sync_bytes;
    uint64_t cpu_sync_bytes;
    uint32_t sync_fallbacks;
    bool profile_enabled;
    struct gpu_profile_metric profile[GPU_PROFILE_COUNT];
    struct gpu_profile_stamp refresh_start;
    struct gpu_profile_stamp render_start;
    uint64_t sync_bytes;
    uint64_t profile_cpu_resolution_ns;
};

typedef struct {
    lv_draw_unit_t base;
    lvgl_gpu_backend_t *backend;
} gpu_draw_unit_t;

static uint64_t profile_clock(clockid_t clock)
{
    struct timespec ts;

    if (clock_gettime(clock, &ts))
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static struct gpu_profile_stamp profile_begin(lvgl_gpu_backend_t *backend)
{
    struct gpu_profile_stamp stamp = { 0 };

    if (backend->profile_enabled) {
        stamp.wall = profile_clock(CLOCK_MONOTONIC);
        stamp.cpu = profile_clock(CLOCK_THREAD_CPUTIME_ID);
    }
    return stamp;
}

static void profile_end(lvgl_gpu_backend_t *backend,
                         enum gpu_profile_stage stage,
                         struct gpu_profile_stamp start)
{
    struct gpu_profile_metric *metric;
    uint64_t wall, cpu, us;
    unsigned int bucket = 0;

    if (!backend->profile_enabled)
        return;
    wall = profile_clock(CLOCK_MONOTONIC) - start.wall;
    cpu = profile_clock(CLOCK_THREAD_CPUTIME_ID) - start.cpu;
    metric = &backend->profile[stage];
    metric->calls++;
    metric->wall += wall;
    metric->cpu += cpu;
    if (wall > metric->max)
        metric->max = wall;
    us = (wall + 999) / 1000;
    while (us > 1 && bucket < 31) {
        us = (us + 1) / 2;
        bucket++;
    }
    metric->histogram[bucket]++;
}

static int gpu_ioctl(lvgl_gpu_backend_t *backend, unsigned long request,
                      void *args, enum gpu_profile_stage stage)
{
    struct gpu_profile_stamp start = profile_begin(backend);
    int ret = ioctl(backend->gpu_fd, request, args);
    int saved_errno = errno;

    profile_end(backend, stage, start);
    errno = saved_errno;
    return ret;
}

static void profile_display_event(lv_event_t *event)
{
    lvgl_gpu_backend_t *backend = lv_event_get_user_data(event);

    switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START:
        backend->refresh_start = profile_begin(backend);
        break;
    case LV_EVENT_REFR_READY:
        profile_end(backend, GPU_PROFILE_REFRESH, backend->refresh_start);
        break;
    case LV_EVENT_RENDER_START:
        backend->render_start = profile_begin(backend);
        break;
    case LV_EVENT_RENDER_READY:
        profile_end(backend, GPU_PROFILE_RENDER, backend->render_start);
        break;
    default:
        break;
    }
}

static void sync_failed(lvgl_gpu_backend_t *backend, lv_display_t *display,
                         int error)
{
    /* A failed job may still hold DMA references. Cancel this refresh before
     * rendering starts, and let the application leave its main loop. */
    backend->error = error;
    /* Invalidation uses a nesting counter, not a boolean. Ensure the clear
     * is accepted, then disable every level for this terminal error. */
    while (!lv_display_is_invalidation_enabled(display))
        lv_display_enable_invalidation(display, true);
    lv_inv_area(display, NULL);
    while (lv_display_is_invalidation_enabled(display))
        lv_display_enable_invalidation(display, false);
    lv_timer_pause(lv_display_get_refr_timer(display));
    fprintf(stderr, "lvgl-gpu: sync failed: %s; display stopped\n",
            strerror(error));
}

static void sync_cb(lv_display_t *display, const lv_area_t *area)
{
    lvgl_gpu_backend_t *backend = lv_display_get_driver_data(display);
    const struct gpu_buffer *front = &backend->buffers[backend->scanout_buffer];
    lv_draw_buf_t *active = lv_display_get_buf_active(display);
    const struct gpu_buffer *back = NULL;
    struct gpu_profile_stamp start = profile_begin(backend);
    uint64_t bytes;
    lv_draw_buf_t source;

    if (backend->error)
        goto ready;
    if (area->x1 < 0 || area->y1 < 0 || area->x2 < area->x1 ||
        area->y2 < area->y1 || area->x2 >= (int32_t)front->width ||
        area->y2 >= (int32_t)front->height ||
        area->x2 >= (int32_t)active->header.w ||
        area->y2 >= (int32_t)active->header.h) {
        sync_failed(backend, display, EINVAL);
        goto ready;
    }
    bytes = (uint64_t)lv_area_get_width(area) * lv_area_get_height(area) * 2U;
    for (unsigned int i = 0; i < GPU_BUFFER_COUNT; i++)
        if (backend->buffers[i].map == active->data)
            back = &backend->buffers[i];
    if (backend->gpu_sync && bytes >= GPU_SYNC_MIN_BYTES && back &&
        back != front && area->x2 < (int32_t)back->width &&
        area->y2 < (int32_t)back->height) {
        struct drm_n32h7xx_blit blit = {
            .src_handle = front->handle,
            .dst_handle = back->handle,
            .src_format = DRM_FORMAT_RGB565,
            .dst_format = DRM_FORMAT_RGB565,
            .src_offset = (uint32_t)area->y1 * front->pitch + (uint32_t)area->x1 * 2U,
            .dst_offset = (uint32_t)area->y1 * back->pitch + (uint32_t)area->x1 * 2U,
            .src_pitch = front->pitch,
            .dst_pitch = back->pitch,
            .src_width = (uint32_t)lv_area_get_width(area),
            .src_height = (uint32_t)lv_area_get_height(area),
            .width = (uint32_t)lv_area_get_width(area),
            .height = (uint32_t)lv_area_get_height(area),
        };

        if (!ioctl(backend->gpu_fd, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
            backend->gpu_sync_bytes += bytes;
            goto copied;
        }
        /* These errors occur before a job can start. Other errors must not
         * trigger a CPU write to buffers whose DMA state is unknown. */
        if (errno != EINVAL && errno != ENOTTY && errno != EOPNOTSUPP &&
            errno != ENOMEM) {
            sync_failed(backend, display, errno);
            goto ready;
        }
        backend->gpu_sync = false;
        backend->sync_fallbacks++;
        fprintf(stderr, "lvgl-gpu: sync unavailable; using CPU copy\n");
    }
    lv_draw_buf_init(&source, front->width, front->height,
                     LV_COLOR_FORMAT_RGB565, front->pitch,
                     front->map, (uint32_t)front->size);
    lv_draw_buf_copy(active, area, &source, area);
    backend->cpu_sync_bytes += bytes;
copied:
    backend->sync_bytes += bytes;
ready:
    profile_end(backend, GPU_PROFILE_SYNC, start);
    lv_display_sync_ready(display);
}

static void profile_dump(const lvgl_gpu_backend_t *backend)
{
    static const char *const names[] = {
        "refresh", "render", "fill_ioctl", "rect_ioctl", "poly_ioctl",
        "sync_copy", "flip_submit", "flip_wait"
    };

    if (!backend->profile_enabled)
        return;
    puts("gpu-profile: nested wall/CPU totals; ioctl time includes completion wait");
    printf("gpu-profile: cpu_clock_resolution_us=%llu\n",
           (unsigned long long)((backend->profile_cpu_resolution_ns + 999) / 1000));
    for (unsigned int i = 0; i < GPU_PROFILE_COUNT; i++) {
        const struct gpu_profile_metric *metric = &backend->profile[i];
        uint64_t sum = 0, p95 = 0;

        if (!metric->calls)
            continue;
        for (unsigned int bucket = 0; bucket < 32; bucket++) {
            sum += metric->histogram[bucket];
            if (sum >= (metric->calls * 95 + 99) / 100) {
                p95 = 1ULL << bucket;
                break;
            }
        }
        printf("gpu-profile: %s calls=%llu wall_us=%llu cpu_us=%llu "
               "max_us=%llu p95_us_upper=%llu\n", names[i],
               (unsigned long long)metric->calls,
               (unsigned long long)(metric->wall / 1000),
               (unsigned long long)(metric->cpu / 1000),
               (unsigned long long)((metric->max + 999) / 1000),
               (unsigned long long)p95);
    }
    printf("gpu-profile: gpu_sync_bytes=%llu cpu_sync_bytes=%llu sync_fallbacks=%u\n",
           (unsigned long long)backend->gpu_sync_bytes,
           (unsigned long long)backend->cpu_sync_bytes, backend->sync_fallbacks);
    printf("gpu-profile: sync_bytes=%llu completed_presents=%u\n",
           (unsigned long long)backend->sync_bytes, backend->presents);
}

static uint32_t tick_get_cb(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return (uint32_t)now.tv_sec * 1000U +
           (uint32_t)now.tv_nsec / 1000000U;
}

static int drm_node_name(const char *path, char *name, size_t name_size)
{
    struct drm_version version = { 0 };
    int fd;
    int ret;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    version.name = name;
    version.name_len = (uint32_t)name_size - 1U;
    ret = ioctl(fd, DRM_IOCTL_VERSION, &version);
    close(fd);
    if (ret)
        return -1;
    name[LV_MIN(version.name_len, name_size - 1U)] = '\0';
    return 0;
}

int lvgl_gpu_discover(char *lcdc_node, size_t lcdc_size,
                      char *gpu_node, size_t gpu_size)
{
    char path[64];
    char name[64];
    int have_lcdc = 0;
    int have_gpu = 0;

    if (!lcdc_node || !lcdc_size || !gpu_node || !gpu_size)
        return -1;
    lcdc_node[0] = '\0';
    gpu_node[0] = '\0';

    for (int i = 0; i < GPU_DRM_NODE_MAX; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (drm_node_name(path, name, sizeof(name)))
            continue;
        if (!have_lcdc && !strcmp(name, "n32h7xx-lcdc")) {
            snprintf(lcdc_node, lcdc_size, "%s", path);
            have_lcdc = 1;
        }
        else if (!have_gpu && !strcmp(name, "n32h7xx-gpu")) {
            snprintf(gpu_node, gpu_size, "%s", path);
            have_gpu = 1;
        }
    }

    if (!have_gpu) {
        for (int i = 0; i < GPU_DRM_NODE_MAX; i++) {
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", 128 + i);
            if (drm_node_name(path, name, sizeof(name)))
                continue;
            if (!strcmp(name, "n32h7xx-gpu")) {
                snprintf(gpu_node, gpu_size, "%s", path);
                have_gpu = 1;
                break;
            }
        }
    }

    return have_lcdc && have_gpu ? 0 : -1;
}

static int get_crtc_state(lvgl_gpu_backend_t *backend)
{
    struct drm_mode_card_res resources = { 0 };
    struct drm_mode_crtc crtc = { 0 };
    uint32_t crtcs[4];
    uint32_t connectors[8];
    int found_connector = 0;

    resources.count_crtcs = 4;
    resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    resources.count_connectors = 8;
    resources.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) ||
        resources.count_crtcs == 0 || resources.count_connectors == 0) {
        perror("lvgl-gpu: MODE_GETRESOURCES");
        return -1;
    }
    if (resources.count_crtcs > 4 || resources.count_connectors > 8) {
        fprintf(stderr,
                "lvgl-gpu: LCDC has too many KMS objects (CRTC=%u connector=%u)\n",
                resources.count_crtcs, resources.count_connectors);
        return -1;
    }

    crtc.crtc_id = crtcs[0];
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_GETCRTC, &crtc) ||
        !crtc.mode_valid || !crtc.fb_id) {
        fprintf(stderr, "lvgl-gpu: LCDC CRTC is not ready\n");
        return -1;
    }

    for (uint32_t i = 0; i < resources.count_connectors; i++) {
        struct drm_mode_get_connector connector = {
            .connector_id = connectors[i],
        };
        struct drm_mode_get_encoder encoder = { 0 };

        if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector)) {
            perror("lvgl-gpu: MODE_GETCONNECTOR");
            return -1;
        }
        if (connector.connection != DRM_MODE_CONNECTED ||
            !connector.encoder_id)
            continue;
        encoder.encoder_id = connector.encoder_id;
        if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_GETENCODER, &encoder)) {
            perror("lvgl-gpu: MODE_GETENCODER");
            return -1;
        }
        if (encoder.crtc_id == crtc.crtc_id) {
            backend->connector_id = connector.connector_id;
            found_connector = 1;
            break;
        }
    }
    if (!found_connector) {
        fprintf(stderr, "lvgl-gpu: LCDC has no connected connector\n");
        return -1;
    }

    backend->crtc_id = crtc.crtc_id;
    backend->old_fb = crtc.fb_id;
    backend->width = crtc.mode.hdisplay;
    backend->height = crtc.mode.vdisplay;
    backend->mode = crtc.mode;
    return 0;
}

static int activate_crtc(lvgl_gpu_backend_t *backend)
{
    struct drm_mode_crtc set = {
        .set_connectors_ptr = (uint64_t)(uintptr_t)&backend->connector_id,
        .count_connectors = 1,
        .crtc_id = backend->crtc_id,
        .fb_id = backend->old_fb,
        .mode = backend->mode,
        .mode_valid = 1,
    };

    if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_SETCRTC, &set)) {
        perror("lvgl-gpu: MODE_SETCRTC");
        return -1;
    }
    return 0;
}

static int allocate_frame_buffer(lvgl_gpu_backend_t *backend,
                                 struct gpu_buffer *buffer)
{
    struct drm_n32h7xx_create create = { 0 };
    struct drm_mode_map_dumb map = { 0 };

    buffer->width = backend->width;
    buffer->height = backend->height;
    buffer->pitch = backend->width * 2U;
    buffer->size = (uint64_t)buffer->pitch * buffer->height;
    create.size = buffer->size;
    if (ioctl(backend->gpu_fd, DRM_IOCTL_N32H7XX_CREATE, &create)) {
        perror("lvgl-gpu: CREATE");
        return -1;
    }
    buffer->handle = create.handle;

    map.handle = create.handle;
    if (ioctl(backend->gpu_fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        perror("lvgl-gpu: MAP_DUMB");
        return -1;
    }
    buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, backend->gpu_fd, map.offset);
    if (buffer->map == MAP_FAILED) {
        perror("lvgl-gpu: mmap");
        buffer->map = NULL;
        return -1;
    }
    memset(buffer->map, 0, buffer->size);
    return 0;
}

static int import_buffer_to_lcdc(lvgl_gpu_backend_t *backend,
                                 struct gpu_buffer *buffer)
{
    struct drm_prime_handle prime = { 0 };
    struct drm_mode_fb_cmd2 fb = { 0 };

    prime.handle = buffer->handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(backend->gpu_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
        perror("lvgl-gpu: PRIME export");
        return -1;
    }
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) {
        close(prime.fd);
        perror("lvgl-gpu: PRIME import into LCDC");
        return -1;
    }
    close(prime.fd);
    buffer->lcdc_handle = prime.handle;

    fb.width = buffer->width;
    fb.height = buffer->height;
    fb.pixel_format = DRM_FORMAT_RGB565;
    fb.handles[0] = buffer->lcdc_handle;
    fb.pitches[0] = buffer->pitch;
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_ADDFB2, &fb)) {
        perror("lvgl-gpu: ADDFB2");
        return -1;
    }
    buffer->fb_id = fb.fb_id;
    buffer->owns_fb = true;
    return 0;
}

static int import_existing_buffer(lvgl_gpu_backend_t *backend,
                                  struct gpu_buffer *buffer, uint32_t fb_id)
{
    struct drm_mode_fb_cmd2 fb = { .fb_id = fb_id };
    struct drm_mode_map_dumb map = { 0 };
    struct drm_prime_handle prime = { 0 };

    if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_GETFB2, &fb)) {
        perror("lvgl-gpu: GETFB2");
        return -1;
    }
    if (fb.pixel_format != DRM_FORMAT_RGB565 || !fb.handles[0] ||
        fb.width != backend->width || fb.height != backend->height) {
        fprintf(stderr,
                "lvgl-gpu: active framebuffer is not compatible RGB565\n");
        return -1;
    }

    buffer->lcdc_handle = fb.handles[0];
    buffer->fb_id = fb_id;
    buffer->width = fb.width;
    buffer->height = fb.height;
    buffer->pitch = fb.pitches[0];
    buffer->size = (uint64_t)buffer->pitch * buffer->height;

    map.handle = buffer->lcdc_handle;
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        perror("lvgl-gpu: map active framebuffer");
        return -1;
    }
    buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, backend->lcdc_fd, map.offset);
    if (buffer->map == MAP_FAILED) {
        buffer->map = NULL;
        perror("lvgl-gpu: mmap active framebuffer");
        return -1;
    }

    prime.handle = buffer->lcdc_handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
        perror("lvgl-gpu: PRIME export active framebuffer");
        return -1;
    }
    if (ioctl(backend->gpu_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) {
        close(prime.fd);
        perror("lvgl-gpu: PRIME import active framebuffer");
        return -1;
    }
    close(prime.fd);
    buffer->handle = prime.handle;
    return 0;
}

/* Check a known pattern at an unaligned destination before enabling sync.
 * Older drivers accepted BLIT but sampled the wrong X origin there. */
static int probe_gpu_sync(lvgl_gpu_backend_t *backend)
{
    struct drm_n32h7xx_create create = { .size = 4096 };
    struct drm_gem_close close_bo = { 0 };
    struct gpu_buffer *back = &backend->buffers[1];
    uint16_t *source;
    int supported = 0;

    if (ioctl(backend->gpu_fd, DRM_IOCTL_N32H7XX_CREATE, &create))
        return 0;
    source = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  backend->gpu_fd, create.mmap_offset);
    if (source == MAP_FAILED)
        goto close;
    for (unsigned int i = 0; i < 8; i++)
        source[i] = (uint16_t)(0x1230 + i * 137);
    {
        struct drm_n32h7xx_blit blit = {
            .src_handle = create.handle, .dst_handle = back->handle,
            .src_format = DRM_FORMAT_RGB565, .dst_format = DRM_FORMAT_RGB565,
            .src_pitch = 16, .dst_pitch = back->pitch, .dst_offset = 6,
            .src_width = 8, .src_height = 1, .width = 8, .height = 1,
        };

        if (ioctl(backend->gpu_fd, DRM_IOCTL_N32H7XX_BLIT, &blit)) {
            if (errno != EINVAL && errno != ENOTTY && errno != EOPNOTSUPP &&
                errno != ENOMEM)
                supported = -1;
        }
        else {
            supported = !memcmp(back->map + 6, source, 16);
            memset(back->map + 6, 0, 16);
        }
    }
    munmap(source, create.size);
close:
    close_bo.handle = create.handle;
    ioctl(backend->gpu_fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
    return supported;
}

static int probe_poly_clip(lvgl_gpu_backend_t *backend)
{
    struct gpu_buffer *back = &backend->buffers[1];
    struct drm_n32h7xx_poly poly = {
        .handle = back->handle, .format = DRM_FORMAT_RGB565,
        .pitch = back->pitch, .color = 0xff00ff00,
        .vertex_count = 4, .flags = DRM_N32H7XX_POLY_NO_AA,
        .x = { 0, 8 * 65536, 8 * 65536, 0 },
        .y = { 0, 0, 8 * 65536, 8 * 65536 },
    };
    struct drm_n32h7xx_poly_clip_batch clip = {
        .handle = back->handle, .count = 1, .polys = (uint64_t)(uintptr_t)&poly,
        .x = 3, .y = 3, .width = 2, .height = 2,
    };
    int supported = 1;

    if (back->width < 8 || back->height < 8)
        return 0;
    if (ioctl(backend->gpu_fd, DRM_IOCTL_N32H7XX_POLY_CLIP_BATCH, &clip))
        return (errno == EINVAL || errno == ENOTTY || errno == EOPNOTSUPP ||
                errno == ENOMEM) ? 0 : -1;
    for (unsigned int y = 0; y < 8; y++) {
        uint16_t *row = (uint16_t *)(back->map + y * back->pitch);

        for (unsigned int x = 0; x < 8; x++)
            if (row[x] != ((x >= 3 && x < 5 && y >= 3 && y < 5) ? 0x07e0 : 0))
                supported = 0;
        memset(row, 0, 16);
    }
    return supported;
}

static int find_buffer(const lvgl_gpu_backend_t *backend, const void *pointer)
{
    const uint8_t *p = pointer;
    int i;

    for (i = 0; i < GPU_BUFFER_COUNT; i++) {
        const struct gpu_buffer *buffer = &backend->buffers[i];

        if (buffer->map && p >= buffer->map &&
            p < buffer->map + (size_t)buffer->size)
            return i;
    }
    return -1;
}

static const struct gpu_buffer *
buffer_for_layer(const lvgl_gpu_backend_t *backend, const lv_layer_t *layer)
{
    int index = find_buffer(backend, layer->draw_buf->data);

    /* Subviews need a separate BO offset; only whole mapped draw buffers
     * are eligible for direct GPU commands. */
    return index < 0 || layer->draw_buf->data != backend->buffers[index].map ?
           NULL : &backend->buffers[index];
}

static int read_flip_event(lvgl_gpu_backend_t *backend)
{
    struct drm_event_vblank event;
    struct pollfd pfd = {
        .fd = backend->lcdc_fd,
        .events = POLLIN,
    };
    struct gpu_profile_stamp start = profile_begin(backend);
    ssize_t length;
    int ret;

    do {
        ret = poll(&pfd, 1, 2000);
    } while (ret < 0 && errno == EINTR);
    if (ret != 1) {
        fprintf(stderr, "lvgl-gpu: timed out waiting for flip completion\n");
        profile_end(backend, GPU_PROFILE_FLIP_WAIT, start);
        return -1;
    }
    do {
        length = read(backend->lcdc_fd, &event, sizeof(event));
    } while (length < 0 && errno == EINTR);
    if (length != sizeof(event) ||
        event.base.type != DRM_EVENT_FLIP_COMPLETE) {
        fprintf(stderr, "lvgl-gpu: invalid flip-complete event\n");
        profile_end(backend, GPU_PROFILE_FLIP_WAIT, start);
        return -1;
    }
    profile_end(backend, GPU_PROFILE_FLIP_WAIT, start);
    return 0;
}

static int submit_flip(lvgl_gpu_backend_t *backend, uint32_t fb_id)
{
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = backend->crtc_id,
        .fb_id = fb_id,
        .flags = DRM_MODE_PAGE_FLIP_EVENT,
    };

    struct gpu_profile_stamp start = profile_begin(backend);
    int ret = ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
    int saved_errno = errno;

    profile_end(backend, GPU_PROFILE_FLIP_SUBMIT, start);
    errno = saved_errno;
    if (ret) {
        perror("lvgl-gpu: page flip");
        return -1;
    }
    return 0;
}

static int flip_and_wait(lvgl_gpu_backend_t *backend, uint32_t fb_id)
{
    if (submit_flip(backend, fb_id))
        return -1;
    return read_flip_event(backend);
}

static void flush_cb(lv_display_t *display, const lv_area_t *area,
                     uint8_t *pixels)
{
    lvgl_gpu_backend_t *backend = lv_display_get_driver_data(display);
    int index;

    (void)area;
    /* DIRECT mode may flush several dirty areas from the same back buffer.
     * Keep that buffer off scanout until LVGL marks the complete frame last. */
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    index = find_buffer(backend, pixels);
    if (index < 0) {
        fprintf(stderr, "lvgl-gpu: display buffer is not GPU-owned\n");
        lv_display_flush_ready(display);
        return;
    }
    if (index != (int)backend->scanout_buffer) {
        if (!submit_flip(backend, backend->buffers[index].fb_id)) {
            backend->pending_buffer = index;
            return;
        }
    }
    lv_display_flush_ready(display);
}

static void finish_pending_flip(lvgl_gpu_backend_t *backend)
{
    if (backend->pending_buffer < 0)
        return;
    if (!read_flip_event(backend)) {
        backend->scanout_buffer = (uint32_t)backend->pending_buffer;
        backend->presents++;
    }
    backend->pending_buffer = -1;
}

static void flush_wait_cb(lv_display_t *display)
{
    lvgl_gpu_backend_t *backend = lv_display_get_driver_data(display);

    /* LVGL waits here before syncing or reusing the old scanout buffer.
     * Queueing the flip in flush_cb lets its vblank wait overlap the main
     * loop's timer sleep instead of adding both delays to every frame. */
    finish_pending_flip(backend);
}

static int64_t gpu_floor_q16(int64_t value)
{
    return value >= 0 ? value >> 16 : -((-value + 0xffff) >> 16);
}

static int64_t gpu_ceil_q16(int64_t value)
{
    return value >= 0 ? (value + 0xffff) >> 16 : -((-value) >> 16);
}

static uint32_t gpu_isqrt64(uint64_t value)
{
    uint64_t bit = (uint64_t)1 << 62;
    uint64_t root = 0;

    while (bit > value)
        bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        }
        else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

static void gpu_visible_area(const lv_draw_task_t *task, lv_area_t *area)
{
    const lv_layer_t *layer = task->target_layer;

    area->x1 = LV_MAX(task->clip_area.x1, layer->buf_area.x1);
    area->y1 = LV_MAX(task->clip_area.y1, layer->buf_area.y1);
    area->x2 = LV_MIN(task->clip_area.x2, layer->buf_area.x2);
    area->y2 = LV_MIN(task->clip_area.y2, layer->buf_area.y2);
}

static int gpu_area_in_bounds(const lv_draw_task_t *task)
{
    lv_area_t visible;

    gpu_visible_area(task, &visible);
    return task->area.x1 >= visible.x1 && task->area.x2 <= visible.x2 &&
           task->area.y1 >= visible.y1 && task->area.y2 <= visible.y2;
}

/* A clipped rounded fill is rectangular when it stays clear of all four
 * corner boxes. Keep corner antialiasing on the existing rounded/SW paths. */
static bool gpu_fill_rect_area(const lv_draw_task_t *task, lv_area_t *area)
{
    const lv_draw_fill_dsc_t *fill = task->draw_dsc;
    lv_area_t visible;
    int32_t radius;

    gpu_visible_area(task, &visible);
    if (!lv_area_intersect(area, &task->area, &visible))
        return false;
    radius = LV_MIN(fill->radius, LV_MIN(lv_area_get_width(&task->area),
                                        lv_area_get_height(&task->area)) / 2);
    if (radius < 0)
        return false;
    return !radius ||
           (area->x1 >= task->area.x1 + radius &&
            area->x2 <= task->area.x2 - radius) ||
           (area->y1 >= task->area.y1 + radius &&
            area->y2 <= task->area.y2 - radius);
}

static int gpu_box_in_bounds(const lv_draw_task_t *task,
                             int64_t min_x, int64_t max_x,
                             int64_t min_y, int64_t max_y)
{
    lv_area_t visible;
    int64_t x0 = gpu_floor_q16(min_x);
    int64_t x1 = gpu_ceil_q16(max_x) - 1;
    int64_t y0 = gpu_floor_q16(min_y);
    int64_t y1 = gpu_ceil_q16(max_y) - 1;

    gpu_visible_area(task, &visible);
    return x0 >= visible.x1 && x1 <= visible.x2 &&
           y0 >= visible.y1 && y1 <= visible.y2;
}

static int gpu_geometry_in_bounds(const lv_draw_task_t *task,
                                   const struct gpu_buffer *buffer,
                                   int64_t min_x, int64_t max_x,
                                   int64_t min_y, int64_t max_y)
{
    const lv_area_t *area = &task->target_layer->buf_area;

    if (!buffer->clipping)
        return gpu_box_in_bounds(task, min_x, max_x, min_y, max_y);
    return gpu_floor_q16(min_x) >= area->x1 &&
           gpu_ceil_q16(max_x) - 1 <= area->x2 &&
           gpu_floor_q16(min_y) >= area->y1 &&
           gpu_ceil_q16(max_y) - 1 <= area->y2;
}

static int gpu_poly_in_bounds(const lv_draw_task_t *task,
                              const struct gpu_buffer *buffer,
                              const int64_t *x, const int64_t *y, int count)
{
    int64_t min_x = x[0], max_x = x[0];
    int64_t min_y = y[0], max_y = y[0];

    for (int i = 1; i < count; i++) {
        min_x = LV_MIN(min_x, x[i]);
        max_x = LV_MAX(max_x, x[i]);
        min_y = LV_MIN(min_y, y[i]);
        max_y = LV_MAX(max_y, y[i]);
    }
    return gpu_geometry_in_bounds(task, buffer, min_x, max_x, min_y, max_y);
}

static int gpu_area_primitive_in_bounds(const lv_draw_task_t *task,
                                        const lv_area_t *area)
{
    return gpu_box_in_bounds(task, (int64_t)area->x1 << 16,
                             (int64_t)(area->x2 + 1) << 16,
                             (int64_t)area->y1 << 16,
                             (int64_t)(area->y2 + 1) << 16);
}

static int gpu_add_fixed_poly(lv_draw_task_t *task,
                              const struct gpu_buffer *buffer,
                              const int64_t *x, const int64_t *y, int count,
                              uint32_t color, struct drm_n32h7xx_poly *poly)
{
    const lv_layer_t *layer = task->target_layer;
    int64_t min_x = x[0], max_x = x[0], min_y = y[0], max_y = y[0];
    int64_t px0, px1, py0, py1;
    int64_t origin_x, origin_y;
    uint32_t offset;
    int i;

    if (count < 3 || count > 6 || !gpu_poly_in_bounds(task, buffer, x, y, count))
        return -1;
    for (i = 1; i < count; i++) {
        min_x = LV_MIN(min_x, x[i]);
        max_x = LV_MAX(max_x, x[i]);
        min_y = LV_MIN(min_y, y[i]);
        max_y = LV_MAX(max_y, y[i]);
    }

    if (buffer->clipping) {
        lv_area_t visible;

        gpu_visible_area(task, &visible);
        if (gpu_ceil_q16(max_x) < visible.x1 ||
            gpu_floor_q16(min_x) > (int64_t)visible.x2 + 1 ||
            gpu_ceil_q16(max_y) < visible.y1 ||
            gpu_floor_q16(min_y) > (int64_t)visible.y2 + 1)
            return 0;
    }

    px0 = gpu_floor_q16(min_x);
    px1 = gpu_ceil_q16(max_x) - 1;
    py0 = gpu_floor_q16(min_y);
    py1 = gpu_ceil_q16(max_y) - 1;
    origin_x = buffer->clipping ? 0 : px0 - layer->buf_area.x1;
    origin_y = buffer->clipping ? 0 : py0 - layer->buf_area.y1;
    origin_x &= ~3LL; /* A 16-bit RGB565 row is 8-byte aligned every 4 pixels. */
    if (origin_x < 0 || origin_y < 0 ||
        px1 - layer->buf_area.x1 >= buffer->width ||
        py1 - layer->buf_area.y1 >= buffer->height)
        return -1;

    offset = (uint32_t)origin_y * layer->draw_buf->header.stride +
             (uint32_t)origin_x * 2U;
    memset(poly, 0, sizeof(*poly));
    poly->handle = buffer->handle;
    poly->format = DRM_FORMAT_RGB565;
    poly->offset = offset;
    poly->pitch = layer->draw_buf->header.stride;
    poly->color = color;
    poly->vertex_count = (uint32_t)count;
    for (i = 0; i < count; i++) {
        poly->x[i] = (int32_t)(x[i] - (origin_x + layer->buf_area.x1) * 65536LL);
        poly->y[i] = (int32_t)(y[i] - (origin_y + layer->buf_area.y1) * 65536LL);
    }
    return 1;
}

static int gpu_flush_polys(gpu_draw_unit_t *gpu, const lv_draw_task_t *task,
                           struct drm_n32h7xx_poly *polys, uint32_t count)
{
    struct drm_n32h7xx_poly_batch batch = {
        .count = count,
        .polys = (uint64_t)(uintptr_t)polys,
    };

    if (!count)
        return 0;
    batch.handle = polys[0].handle;
    if (gpu->backend->clipping) {
        lv_area_t area;
        const lv_area_t *origin = &task->target_layer->buf_area;
        struct drm_n32h7xx_poly_clip_batch clipped = {
            .handle = polys[0].handle,
            .count = count,
            .polys = (uint64_t)(uintptr_t)polys,
        };

        gpu_visible_area(task, &area);
        clipped.x = (uint32_t)(area.x1 - origin->x1);
        clipped.y = (uint32_t)(area.y1 - origin->y1);
        clipped.width = (uint32_t)lv_area_get_width(&area);
        clipped.height = (uint32_t)lv_area_get_height(&area);
        if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_POLY_CLIP_BATCH,
                      &clipped, GPU_PROFILE_POLY))
            return -1;
    }
    else if (count == 1) {
        if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_POLY, polys,
                      GPU_PROFILE_POLY))
            return -1;
    }
    else if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_POLY_BATCH, &batch,
                      GPU_PROFILE_POLY)) {
        return -1;
    }
    gpu->backend->poly_batches++;
    gpu->backend->polygons += count;
    return 0;
}

static int gpu_add_rect_primitive(lv_draw_task_t *task,
                                  const struct gpu_buffer *buffer,
                                  const lv_area_t *area, uint32_t color,
                                  struct drm_n32h7xx_rect *rect)
{
    const lv_layer_t *layer = task->target_layer;
    int x;
    int y;

    if (!gpu_area_primitive_in_bounds(task, area) || area->x1 > area->x2 ||
        area->y1 > area->y2)
        return -1;
    x = area->x1 - layer->buf_area.x1;
    y = area->y1 - layer->buf_area.y1;
    if (x < 0 || y < 0 || area->x2 - layer->buf_area.x1 >= (int)buffer->width ||
        area->y2 - layer->buf_area.y1 >= (int)buffer->height)
        return -1;

    memset(rect, 0, sizeof(*rect));
    rect->handle = buffer->handle;
    rect->format = DRM_FORMAT_RGB565;
    rect->offset = (uint32_t)y * buffer->pitch + (uint32_t)x * 2U;
    rect->pitch = buffer->pitch;
    rect->width = (uint32_t)lv_area_get_width(area);
    rect->height = (uint32_t)lv_area_get_height(area);
    rect->color = color;
    rect->flags = 0;
    return 0;
}

static int gpu_flush_rects(gpu_draw_unit_t *gpu,
                           struct drm_n32h7xx_rect *rects, uint32_t count)
{
    struct drm_n32h7xx_rect_batch batch = {
        .handle = rects[0].handle,
        .count = count,
        .rects = (uint64_t)(uintptr_t)rects,
    };

    if (!count)
        return 0;
    if (count == 1) {
        if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_RECT, rects,
                      GPU_PROFILE_RECT))
            return -1;
    }
    else if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch,
                      GPU_PROFILE_RECT)) {
        return -1;
    }
    gpu->backend->batches++;
    return 0;
}

static int64_t gpu_trigo_fixed(int32_t radius, int16_t angle, int sine)
{
    int32_t value = sine ? lv_trigo_sin(angle) : lv_trigo_cos(angle);

    return (int64_t)radius * value * 2;
}

static int gpu_add_quarter_disk(lv_draw_task_t *task,
                                const struct gpu_buffer *buffer,
                                lv_point_t center, int32_t radius,
                                int16_t start_angle, uint32_t color,
                                struct drm_n32h7xx_poly *poly)
{
    int64_t x[6], y[6];

    x[0] = (int64_t)center.x << 16;
    y[0] = (int64_t)center.y << 16;
    for (int i = 0; i < 5; i++) {
        int16_t angle = start_angle + i * 90 / 4;

        x[i + 1] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(radius, angle, 0);
        y[i + 1] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(radius, angle, 1);
    }
    return gpu_add_fixed_poly(task, buffer, x, y, 6, color, poly);
}

static int gpu_add_ring_arc(lv_draw_task_t *task,
                            const struct gpu_buffer *buffer,
                            lv_point_t center, int32_t outer_radius,
                            int32_t inner_radius, int16_t start_angle,
                            int16_t end_angle, uint32_t color,
                            struct drm_n32h7xx_poly *polys, uint32_t *count)
{
    int32_t sweep = end_angle - start_angle;
    int32_t segments = (sweep + 5) / 6;

    if (segments < 1)
        segments = 1;
    for (int i = 0; i < segments; i++) {
        int16_t a0 = start_angle + sweep * i / segments;
        int16_t a1 = start_angle + sweep * (i + 1) / segments;
        int64_t x[4], y[4];
        int vertices;
        int ret;

        if (*count == DRM_N32H7XX_POLY_BATCH_MAX)
            return -1;
        if (inner_radius > 0) {
            x[0] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(inner_radius, a0, 0);
            y[0] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(inner_radius, a0, 1);
            x[1] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(outer_radius, a0, 0);
            y[1] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(outer_radius, a0, 1);
            x[2] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(outer_radius, a1, 0);
            y[2] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(outer_radius, a1, 1);
            x[3] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(inner_radius, a1, 0);
            y[3] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(inner_radius, a1, 1);
            vertices = 4;
        }
        else {
            x[0] = (int64_t)center.x << 16;
            y[0] = (int64_t)center.y << 16;
            x[1] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(outer_radius, a0, 0);
            y[1] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(outer_radius, a0, 1);
            x[2] = ((int64_t)center.x << 16) +
                   gpu_trigo_fixed(outer_radius, a1, 0);
            y[2] = ((int64_t)center.y << 16) +
                   gpu_trigo_fixed(outer_radius, a1, 1);
            vertices = 3;
        }
        ret = gpu_add_fixed_poly(task, buffer, x, y, vertices, color,
                                 &polys[*count]);
        if (ret < 0)
            return -1;
        if (ret > 0)
            (*count)++;
    }
    return 0;
}

static int gpu_add_disk(lv_draw_task_t *task,
                        const struct gpu_buffer *buffer, lv_point_t center,
                        int32_t radius, uint32_t color,
                        struct drm_n32h7xx_poly *poly)
{
    int64_t x[6], y[6];

    for (int i = 0; i < 6; i++) {
        int16_t angle = (int16_t)(i * 60);

        x[i] = ((int64_t)center.x << 16) +
               gpu_trigo_fixed(radius, angle, 0);
        y[i] = ((int64_t)center.y << 16) +
               gpu_trigo_fixed(radius, angle, 1);
    }
    return gpu_add_fixed_poly(task, buffer, x, y, 6, color, poly);
}

static int gpu_add_line_segment(lv_draw_task_t *task,
                                const struct gpu_buffer *buffer,
                                const lv_draw_line_dsc_t *dsc,
                                lv_point_precise_t p1, lv_point_precise_t p2,
                                struct drm_n32h7xx_poly *poly)
{
    int64_t dx = (int64_t)p2.x - p1.x;
    int64_t dy = (int64_t)p2.y - p1.y;
    uint32_t len = gpu_isqrt64((uint64_t)(dx * dx + dy * dy));
    int64_t off_x, off_y;
    int64_t x[4], y[4];
    uint32_t color;

    if (!len)
        return 0;
    off_x = -dy * dsc->width * 32768 / len;
    off_y = dx * dsc->width * 32768 / len;
    x[0] = ((int64_t)p1.x << 16) + off_x;
    y[0] = ((int64_t)p1.y << 16) + off_y;
    x[1] = ((int64_t)p2.x << 16) + off_x;
    y[1] = ((int64_t)p2.y << 16) + off_y;
    x[2] = ((int64_t)p2.x << 16) - off_x;
    y[2] = ((int64_t)p2.y << 16) - off_y;
    x[3] = ((int64_t)p1.x << 16) - off_x;
    y[3] = ((int64_t)p1.y << 16) - off_y;
    color = lv_color_to_u32(dsc->color);
    return gpu_add_fixed_poly(task, buffer, x, y, 4, color, poly);
}

static int gpu_line_segment_in_bounds(lv_draw_task_t *task,
                                      const struct gpu_buffer *buffer,
                                      const lv_draw_line_dsc_t *dsc,
                                      lv_point_precise_t p1,
                                      lv_point_precise_t p2)
{
    int64_t dx = (int64_t)p2.x - p1.x;
    int64_t dy = (int64_t)p2.y - p1.y;
    uint32_t len = gpu_isqrt64((uint64_t)(dx * dx + dy * dy));
    int64_t off_x, off_y;
    int64_t x[4], y[4];

    if (!len)
        return 1;
    off_x = -dy * dsc->width * 32768 / len;
    off_y = dx * dsc->width * 32768 / len;
    x[0] = ((int64_t)p1.x << 16) + off_x;
    y[0] = ((int64_t)p1.y << 16) + off_y;
    x[1] = ((int64_t)p2.x << 16) + off_x;
    y[1] = ((int64_t)p2.y << 16) + off_y;
    x[2] = ((int64_t)p2.x << 16) - off_x;
    y[2] = ((int64_t)p2.y << 16) - off_y;
    x[3] = ((int64_t)p1.x << 16) - off_x;
    y[3] = ((int64_t)p1.y << 16) - off_y;
    return gpu_poly_in_bounds(task, buffer, x, y, 4);
}

static int gpu_line_in_bounds(lv_draw_task_t *task,
                              const struct gpu_buffer *buffer,
                              const lv_draw_line_dsc_t *dsc)
{
    if (!dsc->points) {
        if (dsc->p1.x == dsc->p2.x && dsc->p1.y == dsc->p2.y)
            return 1;
        return gpu_line_segment_in_bounds(task, buffer, dsc, dsc->p1, dsc->p2);
    }
    if (!dsc->point_cnt || !dsc->points)
        return 0;
    for (int32_t i = 0; i + 1 < dsc->point_cnt; i++) {
        lv_point_precise_t p1 = dsc->points[i];
        lv_point_precise_t p2 = dsc->points[i + 1];

        if (p1.x == LV_DRAW_LINE_POINT_NONE ||
            p1.y == LV_DRAW_LINE_POINT_NONE ||
            p2.x == LV_DRAW_LINE_POINT_NONE ||
            p2.y == LV_DRAW_LINE_POINT_NONE)
            continue;
        if (p1.x != p2.x || p1.y != p2.y) {
            if (!gpu_line_segment_in_bounds(task, buffer, dsc, p1, p2))
                return 0;
        }
    }
    return 1;
}

static int gpu_add_line_polys(lv_draw_task_t *task,
                              const struct gpu_buffer *buffer,
                              const lv_draw_line_dsc_t *dsc,
                              struct drm_n32h7xx_poly *polys,
                              uint32_t *count, int32_t *next_point)
{
    if (!dsc->points) {
        int ret;

        if (*next_point)
            return 0;
        ret = gpu_add_line_segment(task, buffer, dsc, dsc->p1, dsc->p2,
                                   &polys[*count]);

        if (ret > 0)
            (*count)++;
        *next_point = 1;
        return ret < 0 ? ret : 0;
    }

    while (*next_point + 1 < dsc->point_cnt) {
        int32_t i = *next_point;
        int ret;

        if (dsc->points[i].x == LV_DRAW_LINE_POINT_NONE ||
            dsc->points[i].y == LV_DRAW_LINE_POINT_NONE ||
            dsc->points[i + 1].x == LV_DRAW_LINE_POINT_NONE ||
            dsc->points[i + 1].y == LV_DRAW_LINE_POINT_NONE) {
            (*next_point)++;
            continue;
        }
        if (*count == DRM_N32H7XX_POLY_BATCH_MAX)
            return 1;
        ret = gpu_add_line_segment(task, buffer, dsc, dsc->points[i],
                                   dsc->points[i + 1], &polys[*count]);
        if (ret < 0)
            return ret;
        if (ret > 0)
            (*count)++;
        (*next_point)++;
    }
    return 0;
}

static int gpu_add_arc_polys(lv_draw_task_t *task,
                             const struct gpu_buffer *buffer,
                             const lv_draw_arc_dsc_t *dsc,
                             struct drm_n32h7xx_poly *polys,
                             uint32_t *count)
{
    int64_t sweep = (int64_t)dsc->end_angle - dsc->start_angle;
    int32_t segments;
    int32_t inner_radius = dsc->radius - dsc->width;
    uint32_t color = lv_color_to_u32(dsc->color);

    while (sweep < 0)
        sweep += 360;
    while (sweep > 360)
        sweep -= 360;
    if (!sweep || inner_radius <= 0)
        return 0;
    segments = (int32_t)((sweep + 5) / 6);
    if (segments < 1)
        segments = 1;
    if (segments > 60)
        segments = 60;

    for (int32_t i = 0; i < segments; i++) {
        int32_t a0 = dsc->start_angle +
                     (int32_t)(sweep * i / segments);
        int32_t a1 = dsc->start_angle +
                     (int32_t)(sweep * (i + 1) / segments);
        int64_t x[4], y[4];
        int ret;

        if (*count == DRM_N32H7XX_POLY_BATCH_MAX)
            return 1;
        x[0] = ((int64_t)dsc->center.x << 16) +
               gpu_trigo_fixed(inner_radius, (int16_t)a0, 0);
        y[0] = ((int64_t)dsc->center.y << 16) +
               gpu_trigo_fixed(inner_radius, (int16_t)a0, 1);
        x[1] = ((int64_t)dsc->center.x << 16) +
               gpu_trigo_fixed(dsc->radius, (int16_t)a0, 0);
        y[1] = ((int64_t)dsc->center.y << 16) +
               gpu_trigo_fixed(dsc->radius, (int16_t)a0, 1);
        x[2] = ((int64_t)dsc->center.x << 16) +
               gpu_trigo_fixed(dsc->radius, (int16_t)a1, 0);
        y[2] = ((int64_t)dsc->center.y << 16) +
               gpu_trigo_fixed(dsc->radius, (int16_t)a1, 1);
        x[3] = ((int64_t)dsc->center.x << 16) +
               gpu_trigo_fixed(inner_radius, (int16_t)a1, 0);
        y[3] = ((int64_t)dsc->center.y << 16) +
               gpu_trigo_fixed(inner_radius, (int16_t)a1, 1);
        ret = gpu_add_fixed_poly(task, buffer, x, y, 4, color,
                                 &polys[*count]);
        if (ret < 0)
            return ret;
        if (ret > 0)
            (*count)++;
    }

    if (dsc->rounded && sweep < 360 && inner_radius > 0) {
        int32_t cap_radius = dsc->width / 2;
        int32_t mid_radius = inner_radius + cap_radius;
        int16_t angles[2] = {
            (int16_t)dsc->start_angle,
            (int16_t)(dsc->start_angle + sweep),
        };

        for (int i = 0; i < 2; i++) {
            lv_point_t center = {
                .x = dsc->center.x +
                     (int32_t)(gpu_trigo_fixed(mid_radius, angles[i], 0) >> 16),
                .y = dsc->center.y +
                     (int32_t)(gpu_trigo_fixed(mid_radius, angles[i], 1) >> 16),
            };
            int ret;

            if (*count == DRM_N32H7XX_POLY_BATCH_MAX)
                return 1;
            ret = gpu_add_disk(task, buffer, center, cap_radius, color,
                               &polys[*count]);
            if (ret < 0)
                return ret;
            if (ret > 0)
                (*count)++;
        }
    }
    return 0;
}

static int32_t gpu_evaluate(lv_draw_unit_t *unit, lv_draw_task_t *task)
{
    gpu_draw_unit_t *gpu = (gpu_draw_unit_t *)unit;
    const struct gpu_buffer *buffer;
    lv_area_t visible;

    if (gpu->backend->disable_acceleration)
        return 0;
    if (!task->target_layer || !task->target_layer->draw_buf ||
        task->target_layer->draw_buf->header.cf != LV_COLOR_FORMAT_RGB565 ||
        task->opa != LV_OPA_COVER)
        return 0;
    buffer = buffer_for_layer(gpu->backend, task->target_layer);
    if (!buffer)
        return 0;
    gpu_visible_area(task, &visible);
    if (visible.x1 > visible.x2 || visible.y1 > visible.y2)
        return 0;

    if (task->type != LV_DRAW_TASK_TYPE_FILL && !gpu_area_in_bounds(task)) {
        lv_area_t clipped;

        /* Small strokes are cheaper in software. Tessellating a clipped arc
         * also costs more CPU than its software path on this platform. */
        if (!buffer->clipping ||
            (task->type != LV_DRAW_TASK_TYPE_LINE &&
             task->type != LV_DRAW_TASK_TYPE_TRIANGLE) ||
            !lv_area_intersect(&clipped, &task->area, &visible) ||
            (int64_t)lv_area_get_width(&clipped) * lv_area_get_height(&clipped) <
                GPU_MIN_CLIPPED_POLY_PIXELS)
            return 0;
        if (task->type == LV_DRAW_TASK_TYPE_LINE) {
            const lv_draw_line_dsc_t *line = task->draw_dsc;

            if (!line->points &&
                (line->p1.x == line->p2.x || line->p1.y == line->p2.y))
                return 0;
        }
    }

    if (task->type == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_fill_dsc_t *fill = task->draw_dsc;
        lv_area_t area;
        int32_t pixels;

        if (fill->grad.dir != LV_GRAD_DIR_NONE ||
            fill->opa != LV_OPA_COVER || fill->radius < 0)
            return 0;
        if (!gpu_fill_rect_area(task, &area) && !gpu_area_in_bounds(task))
            return 0;
        pixels = lv_area_get_width(&area) * lv_area_get_height(&area);
        if (pixels < GPU_MIN_PLAIN_FILL_PIXELS)
            return 0;
        if ((uint32_t)pixels > gpu->backend->max_plain_fill_pixels) {
            gpu->backend->large_fill_fallbacks++;
            return 0;
        }
    }
    else if (task->type == LV_DRAW_TASK_TYPE_BORDER) {
        lv_draw_border_dsc_t *border = task->draw_dsc;
        int32_t limit = LV_MIN(lv_area_get_width(&task->area),
                               lv_area_get_height(&task->area)) / 2;

        if (!border->width || border->opa != LV_OPA_COVER ||
            border->side == LV_BORDER_SIDE_NONE || border->radius < 0 ||
            border->width > limit)
            return 0;
    }
    else if (task->type == LV_DRAW_TASK_TYPE_LINE) {
        lv_draw_line_dsc_t *line = task->draw_dsc;

        if (line->width <= 0 || line->opa != LV_OPA_COVER ||
            line->round_start || line->round_end ||
            (line->dash_width && line->dash_gap) ||
            !gpu_line_in_bounds(task, buffer, line))
            return 0;
    }
    else if (task->type == LV_DRAW_TASK_TYPE_ARC) {
        lv_draw_arc_dsc_t *arc = task->draw_dsc;

        if (arc->opa != LV_OPA_COVER || !arc->width ||
            arc->img_src || arc->width >= arc->radius ||
            arc->start_angle == arc->end_angle ||
            !gpu_geometry_in_bounds(task, buffer,
                               ((int64_t)arc->center.x - arc->radius) << 16,
                               ((int64_t)arc->center.x + arc->radius + 1) << 16,
                               ((int64_t)arc->center.y - arc->radius) << 16,
                               ((int64_t)arc->center.y + arc->radius + 1) << 16))
            return 0;
    }
    else if (task->type == LV_DRAW_TASK_TYPE_TRIANGLE) {
        lv_draw_triangle_dsc_t *triangle = task->draw_dsc;
        int64_t x[3], y[3];

        if (triangle->opa != LV_OPA_COVER ||
            triangle->grad.dir != LV_GRAD_DIR_NONE)
            return 0;
        for (int i = 0; i < 3; i++) {
            x[i] = (int64_t)triangle->p[i].x << 16;
            y[i] = (int64_t)triangle->p[i].y << 16;
        }
        if (!gpu_poly_in_bounds(task, buffer, x, y, 3))
            return 0;
    }
    else {
        return 0;
    }

    task->preferred_draw_unit_id = GPU_DRAW_UNIT_ID;
    task->preference_score = 0;
    return 0;
}

static int32_t gpu_radius_for_area(const lv_area_t *area, int32_t radius)
{
    int32_t limit = LV_MIN(lv_area_get_width(area),
                           lv_area_get_height(area)) / 2;

    if (radius > limit)
        radius = limit;
    return LV_MAX(radius, 0);
}

static int gpu_append_rect(lv_draw_task_t *task,
                           const struct gpu_buffer *buffer,
                           const lv_area_t *area, uint32_t color,
                           struct drm_n32h7xx_rect *rects, uint32_t *count)
{
    int ret;

    if (area->x1 > area->x2 || area->y1 > area->y2)
        return 0;
    if (*count == DRM_N32H7XX_BATCH_MAX)
        return -1;
    ret = gpu_add_rect_primitive(task, buffer, area, color, &rects[*count]);
    if (ret < 0)
        return -1;
    (*count)++;
    return 0;
}

static int gpu_build_rounded_fill(lv_draw_task_t *task,
                                  const struct gpu_buffer *buffer,
                                  const lv_draw_fill_dsc_t *dsc,
                                  struct drm_n32h7xx_rect *rects,
                                  uint32_t *rect_count,
                                  struct drm_n32h7xx_poly *polys,
                                  uint32_t *poly_count)
{
    const lv_area_t *area = &task->area;
    lv_area_t part;
    int32_t radius = gpu_radius_for_area(area, dsc->radius);
    uint32_t color = lv_color_to_u32(dsc->color);
    int ret;

    if (gpu_fill_rect_area(task, &part)) {
        if (gpu_append_rect(task, buffer, &part, color, rects, rect_count) < 0)
            return -1;
        return 0;
    }

    part.x1 = area->x1;
    part.x2 = area->x2;
    part.y1 = area->y1 + radius;
    part.y2 = area->y2 - radius;
    if (gpu_append_rect(task, buffer, &part, color, rects, rect_count) < 0)
        return -1;

    part.x1 = area->x1 + radius;
    part.x2 = area->x2 - radius;
    part.y1 = area->y1;
    part.y2 = area->y1 + radius - 1;
    if (gpu_append_rect(task, buffer, &part, color, rects, rect_count) < 0)
        return -1;
    part.y1 = area->y2 - radius + 1;
    part.y2 = area->y2;
    if (gpu_append_rect(task, buffer, &part, color, rects, rect_count) < 0)
        return -1;

    ret = gpu_add_quarter_disk(task, buffer,
                               (lv_point_t){ area->x1 + radius,
                                             area->y1 + radius },
                               radius, 180, color, &polys[*poly_count]);
    if (ret < 0)
        return -1;
    if (ret > 0)
        (*poly_count)++;
    ret = gpu_add_quarter_disk(task, buffer,
                               (lv_point_t){ area->x2 - radius,
                                             area->y1 + radius },
                               radius, 270, color, &polys[*poly_count]);
    if (ret < 0)
        return -1;
    if (ret > 0)
        (*poly_count)++;
    ret = gpu_add_quarter_disk(task, buffer,
                               (lv_point_t){ area->x2 - radius,
                                             area->y2 - radius },
                               radius, 0, color, &polys[*poly_count]);
    if (ret < 0)
        return -1;
    if (ret > 0)
        (*poly_count)++;
    ret = gpu_add_quarter_disk(task, buffer,
                               (lv_point_t){ area->x1 + radius,
                                             area->y2 - radius },
                               radius, 90, color, &polys[*poly_count]);
    if (ret < 0)
        return -1;
    if (ret > 0)
        (*poly_count)++;
    return 0;
}

static int gpu_build_border(lv_draw_task_t *task,
                            const struct gpu_buffer *buffer,
                            const lv_draw_border_dsc_t *dsc,
                            struct drm_n32h7xx_rect *rects,
                            uint32_t *rect_count,
                            struct drm_n32h7xx_poly *polys,
                            uint32_t *poly_count)
{
    const lv_area_t *area = &task->area;
    lv_area_t part;
    int32_t radius = gpu_radius_for_area(area, dsc->radius);
    int32_t inner_radius = LV_MAX(radius - dsc->width, 0);
    uint32_t color = lv_color_to_u32(dsc->color);
    int top = dsc->side & LV_BORDER_SIDE_TOP;
    int bottom = dsc->side & LV_BORDER_SIDE_BOTTOM;
    int left = dsc->side & LV_BORDER_SIDE_LEFT;
    int right = dsc->side & LV_BORDER_SIDE_RIGHT;

    if (radius) {
        if (top) {
            part = (lv_area_t){ area->x1 + radius, area->y1,
                                area->x2 - radius,
                                area->y1 + dsc->width - 1 };
            if (gpu_append_rect(task, buffer, &part, color,
                                rects, rect_count) < 0)
                return -1;
        }
        if (bottom) {
            part = (lv_area_t){ area->x1 + radius,
                                area->y2 - dsc->width + 1,
                                area->x2 - radius, area->y2 };
            if (gpu_append_rect(task, buffer, &part, color,
                                rects, rect_count) < 0)
                return -1;
        }
        if (left) {
            part = (lv_area_t){ area->x1, area->y1 + radius,
                                area->x1 + dsc->width - 1,
                                area->y2 - radius };
            if (gpu_append_rect(task, buffer, &part, color,
                                rects, rect_count) < 0)
                return -1;
        }
        if (right) {
            part = (lv_area_t){ area->x2 - dsc->width + 1,
                                area->y1 + radius, area->x2,
                                area->y2 - radius };
            if (gpu_append_rect(task, buffer, &part, color,
                                rects, rect_count) < 0)
                return -1;
        }

        if ((left || top) &&
            gpu_add_ring_arc(task, buffer,
                             (lv_point_t){ area->x1 + radius,
                                           area->y1 + radius },
                             radius, inner_radius, 180, 270, color,
                             polys, poly_count) < 0)
            return -1;
        if ((top || right) &&
            gpu_add_ring_arc(task, buffer,
                             (lv_point_t){ area->x2 - radius,
                                           area->y1 + radius },
                             radius, inner_radius, 270, 360, color,
                             polys, poly_count) < 0)
            return -1;
        if ((right || bottom) &&
            gpu_add_ring_arc(task, buffer,
                             (lv_point_t){ area->x2 - radius,
                                           area->y2 - radius },
                             radius, inner_radius, 0, 90, color,
                             polys, poly_count) < 0)
            return -1;
        if ((bottom || left) &&
            gpu_add_ring_arc(task, buffer,
                             (lv_point_t){ area->x1 + radius,
                                           area->y2 - radius },
                             radius, inner_radius, 90, 180, color,
                             polys, poly_count) < 0)
            return -1;
        return 0;
    }

    if (top) {
        part = (lv_area_t){ area->x1, area->y1, area->x2,
                            area->y1 + dsc->width - 1 };
        if (gpu_append_rect(task, buffer, &part, color,
                            rects, rect_count) < 0)
            return -1;
    }
    if (bottom) {
        part = (lv_area_t){ area->x1, area->y2 - dsc->width + 1,
                            area->x2, area->y2 };
        if (gpu_append_rect(task, buffer, &part, color,
                            rects, rect_count) < 0)
            return -1;
    }
    if (left) {
        part = (lv_area_t){ area->x1,
                            top ? area->y1 + dsc->width : area->y1,
                            area->x1 + dsc->width - 1,
                            bottom ? area->y2 - dsc->width : area->y2 };
        if (gpu_append_rect(task, buffer, &part, color,
                            rects, rect_count) < 0)
            return -1;
    }
    if (right) {
        part = (lv_area_t){ area->x2 - dsc->width + 1,
                            top ? area->y1 + dsc->width : area->y1,
                            area->x2,
                            bottom ? area->y2 - dsc->width : area->y2 };
        if (gpu_append_rect(task, buffer, &part, color,
                            rects, rect_count) < 0)
            return -1;
    }
    return 0;
}

static bool gpu_same_clip(const lv_draw_task_t *a, const lv_draw_task_t *b)
{
    lv_area_t x, y;

    gpu_visible_area(a, &x);
    gpu_visible_area(b, &y);
    return x.x1 == y.x1 && x.x2 == y.x2 && x.y1 == y.y1 && x.y2 == y.y2 &&
           a->target_layer->buf_area.x1 == b->target_layer->buf_area.x1 &&
           a->target_layer->buf_area.y1 == b->target_layer->buf_area.y1;
}

static int32_t gpu_dispatch_rect_tasks(gpu_draw_unit_t *gpu,
                                       lv_draw_unit_t *unit,
                                       lv_layer_t *layer,
                                       lv_draw_task_t *task)
{
    lv_draw_task_t *tasks[DRM_N32H7XX_BATCH_MAX];
    struct drm_n32h7xx_rect rects[DRM_N32H7XX_BATCH_MAX];
    struct drm_n32h7xx_poly polys[DRM_N32H7XX_POLY_BATCH_MAX];
    const struct gpu_buffer *buffer = NULL;
    uint32_t task_count = 0;
    uint32_t rect_count = 0;
    uint32_t poly_count = 0;
    int err = 0;

    while (task && task_count < DRM_N32H7XX_BATCH_MAX) {
        struct drm_n32h7xx_rect task_rects[4];
        struct drm_n32h7xx_poly task_polys[DRM_N32H7XX_POLY_BATCH_MAX];
        const struct gpu_buffer *candidate;
        uint32_t task_rect_count = 0;
        uint32_t task_poly_count = 0;
        int ret;

        if (task->type != LV_DRAW_TASK_TYPE_FILL &&
            task->type != LV_DRAW_TASK_TYPE_BORDER) {
            task = lv_draw_get_next_available_task(layer, task,
                                                   GPU_DRAW_UNIT_ID);
            continue;
        }
        if (gpu->backend->clipping && task_count &&
            !gpu_same_clip(task, tasks[0]))
            break;
        candidate = buffer_for_layer(gpu->backend, task->target_layer);
        if (!candidate || (buffer && candidate->handle != buffer->handle))
            break;

        if (task->type == LV_DRAW_TASK_TYPE_FILL)
            ret = gpu_build_rounded_fill(task, candidate, task->draw_dsc,
                                         task_rects, &task_rect_count,
                                         task_polys, &task_poly_count);
        else
            ret = gpu_build_border(task, candidate, task->draw_dsc,
                                   task_rects, &task_rect_count,
                                   task_polys, &task_poly_count);
        if (ret < 0)
            break;
        if (rect_count + task_rect_count > DRM_N32H7XX_BATCH_MAX ||
            poly_count + task_poly_count > DRM_N32H7XX_POLY_BATCH_MAX)
            break;

        memcpy(&rects[rect_count], task_rects,
               task_rect_count * sizeof(task_rects[0]));
        memcpy(&polys[poly_count], task_polys,
               task_poly_count * sizeof(task_polys[0]));
        rect_count += task_rect_count;
        poly_count += task_poly_count;
        task->draw_unit = unit;
        task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
        tasks[task_count++] = task;
        if (!buffer)
            buffer = candidate;
        task = lv_draw_get_next_available_task(layer, task, GPU_DRAW_UNIT_ID);
    }

    if (!task_count)
        return LV_DRAW_UNIT_IDLE;

    if (task_count == 1 && tasks[0]->type == LV_DRAW_TASK_TYPE_FILL && rect_count == 1 &&
        !poly_count) {
        struct drm_n32h7xx_fill fill = {
            .handle = rects[0].handle,
            .format = rects[0].format,
            .offset = rects[0].offset,
            .pitch = rects[0].pitch,
            .width = rects[0].width,
            .height = rects[0].height,
            .color = lv_color_to_u16(
                ((lv_draw_fill_dsc_t *)tasks[0]->draw_dsc)->color),
        };

        if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_FILL, &fill,
                      GPU_PROFILE_FILL))
            err = errno;
        else
            gpu->backend->batches++;
    }
    else if (gpu_flush_rects(gpu, rects, rect_count)) {
        err = errno;
    }
    if (!err && gpu_flush_polys(gpu, tasks[0], polys, poly_count))
        err = errno;
    if (err) {
        if (err == EINVAL)
            fprintf(stderr, "lvgl-gpu: invalid rectangle geometry\n");
        else
            perror("lvgl-gpu: rectangle task");
    }
    for (uint32_t i = 0; i < task_count; i++) {
        tasks[i]->state = err ? LV_DRAW_TASK_STATE_FAILED :
                                LV_DRAW_TASK_STATE_FINISHED;
        if (!err) {
            if (tasks[i]->type == LV_DRAW_TASK_TYPE_FILL)
                gpu->backend->fills++;
            else
                gpu->backend->borders++;
        }
    }
    lv_draw_dispatch_request();
    return 1;
}

static int32_t gpu_dispatch_fill(gpu_draw_unit_t *gpu, lv_draw_unit_t *unit,
                                 lv_layer_t *layer, lv_draw_task_t *task)
{
    lv_draw_task_t *tasks[DRM_N32H7XX_BATCH_MAX];
    struct drm_n32h7xx_rect rects[DRM_N32H7XX_BATCH_MAX];
    struct drm_n32h7xx_rect_batch batch;
    const struct gpu_buffer *buffer = NULL;
    uint32_t count = 0;
    uint32_t handle = 0;
    int err = 0;

    while (task && count < DRM_N32H7XX_BATCH_MAX &&
           task->type == LV_DRAW_TASK_TYPE_FILL &&
           task->preferred_draw_unit_id == GPU_DRAW_UNIT_ID) {
        lv_draw_fill_dsc_t *description = task->draw_dsc;
        const struct gpu_buffer *candidate;
        lv_area_t area;
        int x;
        int y;

        if (!gpu_fill_rect_area(task, &area))
            break;
        candidate = buffer_for_layer(gpu->backend, task->target_layer);
        if (!candidate || (buffer && candidate->handle != handle))
            break;
        x = area.x1 - task->target_layer->buf_area.x1;
        y = area.y1 - task->target_layer->buf_area.y1;
        if (x < 0 || y < 0)
            break;

        rects[count].handle = candidate->handle;
        rects[count].format = DRM_FORMAT_RGB565;
        rects[count].offset = (uint32_t)y * candidate->pitch + (uint32_t)x * 2U;
        rects[count].pitch = candidate->pitch;
        rects[count].width = (uint32_t)lv_area_get_width(&area);
        rects[count].height = (uint32_t)lv_area_get_height(&area);
        rects[count].color = lv_color_to_u32(description->color);
        rects[count].flags = 0;
        tasks[count] = task;
        count++;
        if (!buffer) {
            buffer = candidate;
            handle = candidate->handle;
        }
        task->draw_unit = unit;
        task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
        task = lv_draw_get_next_available_task(layer, task, GPU_DRAW_UNIT_ID);
    }

    if (!count)
        return LV_DRAW_UNIT_IDLE;

    if (count == 1) {
        struct drm_n32h7xx_fill fill = {
            .handle = rects[0].handle,
            .format = rects[0].format,
            .offset = rects[0].offset,
            .pitch = rects[0].pitch,
            .width = rects[0].width,
            .height = rects[0].height,
            .color = lv_color_to_u16(
                ((lv_draw_fill_dsc_t *)tasks[0]->draw_dsc)->color),
        };

        if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_FILL, &fill,
                      GPU_PROFILE_FILL))
            err = errno;
        else
            gpu->backend->batches++;
    }
    else {
        batch.handle = rects[0].handle;
        batch.count = count;
        batch.rects = (uint64_t)(uintptr_t)rects;
        batch.reserved = 0;
        batch.pad = 0;
        if (gpu_ioctl(gpu->backend, DRM_IOCTL_N32H7XX_RECT_BATCH, &batch,
                      GPU_PROFILE_RECT))
            err = errno;
        else
            gpu->backend->batches++;
    }
    if (err)
        perror("lvgl-gpu: fill");
    for (uint32_t i = 0; i < count; i++)
        tasks[i]->state = err ? LV_DRAW_TASK_STATE_FAILED :
                                LV_DRAW_TASK_STATE_FINISHED;
    if (!err)
        gpu->backend->fills += count;
    lv_draw_dispatch_request();
    return 1;
}

static int32_t gpu_dispatch_poly_task(gpu_draw_unit_t *gpu,
                                      lv_draw_unit_t *unit,
                                      lv_draw_task_t *task)
{
    struct drm_n32h7xx_poly polys[DRM_N32H7XX_POLY_BATCH_MAX];
    const struct gpu_buffer *buffer = buffer_for_layer(gpu->backend,
                                                       task->target_layer);
    uint32_t count = 0;
    int32_t next_point = 0;
    int err = 0;

    task->draw_unit = unit;
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    if (task->type == LV_DRAW_TASK_TYPE_LINE) {
        lv_draw_line_dsc_t *dsc = task->draw_dsc;

        while (1) {
            int ret = gpu_add_line_polys(task, buffer, dsc, polys, &count,
                                         &next_point);

            if (ret < 0)
                err = EINVAL;
            if (ret <= 0)
                break;
            if (gpu_flush_polys(gpu, task, polys, count)) {
                err = errno;
                break;
            }
            count = 0;
        }
    }
    else if (task->type == LV_DRAW_TASK_TYPE_ARC) {
        lv_draw_arc_dsc_t *dsc = task->draw_dsc;
        int ret = gpu_add_arc_polys(task, buffer, dsc, polys, &count);

        if (ret < 0)
            err = EINVAL;
    }
    else {
        lv_draw_triangle_dsc_t *dsc = task->draw_dsc;
        int64_t x[3], y[3];
        int ret;

        for (int i = 0; i < 3; i++) {
            x[i] = (int64_t)dsc->p[i].x << 16;
            y[i] = (int64_t)dsc->p[i].y << 16;
        }
        ret = gpu_add_fixed_poly(task, buffer, x, y, 3,
                                 lv_color_to_u32(dsc->color), &polys[0]);
        if (ret > 0)
            count = 1;
        else if (ret < 0)
            err = EINVAL;
    }
    if (!err && gpu_flush_polys(gpu, task, polys, count))
        err = errno;
    if (err)
        perror("lvgl-gpu: polygon");
    task->state = err ? LV_DRAW_TASK_STATE_FAILED : LV_DRAW_TASK_STATE_FINISHED;
    if (!err) {
        if (task->type == LV_DRAW_TASK_TYPE_LINE)
            gpu->backend->lines++;
        else if (task->type == LV_DRAW_TASK_TYPE_ARC)
            gpu->backend->arcs++;
        else
            gpu->backend->triangles++;
    }
    lv_draw_dispatch_request();
    return 1;
}

static int32_t gpu_dispatch_poly_tasks(gpu_draw_unit_t *gpu,
                                       lv_draw_unit_t *unit,
                                       lv_layer_t *layer,
                                       lv_draw_task_t *task)
{
    lv_draw_task_t *tasks[DRM_N32H7XX_POLY_BATCH_MAX];
    struct drm_n32h7xx_poly polys[DRM_N32H7XX_POLY_BATCH_MAX];
    const struct gpu_buffer *buffer = NULL;
    uint32_t task_count = 0;
    uint32_t poly_count = 0;
    int err = 0;

    while (task && task_count < DRM_N32H7XX_POLY_BATCH_MAX) {
        struct drm_n32h7xx_poly task_polys[DRM_N32H7XX_POLY_BATCH_MAX];
        const struct gpu_buffer *candidate;
        uint32_t task_poly_count = 0;
        int ret = 0;

        if (task->type != LV_DRAW_TASK_TYPE_LINE &&
            task->type != LV_DRAW_TASK_TYPE_ARC &&
            task->type != LV_DRAW_TASK_TYPE_TRIANGLE) {
            task = lv_draw_get_next_available_task(layer, task,
                                                   GPU_DRAW_UNIT_ID);
            continue;
        }
        if (gpu->backend->clipping && task_count &&
            !gpu_same_clip(task, tasks[0]))
            break;
        candidate = buffer_for_layer(gpu->backend, task->target_layer);
        if (!candidate || (buffer && candidate->handle != buffer->handle))
            break;

        if (task->type == LV_DRAW_TASK_TYPE_LINE) {
            lv_draw_line_dsc_t *dsc = task->draw_dsc;
            int32_t next_point = 0;

            ret = gpu_add_line_polys(task, candidate, dsc, task_polys,
                                     &task_poly_count, &next_point);
            if (ret > 0)
                break; /* Long polylines stay in the single-task path. */
        }
        else if (task->type == LV_DRAW_TASK_TYPE_ARC) {
            lv_draw_arc_dsc_t *dsc = task->draw_dsc;

            ret = gpu_add_arc_polys(task, candidate, dsc, task_polys,
                                    &task_poly_count);
            if (ret > 0)
                break;
        }
        else {
            lv_draw_triangle_dsc_t *dsc = task->draw_dsc;
            int64_t x[3], y[3];

            for (int i = 0; i < 3; i++) {
                x[i] = (int64_t)dsc->p[i].x << 16;
                y[i] = (int64_t)dsc->p[i].y << 16;
            }
            ret = gpu_add_fixed_poly(task, candidate, x, y, 3,
                                     lv_color_to_u32(dsc->color),
                                     &task_polys[0]);
            if (ret > 0)
                task_poly_count = 1;
        }
        if (ret < 0)
            break;
        if (poly_count + task_poly_count > DRM_N32H7XX_POLY_BATCH_MAX)
            break;

        memcpy(&polys[poly_count], task_polys,
               task_poly_count * sizeof(task_polys[0]));
        poly_count += task_poly_count;
        task->draw_unit = unit;
        task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
        tasks[task_count++] = task;
        if (!buffer)
            buffer = candidate;
        task = lv_draw_get_next_available_task(layer, task, GPU_DRAW_UNIT_ID);
    }

    if (!task_count)
        return gpu_dispatch_poly_task(gpu, unit, task);
    if (gpu_flush_polys(gpu, tasks[0], polys, poly_count))
        err = errno;
    if (err)
        perror("lvgl-gpu: polygon batch");
    for (uint32_t i = 0; i < task_count; i++) {
        tasks[i]->state = err ? LV_DRAW_TASK_STATE_FAILED :
                                LV_DRAW_TASK_STATE_FINISHED;
        if (!err) {
            if (tasks[i]->type == LV_DRAW_TASK_TYPE_LINE)
                gpu->backend->lines++;
            else if (tasks[i]->type == LV_DRAW_TASK_TYPE_ARC)
                gpu->backend->arcs++;
            else
                gpu->backend->triangles++;
        }
    }
    lv_draw_dispatch_request();
    return 1;
}

static int32_t gpu_dispatch(lv_draw_unit_t *unit, lv_layer_t *layer)
{
    gpu_draw_unit_t *gpu = (gpu_draw_unit_t *)unit;
    lv_draw_task_t *task;

    task = lv_draw_get_next_available_task(layer, NULL, GPU_DRAW_UNIT_ID);
    if (!task || task->preferred_draw_unit_id != GPU_DRAW_UNIT_ID)
        return LV_DRAW_UNIT_IDLE;
    if (task->type == LV_DRAW_TASK_TYPE_FILL) {
        lv_area_t area;

        if (gpu_fill_rect_area(task, &area))
            return gpu_dispatch_fill(gpu, unit, layer, task);
    }
    if (task->type == LV_DRAW_TASK_TYPE_FILL ||
        task->type == LV_DRAW_TASK_TYPE_BORDER)
        return gpu_dispatch_rect_tasks(gpu, unit, layer, task);
    return gpu_dispatch_poly_tasks(gpu, unit, layer, task);
}

lvgl_gpu_backend_t *lvgl_gpu_display_create(lv_display_t **display,
                                            const char *lcdc_node,
                                            const char *gpu_node)
{
    lvgl_gpu_backend_t *backend;
    char lcdc_version_name[32] = { 0 };
    char version_name[32] = { 0 };
    struct drm_version lcdc_version = { 0 };
    struct drm_version version = { 0 };
    gpu_draw_unit_t *unit;
    lv_display_t *disp;

    backend = calloc(1, sizeof(*backend));
    if (!backend)
        return NULL;
    backend->lcdc_fd = -1;
    backend->gpu_fd = -1;
    backend->pending_buffer = -1;
    backend->max_plain_fill_pixels = GPU_MAX_PLAIN_FILL_PIXELS;
    backend->disable_acceleration = getenv("LVGL_GPU_DISABLE_ACCEL") != NULL;
    {
        const char *value = getenv("LVGL_GPU_MAX_FILL_PIXELS");

        if (value && *value) {
            unsigned long parsed = strtoul(value, NULL, 0);

            if (parsed >= GPU_MIN_PLAIN_FILL_PIXELS && parsed <= UINT_MAX)
                backend->max_plain_fill_pixels = (uint32_t)parsed;
        }
    }

    backend->lcdc_fd = open(lcdc_node, O_RDWR | O_CLOEXEC);
    backend->gpu_fd = open(gpu_node, O_RDWR | O_CLOEXEC);
    if (backend->lcdc_fd < 0 || backend->gpu_fd < 0) {
        perror("lvgl-gpu: open");
        goto fail;
    }
    snprintf(backend->lcdc_node, sizeof(backend->lcdc_node), "%s", lcdc_node);
    snprintf(backend->gpu_node, sizeof(backend->gpu_node), "%s", gpu_node);

    lv_tick_set_cb(tick_get_cb);

    lcdc_version.name = lcdc_version_name;
    lcdc_version.name_len = sizeof(lcdc_version_name) - 1;
    version.name = version_name;
    version.name_len = sizeof(version_name) - 1;
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_VERSION, &lcdc_version) ||
        strcmp(lcdc_version_name, "n32h7xx-lcdc")) {
        fprintf(stderr, "lvgl-gpu: %s is not the LCDC node\n", lcdc_node);
        goto fail;
    }
    if (ioctl(backend->gpu_fd, DRM_IOCTL_VERSION, &version) ||
        strcmp(version_name, "n32h7xx-gpu")) {
        fprintf(stderr, "lvgl-gpu: %s is not the GPU node\n", gpu_node);
        goto fail;
    }
    if (get_crtc_state(backend))
        goto fail;

    if (import_existing_buffer(backend, &backend->buffers[0],
                               backend->old_fb)) {
        fprintf(stderr, "lvgl-gpu: active framebuffer import failed\n");
        goto fail;
    }
    backend->scanout_buffer = 0;
    if (allocate_frame_buffer(backend, &backend->buffers[1])) {
        fprintf(stderr, "lvgl-gpu: frame buffer allocation failed\n");
        goto fail;
    }
    if (!backend->disable_acceleration &&
        (!getenv("LVGL_GPU_SYNC") || strcmp(getenv("LVGL_GPU_SYNC"), "cpu"))) {
        int supported = probe_gpu_sync(backend);

        if (supported < 0) {
            fprintf(stderr, "lvgl-gpu: sync probe failed\n");
            goto fail;
        }
        backend->gpu_sync = supported > 0;
    }
    if (!backend->disable_acceleration) {
        int supported = probe_poly_clip(backend);

        if (supported < 0)
            goto fail;
        backend->clipping = supported > 0;
        for (unsigned int i = 0; i < GPU_BUFFER_COUNT; i++)
            backend->buffers[i].clipping = backend->clipping;
    }
    if (import_buffer_to_lcdc(backend, &backend->buffers[1])) {
        fprintf(stderr, "lvgl-gpu: frame buffer import failed\n");
        goto fail;
    }
    if (ioctl(backend->lcdc_fd, DRM_IOCTL_SET_MASTER, 0)) {
        perror("lvgl-gpu: SET_MASTER");
        goto fail;
    }
    backend->master = true;
    if (flip_and_wait(backend, backend->buffers[1].fb_id)) {
        /* fbdev normally activates the CRTC when the UI unblanks fb0.  If
         * another client left it inactive, legacy page flip is rejected by
         * DRM before reaching the driver, so restore the saved modeset once. */
        if (activate_crtc(backend) ||
            flip_and_wait(backend, backend->buffers[1].fb_id))
            goto fail;
    }
    backend->scanout_buffer = 1;

    disp = lv_display_create(backend->width, backend->height);
    if (!disp)
        goto fail;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers_with_stride(disp, backend->buffers[0].map,
                                       backend->buffers[1].map,
                                       (uint32_t)backend->buffers[0].size,
                                       backend->buffers[0].pitch,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_driver_data(disp, backend);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_flush_wait_cb(disp, flush_wait_cb);

    unit = (gpu_draw_unit_t *)lv_draw_create_unit(sizeof(*unit));
    if (!unit)
        goto fail;
    unit->base.idx = GPU_DRAW_UNIT_ID;
    unit->base.name = "N32H787_GPU";
    unit->base.evaluate_cb = gpu_evaluate;
    unit->base.dispatch_cb = gpu_dispatch;
    unit->backend = backend;

    backend->profile_enabled = getenv("LVGL_GPU_PROFILE") != NULL;
    if (backend->profile_enabled) {
        struct timespec ts;

        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) {
            fprintf(stderr, "gpu-profile: thread CPU clock unavailable; "
                    "profiling disabled\n");
            backend->profile_enabled = false;
        }
        else if (!clock_getres(CLOCK_THREAD_CPUTIME_ID, &ts)) {
            backend->profile_cpu_resolution_ns =
                (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        }
    }
    if (backend->gpu_sync || backend->profile_enabled)
        lv_display_set_sync_cb(disp, sync_cb);
    fprintf(stderr, "lvgl-gpu: %s buffer sync, polygon clipping %s\n",
            backend->gpu_sync ? "GPU" : "CPU", backend->clipping ? "on" : "off");
    if (backend->profile_enabled) {
        lv_display_add_event_cb(disp, profile_display_event,
                                 LV_EVENT_ALL, backend);
    }
    *display = disp;
    return backend;

fail:
    lvgl_gpu_display_destroy(backend);
    return NULL;
}

void lvgl_gpu_display_destroy(lvgl_gpu_backend_t *backend)
{
    struct drm_gem_close close_gem = { 0 };

    if (!backend)
        return;

    finish_pending_flip(backend);
    profile_dump(backend);
    if (backend->master && backend->scanout_buffer != 0 &&
        backend->buffers[0].fb_id)
        flip_and_wait(backend, backend->buffers[0].fb_id);

    for (int i = GPU_BUFFER_COUNT - 1; i >= 0; i--) {
        struct gpu_buffer *buffer = &backend->buffers[i];

        if (backend->lcdc_fd >= 0 && buffer->fb_id && buffer->owns_fb)
            ioctl(backend->lcdc_fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id);
        if (backend->lcdc_fd >= 0 && buffer->lcdc_handle) {
            close_gem.handle = buffer->lcdc_handle;
            ioctl(backend->lcdc_fd, DRM_IOCTL_GEM_CLOSE, &close_gem);
        }
        if (buffer->map)
            munmap(buffer->map, buffer->size);
        if (backend->gpu_fd >= 0 && buffer->handle) {
            close_gem.handle = buffer->handle;
            ioctl(backend->gpu_fd, DRM_IOCTL_GEM_CLOSE, &close_gem);
        }
    }

    if (backend->lcdc_fd >= 0 && backend->master)
        ioctl(backend->lcdc_fd, DRM_IOCTL_DROP_MASTER, 0);
    if (backend->lcdc_fd >= 0)
        close(backend->lcdc_fd);
    if (backend->gpu_fd >= 0)
        close(backend->gpu_fd);
    free(backend);
}

const char *lvgl_gpu_lcdc_device(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->lcdc_node : NULL;
}

const char *lvgl_gpu_gpu_device(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->gpu_node : NULL;
}

uint32_t lvgl_gpu_fill_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->fills : 0;
}

uint32_t lvgl_gpu_batch_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->batches : 0;
}

uint32_t lvgl_gpu_polygon_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->polygons : 0;
}

uint32_t lvgl_gpu_poly_batch_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->poly_batches : 0;
}

uint32_t lvgl_gpu_line_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->lines : 0;
}

uint32_t lvgl_gpu_arc_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->arcs : 0;
}

uint32_t lvgl_gpu_triangle_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->triangles : 0;
}

uint32_t lvgl_gpu_border_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->borders : 0;
}

uint32_t lvgl_gpu_present_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->presents : 0;
}

uint32_t lvgl_gpu_large_fill_fallback_count(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->large_fill_fallbacks : 0;
}

uint32_t lvgl_gpu_max_fill_pixels(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->max_plain_fill_pixels : 0;
}

int lvgl_gpu_error(const lvgl_gpu_backend_t *backend)
{
    return backend ? backend->error : 0;
}
