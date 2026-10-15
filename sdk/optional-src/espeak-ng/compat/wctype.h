/* SPDX-License-Identifier: BSD-3-Clause
 *
 * <wctype.h> for the SDK's uClibc-ng, which is built without
 * __UCLIBC_HAS_WCHAR__ and therefore ships no <wctype.h> either.
 *
 * Upstream already solves the *classification* half of this problem, in
 * src/include/compat/wctype.h: after including whatever system <wctype.h>
 * exists, it redirects the isw* functions at ucd-tools -- the Unicode
 * character database espeak-ng links for exactly this purpose.  That shim
 * cannot be used here as-is, because it is written to sit in front of a real
 * system header (`#include_next`, with MSVC's bundled one as the other fallback)
 * and this libc has no such header underneath it.
 *
 * So the port supplies the header and keeps upstream's redirection, which is
 * not a workaround but the right behaviour: espeak-ng runs these tests over
 * characters from every script it can read, and the C-locale iswalpha() a real
 * libc would offer is strictly worse at that than ucd-tools.  The ucd_*
 * functions are already compiled into the port (libucd.a), so this adds no
 * code -- only the names.
 *
 * Deliberately no isw* *definitions* anywhere in the port.  Defining
 * `int iswalpha(wint_t)` alongside these macros does not work: the macro
 * rewrites the definition itself into a second definition of the ucd symbol,
 * which collides with ctype.c at link time.
 *
 * This file is found before src/include/compat/wctype.h; see the include-order
 * note in the Makefile.  Keep this list in sync with upstream's when bumping
 * the version -- it is the same twelve names with the same spelling.
 */

#ifndef ESPEAK_NG_COMPAT_WCTYPE_H
#define ESPEAK_NG_COMPAT_WCTYPE_H

#include <wchar.h>

#include <ucd/ucd.h>

#define iswalnum  ucd_isalnum
#define iswalpha  ucd_isalpha
#define iswblank  ucd_isblank
#define iswcntrl  ucd_iscntrl
#define iswdigit  ucd_isdigit
#define iswgraph  ucd_isgraph
#define iswlower  ucd_islower
#define iswprint  ucd_isprint
#define iswpunct  ucd_ispunct
#define iswspace  ucd_isspace
#define iswupper  ucd_isupper
#define iswxdigit ucd_isxdigit

#endif /* ESPEAK_NG_COMPAT_WCTYPE_H */
