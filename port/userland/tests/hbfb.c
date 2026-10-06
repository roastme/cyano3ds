// SPDX-License-Identifier: GPL-2.0
/*
 * hbfb.c - paint a "still alive" progress bar straight onto a framebuffer
 *
 * Bring-up tool for the Cyano3DS port.
 *
 * Why this exists
 * ---------------
 * The machine hard-stops ~90 s into boot, exactly when the first Android app
 * process starts, and *every* log writer stops at the same instant (kernel
 * kmsg -> SD, Android log -> SD, the userspace watchbeat).  That signature is
 * consistent with two very different failures:
 *
 *   (a) the kernel itself stopped scheduling (an IRQ-off spin / a fault), or
 *   (b) the kernel is fine but the SD write path wedged, the logger ring
 *       buffers filled, and every process that logs then blocked.
 *
 * The kernel log cannot distinguish them because writing it out goes through
 * the same wedged SD path.  /bin/heartbeat covers this by painting a bar
 * *directly* into the top-screen framebuffer (fb0), which the LCD controller
 * scans continuously and which needs no printk, no console_lock and no SD:
 *
 *   * bar keeps advancing after the bottom screen freezes -> kernel and
 *     userspace are alive, the freeze is storage/logging only;
 *   * bar freezes at the same moment -> the kernel really did stop.
 *
 * The bar is drawn along the physical bottom edge and grows/resets with the
 * counter, so it is readable regardless of the panel's portrait rotation and
 * cannot be confused with the console text.
 *
 * Usage:  hbfb <fbdev> <counter>
 *         hbfb <fbdev> --loop [interval_ms]
 *
 * In --loop mode it stays resident, redrawing the bar every interval (default
 * 2000 ms) with its own independent counter.  That is deliberately a *separate*
 * process from /bin/heartbeat: the heartbeat includes a printk, and printk can
 * block on console_lock (the top-screen fbcon is very slow).  A liveness
 * indicator that shares a process with a blocking printk is useless exactly
 * when it matters.  This one touches only the framebuffer mapping.
 *
 * Framebuffer pixel packing is taken from the driver's advertised
 * red/green/blue offsets, so this works on both the 24bpp top screen and the
 * 16bpp bottom screen.
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

#define BAND_H 14	/* physical rows used by the indicator */

static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *mem;
static int fb_fd;
static int bpp, linelen;

static void putpx(int x, int y, unsigned int v)
{
	unsigned char *p = mem + (size_t)y * linelen + (size_t)x * (bpp / 8);

	if (x < 0 || y < 0 || x >= (int)var.xres || y >= (int)var.yres)
		return;
	switch (bpp) {
	case 16:
		*(unsigned short *)p = (unsigned short)v;
		break;
	case 24:
		p[0] = v & 0xff;
		p[1] = (v >> 8) & 0xff;
		p[2] = (v >> 16) & 0xff;
		break;
	case 32:
		*(unsigned int *)p = v;
		break;
	}
}

static unsigned int pack(unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int v = 0;

	if (var.red.length)
		v |= ((r >> (8 - var.red.length)) & ((1u << var.red.length) - 1))
		     << var.red.offset;
	if (var.green.length)
		v |= ((g >> (8 - var.green.length)) & ((1u << var.green.length) - 1))
		     << var.green.offset;
	if (var.blue.length)
		v |= ((b >> (8 - var.blue.length)) & ((1u << var.blue.length) - 1))
		     << var.blue.offset;
	return v;
}

int main(int argc, char **argv)
{
	unsigned int black, bar, tip;
	long n = 0, seg;
	int x, y, i, steps = 24;
	int loop = 0, interval_ms = 2000;

	if (argc < 3) {
		fprintf(stderr, "usage: hbfb <fbdev> <counter> | <fbdev> --loop [ms]\n");
		return 2;
	}
	if (!strcmp(argv[2], "--loop")) {
		loop = 1;
		if (argc > 3)
			interval_ms = atoi(argv[3]);
		if (interval_ms < 100)
			interval_ms = 100;
	} else {
		n = atol(argv[2]);
	}

	fb_fd = open(argv[1], O_RDWR);
	if (fb_fd < 0) {
		fprintf(stderr, "hbfb: open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		fprintf(stderr, "hbfb: ioctl: %s\n", strerror(errno));
		return 1;
	}
	bpp = var.bits_per_pixel;
	linelen = fix.line_length;
	mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fb_fd, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "hbfb: mmap: %s\n", strerror(errno));
		return 1;
	}

	black = pack(0, 0, 0);
	bar = pack(0xff, 0xa0, 0x00);	/* orange */
	tip = pack(0x00, 0xff, 0x40);	/* bright green */

	for (;;) {
		/* wipe the band (a full physical row range, so the panel's
		 * portrait rotation does not matter) */
		for (y = var.yres - BAND_H; y < (int)var.yres; y++)
			for (x = 0; x < (int)var.xres; x++)
				putpx(x, y, black);

		seg = (n % steps) * ((int)var.xres / steps);
		for (x = 0; x < seg; x++)
			for (y = var.yres - BAND_H; y < (int)var.yres; y++)
				putpx(x, y, bar);

		/* a chunky tip block, so a single step is unmistakable */
		for (i = 0; i < 10; i++)
			for (y = var.yres - BAND_H; y < (int)var.yres; y++)
				putpx((int)seg + i, y, tip);

		if (!loop)
			break;
		/* No msync(): the fb is a device mapping (remap_pfn_range), not a
		 * page-cache file, so there is nothing to sync - and calling it on
		 * every step risks blocking the very indicator we need.  The writes
		 * above already went straight to VRAM. */
		n++;
		usleep(interval_ms * 1000);
	}

	munmap(mem, fix.smem_len);
	close(fb_fd);
	return 0;
}
