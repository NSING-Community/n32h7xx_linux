# libdrm 2.4.134

Upstream [libdrm](https://gitlab.freedesktop.org/mesa/drm) 2.4.134, used by
the LVGL DRM display backend and by other applications that need the standard
DRM/KMS userspace API.

The source under `upstream/` is the release tarball
`libdrm-2.4.134.tar.xz` unpacked without changes:

```text
https://dri.freedesktop.org/libdrm/libdrm-2.4.134.tar.xz
sha256 ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95
```

There are no patches. `Makefile` only generates a Meson cross file outside the
source tree and enables the core library; the vendor-specific DRM helpers,
tests and man pages are disabled because this board does not use them.

## Build

Install Meson, Ninja and Python 3 on the build host, then run from the
repository root:

```sh
make libdrm
```

The staged result is:

```text
out/libdrm/rootfs/usr/include/libdrm/   public headers
out/libdrm/rootfs/usr/lib/libdrm.so*    shared library
out/libdrm/rootfs/usr/lib/pkgconfig/    libdrm.pc
```

To put the runtime library into the board image tree and repack the rootfs:

```sh
make install-libdrm
make rootfs-squashfs
```

The installed SONAME is `libdrm.so.2`. Applications can link with the normal
`-ldrm` and include `<xf86drm.h>`, `<xf86drmMode.h>` and `<drm_fourcc.h>` from
the staged header directory.

## Licence

libdrm is distributed under the MIT licence. Copyright notices and the full
permission text are in the SPDX headers of the upstream source files. The
release's third-party notices must identify libdrm 2.4.134 and this source
location when a binary linked against it is redistributed.
