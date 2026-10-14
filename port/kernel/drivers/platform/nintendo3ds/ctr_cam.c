// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr_cam.c - Nintendo 3DS camera capture driver
 *
 *  N3DS_CAM_CAPTURE
 *
 *  This file used to be a bring-up probe that swept registers and walked
 *  control bits in search of a start bit, because the CAM block was believed
 *  to be undocumented (3dbrew's page is a stub). It is not undocumented:
 *  GBATEK documents it under "3DS Video CAM Registers (Camera Input)", and
 *  documents the MT9V113 sensor's init sequence under "DSi Aptina Camera
 *  Initialization". The bring-up history is in git; what follows is the
 *  capture path those two sections describe.
 *
 *  Why every earlier capture came back empty: the sensor's parallel output
 *  was never switched on. The MT9V113 reaches SEQ_STATE 3 (preview) with its
 *  pixel bus still tri-stated; RESET_AND_MISC_CONTROL bit 9 is what drives
 *  it, and nothing here ever set it. And the pixels were never going to show
 *  up in the port registers anyway -- they arrive through a separate FIFO
 *  window at 0x10320000 (port 0) / 0x10321000 (port 1).
 *
 *  Hardware, as used below:
 *
 *    port regs  CAM0 0x10120000 (internal + outer right sensors share it),
 *               CAM1 0x10121000 (outer left sensor)
 *      +000 CAM_CNT   bit4 must be 1, bit15 transfer enable; bits 2, 3 and
 *                     13 must stay 0; bit14 is the CDMA request (unused here)
 *      +004 CAM_STAT  bits 0-10 FIFO fill level, bit15 overrun. After an
 *                     overrun no more data arrives until it is cleared.
 *      +006           bit15 (write, with transfer disabled) clears overrun
 *      +010/+014      trimming window; GBATEK finds it has no effect on 3DS
 *    FIFO       0x80-byte read window. Consecutive reads must come from
 *               increasing addresses (the hardware pops on bit 2 toggling);
 *               a fixed address returns the same word forever. YUYV data.
 *    sensors    MT9V113, i2c bus 0 (DT i2c1): 7-bit 0x3d internal, 0x3c
 *               outer right; i2c bus 1 (DT i2c2): 0x3c outer left.
 *
 *  Capture is PIO. The CDMA (PL330) could do this, but the dmaengine API has
 *  no way to express "read 0x80 bytes incrementing, then rewind the source",
 *  and the FIFO read rule makes a fixed-address transfer useless. So a
 *  SCHED_FIFO kthread drains the FIFO while a stream is running.
 *
 *  Framing. There is no frame-start marker in the data. What there is, is
 *  vertical blanking: between frames the FIFO runs dry for milliseconds,
 *  while inside a frame it never pauses longer than a line. A frame is
 *  therefore "the first width*height*2 bytes after a gap". Anything left
 *  over before the next gap is discarded, a frame cut short by a gap is
 *  dropped, and both are counted -- the measured gap-to-gap size is the one
 *  number that says whether the sensor is sending what we asked for.
 *
 *  Data rate. The FIFO holds ~10 KB, well under a millisecond of a line
 *  burst, so how fast PIO can drain it decides whether frames survive. The
 *  sensor's PLL post-divider (P1) sets the pixel clock; the driver starts at
 *  pll_p1 and, if frames keep overrunning, raises P1 itself and remembers
 *  the value that worked for that resolution class. Every step is logged.
 *
 *  Frame rate (N3DS_CAM_FRAME_RATE). #314 previewed at 0.7-1.6 fps indoors.
 *  It streamed at P1 1, which is half the DSi's pixel clock, and it left
 *  AE_MAX_INDEX at its power-on 0x18. That lets auto-exposure integrate for
 *  24 flicker zones of ~212 rows each, against a 704-row frame, and the
 *  sensor stretches the frame to fit. The measured gap-to-gap period, 1.41 s,
 *  is that stretch. Now P1 is 0 at 320x240 and AE is capped at ae_max_index.
 *
 *  Restart (N3DS_CAM_RECEIVER_RESET). In #314 only the first stream ever
 *  received data. After one STOP, every later START left the sensor saying
 *  SEQ_STATE 3 with its parallel port on, while the receiver sat at
 *  CNT=8013 STAT=0000 forever. The receiver only ever saw a CAM_CNT bit 4
 *  0->1 edge once, at probe, and GBATEK guesses that bit is a reset. So every
 *  START now pulses it. A stream that still gets nothing escalates, one step
 *  every 2 s, and logs which step brought the data back.
 *
 *  Userspace: /dev/ctr_cam. ioctl START {camera, width, height} configures
 *  the sensor and starts streaming; read() returns the newest complete YUYV
 *  frame (blocking, one frame per call); STOP and close() park the sensor.
 *  The only user is mediaserver's CameraHardwareN3ds HAL.
 *
 *  Every early return below prints first. The DSP driver cost a whole
 *  session because its probe could fail in total silence.
 */

#define DRIVER_NAME "3ds-cam"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/ktime.h>

/* ---- userspace ABI; CameraHardwareN3ds.cpp carries a copy ---------------- */

#define CTR_CAM_OUTER_RIGHT	0	/* Android's "back" camera */
#define CTR_CAM_INNER		1	/* self-facing */
#define CTR_CAM_OUTER_LEFT	2
#define CTR_CAM_COUNT		3

struct ctr_cam_config {
	__u32 camera;
	__u32 width;
	__u32 height;
	__u32 flags;		/* reserved, must be 0 */
};

struct ctr_cam_stats {
	__u32 frames;		/* complete frames delivered */
	__u32 short_frames;	/* frames a gap cut short, dropped */
	__u32 overruns;
	__u32 last_gap_bytes;	/* bytes between the last two gaps */
	__u32 max_fifo;		/* highest CAM_STAT fill level seen */
	__u32 p1;		/* sensor PLL post-divider in use */
	__u64 words;		/* FIFO words read, total */
};

#define CTR_CAM_IOC_START	_IOW('c', 0x01, struct ctr_cam_config)
#define CTR_CAM_IOC_STOP	_IO('c', 0x02)
#define CTR_CAM_IOC_STATS	_IOR('c', 0x03, struct ctr_cam_stats)

/* ---- hardware ------------------------------------------------------------ */

/* CFG11_CAMERA_CNT, 0x10141224: bit0 = 16 MHz camera clock. */
#define PDN_CAM_CLOCK_ENABLE	BIT(0)

#define CAM_CNT			0x000
#define  CAM_CNT_MUST_BE_1	BIT(4)
#define  CAM_CNT_XFER		BIT(15)
#define CAM_STAT		0x004
#define  CAM_STAT_FILL		0x07ff
#define  CAM_STAT_OVERRUN	BIT(15)
#define CAM_CTL2		0x006
#define  CAM_CTL2_CLR_OVERRUN	BIT(15)
#define CAM_TRIM_X0		0x010
#define CAM_TRIM_Y0		0x012
#define CAM_TRIM_X1		0x014
#define CAM_TRIM_Y1		0x016

#define CAM_FIFO_WINDOW		0x80

#define CAM_MAX_WIDTH		640
#define CAM_MAX_HEIGHT		480
#define CAM_BUF_BYTES		(CAM_MAX_WIDTH * CAM_MAX_HEIGHT * 2)

/* FIFO empty this long = vertical blanking. A line is ~50-200 us. */
#define CAM_GAP_NS		(3 * NSEC_PER_MSEC)
#define CAM_REPORT_NS		(10ULL * NSEC_PER_SEC)
#define CAM_NODATA_NS		(2ULL * NSEC_PER_SEC)
#define CAM_OVERRUN_SLOWDOWN	3	/* consecutive overruns, no frame between */
#define CAM_P1_MAX		7
#define CAM_RECOVERY_STEPS	3
#define CAM_QVGA_BYTES		(320 * 240 * 2)

#define MT9V113_CHIP_VERSION		0x2280
#define MT9V113_REG_CHIP_VERSION	0x0000
#define MT9V113_REG_STANDBY_CONTROL	0x0018
#define  MT9V113_STANDBY		BIT(0)
#define  MT9V113_STANDBY_DONE		BIT(14)
#define MT9V113_REG_RESET_MISC		0x001a
#define  MT9V113_PARALLEL_ENABLE	BIT(9)
#define MT9V113_REG_CORE_301A		0x301a
#define  MT9V113_CORE_AWAKE		BIT(2)
#define MT9V113_REG_MCU_ADDRESS		0x098c
#define MT9V113_REG_MCU_DATA_0		0x0990
#define MT9V113_VAR_SEQ_CMD		0xa103
#define MT9V113_VAR_SEQ_STATE		0xa104
#define MT9V113_SEQ_CMD_REFRESH		5
#define MT9V113_SEQ_CMD_REFRESH_MODE	6
#define MT9V113_VAR_AE_MAX_INDEX	0xa20c
#define MT9V113_VAR_AE_INDEX		0xa21b
#define MT9V113_VAR_AE_VIRTGAIN		0xa21c
#define MT9V113_REG_PLL_CONTROL		0x0014
#define MT9V113_REG_CLOCKS_CONTROL	0x0016

#define CAM_SENSOR_WAIT_MS	500

/* ---- tunables ------------------------------------------------------------ */

static uint pll_p1;
module_param(pll_p1, uint, 0644);
MODULE_PARM_DESC(pll_p1,
		 "initial MT9V113 PLL P1 post-divider up to 320 wide (0 = DSi timing, fastest); raised automatically on overrun");

/* 640 wide is 4x the bytes per line: PIO needs the slower clock from the start. */
static uint pll_p1_vga = 1;
module_param(pll_p1_vga, uint, 0644);
MODULE_PARM_DESC(pll_p1_vga,
		 "initial MT9V113 PLL P1 post-divider above 320 wide; raised automatically on overrun");

static uint cnt_bits = CAM_CNT_MUST_BE_1;
module_param(cnt_bits, uint, 0644);
MODULE_PARM_DESC(cnt_bits, "CAM_CNT value used while streaming, less bit 15");

/* MODE_SENSOR_READ_MODE: bit0 mirrors X, bit1 flips Y. DSi values. */
static uint read_mode_inner = 0x24;
module_param(read_mode_inner, uint, 0644);
MODULE_PARM_DESC(read_mode_inner, "MT9V113 read mode for the internal camera");

static uint read_mode_outer = 0x25;
module_param(read_mode_outer, uint, 0644);
MODULE_PARM_DESC(read_mode_outer, "MT9V113 read mode for the outer cameras");

/*
 * AE_MAX_INDEX: how many flicker zones auto-exposure may integrate for.
 * GBATEK: the DSi's 6 leaves pictures nearly black indoors, and the power-on
 * 0x18 is much brighter at the cost of frame rate. On hardware (#314) that
 * cost was a 1 fps preview, so 12 halves the worst case.
 * 0 leaves the power-on value.
 */
static uint ae_max_index = 12;
module_param(ae_max_index, uint, 0644);
MODULE_PARM_DESC(ae_max_index,
		 "MT9V113 AE_MAX_INDEX (6 = DSi, dark but fast; 0x18 = power-on, bright but ~1 fps indoors; 0 = leave)");

/* ---- state --------------------------------------------------------------- */

struct ctr_cam_sensor {
	const char *name;
	struct i2c_adapter *adap;
	u16 addr;
	u8 port;
	bool inner;
	bool present;
	bool active;
};

struct ctr_cam_port {
	void __iomem *regs;
	void __iomem *fifo;
};

struct ctr_cam {
	struct device *dev;
	void __iomem *pdn;
	struct ctr_cam_port port[2];
	struct ctr_cam_sensor sensor[CTR_CAM_COUNT];
	struct miscdevice misc;

	struct mutex lock;		/* open, ioctl, release */
	bool opened;

	/* Stream configuration; changed only with the thread stopped. */
	struct task_struct *thread;
	int cur;			/* streaming camera, -1 = none */
	u32 width, height, frame_bytes;
	u8 p1;
	u8 p1_learned[2];		/* [0] <= 320 wide, [1] larger */
	bool streaming;

	/* Triple buffer: the thread fills, read() takes the newest. */
	void *buf[3];
	int fill, ready, user;
	u32 ready_seq, user_seq, ready_len;
	spinlock_t buf_lock;
	wait_queue_head_t frame_wq;

	/* Framing, owned by the thread. */
	u32 pos;			/* bytes in buf[fill] */
	u32 since_gap;			/* bytes read since the last gap */
	u32 win;			/* FIFO window read offset */
	bool need_gap;
	bool gapless;
	unsigned int consec_overruns;
	unsigned int gaps_logged;
	u64 last_data_ns;

	struct ctr_cam_stats stats;	/* thread writes, STATS ioctl reads */
};

/* ---- MT9V113 over i2c ----------------------------------------------------- */

static int ctr_cam_sensor_read(struct i2c_adapter *adap, u16 addr, u16 reg,
			       u16 *out)
{
	u8 tx[2] = { reg >> 8, reg & 0xff };
	u8 rx[2] = { 0x00, 0x00 };
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0,        .len = 2, .buf = tx },
		{ .addr = addr, .flags = I2C_M_RD, .len = 2, .buf = rx },
	};
	int ret = i2c_transfer(adap, msgs, 2);

	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	*out = (rx[0] << 8) | rx[1];
	return 0;
}

