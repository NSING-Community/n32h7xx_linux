/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * leds.c - board LED marquee for the N32H787 HMI board.
 *
 * The board DTS registers PB3, PF10 and PI8 as active-low gpio-leds.  The
 * kernel owns those GPIO lines and exposes them through the standard LED
 * class:
 *
 *   /sys/class/leds/red/brightness
 *   /sys/class/leds/green/brightness
 *   /sys/class/leds/blue/brightness
 *
 * Active-low polarity is handled by the device tree, so userspace uses the
 * logical values 1 (on) and 0 (off).  Do not export these pins through
 * /sys/class/gpio or claim them with gpioctl; the LED driver already owns them.
 *
 * Usage:
 *   leds             marquee forever, 250 ms per step
 *   leds 5           five complete rounds, then exit
 *   leds 5 120       five rounds, 120 ms per step
 *
 * On exit, including Ctrl-C, the LEDs are switched off.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LED_ROOT "/sys/class/leds"

struct led {
	const char *name;
	const char *brightness;
};

static const struct led leds[] = {
	{ "red",   LED_ROOT "/red/brightness" },
	{ "green", LED_ROOT "/green/brightness" },
	{ "blue",  LED_ROOT "/blue/brightness" },
};

static const int led_count = (int)(sizeof(leds) / sizeof(leds[0]));

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static int led_set(int index, int on)
{
	const char *value = on ? "1" : "0";
	ssize_t n;
	int fd;

	fd = open(leds[index].brightness, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, value, 1);
	close(fd);
	return n == 1 ? 0 : -1;
}

static int led_prepare(int index)
{
	if (access(leds[index].brightness, W_OK) == 0)
		return 0;
	fprintf(stderr, "%s: can't access %s: %s\n", leds[index].name,
		leds[index].brightness, strerror(errno));
	return -1;
}

int main(int argc, char **argv)
{
	int rounds = 0;		/* 0 means forever */
	int step_ms = 250;
	int i, round, step;
	struct sigaction sa;

	if (argc > 1)
		rounds = atoi(argv[1]);
	if (argc > 2)
		step_ms = atoi(argv[2]);
	if (step_ms < 10)
		step_ms = 10;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	for (i = 0; i < led_count; i++) {
		if (led_prepare(i) != 0) {
			int j;

			for (j = 0; j < i; j++)
				led_set(j, 0);
			return 1;
		}
	}

	printf("red=%s  green=%s  blue=%s   (%d ms/step, %s)\n",
	       leds[0].brightness, leds[1].brightness, leds[2].brightness,
	       step_ms, rounds ? "finite" : "Ctrl-C to stop");
	fflush(stdout);

	/* Both parts matter: the inner loop breaks on a signal, but without
	 * the !stopping test here an endless run (rounds == 0) would just
	 * start the next round and never reach the cleanup below. */
	for (round = 0; !stopping && (rounds == 0 || round < rounds); round++) {
		for (step = 0; step < led_count && !stopping; step++) {
			for (i = 0; i < led_count; i++) {
				if (led_set(i, i == step) != 0) {
					fprintf(stderr, "%s: write failed: %s\n",
						leds[i].name, strerror(errno));
					stopping = 1;
					break;
				}
			}
			if (!stopping)
				usleep((useconds_t)step_ms * 1000);
		}
	}

	for (i = 0; i < led_count; i++)
		led_set(i, 0);
	puts("leds: stopped, LEDs off");
	return 0;
}
