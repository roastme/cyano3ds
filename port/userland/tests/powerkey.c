// SPDX-License-Identifier: GPL-2.0
/*
 * powerkey.c - short power-button press => clean power off
 *
 * On the 3DS the power button is owned by the MCU; the kernel's mcu_buttons
 * input device reports it as KEY_POWER.  Nothing in this bring-up userspace
 * listened for it, so a short press did nothing and the console only went off
 * when the button was held long enough for the MCU/PMIC to cut power by
 * itself.  fastboot3DS and the homebrew menus instead power off on a short
 * press; this does the same.
 *
 * It scans /dev/input/event* for a device that can emit KEY_POWER (rather
 * than hard-coding event2, whose number depends on probe order), then calls
 * reboot(RB_POWER_OFF) on a press.  The kernel side (mcu/regulator.c +
 * system-power-controller in the device tree) turns that into the MCU
 * power-down command.
 *
 * The scan is retried: the daemon is started before Android's private tmpfs
 * /dev replaces devtmpfs, and the input nodes may not exist yet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/reboot.h>

/* Log through /dev/kmsg: it needs no storage, unlike stdout (which init
 * points at a file on the SD card). */
static void klog(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY);

	if (fd < 0)
		return;
	write(fd, msg, strlen(msg));
	close(fd);
}

static int has_power_key(int fd)
{
	unsigned long bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			   (8 * sizeof(unsigned long))];

	memset(bits, 0, sizeof(bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
		return 0;
	return !!(bits[KEY_POWER / (8 * sizeof(unsigned long))] &
		  (1UL << (KEY_POWER % (8 * sizeof(unsigned long)))));
}

/* Open every /dev/input/event* that can emit KEY_POWER.  Returns the count. */
static int scan_inputs(int *fds, int max)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)) != NULL && n < max) {
		char path[128];
		int fd;

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (has_power_key(fd)) {
			/* Grab it: Android's EventHub opens the same node and
			 * reading without the grab stopped working once the
			 * framework was up. */
			if (ioctl(fd, EVIOCGRAB, 1) < 0)
				printf("powerkey: EVIOCGRAB %s failed: %s\n",
				       path, strerror(errno));
			printf("powerkey: listening on %s\n", path);
			fds[n++] = fd;
		} else {
			close(fd);
		}
	}
	closedir(d);
	return n;
}

int main(void)
{
	int fds[64];
	int n, i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Bring-up probe: keep the fallback snapshots in diagnostic builds, but
	 * omit them from the minimal-load boot.  These probes fork many shell
	 * tools and SIGQUIT every Dalvik VM, which competes with first-frame
	 * rendering on this single-core device. */
	if (access("/etc/3ds-minimal-diagnostics", F_OK) != 0 && fork() == 0) {
		sleep(150);
		system("/bin/lateprobe pk1 quick");
		sleep(120);
		system("/bin/lateprobe pk2 java");
		sleep(120);
		system("/bin/lateprobe pk3 java");
		_exit(0);
	}

	for (;;) {
		n = scan_inputs(fds, sizeof(fds) / sizeof(fds[0]));
		if (n > 0)
			break;
		sleep(1);
	}

	for (;;) {
		fd_set rfds;
		int maxfd = -1;

		FD_ZERO(&rfds);
		for (i = 0; i < n; i++) {
			FD_SET(fds[i], &rfds);
			if (fds[i] > maxfd)
				maxfd = fds[i];
		}
		if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 0; i < n; i++) {
			struct input_event ev;

			if (!FD_ISSET(fds[i], &rfds))
				continue;
			while (read(fds[i], &ev, sizeof(ev)) ==
			       (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.code != KEY_POWER)
					continue;
				if (ev.value != 1)
					continue;
				/* Clean shutdown: broadcast ACTION_SHUTDOWN, stop the
				 * Android processes, flush the persistent /data subset,
				 * sync, then power off.  This is the short-press path;
				 * a long hold still force-powers-off in the MCU/PMIC, so
				 * a wedged shutdown cannot brick the console.  /bin/shutdown
				 * excludes this process, so if it ever returns we fall back
				 * to the old hard path rather than spin. */
				klog("powerkey: KEY_POWER pressed -> clean shutdown\n");
				system("/bin/shutdown");
				klog("powerkey: shutdown returned -> hard power off\n");
				system("/bin/timeout 30 /bin/dataflush");
				reboot(RB_POWER_OFF);
				sleep(1);
				reboot(RB_POWER_OFF);
			}
		}
	}
	return 0;
}