static int ctr_cam_sensor_write(struct i2c_adapter *adap, u16 addr, u16 reg,
				u16 val)
{
	u8 tx[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xff };
	struct i2c_msg msg = {
		.addr = addr, .flags = 0, .len = 4, .buf = tx,
	};
	int ret = i2c_transfer(adap, &msg, 1);

	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	return 0;
}

/*
 * A sequence of sensor operations that stops at the first i2c error and
 * remembers it, so the init lists below read like GBATEK's. Waits that time
 * out only warn: the DSi carries on regardless, and so do we.
 */
struct apt {
	struct ctr_cam *cam;
	struct ctr_cam_sensor *s;
	int err;
};

static u16 apt_rd(struct apt *a, u16 reg)
{
	u16 val = 0;

	if (a->err)
		return 0;
	a->err = ctr_cam_sensor_read(a->s->adap, a->s->addr, reg, &val);
	if (a->err)
		dev_err(a->cam->dev, "%s: read R0x%04x failed (%d)\n",
			a->s->name, reg, a->err);
	return val;
}

static void apt_wr(struct apt *a, u16 reg, u16 val)
{
	if (a->err)
		return;
	a->err = ctr_cam_sensor_write(a->s->adap, a->s->addr, reg, val);
	if (a->err)
		dev_err(a->cam->dev, "%s: write R0x%04x=0x%04x failed (%d)\n",
			a->s->name, reg, val, a->err);
}

