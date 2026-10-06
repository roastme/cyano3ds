// SPDX-License-Identifier: GPL-2.0-or-later
/* Android-oriented Nintendo 3DS resistive touchscreen driver.
 *
 * Imported from the Android3DS (Octoblimp) kernel patch, adapted to the
 * Cyano3DS kernel; see the top-level NOTICE for attribution.
 *
 * N3DS_ANDROID_DIRECT_TOUCH
 * N3DS_MEDIAN_TOUCH_SAMPLES
 * N3DS_TOUCH_CALIBRATION
 * N3DS_FIXED_TOUCH_CALIBRATION
 * N3DS_FULL_ADC_TOUCH_RANGE
 * N3DS_TOUCH_FIFO_UNCONDITIONAL
 * N3DS_CIRCLEPAD_TRACKBALL
 *
 * This intentionally replaces the inherited combined touchscreen/circle-pad
 * input device. Android Eclair classifies an EV_ABS+EV_REL hybrid poorly and
 * a direct touchscreen must never also behave like a relative mouse. The
 * panel contact and the circle pad are therefore two input devices: the
 * circle pad is a separate "Android3DS Circle Pad" trackball (EV_REL +
 * BTN_MOUSE, which is what EventHub needs for CLASS_TRACKBALL), fed from the
 * same CDC FIFO read the touch poll already makes.
 */

#define DRIVER_NAME "android3ds-touch"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/device.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

/* N3DS_SPI_WAKE: this used to be 1, but every SPI message then slept out
 * ctr_spi's 100 ms timeout, so a poll (four SPI messages: two bank switches,
 * two reads) really ran at under 10 Hz -- one 5000-poll diagnostic line in
 * 490 s of the #314 boot log.  With that fixed, 1 ms would be a hundredfold
 * jump in bus and CPU load for nothing: 4 ms is 250 Hz, twice the report
 * rate below, and still bounds DOWN/UP latency to one poll.
 * Do not enqueue every sample into Eclair either; deliver the newest one at
 * 125 Hz; DOWN and UP remain immediate. */
/* N3DS_TOUCH_REALTIME_60HZ: until #321 the kernel clock ran 3x fast
 * (N3DS_TWD_PERIPHCLK), so "4 ms / 8 ms" really polled at 750 Hz and sent
 * MOVEs at 375 Hz -- ten times what WindowManagerService would dispatch
 * (35/s, never dropping any), which is where the multi-second finger trail
 * came from.  With real milliseconds: poll at 250 Hz so DOWN and UP still
 * land within 4 ms, and send MOVEs at 60 Hz, the rate the dispatcher now
 * runs at (N3DS_TOUCH_DISPATCH_60HZ) and coalesces to
 * (N3DS_TOUCH_MOVE_COALESCE). */
#define POLL_INTERVAL_MS 4
#define REPORT_INTERVAL_MS 16
#define DIAGNOSTIC_INTERVAL_POLLS (5000 / POLL_INTERVAL_MS)
#define SAMPLE_COUNT 5
#define ADC_MASK 0x0fff
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
/* N3DS_TOUCH_SLOP: a resistive panel's ADC reading drifts a little as
 * contact pressure ramps up after DOWN and ramps back down just before UP
 * (logcat showed DOWN/UP pairs from the same physical tap landing 20-70px
 * apart). That drift is real per-sample movement, not driver/framework
 * duplication, so REPORT_INTERVAL_MS/dedup above do not catch it. Anchor
 * each touch to its DOWN position and only let the reported position move
 * once it has genuinely traveled -- this is the same "touch slop" concept
 * ViewConfiguration uses upstream, just enforced here since Eclair's own
 * gesture code has no idea this panel is resistive. */
#define TOUCH_SLOP_PX 12
#define TOUCH_REG(reg) ((0x67 << 7) | (reg))
#define TOUCH_FIFO_REG ((0xFB << 7) | 0x01)

