/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * leds.c - board LED marquee for the N32H787 HMI board.
 *
 * The three user LEDs are wired to PB3, PF10 and PI8.  On this board they sit
 * between a 3.3 V rail and the pin (VCC_3V3 -> series resistor -> LED -> pin,
 * see the N32H787 HMI V1.1 schematic netlist), so the pin sinks the current
 * and they are ACTIVE LOW: value 0 lights a LED, value 1 turns it off.
 *
 * They are driven through the classic /sys/class/gpio interface:
 *
 *   /sys/class/gpio/export           write a global GPIO number to claim it
 *   /sys/class/gpio/gpioN/direction  "in" or "out"
 *   /sys/class/gpio/gpioN/value      "0" or "1"
 *
 * The global number is not the pin name.  gpiolib numbers the banks from a
 * base that is only decided when the GPIO chips register - 512..672 on this
 * board, one 16-pin chip per port in order A..K - so GPIOB line 3 is
 * 528 + 3, not 3.  That base is not a firmware constant, so this example
 * looks it up by matching the "label" of each gpiochipN device rather than
 * hard-coding it.
 *
 * Usage:
 *   leds             marquee forever, 250 ms per step
 *   leds 5           five complete rounds, then exit
 *   leds 5 120       five rounds, 120 ms per step
 *
 * On exit, including Ctrl-C, the LEDs are switched off and the lines this
 * run exported are released again.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GPIO_ROOT	"/sys/class/gpio"

/*
 * Scratch size for "<GPIO_ROOT>/<entry>/<attribute>".  <entry> is a sysfs
 * name, so the compiler budgets NAME_MAX (255) for it; leave room for the
 * fixed parts on top of that instead of sizing to what this board uses.
 */
#define SYSPATH_LEN	320

struct led {
	const char *name;	/* pin name, for messages */
	const char *bank;	/* gpiochip label, e.g. "GPIOB" */
	int line;		/* line inside the bank */
	int number;		/* resolved global number, -1 until looked up */
	int exported;		/* true if this run created it */
};

static struct led leds[] = {
	{ "PB3",  "GPIOB",  3, -1, 0 },
	{ "PF10", "GPIOF", 10, -1, 0 },
	{ "PI8",  "GPIOI",  8, -1, 0 },
};

static const int led_count = (int)(sizeof(leds) / sizeof(leds[0]));

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

/* Read a single-line sysfs attribute; returns 0 on success. */
static int read_attr(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

/* Write a string to a sysfs attribute; returns 0 on success. */
static int write_attr(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}

static void led_attr_path(const struct led *led, const char *attr,
			  char *path, size_t size)
{
	snprintf(path, size, GPIO_ROOT "/gpio%d/%s", led->number, attr);
}

/* Find the global base number of the GPIO bank whose label is @p bank. */
static int bank_base(const char *bank)
{
	DIR *dir = opendir(GPIO_ROOT);
	struct dirent *entry;
	char path[SYSPATH_LEN], value[64];
	int base = -1;

	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "gpiochip", 8) != 0)
			continue;
		snprintf(path, sizeof(path), GPIO_ROOT "/%s/label",
			 entry->d_name);
		if (read_attr(path, value, sizeof(value)) != 0)
			continue;
		if (strcmp(value, bank) != 0)
			continue;
		snprintf(path, sizeof(path), GPIO_ROOT "/%s/base",
			 entry->d_name);
		if (read_attr(path, value, sizeof(value)) == 0) {
			base = atoi(value);
			break;
		}
	}
	closedir(dir);
	return base;
}

/* Board LED polarity, kept in one place: see the file header. */
#define LED_LIT		0
#define LED_DARK	1

static void led_set(const struct led *led, int level)
{
	char path[SYSPATH_LEN];

	if (led->number < 0)
		return;
	led_attr_path(led, "value", path, sizeof(path));
	if (write_attr(path, level ? "1" : "0") != 0)
		fprintf(stderr, "%s: can't write %s: %s\n", led->name, path,
			strerror(errno));
}

static void led_unexport(struct led *led)
{
	char path[SYSPATH_LEN], number[16];

	if (led->number < 0 || !led->exported)
		return;
	snprintf(number, sizeof(number), "%d", led->number);
	snprintf(path, sizeof(path), GPIO_ROOT "/unexport");
	write_attr(path, number);
	led->exported = 0;
}

static int led_claim(struct led *led)
{
	char path[SYSPATH_LEN], dir[SYSPATH_LEN], number[16];

	if (led->number < 0) {
		fprintf(stderr, "%s: no gpiochip device labelled %s\n",
			led->name, led->bank);
		return -1;
	}

	led_attr_path(led, "direction", dir, sizeof(dir));
	if (access(dir, F_OK) != 0) {
		/* Not exported yet, so claim it; the kernel then creates
		 * gpioN/{direction,value,edge,active_low} under GPIO_ROOT. */
		snprintf(number, sizeof(number), "%d", led->number);
		snprintf(path, sizeof(path), GPIO_ROOT "/export");
		if (write_attr(path, number) != 0) {
			fprintf(stderr, "%s: export gpio%d failed: %s\n",
				led->name, led->number, strerror(errno));
			return -1;
		}
		led->exported = 1;
	} else {
		/* Already exported: either a previous run was killed, or the
		 * line is in use elsewhere.  Drive it anyway and leave the
		 * export state as we found it. */
	}

	if (write_attr(dir, "out") != 0) {
		fprintf(stderr, "%s: can't set direction to out: %s\n",
			led->name, strerror(errno));
		led_unexport(led);
		return -1;
	}
	return 0;
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

	for (i = 0; i < led_count; i++) {
		int base = bank_base(leds[i].bank);

		if (base >= 0)
			leds[i].number = base + leds[i].line;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	for (i = 0; i < led_count; i++) {
		if (led_claim(&leds[i]) != 0) {
			int j;

			for (j = 0; j < i; j++) {
				led_set(&leds[j], LED_DARK);
				led_unexport(&leds[j]);
			}
			return 1;
		}
	}

	printf("%s=gpio%d  %s=gpio%d  %s=gpio%d   (%d ms/step, %s)\n",
	       leds[0].name, leds[0].number,
	       leds[1].name, leds[1].number,
	       leds[2].name, leds[2].number,
	       step_ms, rounds ? "finite" : "Ctrl-C to stop");
	fflush(stdout);

	/* Both parts matter: the inner loop breaks on a signal, but without
	 * the !stopping test here an endless run (rounds == 0) would just
	 * start the next round and never reach the cleanup below. */
	for (round = 0; !stopping && (rounds == 0 || round < rounds); round++) {
		for (step = 0; step < led_count && !stopping; step++) {
			for (i = 0; i < led_count; i++)
				led_set(&leds[i], i == step ? LED_LIT : LED_DARK);
			usleep((useconds_t)step_ms * 1000);
		}
	}

	for (i = 0; i < led_count; i++) {
		led_set(&leds[i], LED_DARK);
		led_unexport(&leds[i]);
	}
	puts("leds: stopped, LEDs off");
	return 0;
}
