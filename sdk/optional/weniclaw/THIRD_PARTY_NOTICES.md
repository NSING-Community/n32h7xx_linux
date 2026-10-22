# Third-party notices — WeniClaw add-on

This directory distributes the prebuilt ARM `weniclaw` binary. The WeniClaw
code itself is BSD-3-Clause; see `LICENSE`.

The agent, including its `<think>` block filtering, is project code. No
third-party parser source is vendored or adapted.

## Linked into the binary

- Zig 0.16.0 standard-library/compiler runtime, Copyright (c) Zig
  contributors, MIT. Preserve it in binary packages; full text in
  `licenses/Zig-MIT.txt`. Upstream: <https://ziglang.org/>.
- The Zig Linux C declarations used in the generated C originate from bundled
  musl headers. Preserve the musl COPYRIGHT notice; full text in
  `licenses/musl-headers-COPYRIGHT.txt`.
- libcurl, dynamically linked from the SDK rootfs, copyright Daniel Stenberg
  and contributors, curl license. Full text in `licenses/curl-COPYING.txt`,
  upstream: <https://curl.se/>.

## Not redistributed here

- The board's uClibc runtime (`libc.so.0`), libcurl (`libcurl.so.4`) and the
  Mbed TLS shared libraries (`libmbedtls.so.21`, `libmbedx509.so.7`,
  `libmbedcrypto.so.16`) are part of the SDK rootfs, not of this add-on.
  uClibc retains its LGPL grant; this build takes Mbed TLS under its
  Apache-2.0 option and the rootfs carries those notices. The SDK legal
  companion covers both.
- The WeChat text channel follows Tencent's published
  [protocol documentation](https://github.com/Tencent/openclaw-weixin/blob/874637a0dbc24050f72d1385add4fc6170a2d726/docs/protocol_zh_CN.md)
  and is independently implemented; no Tencent plugin source or assets are
  included.

WeniClaw additions use the BSD-3-Clause license in `LICENSE`, copyright 2026
Nsing Technologies Inc.
