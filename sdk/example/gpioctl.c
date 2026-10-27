/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * gpioctl.c - GPIO and LED-class example for the N32H7xx Linux SDK.
 *
 * A target is first matched against an exact LED label under /sys/class/leds.
 * If no LED matches, names such as PG3 are resolved to a gpiochip label
 * (GPIOG) and a line offset (3), then use the Linux GPIO ABI v2.  This keeps
 * the command independent of the GPIO currently assigned to an LED in DTS.
 *
 * Besides one-shot get/set, it demonstrates periodic work driven by
 * CLOCK_MONOTONIC absolute deadlines. pwm and breathe generate a software PWM
 * waveform on the selected output line. The current N32H7xx kernel does not
 * enable the Linux PWM framework, so these modes are not hardware PWM.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/gpio.h>

#define CHIP_PATH_LEN 64
#define OUTPUT_NAME_LEN 128
#define OUTPUT_PATH_LEN 320
#define CONSUMER_LABEL "n32h7xx-gpioctl"
#define LED_ROOT "/sys/class/leds"
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
#define PWM_RT_PRIORITY 20

struct pin {
	char name[16];
	char chip_label[GPIO_MAX_NAME_SIZE];
	char chip_path[CHIP_PATH_LEN];
	unsigned int offset;
	int chip_fd;
};

enum target_kind {
	TARGET_GPIO,
	TARGET_LED,
};

struct target {
	enum target_kind kind;
	struct pin pin;
	char name[OUTPUT_NAME_LEN];
	char brightness_path[OUTPUT_PATH_LEN];
	int line_fd;
};

struct sysfs_line {
	char chip_dir[320];
	char line_dir[64];
	unsigned long number;
};

struct runtime_opts {
	int active_low;
	unsigned long period_ms;
	unsigned long cycle_ms;
	unsigned long duration_ms;
	unsigned long count;
	unsigned long hz;
	double min_duty;
	double max_duty;
};

enum opt_flags {
	OPT_ACTIVE_LOW = 1U << 0,
	OPT_PERIOD_MS = 1U << 1,
	OPT_CYCLE_MS = 1U << 2,
	OPT_DURATION_MS = 1U << 3,
	OPT_COUNT = 1U << 4,
	OPT_HZ = 1U << 5,
	OPT_DUTY_RANGE = 1U << 6,
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static int enable_realtime(void)
{
	struct sched_param param = { .sched_priority = PWM_RT_PRIORITY };

	if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
		fprintf(stderr,
			"gpioctl: cannot enable real-time PWM scheduling: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage:\n"
		"  gpioctl list\n"
		"  gpioctl num PIN\n"
		"  gpioctl info TARGET\n"
		"  gpioctl get TARGET [--active-low]\n"
		"  gpioctl set TARGET 0|1 [--active-low]\n"
		"  gpioctl release TARGET\n"
		"  gpioctl blink TARGET [options]\n"
		"  gpioctl pwm TARGET DUTY [options]\n"
		"  gpioctl breathe TARGET [options]\n"
		"\n"
		"TARGET is an exact LED label under /sys/class/leds, or a GPIO\n"
		"name PA0..PK15. A raw gpiochipN:offset form is also accepted.\n"
		"Runtime options:\n"
		"  --active-low          GPIO only: 1 active, 0 inactive\n"
		"  --period-ms N         blink period (default 1000)\n"
		"  --cycle-ms N          breathe ramp period (default 3000)\n"
		"  --duration-ms N       stop after N ms; 0 means forever\n"
		"  --count N             blink/breathe cycles, or PWM periods\n"
		"  --hz N                software PWM frequency (default 100)\n"
		"  --min D               breathe minimum duty (default 0)\n"
		"  --max D               breathe maximum duty (default 100)\n"
		"\n"
		"Examples:\n"
		"  gpioctl blink red --period-ms 1000 --count 5\n"
		"  gpioctl pwm blue 35 --hz 100 --duration-ms 10000\n"
		"  gpioctl breathe green --cycle-ms 3000 --duration-ms 30000\n"
		"\n"
		"LED labels are resolved at runtime.  If DTS moves an LED to another\n"
		"GPIO without changing its label, the same command keeps working.\n");
}

static int parse_ulong(const char *s, unsigned long min, unsigned long max,
		       unsigned long *out)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(s, &end, 10);
	if (errno || end == s || *end || value < min || value > max)
		return -1;
	*out = value;
	return 0;
}

