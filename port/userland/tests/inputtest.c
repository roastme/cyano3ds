// SPDX-License-Identifier: GPL-2.0
/*
 * inputtest.c - simple on-screen touchscreen + button test (Nintendo 3DS port)
 *
 * WHY
 * ---
 * Android's input pipeline on this port has been fighting us (the notification
 * shade, focus, the screen-off timer), so "does the hardware work?" and "does
 * Android deliver the events?" got mixed together.  This tool answers the
 * first question on its own, before Android starts, and shows the answer
 * directly on the bottom screen:
 *
 *   * every touch sample moves a crosshair in the touch box and is printed as
 *     raw ABS_X / ABS_Y / ABS_PRESSURE;
 *   * every face button, D-pad direction, L/R, Start/Select lights its box;
 *   * the raw Linux keycode is printed, so a device-tree mapping mistake is
 *     immediately visible.
 *
 * It reads /dev/input/event* directly (so it is independent of Android's
 * EventHub / KeyInputQueue / WindowManager) and draws with the same two-buffer
 * page-flip the rest of the port uses.
 *
 * Usage:
 *   inputtest [fbfile] [timeout-seconds]
 *
 * Hold START for ~1.5 s (or wait for the timeout) to leave the test and let
 * the boot continue.  Every event is also logged to stdout, which /init
 * redirects to /mnt/sd/CYANO3DS/inputtest.log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "font8x8.h"

/* ------------------------------------------------------------------ */
/* framebuffer                                                        */
/* ------------------------------------------------------------------ */
static int fb_fd = -1;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb_mem, *buf0, *buf1, *back;
static int W, H, PITCH, BUFSZ, cur;

/* RGB565 colours (ctr_lcd_fb is always 16bpp RGB565) */
#define C_BG     0x0842	/* dark navy */
#define C_WHITE  0xFFFF
#define C_GREEN  0x07E0
#define C_YELLOW 0xFFE0
#define C_GRAY   0x4208
#define C_DKGRAY 0x2104
#define C_RED    0xF800
#define C_CYAN   0x07FF

static void putpx(unsigned char *b, int x, int y, unsigned short c)
{
	if (x < 0 || y < 0 || x >= W || y >= H)
		return;
	*(unsigned short *)(b + y * PITCH + x * 2) = c;
}

static void fill(unsigned char *b, unsigned short c)
{
	int i, n = (PITCH * H) / 2;
	unsigned short *p = (unsigned short *)b;

	for (i = 0; i < n; i++)
		p[i] = c;
}

static void rect_fill(unsigned char *b, int x, int y, int w, int h, unsigned short c)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			putpx(b, x + i, y + j, c);
}

static void rect_outline(unsigned char *b, int x, int y, int w, int h, unsigned short c)
{
	int i;

	for (i = 0; i < w; i++) {
		putpx(b, x + i, y, c);
		putpx(b, x + i, y + h - 1, c);
	}
	for (i = 0; i < h; i++) {
		putpx(b, x, y + i, c);
		putpx(b, x + w - 1, y + i, c);
	}
}

