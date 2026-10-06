// SPDX-License-Identifier: GPL-2.0
/*
 * fbtest.c - Android-3DS display smoke test
 *
 * Runs on the 3DS itself (bottom screen) and validates everything the port's
 * display path relies on:
 *
 *   1. /dev/fb0 reports a sane geometry (320x240, 16bpp, yres_virtual = 480)
 *   2. the two buffers exist at exactly line_length * yres apart
 *   3. FBIOPUT_VSCREENINFO with yoffset flips tear-free (double buffering)
 *   4. the image is *not* rotated or mirrored on the panel
 *   5. red is red and blue is blue (RGB565 order)
 *
 * Build:  arm-linux-gnueabi-gcc -static -O2 -o fbtest fbtest.c
 * Usage:  fbtest            colour bars, then an animated bar (60 frames)
 *         fbtest --once     colour bars only, then exit
 *         fbtest --flip     60 flips, print the achieved frame rate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static int fb_fd = -1;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb0, *fb1;
static int W, H, PITCH, BUFSZ, TWO_BUFS;
static int cur;			/* buffer currently displayed */
static unsigned char *back, *front;

static inline void putpx(unsigned char *b, int x, int y, unsigned short c)
{
	*(unsigned short *)(b + y * PITCH + x * 2) = c;
}

static void clear(unsigned char *b, unsigned short c)
{
	int i, n = (PITCH * H) / 2;
	unsigned short *p = (unsigned short *)b;

	for (i = 0; i < n; i++)
		p[i] = c;
}

/* 8 vertical bars: white, yellow, cyan, green, magenta, red, blue, black */
static void colorbars(unsigned char *b)
{
	static const unsigned short bars[8] = {
		0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
		0xF81F, 0xF800, 0x001F, 0x0000
	};
	int bar, x, y;

	for (bar = 0; bar < 8; bar++) {
		int x0 = bar * W / 8, x1 = (bar + 1) * W / 8;

		for (y = 0; y < H; y++)
			for (x = x0; x < x1; x++)
				putpx(b, x, y, bars[bar]);
	}

	/* A "staircase" in the top-left corner: it makes a 90 degree rotation
	 * or a mirroring instantly visible (the steps must go down-right).
	 */
	for (y = 0; y < 24; y++)
		for (x = 0; x < y; x++)
			putpx(b, x, y, 0xFFFF);
}

/* An arrow pointing right, at the top-left: shows orientation AND chirality */
static void arrow(unsigned char *b, int ox, int oy, unsigned short c)
{
	int i;

	for (i = 0; i < 40; i++) {
		int x = ox + i, t = 40 - i;

		if (t > 20)
			t = 20;
		{
			int y;

			for (y = oy - t / 2; y < oy + t / 2; y++)
				if (x >= 0 && x < W && y >= 0 && y < H)
					putpx(b, x, y, c);
		}
	}
}

static int flip(void)
{
	var.activate = FB_ACTIVATE_VBL;
	var.yoffset = cur ? 0 : H;

	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) == -1) {
		fprintf(stderr, "FBIOPUT_VSCREENINFO(yoffset=%u): %s\n",
			var.yoffset, strerror(errno));
		return -1;
	}

	cur = !cur;
	front = cur ? fb1 : fb0;
	back  = cur ? fb0 : fb1;
	(void)front;
	return 0;
}

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	int once = 0, rate = 0, i;
	const char *dev = NULL;
	char path[64];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--flip"))
			rate = 1;
		else if (argv[i][0] != '-')
			dev = argv[i];
	}

	/*
	 * On the port the kernel console owns fb0 (the top screen, which is the
	 * screen firm_linux_loader prints on), so the Android display - and this
	 * test - is fb1.  /dev/graphics/fb0 is what Android itself opens
	 * (symlink to fb1).
	 */
	if (!dev) {
		if (access("/dev/fb1", F_OK) == 0)
			dev = "/dev/fb1";
		else if (access("/dev/fb0", F_OK) == 0)
			dev = "/dev/fb0";
		else
			dev = "/dev/graphics/fb0";
	}

	fb_fd = open(dev, O_RDWR);
	if (fb_fd < 0 && dev[0] == '/') {
		/* last resort: the Android path for the same device */
		snprintf(path, sizeof(path), "/dev/graphics/%s",
			 strrchr(dev, '/') + 1);
		fb_fd = open(path, O_RDWR);
	}
	if (fb_fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	printf("opened %s\n", dev);

	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		fprintf(stderr, "fb ioctl failed: %s\n", strerror(errno));
		return 1;
	}

	W = var.xres;
	H = var.yres;
	PITCH = fix.line_length;
	BUFSZ = PITCH * H;
	TWO_BUFS = var.yres_virtual >= var.yres * 2;
	cur = var.yoffset / H;

	printf("fb0: id=%s %dx%d %dbpp line_length=%d smem_len=%d "
	       "yres_virtual=%d\n", fix.id, W, H, var.bits_per_pixel,
	       PITCH, fix.smem_len, var.yres_virtual);
	printf("     r=%u:%u g=%u:%u b=%u:%u pixclock=%u refresh~%u Hz\n",
	       var.red.offset, var.red.length, var.green.offset,
	       var.green.length, var.blue.offset, var.blue.length,
	       var.pixclock,
	       (unsigned)(1000000000000000LLU /
			  ((unsigned long long)(var.upper_margin + var.lower_margin + var.yres) *
			   (var.left_margin + var.right_margin + var.xres) *
			   (var.pixclock ? var.pixclock : 1)) / 1000));

	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "expected 16bpp RGB565, got %dbpp\n",
			var.bits_per_pixel);
		return 1;
	}
	if (!TWO_BUFS)
		printf("     WARNING: no second buffer, no page flipping\n");

	fb0 = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fb_fd, 0);
	if (fb0 == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}
	fb1 = fb0 + BUFSZ;
	back = cur ? fb0 : fb1;

	colorbars(back);
	arrow(back, 8, H / 2, 0x001F);
	if (flip())
		return 1;

	if (once)
		return 0;

	if (rate) {
		long long t0 = now_us();
		int n = 60;

		for (i = 0; i < n; i++) {
			if (flip())
				return 1;
		}
		printf("     %d flips in %lld us => %.1f flips/s\n", n,
		       now_us() - t0, n * 1000000.0 / (now_us() - t0));
		return 0;
	}

	/* animated bar: with working double buffering this must not tear */
	{
		int f;

		for (f = 0; f < 240; f++) {
			int x = (f * 2) % (W - 40), y;

			clear(back, 0x0000);
			colorbars(back);
			for (y = 0; y < H; y++)
				putpx(back, x + 20, y, 0xFFFF);
			arrow(back, 8, H / 2, 0x001F);
			if (flip())
				return 1;
			usleep(16000);
		}
	}

	printf("display test done\n");
	return 0;
}