static int parse_double(const char *s, double min, double max, double *out)
{
	char *end = NULL;
	double value;

	errno = 0;
	value = strtod(s, &end);
	if (errno || end == s || *end || value != value || value < min || value > max)
		return -1;
	*out = value;
	return 0;
}

static void opts_init(struct runtime_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->period_ms = 1000;
	opts->cycle_ms = 3000;
	opts->hz = 100;
	opts->max_duty = 100.0;
}

static int parse_runtime_opts(int argc, char **argv, int first,
			      unsigned int allowed, struct runtime_opts *opts)
{
	int i;

	for (i = first; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--active-low") && (allowed & OPT_ACTIVE_LOW)) {
			opts->active_low = 1;
		} else if (!strcmp(arg, "--period-ms") && (allowed & OPT_PERIOD_MS)) {
			if (++i >= argc || parse_ulong(argv[i], 2, 86400000,
						       &opts->period_ms))
				goto bad_value;
		} else if (!strcmp(arg, "--cycle-ms") && (allowed & OPT_CYCLE_MS)) {
			if (++i >= argc || parse_ulong(argv[i], 10, 86400000,
						       &opts->cycle_ms))
				goto bad_value;
		} else if (!strcmp(arg, "--duration-ms") && (allowed & OPT_DURATION_MS)) {
			if (++i >= argc || parse_ulong(argv[i], 0, 86400000UL * 365,
						       &opts->duration_ms))
				goto bad_value;
		} else if (!strcmp(arg, "--count") && (allowed & OPT_COUNT)) {
			if (++i >= argc || parse_ulong(argv[i], 1, 1000000000UL,
						       &opts->count))
				goto bad_value;
		} else if (!strcmp(arg, "--hz") && (allowed & OPT_HZ)) {
			if (++i >= argc || parse_ulong(argv[i], 1, 1000, &opts->hz))
				goto bad_value;
		} else if (!strcmp(arg, "--min") && (allowed & OPT_DUTY_RANGE)) {
			if (++i >= argc || parse_double(argv[i], 0, 100,
							&opts->min_duty))
				goto bad_value;
		} else if (!strcmp(arg, "--max") && (allowed & OPT_DUTY_RANGE)) {
			if (++i >= argc || parse_double(argv[i], 0, 100,
							&opts->max_duty))
				goto bad_value;
		} else {
			fprintf(stderr, "gpioctl: unknown option or argument: %s\n", arg);
			return -1;
		}
	}

	if (opts->min_duty > opts->max_duty) {
		fprintf(stderr, "gpioctl: --min must not exceed --max\n");
		return -1;
	}
	return 0;

bad_value:
	fprintf(stderr, "gpioctl: invalid value for %s\n", argv[i - 1]);
	return -1;
}

static int open_gpiochip(const char *path)
{
	return open(path, O_RDWR | O_CLOEXEC);
}

static int parse_pin_name(const char *name, char *bank, unsigned int *offset)
{
	char *end = NULL;
	unsigned long value;

	if (name[0] != 'P' && name[0] != 'p')
		return -1;
	if (!name[1] || !name[2])
		return -1;
	*bank = toupper((unsigned char)name[1]);
	if (*bank < 'A' || *bank > 'Z')
		return -1;

	errno = 0;
	value = strtoul(name + 2, &end, 10);
	if (errno || end == name + 2 || *end || value > 31)
		return -1;
	*offset = (unsigned int)value;
	return 0;
}

static int resolve_pin(const char *name, struct pin *pin)
{
	char bank;
	char wanted[GPIO_MAX_NAME_SIZE];
	unsigned int offset;
	int chip;

	memset(pin, 0, sizeof(*pin));
	pin->chip_fd = -1;
	snprintf(pin->name, sizeof(pin->name), "%s", name);

	if (sscanf(name, "gpiochip%d:%u", &chip, &offset) == 2) {
		struct gpiochip_info info;

		if (chip < 0 || chip > 63 || offset > GPIO_V2_LINES_MAX - 1)
			return -1;
		snprintf(pin->chip_path, sizeof(pin->chip_path), "/dev/gpiochip%d", chip);
		pin->offset = offset;
		pin->chip_fd = open_gpiochip(pin->chip_path);
		if (pin->chip_fd < 0)
			return -1;
		memset(&info, 0, sizeof(info));
		if (ioctl(pin->chip_fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0 ||
		    offset >= info.lines) {
			int saved = errno ? errno : EINVAL;

			close(pin->chip_fd);
			pin->chip_fd = -1;
			errno = saved;
			return -1;
		}
		snprintf(pin->chip_label, sizeof(pin->chip_label), "%s",
			 info.label);
		return 0;
	}

	if (parse_pin_name(name, &bank, &offset) < 0)
		return -1;
	snprintf(wanted, sizeof(wanted), "GPIO%c", bank);

	for (chip = 0; chip < 64; chip++) {
		struct gpiochip_info info;
		int fd;

		snprintf(pin->chip_path, sizeof(pin->chip_path), "/dev/gpiochip%d", chip);
		fd = open_gpiochip(pin->chip_path);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
		    !strcmp(info.label, wanted)) {
			if (offset >= info.lines) {
				close(fd);
				errno = EINVAL;
				return -1;
			}
			pin->chip_fd = fd;
			pin->offset = offset;
			snprintf(pin->chip_label, sizeof(pin->chip_label), "%s", info.label);
			return 0;
		}
		close(fd);
	}

	errno = ENODEV;
	return -1;
}