static void draw_char(unsigned char *b, int cx, int cy, int scale,
		      unsigned char ch, unsigned short colour)
{
	const unsigned char *glyph = font8x8 + (unsigned)ch * 8;
	int row, col, sy, sx;

	if (ch < 0x20 || ch > 0x7e)
		ch = '.';
	glyph = font8x8 + (unsigned)ch * 8;
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

static void draw_text(unsigned char *b, int x, int y, int scale,
		      const char *s, unsigned short colour)
{
	for (; *s; s++, x += 8 * scale)
		draw_char(b, x, y, scale, (unsigned char)*s, colour);
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
/* input sources                                                      */
/* ------------------------------------------------------------------ */
#define MAXSRC 16
struct src {
	int fd;
	int touch;
	char name[64];
};
static struct src srcs[MAXSRC];
static int nsrc;

static int has_bit(const unsigned long *bits, int bit)
{
	return !!(bits[bit / (8 * sizeof(unsigned long))] &
		  (1UL << (bit % (8 * sizeof(unsigned long)))));
}

/* classify + name an input device, then open it non-blocking */
static void open_src(const char *path)
{
	int fd;
	unsigned long absbits[(ABS_MAX + 8 * sizeof(unsigned long)) /
			      (8 * sizeof(unsigned long))];
	unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			      (8 * sizeof(unsigned long))];
	char name[64] = "?";
	struct src *s;

	if (nsrc >= MAXSRC)
		return;
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;
	memset(absbits, 0, sizeof(absbits));
	memset(keybits, 0, sizeof(keybits));
	ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	s = &srcs[nsrc++];
	s->fd = fd;
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->touch = has_bit(absbits, ABS_X) && has_bit(absbits, ABS_Y) &&
		   has_bit(keybits, BTN_TOUCH);
	printf("inputtest: OPEN %s '%s'%s\n", path, name,
	       s->touch ? " (touchscreen)" : "");
	fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* button table and state                                             */
/* ------------------------------------------------------------------ */
#define NBTN 12
static const char *btn_label[NBTN] = {
	"UP", "DOWN", "LEFT", "RIGHT", "A", "B",
	"X", "Y", "L", "R", "START", "SEL"
};
/*
 * The kernel DTS maps the face buttons to Android-friendly keycodes
 * (fix-buttons.py), but list the raw BTN_* codes as aliases too so the test
 * still lights up if it runs against an unpatched device tree.
 */
static const int btn_alias[NBTN][3] = {
	{ KEY_UP, -1, -1 },        { KEY_DOWN, -1, -1 },
	{ KEY_LEFT, -1, -1 },      { KEY_RIGHT, -1, -1 },
	{ 232, BTN_A, -1 },        { 158, BTN_B, -1 },
	{ 217, BTN_X, -1 },        { 15, BTN_Y, -1 },
	{ 115, BTN_TL, -1 },       { 114, BTN_TR, -1 },
	{ 139, BTN_START, -1 },    { 102, BTN_SELECT, -1 }
};
static int btn_down[NBTN];

static int btn_index(int code)
{
	int i, j;

	for (i = 0; i < NBTN; i++)
		for (j = 0; j < 3; j++)
			if (btn_alias[i][j] == code)
				return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */
static int tx, ty, tp;		/* last absolute touch sample   */
static int tdown;		/* BTN_TOUCH                    */
static int tseen;		/* have we ever had a sample?   */
static int last_key = -1;
static int start_since = -1;	/* ms when START went down      */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long t0;

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */
static void draw_ui(long long now, int exit_hint)
{
	char buf[64];
	int i;

	fill(back, C_BG);

	draw_text(back, 4, 2, 1, "3DS INPUT TEST", C_YELLOW);
	snprintf(buf, sizeof(buf), "up=%llds", (now - t0) / 1000);
	draw_text(back, W - 4 - 8 * (int)strlen(buf), 2, 1, buf, C_GRAY);

	/* touch box */
	rect_outline(back, 4, 16, 312, 140, C_WHITE);

	if (tseen) {
		int cx = 6 + tx * 308 / 319;
		int cy = 18 + ty * 136 / 239;
		int r = tdown ? 6 : 3;
		unsigned short col = tdown ? C_GREEN : C_CYAN;
		int k;

		for (k = -r; k <= r; k++) {
			putpx(back, cx + k, cy, col);
			putpx(back, cx, cy + k, col);
		}
	}
	snprintf(buf, sizeof(buf), "X=%3d  Y=%3d  P=%3d  %s",
		 tx, ty, tp, tdown ? "DOWN" : "up  ");
	draw_text(back, 8, 140, 1, buf, tdown ? C_GREEN : C_WHITE);

	/* button boxes: two rows of six */
	for (i = 0; i < NBTN; i++) {
		int col = i % 6, row = i / 6;
		int bx = 4 + col * 52;
		int by = 166 + row * 34;
		int bw = 48, bh = 30;
		unsigned short fillc = btn_down[i] ? C_GREEN : C_DKGRAY;
		unsigned short txtc = btn_down[i] ? 0x0000 : C_WHITE;
		int tw = (int)strlen(btn_label[i]) * 8;

		rect_fill(back, bx, by, bw, bh, fillc);
		rect_outline(back, bx, by, bw, bh, btn_down[i] ? C_WHITE : C_GRAY);
		draw_text(back, bx + (bw - tw) / 2, by + (bh - 8) / 2, 1,
			  btn_label[i], txtc);
	}

	/* last raw keycode, so unmapped/hidden events are not lost */
	if (last_key >= 0) {
		snprintf(buf, sizeof(buf), "last key: %d (0x%x)", last_key, last_key);
		draw_text(back, 4, 232, 1, buf, C_YELLOW);
	} else {
		draw_text(back, 4, 232, 1,
			  exit_hint ? "hold START to continue" : "press buttons / tap the box",
			  C_GRAY);
	}
}

/* ------------------------------------------------------------------ */
/* event handling                                                     */
/* ------------------------------------------------------------------ */
static void handle_event(const struct input_event *ev, long long now)
{
	int i, code = ev->code, val = ev->value;

	if (ev->type == EV_ABS) {
		if (code == ABS_X)
			tx = val;
		else if (code == ABS_Y)
			ty = val;
		else if (code == ABS_PRESSURE)
			tp = val;
		return;
	}
	if (ev->type == EV_KEY) {
		if (code == BTN_TOUCH) {
			if (val && !tdown)
				printf("inputtest: TOUCH down x=%d y=%d p=%d\n",
				       tx, ty, tp);
			else if (!val && tdown)
				printf("inputtest: TOUCH up   x=%d y=%d\n", tx, ty);
			tdown = val;
			tseen = 1;
			fflush(stdout);
			return;
		}
		i = btn_index(code);
		if (i >= 0) {
			if (!!val != !!btn_down[i]) {
				printf("inputtest: %s %s (code %d) t=%lldms\n",
				       btn_label[i], val ? "DOWN" : "up", code,
				       now - t0);
				fflush(stdout);
			}
			btn_down[i] = val;
			if (i == 10) {	/* START: hold ~1.5 s to continue */
				if (val && start_since < 0)
					start_since = (int)now;
				else if (!val)
					start_since = -1;
			}
		} else {
			last_key = code;
			printf("inputtest: KEY code=%d value=%d t=%lldms\n",
			       code, val, now - t0);
			fflush(stdout);
		}
	}
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	const char *fbpath = argc > 1 ? argv[1] : "/dev/fb1";
	int timeout = argc > 2 ? atoi(argv[2]) : 120;
	long long now, last_draw = 0;
	int i;

	fb_fd = open(fbpath, O_RDWR);
	if (fb_fd < 0) {
		fprintf(stderr, "inputtest: open %s: %s\n", fbpath, strerror(errno));
		return 1;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		fprintf(stderr, "inputtest: fb ioctl: %s\n", strerror(errno));
		return 1;
	}
	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "inputtest: %s is %dbpp, need 16 (RGB565)\n",
			fbpath, var.bits_per_pixel);
		return 1;
	}
	W = var.xres;
	H = var.yres;
	PITCH = fix.line_length;
	BUFSZ = PITCH * H;
	fb_mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fprintf(stderr, "inputtest: mmap: %s\n", strerror(errno));
		return 1;
	}
	cur = var.yoffset / H;
	buf0 = fb_mem;
	buf1 = fb_mem + BUFSZ;
	back = cur ? buf0 : buf1;

	for (i = 0; i < MAXSRC; i++) {
		char p[64];

		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		open_src(p);
	}
	if (!nsrc)
		fprintf(stderr, "inputtest: no input devices found\n");

	printf("inputtest: fb=%s %dx%d %dbpp, %d input devices, timeout=%ds\n",
	       fbpath, W, H, var.bits_per_pixel, nsrc, timeout);
	fflush(stdout);

	t0 = now_ms();
	for (;;) {
		fd_set rfds;
		struct timeval tv;
		int maxfd = -1, r;

		now = now_ms();
		if (timeout > 0 && now - t0 > (long long)timeout * 1000)
			break;
		if (start_since >= 0 && now - start_since > 1500) {
			printf("inputtest: START held -> leaving the test\n");
			fflush(stdout);
			break;
		}

		FD_ZERO(&rfds);
		for (i = 0; i < nsrc; i++) {
			FD_SET(srcs[i].fd, &rfds);
			if (srcs[i].fd > maxfd)
				maxfd = srcs[i].fd;
		}
		tv.tv_sec = 0;
		tv.tv_usec = 20000;	/* 50 Hz poll -> live crosshair */
		r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0) {
			for (i = 0; i < nsrc; i++) {
				struct input_event ev;

				if (!FD_ISSET(srcs[i].fd, &rfds))
					continue;
				while (read(srcs[i].fd, &ev, sizeof(ev)) ==
				       (ssize_t)sizeof(ev))
					handle_event(&ev, now);
			}
		}

		/* redraw on every loop; 20 ms is plenty for this UI */
		if (now - last_draw >= 20) {
			draw_ui(now, 1);
			if (flip())
				fprintf(stderr, "inputtest: flip failed: %s\n",
					strerror(errno));
			last_draw = now;
		}
	}

	draw_ui(now_ms(), 0);
	flip();
	printf("inputtest: done (keycodes seen; see the on-screen result)\n");
	fflush(stdout);
	close(fb_fd);
	return 0;
}