static void apt_set(struct apt *a, u16 reg, u16 bits)
{
	u16 val = apt_rd(a, reg);

	apt_wr(a, reg, val | bits);
}

static void apt_clr(struct apt *a, u16 reg, u16 bits)
{
	u16 val = apt_rd(a, reg);

	apt_wr(a, reg, val & ~bits);
}

static void apt_wait(struct apt *a, u16 reg, u16 mask, bool set,
		     const char *what)
{
	unsigned long end = jiffies + msecs_to_jiffies(CAM_SENSOR_WAIT_MS);
	u16 val;

	for (;;) {
		val = apt_rd(a, reg);
		if (a->err || !!(val & mask) == set)
			return;
		if (time_after(jiffies, end))
			break;
		usleep_range(1000, 2000);
	}
	dev_warn(a->cam->dev, "%s: %s: R0x%04x still 0x%04x after %d ms\n",
		 a->s->name, what, reg, val, CAM_SENSOR_WAIT_MS);
}

static void apt_mcu_wr(struct apt *a, u16 var, u16 val)
{
	apt_wr(a, MT9V113_REG_MCU_ADDRESS, var);
	apt_wr(a, MT9V113_REG_MCU_DATA_0, val);
}

static u16 apt_mcu_rd(struct apt *a, u16 var)
{
	apt_wr(a, MT9V113_REG_MCU_ADDRESS, var);
	return apt_rd(a, MT9V113_REG_MCU_DATA_0);
}

static void apt_mcu_set(struct apt *a, u16 var, u16 bits)
{
	u16 val = apt_mcu_rd(a, var);

	apt_mcu_wr(a, var, val | bits);
}

/* The sensor firmware clears SEQ_CMD once it has taken the command. */
static void apt_seq(struct apt *a, u16 cmd)
{
	unsigned long end = jiffies + msecs_to_jiffies(CAM_SENSOR_WAIT_MS);
	u16 val;

	apt_mcu_wr(a, MT9V113_VAR_SEQ_CMD, cmd);
	for (;;) {
		val = apt_mcu_rd(a, MT9V113_VAR_SEQ_CMD);
		if (a->err || !(val & 0x0f))
			return;
		if (time_after(jiffies, end))
			break;
		usleep_range(1000, 2000);
	}
	dev_warn(a->cam->dev, "%s: SEQ_CMD %u not taken after %d ms (0x%04x)\n",
		 a->s->name, cmd, CAM_SENSOR_WAIT_MS, val);
}

/*
 * GBATEK "aptina_code_list_init" followed by "aptina_code_list_activate",
 * with context A sized to the request instead of the DSi's 256x192, and
 * the PLL post-divider taken from the caller.
 */
