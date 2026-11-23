/*
 * sensors_nintendo3ds.c - Android sensors HAL for the Nintendo 3DS
 *
 * Part of the Cyano3DS port (CyanogenMod 7.2, Android 2.3.7, API 10).
 *
 * There is no Android sensor kernel ABI on this port: the accelerometer is
 * the MCU's LIS331DLH (drivers/platform/nintendo3ds/mcu/accel.c) and the New
 * 3DS gyroscope is an InvenSense ITG-1010 on the XL / New 2DS XL and an ST
 * L3GD20H on the New 3DS (ctr_gyro.c), both exposed as standard IIO devices.
 * This HAL finds them by their IIO channel attributes
 * (in_accel_* and in_anglvel_*), enables them with in_*_en, polls the raw
 * values at the rate SensorService asks for, converts them to Android units
 * and reports normal sensors_event_t events.
 *
 * The gyro's IIO device name ("itg1010" or "l3gd20h", set by ctr_gyro.c
 * from the WHO_AM_I it found) becomes the sensor name apps display.
 *
 * Axis mapping
 * ------------
 * Azahar documents the console's own motion frame in core/frontend/input.h
 * (taken from hardware tests): x+ = LEFT on the D-pad, y+ = normal to the
 * touch screen pointing out of it, z+ = UP on the D-pad.  hid.cpp converts
 * that frame to the raw registers this port's drivers expose:
 *
 *     accel raw = (-2*x, -2*z,  2*y)      gyro raw = ( x,  z, -y)
 *
 * so in both cases raw x is the D-pad-left axis, raw y the D-pad-up axis and
 * raw z the out-of-screen axis.  The selftest confirms the layout: a console
 * lying flat on a table reports raw = (650, 580, -16276), i.e. one g on raw z
 * alone, and |raw| = 9.75 m/s^2 with the driver's scale, so the registers hold
 * the LIS331's 12-bit values left-justified (1024/g << 4 = 16384/g).
 *
 * Android wants its own frame (x+ right, y+ up, z+ out of the screen) and, for
 * the accelerometer, the *up* vector - the direction of the sky, so a console
 * lying flat must read +g on z.  Raw z reads -g in that pose, which fixes the
 * sign of that axis; the remaining one is x, whose console +direction is the
 * D-pad's left while Android's +x is right, so it is negated as well:
 *
 *     android_x = -raw_x      (D-pad left -> screen right)
 *     android_y = +raw_y      (D-pad up   -> screen up)
 *     android_z = -raw_z      (registers report gravity, Android the up vector)
 *
 * Both sensors end up with the same map: the gyro's raw signs are the opposite
 * of the accelerometer's in hid.cpp, and the accelerometer needs its vector
 * negated (gravity -> up), which cancels out.
 *
 * This is worth getting exactly right because WindowOrientationListener in
 * CM7 is unforgiving.  With raw_z on Android's y (the first version of this
 * HAL) a console lying still on a table read (0.39, -9.75, 0.35) and the
 * framework computed -atan2(-x, y) = 177.7 degrees, its ROTATION_180 band:
 * "reverse portrait" while the console sat on the table.  With raw_z on
 * Android's z the tilt is 90 degrees, past its MAX_TILT of 75, so orientation
 * is ignored entirely and the launcher stays in landscape.
 *
 * The map lives in map_axis() so a test on hardware only needs to change one
 * function.
 *
 * License: Apache-2.0, like the Android HALs it implements.
 */

#define LOG_TAG "Sensors3DS"

#include <hardware/hardware.h>
#include <hardware/sensors.h>

#include <cutils/log.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SENSOR_COUNT 2
#define ACCEL 0
#define GYRO  1

/* IIO devices live one directory below this */
#define IIO_DEVICES "/sys/bus/iio/devices"

struct sensor_desc {
	const char *prefix;		/* in_accel / in_anglvel */
	const char *name;
	const char *vendor;
	int type;
	int handle;
	float power;
	int32_t min_delay_us;

	bool present;
	char dir[PATH_MAX];
	float scale;			/* SI unit per raw LSB */
};

/*
 * The handles are indices into this array.  They are also the sensor_t
 * handles SensorService uses to call activate()/setDelay().
 */