static int read_attr(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	if (!size)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		n = read(fd, buf, size - 1);
	} while (n < 0 && errno == EINTR);
	close(fd);
	if (n <= 0) {
		if (!n)
			errno = EIO;
		return -1;
	}
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';
	return 0;
}

static int write_attr(const char *path, const char *value)
{
	size_t left = strlen(value);
	const char *p = value;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	while (left) {
		ssize_t n = write(fd, p, left);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			{
				int saved = errno;

				close(fd);
				errno = saved;
			}
			return -1;
		}
		p += n;
		left -= (size_t)n;
	}
	return close(fd);
}

static int resolve_led(const char *name, struct target *target)
{
	struct dirent *entry;
	DIR *dir;
	int saved_errno = ENOENT;

	if (!name[0] || strlen(name) >= sizeof(target->name)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	dir = opendir(LED_ROOT);
	if (!dir)
		return -1;
	while ((entry = readdir(dir)) != NULL) {
		char path[OUTPUT_PATH_LEN];
		int n;

		if (strcmp(entry->d_name, name))
			continue;
		n = snprintf(path, sizeof(path), LED_ROOT "/%s/brightness",
			     entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(path))
			continue;
		if (access(path, F_OK) < 0) {
			saved_errno = errno;
			continue;
		}

		memset(target, 0, sizeof(*target));
		target->kind = TARGET_LED;
		target->line_fd = -1;
		target->pin.chip_fd = -1;
		memcpy(target->name, entry->d_name, strlen(entry->d_name) + 1);
		snprintf(target->brightness_path,
			 sizeof(target->brightness_path), "%s", path);
		closedir(dir);
		return 0;
	}

	closedir(dir);
	errno = saved_errno;
	return -1;
}

static int read_led_brightness(const struct target *target,
			       unsigned long *brightness)
{
	char text[32];
	char *end = NULL;
	unsigned long value;

	if (read_attr(target->brightness_path, text, sizeof(text)) < 0)
		return -1;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || *end) {
		errno = EIO;
		return -1;
	}
	*brightness = value;
	return 0;
}

static int disable_led_trigger(const struct target *target)
{
	char path[OUTPUT_PATH_LEN];
	int n;

	n = snprintf(path, sizeof(path), LED_ROOT "/%s/trigger", target->name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (access(path, F_OK) < 0)
		return errno == ENOENT ? 0 : -1;
	return write_attr(path, "none");
}

static int led_set(const struct target *target, int active)
{
	return write_attr(target->brightness_path, active ? "1" : "0");
}

static int sysfs_locate(const char *name, struct sysfs_line *line)
{
	DIR *dir;
	struct dirent *entry;
	char base_text[32];
	char label[GPIO_MAX_NAME_SIZE];
	char path[320];
	struct pin pin;
	unsigned long base;
	char *end = NULL;

	if (resolve_pin(name, &pin) < 0)
		return -1;
	close(pin.chip_fd);
	pin.chip_fd = -1;
	if (!pin.chip_label[0]) {
		errno = ENODEV;
		return -1;
	}

	dir = opendir("/sys/class/gpio");
	if (!dir)
		return -1;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "gpiochip", 8))
			continue;

		snprintf(path, sizeof(path), "/sys/class/gpio/%s/label",
			 entry->d_name);
		if (read_attr(path, label, sizeof(label)) < 0 ||
		    strcmp(label, pin.chip_label))
			continue;

		snprintf(path, sizeof(path), "/sys/class/gpio/%s/base",
			 entry->d_name);
		if (read_attr(path, base_text, sizeof(base_text)) < 0)
			continue;
		errno = 0;
		base = strtoul(base_text, &end, 10);
		if (errno || end == base_text || *end)
			continue;

		snprintf(line->chip_dir, sizeof(line->chip_dir),
			 "/sys/class/gpio/%s", entry->d_name);
		line->number = base + pin.offset;
		snprintf(line->line_dir, sizeof(line->line_dir),
			 "/sys/class/gpio/gpio%lu", line->number);
		closedir(dir);
		return 0;
	}

	closedir(dir);
	errno = ENODEV;
	return -1;
}

