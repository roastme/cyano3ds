// SPDX-License-Identifier: GPL-2.0
/*
 * kmsgdump.c - stream the kernel log to a file, durable per message
 *
 * Replaces the initramfs' old "cat /dev/kmsg > file &" for one reason: a hard
 * hang (or an oops that wedges the machine) can happen milliseconds after the
 * interesting message, and with the plain cat the last messages sit in the page
 * cache and never reach the SD card.  That is exactly how the first Android
 * boot attempt lost the "exception" the console showed.
 *
 * This tool:
 *   - opens /dev/kmsg, seeks to the oldest record (so the whole boot log is
 *     captured, no separate dmesg snapshot needed),
 *   - drains everything available, writes it out, then fflush+fsync,
 *   - sleeps briefly and repeats.
 *
 * Reading /dev/kmsg consumes records for *this* reader only; the printk console
 * path is independent, so the top screen keeps showing everything too.
 *
 * Usage: kmsgdump <output-file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define READ_CHUNK 8192

int main(int argc, char **argv)
{
	unsigned char buf[READ_CHUNK];
	const char *path = argc > 1 ? argv[1] : "/mnt/sd/CYANO3DS/kmsg.log";
	off_t dropped = 0;
	int fd, out;

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "kmsgdump: cannot open /dev/kmsg: %s\n", strerror(errno));
		return 1;
	}
	/* 0 = oldest record still in the ring buffer */
	if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
		fprintf(stderr, "kmsgdump: lseek(0) failed: %s\n", strerror(errno));

	out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		fprintf(stderr, "kmsgdump: cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}

	fprintf(stderr, "kmsgdump: streaming /dev/kmsg -> %s\n", path);

	for (;;) {
		int got = 0;
		ssize_t n;

		while ((n = read(fd, buf, sizeof(buf))) > 0) {
			ssize_t off = 0;

			while (off < n) {
				ssize_t w = write(out, buf + off, n - off);

				if (w < 0) {
					if (errno == EINTR)
						continue;
					/* device full or gone: keep going, but say so */
					fprintf(stderr, "kmsgdump: write: %s\n",
						strerror(errno));
					break;
				}
				off += w;
			}
			got = 1;
		}
		if (n < 0 && errno == EPIPE)	/* ring buffer overrun indicator */
			dropped++;
		if (got) {
			fsync(out);		/* the whole point of this tool */
		}
		if (dropped && (dropped % 50) == 1)
			fprintf(stderr, "kmsgdump: %lld overruns so far\n",
				(long long)dropped);

		usleep(200 * 1000);
	}

	return 0;
}