static struct sensor_desc g_desc[SENSOR_COUNT] = {
	[ACCEL] = {
		.prefix = "in_accel",
		.name = "LIS331DLH 3-axis accelerometer",
		.vendor = "STMicroelectronics",
		.type = SENSOR_TYPE_ACCELEROMETER,
		.handle = ACCEL,
		.power = 0.1f,
		.min_delay_us = 20000,	/* the MCU driver samples at 50 Hz */
	},
	[GYRO] = {
		.prefix = "in_anglvel",
		/* replaced by the part ctr_gyro.c reports in <dir>/name */
		.name = "3-axis gyroscope",
		.vendor = "Nintendo",
		.type = SENSOR_TYPE_GYROSCOPE,
		.handle = GYRO,
		.power = 3.6f,
		.min_delay_us = 10000,	/* ctr_gyro.c runs at 100 Hz */
	},
};

/*
 * ctr_gyro.c names its IIO device after the part it identified, so the sensor
 * list shows the chip that is actually on this console instead of whichever
 * one the port was written against.
 */
static int read_text_file(const char *path, char *buf, size_t size);

static void resolve_gyro_name(struct sensor_desc *d, const char *dir)
{
	char path[PATH_MAX], name[64];

	snprintf(path, sizeof(path), "%s/name", dir);
	if (read_text_file(path, name, sizeof(name)) < 0)
		return;

	if (strstr(name, "l3gd20")) {
		d->name = "L3GD20H 3-axis gyroscope";
		d->vendor = "STMicroelectronics";
	} else if (strstr(name, "itg1010")) {
		d->name = "ITG-1010 3-axis gyroscope";
		d->vendor = "InvenSense";
	}
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_scanned;
static struct sensor_t g_sensor_list[SENSOR_COUNT];
static int g_sensor_count;

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int read_text_file(const char *path, char *buf, size_t size)
{
	int fd;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

static int read_float_file(const char *path, float *value)
{
	char buf[64];

	if (read_text_file(path, buf, sizeof(buf)) < 0)
		return -1;
	*value = (float)atof(buf);
	return 0;
}

static bool file_exists(const char *path)
{
	return access(path, R_OK) == 0;
}

/*
 * Find the IIO devices.  The IIO device number is not stable, so match on
 * the channel attribute names instead of on iio:deviceN or the device name.
 */
static void scan_sensors(void)
{
	DIR *dir;
	struct dirent *de;
	char path[PATH_MAX];
	int i, n = 0;

	pthread_mutex_lock(&g_lock);
	if (g_scanned) {
		pthread_mutex_unlock(&g_lock);
		return;
	}
	g_scanned = true;

	dir = opendir(IIO_DEVICES);
	if (!dir) {
		LOGE("cannot open %s: %s", IIO_DEVICES, strerror(errno));
		g_scanned = false;	/* retry if sysfs was not mounted yet */
		pthread_mutex_unlock(&g_lock);
		return;
	}

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "iio:device", 10) != 0)
			continue;

		for (i = 0; i < SENSOR_COUNT; i++) {
			struct sensor_desc *d = &g_desc[i];

			if (d->present)
				continue;

			snprintf(path, sizeof(path), "%s/%s/%s_x_raw",
				 IIO_DEVICES, de->d_name, d->prefix);
			if (!file_exists(path))
				continue;
			snprintf(path, sizeof(path), "%s/%s/%s_y_raw",
				 IIO_DEVICES, de->d_name, d->prefix);
			if (!file_exists(path))
				continue;
			snprintf(path, sizeof(path), "%s/%s/%s_z_raw",
				 IIO_DEVICES, de->d_name, d->prefix);
			if (!file_exists(path))
				continue;
			snprintf(path, sizeof(path), "%s/%s/%s_scale",
				 IIO_DEVICES, de->d_name, d->prefix);
			if (read_float_file(path, &d->scale) < 0 || d->scale <= 0.0f)
				continue;

			snprintf(d->dir, sizeof(d->dir), "%s/%s",
				 IIO_DEVICES, de->d_name);
			d->present = true;
			if (d->type == SENSOR_TYPE_GYROSCOPE)
				resolve_gyro_name(d, d->dir);
			LOGI("%s at %s (scale %.9f)", d->name, d->dir, d->scale);
		}
	}
	closedir(dir);

	/* The sensor list must stay valid for the lifetime of the module. */
	for (i = 0; i < SENSOR_COUNT; i++) {
		struct sensor_desc *d = &g_desc[i];
		struct sensor_t *s = &g_sensor_list[n];

		if (!d->present)
			continue;

		memset(s, 0, sizeof(*s));
		s->name = d->name;
		s->vendor = d->vendor;
		s->version = 1;
		s->handle = d->handle;
		s->type = d->type;
		s->maxRange = d->scale * 32768.0f;
		s->resolution = d->scale;
		s->power = d->power;
		s->minDelay = d->min_delay_us;
		n++;
	}
	g_sensor_count = n;

	if (n == 0)
		LOGW("no IIO accelerometer or gyroscope found");
	pthread_mutex_unlock(&g_lock);
}

