// SPDX-License-Identifier: GPL-2.0
/*
 * ctr_lcd_fb.c - Nintendo 3DS bottom-screen LCD framebuffer driver
 *
 * Part of the Android port to the Nintendo 3DS.
 *
 *
 * WHY THIS DRIVER EXISTS
 * ----------------------
 * The 3DS LCD panels are wired in portrait orientation: the bottom panel is
 * scanned as 240-pixel lines with 320 lines per frame, even though the screen
 * is 320x240 when the console is held normally.  Android renders straight
 * into the mmap'ed framebuffer (EGLDisplaySurface.cpp) and no HAL sits on that
 * path, so the 90 degree rotation has to happen in the kernel.
 *
 * This driver therefore advertises a *normal landscape* 320x240 RGB565 fbdev
 * with two page-flippable buffers - byte for byte the interface
 * EGLDisplaySurface::mapFrameBuffer() asks for - and on flip converts and
 * rotates the finished frame into the panel's portrait scanout buffer.
 *
 *
 * WHAT THE HARDWARE ACTUALLY LOOKS LIKE (measured, not assumed)
 * ------------------------------------------------------------
 * Luma3DS is the reference implementation that works on every 3DS
 * (arm11/source/main.c, initScreens()), so this driver deliberately mirrors it
 * instead of guessing.  For the bottom screen (PDC1, registers at 0x10400500):
 *
 *   0x500 HTotal   = 0x1C2 (450)
 *   0x510 HSync    = 0xCD
 *   0x560 HDisp    = 0x01C100D1  -> image 209..449 = 240 px per scanline (!)
 *   0x564 (low 0x0052, high 0x0192)
 *   0x568/0x56C    = framebuffer A, first/second address (the flip pair)
 *   0x570 Format   = 0x80301  -> colour format 1 = 24bpp, *stored BGR*,
 *                                no interlacing, no alternate pixel mode,
 *                                128-byte DMA bursts
 *   0x574 Control  = 0x00010501 (bit 0 = enable display controller)
 *   0x578 Select   = 0, bit 0 = "next framebuffer after VBlank"
 *   0x590 Stride   = 0x2D0 = 720 = 240 px * 3 bytes
 *
 * and Luma's bottom framebuffer size is 3 * 320 * 240 = 230400 bytes, i.e.
 * 320 scanlines of 720 bytes: a 240x320 portrait buffer, exactly as predicted
 * from 3dbrew ("the first pixel is in the bottom-left corner") and libctru
 * (GSP_SCREEN_WIDTH 240, GSP_SCREEN_HEIGHT_BOTTOM 320).
 *
 * DESIGN RULE THAT FOLLOWS FROM THIS
 * ----------------------------------
 * The panel's timing and pixel format are *owned by the bootloader* and are
 * known-good.  This driver never touches them: it reads the PDC1 registers,
 * derives the scanout geometry from them, allocates two buffers of its own,
 * and writes exactly two things - the two buffer addresses (0x568/0x56C) and
 * the select bit (0x578).  Everything else (format, stride, timing) is left
 * untouched, and the blit adapts to whatever format/stride it found.
 *
 * The fbdev side is completely normal, which is the whole point:
 *
 *   xres = 320, yres = 240, 16bpp RGB565, line_length = 640,
 *   yres_virtual = 480 (two buffers), FBIOPUT_VSCREENINFO(yoffset) = flip
 *
 * so Android runs unpatched, in landscape, on a display whose panel is
 * internally portrait.  RGB565 output needs no colour conversion when the
 * panel format is 2; when it is 3 (Luma's 24bpp BGR, the default) the blit
 * writes B,G,R bytes.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/gfp.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>
#include <asm/ptrace.h>
#include <asm/cacheflush.h>
#include <linux/font.h>
#include <linux/kallsyms.h>
#include <linux/stacktrace.h>
#include <linux/kthread.h>
#include <linux/fs.h>
#include <linux/wait.h>

#define DRV_NAME	"ctr-lcd-fb"

/* ---- PDC (primitive display controller) register offsets ---- */
#define PDC_HDISP	0x60
#define PDC_VTOTAL	0x64
#define PDC_FB_A0	0x68	/* framebuffer A, first address  */
#define PDC_FB_A1	0x6C	/* framebuffer A, second address */
#define PDC_FORMAT	0x70
#define PDC_CTRL	0x74
#define PDC_SELECT	0x78
#define PDC_STRIDE	0x90

#define PDC_CTRL_ENABLE		BIT(0)
#define PDC_SELECT_NEXT		BIT(0)	/* takes effect after VBlank */
#define PDC_SELECT_CURRENT	BIT(4)

/* ---- panel geometry as scanned by the hardware ---- */
#define LCD_PXLINES_PORTRAIT	240	/* 240 px/line, 320 lines  */
#define LCD_LINES_PORTRAIT	320
#define LCD_PXLINES_LANDSCAPE	320	/* 320 px/line, 240 lines  */
#define LCD_LINES_LANDSCAPE	240

/* ---- geometry advertised to the OS (always landscape) ---- */
#define FB_WIDTH	320
#define FB_HEIGHT	240
#define FB_BPP		16	/* RGB565, what Android wants by default */

/* transpose tile: 16 source rows x 16 source columns */
#define BLK		16

struct ctr_fb {
	struct fb_info		*info;

	void __iomem		*pdc;
	void __iomem		*vram;		/* the two scanout buffers */
	phys_addr_t		vram_phys;
	size_t			vram_size;

	void			*fb_mem;	/* the two OS-visible buffers (FCRAM) */
	phys_addr_t		fb_phys;
	size_t			fb_size;	/* one OS buffer  */
	size_t			smem_len;	/* both OS buffers */

	/* OS-visible geometry: always 320x240 RGB565 */
	unsigned int		width;
	unsigned int		height;
	unsigned int		pitch;

	/* scanout geometry, derived from the bootloader's registers */
	unsigned int		px_per_line;
	unsigned int		lcd_w;
	unsigned int		lcd_h;
	unsigned int		lcd_bpp;	/* 2 (RGB565) or 3 (BGR) */
	unsigned int		lcd_stride;
	size_t			scanout_size;
	bool			transpose;
	bool			we_set_format;
	bool			par_set;		/* PDC already programmed (set_par is
						 * called on every page flip!) */

	unsigned int		cur;		/* OS buffer on screen    */
	unsigned int		lcd_idx;	/* scanout buffer selected */
};

/* ------------------------------------------------------------------ */
/* low level scanout pixel access                                     */
/* ------------------------------------------------------------------ */

static inline void lcd_putpx(struct ctr_fb *f, void __iomem *buf,
			     unsigned int x, unsigned int y,
			     unsigned int r, unsigned int g, unsigned int b)
{
	u8 __iomem *p = (u8 __iomem *)buf + y * f->lcd_stride + x * f->lcd_bpp;

	if (f->lcd_bpp == 3) {
		/* 3dbrew: colour components are stored in reverse byte order,
		 * so "RGB8" (format 1) is B,G,R in memory.
		 */
		writeb(b, p);
		writeb(g, p + 1);
		writeb(r, p + 2);
	} else {
		u16 c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

		writew(c, (u16 __iomem *)p);
	}
}

/* ------------------------------------------------------------------ */
/* probe-time self test pattern                                       */
/* ------------------------------------------------------------------ */

/*
 * Probe-time test card.
 *
 * This is deliberately drawn the *userland* way round: a landscape 320x240
 * image is written into the OS-visible buffer in FCRAM and then put on the
 * panel through ctr_blit_rot(), i.e. through the same transposition that all
 * userspace drawing (fbtest, fbsay, and later Android) goes through.
 *
 * An earlier version painted straight into the VRAM scanout buffer, which
 * bypasses the transpose - so a broken transpose was invisible until
 * userspace drew something, and looked like "garbage with letters in it".
 *
 * Expected result on the panel (see docs/DISPLAY.md):
 *   - four VERTICAL bands, left to right: red, green, blue, white
 *   - corner markers, 32x32: yellow top-left, magenta top-right,
 *     cyan bottom-left, red bottom-right
 *   - a white staircase in the top-left growing down-right
 *   - a blue triangle pointing right in the middle
 */
static void ctr_fb_rect(struct ctr_fb *f, u16 *buf, int x, int y, int w, int h,
			u16 colour)
{
	int i, j;

	for (j = y; j < y + h; j++) {
		if (j < 0 || j >= (int)f->height)
			continue;
		for (i = x; i < x + w; i++) {
			if (i < 0 || i >= (int)f->width)
				continue;
			buf[j * f->width + i] = colour;
		}
	}
}