static int ctr_cam_sensor_start(struct ctr_cam *cam, struct ctr_cam_sensor *s,
				u32 width, u32 height, u8 p1)
{
	struct apt a = { .cam = cam, .s = s };
	bool inner = s->inner;
	u16 read_mode = inner ? read_mode_inner : read_mode_outer;
	u16 state;

	/* reset */
	apt_wr(&a, 0x001a, 0x0003);
	apt_wr(&a, 0x001a, 0x0000);
	usleep_range(1000, 2000);

	/* wakeup */
	apt_wr(&a, 0x0018, 0x4028);
	apt_wr(&a, 0x001e, 0x0201);
	apt_wr(&a, 0x0016, 0x42df);
	apt_wait(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY_DONE, false,
		 "wakeup");
	apt_wait(&a, MT9V113_REG_CORE_301A, MT9V113_CORE_AWAKE, true,
		 "wakeup (core)");

	apt_mcu_wr(&a, 0x02f0, 0x0000);
	apt_mcu_wr(&a, 0x02f2, 0x0210);
	apt_mcu_wr(&a, 0x02f4, 0x001a);
	apt_mcu_wr(&a, 0x2145, 0x02f4);
	apt_mcu_wr(&a, 0xa134, 0x0001);
	apt_mcu_set(&a, 0xa115, 0x0002);	/* SEQ_CAP_MODE: video */

	/* YUV, luma first: Y0 Cb Y1 Cr, i.e. YUYV in memory */
	apt_mcu_wr(&a, 0x2755, 0x0002);		/* MODE_OUTPUT_FORMAT_A */
	apt_mcu_wr(&a, 0x2757, 0x0002);		/* MODE_OUTPUT_FORMAT_B */

	/* PLL */
	apt_wr(&a, 0x0014, 0x2145);
	apt_wr(&a, 0x0010, 0x0111);
	apt_wr(&a, 0x0012, p1 & 0x0f);
	apt_wr(&a, 0x0014, 0x244b);
	apt_wr(&a, 0x0014, 0x304b);
	apt_wait(&a, 0x0014, BIT(15), true, "PLL lock");
	apt_clr(&a, 0x0014, 0x0001);		/* leave bypass */

	/* context A = what we stream; B stays the DSi's 640x480 */
	apt_mcu_wr(&a, 0x2703, width);
	apt_mcu_wr(&a, 0x2705, height);
	apt_mcu_wr(&a, 0x2707, 0x0280);
	apt_mcu_wr(&a, 0x2709, 0x01e0);

	apt_mcu_wr(&a, 0x2715, 0x0001);
	apt_mcu_wr(&a, 0x2719, 0x001a);
	apt_mcu_wr(&a, 0x271b, 0x006b);
	apt_mcu_wr(&a, 0x271d, 0x006b);
	apt_mcu_wr(&a, 0x271f, 0x02c0);
	apt_mcu_wr(&a, 0x2721, 0x034b);
	if (ae_max_index)
		apt_mcu_wr(&a, MT9V113_VAR_AE_MAX_INDEX, ae_max_index & 0xff);
	apt_mcu_wr(&a, 0x272b, 0x0001);
	apt_mcu_wr(&a, 0x272f, 0x001a);
	apt_mcu_wr(&a, 0x2731, 0x006b);
	apt_mcu_wr(&a, 0x2733, 0x006b);
	apt_mcu_wr(&a, 0x2735, 0x02c0);
	apt_mcu_wr(&a, 0x2737, 0x034b);

	apt_set(&a, 0x3210, 0x0008);
	apt_mcu_wr(&a, 0xa208, 0x0000);
	apt_mcu_wr(&a, 0xa24c, 0x0020);
	apt_mcu_wr(&a, 0xa24f, 0x0070);
	apt_mcu_wr(&a, 0x2717, read_mode);	/* MODE_SENSOR_READ_MODE_A */
	apt_mcu_wr(&a, 0x272d, read_mode);	/* MODE_SENSOR_READ_MODE_B */
	apt_mcu_wr(&a, 0xa202, inner ? 0x22 : 0x00);	/* AE window */
	apt_mcu_wr(&a, 0xa203, inner ? 0xbb : 0xff);
	apt_set(&a, 0x0016, 0x0020);
	apt_mcu_wr(&a, 0xa115, 0x0072);
	apt_mcu_wr(&a, 0xa11f, 0x0001);
	apt_wr(&a, 0x326c, inner ? 0x0900 : 0x1000);
	apt_mcu_wr(&a, 0xab22, inner ? 0x0001 : 0x0002);
	apt_seq(&a, MT9V113_SEQ_CMD_REFRESH_MODE);
	apt_seq(&a, MT9V113_SEQ_CMD_REFRESH);

	/* activate */
	apt_clr(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY);
	apt_wait(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY_DONE, false,
		 "wakeup");
	apt_wait(&a, MT9V113_REG_CORE_301A, MT9V113_CORE_AWAKE, true,
		 "wakeup (core)");
	apt_set(&a, MT9V113_REG_RESET_MISC, MT9V113_PARALLEL_ENABLE);

	state = apt_mcu_rd(&a, MT9V113_VAR_SEQ_STATE);
	if (a.err) {
		dev_err(cam->dev, "%s: sensor start failed (%d)\n", s->name,
			a.err);
		return a.err;
	}

	s->active = true;
	dev_info(cam->dev, "%s: sensor streaming %ux%u, P1 %u, SEQ_STATE %u, read mode 0x%02x, AE max index %u (N3DS_CAM_FRAME_RATE)\n",
		 s->name, width, height, p1, state, read_mode, ae_max_index);
	return 0;
}

/* GBATEK "aptina_code_list_deactivate": off the shared bus, into standby. */
static void ctr_cam_sensor_park(struct ctr_cam *cam, struct ctr_cam_sensor *s)
{
	struct apt a = { .cam = cam, .s = s };

	apt_clr(&a, MT9V113_REG_RESET_MISC, MT9V113_PARALLEL_ENABLE);
	apt_set(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY);
	apt_wait(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY_DONE, true,
		 "standby");
	apt_wait(&a, MT9V113_REG_CORE_301A, MT9V113_CORE_AWAKE, false,
		 "standby (core)");
	s->active = false;
	if (a.err)
		dev_warn(cam->dev, "%s: park failed (%d)\n", s->name, a.err);
}

/* ---- receiver ------------------------------------------------------------- */

static inline u32 ctr_cam_fifo_word(struct ctr_cam_port *p, u32 *win)
{
	u32 w = readl_relaxed(p->fifo + *win);

	*win = (*win + 4) & (CAM_FIFO_WINDOW - 1);
	return w;
}

static void ctr_cam_fifo_flush(struct ctr_cam_port *p, u32 *win)
{
	unsigned int guard = 0x4000;
	u16 n;

	while (guard && (n = readw(p->regs + CAM_STAT) & CAM_STAT_FILL)) {
		while (n-- && guard--)
			ctr_cam_fifo_word(p, win);
	}
}

static void ctr_cam_port_stop(struct ctr_cam_port *p, u32 *win)
{
	writew(readw(p->regs + CAM_CNT) & ~CAM_CNT_XFER, p->regs + CAM_CNT);
	ctr_cam_fifo_flush(p, win);
	writew(readw(p->regs + CAM_CTL2) | CAM_CTL2_CLR_OVERRUN,
	       p->regs + CAM_CTL2);
}

/*
 * GBATEK on CAM_CNT bit 4: "MUST be 1, else data transfer hangs, maybe
 * reset?". Pulse it low with transfers off so the receiver drops whatever
 * frame state a STOP left it in. Called with the sensor parked or before its
 * init list, never mid-stream.
 */
static void ctr_cam_port_reset(struct ctr_cam_port *p, u32 *win)
{
	writew(0, p->regs + CAM_CNT);
	usleep_range(1000, 2000);
	writew(CAM_CNT_MUST_BE_1, p->regs + CAM_CNT);
	usleep_range(1000, 2000);
	ctr_cam_fifo_flush(p, win);
	writew(readw(p->regs + CAM_CTL2) | CAM_CTL2_CLR_OVERRUN,
	       p->regs + CAM_CTL2);
}

static void ctr_cam_port_start(struct ctr_cam *cam, struct ctr_cam_port *p)
{
	u16 cnt = cnt_bits & ~CAM_CNT_XFER;

	writew(cnt, p->regs + CAM_CNT);
	ctr_cam_fifo_flush(p, &cam->win);
	writew(readw(p->regs + CAM_CTL2) | CAM_CTL2_CLR_OVERRUN,
	       p->regs + CAM_CTL2);

	/* Full-frame trim window, in case the trimming does do something. */
	writew(0, p->regs + CAM_TRIM_X0);
	writew(0, p->regs + CAM_TRIM_Y0);
	writew((cam->width - 2) & 0x3fe, p->regs + CAM_TRIM_X1);
	writew((cam->height - 1) & 0x1ff, p->regs + CAM_TRIM_Y1);

	writew(cnt | CAM_CNT_XFER, p->regs + CAM_CNT);
}

