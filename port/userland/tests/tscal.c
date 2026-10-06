// SPDX-License-Identifier: GPL-2.0
/*
 * tscal.c - one-shot touchscreen calibration for the 3DS port
 *
 * WHY
 * ---
 * The 3DS bottom panel is scanned portrait and the driver transposes a
 * landscape 320x240 frame into it; the touch digitizer has its own native
 * axes.  Which transposition/inversion is right depends on how the digitizer
 * is bonded and cannot be read out of a register, so it has to be measured.
 * Guessing it wrong makes taps land up to 90 degrees away from where the UI
 * thinks they are -- a tap meant for an icon lands on the status bar, opens
 * the touch-modal notification shade, and from then on every touch is
 * swallowed.
 *
 * This tool runs *before* Android and measures the mapping directly:
 *
 *   1. it draws five targets (four corners + centre) on the bottom screen;
 *   2. the user taps each target once, in order;
 *   3. it scores all eight candidate mappings against the taps, picks the
 *      best one, and writes it to /proc/ctr_touch_map and
 *      /mnt/sd/CYANO3DS/touch-map.txt.
 *
 * The mapping bit mask is the same the kernel uses:
 *
 *   bit0 = swap X/Y, bit1 = invert X, bit2 = invert Y.
 *
 * Orientation is only half of the problem.  The driver also has to know which
 * raw 12-bit ADC values correspond to the panel's edges, because the
 * digitizer's full-scale range is *not* the glass: scaling 0..4095 onto
 * 320x240 under-reads the span, so a tap near the middle of the screen is
 * about right and a tap near an edge lands further out the further out you
 * reach -- a gain-and-offset error that no swap or invert can fix.  So the
 * same five taps are fitted for both axes and the raw values that map to
 * screen 0 and screen 319 are written to /proc/ctr_touch_cal and
 * /mnt/sd/CYANO3DS/touch-cal.txt.
 *
 * Usage: tscal [fbfile] [inputfile] [mapfile] [calfile]
 *   defaults: /dev/fb1  (scan /dev/input)  /mnt/sd/CYANO3DS/touch-map.txt
 *             /mnt/sd/CYANO3DS/touch-cal.txt
 * Input is not tapped within TSCAL_TIMEOUT seconds => leave the mapping be.
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
#include <sys/select.h>
#include <sys/mman.h>
#include <dirent.h>
#include <signal.h>
#include <linux/fb.h>
#include <linux/input.h>

#define MAX_12BIT 4095
#define NUM_TARGETS 5
#define TSCAL_TIMEOUT 30	/* seconds per target */
/*
 * A tap is only accepted if it lands within this many pixels of the target
 * that was drawn.  Without it, one mistap -- and a mistap is the *normal*
 * outcome when the targets are not clearly visible, which is exactly when
 * calibration is needed -- goes into the fit as a point at the wrong place
 * and drags every other point with it.  It did: a run where the bottom-
 * right target was tapped at the screen centre (raw 2003,2048, i.e. the
 * middle of the ADC domain) produced a least-squares fit with an RMS of
 * 55 px, which is worse than having no calibration at all.
 */
#define TSCAL_MAXERR 40

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
	 * sequence fbsay and fbtest use).  Without this the targets are simply
	 * invisible, which is exactly how a calibration run is lost.
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

/*
 * The boot log is rendered onto the *same* framebuffer this tool draws its
 * targets on (fbsay, /dev/fb1), and it repaints a page every few seconds --
 * which is exactly why a calibration run used to show the targets being
 * covered by scrolling log text: /init kills fbsay before Android starts, but
 * not on every path that reaches this tool, and a repaint that lands between
 * our clear() and the tap is enough to hide the target.  Kill it here as well,
 * by name, rather than trusting the caller to have done it, and say how many
 * processes were stopped so the log records it.
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
 * The whole calibration screen: black, one big target, and a progress bar of
 * filled squares top-left.  Painted once per target *and* once a second while
 * waiting for the tap, so that even if something else draws on this
 * framebuffer the target is visible again within a second instead of staying
 * hidden for the whole 30 s timeout.
 */
