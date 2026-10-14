// SPDX-License-Identifier: GPL-2.0
/*
 * fakevold.c - tell Android the SD card is mounted
 *
 * Gingerbread decides whether external storage exists by asking vold over the
 * "vold" socket (MountService, Environment.getExternalStorageState()).  On
 * this port the card is not vold's to mount: the bring-up initramfs mounts it
 * long before Android starts (it holds /system, the /data tars and the logs)
 * and bind-mounts it at /mnt/sdcard with vold's ownership and permissions.
 * The real vold, with no vold.fstab entry for it, reports no volume, so
 * Android says "removed": the Camera app refuses to start ("Please insert an
 * SD card"), the Gallery and Music apps show nothing and downloads fail.  A
 * second, real vold mount is not an option either: vold runs fsck_msdos on
 * the device first, on a filesystem that is in use.
 *
 * So this stands in for vold and answers Gingerbread's MountService protocol
 * (no sequence numbers; "<code> <text>\0" replies, 1xx partial, 2xx-5xx
 * final) with a single volume, sdcard at /mnt/sdcard, in state 4 (Mounted):
 *
 *   volume list           110 sdcard /mnt/sdcard 4 ... 200
 *   share status ums      212 ums unavailable           (no USB storage)
 *   asec list             200 (no secure containers)
 *   storage users PATH    200
 *   volume mount PATH     200 (it already is)
 *   anything that would unmount, format or share the card: 405 refused
 *   anything else         200
 *
 * The listening socket comes from Android's init ("socket vold stream 0660
 * root mount" in init.rc, ANDROID_SOCKET_vold); run by hand it binds
 * /dev/socket/vold itself.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define LABEL		"sdcard"
#define MOUNTPOINT	"/mnt/sdcard"
#define STATE_MOUNTED	4

static void logk(const char *fmt, const char *arg)
{
	FILE *k = fopen("/dev/kmsg", "w");

	if (!k)
		return;
	fprintf(k, "fakevold: ");
	fprintf(k, fmt, arg);
	fprintf(k, "\n");
	fclose(k);
}

static int reply(int fd, const char *msg)
{
	size_t n = strlen(msg) + 1;	/* the terminating NUL is the separator */

	return write(fd, msg, n) == (ssize_t)n ? 0 : -1;
}

static int handle(int fd, char *cmd)
{
	char line[256];

	if (!strcmp(cmd, "volume list")) {
		snprintf(line, sizeof(line), "110 %s %s %d", LABEL, MOUNTPOINT,
			 STATE_MOUNTED);
		if (reply(fd, line))
			return -1;
		return reply(fd, "200 Volumes listed.");
	}
	if (!strncmp(cmd, "share status", 12))
		return reply(fd, "212 ums unavailable");
	if (!strcmp(cmd, "asec list"))
		return reply(fd, "200 asec operation succeeded");
	if (!strncmp(cmd, "storage users", 13))
		return reply(fd, "200 Storage list completed");
	if (!strncmp(cmd, "volume mount", 12))
		return reply(fd, "200 volume operation succeeded");
	if (!strncmp(cmd, "volume unmount", 14) ||
	    !strncmp(cmd, "volume format", 13) ||
	    !strncmp(cmd, "volume share", 12) ||
	    !strncmp(cmd, "asec create", 11)) {
		logk("refused \"%s\": the card is owned by the bring-up init", cmd);
		return reply(fd, "405 Operation not supported on this device");
	}
	return reply(fd, "200 Command succeeded");
}

static void serve(int fd)
{
	char buf[1024];
	size_t have = 0;

	for (;;) {
		ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
		char *start, *end;

		if (n <= 0)
			return;
		have += n;
		buf[have] = '\0';
		start = buf;
		/* commands are NUL-terminated (some clients add a newline) */
		while ((end = memchr(start, '\0', buf + have - start)) &&
		       end < buf + have) {
			char *nl = strchr(start, '\n');

			if (nl)
				*nl = '\0';
			if (*start && handle(fd, start))
				return;
			start = end + 1;
		}
		have = buf + have - start;
		memmove(buf, start, have);
		if (have >= sizeof(buf) - 1)
			have = 0;	/* oversized garbage: drop it */
	}
}

int main(void)
{
	const char *env = getenv("ANDROID_SOCKET_vold");
	int lfd;

	signal(SIGPIPE, SIG_IGN);
	if (env) {
		lfd = atoi(env);
	} else {
		struct sockaddr_un a;

		lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		memset(&a, 0, sizeof(a));
		a.sun_family = AF_UNIX;
		strcpy(a.sun_path, "/dev/socket/vold");
		mkdir("/dev/socket", 0755);
		unlink(a.sun_path);
		if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
			perror("fakevold: bind /dev/socket/vold");
			return 1;
		}
		chmod(a.sun_path, 0666);
	}
	if (listen(lfd, 4) < 0) {
		perror("fakevold: listen");
		return 1;
	}
	logk("serving %s as mounted", MOUNTPOINT);
	for (;;) {
		int fd = accept(lfd, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			sleep(1);
			continue;
		}
		serve(fd);
		close(fd);
	}
}
