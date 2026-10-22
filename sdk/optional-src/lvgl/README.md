# LVGL 9.6.0

LVGL v9.6.0 for the N32H787 Linux framebuffer and DRM/KMS display paths.

The source under `upstream/` is the upstream tag `v9.6.0` (commit
`80ca777e37a2b176770726a02e07a6fb79ef0b39`) without patches. `lv_conf.h` is
the board configuration: RGB565, LVGL's C library allocator, the Linux fbdev
driver and the Linux DRM driver are enabled, while optional decoders and
widgets that are not needed are disabled.

The standard DRM path uses the upstream `lv_linux_drm.c` implementation with
dumb buffers and atomic KMS commits. The fbdev path remains available as a
fallback. The example also includes an LVGL draw unit for the board's 2D GPU:
it renders into two full-screen GPU buffers, imports them into LCDC with PRIME,
and presents completed frames with KMS page flips. Text, gradients, rounded
fills and other unsupported tasks continue to use LVGL's software draw unit.
GPU auto-selection is the default when both drivers are available; pass
`-g none` to force software drawing. The LVGL source tree is not patched.

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
out/lvgl/lvgl-demo -d /dev/dri/card0 -t 20
```

Use the framebuffer compatibility path instead:

```sh
out/lvgl/lvgl-demo -d /dev/fb0 -t 20
```

GPU auto-selection is the default when no display or GPU option is supplied:

```sh
out/lvgl/lvgl-demo -i none -t 20 -r 16
```

Use explicit LCDC and GPU DRM cards for a fixed-device test:

```sh
out/lvgl/lvgl-demo -d /dev/dri/card0 -g /dev/dri/card1 \
    -i none -t 20 -r 16
```

Use `-g none` to disable GPU acceleration and verify the software path.

With a Linux input touchscreen:

```sh
out/lvgl/lvgl-demo -d /dev/fb0 -i /dev/input/event0 -t 20
```

`-i /dev/input/event0` is also the default. If the node does not exist, the
demo keeps displaying but reports that the pointer is unavailable. Use
`-i none` to disable input explicitly.

`-r milliseconds` changes the refresh period (`0` keeps LVGL's default),
`-s` forces a full-screen repaint on every tick, and `-t 0` runs until
interrupted. The program prints render FPS, flush rate, RSS and CPU time.

The DRM and GPU paths each need two 800x480 RGB565 buffers. The GPU path also
imports the active LCDC framebuffer, so its three scanout/app buffers fit in the
reserved 3 MiB graphics pool. Stop other display applications before starting
the demo. A failed allocation exits without changing the active framebuffer.

## Baseline

On the tested 800x480 RGB565 panel, a 20-second run reported about 29 FPS for
the normal animated UI with either fbdev CPU drawing or the GPU draw unit. The
GPU path is useful for large opaque fills: `-s -r 5` reported about 10 FPS and
7.2 seconds of user CPU time, versus about 7.3 FPS and 18.6 seconds for the
fbdev CPU path. Small controls still run mostly on the software draw unit, so
the GPU backend does not automatically increase every UI's frame rate.

The demo's text and event loop are application code, not part of LVGL upstream.
The default input device is `/dev/input/event0`; use `-i none` when no
touchscreen or keyboard is available.