static void ctr_lcd_testcard(struct ctr_fb *f, u16 *buf)
{
	const u16 red = 0xf800, green = 0x07e0, blue = 0x001f;
	const u16 white = 0xffff, yellow = 0xffe0, magenta = 0xf81f;
	const u16 cyan = 0x07ff;
	int i, bar = f->width / 4;

	/* four vertical bands */
	ctr_fb_rect(f, buf, 0,          0, bar, f->height, red);
	ctr_fb_rect(f, buf, bar,        0, bar, f->height, green);
	ctr_fb_rect(f, buf, bar * 2,    0, bar, f->height, blue);
	ctr_fb_rect(f, buf, bar * 3,    0, bar, f->height, white);

	/* corner markers (orientation / mirroring) */
	ctr_fb_rect(f, buf, 0, 0, 32, 32, yellow);
	ctr_fb_rect(f, buf, f->width - 32, 0, 32, 32, magenta);
	ctr_fb_rect(f, buf, 0, f->height - 32, 32, 32, cyan);
	ctr_fb_rect(f, buf, f->width - 32, f->height - 32, 32, 32, red);

	/* white staircase growing down-right, drawn over the bands */
	for (i = 0; i < 6; i++)
		ctr_fb_rect(f, buf, 40 + i * 12, 40 + i * 12, 12, 12, white);

	/* blue triangle pointing right, in the middle */
	for (i = 0; i < 40; i++) {
		int t = (40 - i) / 2;

		ctr_fb_rect(f, buf, 130 + i, 120 - t, 1, 2 * t + 1, blue);
	}
}

/* ------------------------------------------------------------------ */
/* flip path                                                          */
/* ------------------------------------------------------------------ */

/* pan-display bookkeeping for the ARM9 black box (timer-published) */
static unsigned int ctr_pan_calls;
static unsigned int ctr_pan_early;
static unsigned int ctr_wait_timeouts;
static unsigned int ctr_wait_max_us;
static unsigned int ctr_blit_max_us;

/*
 * The bring-up liveness/watchdog/capture timers are OFF by default now that
 * the port is usable.  They do a kallsyms lookup in the timer softirq every
 * 500 ms and paint a square on the top screen; enable with
 * ctr_lcd_fb.debug=1 on the kernel command line, or build the marker.
 */
static bool ctr_debug;
module_param_named(debug, ctr_debug, bool, 0444);
MODULE_PARM_DESC(debug, "bring-up heartbeat/watchdog timers (default off)");

/*
 * Wait for the panel to actually latch our previous selection.
 *
 * We are about to overwrite the back buffer (`next`), so we need the panel
 * to be scanning the front buffer (`idx == f->lcd_idx`), not the back buffer
 * it may still be reading from the flip before last.  PDC_SELECT_CURRENT
 * reports which buffer is on screen right now; the PDC latches a new
 * PDC_SELECT_NEXT only at VBlank (~16.7 ms).
 *
 * The old code tested the *opposite* condition (`CURRENT != idx`), so every
 * flip waited the full 20 ms timeout before blitting.  On a single ARM11 that
 * serialised the whole system and looked like a compositor freeze.
 *
 * Sleep rather than busy-wait: this runs from fb_pan_display() process
 * context (fb ioctl / fb_set_var, under console_lock, a sleepable mutex).
 */
static void ctr_lcd_wait_for_current(struct ctr_fb *f, unsigned int idx)
{
	/*
	 * The panel frame is ~16.7 ms, so the previous PDC selection has latched
	 * within one frame.  This kernel has CONFIG_HIGH_RES_TIMERS=n and HZ=250,
	 * so usleep_range() rounds up to a full 4 ms jiffy - the original 250-count
	 * loop therefore meant *up to one second* per flip, all of it with
	 * console_lock held, which throttled the whole compositor to ~1 fps.
	 *
	 * /proc/ctr_lcd_flips then showed the wait still timing out on ~50% of
	 * flips (on this hardware PDC_SELECT_CURRENT lags the written selection by
	 * about a frame), so bound it by time instead of a loop count: just over
	 * one panel frame is enough for the latch, and a bit that never matches
	 * can no longer stall a flip for more than that.
	 */
	ktime_t deadline = ktime_add_ms(ktime_get(), 20);

	while (ktime_before(ktime_get(), deadline)) {
		u32 sel = readl(f->pdc + PDC_SELECT);

		if (!!(sel & PDC_SELECT_CURRENT) == !!idx)
			return;
		usleep_range(1000, 2000);
	}
	ctr_wait_timeouts++;
}

/*
 * Rotate a finished landscape RGB565 frame into the portrait scanout buffer:
 *
 *     lcd(X * stride + (240 - 1 - Y) * bpp) = frame(Y * width + X)
 *
 * The inner loop is contiguous (in reverse) on the destination, so each run
 * lands in a single write-combining burst.
 */
static void ctr_blit_rot(struct ctr_fb *f, const u16 *src, void __iomem *dst)
{
	u16 blk[BLK][BLK];
	unsigned int xb, yb, x, y;

	for (yb = 0; yb < f->height; yb += BLK) {
		for (xb = 0; xb < f->width; xb += BLK) {
			for (y = 0; y < BLK; y++)
				memcpy(blk[y], src + (yb + y) * f->width + xb,
				       BLK * sizeof(u16));

			for (x = 0; x < BLK; x++) {
				u8 __iomem *p = (u8 __iomem *)dst +
					(xb + x) * f->lcd_stride +
					(f->lcd_w - 1 - yb - (BLK - 1)) * f->lcd_bpp;

				if (f->lcd_bpp == 3) {
					/*
					 * One inner run is BLK*3 = 48 contiguous bytes
					 * at a 4-byte-aligned destination (both
					 * (xb+x)*lcd_stride and the
					 * (lcd_w-1-yb-(BLK-1))*3 term are multiples
					 * of 16 bytes for BLK=16).  Pack the run and
					 * store 32-bit words: fewer, wider stores let
					 * the ARM11 write buffer merge them instead of
					 * 48 separate byte stores per run.
					 */
					u32 w[BLK * 3 / 4];
					u8 *bp = (u8 *)w;
					int i;

					for (y = BLK; y-- > 0; ) {
						u16 c = blk[y][x];

						*bp++ = (c & 0x1f) << 3;
						*bp++ = ((c >> 5) & 0x3f) << 2;
						*bp++ = ((c >> 11) & 0x1f) << 3;
					}
					for (i = 0; i < BLK * 3 / 4; i++)
						__raw_writel(w[i],
							(u32 __iomem *)p + i);
				} else {
					for (y = BLK; y-- > 0; ) {
						u16 c = blk[y][x];

						writew(c, (u16 __iomem *)p);
						p += 2;
					}
				}
			}
		}
	}
}

/* The panel is scanned landscape: straight copy with colour conversion. */
static void ctr_blit_direct(struct ctr_fb *f, const u16 *src, void __iomem *dst)
{
	unsigned int x, y;

	for (y = 0; y < f->height; y++) {
		u8 __iomem *p = (u8 __iomem *)dst + y * f->lcd_stride;

		for (x = 0; x < f->width; x++) {
			u16 c = src[y * f->width + x];

			if (f->lcd_bpp == 3) {
				writeb((c & 0x1f) << 3, p);
				writeb(((c >> 5) & 0x3f) << 2, p + 1);
				writeb(((c >> 11) & 0x1f) << 3, p + 2);
				p += 3;
			} else {
				writew(c, (u16 __iomem *)p);
				p += 2;
			}
		}
	}
}

static unsigned long ctr_flip_count;
static pid_t ctr_last_flip_pid;
static char ctr_last_flip_comm[TASK_COMM_LEN];

/*
 * /proc/ctr_lcd_flips - the exact number of flips since boot.
 *
 * The kernel log only prints the first 24 flips and then every 128th (the
 * panel would otherwise flood the very slow fbcon console), so "no flip line
 * in kmsg.log" does NOT mean "nothing flipped" - the bring-up probes read this
 * file instead to tell the compositor's flips from fbsay's handful.
 */