/* N3DS_CIRCLEPAD_TRACKBALL: the FIFO is 5 touch X, 5 touch Y, then 8 circle
 * pad Y and 8 circle pad X samples, all big-endian 12-bit (xerpi's
 * touch_fifo_data; GodMode9's CODEC_Get reads the same offsets).  The pad
 * rests near 2048.
 *
 * Android Eclair has no joystick input class, so the pad is a trackball
 * whose REL_X/REL_Y are screen pixels: a velocity proportional to how far
 * the stick is tilted, integrated over each report.  ViewRoot turns those
 * into DPAD steps for lists (N3DS_CIRCLEPAD_DPAD) and WebView pans the page
 * by them directly (N3DS_CIRCLEPAD_PAN).  Each axis has its own deadzone, so
 * a mostly-vertical tilt scrolls straight down instead of drifting sideways.
 * Speed rises with the square of the tilt past the deadzone, from
 * cpad_min_speed to cpad_max_speed px/s.  The minimum is never zero, so a
 * held stick sends a report at least every other frame and ViewRoot can
 * tell "still held" from "let go" by the gap. */
#define CPAD_SAMPLE_COUNT 8
#define CPAD_FIFO_Y 0x14
#define CPAD_FIFO_X 0x24
#define CPAD_ADC_CENTER 2048
#define CPAD_CENTER_POLLS 16
#define CPAD_CENTER_TOLERANCE 400
#define CPAD_MAX_DT_MS 50

static unsigned int cpad_deadzone = 200;
static unsigned int cpad_full = 1300;
static unsigned int cpad_min_speed = 60;
static unsigned int cpad_max_speed = 720;
/* xerpi's driver and GodMode9 both negate X (the ADC runs right-to-left)
 * and treat a positive Y as "up"; Android's Y grows downward, so both axes
 * are negated by default. */
static bool cpad_invert_x = true;
static bool cpad_invert_y = true;
static bool cpad_enable = true;
module_param(cpad_deadzone, uint, 0644);
module_param(cpad_full, uint, 0644);
module_param(cpad_min_speed, uint, 0644);
module_param(cpad_max_speed, uint, 0644);
module_param(cpad_invert_x, bool, 0644);
module_param(cpad_invert_y, bool, 0644);
module_param(cpad_enable, bool, 0644);

/* N3DS_TOUCH_PROBE_LOG: the 5-second "probe status=... cpad=..." line was
 * bring-up evidence for the circle pad's rest centre.  With ignore_loglevel
 * every line reaches the top-screen console, so it is opt-in now: set the
 * probe_log parameter under /sys/module/<this object>/parameters/. */
static bool probe_log;
module_param(probe_log, bool, 0644);
MODULE_PARM_DESC(probe_log,
	"log raw touch/circle-pad readings every 5 s (default off, N3DS_TOUCH_PROBE_LOG)");

/* Two-point per-axis calibration: (cal_*0_raw -> cal_*0_px) and
 * (cal_*1_raw -> cal_*1_px). This project has no factory NVRAM calibration
 * to read back (unlike GodMode9's HWCAL, whose SPI config-save flash is
 * wired to Wi-Fi calibration on this port, not touch), so these default to
 * the conservative endpoints from the complete retail-hardware capture,
 * assumed to land at the physical screen edges. A real per-unit calibration
 * -- run from the Touch Diagnostic app's Calibrate mode, which samples two
 * inset on-screen targets against raw_x/raw_y below -- does not need its
 * points to sit on the edges, since the general affine fit in scale_axis()
 * extrapolates and clamps like GodMode9's touchcal.c does. */
static unsigned int cal_x0_raw = 256;
static unsigned int cal_x0_px;
static unsigned int cal_x1_raw = 3840;
static unsigned int cal_x1_px = SCREEN_WIDTH - 1;
static unsigned int cal_y0_raw = 256;
static unsigned int cal_y0_px;
static unsigned int cal_y1_raw = 3840;
static unsigned int cal_y1_px = SCREEN_HEIGHT - 1;
module_param(cal_x0_raw, uint, 0644);
module_param(cal_x0_px, uint, 0644);
module_param(cal_x1_raw, uint, 0644);
module_param(cal_x1_px, uint, 0644);
module_param(cal_y0_raw, uint, 0644);
module_param(cal_y0_px, uint, 0644);
module_param(cal_y1_raw, uint, 0644);
module_param(cal_y1_px, uint, 0644);