static void ctr_cam_reset_framing(struct ctr_cam *cam)
{
	cam->pos = 0;
	cam->since_gap = 0;
	cam->need_gap = true;
	cam->consec_overruns = 0;
	cam->last_data_ns = ktime_get_ns();
}

static void ctr_cam_deliver(struct ctr_cam *cam)
{
	spin_lock(&cam->buf_lock);
	swap(cam->fill, cam->ready);
	cam->ready_len = cam->frame_bytes;
	cam->ready_seq++;
	spin_unlock(&cam->buf_lock);
	wake_up_interruptible(&cam->frame_wq);

	cam->stats.frames++;
	cam->consec_overruns = 0;
	cam->pos = 0;
	cam->need_gap = !cam->gapless;
}

static void ctr_cam_gap(struct ctr_cam *cam)
{
	cam->stats.last_gap_bytes = cam->since_gap;
	if (cam->gaps_logged < 4) {
		cam->gaps_logged++;
		dev_info(cam->dev, "%s: frame gap after %u bytes (a %ux%u frame is %u)\n",
			 cam->sensor[cam->cur].name, cam->since_gap,
			 cam->width, cam->height, cam->frame_bytes);
	}
	if (!cam->need_gap && cam->pos)
		cam->stats.short_frames++;
	cam->since_gap = 0;
	cam->pos = 0;
	cam->need_gap = false;
}

static void ctr_cam_drain(struct ctr_cam *cam, struct ctr_cam_port *p,
			  unsigned int n)
{
	cam->since_gap += n * 4;
	cam->stats.words += n;

	while (n) {
		u32 *dst;
		unsigned int take, i;

		if (cam->need_gap) {
			while (n--)
				ctr_cam_fifo_word(p, &cam->win);
			break;
		}

		take = min_t(unsigned int, n, (cam->frame_bytes - cam->pos) / 4);
		dst = cam->buf[cam->fill] + cam->pos;
		for (i = 0; i < take; i++)
			dst[i] = ctr_cam_fifo_word(p, &cam->win);
		cam->pos += take * 4;
		n -= take;

		if (cam->pos == cam->frame_bytes)
			ctr_cam_deliver(cam);
	}

	if (cam->need_gap && !cam->gapless &&
	    cam->since_gap > 2 * cam->frame_bytes) {
		dev_warn(cam->dev,
			 "%s: no inter-frame gap in %u bytes; framing by byte count from here on\n",
			 cam->sensor[cam->cur].name, cam->since_gap);
		cam->gapless = true;
		cam->need_gap = false;
		cam->pos = 0;
	}
}

static void ctr_cam_overrun(struct ctr_cam *cam, struct ctr_cam_port *p)
{
	struct ctr_cam_sensor *s = &cam->sensor[cam->cur];

	ctr_cam_port_stop(p, &cam->win);
	cam->stats.overruns++;

	if (++cam->consec_overruns >= CAM_OVERRUN_SLOWDOWN &&
	    cam->p1 < CAM_P1_MAX) {
		cam->p1++;
		cam->p1_learned[cam->width > 320] = cam->p1;
		cam->stats.p1 = cam->p1;
		dev_warn(cam->dev,
			 "%s: %u overruns without a frame; slowing the pixel clock to P1 %u\n",
			 s->name, cam->consec_overruns, cam->p1);
		ctr_cam_sensor_start(cam, s, cam->width, cam->height, cam->p1);
		cam->consec_overruns = 0;
	} else if (cam->consec_overruns == CAM_OVERRUN_SLOWDOWN * 4) {
		dev_warn(cam->dev,
			 "%s: still overrunning at P1 %u; PIO cannot keep up or the receiver is misconfigured\n",
			 s->name, cam->p1);
	}

	ctr_cam_reset_framing(cam);
	ctr_cam_port_start(cam, p);
}

static void ctr_cam_report_nodata(struct ctr_cam *cam, struct ctr_cam_port *p)
{
	struct ctr_cam_sensor *s = &cam->sensor[cam->cur];
	struct apt a = { .cam = cam, .s = s };
	u16 misc, standby, state, pll, clocks;

	misc = apt_rd(&a, MT9V113_REG_RESET_MISC);
	standby = apt_rd(&a, MT9V113_REG_STANDBY_CONTROL);
	state = apt_mcu_rd(&a, MT9V113_VAR_SEQ_STATE);
	pll = apt_rd(&a, MT9V113_REG_PLL_CONTROL);
	clocks = apt_rd(&a, MT9V113_REG_CLOCKS_CONTROL);
	dev_warn(cam->dev,
		 "%s: no FIFO data 2 s after start: CNT=%04x STAT=%04x +006=%04x; sensor R0x001a=%04x R0x0018=%04x SEQ_STATE=%u PLL=%04x CLK=%04x (i2c %d)\n",
		 s->name, readw(p->regs + CAM_CNT), readw(p->regs + CAM_STAT),
		 readw(p->regs + CAM_CTL2), misc, standby, state, pll, clocks,
		 a.err);
}

/*
 * N3DS_CAM_RECEIVER_RESET, the escalation. Each step is tried after 2 s
 * without a byte:
 *   1. reset the receiver, leaving the sensor streaming
 *   2. park and restart the sensor, and reset the receiver in between
 *   3. step 2 again, after cycling the camera clock (CFG11_CAMERA_CNT)
 */
static void ctr_cam_recover(struct ctr_cam *cam, struct ctr_cam_port *p,
			    unsigned int step)
{
	struct ctr_cam_sensor *s = &cam->sensor[cam->cur];

	dev_warn(cam->dev, "%s: no-data recovery step %u: %s\n", s->name, step,
		 step == 1 ? "receiver reset" :
		 step == 2 ? "sensor restart + receiver reset" :
			     "camera clock cycle + sensor restart + receiver reset");

	ctr_cam_port_stop(p, &cam->win);
	if (step >= 2)
		ctr_cam_sensor_park(cam, s);
	if (step >= 3) {
		iowrite8(ioread8(cam->pdn) & ~PDN_CAM_CLOCK_ENABLE, cam->pdn);
		usleep_range(5000, 6000);
		iowrite8(ioread8(cam->pdn) | PDN_CAM_CLOCK_ENABLE, cam->pdn);
		usleep_range(5000, 6000);
	}
	ctr_cam_port_reset(p, &cam->win);
	if (step >= 2)
		ctr_cam_sensor_start(cam, s, cam->width, cam->height, cam->p1);
	ctr_cam_reset_framing(cam);
	ctr_cam_port_start(cam, p);
}

