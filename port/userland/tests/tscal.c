// SPDX-License-Identifier: GPL-2.0
/*
 * tscal.c - one-shot touchscreen calibration for the 3DS port
 *
 * WHY
 * ---
 * The touchscreen driver (Octoblimp's android3ds-touch, vendored verbatim)
 * turns the digitizer's raw 12-bit ADC reading into a 320x240 pixel with a
 * two-point linear calibration per axis.  Its built-in points assume the
 * glass spans raw 256..3840 edge to edge.  That is a guess, not a
 * measurement -- this port has no way to read the console's factory HWCAL --
 * and every panel is a little different.  When the guess is off the error is
 * a gain plus an offset, so taps are right in one place and drift the
 * further you go from it; if the unit's span is not centred, one side of the
 * screen is simply worse than the other.
 *
 * This tool measures the unit instead.  It runs before Android:
 *
 *   1. it draws five targets on the bottom screen, one at a time (the four
 *      corners, inset 16 px, and the centre);
 *   2. the user taps each one; the raw ADC value is read from the driver's
 *      sysfs attributes (/sys/class/input/inputN/raw_{x,y,down}), which are
 *      the values *before* any calibration, so the result does not depend on
 *      what the driver is currently applying;
 *   3. a least-squares line per axis gives the raw value at screen x 16 / 303
 *      and y 16 / 223, which the driver takes as its calibration points
 *      (/sys/module/touch/parameters/cal_{x,y}{0,1}_{raw,px}).
 *
 * The result is applied at once and written to CYANO3DS/touch-cal as
 * "x0 x1 y0 y1" (raw at screen x16, x303, y16, y223); /init feeds that file to
 * the driver on every boot.  The per-target error after the fit is printed so
 * a panel that is not linear (a left edge that bends, say) shows up in the
 * log instead of being averaged away.
 *
 * A mis-tap can be taken back: B undoes the previous tap and shows that
 * target again (after the last tap there are a few seconds to do the same).
 *
 * Usage: tscal [fbfile] [calfile]
 *   defaults: /dev/fb1  (do not save)
 * No tap within TSCAL_TIMEOUT seconds => leave the calibration alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <signal.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "font8x8.h"

#define NUM_TARGETS 5
#define TSCAL_TIMEOUT 30	/* seconds per target */
#define POLL_US 4000		/* the driver samples every 4 ms */
#define SETTLE_MS 60		/* a resistive panel ramps up after contact */
#define MIN_SAMPLES 6
#define MAX_SAMPLES 4096
#define CONFIRM_SECS 3		/* undo window after the last tap */
/* The kernel DTS maps B to KEY_BACK (158); BTN_B is the unpatched code. */
#define KEY_B_MAPPED 158
/* Screen positions the driver is told the two raw values belong to. */
#define CAL_X0 16
#define CAL_X1 303
#define CAL_Y0 16
#define CAL_Y1 223
/* A fit worse than this on either axis is a mis-tap, not a calibration. */
#define MAX_RMS 9.0
#define PARAM_DIR "/sys/module/touch/parameters"

struct fb {
	int fd;
	int bpp, linelen;
	unsigned char *mem;
	int cur;	/* buffer the panel is scanning now */
	int draw;	/* buffer the next paint goes into (the idle one) */
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
};

static int fb_open(struct fb *f, const char *path)
{
	f->fd = open(path, O_RDWR);
	if (f->fd < 0) {
		fprintf(stderr, "tscal: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) ||
	    ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix)) {
		fprintf(stderr, "tscal: ioctl %s: %s\n", path, strerror(errno));
		return -1;
	}
	f->bpp = f->var.bits_per_pixel;
	f->linelen = f->fix.line_length;
	f->mem = mmap(NULL, f->fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      f->fd, 0);
	if (f->mem == MAP_FAILED) {
		fprintf(stderr, "tscal: mmap %s: %s\n", path, strerror(errno));
		return -1;
	}
	/*
	 * The panel is double buffered and ctr_lcd_fb only updates it on a flip,
	 * so a raw write to the mmap'd framebuffer is never shown.  Paint into
	 * the buffer that is *not* being scanned and flip to it (the same
	 * sequence fbsay and fbtest use).
	 */
	f->cur = f->var.yoffset / f->var.yres;
	if (f->cur > 1)
		f->cur = 0;
	f->draw = 1 - f->cur;
	return 0;
}

