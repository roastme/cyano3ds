// SPDX-License-Identifier: GPL-2.0
/*
 * fbsay.c - display text on a framebuffer, for consoles with no keyboard
 *
 * Bring-up tool for the Cyano3DS port.  The 3DS has no
 * serial port, no keyboard and no network in Linux yet, so when something goes
 * wrong during boot the only channel is the pair of LCDs: one is the kernel
 * console (fb0, top screen) and the other is the display this port drives
 * (fb1, bottom screen).
 *
 * fbsay reads lines from stdin and renders them as large, readable text on a
 * framebuffer, paging through them automatically (nobody can press a key), so
 * the boot log can simply be read off the bottom screen - or photographed for
 * someone else to read.
 *
 * Usage:
 *   fbsay [device] [-s scale] [--once] [--delay seconds]
 *   ... < boot.log
 *
 *   device   /dev/fb1 (default; the Android/3DS bottom screen)
 *   -s N     text scale, 1..4 (default 2: 20 columns x 14 rows, ~16px glyphs)
 *   --once   render the last page and exit, instead of cycling
 *   --delay  seconds per page while cycling (default 4)
 *
 * The framebuffer must be 16bpp (RGB565), which is what ctr_lcd_fb advertises.
 * Page flipping is used when the driver offers two buffers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "font8x8.h"

static int fb_fd = -1;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb_mem, *buf0, *buf1, *back;
static int W, H, PITCH, BUFSZ;
static int cur;

/* ------------------------------------------------------------------ */

static inline void putpx(unsigned char *b, int x, int y, unsigned short c)
{
	*(unsigned short *)(b + y * PITCH + x * 2) = c;
}

static void fill(unsigned char *b, unsigned short c)
{
	int i, n = (PITCH * H) / 2;
	unsigned short *p = (unsigned short *)b;

	for (i = 0; i < n; i++)
		p[i] = c;
}

static void draw_char(unsigned char *b, int cx, int cy, int scale,
		      unsigned char ch, unsigned short colour)
{
	const unsigned char *glyph = font8x8 + (unsigned)ch * 8;
	int row, col, sy, sx;

	for (row = 0; row < 8; row++) {
		unsigned char bits = glyph[row];

		for (col = 0; col < 8; col++) {
			if (!(bits & (0x80 >> col)))
				continue;
			for (sy = 0; sy < scale; sy++)
				for (sx = 0; sx < scale; sx++)
					putpx(b, cx + col * scale + sx,
					      cy + row * scale + sy, colour);
		}
	}
}

static void draw_line(unsigned char *b, int x, int y, int scale,
		      const char *s, unsigned short colour, int maxcols)
{
	int i;

	for (i = 0; i < maxcols && s[i]; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c > 0x7e)
			c = c == '\t' ? ' ' : '.';
		draw_char(b, x + i * 8 * scale, y, scale, c, colour);
	}
}

static int flip(void)
{
	var.activate = FB_ACTIVATE_VBL;
	var.yoffset = cur ? 0 : H;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) == -1)
		return -1;
	cur = !cur;
	buf0 = fb_mem;
	buf1 = fb_mem + BUFSZ;
	back = cur ? buf0 : buf1;
	return 0;
}

/* ------------------------------------------------------------------ */

#define MAX_LINES 1024
#define MAX_LEN   256

