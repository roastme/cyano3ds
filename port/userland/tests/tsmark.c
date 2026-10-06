// SPDX-License-Identifier: GPL-2.0
/*
 * tsmark.c - draw a crosshair on the TOP screen at the touch position
 *
 * M4 bring-up tool.  The TSC driver now reports real samples and Android's
 * EventHub/KeyInputQueue get them, but a tap on an on-screen button sometimes
 * does nothing, so we need to see *where* the touch lands independently of
 * Android (and of the bottom screen, which SurfaceFlinger owns).
 *
 * The top screen is the kernel console (fb0) and is otherwise idle after boot,
 * so as long as the raw 320x240 touch is mapped correctly a red crosshair
 * should follow the finger 1:1 across the top screen.  If it is mirrored,
 * swapped or offset, that is immediately visible and tells us to fix the
 * coordinate mapping (or calibration) rather than the kernel.
 *
 * Mapping: the top framebuffer is 240x400 landscape-viewed-through-portrait,
 * visible(X,Y) lives at fb column (239-Y), row X.  The bottom touch (tx,ty) is
 * 320x240; scale it onto the 400x240 visible top screen as
 * (X = tx*400/320, Y = ty).
 *
 * Usage: tsmark [/dev/fb0] [/dev/input/eventN]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

struct fb {
	int fd;
	int bpp, linelen;
	unsigned char *mem;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
};

static int fb_open(struct fb *f, const char *path)
{
	f->fd = open(path, O_RDWR);
	if (f->fd < 0) {
		fprintf(stderr, "tsmark: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) ||
	    ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix)) {
		fprintf(stderr, "tsmark: ioctl %s: %s\n", path, strerror(errno));
		return -1;
	}
	f->bpp = f->var.bits_per_pixel;
	f->linelen = f->fix.line_length;
	f->mem = mmap(NULL, f->fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      f->fd, 0);
	if (f->mem == MAP_FAILED) {
		fprintf(stderr, "tsmark: mmap %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

static void putpx(struct fb *f, int x, int y, unsigned int v)
{
	unsigned char *p;

	if (x < 0 || y < 0 || x >= (int)f->var.xres || y >= (int)f->var.yres)
		return;
	p = f->mem + (size_t)y * f->linelen + (size_t)x * (f->bpp / 8);
	switch (f->bpp) {
	case 16: *(unsigned short *)p = (unsigned short)v; break;
	case 24: p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; break;
	case 32: *(unsigned int *)p = v; break;
	}
}

static unsigned int pack_rgb(struct fb *f, unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int v = 0;

	if (f->var.red.length)
		v |= ((r >> (8 - f->var.red.length)) & ((1u << f->var.red.length) - 1))
		     << f->var.red.offset;
	if (f->var.green.length)
		v |= ((g >> (8 - f->var.green.length)) & ((1u << f->var.green.length) - 1))
		     << f->var.green.offset;
	if (f->var.blue.length)
		v |= ((b >> (8 - f->var.blue.length)) & ((1u << f->var.blue.length) - 1))
		     << f->var.blue.offset;
	return v;
}

/* Draw (or erase) a crosshair at visible top-screen (X,Y). */
static void crosshair(struct fb *f, int X, int Y, unsigned int color, int r)
{
	int i;

	for (i = -r; i <= r; i++) {
		/* fb: column = 239-Y, row = X */
		putpx(f, 239 - (Y + i), X, color);         /* vertical arm */
		putpx(f, 239 - Y, X + i, color);         /* horizontal arm */
	}
}

/* Bottom-screen touch (tx,ty) -> top-screen visible (X,Y). */
static void map_top(int tx, int ty, int *X, int *Y)
{
	*X = tx * 400 / 320;
	*Y = ty;
}

static int is_touchscreen(int fd)
{
	unsigned long absbits[(ABS_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
	unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];

	memset(absbits, 0, sizeof(absbits));
	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
		return 0;
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
		return 0;
	return !!(absbits[ABS_X / (8 * sizeof(unsigned long))] & (1UL << (ABS_X % (8 * sizeof(unsigned long))))) &&
	       !!(absbits[ABS_Y / (8 * sizeof(unsigned long))] & (1UL << (ABS_Y % (8 * sizeof(unsigned long))))) &&
	       !!(keybits[BTN_TOUCH / (8 * sizeof(unsigned long))] & (1UL << (BTN_TOUCH % (8 * sizeof(unsigned long)))));
}

static int open_touch(const char *want)
{
	int i, fd = -1;

	if (want && (fd = open(want, O_RDONLY)) >= 0)
		return fd;
	for (i = 0; i < 16; i++) {
		char p[64];

		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		if (is_touchscreen(fd))
			return fd;
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *fbpath = argc > 1 ? argv[1] : "/dev/fb0";
	const char *inpath = argc > 2 ? argv[2] : NULL;
	struct fb f;
	struct input_event ev;
	int tfd, x = -1, y = -1, down = 0, px = -1, py = -1;
	unsigned int red, white;

	memset(&f, 0, sizeof(f));
	if (fb_open(&f, fbpath) < 0)
		return 1;
	red = pack_rgb(&f, 0xff, 0, 0);
	white = pack_rgb(&f, 0xff, 0xff, 0xff);

	tfd = open_touch(inpath);
	if (tfd < 0) {
		fprintf(stderr, "tsmark: no touchscreen found: %s\n", strerror(errno));
		return 1;
	}

	printf("tsmark: fb=%s %ux%u %dbpp, touch fd=%d\n",
	       fbpath, f.var.xres, f.var.yres, f.bpp, tfd);
	fflush(stdout);

	while (read(tfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_ABS && ev.code == ABS_X)
			x = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_Y)
			y = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
			down = ev.value;
		else
			continue;

		if (x < 0 || y < 0)
			continue;

		{
			int X, Y;

			map_top(x, y, &X, &Y);
			/* erase the previous marker, draw the new one */
			if (px >= 0) {
				int oX, oY;

				map_top(px, py, &oX, &oY);
				crosshair(&f, oX, oY, pack_rgb(&f, 0, 0, 0), 9);
			}
			crosshair(&f, X, Y, down ? red : white, 9);
			px = x;
			py = y;
		}
	}

	printf("tsmark: read ended: %s\n", strerror(errno));
	close(tfd);
	munmap(f.mem, f.fix.smem_len);
	close(f.fd);
	return 0;
}