static void fb_flip(struct fb *f)
{
	f->var.activate = FB_ACTIVATE_VBL;
	f->var.yoffset = f->draw * f->var.yres;
	if (ioctl(f->fd, FBIOPUT_VSCREENINFO, &f->var) == -1)
		fprintf(stderr, "tscal: flip to yoffset %u: %s\n",
			f->var.yoffset, strerror(errno));
	f->cur = f->draw;
	f->draw = 1 - f->draw;
}

static void putpx(struct fb *f, int x, int y, unsigned int v)
{
	unsigned char *p;

	if (x < 0 || y < 0 || x >= (int)f->var.xres || y >= (int)f->var.yres)
		return;
	p = f->mem + (size_t)f->draw * f->linelen * f->var.yres +
	    (size_t)y * f->linelen + (size_t)x * (f->bpp / 8);
	switch (f->bpp) {
	case 16: *(unsigned short *)p = (unsigned short)v; break;
	case 24:
		p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff;
		break;
	case 32: *(unsigned int *)p = v; break;
	}
}

static unsigned int pack_rgb(struct fb *f, unsigned int r,
			     unsigned int g, unsigned int b)
{
	unsigned int v = 0;

	if (f->var.red.length)
		v |= ((r >> (8 - f->var.red.length)) &
		      ((1u << f->var.red.length) - 1)) << f->var.red.offset;
	if (f->var.green.length)
		v |= ((g >> (8 - f->var.green.length)) &
		      ((1u << f->var.green.length) - 1)) << f->var.green.offset;
	if (f->var.blue.length)
		v |= ((b >> (8 - f->var.blue.length)) &
		      ((1u << f->var.blue.length) - 1)) << f->var.blue.offset;
	return v;
}

static void clear(struct fb *f, unsigned int color)
{
	int x, y;

	for (y = 0; y < (int)f->var.yres; y++)
		for (x = 0; x < (int)f->var.xres; x++)
			putpx(f, x, y, color);
}

/* A crosshair with a hollow ring, so it is obvious where to tap. */
static void target(struct fb *f, int cx, int cy, unsigned int color)
{
	int i, r = 16;

	for (i = -r; i <= r; i++) {
		putpx(f, cx + i, cy, color);
		putpx(f, cx, cy + i, color);
		putpx(f, cx - r, cy + i, color);
		putpx(f, cx + r, cy + i, color);
		putpx(f, cx + i, cy - r, color);
		putpx(f, cx + i, cy + r, color);
	}
	/* solid centre dot */
	for (i = -2; i <= 2; i++)
		putpx(f, cx + i, cy, color), putpx(f, cx, cy + i, color);
}

static void text(struct fb *f, int x, int y, const char *s, unsigned int color)
{
	for (; *s; s++, x += FONT_W) {
		const unsigned char *g = font8x8 + (unsigned)(unsigned char)*s * FONT_H;
		int row, col;

		for (row = 0; row < FONT_H; row++)
			for (col = 0; col < FONT_W; col++)
				if (g[row] & (0x80 >> col))
					putpx(f, x + col, y + row, color);
	}
}

/*
 * The boot log is rendered onto the *same* framebuffer this tool draws its
 * targets on (fbsay), and it repaints a page every few seconds -- a repaint
 * that lands between our clear() and the tap hides the target.  Kill it here
 * by name rather than trusting the caller to have done it.
 */
static int kill_log_renderers(void)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	int pid, killed = 0;
	char path[64], buf[256];
	int fd;

	if (!d)
		return 0;
	while ((de = readdir(d)) != NULL) {
		pid = atoi(de->d_name);
		if (pid <= 1)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		{
			ssize_t n = read(fd, buf, sizeof(buf) - 1);

			close(fd);
			if (n <= 0)
				continue;
			buf[n] = 0;
		}
		if (strstr(buf, "fbsay") || strstr(buf, "tsmark")) {
			if (kill(pid, SIGKILL) == 0) {
				printf("tscal: killed log renderer pid %d\n",
				       pid);
				killed++;
			}
		}
	}
	closedir(d);
	return killed;
}