static int sysfs_export(const char *name, struct sysfs_line *line)
{
	char number[32];

	if (sysfs_locate(name, line) < 0)
		return -1;
	if (access(line->line_dir, F_OK) == 0)
		return 0;
	if (errno != ENOENT)
		return -1;
	snprintf(number, sizeof(number), "%lu", line->number);
	if (write_attr("/sys/class/gpio/export", number) < 0)
		return -1;
	return access(line->line_dir, F_OK);
}

static int sysfs_release(const char *name)
{
	char number[32];
	struct sysfs_line line;

	if (sysfs_locate(name, &line) < 0)
		return -1;
	if (access(line.line_dir, F_OK) < 0)
		return errno == ENOENT ? 0 : -1;
	snprintf(number, sizeof(number), "%lu", line.number);
	return write_attr("/sys/class/gpio/unexport", number);
}

static int sysfs_set_output(const char *name, int active,
			    const struct runtime_opts *opts)
{
	char path[96];
	struct sysfs_line line;
	const char *direction;

	if (sysfs_export(name, &line) < 0)
		return -1;
	snprintf(path, sizeof(path), "%s/active_low", line.line_dir);
	if (write_attr(path, opts->active_low ? "1" : "0") < 0)
		return -1;
	/* direction high/low uses the raw pin level and avoids an output glitch. */
	direction = (opts->active_low ? !active : active) ? "high" : "low";
	snprintf(path, sizeof(path), "%s/direction", line.line_dir);
	return write_attr(path, direction);
}

static int sysfs_get_value(const char *name, const struct runtime_opts *opts,
			   int *active)
{
	char path[96];
	char text[16];
	struct sysfs_line line;
	unsigned long value, active_low;

	if (sysfs_locate(name, &line) < 0)
		return -1;
	if (access(line.line_dir, F_OK) < 0) {
		if (errno == ENOENT)
			return 1;
		return -1;
	}
	snprintf(path, sizeof(path), "%s/value", line.line_dir);
	if (read_attr(path, text, sizeof(text)) < 0)
		return -1;
	value = strtoul(text, NULL, 10) & 1;
	snprintf(path, sizeof(path), "%s/active_low", line.line_dir);
	if (read_attr(path, text, sizeof(text)) < 0)
		active_low = 0;
	else
		active_low = strtoul(text, NULL, 10) & 1;
	*active = (int)((value ^ active_low ^ !!opts->active_low) & 1);
	return 0;
}

static int request_line(struct pin *pin, uint64_t flags, int initial_active,
			int active_low)
{
	struct gpio_v2_line_request request;
	int fd;

	if (pin->chip_fd < 0) {
		errno = EBADF;
		return -1;
	}

	memset(&request, 0, sizeof(request));
	request.offsets[0] = pin->offset;
	request.num_lines = 1;
	request.config.flags = flags;
	if (active_low)
		request.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
	if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
		request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		request.config.attrs[0].attr.values = initial_active ? 1 : 0;
		request.config.attrs[0].mask = 1;
		request.config.num_attrs = 1;
	}
	snprintf(request.consumer, sizeof(request.consumer), "%s", CONSUMER_LABEL);

	if (ioctl(pin->chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
		int saved = errno;

		close(pin->chip_fd);
		pin->chip_fd = -1;
		errno = saved;
		return -1;
	}
	fd = request.fd;
	close(pin->chip_fd);
	pin->chip_fd = -1;
	return fd;
}

