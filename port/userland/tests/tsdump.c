// SPDX-License-Identifier: GPL-2.0
/*
 * tsdump.c - dump raw evdev events from a 3DS input device to a log
 *
 * Bring-up tool for the M4 touchscreen work.  Android's EventHub classifies
 * the dedicated "Nintendo 3DS touchscreen" input device as CLASS_TOUCHSCREEN
 * (see KeyInputQueue: "X: min=0 max=319 ..."), but tapping the panel did
 * nothing.  This dumps the raw input_event stream so a single boot answers the
 * question the kernel log cannot: is the input core delivering ABS_X/ABS_Y +
 * BTN_TOUCH/SYN_REPORT to userspace at all?
 *
 * It runs alongside Android (each open fd of an evdev node gets its own event
 * queue), and flushes every line so the data lands on the SD card even if the
 * system is later killed.
 *
 * Usage: tsdump [device]
 *   No argument (or a device that will not open) makes it scan /dev/input for
 *   the touchscreen, i.e. a node that reports BTN_TOUCH plus ABS_X and ABS_Y,
 *   so it does not depend on the evdev minor that probe order happened to
 *   produce.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

static int has_bit(const unsigned long *bits, int bit)
{
	return !!(bits[bit / (8 * sizeof(unsigned long))] &
		  (1UL << (bit % (8 * sizeof(unsigned long)))));
}

/* Return 1 if the (already open) fd is a single-touch touchscreen. */
static int is_touchscreen(int fd)
{
	unsigned long absbits[(ABS_MAX + 8 * sizeof(unsigned long)) /
			      (8 * sizeof(unsigned long))];
	unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			      (8 * sizeof(unsigned long))];

	memset(absbits, 0, sizeof(absbits));
	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
		return 0;
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
		return 0;
	return has_bit(absbits, ABS_X) && has_bit(absbits, ABS_Y) &&
	       has_bit(keybits, BTN_TOUCH);
}

/* Open the touchscreen, preferring an explicit path, then scanning. */
static int open_touchscreen(const char *path, char *found, size_t foundlen)
{
	int i, fd = -1;

	if (path && (fd = open(path, O_RDONLY)) >= 0) {
		snprintf(found, foundlen, "%s", path);
		return fd;
	}

	for (i = 0; i < 16; i++) {
		char p[64];

		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (is_touchscreen(fd)) {
			snprintf(found, foundlen, "%s", p);
			/* the scan opened it O_NONBLOCK; block for events */
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *want = argc > 1 ? argv[1] : NULL;
	struct input_event ev;
	char path[64] = "?";
	char name[256] = "?";
	int fd;

	fd = open_touchscreen(want, path, sizeof(path));
	if (fd < 0) {
		printf("tsdump: no touchscreen node found (%s)\n", strerror(errno));
		fflush(stdout);
		return 1;
	}

	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
		strcpy(name, "?");

	printf("tsdump: opened %s name='%s' evsize=%zu\n",
	       path, name, sizeof(ev));
	fflush(stdout);

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		printf("tsdump: type=%u code=%u value=%d\n",
		       ev.type, ev.code, ev.value);
		fflush(stdout);
	}

	printf("tsdump: read %s: %s\n", path, strerror(errno));
	fflush(stdout);
	close(fd);
	return 0;
}