static int ctr_flips_show(struct seq_file *m, void *v)
{
	seq_printf(m, "flips %lu last_pid %d last_comm %s\n",
		   ctr_flip_count, ctr_last_flip_pid,
		   ctr_last_flip_comm[0] ? ctr_last_flip_comm : "-");
	seq_printf(m, "pan_calls %u pan_early %u wait_timeouts %u wait_max_us %u blit_max_us %u\n",
		   ctr_pan_calls, ctr_pan_early, ctr_wait_timeouts,
		   ctr_wait_max_us, ctr_blit_max_us);
	return 0;
}

static int ctr_flips_open(struct inode *inode, struct file *file)
{
	return single_open(file, ctr_flips_show, NULL);
}

static const struct proc_ops ctr_flips_fops = {
	.proc_open	= ctr_flips_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ctr_fb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct ctr_fb *f = info->par;
	unsigned int idx = var->yoffset / info->var.yres;
	unsigned int next;
	const u16 *src;
	void __iomem *dst;

	if (idx > 1)
		return -EINVAL;
	ctr_pan_calls++;
	if (idx == f->cur) {
		ctr_pan_early++;
		return 0;
	}

	next = 1 - f->lcd_idx;
	src  = (const u16 *)f->fb_mem + idx * f->fb_size;
	/* f->fb_mem is byte addressed */
	src  = (const u16 *)((u8 *)f->fb_mem + idx * f->fb_size);
	dst  = (u8 __iomem *)f->vram + next * f->scanout_size;

	{
		ktime_t t0 = ktime_get(), t1;
		unsigned int dt;

		ctr_lcd_wait_for_current(f, f->lcd_idx);
		t1 = ktime_get();
		dt = ktime_to_us(ktime_sub(t1, t0));
		if (dt > ctr_wait_max_us)
			ctr_wait_max_us = dt;

		t0 = ktime_get();
		if (f->transpose)
			ctr_blit_rot(f, src, dst);
		else
			ctr_blit_direct(f, src, dst);
		t1 = ktime_get();
		dt = ktime_to_us(ktime_sub(t1, t0));
		if (dt > ctr_blit_max_us)
			ctr_blit_max_us = dt;
	}

	/* make sure the pixels are in VRAM before the panel looks at them */
	mb();

	writel(next ? PDC_SELECT_NEXT : 0, f->pdc + PDC_SELECT);

	f->lcd_idx = next;
	f->cur = idx;

	/* Bring-up: prove whether anyone is actually flipping the panel.  If
	 * Android's UI never appears but this keeps counting, the compositor is
	 * drawing and the problem is elsewhere; if it stays at 0 after boot,
	 * nothing is calling fb_pan_display at all. */
	ctr_flip_count++;
	ctr_last_flip_pid = task_pid_nr(current);
	strscpy(ctr_last_flip_comm, current->comm, sizeof(ctr_last_flip_comm));
	if (ctr_flip_count <= 24 || (ctr_flip_count & 0x7f) == 0)
		pr_info("ctr-lcd: flip #%lu idx=%u pid=%d comm=%s\n",
			ctr_flip_count, idx, task_pid_nr(current),
			current->comm);

	return 0;
}

/* ------------------------------------------------------------------ */
/* fb_ops                                                             */
/* ------------------------------------------------------------------ */

static int ctr_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct ctr_fb *f = info->par;

	if (var->bits_per_pixel != FB_BPP)
		return -EINVAL;
	if (var->xres != f->width || var->yres != f->height)
		return -EINVAL;

	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->yres_virtual < var->yres)
		var->yres_virtual = f->height * 2;
	if (var->yres_virtual > f->height * 2)
		var->yres_virtual = f->height * 2;

	var->xoffset = 0;
	if (var->yoffset > var->yres_virtual - var->yres)
		return -EINVAL;
	if (var->yoffset % var->yres)
		var->yoffset = 0;

	var->red.offset = 11;  var->red.length = 5;
	var->green.offset = 5; var->green.length = 6;
	var->blue.offset = 0;  var->blue.length = 5;
	var->transp.offset = 0; var->transp.length = 0;
	var->nonstd = 0;
	var->vmode = FB_VMODE_NONINTERLACED;
	var->height = 46;	/* ~46 x 61 mm  => ~133 dpi, the real panel */
	var->width = 61;
	/*
	 * EGLDisplaySurface.cpp divides by pixclock when computing the refresh
	 * rate, so it must never be zero.  These margins/clock report ~59.8 Hz:
	 * 1e15 / ((12+11+240) * (12+12+320) * 184745)
	 */
	var->upper_margin = 12;
	var->lower_margin = 11;
	var->left_margin  = 12;
	var->right_margin = 12;
	var->hsync_len = 4;
	var->vsync_len = 1;
	var->pixclock = 184745;
	var->sync = 0;
	var->activate = FB_ACTIVATE_NOW;

	return 0;
}

static int ctr_fb_set_par(struct fb_info *info)
{
	struct ctr_fb *f = info->par;
	u32 v;

	info->fix.line_length = f->pitch;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.ypanstep = info->var.yres;

	/*
	 * fb_set_var() calls fb_set_par() unconditionally for *every*
	 * FBIOPUT_VSCREENINFO - and SurfaceFlinger issues one of those per
	 * frame to page-flip.  Re-programming the live PDC buffer addresses and
	 * control register mid-scan can glitch (or stall) the display, so do the
	 * hardware setup exactly once.
	 */
	if (f->par_set)
		return 0;

	/* Never touch the panel timing (PDC_HDISP/PDC_VTOTAL): the bootloader
	 * configured it and it is known good.  Same for the format and stride
	 * unless the device tree explicitly asked for a different bpp.
	 */
	if (f->we_set_format) {
		writel((f->lcd_bpp == 3) ? 0x80301 : 0x80302,
		       f->pdc + PDC_FORMAT);
		writel(f->lcd_stride, f->pdc + PDC_STRIDE);
	}

	/* Only the buffer pair is ours. */
	writel(f->vram_phys, f->pdc + PDC_FB_A0);
	writel(f->vram_phys + f->scanout_size, f->pdc + PDC_FB_A1);

	v = readl(f->pdc + PDC_CTRL);
	writel(v | PDC_CTRL_ENABLE, f->pdc + PDC_CTRL);

	f->par_set = true;
	return 0;
}

static int ctr_fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			    unsigned blue, unsigned transp, struct fb_info *info)
{
	return 1;	/* truecolour, no palette */
}

static int ctr_fb_blank(int blank, struct fb_info *info)
{
	return 0;
}

static int ctr_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct ctr_fb *f = info->par;
	unsigned long off = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (off >= f->smem_len || size > f->smem_len - off)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       (f->fb_phys + off) >> PAGE_SHIFT, size,
			       vma->vm_page_prot);
}

static const struct fb_ops ctr_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= ctr_fb_check_var,
	.fb_set_par	= ctr_fb_set_par,
	.fb_setcolreg	= ctr_fb_setcolreg,
	.fb_pan_display	= ctr_fb_pan_display,
	.fb_blank	= ctr_fb_blank,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_mmap	= ctr_fb_mmap,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* kernel liveness marker (top screen, timer context)                 */
/* ------------------------------------------------------------------ */
/*
 * The framework freezes hard ~2 s after the first app process forks, with no
 * hung-task report, no softlockup and no lockdep splat - so it is either an
 * IRQ-off spin (timers stop) or a long non-preemptible kernel loop / stall
 * (timers keep firing).  Those look identical when all you have is a
 * userspace heartbeat, which can itself be starved.
 *
 * This timer paints one 24x24 square in the top-left corner of the TOP screen
 * (registered_fb[0], the simple-framebuffer console), alternating green/red
 * every 500 ms.  It runs from the timer softirq, so a square that keeps
 * changing after everything else stops proves the kernel's timers are alive;
 * a square frozen mid-colour means IRQs really are off.  It touches no SD, no
 * printk and no console_lock, and the bottom screen is left alone so this can
 * never be confused with Android's output.
 */
static struct timer_list ctr_hb_timer;
static bool ctr_hb_state;

static void ctr_wd_sym(struct task_struct *p, unsigned long addr, char *buf, int n);

/*
 * Last-state capture: every 500 ms record which task the timer interrupted and
 * what code it was executing.  The ctr-diag thread writes the latest value to
 * the SD, so when the machine hard-freezes (the timer square stops, so nothing
 * can run afterwards) the LAST line on the card still names the task and the
 * kernel function that were running at the moment of the stop.
 */
static char ctr_last_state[128];