static void paint_target(struct fb *f, const int (*tgt)[2], int i,
			 unsigned int fg, unsigned int bg)
{
	int k, j;

	clear(f, bg);
	target(f, tgt[i][0], tgt[i][1], pack_rgb(f, 0xff, 0xff, 0xff));
	for (k = 0; k <= i; k++)
		for (j = 0; j < 10; j++)
			putpx(f, 4 + k * 14, 4 + j, fg);
	fb_flip(f);
}

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
	return !!(absbits[ABS_X / (8 * sizeof(unsigned long))] &
		  (1UL << (ABS_X % (8 * sizeof(unsigned long))))) &&
	       !!(absbits[ABS_Y / (8 * sizeof(unsigned long))] &
		  (1UL << (ABS_Y % (8 * sizeof(unsigned long))))) &&
	       !!(keybits[BTN_TOUCH / (8 * sizeof(unsigned long))] &
		  (1UL << (BTN_TOUCH % (8 * sizeof(unsigned long)))));
}

static int open_touch(const char *want)
{
	int i, fd = -1;

	if (want && (fd = open(want, O_RDONLY | O_NONBLOCK)) >= 0)
		return fd;
	for (i = 0; i < 16; i++) {
		char p[64];

		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (is_touchscreen(fd))
			return fd;
		close(fd);
	}
	return -1;
}

/*
 * The affine calibration the kernel is currently applying, so a tap can be
 * inverted back to the raw ADC value the orientation sweep needs to see.
 * The defaults are what the driver falls back to when nobody has measured
 * anything; they are deliberately *not* 0..4095 (see the header).
 */
static void read_proc_cal(int *x0, int *x1, int *y0, int *y1)
{
	FILE *f = fopen("/proc/ctr_touch_cal", "r");

	*x0 = 0;
	*x1 = MAX_12BIT;
	*y0 = 0;
	*y1 = MAX_12BIT;
	if (!f)
		return;
	if (fscanf(f, "%d %d %d %d", x0, x1, y0, y1) != 4 ||
	    *x1 - *x0 < 16 || *y1 - *y0 < 16 ||
	    *x0 < 0 || *y0 < 0 || *x1 > MAX_12BIT || *y1 > MAX_12BIT) {
		*x0 = 0;
		*x1 = MAX_12BIT;
		*y0 = 0;
		*y1 = MAX_12BIT;
	}
	fclose(f);
	printf("tscal: active kernel calibration x0=%d x1=%d y0=%d y1=%d\n",
	       *x0, *x1, *y0, *y1);
	fflush(stdout);
}

static void map_raw(int m, int rx, int ry, int *x, int *y)
{
	unsigned int mx = (unsigned int)rx, my = (unsigned int)ry;

	if (m & 1) {
		unsigned int t = mx;

		mx = my;
		my = t;
	}
	*x = (int)(mx * 320 / MAX_12BIT);
	*y = (int)(my * 240 / MAX_12BIT);
	if (m & 2)
		*x = 319 - *x;
	if (m & 4)
		*y = 239 - *y;
}

static int read_proc_map(void)
{
	FILE *f = fopen("/proc/ctr_touch_map", "r");
	int m = 0;

	if (f) {
		if (fscanf(f, "%d", &m) != 1)
			m = 0;
		fclose(f);
	}
	return m & 7;
}

/*
 * Invert the mapping the kernel is *currently* applying.
 *
 * The kernel already turns the raw 12-bit ADC sample into the landscape
 * screen coordinate using ctr_touch_map, and that is what the input device
 * reports as ABS_X/ABS_Y.  Scoring the eight candidate mappings on those
 * values would map them a second time ("raw=(0x01a,0x013) map3=(318,1)" for a
 * tap at (20,20)) and pick a random winner.  Undo the active map first so the
 * scorer sees the raw ADC values it expects and can compare the eight
 * candidates honestly.
 */
static void unmap_active(int d, int ex, int ey,
			int cx0, int cx1, int cy0, int cy1,
			int *rx, int *ry)
{
	int mx = ex, my = ey;

	/* undo invert-x / invert-y (applied last by the kernel) */
	if (d & 2)
		mx = 319 - mx;
	if (d & 4)
		my = 239 - my;

	/* undo the affine calibration: screen 0..320 came from x0..x1 */
	mx = cx0 + mx * (cx1 - cx0) / 320;
	my = cy0 + my * (cy1 - cy0) / 240;

	/* undo the swap (applied first by the kernel) */
	if (d & 1) {
		int t = mx;

		mx = my;
		my = t;
	}
	*rx = mx;
	*ry = my;
}