/*
 * The whole calibration screen: black, one big target, the instruction and a
 * counter.  Painted once per target *and* once a second while waiting for the
 * tap, so that even if something else draws on this framebuffer the target is
 * visible again within a second instead of staying hidden for the timeout.
 */
static void paint_target(struct fb *f, const int (*tgt)[2], int i,
			 unsigned int fg, unsigned int bg)
{
	char msg[48];

	clear(f, bg);
	target(f, tgt[i][0], tgt[i][1], pack_rgb(f, 0xff, 0xff, 0xff));
	snprintf(msg, sizeof(msg), "TOUCH SCREEN SETUP %d/%d", i + 1, NUM_TARGETS);
	text(f, 56, 100 + (tgt[i][1] < 120 ? 40 : -40), msg, fg);
	text(f, 56, 112 + (tgt[i][1] < 120 ? 40 : -40),
	     "Tap the centre of the cross", fg);
	text(f, 56, 124 + (tgt[i][1] < 120 ? 40 : -40),
	     i > 0 ? "B = undo the last tap" : "", fg);
	fb_flip(f);
}

/* ---- the B button (undo) ---------------------------------------------- */

#define MAXKEYS 8
static int keyfd[MAXKEYS], nkeys;

static int has_bit(const unsigned long *bits, int bit)
{
	return !!(bits[bit / (8 * sizeof(unsigned long))] &
		  (1UL << (bit % (8 * sizeof(unsigned long)))));
}

/* Open every event node that can report B (the touchscreen cannot). */
static void keys_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	char path[320];
	unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			      (8 * sizeof(unsigned long))];

	if (!d)
		return;
	while ((de = readdir(d)) != NULL && nkeys < MAXKEYS) {
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(keybits, 0, sizeof(keybits));
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		if (has_bit(keybits, KEY_B_MAPPED) || has_bit(keybits, BTN_B))
			keyfd[nkeys++] = fd;
		else
			close(fd);
	}
	closedir(d);
	if (!nkeys)
		printf("tscal: no button device found, B (undo) is not available\n");
}

/* Drain the button queues; 1 if B went down since the last call. */
static int b_pressed(void)
{
	struct input_event ev;
	int i, hit = 0;

	for (i = 0; i < nkeys; i++)
		while (read(keyfd[i], &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && ev.value == 1 &&
			    (ev.code == KEY_B_MAPPED || ev.code == BTN_B))
				hit = 1;
	return hit;
}

/* ---- raw digitizer values, straight from the driver's sysfs ---------- */

struct raw {
	int fx, fy, fd;
	int ev;		/* the evdev node; see raw_open() */
};

static int slurp(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = 0;
	return 0;
}

static int raw_open(struct raw *r)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *de;
	char path[320], name[96];

	r->fx = r->fy = r->fd = r->ev = -1;
	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "input", 5))
			continue;
		snprintf(path, sizeof(path), "/sys/class/input/%s/name", de->d_name);
		if (slurp(path, name, sizeof(name)) < 0 ||
		    !strstr(name, "Touchscreen"))
			continue;
		snprintf(path, sizeof(path), "/sys/class/input/%s/raw_x", de->d_name);
		r->fx = open(path, O_RDONLY);
		snprintf(path, sizeof(path), "/sys/class/input/%s/raw_y", de->d_name);
		r->fy = open(path, O_RDONLY);
		snprintf(path, sizeof(path), "/sys/class/input/%s/raw_down", de->d_name);
		r->fd = open(path, O_RDONLY);
		if (r->fx >= 0 && r->fy >= 0 && r->fd >= 0) {
			/*
			 * The driver polls the digitizer only while something
			 * has its evdev node open, so raw_x/raw_y/raw_down stay
			 * frozen until we open it too.  Nothing is read from it.
			 */
			DIR *c;
			struct dirent *ce;

			snprintf(path, sizeof(path), "/sys/class/input/%s", de->d_name);
			c = opendir(path);
			while (c && (ce = readdir(c)) != NULL) {
				if (strncmp(ce->d_name, "event", 5))
					continue;
				snprintf(path, sizeof(path), "/dev/input/%s", ce->d_name);
				r->ev = open(path, O_RDONLY | O_NONBLOCK);
				if (r->ev >= 0)
					break;
			}
			if (c)
				closedir(c);
			if (r->ev < 0)
				printf("tscal: cannot open the evdev node, raw values may not update\n");
			printf("tscal: raw values from /sys/class/input/%s (%s)\n",
			       de->d_name, strtok(name, "\n"));
			closedir(d);
			return 0;
		}
		if (r->fx >= 0) close(r->fx);
		if (r->fy >= 0) close(r->fy);
		if (r->fd >= 0) close(r->fd);
		r->fx = r->fy = r->fd = -1;
	}
	closedir(d);
	return -1;
}