static int ctr_cam_thread(void *data)
{
	struct ctr_cam *cam = data;
	struct ctr_cam_sensor *s = &cam->sensor[cam->cur];
	struct ctr_cam_port *p = &cam->port[s->port];
	u64 started = ktime_get_ns();
	u64 next_report = started + CAM_REPORT_NS;
	u64 next_recovery = started + CAM_NODATA_NS;
	u32 frames_at_report = 0;
	unsigned int recovery = 0;
	bool seen_data = false;
	/*
	 * Without high-res timers a sleep here is a whole tick (1 ms). Above
	 * 320x240 the FIFO fills faster than that, so inside a frame the
	 * thread spins instead. It only spins for the frame's active lines.
	 */
	bool spin = cam->frame_bytes > CAM_QVGA_BYTES;

	/*
	 * #314 ran at sched_set_fifo_low(), where every other RT thread
	 * preempted it; its 8 overruns in 140 s came at a FIFO peak of 480
	 * units out of 1280.
	 */
	sched_set_fifo(current);

	while (!kthread_should_stop()) {
		u16 stat = readw_relaxed(p->regs + CAM_STAT);
		unsigned int n = stat & CAM_STAT_FILL;
		u64 now;

		if (stat & CAM_STAT_OVERRUN) {
			ctr_cam_overrun(cam, p);
			continue;
		}

		if (n) {
			if (n > cam->stats.max_fifo)
				cam->stats.max_fifo = n;
			ctr_cam_drain(cam, p, n);
			cam->last_data_ns = ktime_get_ns();
			if (!seen_data) {
				seen_data = true;
				dev_info(cam->dev, "%s: first FIFO data %llu ms after start (recovery steps used: %u)\n",
					 s->name,
					 div_u64(cam->last_data_ns - started,
						 NSEC_PER_MSEC), recovery);
			}
			continue;
		}

		now = ktime_get_ns();
		if (cam->since_gap) {
			if (now - cam->last_data_ns > CAM_GAP_NS) {
				ctr_cam_gap(cam);
			} else if (spin) {
				udelay(10);
				continue;
			}
		}

		if (!seen_data && now > next_recovery) {
			if (!recovery)
				ctr_cam_report_nodata(cam, p);
			if (recovery < CAM_RECOVERY_STEPS)
				ctr_cam_recover(cam, p, ++recovery);
			next_recovery = recovery < CAM_RECOVERY_STEPS ?
					ktime_get_ns() + CAM_NODATA_NS : U64_MAX;
			continue;
		}

		/* Only between frames: the i2c reads below take milliseconds. */
		if (now > next_report && !cam->since_gap) {
			struct apt a = { .cam = cam, .s = s };
			u32 frames = cam->stats.frames;
			u16 ae_index = apt_mcu_rd(&a, MT9V113_VAR_AE_INDEX);
			u16 ae_gain = apt_mcu_rd(&a, MT9V113_VAR_AE_VIRTGAIN);

			dev_info(cam->dev,
				 "%s: %u frames (%u in 10 s), %u short, %u overruns, last gap-to-gap %u bytes (want %u), FIFO peak %u, P1 %u, AE index %u (max %u) gain 0x%02x (i2c %d)\n",
				 s->name, frames,
				 frames - frames_at_report,
				 cam->stats.short_frames, cam->stats.overruns,
				 cam->stats.last_gap_bytes, cam->frame_bytes,
				 cam->stats.max_fifo, cam->p1, ae_index,
				 ae_max_index, ae_gain, a.err);
			frames_at_report = frames;
			next_report = ktime_get_ns() + CAM_REPORT_NS;
		}

		usleep_range(100, 200);
	}
	return 0;
}

/* ---- stream control (cam->lock held) ------------------------------------- */

static void ctr_cam_stream_stop(struct ctr_cam *cam)
{
	struct ctr_cam_sensor *s;

	if (cam->cur < 0)
		return;
	s = &cam->sensor[cam->cur];

	if (cam->thread) {
		kthread_stop(cam->thread);
		cam->thread = NULL;
	}
	ctr_cam_port_stop(&cam->port[s->port], &cam->win);
	ctr_cam_sensor_park(cam, s);

	spin_lock(&cam->buf_lock);
	cam->streaming = false;
	spin_unlock(&cam->buf_lock);
	wake_up_interruptible(&cam->frame_wq);

	dev_info(cam->dev, "%s: stopped after %u frames, %u short, %u overruns\n",
		 s->name, cam->stats.frames, cam->stats.short_frames,
		 cam->stats.overruns);
	cam->cur = -1;
}

static int ctr_cam_stream_start(struct ctr_cam *cam,
				const struct ctr_cam_config *cfg)
{
	struct ctr_cam_sensor *s;
	struct task_struct *t;
	unsigned int i;
	int ret;

	if (cfg->camera >= CTR_CAM_COUNT || cfg->flags)
		return -EINVAL;
	if (cfg->width < 16 || cfg->width > CAM_MAX_WIDTH || (cfg->width & 1) ||
	    cfg->height < 16 || cfg->height > CAM_MAX_HEIGHT)
		return -EINVAL;

	s = &cam->sensor[cfg->camera];
	if (!s->present) {
		dev_err(cam->dev, "%s: not present, cannot stream\n", s->name);
		return -ENODEV;
	}
	if (!cam->port[s->port].fifo) {
		dev_err(cam->dev, "%s: port %u FIFO not mapped\n", s->name,
			s->port);
		return -ENODEV;
	}

	ctr_cam_stream_stop(cam);

	/* The internal and outer-right sensors share CAM0's pixel bus. */
	for (i = 0; i < CTR_CAM_COUNT; i++)
		if (i != cfg->camera && cam->sensor[i].active &&
		    cam->sensor[i].port == s->port)
			ctr_cam_sensor_park(cam, &cam->sensor[i]);

	cam->cur = cfg->camera;
	cam->width = cfg->width;
	cam->height = cfg->height;
	cam->frame_bytes = cfg->width * cfg->height * 2;
	cam->p1 = max_t(u8, cam->p1_learned[cam->width > 320],
			min_t(uint, cam->width > 320 ? pll_p1_vga : pll_p1,
			      CAM_P1_MAX));
	cam->gapless = false;
	cam->gaps_logged = 0;
	memset(&cam->stats, 0, sizeof(cam->stats));
	cam->stats.p1 = cam->p1;

	/* N3DS_CAM_RECEIVER_RESET: before the sensor drives the bus again. */
	ctr_cam_port_reset(&cam->port[s->port], &cam->win);

	ret = ctr_cam_sensor_start(cam, s, cam->width, cam->height, cam->p1);
	if (ret) {
		ctr_cam_sensor_park(cam, s);
		cam->cur = -1;
		return ret;
	}

	spin_lock(&cam->buf_lock);
	cam->user_seq = cam->ready_seq;
	cam->streaming = true;
	spin_unlock(&cam->buf_lock);

	ctr_cam_reset_framing(cam);
	ctr_cam_port_start(cam, &cam->port[s->port]);

	t = kthread_run(ctr_cam_thread, cam, "ctr_cam%u", cfg->camera);
	if (IS_ERR(t)) {
		dev_err(cam->dev, "stream thread failed: %ld\n", PTR_ERR(t));
		ctr_cam_port_stop(&cam->port[s->port], &cam->win);
		ctr_cam_sensor_park(cam, s);
		cam->streaming = false;
		cam->cur = -1;
		return PTR_ERR(t);
	}
	cam->thread = t;
	return 0;
}

