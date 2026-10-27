// SPDX-License-Identifier: BSD-3-Clause

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static int show_timestamp;
static int summary_only;

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
	if (show_timestamp) {
		printf("%ld.%06ld ", (long)ev->time.tv_sec,
		       (long)ev->time.tv_usec);
	}

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
	const char *path = "/dev/input/event0";
	struct input_event events[16];
	struct sigaction action;
	struct pollfd pollfd;
	long long first_us = 0;
	long long previous_sync_us = 0;
	long long min_period_us = 0;
	long long max_period_us = 0;
	unsigned long sync_frames = 0;
	ssize_t count;
	int fd, opt;

	while ((opt = getopt(argc, argv, "st")) != -1) {
		switch (opt) {
		case 's':
			summary_only = 1;
			break;
		case 't':
			show_timestamp = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-s] [-t] [/dev/input/eventN]\n",
				argv[0]);
			return 2;
		}
	}
	if (optind < argc)
		path = argv[optind];
	if (optind + 1 < argc) {
		fprintf(stderr, "usage: %s [-s] [-t] [/dev/input/eventN]\n",
			argv[0]);
		return 2;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "input-events: cannot open %s: %s\n",
			path, strerror(errno));
		return 1;
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = stop;
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	printf("input-events: reading %s\n", path);
	fflush(stdout);

	pollfd.fd = fd;
	pollfd.events = POLLIN;

	while (running) {
		size_t i;
		size_t events_count;
		int ready;

		ready = poll(&pollfd, 1, 250);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "input-events: poll %s: %s\n",
				path, strerror(errno));
			close(fd);
			return 1;
		}
		if (ready == 0)
			continue;
		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "input-events: device %s disconnected\n", path);
			close(fd);
			return 1;
		}
		if (!(pollfd.revents & POLLIN))
			continue;

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
		for (i = 0; i < events_count; i++) {
			if (events[i].type == EV_SYN && events[i].code == SYN_REPORT) {
				long long sync_us = (long long)events[i].time.tv_sec *
						1000000 + events[i].time.tv_usec;

				if (!first_us)
					first_us = sync_us;
				if (previous_sync_us) {
					long long period = sync_us - previous_sync_us;

					if (!min_period_us || period < min_period_us)
						min_period_us = period;
					if (period > max_period_us)
						max_period_us = period;
				}
				previous_sync_us = sync_us;
				sync_frames++;
			}
			if (!summary_only)
				report_event(&events[i]);
		}
	}

	close(fd);
	if (summary_only && first_us) {
		long long duration_us = previous_sync_us - first_us;
		double rate = duration_us > 0 ?
			(double)(sync_frames - 1) * 1000000.0 / duration_us : 0.0;

		printf("input-events: frames=%lu duration=%.3fs rate=%.1fHz "
		       "period-min=%.3fms period-max=%.3fms\n",
		       sync_frames, duration_us / 1000000.0, rate,
		       min_period_us / 1000.0, max_period_us / 1000.0);
	}
	return 0;
}