/* compact last watchdog report (task/pid + pc symbol), published into the
 * ARM9 page so a kernel-mode stall survives even a wedged SD log path */
static char ctr_wd_state[40];

static void ctr_hb_capture(void)
{
	struct task_struct *p = current;
	struct pt_regs *regs = task_pt_regs(p);
	unsigned long *q, *top;
	unsigned long kpc = 0, klr = 0;
	char sym[64];

	/*
	 * If the task was in userspace when the timer IRQ hit, the kernel stack
	 * was empty and the IRQ entry pushed its pt_regs at the very top, i.e.
	 * task_pt_regs(p) *is* the interrupted frame.  Its PC is the real
	 * userspace PC.  (When the task was in the kernel, task_pt_regs() still
	 * points at the older syscall frame, so the scan below is needed.)
	 *
	 * This is the deterministic answer the old code was missing: it scanned
	 * the stack for a mode-0x13 frame and happily matched stale exception
	 * frames, so userspace spins were reported as `kern __loop_delay`, and
	 * the later attempt to also match mode-0x10 words picked up garbage
	 * (`user 00000004`).
	 */
	if (user_mode(regs)) {
		unsigned long upc = instruction_pointer(regs);
		unsigned long ulr = regs->ARM_lr;
		char csym[64];

		ctr_wd_sym(p, upc, sym, sizeof(sym));
		ctr_wd_sym(p, ulr, csym, sizeof(csym));
		scnprintf(ctr_last_state, sizeof(ctr_last_state),
			  "%s/%d user pc=%08lx lr=%08lx %s from %s",
			  p->comm, p->pid, upc, ulr, sym, csym);
		return;
	}

	/*
	 * Kernel mode: find the *most recent* SVC exception frame walking up the
	 * stack from the current SP (the first plausible pt_regs).
	 */
	q = (unsigned long *)current_stack_pointer;
	top = (unsigned long *)(((unsigned long)q & ~(THREAD_SIZE - 1)) +
				THREAD_SIZE - sizeof(unsigned long));
	for (; q + 17 < top; q++) {
		if ((q[16] & 0x1f) == 0x13 &&
		    q[15] >= 0xc0008000UL && q[15] < 0xc0700000UL) {
			kpc = q[15]; klr = q[14];
			break;
		}
	}

	if (kpc) {
		ctr_wd_sym(p, kpc, sym, sizeof(sym));
		/* A task parked in __loop_delay is in a udelay() busy-wait.  q[14]
		 * is the return address of whatever called udelay() (the generic
		 * unwinder cannot see it; __loop_delay saves nothing). */
		if (strstr(sym, "delay")) {
			char csym[64];

			ctr_wd_sym(p, klr, csym, sizeof(csym));
			scnprintf(ctr_last_state, sizeof(ctr_last_state),
				  "%s/%d kern %s from %s",
				  p->comm, p->pid, sym, csym);
		} else {
			scnprintf(ctr_last_state, sizeof(ctr_last_state),
				  "%s/%d kern %s", p->comm, p->pid, sym);
		}
	} else {
		ctr_wd_sym(p, instruction_pointer(regs), sym, sizeof(sym));
		scnprintf(ctr_last_state, sizeof(ctr_last_state),
			  "%s/%d kern? %s", p->comm, p->pid, sym);
	}
}

static void ctr_hb_putpx(struct fb_info *info, int x, int y, u32 v)
{
	u8 *base = (u8 *)info->screen_base;
	unsigned int line = info->fix.line_length;
	int bytes = info->var.bits_per_pixel >> 3;
	u8 *p;

	if (!base || x < 0 || y < 0 ||
	    x >= (int)info->var.xres || y >= (int)info->var.yres)
		return;
	p = base + (size_t)y * line + (size_t)x * bytes;
	switch (bytes) {
	case 2: *(u16 *)p = (u16)v; break;
	case 3: p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; break;
	case 4: *(u32 *)p = v; break;
	}
}

static u32 ctr_hb_colour(struct fb_info *info, u8 r, u8 g, u8 b)
{
	u32 v = 0;

	if (info->var.red.length)
		v |= ((r >> (8 - info->var.red.length)) &
		      ((1u << info->var.red.length) - 1)) << info->var.red.offset;
	if (info->var.green.length)
		v |= ((g >> (8 - info->var.green.length)) &
		      ((1u << info->var.green.length) - 1)) << info->var.green.offset;
	if (info->var.blue.length)
		v |= ((b >> (8 - info->var.blue.length)) &
		      ((1u << info->var.blue.length) - 1)) << info->var.blue.offset;
	return v;
}

static void ctr_cap_timer_tick(void);
static void ctr_cap_alog_publish(void);

static void ctr_hb_tick(struct timer_list *t)
{
	struct fb_info *info = registered_fb[0];

	ctr_hb_capture();
	ctr_cap_timer_tick();

	if (info && info->screen_base && info->var.bits_per_pixel) {
		u32 col = ctr_hb_state ? ctr_hb_colour(info, 0x00, 0xff, 0x00)
				       : ctr_hb_colour(info, 0xff, 0x00, 0x00);
		int x, y;

		for (y = 0; y < 24; y++)
			for (x = 0; x < 24; x++)
				ctr_hb_putpx(info, x, y, col);
	}
	ctr_hb_state = !ctr_hb_state;
	mod_timer(&ctr_hb_timer, jiffies + msecs_to_jiffies(500));
}

/*
 * Stall watchdog.
 *
 * On hardware the framework freezes with the top-screen timer square still
 * blinking (so IRQs and the timer softirq run) while every userspace process
 * - including the heartbeat - stops.  That combination means the CPU is not
 * returning to userspace: a spin_lock()/preempt_count hang (IRQs enabled), or
 * a long non-preemptible kernel path.  The hung-task and softlockup detectors
 * are both blind to it (the task is R, and the hrtimer keeps firing).
 *
 * This watchdog runs from the timer softirq with IRQs on, so it executes even
 * while a task spins with preemption disabled.  If the interrupted task stays
 * the same and stayed in *kernel* mode for three consecutive ticks, it is
 * stuck in the kernel: dump its state and kernel stack to the console and the
 * kernel log.  (In softirq context `current` is exactly the task that was
 * interrupted, i.e. the spinner.)
 */
static struct timer_list ctr_wd_timer;
static char ctr_wd_last_comm[TASK_COMM_LEN];
static pid_t ctr_wd_last_pid;
static int ctr_wd_same;
static bool ctr_wd_dumped;
static int ctr_wd_dump_count;

/* Draw one line of text straight into a framebuffer, using the in-kernel
 * VGA 8x8 font.  This is what makes the watchdog's answer visible even when
 * the console is frozen (which is itself the symptom: the spinner usually
 * holds console_lock, so printk's output never reaches the panel). */
static void ctr_wd_text(struct fb_info *info, int x, int y, const char *s)
{
	const struct font_desc *f = &font_vga_8x8;
	u32 fg = ctr_hb_colour(info, 0xff, 0xff, 0x00);
	u32 bg = ctr_hb_colour(info, 0x00, 0x00, 0x00);
	int i, row, col;

	for (i = 0; s[i]; i++) {
		unsigned char ch = (unsigned char)s[i];
		const unsigned char *g;

		if (ch < 32 || ch > 127)
			ch = '.';
		g = (const unsigned char *)f->data + (unsigned int)ch * f->height;
		for (row = 0; row < (int)f->height; row++)
			for (col = 0; col < (int)f->width; col++)
				ctr_hb_putpx(info, x + i * f->width + col,
					     y + row,
					     (g[row] & (0x80 >> col)) ? fg : bg);
	}
}

/* Resolve an address the way /proc/<pid>/maps would: kernel addresses via
 * kallsyms, user addresses against the task's VMAs (library name + offset).
 * Called only from the one-shot watchdog dump, so the unlocked find_vma() read
 * is an acceptable trade against not being able to identify the spin at all. */
static void ctr_wd_sym(struct task_struct *p, unsigned long addr, char *buf, int n)
{
	if (addr >= TASK_SIZE) {
		sprint_symbol(buf, addr);
		return;
	}
	if (p->mm) {
		struct vm_area_struct *vma = find_vma(p->mm, addr);

		/* find_vma() can return a VMA that starts *above* addr when
		 * addr sits in a gap (the mmap_lock is not held here).  Do not
		 * subtract in that case - it wraps and prints nonsense like
		 * "app_process+0xffff8004". */
		if (vma && addr >= vma->vm_start && addr < vma->vm_end) {
			if (vma->vm_file) {
				snprintf(buf, n, "%s+0x%lx",
					 vma->vm_file->f_path.dentry->d_name.name,
					 addr - vma->vm_start);
				return;
			}
			snprintf(buf, n, "[anon]+0x%lx", addr - vma->vm_start);
			return;
		}
	}
	snprintf(buf, n, "[%08lx]", addr);
}