static int sensors_get_sensors_list(struct sensors_module_t *module,
				    struct sensor_t const **list)
{
	(void)module;
	scan_sensors();
	*list = g_sensor_count ? g_sensor_list : NULL;
	return g_sensor_count;
}

struct poll_sensor {
	bool present;
	bool active;
	int raw_fd[3];
	char en_path[PATH_MAX];
	float scale;
	int type;
	int handle;
	int errors;
	int64_t delay_ns;
	int64_t next_ns;
};

struct sensors_ctx {
	struct sensors_poll_device_t device;
	pthread_mutex_t lock;
	int wake_fd[2];
	struct poll_sensor sensor[SENSOR_COUNT];
};

static int find_sensor(struct sensors_ctx *ctx, int handle)
{
	int i;

	for (i = 0; i < SENSOR_COUNT; i++)
		if (ctx->sensor[i].present && ctx->sensor[i].handle == handle)
			return i;
	return -1;
}

/*
 * Console frame -> Android screen frame, see the header comment.  The
 * accelerometer's raw values are the MCU's LIS331 registers, the gyro's are
 * the IMU's own little-endian output, so the two need different permutations.
 */
/* Name the pose the way a person holding the console would, so the log says
 * why WindowOrientationListener rotated (or refused to).  A console lying flat
 * reads ~1 g on Android z, which puts the tilt past the listener's MAX_TILT of
 * 75 degrees, so it ignores orientation entirely and the launcher stays in
 * landscape - that is the pose that must not rotate. */
static const char *pose_name(const float v[3])
{
	float ax = fabsf(v[0]), ay = fabsf(v[1]), az = fabsf(v[2]);

	if (az >= ax && az >= ay)
		return v[2] > 0 ? "flat, screen up" : "flat, screen down";
	if (ay >= ax)
		return v[1] > 0 ? "upright, hinge up" : "upside down";
	return v[0] > 0 ? "right side up" : "left side up";
}

static void map_axis(const int raw[3], float scale, float out[3])
{
	out[0] = -raw[0] * scale;	/* D-pad left -> screen right */
	out[1] = raw[1] * scale;	/* D-pad up   -> screen up */
	out[2] = -raw[2] * scale;	/* gravity -> up vector */
}