struct android3ds_touch {
	struct regmap *map;
	struct input_dev *input;
	bool down;
	u16 last_x;
	u16 last_y;
	u16 down_x;
	u16 down_y;
	/* Latest raw ADC sample, cached unconditionally (independent of the
	 * debounce/dedup below) so a userspace calibration tool can read a
	 * live raw touch position via sysfs before any calibration exists. */
	u16 raw_x;
	u16 raw_y;
	bool raw_down;
	unsigned int polls;
	unsigned long last_report;
	/* N3DS_CIRCLEPAD_TRACKBALL state. */
	struct input_dev *cpad;
	s16 cpad_raw_x;
	s16 cpad_raw_y;
	int cpad_center_x;
	int cpad_center_y;
	int cpad_center_sum_x;
	int cpad_center_sum_y;
	unsigned int cpad_center_polls;
	unsigned long cpad_last_report;
	long cpad_acc_x;	/* sub-pixel remainder, px * ms / s */
	long cpad_acc_y;
	bool cpad_active;
};

static int android3ds_touch_hw_init(struct regmap *map)
{
	static const struct reg_sequence init[] = {
		REG_SEQ(TOUCH_REG(0x24), 0x98, 10),
		REG_SEQ(TOUCH_REG(0x26), 0x00, 10),
		REG_SEQ(TOUCH_REG(0x25), 0x43, 10),
		REG_SEQ(TOUCH_REG(0x24), 0x18, 10),
		REG_SEQ(TOUCH_REG(0x17), 0x43, 10),
		REG_SEQ(TOUCH_REG(0x19), 0x69, 10),
		REG_SEQ(TOUCH_REG(0x1b), 0x80, 10),
		REG_SEQ(TOUCH_REG(0x27), 0x11, 10),
		REG_SEQ(TOUCH_REG(0x26), 0xec, 10),
		REG_SEQ(TOUCH_REG(0x24), 0x18, 10),
		REG_SEQ(TOUCH_REG(0x25), 0x53, 10),
	};
	int ret;

	ret = regmap_multi_reg_write(map, init, ARRAY_SIZE(init));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, TOUCH_REG(0x26), 0x80, 0x80);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, TOUCH_REG(0x24), 0x80, 0x00);
	if (ret)
		return ret;
	return regmap_update_bits(map, TOUCH_REG(0x25), 0x3c, 0x10);
}

static u16 median5(u16 *v)
{
	int i, j;
	for (i = 1; i < SAMPLE_COUNT; i++) {
		u16 n = v[i];
		for (j = i; j && v[j - 1] > n; j--)
			v[j] = v[j - 1];
		v[j] = n;
	}
	return v[2];
}

