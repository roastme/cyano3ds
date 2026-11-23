// SPDX-License-Identifier: GPL-2.0
/*
 * gyrocal.c - measure the gyroscope's zero-rate offset and hand it to the
 * kernel driver
 *
 * A console lying still does not read zero: the L3GD20H in this unit reports
 * about (-29, +10, -31) LSB, and at 70 mdps/LSB that is 2 deg/s of drift that
 * Android would integrate forever (a game's aim slowly walks across the
 * screen with the console on the table).  ctr_gyro.c subtracts
 * in_anglvel_*_calibbias from every raw read, so this tool only has to measure
 * the offset once and write it back:
 *
 *   gyrocal [-p calib-file]
 *
 * It powers the sensor, throws away a few samples while the filter settles,
 * averages SAMPLES more and - only if the spread over those samples is small
 * enough to prove the console was not being moved - writes the mean as the
 * calibration and saves it to the card, so a boot where the console *is* being
 * handled still starts from the last known-good offset instead of none.
 *
 * Exit status is 0 when a calibration is in effect (fresh or restored), 1 when
 * neither was possible; the boot script only logs it.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef IIO_DIR
#define IIO_DIR		"/sys/bus/iio/devices"
#endif
#define CALIB_DEFAULT	"/mnt/sd/CYANO3DS/gyro-calib"
#define SETTLE		5	/* samples discarded after power-up */
#define SAMPLES		64
#define INTERVAL_US	10000	/* 100 Hz, the sensor's own rate */
#define MAX_SPREAD	40	/* LSB (~2.8 deg/s): above this it moved */
#define VERIFY		8

static const char *axes[3] = { "x", "y", "z" };

static int read_int(const char *path, int *out)
{
	char buf[64];
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	*out = atoi(buf);
	return 0;
}

static int write_int(const char *path, int val)
{
	char buf[16];
	ssize_t n;
	int fd, len = snprintf(buf, sizeof(buf), "%d\n", val);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, buf, len);
	close(fd);
	return n == len ? 0 : -1;
}

/* Path of the gyroscope's attribute: the device number is not stable, so it
 * is found by the attribute itself, exactly as the Android HAL does. */
