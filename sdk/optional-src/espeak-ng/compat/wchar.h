/* SPDX-License-Identifier: BSD-3-Clause
 *
 * <wchar.h> for the SDK's uClibc-ng, which is built without
 * __UCLIBC_HAS_WCHAR__: the sysroot has no <wchar.h> at all, and none of the
 * wcs* functions exist.  wchar_t itself is fine -- GCC defines it in its own
 * <stddef.h>, independently of libc -- and espeak-ng never calls the
 * multi-byte conversion functions (mbrtowc/mbstowcs/wcrtomb); it does its own
 * UTF-8 decoding in encoding.c.
 *
 * What is left is the handful of wide string helpers below, implemented in
 * wchar_shim.c as plain loops.
 *
 * This file is found *before* upstream's src/include/compat/wchar.h, which is
 * only a `#include_next <wchar.h>` wrapper around a system header that a
 * normal libc provides.  See the include-order note in the Makefile.  Because
 * upstream's wrapper is therefore never processed on the target, the
 * <ucd/ucd.h> include it performs has to be repeated here: some sources rely
 * on getting the ucd prototypes transitively through <wchar.h>.
 */

#ifndef ESPEAK_NG_COMPAT_WCHAR_H
#define ESPEAK_NG_COMPAT_WCHAR_H

#include <stddef.h>

#include <ucd/ucd.h>

/* GCC knows the type it uses for wint_t; take it from the compiler exactly
 * like glibc and uClibc do, so this matches whatever libc would have used. */
typedef __WINT_TYPE__ wint_t;

#ifndef WEOF
#define WEOF ((wint_t)0xffffffffu)
#endif

#ifdef __cplusplus
extern "C" {
#endif

size_t wcslen(const wchar_t *s);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
double wcstod(const wchar_t *s, wchar_t **endp);

#ifdef __cplusplus
}
#endif

#endif /* ESPEAK_NG_COMPAT_WCHAR_H */