static int line_set(int fd, int active)
{
	struct gpio_v2_line_values values;

	memset(&values, 0, sizeof(values));
	values.mask = 1;
	values.bits = active ? 1 : 0;
	return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

static int line_get(int fd, int *active)
{
	struct gpio_v2_line_values values;

	memset(&values, 0, sizeof(values));
	values.mask = 1;
	if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
		return -1;
	*active = values.bits & 1 ? 1 : 0;
	return 0;
}

static void print_pin(const struct pin *pin)
{
	printf("%s: %s line %u", pin->name, pin->chip_path, pin->offset);
	if (pin->chip_label[0])
		printf(" (%s)", pin->chip_label);
	putchar('\n');
}

static int list_chips(void)
{
	int chip, found = 0;

	for (chip = 0; chip < 64; chip++) {
		struct gpiochip_info info;
		char path[CHIP_PATH_LEN];
		int fd;

		snprintf(path, sizeof(path), "/dev/gpiochip%d", chip);
		fd = open_gpiochip(path);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
			printf("%-20s label=%-8s lines=%u\n", path,
			       info.label[0] ? info.label : "-", info.lines);
			found++;
		}
		close(fd);
	}
	return found ? 0 : 1;
}

static int list_leds(void)
{
	struct dirent *entry;
	DIR *dir;
	int found = 0;

	dir = opendir(LED_ROOT);
	if (!dir)
		return 1;
	while ((entry = readdir(dir)) != NULL) {
		char path[OUTPUT_PATH_LEN];
		int n;

		if (entry->d_name[0] == '.')
			continue;
		n = snprintf(path, sizeof(path), LED_ROOT "/%s/brightness",
			     entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(path))
			continue;
		if (access(path, F_OK) < 0)
			continue;
		printf("%-20s class=led label=%s\n",
		       "/sys/class/leds", entry->d_name);
		found++;
	}
	closedir(dir);
	return found ? 0 : 1;
}

static int list_targets(void)
{
	int chips = list_chips();
	int leds = list_leds();

	return chips && leds;
}

static int print_led_info(const struct target *target)
{
	char path[OUTPUT_PATH_LEN];
	char text[64];
	unsigned long brightness;

	printf("%s: LED class %s\n", target->name, target->brightness_path);
	if (read_led_brightness(target, &brightness) == 0)
		printf("  brightness=%lu\n", brightness);
	else
		printf("  brightness=<unreadable>\n");

	snprintf(path, sizeof(path), LED_ROOT "/%s/max_brightness",
		 target->name);
	if (read_attr(path, text, sizeof(text)) == 0)
		printf("  max_brightness=%s\n", text);
	snprintf(path, sizeof(path), LED_ROOT "/%s/trigger", target->name);
	if (read_attr(path, text, sizeof(text)) == 0)
		printf("  trigger=%s\n", text);
	return 0;
}

static int cmd_info(const char *name)
{
	struct gpio_v2_line_info info;
	struct target target;
	struct pin pin;
	int fd;

	if (resolve_led(name, &target) == 0)
		return print_led_info(&target);
	if (resolve_pin(name, &pin) < 0) {
		fprintf(stderr, "gpioctl: cannot resolve %s: %s\n", name, strerror(errno));
		return 1;
	}
	fd = pin.chip_fd;
	print_pin(&pin);
	memset(&info, 0, sizeof(info));
	info.offset = pin.offset;
	if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0) {
		fprintf(stderr, "gpioctl: line info failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("  name=%s consumer=%s\n",
	       info.name[0] ? info.name : "-",
	       info.consumer[0] ? info.consumer : "-");
	printf("  flags=%s%s%s%s%s\n",
	       info.flags & GPIO_V2_LINE_FLAG_USED ? "used " : "",
	       info.flags & GPIO_V2_LINE_FLAG_INPUT ? "input " : "",
	       info.flags & GPIO_V2_LINE_FLAG_OUTPUT ? "output " : "",
	       info.flags & GPIO_V2_LINE_FLAG_ACTIVE_LOW ? "active-low " : "",
	       info.flags & (GPIO_V2_LINE_FLAG_EDGE_RISING |
			     GPIO_V2_LINE_FLAG_EDGE_FALLING) ? "edge" : "");
	close(fd);
	return 0;
}

static int cmd_get(const char *name, const struct runtime_opts *opts)
{
	struct target target;
	struct pin pin;
	int active, fd, rc;

	if (resolve_led(name, &target) == 0) {
		unsigned long brightness;

		if (read_led_brightness(&target, &brightness) < 0) {
			fprintf(stderr, "gpioctl: read %s failed: %s\n",
				name, strerror(errno));
			return 1;
		}
		printf("%lu\n", brightness);
		return 0;
	}

	rc = sysfs_get_value(name, opts, &active);
	if (rc == 0) {
		printf("%d\n", active);
		return 0;
	}
	if (rc < 0 && errno != ENODEV) {
		fprintf(stderr, "gpioctl: read %s failed: %s\n", name, strerror(errno));
		return 1;
	}

	if (resolve_pin(name, &pin) < 0) {
		fprintf(stderr, "gpioctl: cannot resolve %s: %s\n", name, strerror(errno));
		return 1;
	}
	fd = request_line(&pin, GPIO_V2_LINE_FLAG_INPUT, 0, opts->active_low);
	if (fd < 0) {
		fprintf(stderr, "gpioctl: request %s as input failed: %s\n",
			name, strerror(errno));
		return 1;
	}
	if (line_get(fd, &active) < 0) {
		fprintf(stderr, "gpioctl: read %s failed: %s\n", name, strerror(errno));
		close(fd);
		return 1;
	}
	printf("%d\n", active);
	close(fd);
	return 0;
}