static u16 median_n(u16 *v, int n)
{
	int i, j;
	for (i = 1; i < n; i++) {
		u16 x = v[i];
		for (j = i; j && v[j - 1] > x; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
	return v[n / 2];
}

/* px * ms / s for one axis over dt_ms: 0 inside the deadzone, otherwise the
 * square-law speed above, signed like the deflection. */
static long cpad_axis_travel(int deflection, unsigned int dt_ms)
{
	unsigned int mag = abs(deflection), dz = cpad_deadzone;
	unsigned int span = cpad_full > dz ? cpad_full - dz : 1;
	unsigned long n, speed;

	if (mag <= dz)
		return 0;
	n = min_t(unsigned long, mag - dz, span) * 1024 / span;	/* 0..1024 */
	speed = cpad_min_speed +
		(unsigned long)(cpad_max_speed > cpad_min_speed ?
				cpad_max_speed - cpad_min_speed : 0) * n * n / (1024 * 1024);
	return deflection < 0 ? -(long)(speed * dt_ms) : (long)(speed * dt_ms);
}

static void android3ds_cpad_update(struct android3ds_touch *ts, const u8 *fifo)
{
	u16 xs[CPAD_SAMPLE_COUNT], ys[CPAD_SAMPLE_COUNT];
	int i, dx, dy, px, py;
	unsigned int dt_ms;
	long tx, ty;

	for (i = 0; i < CPAD_SAMPLE_COUNT; i++) {
		ys[i] = (((u16)fifo[CPAD_FIFO_Y + i * 2] << 8) |
			 fifo[CPAD_FIFO_Y + i * 2 + 1]) & ADC_MASK;
		xs[i] = (((u16)fifo[CPAD_FIFO_X + i * 2] << 8) |
			 fifo[CPAD_FIFO_X + i * 2 + 1]) & ADC_MASK;
	}
	ts->cpad_raw_x = (s16)median_n(xs, CPAD_SAMPLE_COUNT) - CPAD_ADC_CENTER;
	ts->cpad_raw_y = (s16)median_n(ys, CPAD_SAMPLE_COUNT) - CPAD_ADC_CENTER;

	/* Learn this unit's rest position from the first polls (nobody is
	 * holding the stick while the kernel boots).  A reading far from the
	 * nominal centre means it was being held after all: keep 2048. */
	if (ts->cpad_center_polls < CPAD_CENTER_POLLS) {
		ts->cpad_center_sum_x += ts->cpad_raw_x;
		ts->cpad_center_sum_y += ts->cpad_raw_y;
		if (++ts->cpad_center_polls == CPAD_CENTER_POLLS) {
			int cx = ts->cpad_center_sum_x / CPAD_CENTER_POLLS;
			int cy = ts->cpad_center_sum_y / CPAD_CENTER_POLLS;

			if (abs(cx) <= CPAD_CENTER_TOLERANCE &&
			    abs(cy) <= CPAD_CENTER_TOLERANCE) {
				ts->cpad_center_x = cx;
				ts->cpad_center_y = cy;
			}
			pr_info("circle pad rest (%d,%d) -> centre offset (%d,%d) (N3DS_CIRCLEPAD_TRACKBALL)\n",
				cx, cy, ts->cpad_center_x, ts->cpad_center_y);
		}
		return;
	}
	if (!cpad_enable)
		return;

	dx = ts->cpad_raw_x - ts->cpad_center_x;
	dy = ts->cpad_raw_y - ts->cpad_center_y;
	if (abs(dx) <= cpad_deadzone && abs(dy) <= cpad_deadzone) {
		ts->cpad_active = false;
		ts->cpad_acc_x = ts->cpad_acc_y = 0;
		return;
	}
	if (!ts->cpad_active) {
		/* Just left the deadzone: start integrating from now. */
		ts->cpad_active = true;
		ts->cpad_last_report = jiffies;
		ts->cpad_acc_x = ts->cpad_acc_y = 0;
		return;
	}
	if (time_before(jiffies, ts->cpad_last_report +
			msecs_to_jiffies(REPORT_INTERVAL_MS)))
		return;
	dt_ms = min_t(unsigned int, CPAD_MAX_DT_MS,
		      jiffies_to_msecs(jiffies - ts->cpad_last_report));
	ts->cpad_last_report = jiffies;

	tx = ts->cpad_acc_x + cpad_axis_travel(dx, dt_ms);
	ty = ts->cpad_acc_y + cpad_axis_travel(dy, dt_ms);
	px = tx / 1000;
	py = ty / 1000;
	ts->cpad_acc_x = tx - (long)px * 1000;
	ts->cpad_acc_y = ty - (long)py * 1000;
	if (!px && !py)
		return;
	if (cpad_invert_x)
		px = -px;
	if (cpad_invert_y)
		py = -py;
	input_report_rel(ts->cpad, REL_X, px);
	input_report_rel(ts->cpad, REL_Y, py);
	input_sync(ts->cpad);
}

static u16 scale_axis(u16 raw, unsigned int raw0, unsigned int px0,
		      unsigned int raw1, unsigned int px1, unsigned int pixels)
{
	long delta_raw = (long)raw1 - (long)raw0;
	long px;

	if (!pixels)
		return 0;
	if (!delta_raw)
		return (u16)min_t(long, px0, pixels - 1);

	px = (long)px0 + ((long)raw - (long)raw0) * ((long)px1 - (long)px0) / delta_raw;
	if (px < 0)
		px = 0;
	if (px > (long)pixels - 1)
		px = (long)pixels - 1;
	return (u16)px;
}

static ssize_t raw_x_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct android3ds_touch *ts = input_get_drvdata(to_input_dev(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n", ts->raw_x);
}
static DEVICE_ATTR_RO(raw_x);

static ssize_t raw_y_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct android3ds_touch *ts = input_get_drvdata(to_input_dev(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n", ts->raw_y);
}
static DEVICE_ATTR_RO(raw_y);

static ssize_t raw_down_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct android3ds_touch *ts = input_get_drvdata(to_input_dev(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n", ts->raw_down ? 1 : 0);
}
static DEVICE_ATTR_RO(raw_down);

/* N3DS_TOUCH_CALIBRATION_SYSFS: exposed on the input device's own kobject
 * (/sys/class/input/inputN/raw_{x,y,down}) rather than the platform device's,
 * so a calibration app can find it by matching the world-readable sibling
 * "name" file instead of needing to know this driver's DT node address. */
static struct attribute *android3ds_touch_raw_attrs[] = {
	&dev_attr_raw_x.attr,
	&dev_attr_raw_y.attr,
	&dev_attr_raw_down.attr,
	NULL,
};

static const struct attribute_group android3ds_touch_raw_attr_group = {
	.attrs = android3ds_touch_raw_attrs,
};

static void android3ds_touch_poll(struct input_dev *input)
{
	struct android3ds_touch *ts = input_get_drvdata(input);
	u8 fifo[0x34] __aligned(sizeof(u32));
	u16 xs[SAMPLE_COUNT], ys[SAMPLE_COUNT], raw_x, raw_y, x, y;
	unsigned int status;
	bool down;
	int i, ret;

	ret = regmap_read(ts->map, TOUCH_REG(0x26), &status);
	if (!ret)
		ret = regmap_bulk_read(ts->map, TOUCH_FIFO_REG, fifo, sizeof(fifo));
	if (ret)
		return;

	down = !(fifo[0] & BIT(4));
	for (i = 0; i < SAMPLE_COUNT; i++) {
		xs[i] = (((u16)fifo[i * 2] << 8) | fifo[i * 2 + 1]) & ADC_MASK;
		ys[i] = (((u16)fifo[10 + i * 2] << 8) | fifo[11 + i * 2]) & ADC_MASK;
	}
	raw_x = median5(xs);
	raw_y = median5(ys);
	ts->raw_x = raw_x;
	ts->raw_y = raw_y;
	ts->raw_down = down;

	android3ds_cpad_update(ts, fifo);

	if (!(ts->polls++ % DIAGNOSTIC_INTERVAL_POLLS) && probe_log)
		pr_info("probe status=%02x pen=%u raw=(%u,%u) logical=(%u,%u) cpad=(%d,%d)\n",
			status & 0xff, down, raw_x, raw_y, ts->last_x, ts->last_y,
			ts->cpad_raw_x - ts->cpad_center_x,
			ts->cpad_raw_y - ts->cpad_center_y);

	if (down) {
		x = scale_axis(raw_x, cal_x0_raw, cal_x0_px, cal_x1_raw, cal_x1_px,
			       SCREEN_WIDTH);
		y = scale_axis(raw_y, cal_y0_raw, cal_y0_px, cal_y1_raw, cal_y1_px,
			       SCREEN_HEIGHT);
		if (!ts->down) {
			ts->down_x = x;
			ts->down_y = y;
		} else if (abs((int)x - (int)ts->down_x) <= TOUCH_SLOP_PX &&
			   abs((int)y - (int)ts->down_y) <= TOUCH_SLOP_PX) {
			/* Still within slop of the original contact point: treat
			 * as the same tap location instead of chasing per-sample
			 * ADC noise. A genuine drag clears the slop box and
			 * reports real movement from then on. */
			x = ts->down_x;
			y = ts->down_y;
		}
		/* N3DS_TOUCH_LATEST_SAMPLE_DELIVERY: Eclair's input queue
		 * otherwise preserves every sample while the compositor is
		 * drawing, producing the measured multi-second trail.  Skip
		 * stale intermediate coordinates and publish the newest one at
		 * REPORT_INTERVAL_MS (60 Hz). */
		if (ts->down && time_before(jiffies, ts->last_report +
				msecs_to_jiffies(REPORT_INTERVAL_MS))) {
			ts->down = down;
			return;
		}
		if (ts->down && x == ts->last_x && y == ts->last_y) {
			ts->down = down;
			return;
		}
		ts->last_x = x;
		ts->last_y = y;
		ts->last_report = jiffies;
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
		/* N3DS_HARDWARE_SINGLE_TOUCH: the resistive panel can report one
		 * contact.  Keep it on Eclair's persistent BTN_TOUCH state machine;
		 * do not advertise synthetic ABS_MT axes on this device. */
		input_report_abs(input, ABS_PRESSURE, 1);
		input_report_key(input, BTN_TOUCH, 1);
		input_sync(input);
		/* N3DS_TOUCH_QUIET_INPUT: pr_debug, not pr_info -- with
		 * ignore_loglevel every pr_info is an fbcon render on the top
		 * screen, done from inside this poll. */
		if (!ts->down)
			pr_debug("DOWN raw=(%u,%u) logical=(%u,%u)\n", raw_x, raw_y, x, y);
	} else if (ts->down) {
		input_report_abs(input, ABS_PRESSURE, 0);
		input_report_key(input, BTN_TOUCH, 0);
		input_sync(input);
		pr_debug("UP logical=(%u,%u)\n", ts->last_x, ts->last_y);
	}
	ts->down = down;
}

static int android3ds_touch_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct android3ds_touch *ts;
	struct input_dev *input, *cpad;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	input = devm_input_allocate_device(dev);
	cpad = devm_input_allocate_device(dev);
	if (!ts || !input || !cpad)
		return -ENOMEM;
	ts->map = dev_get_regmap(dev->parent, NULL);
	if (!ts->map)
		return -ENODEV;

	input->name = "Android3DS Direct Touchscreen";
	input->phys = DRIVER_NAME "/input0";
	input->id.bustype = BUS_HOST;
	input->dev.parent = dev;
	input_set_drvdata(input, ts);
	input_set_abs_params(input, ABS_X, 0, SCREEN_WIDTH - 1, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, SCREEN_HEIGHT - 1, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, 1, 0, 0);
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	set_bit(INPUT_PROP_DIRECT, input->propbit);
	ts->input = input;
	platform_set_drvdata(pdev, ts);

	/* N3DS_CIRCLEPAD_TRACKBALL: registered before the touch poller starts,
	 * since the poller is what drives it. */
	cpad->name = "Android3DS Circle Pad";
	cpad->phys = DRIVER_NAME "/input1";
	cpad->id.bustype = BUS_HOST;
	cpad->dev.parent = dev;
	input_set_capability(cpad, EV_REL, REL_X);
	input_set_capability(cpad, EV_REL, REL_Y);
	/* Never pressed: it is only here because EventHub requires BTN_MOUSE
	 * before it will call a REL device a trackball. */
	input_set_capability(cpad, EV_KEY, BTN_MOUSE);
	ts->cpad = cpad;

	ret = android3ds_touch_hw_init(ts->map);
	if (ret)
		return ret;
	ret = input_register_device(cpad);
	if (ret)
		return ret;
	ret = input_setup_polling(input, android3ds_touch_poll);
	if (ret)
		return ret;
	input_set_poll_interval(input, POLL_INTERVAL_MS);
	ret = input_register_device(input);
	if (ret)
		return ret;
	if (sysfs_create_group(&input->dev.kobj, &android3ds_touch_raw_attr_group))
		pr_warn("failed to create raw calibration sysfs group\n");
	pr_info("registered pure Android direct-touch device 320x240 + circle pad trackball, probe log %s (N3DS_TOUCH_PROBE_LOG)\n",
		probe_log ? "on" : "off");
	return 0;
}

static const struct of_device_id android3ds_touch_of_match[] = {
	{ .compatible = "nintendo,android3ds-touchscreen" },
	{ }
};
MODULE_DEVICE_TABLE(of, android3ds_touch_of_match);

static struct platform_driver android3ds_touch_driver = {
	.probe = android3ds_touch_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = android3ds_touch_of_match,
	},
};
module_platform_driver(android3ds_touch_driver);
MODULE_DESCRIPTION("Android direct-touch driver for Nintendo 3DS");
MODULE_LICENSE("GPL");