/* ---- /dev/ctr_cam --------------------------------------------------------- */

static struct ctr_cam *ctr_cam_from_file(struct file *f)
{
	return container_of(f->private_data, struct ctr_cam, misc);
}

static int ctr_cam_open(struct inode *inode, struct file *f)
{
	struct ctr_cam *cam = ctr_cam_from_file(f);
	unsigned int i;
	int ret = 0;

	mutex_lock(&cam->lock);
	if (cam->opened) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < 3; i++) {
		cam->buf[i] = vzalloc(CAM_BUF_BYTES);
		if (!cam->buf[i]) {
			while (i--) {
				vfree(cam->buf[i]);
				cam->buf[i] = NULL;
			}
			ret = -ENOMEM;
			goto out;
		}
	}
	cam->fill = 0;
	cam->ready = 1;
	cam->user = 2;
	cam->ready_seq = cam->user_seq = 0;
	cam->ready_len = 0;
	cam->opened = true;
out:
	mutex_unlock(&cam->lock);
	return ret;
}

static int ctr_cam_release(struct inode *inode, struct file *f)
{
	struct ctr_cam *cam = ctr_cam_from_file(f);
	unsigned int i;

	mutex_lock(&cam->lock);
	ctr_cam_stream_stop(cam);
	for (i = 0; i < 3; i++) {
		vfree(cam->buf[i]);
		cam->buf[i] = NULL;
	}
	cam->opened = false;
	mutex_unlock(&cam->lock);
	return 0;
}

static bool ctr_cam_frame_waiting(struct ctr_cam *cam)
{
	bool ret;

	spin_lock(&cam->buf_lock);
	ret = cam->ready_seq != cam->user_seq || !cam->streaming;
	spin_unlock(&cam->buf_lock);
	return ret;
}

/* One call, one frame: the newest complete one not yet returned. */
static ssize_t ctr_cam_read(struct file *f, char __user *ubuf, size_t len,
			    loff_t *ppos)
{
	struct ctr_cam *cam = ctr_cam_from_file(f);
	long left;
	u32 n;

	if (!(f->f_flags & O_NONBLOCK)) {
		left = wait_event_interruptible_timeout(cam->frame_wq,
				ctr_cam_frame_waiting(cam), 2 * HZ);
		if (left < 0)
			return left;
		if (!left)
			return -ETIMEDOUT;
	}

	spin_lock(&cam->buf_lock);
	if (!cam->streaming) {
		spin_unlock(&cam->buf_lock);
		return -EIO;
	}
	if (cam->ready_seq == cam->user_seq) {
		spin_unlock(&cam->buf_lock);
		return -EAGAIN;
	}
	swap(cam->ready, cam->user);
	cam->user_seq = cam->ready_seq;
	n = cam->ready_len;
	spin_unlock(&cam->buf_lock);

	if (len < n)
		return -EINVAL;
	if (copy_to_user(ubuf, cam->buf[cam->user], n))
		return -EFAULT;
	return n;
}