/*
 * Diagnostic sink that does not need userspace.
 *
 * The stall watchdog runs in softirq context, where the FAT filesystem cannot
 * be touched (it may sleep).  Transcribing the top-screen dump by hand is not
 * an option, so the watchdog leaves a text message here and a small kthread
 * writes it to the SD card.  The kthread can sleep, so it uses the ordinary
 * file API; with CONFIG_PREEMPT the timer interrupt preempts the runaway
 * udelay() loop and the kthread keeps running, which is exactly what makes
 * this work when the machine looks frozen.
 */
static DEFINE_SPINLOCK(ctr_diag_lock);
static char ctr_diag_msg[1400];
static bool ctr_diag_ready;
static struct task_struct *ctr_diag_task;
static DECLARE_WAIT_QUEUE_HEAD(ctr_diag_wq);

static void ctr_diag_write_file(const char *text, size_t len)
{
	struct file *f;

	/*
	 * Write to tmpfs first.  The ARM9/SD write path has been seen to wedge
	 * under load and block every SD writer at once; the ctr-diag kthread
	 * must not be another victim (a blocked vfs_fsync on the FAT can also
	 * hold locks the framework's statfs/mount paths wait on).  The initramfs
	 * flusher mirrors /mnt/probe to the card.
	 */
	f = filp_open("/mnt/probe/ctr-diag.txt",
		      O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (IS_ERR(f))
		f = filp_open("/mnt/sd/CYANO3DS/ctr-diag.txt",
			      O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (IS_ERR(f))
		return;
	kernel_write(f, text, len, &f->f_pos);
	filp_close(f, NULL);
}

/*
 * ARM9 black-box capture (option B) -----------------------------------------
 *
 * The 15 s tmpfs->SD flusher (and every other Linux SD writer) wedges on the
 * ARM11/PXI/MMC write path under framework load.  Once it does, every card log
 * stops at once and everything after that is trapped in tmpfs and lost when the
 * console is powered off, so we cannot see whether Android reaches the launcher
 * or composites a frame.
 *
 * The ARM9 is an independent CPU and owns the SD controller.  This driver keeps
 * the latest ctr-diag text in a 4 KiB page of DRAM and hands the ARM9 the
 * page's physical address once, over an otherwise-unused PXI manager register.
 * The ARM9 then polls the page and mirrors it to a rotating set of sectors in
 * the preallocated /mnt/sd/CYANO3DS/arm9cap.bin - with no Linux block layer
 * involved, so a wedged Linux SD writer can no longer blind us.
 *
 * The struct layout must match port/arm9linuxfw/capture.h exactly.
 */
#define ARM9CAP_MAGIC		0x50433941u	/* "A9CP" */
#define ARM9CAP_VERSION		1u
#define ARM9CAP_SIZE		4096u
#define ARM9CAP_TEXT_OFF	0x100u
#define ARM9CAP_SLOTS		32u
#define ARM9CAP_MGR_REG		0x20u

/* the text[] region now carries only the Android log tail (see below); the
 * kernel state is published by the timer in reserved[] */
#define ARM9CAP_DIAG_LEN	0u
#define ARM9CAP_ALOG_LEN	((u32)sizeof(((struct arm9cap *)0)->text) - \
				 ARM9CAP_DIAG_LEN)

struct arm9cap {
	u32 magic;
	u32 version;
	u32 seq;
	u32 heartbeat;
	u32 sector;
	u32 slots;
	u32 text_len;
	u32 text_off;
	u32 a9_seq;
	u32 a9_slot;
	u32 a9_ticks_lo;
	u32 a9_ticks_hi;
	u32 a9_last_addr;
	u32 a9_status;
	u32 reserved[50];
	char text[ARM9CAP_SIZE - ARM9CAP_TEXT_OFF];
};

extern int ctr_pxi_mgr_write(u32 reg, u32 val);

static struct arm9cap *ctr_cap_page;
static u32 ctr_cap_phys;
static bool ctr_cap_file_ready;
static u32 ctr_cap_text_len;
static u32 ctr_cap_seq;
static unsigned int ctr_cap_attempts;
static bool ctr_cap_handshake_done;

static void ctr_cap_handshake(void)
{
	/*
	 * The ARM9 only needs the page address once.  Do not keep poking the PXI
	 * manager register on every update: pxi_txrx() sleeps on the FIFO mutex,
	 * and once the PXI/SD path wedges that call blocks - which would freeze the
	 * whole ctr-diag kthread, exactly the blindness this mechanism exists to
	 * remove.  One success (or a few failed tries) and we are done.
	 */
	if (!ctr_cap_phys || ctr_cap_handshake_done)
		return;
	if (ctr_pxi_mgr_write(ARM9CAP_MGR_REG, ctr_cap_phys) == 0)
		ctr_cap_handshake_done = true;
	else if (++ctr_cap_attempts >= 10)
		ctr_cap_handshake_done = true;
}

static void ctr_cap_timer_tick(void)
{
	char *ls;
	char *ws;
	u32 ls_len = 128;   /* ctr_last_state[128] -> reserved[8..39] */
	u32 ws_off = 40 * (u32)sizeof(u32);
	u32 ws_len = (u32)sizeof(((struct arm9cap *)0)->reserved) - ws_off;

	if (!ctr_cap_page)
		return;
	/*
	 * Liveness + state from the timer softirq, independent of the ctr-diag
	 * kthread.  If the timer keeps advancing across snapshots while arm11_seq
	 * is frozen, the kernel (and its IRQs) is alive and only a task is stuck;
	 * if it is frozen too, the machine is in an IRQ-off stall.  reserved[3]
	 * is the exact flip count, [4..6] the pan-display counters, [8..39] the
	 * last interrupted task (with the udelay caller if it is in one) and
	 * [40..] the last kernel-mode watchdog report.
	 */
	ctr_cap_page->reserved[2]++;
	ctr_cap_page->reserved[3] = (u32)ctr_flip_count;
	ctr_cap_page->reserved[4] = ctr_pan_calls;
	ctr_cap_page->reserved[5] = ctr_pan_early;
	ctr_cap_page->reserved[6] = ctr_wait_timeouts;
	ls = (char *)&ctr_cap_page->reserved[8];
	strscpy(ls, ctr_last_state, ls_len);
	ws = (char *)&ctr_cap_page->reserved[40];
	strscpy(ws, ctr_wd_state, ws_len);
	ctr_cap_alog_publish();
	dmac_flush_range(&ctr_cap_page->reserved[1], ws + ws_len);
}

static void ctr_cap_setup_file(void)
{
	const u32 total = ARM9CAP_SLOTS * (ARM9CAP_SIZE / 512u);
	struct file *f;
	struct inode *inode;
	char *zeros;
	loff_t pos;
	u32 first = 0, run = 0, i;

	if (ctr_cap_file_ready || !ctr_cap_page)
		return;

	f = filp_open("/mnt/sd/CYANO3DS/arm9cap.bin",
		      O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(f)) {
		f = filp_open("/mnt/sd/arm9cap.bin",
			      O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (IS_ERR(f))
			return;		/* /mnt/sd not up yet - retry next time */
	}

	zeros = kzalloc(ARM9CAP_SIZE, GFP_KERNEL);
	if (!zeros) {
		filp_close(f, NULL);
		return;
	}
	pos = 0;
	for (i = 0; i < ARM9CAP_SLOTS; i++)
		kernel_write(f, zeros, ARM9CAP_SIZE, &pos);
	kfree(zeros);

	/* let the data reach the card and the pages go clean: after this the
	 * kernel never writes the file again, so the ARM9's raw writes stick. */
	vfs_fsync(f, 0);

	inode = file_inode(f);

	/* largest contiguous run of 512-byte blocks from the start of the file */
	for (i = 0; i < total; i++) {
		sector_t b = i;

		if (bmap(inode, &b) || !b)
			break;
		if (i == 0) {
			first = (u32)b;
			run = 1;
		} else if (b == (sector_t)(first + run)) {
			run++;
		} else {
			break;
		}
	}

	ctr_cap_page->magic = ARM9CAP_MAGIC;
	ctr_cap_page->version = ARM9CAP_VERSION;
	ctr_cap_page->sector = first;
	ctr_cap_page->slots = run / (ARM9CAP_SIZE / 512u);
	ctr_cap_page->text_len = 0;
	ctr_cap_page->text_off = 0;
	ctr_cap_page->heartbeat = 0;
	ctr_cap_page->seq = 0;
	ctr_cap_text_len = 0;
	ctr_cap_seq = 0;
	ctr_cap_page->reserved[0] = 0;
	ctr_cap_page->reserved[1] = 0;
	dmac_flush_range((void *)ctr_cap_page,
			 (void *)((char *)ctr_cap_page + ARM9CAP_SIZE));

	pr_info("ctr-cap: arm9cap.bin lba=%u run=%u sectors slots=%u\n",
		first, run, ctr_cap_page->slots);

	filp_close(f, NULL);
	ctr_cap_file_ready = true;
}

/* keep the last `size` bytes in a plain linear buffer (base[0..*lenp)) */
static void ctr_cap_ring(char *base, u32 size, u32 *lenp, const char *s,
			 size_t len)
{
	u32 have = *lenp;

	if (len >= size) {
		memcpy(base, s + len - size, size);
		*lenp = size;
		return;
	}
	if (have + len > size) {
		u32 drop = have + len - size;

		memmove(base, base + drop, have - drop);
		have -= drop;
	}
	memcpy(base + have, s, len);
	*lenp = have + len;
}

/*
 * Android log mirror, straight from the kernel logger driver.
 *
 * The tmpfs logdump file + ctr-diag kthread used to feed this page, but that
 * kthread can block when the SD/tmpfs path wedges - exactly when we need the
 * log most.  The logger driver calls ctr_cap_log_append() in the writer's own
 * context, so every Android line is captured independently of tmpfs, the
 * flusher and the kthread; the timer publishes the ring into the page.
 */
static DEFINE_SPINLOCK(ctr_log_lock);
static char ctr_log_buf[8192];
static u32 ctr_log_head;
static u32 ctr_log_total;

void ctr_cap_log_append(const char *payload, size_t len)
{
	unsigned long flags;
	const char *tag, *text;
	size_t tlen, xlen, i;
	char line[256];
	u32 n = 0;

	if (len < 2)
		return;
	/* payload is [priority][tag NUL][text], the classic 2009 ABI */
	tag = payload + 1;
	text = memchr(tag, '\0', len - 1);
	if (!text)
		return;
	tlen = min((size_t)(text - tag), (size_t)60);
	text++;
	xlen = min((size_t)(len - (text - payload)), (size_t)180);

	if (tlen) {
		memcpy(line, tag, tlen);
		n = tlen;
		line[n++] = ':';
		line[n++] = ' ';
	}
	memcpy(line + n, text, xlen);
	n += xlen;
	line[n++] = '\n';

	spin_lock_irqsave(&ctr_log_lock, flags);
	for (i = 0; i < n; i++) {
		ctr_log_buf[ctr_log_head] = line[i];
		ctr_log_head = (ctr_log_head + 1) % sizeof(ctr_log_buf);
	}
	ctr_log_total += n;
	spin_unlock_irqrestore(&ctr_log_lock, flags);
}

static void ctr_cap_alog_publish(void)
{
	unsigned long flags;
	u32 avail, len, start, i;
	char *dst;

	if (!ctr_cap_page)
		return;
	spin_lock_irqsave(&ctr_log_lock, flags);
	avail = min(ctr_log_total, (u32)sizeof(ctr_log_buf));
	len = min(avail, ARM9CAP_ALOG_LEN);
	start = (ctr_log_head + sizeof(ctr_log_buf) - len) %
		sizeof(ctr_log_buf);
	dst = ctr_cap_page->text + ARM9CAP_DIAG_LEN;
	for (i = 0; i < len; i++)
		dst[i] = ctr_log_buf[(start + i) % sizeof(ctr_log_buf)];
	spin_unlock_irqrestore(&ctr_log_lock, flags);
	ctr_cap_page->reserved[1] = len;
	dmac_flush_range(dst, dst + len);
}

static void ctr_cap_append(const char *text, size_t len)
{
	if (!ctr_cap_page)
		return;

	if (!ctr_cap_file_ready) {
		ctr_cap_setup_file();
		if (!ctr_cap_file_ready)
			return;
	}

	ctr_cap_handshake();

	if (len > ARM9CAP_DIAG_LEN)
		len = ARM9CAP_DIAG_LEN;

	/* seqlock: odd = update in progress, even = consistent */
	ctr_cap_page->seq = ++ctr_cap_seq;
	ctr_cap_ring(ctr_cap_page->text, ARM9CAP_DIAG_LEN, &ctr_cap_text_len,
		     text, len);
	ctr_cap_page->text_len = ctr_cap_text_len;
	ctr_cap_page->text_off = 0;
	ctr_cap_page->heartbeat++;

	/*
	 * Flush the body (with the odd seq) first and the final, even sequence word
	 * last: the ARM9 does not share the ARM11's data cache, so it reads a
	 * consistent snapshot - odd seq means "busy", and a seq it saw before the
	 * copy that is unchanged after it means "consistent".
	 */
	dmac_flush_range((void *)ctr_cap_page,
			 (void *)((char *)ctr_cap_page + ARM9CAP_SIZE));
	ctr_cap_page->seq = ++ctr_cap_seq;
	dmac_flush_range((void *)&ctr_cap_page->seq,
			 (void *)((char *)&ctr_cap_page->seq + sizeof(u32)));
}

static int ctr_diag_thread(void *unused)
{
	char *local = kmalloc(sizeof(ctr_diag_msg), GFP_KERNEL);

	if (!local)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(ctr_diag_wq, ctr_diag_ready,
						 msecs_to_jiffies(2000));
		if (kthread_should_stop())
			break;
		if (ctr_diag_ready) {
			spin_lock_irq(&ctr_diag_lock);
			memcpy(local, ctr_diag_msg, sizeof(ctr_diag_msg));
			ctr_diag_ready = false;
			spin_unlock_irq(&ctr_diag_lock);
		} else {
			/* periodic heartbeat: proves the sink works, and (via
			 * last_state) names what the CPU was doing */
			snprintf(local, sizeof(ctr_diag_msg),
				 "ctr-diag: alive uptime=%us flips=%lu last=%s\n",
				 jiffies / HZ, ctr_flip_count, ctr_last_state);
		}
		ctr_diag_write_file(local, strlen(local));
		ctr_cap_append(local, strlen(local));
	}
	kfree(local);
	return 0;
}

static void ctr_wd_dump(struct task_struct *p, bool user)
{
	struct fb_info *info = registered_fb[0];
	struct pt_regs *regs = task_pt_regs(p);
	unsigned long pc = instruction_pointer(regs);
	unsigned long flags;
	char buf[80], sym[64];
	char msg[1400];
	int off = 0;
	struct stack_trace trace;
	unsigned long entries[24];

#define DIAG(...) do { \
	if (off < (int)sizeof(msg) - 1) \
		off += scnprintf(msg + off, sizeof(msg) - off, __VA_ARGS__); \
} while (0)

	DIAG("ctr-diag: CPU stall at uptime=%us flips=%lu last_comm=%s\n",
	     jiffies / HZ, ctr_flip_count,
	     ctr_last_flip_comm[0] ? ctr_last_flip_comm : "-");
	DIAG("  task %s pid %d user %d state %ld\n",
	     p->comm, p->pid, user, (long)p->state);
	DIAG("  syscall r7=%08lx (arm priv 0xf0000+x, eabi see unistd)\n",
	     (unsigned long)regs->ARM_r7);

	ctr_wd_sym(p, pc, sym, sizeof(sym));
	DIAG("  pc %08lx %s\n", pc, sym);
	scnprintf(ctr_wd_state, sizeof(ctr_wd_state), "%s/%d %s",
		  p->comm, p->pid, sym);
	ctr_wd_sym(p, regs->ARM_lr, sym, sizeof(sym));
	DIAG("  lr %08lx %s\n", regs->ARM_lr, sym);
	DIAG("  sp %08lx\n", regs->ARM_sp);

	pr_emerg("ctr-wd: %s/%d user=%d stuck; pc=%08lx lr=%08lx sp=%08lx\n",
		 p->comm, p->pid, user, pc, regs->ARM_lr, regs->ARM_sp);
	sched_show_task(p);

	if (info && info->screen_base) {
		ctr_wd_text(info, 2, 30, "ctr-watchdog: CPU stuck");
		snprintf(buf, sizeof(buf), "task %s pid %d user %d",
			 p->comm, p->pid, user);
		ctr_wd_text(info, 2, 42, buf);
		snprintf(buf, sizeof(buf), "state %ld", (long)p->state);
		ctr_wd_text(info, 2, 54, buf);
		ctr_wd_sym(p, pc, sym, sizeof(sym));
		ctr_wd_text(info, 2, 66, sym);
		ctr_wd_sym(p, regs->ARM_lr, sym, sizeof(sym));
		ctr_wd_text(info, 2, 78, sym);
	}

	/* __loop_delay is a leaf that saves nothing, so the unwinder cannot see
	 * who called it.  The IRQ's saved pt_regs *can*: walking up the stack
	 * from here, the first frame whose saved CPSR is SVC mode (0x13) with a
	 * kernel-text PC is the timer IRQ's frame, and its saved LR is the
	 * return address of the function that was looping - i.e. the udelay()
	 * caller. */
	trace.nr_entries = 0;
	trace.max_entries = ARRAY_SIZE(entries);
	trace.entries = entries;
	trace.skip = 0;
	save_stack_trace_tsk(p, &trace);
	{
		unsigned long *q = (unsigned long *)current_stack_pointer;
		unsigned long *top = (unsigned long *)
			(((unsigned long)q & ~(THREAD_SIZE - 1)) + THREAD_SIZE -
			 sizeof(unsigned long));
		int line = 0;

		for (; q + 17 < top; q++) {
			if ((q[16] & 0x1f) != 0x13 ||
			    q[15] < 0xc0008000UL || q[15] >= 0xc0700000UL)
				continue;
			ctr_wd_sym(p, q[15], sym, sizeof(sym));
			DIAG("  irq pc %08lx %s\n", q[15], sym);
			if (info && info->screen_base)
				ctr_wd_text(info, 2, 96, sym);
			ctr_wd_sym(p, q[14], sym, sizeof(sym));
			DIAG("  irq lr %08lx %s   <-- udelay() caller\n",
			     q[14], sym);
			if (info && info->screen_base)
				ctr_wd_text(info, 2, 108, sym);
			line = 2;

			/* Walk the *interrupted* stack from its own SP: this is
			 * the real caller chain of the spinning function. */
			{
				unsigned long *s = (unsigned long *)q[13];
				/*
				 * q[13] comes from a *heuristic* frame match (any stack
				 * word that looks like an SVC-mode frame), so it is NOT
				 * guaranteed to be a real stack pointer.  Dereferencing a
				 * bad address here faults in softirq context and panics
				 * the machine as "Fatal exception in interrupt" - which
				 * is exactly what happened on hardware.  Only walk it
				 * when it really lies inside this kernel stack.
				 */
				unsigned long sbase =
					(unsigned long)top & ~(THREAD_SIZE - 1UL);
				unsigned long prevv = 0;
				int j;

				DIAG("  interrupted stack (sp=%08lx):\n",
				     (unsigned long)q[13]);
				if ((unsigned long)s < sbase ||
				    (unsigned long)s >= (unsigned long)top ||
				    ((unsigned long)s & 3)) {
					DIAG("  (sp outside the kernel stack; skipped)\n");
					s = (unsigned long *)top;
				}
				for (j = 0; s < top && j < 14; s++) {
					unsigned long v = *s;

					if (v < 0xc0008000UL ||
					    v >= 0xc0700000UL || v == prevv)
						continue;
					ctr_wd_sym(p, v, sym, sizeof(sym));
					if (!strchr(sym, '+'))
						continue;
					prevv = v;
					DIAG("    [<%08lx>] %s\n", v, sym);
					if (info && info->screen_base) {
						ctr_wd_text(info, 2,
							    96 + line * 12, sym);
						if (++line >= 22)
							break;
					}
					j++;
				}
			}
			break;
		}

		/* Whatever remains of the stack, in case the frame above is not
		 * what we think it is. */
		q = (unsigned long *)current_stack_pointer;
		DIAG("  raw stack (kernel text only):\n");
		{
			unsigned long prev = 0;
			int i;

			for (i = 0; q < top && i < 20; q++) {
				unsigned long v = *q;

				if (v < 0xc0008000UL || v >= 0xc0700000UL ||
				    v == prev)
					continue;
				ctr_wd_sym(p, v, sym, sizeof(sym));
				if (!strchr(sym, '+'))
					continue;
				prev = v;
				DIAG("    [<%08lx>] %s\n", v, sym);
				if (info && info->screen_base) {
					ctr_wd_text(info, 2, 96 + line * 12, sym);
					if (++line >= 22)
						break;
				}
				i++;
			}
		}
	}
	DIAG("ctr-diag: end\n");
	DIAG(" __irq_svc vs __irq_usr tells kernel-mode from user-mode spin\n");

	/* hand the whole thing to the kthread that writes it to the SD */
	spin_lock_irqsave(&ctr_diag_lock, flags);
	memcpy(ctr_diag_msg, msg, off + 1);
	ctr_diag_ready = true;
	spin_unlock_irqrestore(&ctr_diag_lock, flags);
	wake_up_interruptible(&ctr_diag_wq);

#undef DIAG
}

