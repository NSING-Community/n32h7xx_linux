# LVGL 9.6.0

LVGL v9.6.0 for the N32H787 Linux framebuffer and DRM/KMS display paths.

The source under `upstream/` is the upstream tag `v9.6.0` (commit
`80ca777e37a2b176770726a02e07a6fb79ef0b39`) without patches. `lv_conf.h` is
the board configuration: RGB565, LVGL's C library allocator, the Linux fbdev
driver and the Linux DRM driver are enabled, while optional decoders and
widgets that are not needed are disabled.

The DRM path is the standard upstream `lv_linux_drm.c` implementation. It uses
dumb buffers through the normal libdrm API and atomic KMS commits; it does not
add a private DRM wrapper, patch LVGL, or access DRM ioctls directly. The
fbdev path remains available as a fallback.

The current backend is CPU-rendered. The display path is accelerated by the
LCDC scanout, but LVGL drawing is not submitted to the board's 2D GPU. GPU
draw units are a separate follow-up and should use the same standard DRM
interfaces.

## Build

libdrm must be built first; the LVGL Makefile also invokes its build when the
staged library is missing. From the repository root:

```sh
make lvgl-demo
```

This produces `out/lvgl/lvgl-demo` and `out/lvgl/liblvgl.a`. The program links
the shared `libdrm.so.2` staged by the libdrm build, so install the runtime
library for board use with:

```sh
make install-libdrm
make rootfs-squashfs
```

The `out/` directory is generated and is not part of the source release.

## Run

Use the LCDC DRM card for the standard DRM path:

```sh
./lvgl-demo -d /dev/dri/card0 -t 20
```

Use the framebuffer compatibility path instead:

```sh
./lvgl-demo -d /dev/fb0 -t 20
```

`-r milliseconds` changes the refresh period (`0` keeps LVGL's default),
`-s` forces a full-screen repaint on every tick, and `-t 0` runs until
interrupted. The program prints render FPS, flush rate, RSS and CPU time.

The DRM backend allocates two full-screen dumb buffers. On this NOMMU board
that is two 800x480 RGB565 mappings, each rounded to a 1 MiB contiguous block
(order-8), so stop other display applications and leave enough contiguous
memory before starting it. A low-memory run reports
`DRM_IOCTL_MODE_CREATE_DUMB fail` and `DRM buffer allocation failed`, then
exits without the display being changed.

## Baseline

On the tested 800x480 RGB565 panel the CPU-drawn demo reached roughly 33-39
FPS for the normal animated UI at a 16 ms refresh period. Full-screen redraws
with `-s -r 5` reached roughly 16-20 FPS on fbdev and 25-32 FPS on DRM. These
are smoke-test figures, not a benchmark, and depend on the screen, refresh
setting and available memory.

The demo's text and event loop are application code, not part of LVGL upstream.
The board has no touch or keyboard device bound by this example; input support
must be configured separately.
