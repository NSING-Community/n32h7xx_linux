/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Allocation probe for the N32H787 no-MMU target.
 *
 * The board's uClibc is built with MALLOC_SIMPLE, which on a no-MMU kernel
 * turns every malloc() into
 *
 *     mmap(NULL, size + 4, PROT_READ|PROT_WRITE,
 *          MAP_SHARED|MAP_ANONYMOUS|MAP_UNINITIALIZED, 0, 0)
 *
 * (libc/stdlib/malloc-simple/alloc.c).  There is no heap and no free-list: one
 * malloc is one mapping, and a no-MMU mapping is backed by physically
 * contiguous RAM obtained through alloc_pages_exact(), which rounds the
 * request up to a *power-of-two* number of pages.  So malloc(600104) needs a
 * free 1 MiB block, not 600 KiB.
 *
 * This program reproduces that shape directly: it takes a list of sizes,
 * allocates them all and keeps them, and reports which ones the board can
 * actually satisfy right now.  Run it with one size per invocation to measure
 * the largest single allocation, or with espeak-ng's sizes in espeak-ng's order
 * to measure the real requirement.
 *
 * Deliberately does not free(): the process exit tears the whole mm down, and
 * keeping every block live is the point of the multi-size mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static long meminfo_kb(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	size_t klen = strlen(key);
	long val = -1;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, klen) == 0) {
			val = strtol(line + klen, NULL, 10);
			break;
		}
	}
	fclose(f);
	return val;
}

static void report_mem(const char *when)
{
	printf("  mem %-8s MemFree=%ld kB MemAvailable=%ld kB\n",
	       when, meminfo_kb("MemFree:"), meminfo_kb("MemAvailable:"));
}

int main(int argc, char **argv)
{
	int i;
	int ok = 0, fail = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s SIZE [SIZE ...]\n", argv[0]);
		fprintf(stderr, "  each SIZE is a malloc() held until exit\n");
		return 2;
	}

	report_mem("before");
	for (i = 1; i < argc; i++) {
		unsigned long size = strtoul(argv[i], NULL, 0);
		void *p;

		errno = 0;
		p = malloc(size);
		if (p == NULL) {
			printf("  malloc(%-8lu) = FAIL errno=%d (%s)\n",
			       size, errno, strerror(errno));
			fail++;
		} else {
			/* Touch the whole block: proves it is really backed. */
			memset(p, 0x5a, size);
			printf("  malloc(%-8lu) = ok   at %p\n", size, p);
			ok++;
		}
	}
	report_mem("after");
	printf("  %d ok, %d failed\n", ok, fail);

	return fail != 0;
}
