/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Wide string functions missing from the SDK's uClibc-ng, which is built
 * without __UCLIBC_HAS_WCHAR__.  Target build only; see compat/wchar.h for why
 * this exists and which functions are covered.
 *
 * Only the string half lives here.  The classification and case-mapping
 * functions (isw*) are provided as macros over ucd-tools by compat/wctype.h,
 * following upstream's own src/include/compat/wctype.h -- they must NOT be
 * reimplemented here, because a definition of `iswalpha` is textually
 * rewritten by that very macro into a duplicate definition of `ucd_isalpha`.
 */

#include <wchar.h>

#include <stdlib.h>

size_t wcslen(const wchar_t *s)
{
	const wchar_t *p = s;

	while (*p != 0)
		p++;
	return (size_t)(p - s);
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
	for (; *s != 0; s++) {
		if (*s == c)
			return (wchar_t *)s;
	}
	return (c == 0) ? (wchar_t *)s : NULL;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
	const wchar_t *last = NULL;

	for (;; s++) {
		if (*s == c)
			last = s;
		if (*s == 0)
			break;
	}
	return (wchar_t *)last;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
	while (*a != 0 && *a == *b) {
		a++;
		b++;
	}
	return (int)(*a - *b);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	for (; n > 0; n--, a++, b++) {
		if (*a != *b)
			return (int)(*a - *b);
		if (*a == 0)
			break;
	}
	return 0;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)
{
	wchar_t *d = dst;

	while ((*d++ = *src++) != 0)
		;
	return dst;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n)
{
	wchar_t *d = dst;

	for (; n > 0 && *src != 0; n--)
		*d++ = *src++;
	for (; n > 0; n--)
		*d++ = 0;
	return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src)
{
	wchar_t *d = dst + wcslen(dst);

	while ((*d++ = *src++) != 0)
		;
	return dst;
}

/* This is the one function here that is not a loop, and it is the one the
 * library actually reaches at run time: ssml.c parses <prosody rate="...">
 * values with it.
 *
 * strtod() only ever consumes ASCII, so a byte-per-byte copy of a bounded
 * prefix gives the same value, and the byte offset it stops at maps back to
 * the same index in the wide string.  Non-ASCII codepoints become '\1', which
 * is not a character strtod can consume or skip, so parsing stops there --
 * which is what a wide strtod would do as well.  A double cannot hold more
 * than ~17 significant digits, so the prefix bound never changes the result;
 * it only stops a pathological input from overrunning the buffer. */
double wcstod(const wchar_t *s, wchar_t **endp)
{
	char buf[128];
	const wchar_t *p = s;
	size_t n = 0;
	char *end;
	double value;

	while (*p != 0 && n < sizeof(buf) - 1) {
		buf[n++] = (*p < 0x80) ? (char)*p : '\1';
		p++;
	}
	buf[n] = 0;

	value = strtod(buf, &end);
	if (endp != NULL)
		*endp = (wchar_t *)(s + (size_t)(end - buf));
	return value;
}