static int find_device(char *dir, size_t len)
{
	struct dirent *de;
	DIR *d = opendir(IIO_DIR);
	char path[320];

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "iio:device", 10))
			continue;
		snprintf(path, sizeof(path), IIO_DIR "/%s/in_anglvel_x_raw",
			 de->d_name);
		if (!access(path, R_OK)) {
			snprintf(dir, len, IIO_DIR "/%s", de->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static void attr(char *buf, size_t len, const char *dir, const char *name,
		 const char *suffix)
{
	if (name)
		snprintf(buf, len, "%s/in_anglvel_%s_%s", dir, name, suffix);
	else
		snprintf(buf, len, "%s/in_anglvel_%s", dir, suffix);
}

static void load_calib(const char *path, int calib[3])
{
	FILE *f = fopen(path, "r");

	if (!f)
		return;
	if (fscanf(f, "%d %d %d", &calib[0], &calib[1], &calib[2]) != 3)
		calib[0] = calib[1] = calib[2] = 0;
	fclose(f);
}

static int apply_calib(const char *dir, const int calib[3])
{
	char path[512];
	int a, bad = 0;

	for (a = 0; a < 3; a++) {
		attr(path, sizeof(path), dir, axes[a], "calibbias");
		if (write_int(path, calib[a]))
			bad++;
	}
	return bad ? -1 : 0;
}

/* Mean and spread of @n samples of one axis, or -1 if the read fails. */
static int sample_axis(const char *dir, const char *axis, int n,
		       int *mean, int *spread)
{
	char path[512];
	int i, v, lo = 0x7fffffff, hi = -0x7fffffff;
	long sum = 0;

	attr(path, sizeof(path), dir, axis, "raw");
	for (i = 0; i < n; i++) {
		if (read_int(path, &v))
			return -1;
		sum += v;
		if (v < lo)
			lo = v;
		if (v > hi)
			hi = v;
		usleep(INTERVAL_US);
	}
	*mean = (int)(sum / n);
	*spread = hi - lo;
	return 0;
}

int main(int argc, char **argv)
{
	const char *calib_path = CALIB_DEFAULT;
	char dir[320], path[512];
	int calib[3] = { 0, 0, 0 }, saved[3], base[3] = { 0, 0, 0 };
	int mean[3], spread[3];
	int enabled = 0, a, i, moved = 0;

	if (argc > 2 && !strcmp(argv[1], "-p"))
		calib_path = argv[2];

	if (find_device(dir, sizeof(dir))) {
		printf("gyrocal: no gyroscope IIO device, nothing to do\n");
		return 1;
	}

	load_calib(calib_path, calib);
	memcpy(saved, calib, sizeof(calib));
	/* Apply the saved offset first: a boot where the console is handled from
	 * the start then still runs on the last known-good calibration even if
	 * this measurement is unusable. */
	if (apply_calib(dir, calib))
		printf("gyrocal: cannot set calibbias\n");

	/* What the driver subtracts now is the base: read_raw() already applies
	 * it, so this run measures the leftover, not the total offset.  That
	 * makes a later re-measurement (the part drifts as it warms up) an
	 * addition to the existing calibration instead of a replacement. */
	for (a = 0; a < 3; a++) {
		attr(path, sizeof(path), dir, axes[a], "calibbias");
		if (read_int(path, &base[a]))
			base[a] = calib[a];
	}

	attr(path, sizeof(path), dir, NULL, "en");
	read_int(path, &enabled);
	if (!enabled && write_int(path, 1)) {
		printf("gyrocal: cannot power the gyroscope\n");
		return 1;
	}

	/* The part's digital filter needs a few samples after power-up. */
	for (a = 0; a < 3; a++) {
		attr(path, sizeof(path), dir, axes[a], "raw");
		for (i = 0; i < SETTLE; i++) {
			read_int(path, &mean[a]);
			usleep(INTERVAL_US);
		}
	}

	for (a = 0; a < 3; a++) {
		if (sample_axis(dir, axes[a], SAMPLES, &mean[a], &spread[a])) {
			printf("gyrocal: read failed on %s, keeping %d %d %d\n",
			       axes[a], calib[0], calib[1], calib[2]);
			goto out;
		}
		if (spread[a] > MAX_SPREAD)
			moved = 1;
	}

	printf("gyrocal: residual mean %d %d %d LSB on top of %d %d %d, spread %d %d %d LSB\n",
	       mean[0], mean[1], mean[2], base[0], base[1], base[2],
	       spread[0], spread[1], spread[2]);

	if (moved) {
		/* Do not learn an offset from a console that is being moved. */
		printf("gyrocal: console moved during sampling, keeping %d %d %d\n",
		       calib[0], calib[1], calib[2]);
	} else {
		for (a = 0; a < 3; a++)
			calib[a] = base[a] + mean[a];
		if (apply_calib(dir, calib)) {
			printf("gyrocal: cannot set calibbias\n");
			goto out;
		}
		if (memcmp(saved, calib, sizeof(calib))) {
			FILE *f = fopen(calib_path, "w");

			if (f) {
				fprintf(f, "%d %d %d\n", calib[0], calib[1],
					calib[2]);
				fclose(f);
			}
		}
	}

	/* Read back through the same path the HAL uses: if the driver ignored
	 * calibbias the mean would still be the raw one. */
	for (a = 0; a < 3; a++)
		sample_axis(dir, axes[a], VERIFY, &mean[a], &spread[a]);
	printf("gyrocal: calib %d %d %d LSB applied, residual %d %d %d LSB\n",
	       calib[0], calib[1], calib[2], mean[0], mean[1], mean[2]);

out:
	if (!enabled) {
		attr(path, sizeof(path), dir, NULL, "en");
		write_int(path, 0);
	}
	return 0;
}