static void ctr_wd_tick(struct timer_list *t)
{
	struct task_struct *p = current;
	bool user = user_mode(task_pt_regs(p));

	/* Don't arm before the boot has settled: some one-off early kernel
	 * work (unpacking, register_framebuffer's testcard blit) can hold a
	 * CPU for seconds and would be a false positive. */
	if (jiffies < msecs_to_jiffies(30000)) {
		mod_timer(&ctr_wd_timer, jiffies + msecs_to_jiffies(1000));
		return;
	}

	if (!is_idle_task(p) && p->pid == ctr_wd_last_pid &&
	    strcmp(p->comm, ctr_wd_last_comm) == 0) {
		ctr_wd_same++;
	} else {
		ctr_wd_same = 0;
		/* a new task is on the CPU: allow a later, real stall to dump */
		ctr_wd_dumped = false;
	}

	ctr_wd_last_pid = p->pid;
	strscpy(ctr_wd_last_comm, p->comm, TASK_COMM_LEN);

	/* Same task on the CPU for three seconds = it is monopolising it.
	 * Report both kernel- and user-mode spins, but bound the total number of
	 * dumps: each one (sched_show_task + fbcon + top-screen writes) is
	 * expensive on this single slow CPU.  The user-mode dump is the one that
	 * names a userspace spin - sched_show_task prints task_pt_regs(), whose
	 * PC/LR are the real userspace PC and caller. */
	if (ctr_wd_same >= 3 && !ctr_wd_dumped && ctr_wd_dump_count < 8) {
		ctr_wd_dumped = true;
		ctr_wd_dump_count++;
		ctr_wd_dump(p, user);
	}

	mod_timer(&ctr_wd_timer, jiffies + msecs_to_jiffies(1000));
}

