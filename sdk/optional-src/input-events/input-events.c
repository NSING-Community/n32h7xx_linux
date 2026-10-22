// SPDX-License-Identifier: BSD-3-Clause

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
	(void)sig;
	running = 0;
}

static const char *event_name(uint16_t type)
{
	switch (type) {
	case EV_SYN: return "EV_SYN";
	case EV_KEY: return "EV_KEY";
	case EV_ABS: return "EV_ABS";
	default:     return "EV_OTHER";
	}
}

static const char *key_name(uint16_t code)
{
	switch (code) {
	case BTN_TOUCH: return "BTN_TOUCH";
	case BTN_TOOL_FINGER: return "BTN_TOOL_FINGER";
	default: return "KEY";
	}
}

static const char *abs_name(uint16_t code)
{
	switch (code) {
	case ABS_X: return "ABS_X";
	case ABS_Y: return "ABS_Y";
	case ABS_PRESSURE: return "ABS_PRESSURE";
	case ABS_MT_SLOT: return "ABS_MT_SLOT";
	case ABS_MT_TOUCH_MAJOR: return "ABS_MT_TOUCH_MAJOR";
	case ABS_MT_POSITION_X: return "ABS_MT_POSITION_X";
	case ABS_MT_POSITION_Y: return "ABS_MT_POSITION_Y";
	case ABS_MT_TRACKING_ID: return "ABS_MT_TRACKING_ID";
	default: return "ABS";
	}
}

static void report_event(const struct input_event *ev)
{
	if (ev->type == EV_SYN) {
		printf("EV_SYN code=%u value=%d\n", ev->code, ev->value);
	} else if (ev->type == EV_KEY) {
		printf("EV_KEY %s code=%u value=%d\n",
		       key_name(ev->code), ev->code, ev->value);
	} else if (ev->type == EV_ABS) {
		printf("EV_ABS %s code=%u value=%d\n",
		       abs_name(ev->code), ev->code, ev->value);
	} else {
		printf("%s code=%u value=%d\n",
		       event_name(ev->type), ev->code, ev->value);
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/input/event0";
	struct input_event events[16];
	ssize_t count;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [/dev/input/eventN]\n", argv[0]);
		return 2;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "input-events: cannot open %s: %s\n",
			path, strerror(errno));
		return 1;
	}

	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	printf("input-events: reading %s\n", path);
	fflush(stdout);

	while (running) {
		size_t i;
		size_t events_count;

		count = read(fd, events, sizeof(events));
		if (count < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "input-events: read %s: %s\n",
				path, strerror(errno));
			close(fd);
			return 1;
		}
		if (count % sizeof(events[0]) != 0) {
			fprintf(stderr, "input-events: short input_event record\n");
			close(fd);
			return 1;
		}

		events_count = (size_t)count / sizeof(events[0]);
		for (i = 0; i < events_count; i++)
			report_event(&events[i]);
	}

	close(fd);
	return 0;
}