static long ctr_cam_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct ctr_cam *cam = ctr_cam_from_file(f);
	struct ctr_cam_config cfg;
	struct ctr_cam_stats st;
	long ret;

	switch (cmd) {
	case CTR_CAM_IOC_START:
		if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
			return -EFAULT;
		mutex_lock(&cam->lock);
		ret = ctr_cam_stream_start(cam, &cfg);
		mutex_unlock(&cam->lock);
		return ret;
	case CTR_CAM_IOC_STOP:
		mutex_lock(&cam->lock);
		ctr_cam_stream_stop(cam);
		mutex_unlock(&cam->lock);
		return 0;
	case CTR_CAM_IOC_STATS:
		st = cam->stats;
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations ctr_cam_fops = {
	.owner		= THIS_MODULE,
	.open		= ctr_cam_open,
	.release	= ctr_cam_release,
	.read		= ctr_cam_read,
	.unlocked_ioctl	= ctr_cam_ioctl,
	.llseek		= noop_llseek,
};

/* ---- probe ---------------------------------------------------------------- */

static void ctr_cam_identify(struct ctr_cam *cam, struct ctr_cam_sensor *s)
{
	u16 id = 0;
	int ret;

	if (!s->adap) {
		dev_info(cam->dev, "%s: no i2c bus in the DT, skipped\n",
			 s->name);
		return;
	}

	ret = ctr_cam_sensor_read(s->adap, s->addr, MT9V113_REG_CHIP_VERSION,
				  &id);
	if (ret) {
		dev_info(cam->dev, "%s: no answer at 7-bit 0x%02x (%d)\n",
			 s->name, s->addr, ret);
		return;
	}
	s->present = true;
	dev_info(cam->dev, "%s: chip id 0x%04x%s\n", s->name, id,
		 id == MT9V113_CHIP_VERSION ? " (MT9V113)" : " (unexpected)");

	/* Whatever the last owner left running, get it off the pixel bus. */
	ctr_cam_sensor_park(cam, s);
}

static void __iomem *ctr_cam_map(struct platform_device *pdev,
				 const char *name, phys_addr_t fallback)
{
	struct ctr_cam *cam = platform_get_drvdata(pdev);
	struct resource *res;
	void __iomem *p;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (res)
		return devm_ioremap_resource(&pdev->dev, res);
	if (!fallback)
		return IOMEM_ERR_PTR(-ENODEV);

	/* A DTB older than this driver: map the documented address. */
	dev_info(cam->dev, "no \"%s\" reg in the DT, mapping %pa\n", name,
		 &fallback);
	p = devm_ioremap(&pdev->dev, fallback, 0x1000);
	return p ? p : IOMEM_ERR_PTR(-ENOMEM);
}

static struct i2c_adapter *ctr_cam_bus(struct ctr_cam *cam, int index)
{
	struct device_node *node;
	struct i2c_adapter *adap;

	node = of_parse_phandle(cam->dev->of_node, "nintendo,i2c-bus", index);
	if (!node)
		return NULL;
	adap = of_get_i2c_adapter_by_node(node);
	of_node_put(node);
	return adap ? adap : ERR_PTR(-EPROBE_DEFER);
}

static void ctr_cam_put_buses(struct ctr_cam *cam)
{
	/* Sensors 0 and 1 share one adapter reference. */
	if (cam->sensor[0].adap)
		i2c_put_adapter(cam->sensor[0].adap);
	if (cam->sensor[2].adap)
		i2c_put_adapter(cam->sensor[2].adap);
}

static int ctr_cam_probe(struct platform_device *pdev)
{
	struct ctr_cam *cam;
	struct i2c_adapter *bus0, *bus1;
	unsigned int i;
	int ret;

	cam = devm_kzalloc(&pdev->dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->dev = &pdev->dev;
	cam->cur = -1;
	mutex_init(&cam->lock);
	spin_lock_init(&cam->buf_lock);
	init_waitqueue_head(&cam->frame_wq);
	platform_set_drvdata(pdev, cam);

	cam->port[0].regs = ctr_cam_map(pdev, "registers", 0);
	cam->pdn = ctr_cam_map(pdev, "pdn", 0);
	cam->port[1].regs = ctr_cam_map(pdev, "registers-mirror", 0x10121000);
	cam->port[0].fifo = ctr_cam_map(pdev, "fifo0", 0x10320000);
	cam->port[1].fifo = ctr_cam_map(pdev, "fifo1", 0x10321000);
	if (IS_ERR(cam->port[0].regs) || IS_ERR(cam->pdn) ||
	    IS_ERR(cam->port[1].regs) || IS_ERR(cam->port[0].fifo) ||
	    IS_ERR(cam->port[1].fifo)) {
		dev_err(cam->dev, "register map failed: cam %d pdn %d cam1 %d fifo0 %d fifo1 %d\n",
			PTR_ERR_OR_ZERO(cam->port[0].regs),
			PTR_ERR_OR_ZERO(cam->pdn),
			PTR_ERR_OR_ZERO(cam->port[1].regs),
			PTR_ERR_OR_ZERO(cam->port[0].fifo),
			PTR_ERR_OR_ZERO(cam->port[1].fifo));
		return -ENODEV;
	}

	bus0 = ctr_cam_bus(cam, 0);
	bus1 = ctr_cam_bus(cam, 1);
	if (IS_ERR(bus0) || IS_ERR(bus1)) {
		/*
		 * N3DS_CAM_I2C_PROBE_DEFER: nothing orders this driver after
		 * the i2c controllers, and on hardware it wins the race.
		 */
		if (!IS_ERR_OR_NULL(bus0))
			i2c_put_adapter(bus0);
		if (!IS_ERR_OR_NULL(bus1))
			i2c_put_adapter(bus1);
		dev_info(cam->dev, "i2c bus not registered yet; deferring probe\n");
		return -EPROBE_DEFER;
	}

	cam->sensor[CTR_CAM_OUTER_RIGHT] = (struct ctr_cam_sensor) {
		.name = "outer-right", .adap = bus0, .addr = 0x78 >> 1,
		.port = 0,
	};
	cam->sensor[CTR_CAM_INNER] = (struct ctr_cam_sensor) {
		.name = "inner", .adap = bus0, .addr = 0x7a >> 1,
		.port = 0, .inner = true,
	};
	cam->sensor[CTR_CAM_OUTER_LEFT] = (struct ctr_cam_sensor) {
		.name = "outer-left", .adap = bus1, .addr = 0x78 >> 1,
		.port = 1,
	};

	iowrite8(ioread8(cam->pdn) | PDN_CAM_CLOCK_ENABLE, cam->pdn);
	usleep_range(1000, 2000);
	if (!(ioread8(cam->pdn) & PDN_CAM_CLOCK_ENABLE))
		dev_warn(cam->dev, "CFG11_CAMERA_CNT clock enable did not stick\n");

	for (i = 0; i < 2; i++) {
		writew(CAM_CNT_MUST_BE_1, cam->port[i].regs + CAM_CNT);
		ctr_cam_port_stop(&cam->port[i], &cam->win);
	}

	for (i = 0; i < CTR_CAM_COUNT; i++)
		ctr_cam_identify(cam, &cam->sensor[i]);

	cam->misc.minor = MISC_DYNAMIC_MINOR;
	cam->misc.name = "ctr_cam";
	cam->misc.fops = &ctr_cam_fops;
	cam->misc.mode = 0660;
	ret = misc_register(&cam->misc);
	if (ret) {
		dev_err(cam->dev, "misc_register failed: %d\n", ret);
		ctr_cam_put_buses(cam);
		return ret;
	}

	dev_info(cam->dev,
		 "capture driver ready (N3DS_CAM_CAPTURE): outer-right %s, inner %s, outer-left %s; /dev/ctr_cam\n",
		 cam->sensor[0].present ? "yes" : "NO",
		 cam->sensor[1].present ? "yes" : "NO",
		 cam->sensor[2].present ? "yes" : "NO");
	return 0;
}

static int ctr_cam_remove(struct platform_device *pdev)
{
	struct ctr_cam *cam = platform_get_drvdata(pdev);

	misc_deregister(&cam->misc);
	ctr_cam_put_buses(cam);
	iowrite8(ioread8(cam->pdn) & ~PDN_CAM_CLOCK_ENABLE, cam->pdn);
	return 0;
}

static const struct of_device_id ctr_cam_of_match[] = {
	{ .compatible = "nintendo,3ds-cam" },
	{ }
};
MODULE_DEVICE_TABLE(of, ctr_cam_of_match);

static struct platform_driver ctr_cam_driver = {
	.probe = ctr_cam_probe,
	.remove = ctr_cam_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_cam_of_match,
	},
};

/* Same registration diagnostic as ctr_dsp.c: a capture with no 3ds-cam line
 * at all must be able to distinguish "never registered" from "registered but
 * the DT node never bound". */
static int __init ctr_cam_init(void)
{
	int ret = platform_driver_register(&ctr_cam_driver);

	pr_info("driver registered: %d (probe follows only if a nintendo,3ds-cam node bound)\n",
		ret);
	return ret;
}

static void __exit ctr_cam_exit(void)
{
	platform_driver_unregister(&ctr_cam_driver);
}

module_init(ctr_cam_init);
module_exit(ctr_cam_exit);

MODULE_DESCRIPTION("Nintendo 3DS camera capture driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
