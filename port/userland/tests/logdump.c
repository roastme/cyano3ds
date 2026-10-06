// SPDX-License-Identifier: GPL-2.0
/*
 * logdump.c - print Android logger entries as text
 *
 * The port's own reader for /dev/log/main and /dev/log/radio.  The stock logcat
 * would work too, but it needs a full /system to be mounted, needs the ioctl
 * set to behave, and lives on the card; this tool is static, lives in the
 * initramfs, and is started *before* Android's init runs, so the very first
 * Android messages are captured even if Android itself dies immediately.
 *
 * Entry format (see the kernel's drivers/staging/android/logger.c):
 *
 *   struct logger_entry { u16 len; u16 __pad; s32 pid; s32 tid;
 *                         s32 sec; s32 nsec; char msg[]; }
 *
 * where msg[] is  [priority][tag NUL][text].
 *
 * Usage: logdump [device ...]        (default: /dev/log/main /dev/log/radio)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>

#define ENTRY_HDR 20
#define MAX_ENTRY (4096 + 64)

static int dump_dev(const char *dev)
{
	unsigned char buf[MAX_ENTRY];
	int since_sync = 0;
	int fd = open(dev, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "logdump: cannot open %s: %s\n", dev, strerror(errno));
		return -1;
	}
	fprintf(stderr, "logdump: reading %s\n", dev);

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		struct tm tm;
		time_t t;
		unsigned char *m;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "logdump: read %s: %s\n", dev, strerror(errno));
			return -1;
		}
		if (n < ENTRY_HDR + 2)
			continue;

		{
			unsigned short len;
			int sec, pid;

			memcpy(&len, buf, 2);
			/* sec is at offset 12: len(2)+pad(2)+pid(4)+tid(4) */
			memcpy(&sec, buf + 12, 4);
			memcpy(&pid, buf + 4, 4);
			if (len == 0 || ENTRY_HDR + len > (size_t)n)
				continue;

			t = sec;
			localtime_r(&t, &tm);
			m = buf + ENTRY_HDR;

			/* payload = [priority][tag NUL][text]; render the tag
			 * separately so the pid and priority are visible.  The
			 * first version printed the payload verbatim and the
			 * (then-buggy) kernel had already eaten the head of every
			 * line, which hid which process logged what. */
			{
				unsigned int i, taglen = 0;
				int prio = (m[0] >= '0' && m[0] <= '9') ? m[0] : '?';

				while (1 + taglen < len && m[1 + taglen] != 0)
					taglen++;
				printf("%02d:%02d:%02d %5d %c %.*s: ",
				       tm.tm_hour, tm.tm_min, tm.tm_sec,
				       pid, prio, (int)taglen, m + 1);
				for (i = 1 + taglen + 1; i < len; i++) {
					unsigned char c = m[i];

					if (c == 0)
						continue;
					if (c < 0x20 || c > 0x7e)
						c = '.';
					putchar(c);
				}
				putchar('\n');
			}
			fflush(stdout);
			/* make it durable too: an Android crash right after the last
			 * message must still be readable from the SD card */
			if (++since_sync >= 16) {
				since_sync = 0;
				fsync(fileno(stdout));
			}
		}
	}
}

int main(int argc, char **argv)
{
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc > 1) {
		for (i = 1; i < argc; i++)
			dump_dev(argv[i]);
		return 0;
	}

	/* One process per device; the main log is the interesting one. */
	dump_dev("/dev/log/main");
	dump_dev("/dev/log/radio");
	return 0;
}