static int read_raw_value(int fd, int *value)
{
	char buf[32];
	char *end;
	ssize_t n;
	long v;

	n = pread(fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	errno = 0;
	v = strtol(buf, &end, 10);
	if (end == buf)
		return -1;
	*value = (int)v;
	return 0;
}

static int write_enable(const char *path, int enabled)
{
	int fd;
	ssize_t n;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	n = write(fd, enabled ? "1" : "0", 1);
	close(fd);
	return n == 1 ? 0 : -errno;
}

static void wake_poll(struct sensors_ctx *ctx)
{
	char c = 'W';

	/* The pipe is non-blocking; a full pipe already guarantees a wakeup. */
	(void)write(ctx->wake_fd[1], &c, 1);
}

static int poll_close(struct hw_device_t *dev)
{
	struct sensors_ctx *ctx = (struct sensors_ctx *)dev;
	int i, a;

	for (i = 0; i < SENSOR_COUNT; i++) {
		if (ctx->sensor[i].present && ctx->sensor[i].active &&
		    ctx->sensor[i].en_path[0])
			(void)write_enable(ctx->sensor[i].en_path, 0);
		for (a = 0; a < 3; a++)
			if (ctx->sensor[i].raw_fd[a] >= 0)
				close(ctx->sensor[i].raw_fd[a]);
	}
	close(ctx->wake_fd[0]);
	close(ctx->wake_fd[1]);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
	return 0;
}

static int poll_activate(struct sensors_poll_device_t *dev, int handle,
			 int enabled)
{
	struct sensors_ctx *ctx = (struct sensors_ctx *)dev;
	struct poll_sensor *s;
	int index, err;

	pthread_mutex_lock(&ctx->lock);
	index = find_sensor(ctx, handle);
	if (index < 0) {
		pthread_mutex_unlock(&ctx->lock);
		return -EINVAL;
	}
	s = &ctx->sensor[index];

	err = write_enable(s->en_path, enabled);
	if (!err) {
		s->active = !!enabled;
		s->errors = 0;
		s->next_ns = enabled ? now_ns() + s->delay_ns : 0;
		LOGI("%s %s", enabled ? "enabled" : "disabled",
		      g_desc[index].name);
	} else {
		LOGE("cannot %s %s: %s", enabled ? "enable" : "disable",
		      g_desc[index].name, strerror(-err));
	}
	pthread_mutex_unlock(&ctx->lock);

	if (enabled)
		wake_poll(ctx);
	return err;
}

static int poll_set_delay(struct sensors_poll_device_t *dev, int handle,
			  int64_t ns)
{
	struct sensors_ctx *ctx = (struct sensors_ctx *)dev;
	struct poll_sensor *s;
	int index;

	pthread_mutex_lock(&ctx->lock);
	index = find_sensor(ctx, handle);
	if (index < 0) {
		pthread_mutex_unlock(&ctx->lock);
		return -EINVAL;
	}
	s = &ctx->sensor[index];

	if (ns < (int64_t)g_desc[index].min_delay_us * 1000LL)
		ns = (int64_t)g_desc[index].min_delay_us * 1000LL;
	if (ns > 1000000000LL)
		ns = 1000000000LL;
	s->delay_ns = ns;
	pthread_mutex_unlock(&ctx->lock);

	wake_poll(ctx);
	return 0;
}

static int poll_events(struct sensors_poll_device_t *dev, sensors_event_t *data,
		       int count)
{
	struct sensors_ctx *ctx = (struct sensors_ctx *)dev;
	char drain[16];

	if (count <= 0)
		return -EINVAL;

	for (;;) {
		int64_t now, next = INT64_MAX;
		int i, timeout, n = 0;
		struct pollfd pfd;

		pthread_mutex_lock(&ctx->lock);
		now = now_ns();
		for (i = 0; i < SENSOR_COUNT; i++) {
			struct poll_sensor *s = &ctx->sensor[i];

			if (!s->present || !s->active)
				continue;
			/* First sample after activate() or a long stall. */
			if (s->next_ns == 0)
				s->next_ns = now;
			if (s->next_ns < next)
				next = s->next_ns;
		}
		pthread_mutex_unlock(&ctx->lock);

		if (next == INT64_MAX) {
			timeout = -1;	/* no active sensor; wake pipe only */
		} else {
			int64_t delta = next - now;

			timeout = delta <= 0 ? 0 :
				  (int)((delta + 999999LL) / 1000000LL);
		}

		pfd.fd = ctx->wake_fd[0];
		pfd.events = POLLIN;
		pfd.revents = 0;
		i = poll(&pfd, 1, timeout);
		if (i < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (pfd.revents & POLLIN) {
			while (read(ctx->wake_fd[0], drain, sizeof(drain)) > 0)
				;
		}
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EIO;

		now = now_ns();
		pthread_mutex_lock(&ctx->lock);
		for (i = 0; i < SENSOR_COUNT && n < count; i++) {
			struct poll_sensor *s = &ctx->sensor[i];
			float values[3];
			int raw[3], a;
			bool ok = true;

			if (!s->present || !s->active || s->next_ns > now)
				continue;

			for (a = 0; a < 3; a++) {
				if (read_raw_value(s->raw_fd[a], &raw[a]) < 0) {
					ok = false;
					break;
				}
			}
			if (!ok) {
				/* Keep the sensor active: a transient I2C error must not
				 * silently stop the stream, and the next interval retries. */
				if (++s->errors == 1 || (s->errors % 100) == 0)
					LOGW("read failed for %s (%d times)",
					     g_desc[i].name, s->errors);
				s->next_ns = now + s->delay_ns;
				continue;
			}
			s->errors = 0;

			map_axis(raw, s->scale, values);

			if (s->type == SENSOR_TYPE_ACCELEROMETER) {
				static const char *last_pose;
				const char *pose = pose_name(values);

				if (pose != last_pose) {
					LOGI("accel %+0.2f %+0.2f %+0.2f -> %s",
					     values[0], values[1], values[2], pose);
					last_pose = pose;
				}
			}

			memset(&data[n], 0, sizeof(data[n]));
			data[n].version = sizeof(sensors_event_t);
			data[n].sensor = s->handle;
			data[n].type = s->type;
			data[n].timestamp = now;
			data[n].data[0] = values[0];
			data[n].data[1] = values[1];
			data[n].data[2] = values[2];
			n++;

			s->next_ns += s->delay_ns;
			/* Do not try to catch up after a long stall. */
			if (s->next_ns <= now)
				s->next_ns = now + s->delay_ns;
		}
		pthread_mutex_unlock(&ctx->lock);

		if (n > 0)
			return n;
		/* Woken by activate(), or a spurious timeout: wait again. */
	}
}

static int sensors_device_open(const struct hw_module_t *module, const char *name,
			struct hw_device_t **device)
{
	struct sensors_ctx *ctx;
	int i, a;

	if (!name || strcmp(name, SENSORS_HARDWARE_POLL) != 0)
		return -EINVAL;

	scan_sensors();

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	pthread_mutex_init(&ctx->lock, NULL);
	for (i = 0; i < SENSOR_COUNT; i++) {
		for (a = 0; a < 3; a++)
			ctx->sensor[i].raw_fd[a] = -1;
		ctx->sensor[i].delay_ns = (int64_t)g_desc[i].min_delay_us * 1000LL;
	}

	if (pipe(ctx->wake_fd) < 0) {
		int err = -errno;

		pthread_mutex_destroy(&ctx->lock);
		free(ctx);
		return err;
	}
	fcntl(ctx->wake_fd[0], F_SETFL, O_NONBLOCK);
	fcntl(ctx->wake_fd[1], F_SETFL, O_NONBLOCK);

	for (i = 0; i < SENSOR_COUNT; i++) {
		struct sensor_desc *d = &g_desc[i];
		struct poll_sensor *s = &ctx->sensor[i];
		static const char *axis[3] = { "x", "y", "z" };
		char path[PATH_MAX];
		bool ok = true;

		if (!d->present)
			continue;

		s->present = true;
		s->scale = d->scale;
		s->type = d->type;
		s->handle = d->handle;

		for (a = 0; a < 3; a++) {
			snprintf(path, sizeof(path), "%s/%s_%s_raw",
				 d->dir, d->prefix, axis[a]);
			s->raw_fd[a] = open(path, O_RDONLY);
			if (s->raw_fd[a] < 0) {
				LOGE("cannot open %s: %s", path, strerror(errno));
				ok = false;
			}
		}
		snprintf(s->en_path, sizeof(s->en_path), "%s/%s_en",
			 d->dir, d->prefix);
		if (!file_exists(s->en_path)) {
			LOGE("cannot find %s", s->en_path);
			ok = false;
		}
		if (!ok) {
			for (a = 0; a < 3; a++) {
				if (s->raw_fd[a] >= 0)
					close(s->raw_fd[a]);
				s->raw_fd[a] = -1;
			}
			s->present = false;
		}
	}

	ctx->device.common.tag = HARDWARE_DEVICE_TAG;
	ctx->device.common.version = 0;
	ctx->device.common.module = (struct hw_module_t *)module;
	ctx->device.common.close = poll_close;
	ctx->device.activate = poll_activate;
	ctx->device.setDelay = poll_set_delay;
	ctx->device.poll = poll_events;

	*device = &ctx->device.common;
	return 0;
}

static struct hw_module_methods_t sensors_module_methods = {
	.open = sensors_device_open,
};

const struct sensors_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.version_major = 1,
		.version_minor = 0,
		.id = SENSORS_HARDWARE_MODULE_ID,
		.name = "Nintendo 3DS sensors",
		.author = "Cyano3DS",
		.methods = &sensors_module_methods,
	},
	.get_sensors_list = sensors_get_sensors_list,
};