/*
 * Least-squares fit of screen = k * raw + c over the tapped targets, and from
 * it the raw values at screen 0 and screen 319, which is what the driver's
 *
 *     screen = (raw - edge0) * 320 / (edge1 - edge0)
 *
 * form expects.  Returns 0 on success and -1 when the fit is not trustworthy
 * (not enough spread, or a gain that is not positive -- both mean the taps
 * were not a usable calibration, and applying them would move every touch).
 */
static int fit_axis(const int *raw, const int *target, int n,
		    int *edge0, int *edge1, double *rms)
{
	double sa = 0, st = 0, saa = 0, sat = 0, e2 = 0, amean, tmean, k, c;
	int i;

	for (i = 0; i < n; i++) {
		sa += raw[i];
		st += target[i];
	}
	amean = sa / n;
	tmean = st / n;
	for (i = 0; i < n; i++) {
		double da = raw[i] - amean, dt = target[i] - tmean;

		saa += da * da;
		sat += da * dt;
	}
	if (saa < 16.0 || sat <= 0.0)
		return -1;
	k = sat / saa;
	c = tmean - k * amean;
	if (k <= 0.0)
		return -1;
	for (i = 0; i < n; i++) {
		double d = (k * raw[i] + c) - target[i];

		e2 += d * d;
	}
	*rms = sqrt(e2 / n);
	*edge0 = (int)(-c / k);
	*edge1 = (int)(-c / k + 319.0 / k);
	return 0;
}

static long long score(int m, const int *rx, const int *ry,
		       const int *tx, const int *ty, int n)
{
	long long e = 0;
	int i;

	for (i = 0; i < n; i++) {
		int x, y, dx, dy;

		map_raw(m, rx[i], ry[i], &x, &y);
		dx = x - tx[i];
		dy = y - ty[i];
		e += (long long)dx * dx + (long long)dy * dy;
	}
	return e;
}

