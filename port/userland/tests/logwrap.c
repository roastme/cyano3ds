// SPDX-License-Identifier: GPL-2.0
/*
 * logwrap.c - run a command and report how it terminated through /dev/kmsg
 *
 * Bring-up only.  Android's init restarts a service whose process exits, but
 * it sends the service's stdout/stderr to /dev/null and logs nothing useful.
 * Zygote was exiting part-way through preloadClasses with no exception in
 * logcat, no new tombstone and no kernel signal message, so this wrapper:
 *   - keeps the child's stdout/stderr in a *tmpfs* file that a flusher copies
 *     to the SD card (see below),
 *   - logs "exited with status N" or "killed by signal N" to /dev/kmsg.
 *
 * The stderr sink used to be /mnt/sd/CYANO3DS/service-stderr.log directly.
 * That made every wrapped service (zygote, servicemanager, media) share a
 * blocking dependency on the SD write path: if the card wedged, the first
 * message they printed blocked them.  Writing to tmpfs instead keeps the
 * services running and a bounded flusher in /init mirrors the file to the SD.
 *
 * Usage (from init.rc):  service zygote /bin/logwrap /system/bin/app_process ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/wait.h>

static void klog(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n, fd;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	buf[n] = '\0';

	fd = open("/dev/kmsg", O_WRONLY);
	if (fd < 0)
		return;
	write(fd, buf, strlen(buf));
	close(fd);
}

int main(int argc, char **argv)
{
	pid_t pid;
	int st, fd;

	if (argc < 2) {
		fprintf(stderr, "usage: logwrap cmd [args...]\n");
		return 2;
	}

	klog("logwrap: starting %s\n", argv[1]);

	pid = fork();
	if (pid < 0) {
		klog("logwrap: fork: %d\n", errno);
		return 1;
	}
	if (pid == 0) {
		/* Keep the child's stderr so a message that only goes there is
		 * not lost (init points it at /dev/null) - but never let that
		 * sink be the SD card, which would block the service on a wedged
		 * card (see the header).  /mnt/probe is a private tmpfs; a flusher
		 * in /init mirrors the file to the SD card. */
		fd = open("/mnt/probe/service-stderr.log",
			  O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd < 0)
			fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			char hdr[256];
			int n = snprintf(hdr, sizeof(hdr),
					"\n===== %s =====\n", argv[1]);
			if (n > 0)
				write(fd, hdr, n);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
		execv(argv[1], &argv[1]);
		klog("logwrap: exec %s failed: %d\n", argv[1], errno);
		_exit(127);
	}

	if (waitpid(pid, &st, 0) < 0) {
		klog("logwrap: waitpid: %d\n", errno);
		return 1;
	}
	if (WIFSIGNALED(st))
		klog("logwrap: %s killed by signal %d\n", argv[1], WTERMSIG(st));
	else
		klog("logwrap: %s exited with status %d\n", argv[1],
		     WEXITSTATUS(st));
	return 0;
}