static int ctr_fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fb_info *info;
	struct ctr_fb *f;
	struct resource *vram_res;
	u32 hdisp, fmt, stride, vtot, ctrl, sel, fba0, fba1;
	u32 start, end;
	int want_bpp = 0;
	int ret;

	info = framebuffer_alloc(sizeof(struct ctr_fb), dev);
	if (!info)
		return -ENOMEM;

	f = info->par;
	f->info = info;
	platform_set_drvdata(pdev, info);

	f->pdc = devm_platform_ioremap_resource_byname(pdev, "pdc");
	if (IS_ERR(f->pdc)) {
		ret = PTR_ERR(f->pdc);
		goto err_free;
	}

	vram_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vram");
	if (!vram_res) {
		ret = -ENODEV;
		goto err_free;
	}
	f->vram_phys = vram_res->start;
	f->vram_size = resource_size(vram_res);
	f->vram = devm_ioremap_wc(dev, vram_res->start, f->vram_size);
	if (!f->vram) {
		ret = -ENOMEM;
		goto err_free;
	}

	/* --- 1. ask the hardware what it is already doing --- */
	hdisp  = readl(f->pdc + PDC_HDISP);
	fmt    = readl(f->pdc + PDC_FORMAT);
	stride = readl(f->pdc + PDC_STRIDE);
	vtot   = readl(f->pdc + PDC_VTOTAL);
	ctrl   = readl(f->pdc + PDC_CTRL);
	sel    = readl(f->pdc + PDC_SELECT);
	fba0   = readl(f->pdc + PDC_FB_A0);
	fba1   = readl(f->pdc + PDC_FB_A1);

	start = hdisp & 0xfff;
	end = (hdisp >> 16) & 0xfff;
	f->px_per_line = (end > start) ? end - start : 0;

	dev_info(dev, "PDC1 as left by the bootloader:\n");
	dev_info(dev, "  format=%#x (colour format %u) stride=%u px/line=%u\n",
		 fmt, fmt & 0x7, stride, f->px_per_line);
	dev_info(dev, "  hdisp=%#x vtotal=%#x ctrl=%#x select=%#x fbA=%#x/%#x\n",
		 hdisp, vtot, ctrl, sel, fba0, fba1);

	if (!f->px_per_line)
		f->px_per_line = LCD_PXLINES_PORTRAIT;

	/* --- 2. derive a scanout layout consistent with what we found --- */
	if (of_property_read_u32(dev->of_node, "nintendo,lcd-bpp", &start) == 0)
		want_bpp = (int)start;

	f->transpose = (f->px_per_line == LCD_PXLINES_PORTRAIT);
	f->lcd_w = f->transpose ? LCD_PXLINES_PORTRAIT : LCD_PXLINES_LANDSCAPE;
	f->lcd_h = f->transpose ? LCD_LINES_PORTRAIT : LCD_LINES_LANDSCAPE;

	/* bytes per pixel the bootloader is using */
	if (f->px_per_line && stride % f->px_per_line == 0)
		f->lcd_bpp = stride / f->px_per_line;
	if (f->lcd_bpp != 2 && f->lcd_bpp != 3)
		f->lcd_bpp = 3;

	if (want_bpp && want_bpp != (int)f->lcd_bpp) {
		if (want_bpp != 2 && want_bpp != 3) {
			dev_err(dev, "nintendo,lcd-bpp must be 2 or 3\n");
			ret = -EINVAL;
			goto err_free;
		}
		f->lcd_bpp = want_bpp;
		f->we_set_format = true;
	}
	f->lcd_stride = f->px_per_line * f->lcd_bpp;
	f->scanout_size = (size_t)f->lcd_stride * f->lcd_h;

	if (f->vram_size < 2 * f->scanout_size) {
		dev_err(dev, "vram too small: need %zu, have %zu\n",
			2 * f->scanout_size, f->vram_size);
		ret = -ENOMEM;
		goto err_free;
	}

	/* --- 3. OS-visible geometry: always landscape RGB565 --- */
	f->width = FB_WIDTH;
	f->height = FB_HEIGHT;
	f->pitch = f->width * FB_BPP / 8;
	f->fb_size = (size_t)f->pitch * f->height;
	f->smem_len = f->fb_size * 2;

	f->fb_mem = alloc_pages_exact(f->smem_len, GFP_KERNEL | __GFP_ZERO);
	if (!f->fb_mem) {
		ret = -ENOMEM;
		goto err_free;
	}
	f->fb_phys = virt_to_phys(f->fb_mem);

	info->screen_base = f->fb_mem;
	info->screen_size = f->smem_len;
	info->fbops = &ctr_fb_ops;
	info->flags = 0;
	info->fix.smem_start = f->fb_phys;
	info->fix.smem_len = f->smem_len;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = f->pitch;
	strscpy(info->fix.id, "ctr-lcd", sizeof(info->fix.id));

	info->var.xres = f->width;
	info->var.yres = f->height;
	info->var.xres_virtual = f->width;
	info->var.yres_virtual = f->height * 2;
	info->var.bits_per_pixel = FB_BPP;
	info->var.xoffset = 0;
	info->var.yoffset = 0;
	ctr_fb_check_var(&info->var, info);

	/* --- 4. show the test card through the userspace path ---------------
	 * Deliberately: draw into the OS buffer, then transpose, so this
	 * exercises exactly the code userspace drawing goes through.
	 */
	memset_io(f->vram, 0, f->vram_size);
	ctr_fb_set_par(info);

	f->cur = 0;
	f->lcd_idx = 0;
	writel(0, f->pdc + PDC_SELECT);

	ctr_lcd_testcard(f, (u16 *)f->fb_mem);
	if (f->transpose) {
		ctr_blit_rot(f, (const u16 *)f->fb_mem, f->vram);
		ctr_blit_rot(f, (const u16 *)f->fb_mem,
			     (u8 __iomem *)f->vram + f->scanout_size);
	} else {
		ctr_blit_direct(f, (const u16 *)f->fb_mem, f->vram);
		ctr_blit_direct(f, (const u16 *)f->fb_mem,
				(u8 __iomem *)f->vram + f->scanout_size);
	}
	mb();

	proc_create("ctr_lcd_flips", 0444, NULL, &ctr_flips_fops);

	ret = register_framebuffer(info);
	if (ret) {
		remove_proc_entry("ctr_lcd_flips", NULL);
		dev_err(dev, "register_framebuffer failed: %d\n", ret);
		goto err_free_pages;
	}

	dev_info(dev, "fb%d: %ux%u %dbpp RGB565 -> %ux%u %ubpp scanout, "
		 "stride %u, %s\n",
		 info->node, f->width, f->height, FB_BPP,
		 f->lcd_w, f->lcd_h, f->lcd_bpp * 8, f->lcd_stride,
		 f->transpose ? "transposed (portrait panel)"
			      : "direct (landscape panel)");
	if (f->transpose)
		dev_info(dev, "testcard (through the transposition): expect 4 VERTICAL bands "
			 "red,green,blue,white left->right; corner markers yellow "
			 "top-left, magenta top-right, cyan bottom-left, red "
			 "bottom-right; white staircase top-left; blue triangle "
			 "pointing right\n");
	else
		dev_info(dev, "testcard (direct): same layout, but the panel is scanned "
			 "landscape\n");

	/* kernel-timer liveness marker on the top screen (bring-up only) */
	timer_setup(&ctr_hb_timer, ctr_hb_tick, 0);

	/* stall watchdog: dumps the stuck task's kernel stack (bring-up only) */
	timer_setup(&ctr_wd_timer, ctr_wd_tick, 0);

	if (ctr_debug) {
		mod_timer(&ctr_hb_timer, jiffies + msecs_to_jiffies(500));
		mod_timer(&ctr_wd_timer, jiffies + msecs_to_jiffies(1000));
	}

	/* kernel-side diagnostic sink: writes the watchdog's message to the SD
	 * without needing any userspace process to be runnable */
	ctr_cap_page = alloc_pages_exact(ARM9CAP_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (ctr_cap_page)
		ctr_cap_phys = virt_to_phys(ctr_cap_page);
	else
		dev_warn(dev, "ctr-cap: no memory for the ARM9 capture page\n");

	if (ctr_debug) {
		ctr_diag_task = kthread_run(ctr_diag_thread, NULL, "ctr-diag");
		if (IS_ERR(ctr_diag_task))
			ctr_diag_task = NULL;
	}

	return 0;

err_free_pages:
	free_pages_exact(f->fb_mem, f->smem_len);
err_free:
	framebuffer_release(info);
	return ret;
}

static int ctr_fb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct ctr_fb *f = info->par;

	del_timer_sync(&ctr_hb_timer);
	del_timer_sync(&ctr_wd_timer);
	if (ctr_diag_task)
		kthread_stop(ctr_diag_task);
	if (ctr_cap_page)
		free_pages_exact(ctr_cap_page, ARM9CAP_SIZE);
	remove_proc_entry("ctr_lcd_flips", NULL);
	unregister_framebuffer(info);
	free_pages_exact(f->fb_mem, f->smem_len);
	framebuffer_release(info);
	return 0;
}

static const struct of_device_id ctr_fb_of_match[] = {
	{ .compatible = "nintendo,3ds-lcd" },
	{ }
};
MODULE_DEVICE_TABLE(of, ctr_fb_of_match);

static struct platform_driver ctr_fb_driver = {
	.probe	= ctr_fb_probe,
	.remove	= ctr_fb_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= ctr_fb_of_match,
	},
};
module_platform_driver(ctr_fb_driver);

MODULE_DESCRIPTION("Nintendo 3DS bottom screen LCD framebuffer (Android port)");
MODULE_LICENSE("GPL v2");