int main(int argc, char **argv)
{
	static char lines[MAX_LINES][MAX_LEN];
	int nlines = 0, scale = 2, once = 0, delay = 4;
	const char *dev = NULL;
	int i, cols, text_rows, pages, page;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc)
			scale = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
			delay = atoi(argv[++i]);
		else if (argv[i][0] != '-')
			dev = argv[i];
	}
	if (scale < 1) scale = 1;
	if (scale > 4) scale = 4;
	if (delay < 1) delay = 1;

	if (!dev)
		dev = access("/dev/fb1", F_OK) == 0 ? "/dev/fb1" : "/dev/fb0";

	fb_fd = open(dev, O_RDWR);
	if (fb_fd < 0) {
		fprintf(stderr, "fbsay: cannot open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		fprintf(stderr, "fbsay: fb ioctl failed: %s\n", strerror(errno));
		return 1;
	}
	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "fbsay: %s is %dbpp, need 16 (RGB565)\n",
			dev, var.bits_per_pixel);
		return 1;
	}

	W = var.xres;
	H = var.yres;
	PITCH = fix.line_length;
	BUFSZ = PITCH * H;
	fb_mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fprintf(stderr, "fbsay: mmap: %s\n", strerror(errno));
		return 1;
	}
	cur = var.yoffset / H;
	buf0 = fb_mem;
	buf1 = fb_mem + BUFSZ;
	back = cur ? buf0 : buf1;

	/*
	 * Geometry first: the input has to be wrapped at the real column count.
	 * (The first version hard-wrapped at 78 characters and then clipped at
	 * `cols` when drawing, so every line was cut off around 20 characters -
	 * which is exactly how the on-screen log hid the ashmem failure reason.)
	 */
	cols = W / (8 * scale);
	text_rows = H / (8 * scale) - 1;	/* one row for the page indicator */
	if (text_rows < 1)
		text_rows = 1;
	if (cols < 1)
		cols = 1;

	/* --- read and wrap the input --- */
	{
		char raw[MAX_LEN];

		while (nlines < MAX_LINES && fgets(raw, sizeof(raw), stdin)) {
			int len = strlen(raw);
			int off = 0;

			while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
				raw[--len] = 0;
			/* expand tabs a little so kernel messages stay aligned */
			{
				char tmp[MAX_LEN];
				int t, o = 0;

				for (t = 0; t < len && o < MAX_LEN - 1; t++) {
					if (raw[t] == '\t') {
						do { tmp[o++] = ' '; }
						while (o % 8 && o < MAX_LEN - 1);
					} else {
						tmp[o++] = raw[t];
					}
				}
				tmp[o] = 0;
				memcpy(raw, tmp, o + 1);
				len = o;
			}

			if (len == 0) {
				lines[nlines][0] = 0;
				nlines++;
				continue;
			}
			/* hard-wrap long lines at the real column count */
			while (off < len && nlines < MAX_LINES) {
				int n = len - off;

				if (n > cols)
					n = cols;
				memcpy(lines[nlines], raw + off, n);
				lines[nlines][n] = 0;
				nlines++;
				off += n;
			}
		}
	}
	if (nlines == 0) {
		strcpy(lines[0], "(fbsay: no input)");
		nlines = 1;
	}

	cols = W / (8 * scale);
	text_rows = H / (8 * scale) - 1;	/* one row for the page indicator */
	if (text_rows < 1)
		text_rows = 1;
	pages = (nlines + text_rows - 1) / text_rows;

	printf("fbsay: %s %dx%d, scale %d => %d cols x %d text rows, "
	       "%d lines, %d page(s)\n", dev, W, H, scale, cols, text_rows,
	       nlines, pages);

	page = once ? pages - 1 : 0;
	for (;;) {
		char ind[64];
		int r;

		fill(back, 0x0842);	/* dark navy: easy to read, proves colour */
		for (r = 0; r < text_rows; r++) {
			int li = page * text_rows + r;

			if (li >= nlines)
				break;
			draw_line(back, 0, r * 8 * scale, scale, lines[li], 0xFFFF, cols);
		}
		snprintf(ind, sizeof(ind), "page %d/%d  (scale %d, %d cols)",
			 page + 1, pages, scale, cols);
		draw_line(back, 0, text_rows * 8 * scale, scale, ind, 0xFFE0, cols);
		if (flip())
			break;

		if (once)
			break;
		sleep(delay);
		page = (page + 1) % pages;
	}

	return 0;
}