static int cmd_set(const char *name, int active, const struct runtime_opts *opts)
{
	struct target target;

	if (resolve_led(name, &target) == 0) {
		if (disable_led_trigger(&target) < 0 ||
		    led_set(&target, active) < 0) {
			fprintf(stderr, "gpioctl: set %s failed: %s\n",
				name, strerror(errno));
			return 1;
		}
		return 0;
	}
	if (sysfs_set_output(name, active, opts) < 0) {
		fprintf(stderr, "gpioctl: set %s failed: %s\n",
			name, strerror(errno));
		return 1;
	}
	return 0;
}

static int cmd_release(const char *name)
{
	struct target target;

	if (resolve_led(name, &target) == 0) {
		if (disable_led_trigger(&target) < 0 ||
		    led_set(&target, 0) < 0) {
			fprintf(stderr, "gpioctl: release %s failed: %s\n",
				name, strerror(errno));
			return 1;
		}
		return 0;
	}
	if (sysfs_release(name) < 0) {
		fprintf(stderr, "gpioctl: release %s failed: %s\n",
			name, strerror(errno));
		return 1;
	}
	return 0;
}

static void timespec_add_ns(struct timespec *ts, uint64_t ns)
{
	ts->tv_sec += (time_t)(ns / NSEC_PER_SEC);
	ts->tv_nsec += (long)(ns % NSEC_PER_SEC);
	if (ts->tv_nsec >= (long)NSEC_PER_SEC) {
		ts->tv_sec++;
		ts->tv_nsec -= (long)NSEC_PER_SEC;
	}
}

static uint64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b)
{
	return (uint64_t)(a->tv_sec - b->tv_sec) * NSEC_PER_SEC +
	       (uint64_t)(a->tv_nsec - b->tv_nsec);
}