int main(int argc, char **argv)
{
	const char *fbpath = argc > 1 ? argv[1] : "/dev/fb1";
	const char *inpath = argc > 2 ? argv[2] : NULL;
	const char *mappath = argc > 3 ? argv[3] : NULL;
	const char *calpath = argc > 4 ? argv[4] : NULL;
	static const int tgt[NUM_TARGETS][2] = {
		{ 14, 12 }, { 305, 12 }, { 14, 227 }, { 305, 227 }, { 160, 120 }
	};
	struct fb f;
	struct input_event ev;
	int tfd, i, n = 0;
	int rx[NUM_TARGETS], ry[NUM_TARGETS];
	int tx[NUM_TARGETS], ty[NUM_TARGETS];
	unsigned int fg, bg;
	long long best = -1;
	int bestm = -1;
	int active;
	int cx0, cx1, cy0, cy1; /* calibration the kernel is applying right now */
	int vx[NUM_TARGETS], vy[NUM_TARGETS];  /* post-swap raw: the calibrated axes */
	int rtx = 0, rty = 0;   /* recovered raw ADC values of the current tap */

	memset(&f, 0, sizeof(f));
	if (fb_open(&f, fbpath) < 0)
		return 1;
	tfd = open_touch(inpath);
	if (tfd < 0) {
		fprintf(stderr, "tscal: no touchscreen node found\n");
		return 1;
	}
	fg = pack_rgb(&f, 0x00, 0xff, 0x40);
	bg = pack_rgb(&f, 0x00, 0x00, 0x00);
	/* Our targets share the bottom screen with the boot-log renderer. */
	if (kill_log_renderers() > 0)
		fflush(stdout);

	active = read_proc_map();
	read_proc_cal(&cx0, &cx1, &cy0, &cy1);
	printf("tscal: fb=%s %dx%d %dbpp, touch fd=%d, active kernel map=%d\n",
	       fbpath, f.var.xres, f.var.yres, f.bpp, tfd, active);
	fflush(stdout);

	for (i = 0; i < NUM_TARGETS; i++) {
		int x = 0, y = 0, down = 0, got = 0;
		time_t deadline = time(NULL) + TSCAL_TIMEOUT;

		paint_target(&f, tgt, i, fg, bg);
		printf("tscal: target %d/%d screen=(%d,%d) -- tap it\n",
		       i + 1, NUM_TARGETS, tgt[i][0], tgt[i][1]);
		fflush(stdout);

		{
		time_t last_paint = time(NULL);

		while (!got && time(NULL) < deadline) {
			fd_set rfds;
			struct timeval tv = { 1, 0 };
			int r;

			FD_ZERO(&rfds);
			FD_SET(tfd, &rfds);
			r = select(tfd + 1, &rfds, NULL, NULL, &tv);
			if (time(NULL) != last_paint) {
				last_paint = time(NULL);
				paint_target(&f, tgt, i, fg, bg);
			}
			if (r <= 0)
				continue;
			while (read(tfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type == EV_ABS && ev.code == ABS_X)
					x = ev.value;
				else if (ev.type == EV_ABS && ev.code == ABS_Y)
					y = ev.value;
				else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
					down = ev.value;
				else if (ev.type == EV_SYN && down) {
					got = 1;
					break;
				}
			}
		}
		if (!got) {
			printf("tscal: target %d timed out\n", i + 1);
			fflush(stdout);
			break;
		}
		/*
		 * Did the finger actually hit what was drawn?  Only evdev
		 * coordinates are known here, and with a calibration that is
		 * still roughly right, a miss shows up immediately.  Reject and
		 * ask again rather than record a point that is nowhere near its
		 * target -- one such point is enough to make the fit useless.
		 */
		{
			int dx = x - tgt[i][0], dy = y - tgt[i][1];
			int err = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

			if (err > TSCAL_MAXERR) {
				printf("tscal: target %d ev=(%d,%d) is %d px from "
				       "(%d,%d) -- MISS, tap the ring again\n",
				       i + 1, x, y, err, tgt[i][0], tgt[i][1]);
				fflush(stdout);
				/* wait for the finger to come up before re-arming */
				while (read(tfd, &ev, sizeof(ev)) ==
				       (ssize_t)sizeof(ev)) {
					if (ev.type == EV_KEY &&
					    ev.code == BTN_TOUCH && !ev.value)
						break;
					if (ev.type == EV_SYN)
						break;
				}
				usleep(300000);
				continue;	/* same target, drawn again */
			}
		}
		}
		unmap_active(active, x, y, cx0, cx1, cy0, cy1, &rtx, &rty);
		rx[n] = rtx;
		ry[n] = rty;
		tx[n] = tgt[i][0];
		ty[n] = tgt[i][1];
		{
			int mx, my;

			map_raw(active, rtx, rty, &mx, &my);
			printf("tscal: target %d ev=(%d,%d) raw=(0x%03x,0x%03x) "
			       "(active map %d -> %d,%d)\n",
			       i + 1, x, y, rtx, rty, active, mx, my);
			fflush(stdout);
		}
		n++;
		/* wait for release so the next target is a fresh tap */
		while (read(tfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type == EV_KEY && ev.code == BTN_TOUCH && !ev.value)
				break;
			if (ev.type == EV_SYN)
				break;
		}
		usleep(200000);
	}

	clear(&f, bg);
	fb_flip(&f);

	if (n < 2) {
		printf("tscal: not enough taps (%d), leaving mapping unchanged\n", n);
		fflush(stdout);
		return 1;
	}

	for (i = 0; i < 8; i++) {
		long long e = score(i, rx, ry, tx, ty, n);
		printf("tscal: map %d score %lld\n", i, e);
		if (best < 0 || e < best) {
			best = e;
			bestm = i;
		}
	}
	printf("tscal: best map=%d score=%lld (n=%d)\n", bestm, best, n);
	fflush(stdout);

	if (mappath) {
		FILE *m = fopen(mappath, "w");

		if (m) {
			fprintf(m, "%d\n", bestm);
			fclose(m);
		}
	}
	{
		FILE *m = fopen("/proc/ctr_touch_map", "w");

		if (m) {
			fprintf(m, "%d\n", bestm);
			fclose(m);
		}
	}
	printf("tscal: applied map=%d (bit0=swap, bit1=invert-x, bit2=invert-y)\n",
	       bestm);
	fflush(stdout);

	/*
	 * Now the same five taps give the calibration.  vx/vy hold the raw
	 * pairs with the winning swap already applied, i.e. exactly the axes
	 * the driver calibrates, and tx/ty the screen positions they should
	 * have produced.
	 */
	{
		int e0, e1, f0, f1, ok = 1, j;
		double rx_ms = 0.0, ry_ms = 0.0;

		/* Post-swap axes: the winning map decides which raw axis ends up
		 * as screen X, and that is the axis the driver calibrates. */
		for (j = 0; j < n; j++) {
			vx[j] = (bestm & 1) ? ry[j] : rx[j];
			vy[j] = (bestm & 1) ? rx[j] : ry[j];
		}
		/*
		 * Drop outliers and refit.  The tap rejection above catches the
		 * obvious cases, but a tap can still land just inside the window
		 * and be a poor fit, and a single such point moves a
		 * least-squares line a long way.  Refit without the worst point
		 * while it is clearly the worst and enough points remain.
		 */
		for (j = 0; j < 4; j++) {
			double dx = 0, dy = 0, wx = -1, wy = -1;
			double kx, bx, ky, by;

			if (n < 4 || fit_axis(vx, tx, n, &e0, &e1, &rx_ms) != 0 ||
			    fit_axis(vy, ty, n, &f0, &f1, &ry_ms) != 0)
				break;
			kx = 319.0 / (double)(e1 - e0);
			bx = tx[0] - kx * vx[0];
			ky = 239.0 / (double)(f1 - f0);
			by = ty[0] - ky * vy[0];
			for (i = 0; i < n; i++) {
				double ex = (kx * vx[i] + bx) - tx[i];
				double ey = (ky * vy[i] + by) - ty[i];
				double d;

				if (ex < 0) ex = -ex;
				if (ey < 0) ey = -ey;
				d = ex + ey;
				if (d > dx) { dx = d; wx = i; }
				if (d > dy) { dy = d; wy = i; }
			}
			if (dx <= 12.0 && dy <= 12.0)
				break;	/* everyone agrees, good enough */
			{
				int drop = (dx >= dy) ? (int)wx : (int)wy;

				printf("tscal: dropping target %d as an outlier "
				       "(%d px off the fitted line)\n",
				       drop + 1,
				       (int)(dx >= dy ? dx : dy));
				for (i = drop; i < n - 1; i++) {
					rx[i] = rx[i + 1];
					ry[i] = ry[i + 1];
					vx[i] = vx[i + 1];
					vy[i] = vy[i + 1];
					tx[i] = tx[i + 1];
					ty[i] = ty[i + 1];
				}
				n--;
				fflush(stdout);
			}
		}
		if (fit_axis(vx, tx, n, &e0, &e1, &rx_ms) != 0 ||
		    fit_axis(vy, ty, n, &f0, &f1, &ry_ms) != 0 ||
		    e1 - e0 < 16 || f1 - f0 < 16 ||
		    e0 < 0 || f0 < 0 || e1 > MAX_12BIT || f1 > MAX_12BIT ||
		    rx_ms > 8.0 || ry_ms > 8.0) {
			printf("tscal: calibration fit not usable "
			       "(x rms=%.1f, y rms=%.1f), leaving the current one "
			       "in place\n", rx_ms, ry_ms);
			ok = 0;
		}
		if (ok) {
			/* What the taps imply, before and after. */
			for (i = 0; i < n; i++) {
				int bx = vx[i] * 320 / MAX_12BIT;
				int by = vy[i] * 240 / MAX_12BIT;
				int ax = (vx[i] - e0) * 320 / (e1 - e0);
				int ay = (vy[i] - f0) * 240 / (f1 - f0);

				if (ax < 0) ax = 0;
				if (ax > 319) ax = 319;
				if (ay < 0) ay = 0;
				if (ay > 239) ay = 239;
				printf("tscal: target %d screen=(%d,%d) raw=(%d,%d) "
				       "was (%d,%d) now (%d,%d)\n",
				       i + 1, tx[i], ty[i], vx[i], vy[i],
				       bx, by, ax, ay);
			}
			printf("tscal: applied cal x0=%d x1=%d y0=%d y1=%d "
			       "(rms x %.1f px, y %.1f px)\n",
			       e0, e1, f0, f1, rx_ms, ry_ms);
			if (calpath) {
				FILE *c = fopen(calpath, "w");

				if (c) {
					fprintf(c, "%d %d %d %d\n", e0, e1, f0, f1);
					fclose(c);
					printf("tscal: calibration saved to %s\n",
					       calpath);
				}
			}
			{
				FILE *c = fopen("/proc/ctr_touch_cal", "w");

				if (c) {
					fprintf(c, "%d %d %d %d\n",
						e0, e1, f0, f1);
					fclose(c);
				}
			}
		}
		fflush(stdout);
	}
	close(tfd);
	return 0;
}