static int raw_read(int fd)
{
	char b[16];
	ssize_t n = pread(fd, b, sizeof(b) - 1, 0);

	if (n <= 0)
		return -1;
	b[n] = 0;
	return atoi(b);
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/*
 * Wait for one complete tap and return the median raw position over the
 * steady part of the press.  The first SETTLE_MS are dropped (the contact
 * resistance is still ramping) and so is the last quarter (the finger is
 * already lifting).  Returns 0 = tap, 1 = too short / unreadable (ask again),
 * 2 = B pressed (undo), -1 = nobody touched anything within TSCAL_TIMEOUT.
 */
static int wait_tap(struct raw *r, struct fb *f, const int (*tgt)[2], int i,
		    unsigned int fg, unsigned int bg, int *rx, int *ry)
{
	static int xs[MAX_SAMPLES], ys[MAX_SAMPLES];
	time_t deadline = time(NULL) + TSCAL_TIMEOUT, last_paint = time(NULL);
	int n = 0, up_polls = 0, down = 0, ms = 0, lo, hi;

	for (;;) {
		int d = raw_read(r->fd);

		if (d < 0) {
			usleep(POLL_US);
			continue;
		}
		if (!down) {
			if (b_pressed())
				return 2;
			if (d) {
				down = 1;
				n = ms = 0;
				up_polls = 0;
			} else {
				if (time(NULL) >= deadline)
					return -1;
				if (time(NULL) != last_paint) {
					last_paint = time(NULL);
					paint_target(f, tgt, i, fg, bg);
				}
			}
		} else if (d) {
			up_polls = 0;
			ms += POLL_US / 1000;
			if (ms > SETTLE_MS && n < MAX_SAMPLES) {
				int x = raw_read(r->fx), y = raw_read(r->fy);

				/* 0 and 4095 are rails, not positions */
				if (x > 0 && x < 4095 && y > 0 && y < 4095) {
					xs[n] = x;
					ys[n] = y;
					n++;
				}
			}
		} else if (++up_polls >= 3) {
			break;
		}
		usleep(POLL_US);
	}
	if (n < MIN_SAMPLES)
		return 1;
	lo = n / 4;
	hi = n - n / 4;
	qsort(xs + lo, hi - lo, sizeof(int), cmp_int);
	qsort(ys + lo, hi - lo, sizeof(int), cmp_int);
	*rx = xs[lo + (hi - lo) / 2];
	*ry = ys[lo + (hi - lo) / 2];
	return 0;
}

/*
 * Least-squares line  screen = k * raw + c  over n points; returns the raw
 * values that sit at screen positions p0 and p1.  -1 when the taps are not a
 * usable calibration (no spread, or a gain that is not positive).
 */
static int fit_axis(const int *raw, const int *scr, int n, int p0, int p1,
		    int *r0, int *r1, double *rms)
{
	double sa = 0, st = 0, saa = 0, sat = 0, e2 = 0, am, tm, k, c;
	int i;

	for (i = 0; i < n; i++) {
		sa += raw[i];
		st += scr[i];
	}
	am = sa / n;
	tm = st / n;
	for (i = 0; i < n; i++) {
		saa += (raw[i] - am) * (raw[i] - am);
		sat += (raw[i] - am) * (scr[i] - tm);
	}
	if (saa < 16.0 || sat <= 0.0)
		return -1;
	k = sat / saa;
	c = tm - k * am;
	for (i = 0; i < n; i++) {
		double e = k * raw[i] + c - scr[i];

		e2 += e * e;
	}
	*rms = sqrt(e2 / n);
	*r0 = (int)lround((p0 - c) / k);
	*r1 = (int)lround((p1 - c) / k);
	return 0;
}

/* Fit one axis, dropping the single worst tap once if it spoils the fit. */
static int fit_robust(int *raw, int *scr, int *n, int p0, int p1,
		      int *r0, int *r1, double *rms, const char *axis)
{
	int i, worst = -1;
	double wd = 0, k;

	if (fit_axis(raw, scr, *n, p0, p1, r0, r1, rms))
		return -1;
	if (*rms <= MAX_RMS || *n < 5)
		return 0;
	k = (double)(p1 - p0) / (double)(*r1 - *r0);
	for (i = 0; i < *n; i++) {
		double e = fabs(k * (raw[i] - *r0) + p0 - scr[i]);

		if (e > wd) {
			wd = e;
			worst = i;
		}
	}
	printf("tscal: %s: dropping tap %d (%.0f px off the line)\n",
	       axis, worst + 1, wd);
	for (i = worst; i < *n - 1; i++) {
		raw[i] = raw[i + 1];
		scr[i] = scr[i + 1];
	}
	(*n)--;
	return fit_axis(raw, scr, *n, p0, p1, r0, r1, rms);
}

static int set_param(const char *name, int v)
{
	char path[96];
	FILE *f;

	snprintf(path, sizeof(path), PARAM_DIR "/%s", name);
	f = fopen(path, "w");
	if (!f) {
		printf("tscal: cannot write %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(f, "%d\n", v);
	return fclose(f) ? -1 : 0;
}

int main(int argc, char **argv)
{
	const char *fbpath = argc > 1 && argv[1][0] ? argv[1] : "/dev/fb1";
	const char *calpath = argc > 2 && argv[2][0] ? argv[2] : NULL;
	static const int tgt[NUM_TARGETS][2] = {
		{ CAL_X0, CAL_Y0 }, { CAL_X1, CAL_Y0 },
		{ CAL_X0, CAL_Y1 }, { CAL_X1, CAL_Y1 }, { 160, 120 }
	};
	struct fb f;
	struct raw r;
	int i, n = 0, ny, ok;
	int rx[NUM_TARGETS], ry[NUM_TARGETS];
	int sx[NUM_TARGETS], sy[NUM_TARGETS];
	int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	double xrms = 0, yrms = 0;
	unsigned int fg, bg;

	memset(&f, 0, sizeof(f));
	if (fb_open(&f, fbpath) < 0)
		return 1;
	if (raw_open(&r) < 0) {
		fprintf(stderr, "tscal: no touchscreen raw_x/raw_y/raw_down in "
			"/sys/class/input\n");
		return 1;
	}
	keys_open();
	b_pressed();	/* a B pressed before the first target is not an undo */
	fg = pack_rgb(&f, 0x00, 0xff, 0x40);
	bg = pack_rgb(&f, 0x00, 0x00, 0x00);
	if (kill_log_renderers() > 0)
		fflush(stdout);
	printf("tscal: fb=%s %dx%d %dbpp\n", fbpath, f.var.xres, f.var.yres,
	       f.bpp);
	fflush(stdout);

	for (;;) {
		for (i = n; i < NUM_TARGETS; i++) {
			int got, x = 0, y = 0;

			for (;;) {
				paint_target(&f, tgt, i, fg, bg);
				printf("tscal: target %d/%d screen=(%d,%d) -- tap it\n",
				       i + 1, NUM_TARGETS, tgt[i][0], tgt[i][1]);
				fflush(stdout);
				got = wait_tap(&r, &f, tgt, i, fg, bg, &x, &y);
				if (got != 1)
					break;
				printf("tscal: tap too short, again\n");
				fflush(stdout);
			}
			if (got < 0) {
				printf("tscal: target %d timed out\n", i + 1);
				fflush(stdout);
				break;
			}
			if (got == 2) {
				/* B: forget the previous tap and ask for it again */
				if (n > 0) {
					n--;
					printf("tscal: B -- undid tap %d\n", n + 1);
					i -= 2;
				} else {
					printf("tscal: B -- nothing to undo\n");
					i--;
				}
				fflush(stdout);
				continue;
			}
			rx[n] = x;
			ry[n] = y;
			sx[n] = tgt[i][0];
			sy[n] = tgt[i][1];
			printf("tscal: target %d screen=(%d,%d) raw=(%d,%d)\n", i + 1,
			       sx[n], sy[n], x, y);
			fflush(stdout);
			n++;
			/* the next target must be a fresh tap */
			usleep(300000);
			while (raw_read(r.fd) > 0)
				usleep(POLL_US);
		}
		if (n < NUM_TARGETS)
			break;
		/* last tap in: a few seconds to take it back before it is used */
		clear(&f, bg);
		text(&f, 56, 100, "All five taps are in.", fg);
		text(&f, 56, 112, "Saving in a moment...", fg);
		text(&f, 56, 124, "B = undo the last tap", fg);
		fb_flip(&f);
		for (i = 0; i < CONFIRM_SECS * 50 && !b_pressed(); i++)
			usleep(20000);
		if (i == CONFIRM_SECS * 50)
			break;
		n--;
		printf("tscal: B -- undid tap %d\n", n + 1);
		fflush(stdout);
	}

	clear(&f, bg);
	fb_flip(&f);

	if (n < NUM_TARGETS) {
		printf("tscal: only %d of %d taps, leaving the calibration unchanged\n",
		       n, NUM_TARGETS);
		fflush(stdout);
		return 1;
	}

	ny = n;
	ok = !fit_robust(rx, sx, &n, CAL_X0, CAL_X1, &x0, &x1, &xrms, "x") &&
	     !fit_robust(ry, sy, &ny, CAL_Y0, CAL_Y1, &y0, &y1, &yrms, "y");
	if (!ok || xrms > MAX_RMS || yrms > MAX_RMS ||
	    x1 - x0 < 1500 || y1 - y0 < 1500 ||
	    x0 < 1 || y0 < 1 || x1 > 4094 || y1 > 4094) {
		printf("tscal: fit not usable (x %d..%d rms %.1f px, y %d..%d rms "
		       "%.1f px), leaving the calibration unchanged\n",
		       x0, x1, xrms, y0, y1, yrms);
		fflush(stdout);
		return 1;
	}
	/* How well a straight line explains this panel, per tap. */
	for (i = 0; i < n; i++) {
		double e = (double)(CAL_X1 - CAL_X0) * (rx[i] - x0) / (x1 - x0) +
			   CAL_X0 - sx[i];

		printf("tscal: x tap at screen %3d: raw %4d, line says %.1f (error %+.1f px)\n",
		       sx[i], rx[i], e + sx[i], e);
	}
	for (i = 0; i < ny; i++) {
		double e = (double)(CAL_Y1 - CAL_Y0) * (ry[i] - y0) / (y1 - y0) +
			   CAL_Y0 - sy[i];

		printf("tscal: y tap at screen %3d: raw %4d, line says %.1f (error %+.1f px)\n",
		       sy[i], ry[i], e + sy[i], e);
	}
	if (set_param("cal_x0_raw", x0) || set_param("cal_x0_px", CAL_X0) ||
	    set_param("cal_x1_raw", x1) || set_param("cal_x1_px", CAL_X1) ||
	    set_param("cal_y0_raw", y0) || set_param("cal_y0_px", CAL_Y0) ||
	    set_param("cal_y1_raw", y1) || set_param("cal_y1_px", CAL_Y1)) {
		printf("tscal: could not apply the calibration to the driver\n");
		fflush(stdout);
		return 1;
	}
	printf("tscal: applied cal x0=%d x1=%d y0=%d y1=%d (raw at screen x%d x%d y%d y%d; "
	       "rms x %.1f px, y %.1f px)\n", x0, x1, y0, y1, CAL_X0, CAL_X1,
	       CAL_Y0, CAL_Y1, xrms, yrms);
	if (calpath) {
		FILE *c = fopen(calpath, "w");

		if (c) {
			fprintf(c, "%d %d %d %d\n", x0, x1, y0, y1);
			fflush(c);
			fsync(fileno(c));
			fclose(c);
			printf("tscal: calibration saved to %s\n", calpath);
		} else {
			printf("tscal: cannot save %s: %s\n", calpath,
			       strerror(errno));
		}
	}
	fflush(stdout);
	return 0;
}