static int sleep_until(const struct timespec *deadline)
{
	int rc;

	do {
		rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
	} while (rc == EINTR && !stop_requested);
	if (stop_requested && rc == EINTR)
		return -1;
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

static int output_set(const struct target *target, int active)
{
	if (target->kind == TARGET_LED)
		return led_set(target, active);
	return line_set(target->line_fd, active);
}

static int open_output(const char *name, const struct runtime_opts *opts,
		       struct target *target)
{
	struct pin pin;

	if (resolve_led(name, target) == 0) {
		if (disable_led_trigger(target) < 0) {
			fprintf(stderr, "gpioctl: disable %s trigger failed: %s\n",
				name, strerror(errno));
			return -1;
		}
		return 0;
	}

	/* A persistent `set` uses sysfs. Timed modes need exclusive ownership. */
	(void)sysfs_release(name);
	errno = 0;
	if (resolve_pin(name, &pin) < 0) {
		fprintf(stderr, "gpioctl: cannot resolve %s: %s\n", name, strerror(errno));
		return -1;
	}
	target->kind = TARGET_GPIO;
	target->pin = pin;
	snprintf(target->name, sizeof(target->name), "%s", name);
	target->line_fd = request_line(&pin, GPIO_V2_LINE_FLAG_OUTPUT, 0,
				      opts->active_low);
	if (target->line_fd < 0) {
		fprintf(stderr, "gpioctl: request %s as output failed: %s\n",
			name, strerror(errno));
		return -1;
	}
	return 0;
}

static int run_blink(const struct target *target,
		     const struct runtime_opts *opts)
{
	struct timespec start, deadline;
	uint64_t period_ns = (uint64_t)opts->period_ms * NSEC_PER_MSEC;
	uint64_t on_ns = period_ns / 2;
	uint64_t off_ns = period_ns - on_ns;
	unsigned long cycle;

	clock_gettime(CLOCK_MONOTONIC, &start);
	deadline = start;
	printf("blink: target=%s period=%lums count=%lu duration=%lums%s\n",
	       target->name,
	       opts->period_ms, opts->count, opts->duration_ms,
	       target->kind == TARGET_GPIO && opts->active_low ?
		       " active-low" : "");
	fflush(stdout);

	for (cycle = 0; !stop_requested; cycle++) {
		if (opts->count && cycle >= opts->count)
			break;
		if (opts->duration_ms &&
		    timespec_diff_ns(&deadline, &start) >=
			    (uint64_t)opts->duration_ms * NSEC_PER_MSEC)
			break;

		if (output_set(target, 1) < 0)
			return -1;
		timespec_add_ns(&deadline, on_ns);
		if (sleep_until(&deadline) < 0)
			break;
		if (output_set(target, 0) < 0)
			return -1;
		timespec_add_ns(&deadline, off_ns);
		if (sleep_until(&deadline) < 0)
			break;
	}
	return 0;
}

static int run_pwm(const struct target *target,
		   const struct runtime_opts *opts, double duty)
{
	struct timespec start, period_start, deadline;
	uint64_t period_ns = NSEC_PER_SEC / opts->hz;
	uint64_t on_ns = (uint64_t)((double)period_ns * duty / 100.0 + 0.5);
	unsigned long pulse;

	clock_gettime(CLOCK_MONOTONIC, &start);
	period_start = start;
	printf("pwm: target=%s duty=%.1f%% hz=%lu count=%lu duration=%lums%s\n",
	       target->name,
	       duty, opts->hz, opts->count, opts->duration_ms,
	       target->kind == TARGET_GPIO && opts->active_low ?
		       " active-low" : "");
	fflush(stdout);

	for (pulse = 0; !stop_requested; pulse++) {
		if (opts->count && pulse >= opts->count)
			break;
		if (opts->duration_ms &&
		    timespec_diff_ns(&period_start, &start) >=
			    (uint64_t)opts->duration_ms * NSEC_PER_MSEC)
			break;

		deadline = period_start;
		timespec_add_ns(&deadline, on_ns);
		if (on_ns) {
			if (output_set(target, 1) < 0)
				return -1;
			if (sleep_until(&deadline) < 0)
				break;
		}
		if (on_ns < period_ns) {
			if (output_set(target, 0) < 0)
				return -1;
			deadline = period_start;
			timespec_add_ns(&deadline, period_ns);
			if (sleep_until(&deadline) < 0)
				break;
		} else {
			deadline = period_start;
			timespec_add_ns(&deadline, period_ns);
		}
		period_start = deadline;
	}
	return 0;
}

static int run_breathe(const struct target *target,
		       const struct runtime_opts *opts)
{
	struct timespec start, period_start, deadline;
	uint64_t pwm_period_ns = NSEC_PER_SEC / opts->hz;
	uint64_t cycle_ns = (uint64_t)opts->cycle_ms * NSEC_PER_MSEC;
	uint64_t half_cycle_ns = cycle_ns / 2;

	clock_gettime(CLOCK_MONOTONIC, &start);
	period_start = start;
	printf("breathe: target=%s cycle=%lums hz=%lu min=%.1f max=%.1f count=%lu duration=%lums%s\n",
	       target->name,
	       opts->cycle_ms, opts->hz, opts->min_duty, opts->max_duty,
	       opts->count, opts->duration_ms,
	       target->kind == TARGET_GPIO && opts->active_low ?
		       " active-low" : "");
	fflush(stdout);

	while (!stop_requested) {
		uint64_t elapsed = timespec_diff_ns(&period_start, &start);
		uint64_t phase = elapsed % cycle_ns;
		uint64_t triangle;
		double factor, duty;
		uint64_t on_ns;

		if (opts->duration_ms &&
		    elapsed >= (uint64_t)opts->duration_ms * NSEC_PER_MSEC)
			break;
		if (opts->count && elapsed / cycle_ns >= opts->count)
			break;

		if (phase < half_cycle_ns)
			triangle = phase;
		else
			triangle = cycle_ns - phase;
		factor = half_cycle_ns ? (double)triangle / half_cycle_ns : 1.0;
		duty = opts->min_duty + (opts->max_duty - opts->min_duty) * factor;
		on_ns = (uint64_t)((double)pwm_period_ns * duty / 100.0 + 0.5);

		deadline = period_start;
		timespec_add_ns(&deadline, on_ns);
		if (on_ns) {
			if (output_set(target, 1) < 0)
				return -1;
			if (sleep_until(&deadline) < 0)
				break;
		}
		if (on_ns < pwm_period_ns) {
			if (output_set(target, 0) < 0)
				return -1;
			deadline = period_start;
			timespec_add_ns(&deadline, pwm_period_ns);
			if (sleep_until(&deadline) < 0)
				break;
		} else {
			deadline = period_start;
			timespec_add_ns(&deadline, pwm_period_ns);
		}
		period_start = deadline;
	}
	return 0;
}

static int run_timed(const char *name, const char *mode,
		     const struct runtime_opts *opts, double duty)
{
	struct target target;
	int rc, stop_rc;

	if (strcmp(mode, "blink") && enable_realtime() < 0)
		return 1;
	if (open_output(name, opts, &target) < 0)
		return 1;

	if (!strcmp(mode, "blink"))
		rc = run_blink(&target, opts);
	else if (!strcmp(mode, "pwm"))
		rc = run_pwm(&target, opts, duty);
	else
		rc = run_breathe(&target, opts);

	stop_rc = output_set(&target, 0);
	if (target.kind == TARGET_GPIO && target.line_fd >= 0)
		close(target.line_fd);
	if (rc < 0) {
		fprintf(stderr, "gpioctl: %s failed: %s\n", mode, strerror(errno));
		return 1;
	}
	if (stop_rc < 0) {
		fprintf(stderr, "gpioctl: stop %s failed: %s\n",
			name, strerror(errno));
		return 1;
	}
	printf("%s: stopped, output inactive\n", mode);
	return 0;
}

int main(int argc, char **argv)
{
	struct runtime_opts opts;
	struct sigaction sa;
	unsigned long value;
	double duty;

	opts_init(&opts);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
	    !strcmp(argv[1], "-h")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(argv[1], "list")) {
		if (argc != 2)
			goto bad_usage;
		return list_targets();
	}
	if (!strcmp(argv[1], "num")) {
		struct target target;
		struct pin pin;

		if (argc != 3)
			goto bad_usage;
		if (resolve_led(argv[2], &target) == 0) {
			fprintf(stderr,
				"gpioctl: %s is an LED class label, not a GPIO line; use 'gpioctl info %s'\n",
				argv[2], argv[2]);
			return 1;
		}
		if (resolve_pin(argv[2], &pin) < 0) {
			fprintf(stderr, "gpioctl: cannot resolve %s: %s\n",
				argv[2], strerror(errno));
			return 1;
		}
		print_pin(&pin);
		close(pin.chip_fd);
		return 0;
	}
	if (!strcmp(argv[1], "info")) {
		if (argc != 3)
			goto bad_usage;
		return cmd_info(argv[2]);
	}
	if (!strcmp(argv[1], "get")) {
		if (argc < 3 ||
		    parse_runtime_opts(argc, argv, 3, OPT_ACTIVE_LOW, &opts))
			goto bad_usage;
		return cmd_get(argv[2], &opts);
	}
	if (!strcmp(argv[1], "set")) {
		if (argc < 4 || parse_ulong(argv[3], 0, 1, &value) ||
		    parse_runtime_opts(argc, argv, 4, OPT_ACTIVE_LOW, &opts))
			goto bad_usage;
		return cmd_set(argv[2], (int)value, &opts);
	}
	if (!strcmp(argv[1], "release")) {
		if (argc != 3)
			goto bad_usage;
		return cmd_release(argv[2]);
	}
	if (!strcmp(argv[1], "blink")) {
		if (argc < 3 ||
		    parse_runtime_opts(argc, argv, 3,
			OPT_ACTIVE_LOW | OPT_PERIOD_MS | OPT_DURATION_MS |
			OPT_COUNT, &opts))
			goto bad_usage;
		return run_timed(argv[2], "blink", &opts, 50.0);
	}
	if (!strcmp(argv[1], "pwm")) {
		if (argc < 4 || parse_double(argv[3], 0, 100, &duty) ||
		    parse_runtime_opts(argc, argv, 4,
			OPT_ACTIVE_LOW | OPT_DURATION_MS | OPT_COUNT | OPT_HZ,
			&opts))
			goto bad_usage;
		return run_timed(argv[2], "pwm", &opts, duty);
	}
	if (!strcmp(argv[1], "breathe")) {
		if (argc < 3 ||
		    parse_runtime_opts(argc, argv, 3,
			OPT_ACTIVE_LOW | OPT_CYCLE_MS | OPT_DURATION_MS |
			OPT_COUNT | OPT_HZ | OPT_DUTY_RANGE, &opts))
			goto bad_usage;
		return run_timed(argv[2], "breathe", &opts, 0.0);
	}

bad_usage:
	usage(stderr);
	return 2;
}
