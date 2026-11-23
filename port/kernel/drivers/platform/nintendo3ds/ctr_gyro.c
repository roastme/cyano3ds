// SPDX-License-Identifier: GPL-2.0
/*
 * ctr_gyro.c - New Nintendo 3DS gyroscope (ITG-1010 or ST L3GD20H)
 *
 * The gyroscope sits on I2C bus 2 (0x10148000, the IR/gyro bus).  Which part
 * is fitted depends on the model:
 *
 *   New 3DS XL    InvenSense ITG-1010, 7-bit 0x69 (AD0 high, GBATEK 2:D2h)
 *                                     or 0x68 (AD0 low, 2:D0h)
 *   New 2DS XL    InvenSense ITG-1010, same strapping
 *   New 3DS (KTR) ST L3GD20H, 7-bit 0x6B (SA0 high, 2:D6h) or 0x6A (2:D4h).
 *                 3dbrew's i2c::HID device 9 lists 0xD6, and a bus scan of
 *                 the KTR shows exactly that address answering next to the
 *                 IR chip (2:9Ah) and ZL/ZR (2:2Ah).
 *
 * The two parts are not register compatible - the ITG-1010 keeps its output
 * in 0x43..0x48 (big-endian, WHO_AM_I in 0x75), the L3GD20H in 0x28..0x2D
 * (little-endian, WHO_AM_I in 0x0F) - so the driver identifies the part and
 * uses a per-variant register and power sequence.  Neither is compatible
 * with the ITG-3200/ITG-3270 of the Old 3DS either (whose output is at
 * 0x1D..0x22 and whose WHO_AM_I is register 0x00), which is why the mainline
 * itg3200 driver cannot bind to these boards; this driver is the small IIO
 * wrapper the Android sensors HAL reads through sysfs.
 *
 * The sensor is exposed as a normal IIO angular-velocity device:
 *
 *   /sys/bus/iio/devices/iio:deviceN/in_anglvel_{x,y,z}_raw
 *   /sys/bus/iio/devices/iio:deviceN/in_anglvel_scale      (rad/s per LSB)
 *   /sys/bus/iio/devices/iio:deviceN/in_anglvel_{x,y,z}_calibbias
 *   /sys/bus/iio/devices/iio:deviceN/in_anglvel_en          (1 = powered)
 *
 * Both parts have a zero-rate offset: a console sitting still reads a few
 * LSB on each axis, and at 70 mdps/LSB that is a couple of degrees per
 * second of drift that Android would integrate forever.  in_anglvel_*_calibbias
 * holds the offset in LSB and is subtracted from every raw read, so the
 * calibration lives in the kernel and every reader (the Android HAL, the
 * boot self-test) sees corrected values.  /bin/gyrocal measures it at boot.
 *
 * The userspace sensors HAL finds the device by its in_anglvel_* attributes,
 * so the IIO device number is irrelevant.
 *
 * Both parts are configured for +/-2000 deg/s and a ~100 Hz sample rate (the
 * ITG with a 42 Hz DLPF, the ST part with its 25 Hz filter at 100 Hz ODR).
 * The driver leaves the sensor asleep after probe and wakes it when the HAL
 * writes 1 to in_anglvel_en.
 */

#define DRIVER_NAME "3ds-gyro"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/iio/iio.h>

/* InvenSense ITG-1010: New 3DS XL, GBATEK "2:D2h"/"2:D0h" */
#define ITG1010_ID_REG			0x75
#define ITG1010_ID			0x68
#define ITG1010_OUT_XOUT_H		0x43
#define ITG1010_SMPLRT_DIV		0x19
#define ITG1010_CONFIG			0x1a
#define ITG1010_GYRO_CONFIG		0x1b
#define ITG1010_PWR_MGMT_1		0x6b
#define ITG1010_PWR_MGMT_2		0x6c

/* ST L3GD20/L3GD20H: small New 3DS, GBATEK "2:D6h"/"2:D4h" */
#define L3G_ID_REG			0x0f
#define L3GD20_ID			0xd4
#define L3GD20H_ID			0xd7
#define L3G_OUT_X_L			0x28
#define L3G_CTRL1			0x20
#define L3G_CTRL4			0x23

/* 7-bit addresses.  The gyroscope is strapped to one of two addresses by a
 * pin, so a DT node can only name one of them; the driver probes the others
 * when the address in the DT does not answer. */
#define CTR_GYRO_ADDR			0x69
#define CTR_GYRO_ADDR_ALT		0x68
#define CTR_GYRO_ADDR_L3G		0x6b

/*
 * 2000 deg/s full scale is 0.06103515625 deg/s per LSB on the ITG-1010
 * (FS_SEL = 3) and 70 mdps/digit on the ST parts (FS = 10).  In rad/s, and
 * in nanoseconds of that:
 *
 *   0.06103515625 * pi / 180 = 0.001065264... rad/s
 *   0.070 * pi / 180         = 0.001221730... rad/s
 */
#define CTR_GYRO_SCALE_NANO		1065264
#define CTR_GYRO_SCALE_NANO_L3G		1221730

struct ctr_gyro_op {
	u8 reg;
	u8 val;
	u16 delay_ms;			/* after the write */
};

struct ctr_gyro_variant {
	const char *name;		/* IIO device name */
	const char *model;		/* for log lines */
	u8 id_reg;
	u8 id_val;
	u8 out_reg;			/* first of the six output bytes */
	bool out_le;			/* ST parts are little-endian */
	int scale_nano;
	/*
	 * The ST parts only auto-increment the register address when bit 7 of the
	 * sub-address is set; without it a burst read returns the first register
	 * over and over.  InvenSense parts always increment and want bit 7 clear.
	 */
	u8 reg_incr;
	const struct ctr_gyro_op *init;
	int n_init;
	const struct ctr_gyro_op *power_on;
	int n_on;
	const struct ctr_gyro_op *power_off;
	int n_off;
};

/* Reset, then +/-2000 deg/s, DLPF at 42 Hz and 1 kHz / (9 + 1) = 100 Hz. */
static const struct ctr_gyro_op itg1010_init[] = {
	{ ITG1010_PWR_MGMT_1, 0x80, 100 },	/* device reset */
	{ ITG1010_PWR_MGMT_1, 0x01, 0 },	/* PLL with X gyro reference */
	{ ITG1010_PWR_MGMT_2, 0x00, 0 },	/* no axis in standby */
	{ ITG1010_GYRO_CONFIG, 3 << 3, 0 },	/* FS_SEL = 3 */
	{ ITG1010_CONFIG, 3, 0 },		/* DLPF_CFG = 3 */
	{ ITG1010_SMPLRT_DIV, 9, 30 },
};

/*
 * Sleep before putting the axes in standby: standby with the gyro PLL as
 * clock source leaves the chip without a clock.
 */
static const struct ctr_gyro_op itg1010_off[] = {
	{ ITG1010_PWR_MGMT_1, 0x40, 0 },	/* sleep */
	{ ITG1010_PWR_MGMT_2, 0x07, 0 },	/* all axes in standby */
};

/* All three axes must be enabled at once, so clear standby before the clock. */
static const struct ctr_gyro_op itg1010_on[] = {
	{ ITG1010_PWR_MGMT_2, 0x00, 0 },
	{ ITG1010_PWR_MGMT_1, 0x01, 30 },	/* PLL start-up */
};

/*
 * CTRL1 = 0x1f: normal mode, all axes, ODR 100 Hz (DR = 00, LOW_ODR = 0),
 * 25 Hz filter (BW = 01); CTRL4 = 0xa0: block data update, +/-2000 deg/s.
 */
static const struct ctr_gyro_op l3gd20h_init[] = {
	{ L3G_CTRL4, 0xa0, 0 },
	{ L3G_CTRL1, 0x1f, 30 },
};

static const struct ctr_gyro_op l3gd20h_off[] = {
	{ L3G_CTRL1, 0x00, 0 },			/* power-down */
};

static const struct ctr_gyro_op l3gd20h_on[] = {
	{ L3G_CTRL1, 0x1f, 30 },
};

static const struct ctr_gyro_variant ctr_gyro_variants[] = {
	{
		.name = "itg1010",
		.model = "ITG-1010",
		.id_reg = ITG1010_ID_REG,
		.id_val = ITG1010_ID,
		.out_reg = ITG1010_OUT_XOUT_H,
		.out_le = false,
		.reg_incr = 0x00,
		.scale_nano = CTR_GYRO_SCALE_NANO,
		.init = itg1010_init,
		.n_init = ARRAY_SIZE(itg1010_init),
		.power_on = itg1010_on,
		.n_on = ARRAY_SIZE(itg1010_on),
		.power_off = itg1010_off,
		.n_off = ARRAY_SIZE(itg1010_off),
	},
	{
		.name = "l3gd20h",
		.model = "L3GD20H",
		.id_reg = L3G_ID_REG,
		.id_val = L3GD20H_ID,
		.out_reg = L3G_OUT_X_L,
		.out_le = true,
		.reg_incr = 0x80,
		.scale_nano = CTR_GYRO_SCALE_NANO_L3G,
		.init = l3gd20h_init,
		.n_init = ARRAY_SIZE(l3gd20h_init),
		.power_on = l3gd20h_on,
		.n_on = ARRAY_SIZE(l3gd20h_on),
		.power_off = l3gd20h_off,
		.n_off = ARRAY_SIZE(l3gd20h_off),
	},
};

struct ctr_gyro {
	struct i2c_client *client;
	const struct ctr_gyro_variant *var;
	struct mutex lock;		/* register access + enabled state */
	bool enabled;
	bool resume_enabled;		/* was enabled before system suspend */
	s16 calib[3];			/* zero-rate offset per axis, in LSB */
};

static int ctr_gyro_read_addr(struct i2c_adapter *adap, u16 addr, u16 flags,
			      u8 reg, void *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{
			.addr = addr,
			.flags = flags,
			.len = 1,
			.buf = (char *)&reg,
		},
		{
			.addr = addr,
			.flags = flags | I2C_M_RD,
			.len = len,
			.buf = (char *)buf,
		},
	};
	int ret;

	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret == ARRAY_SIZE(msgs))
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

static int ctr_gyro_read(struct ctr_gyro *gyro, u8 reg, void *buf, int len)
{
	struct i2c_client *client = gyro->client;

	return ctr_gyro_read_addr(client->adapter, client->addr,
				  client->flags, reg | gyro->var->reg_incr,
				  buf, len);
}

static int ctr_gyro_write(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = client->flags,
		.len = sizeof(buf),
		.buf = (char *)buf,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

/*
 * Look for a supported gyroscope at @addr: 0 on a match (with *var and the ID
 * byte read), -ENXIO when nothing acknowledges the address at all, -ENODEV
 * when something does but its ID is not one of ours.
 */
static int ctr_gyro_detect(struct i2c_adapter *adap, u16 addr, u16 flags,
			   const struct ctr_gyro_variant **var, u8 *id)
{
	const struct ctr_gyro_variant *v;
	u8 val;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ctr_gyro_variants); i++) {
		v = &ctr_gyro_variants[i];
		ret = ctr_gyro_read_addr(adap, addr, flags, v->id_reg, &val, 1);
		if (ret)
			return ret;
		*id = val;
		if (val == v->id_val) {
			*var = v;
			return 0;
		}
	}

	return -ENODEV;
}

static int ctr_gyro_run_ops(struct ctr_gyro *gyro,
			    const struct ctr_gyro_op *ops, int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		ret = ctr_gyro_write(gyro->client, ops[i].reg, ops[i].val);
		if (ret)
			return ret;
		if (ops[i].delay_ms)
			msleep(ops[i].delay_ms);
	}

	return 0;
}

static int ctr_gyro_set_power(struct ctr_gyro *gyro, bool on)
{
	const struct ctr_gyro_variant *v = gyro->var;
	int ret;

	if (on)
		ret = ctr_gyro_run_ops(gyro, v->power_on, v->n_on);
	else
		ret = ctr_gyro_run_ops(gyro, v->power_off, v->n_off);
	if (ret)
		return ret;

	gyro->enabled = on;
	return 0;
}

/*
 * Reset and configure the chip.  Called once from probe (the caller has
 * already checked the ID and filled in gyro->var); the register settings
 * survive the sleep/wake sequence in ctr_gyro_set_power().
 */
static int ctr_gyro_configure(struct ctr_gyro *gyro)
{
	return ctr_gyro_run_ops(gyro, gyro->var->init, gyro->var->n_init);
}

static int ctr_gyro_read_xyz(struct ctr_gyro *gyro, s16 data[3])
{
	u8 raw[6];
	int i, ret;

	ret = ctr_gyro_read(gyro, gyro->var->out_reg, raw, sizeof(raw));
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		if (gyro->var->out_le)
			data[i] = (s16)(raw[i * 2] | (raw[i * 2 + 1] << 8));
		else
			data[i] = (s16)((raw[i * 2] << 8) | raw[i * 2 + 1]);
	}

	return 0;
}

/*
 * Bring-up check: wake the part, take two samples 80 ms apart and log them
 * raw.  At rest both should be small (a few deg/s of noise) and close to each
 * other; that proves the power sequence, the output register base and the
 * byte order without needing userspace to enable anything.
 * ctr_gyro.selftest=0 disables it.
 */
static bool selftest = true;
module_param(selftest, bool, 0444);

static void ctr_gyro_selftest(struct ctr_gyro *gyro)
{
	s16 a[3], b[3];
	int ret;

	ret = ctr_gyro_set_power(gyro, true);
	if (ret) {
		dev_err(&gyro->client->dev, "self-test: cannot power up (%d)\n",
			ret);
		return;
	}

	msleep(80);
	ret = ctr_gyro_read_xyz(gyro, a);
	if (!ret) {
		msleep(80);
		ret = ctr_gyro_read_xyz(gyro, b);
	}
	if (ret)
		dev_err(&gyro->client->dev, "self-test: sample read failed (%d)\n",
			ret);
	else
		dev_info(&gyro->client->dev,
			 "self-test: raw %d %d %d then %d %d %d LSB (scale %d nano-rad/s/LSB)\n",
			a[0], a[1], a[2], b[0], b[1], b[2],
			gyro->var->scale_nano);

	ctr_gyro_set_power(gyro, false);
}

static int ctr_gyro_read_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int *val, int *val2, long mask)
{
	struct ctr_gyro *gyro = iio_priv(indio_dev);
	s16 data[3];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&gyro->lock);
		if (!gyro->enabled) {
			mutex_unlock(&gyro->lock);
			return -EBUSY;
		}
		ret = ctr_gyro_read_xyz(gyro, data);
		mutex_unlock(&gyro->lock);
		if (ret)
			return ret;
		/* Subtract the zero-rate offset measured by /bin/gyrocal. */
		*val = (int)data[chan->address] -
		       (int)gyro->calib[chan->address];
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_CALIBBIAS:
		*val = gyro->calib[chan->address];
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = gyro->var->scale_nano;
		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_ENABLE:
		mutex_lock(&gyro->lock);
		*val = gyro->enabled;
		mutex_unlock(&gyro->lock);
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int ctr_gyro_write_raw(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan, int val,
			      int val2, long mask)
{
	struct ctr_gyro *gyro = iio_priv(indio_dev);
	int ret;

	if (mask == IIO_CHAN_INFO_CALIBBIAS) {
		mutex_lock(&gyro->lock);
		gyro->calib[chan->address] = (s16)clamp(val, -32768, 32767);
		mutex_unlock(&gyro->lock);
		return 0;
	}

	if (mask != IIO_CHAN_INFO_ENABLE)
		return -EINVAL;

	mutex_lock(&gyro->lock);
	if (!!val == gyro->enabled)
		ret = 0;
	else
		ret = ctr_gyro_set_power(gyro, !!val);
	mutex_unlock(&gyro->lock);

	return ret;
}

#define CTR_GYRO_CHANNEL(_axis, _index)					\
	{								\
		.type = IIO_ANGL_VEL,					\
		.modified = 1,						\
		.channel2 = IIO_MOD_##_axis,				\
		.address = (_index),					\
		.scan_type = {						\
			.sign = 's',					\
			.realbits = 16,					\
			.storagebits = 16,				\
			.endianness = IIO_BE,				\
		},							\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_CALIBBIAS),	\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |	\
					    BIT(IIO_CHAN_INFO_ENABLE),	\
	}

static const struct iio_chan_spec ctr_gyro_channels[] = {
	CTR_GYRO_CHANNEL(X, 0),
	CTR_GYRO_CHANNEL(Y, 1),
	CTR_GYRO_CHANNEL(Z, 2),
};

static const struct iio_info ctr_gyro_info = {
	.read_raw = ctr_gyro_read_raw,
	.write_raw = ctr_gyro_write_raw,
};

/*
 * The part answers at one of two addresses, selected by a strap pin, but a DT
 * node can only name one.  When this client's address does not answer, look
 * at the others and, if the part is there, ask the i2c core for a client at
 * that address: the core probes it and binds this driver to it, so the whole
 * setup runs against the right address.  Once is enough - the client created
 * here fails its own probe without recursing.
 */
static bool ctr_gyro_alt_probed;

/*
 * Bring-up diagnostic: when no gyroscope answers, say which addresses on
 * which bus do answer.  A sensor that is missing, strapped to an unexpected
 * address or not powered then shows up in one boot instead of a guess per
 * flash.  It only runs when detection failed, uses an address-only (SMBus
 * quick) write that no device acts on, and ctr_gyro.scan_bus=0 disables it.
 */
static bool scan_bus = true;
module_param(scan_bus, bool, 0444);

static void ctr_gyro_scan_buses(struct device *dev)
{
	struct i2c_adapter *adap;
	int bus, addr;

	for (bus = 0; bus < 16; bus++) {
		adap = i2c_get_adapter(bus);
		if (!adap)
			continue;
		dev_info(dev, "i2c-%d (%s):\n", bus, adap->name);
		for (addr = 0x03; addr < 0x78; addr++) {
			if (i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_QUICK,
					   I2C_SMBUS_WRITE, 0, NULL) == 0)
				dev_info(dev, "  i2c-%d answers at 0x%02x\n",
					 bus, addr);
		}
		i2c_put_adapter(adap);
	}
}

static const u16 ctr_gyro_probe_addrs[] = {
	CTR_GYRO_ADDR,
	CTR_GYRO_ADDR_ALT,
	CTR_GYRO_ADDR_L3G,
};

static bool ctr_gyro_addr_acks(struct i2c_adapter *adap, u16 addr, u16 flags)
{
	return i2c_smbus_xfer(adap, addr, flags, I2C_SMBUS_QUICK,
			      I2C_SMBUS_WRITE, 0, NULL) == 0;
}

/*
 * Describe whatever is sitting at @addr: each supported part's ID register,
 * then the raw sample registers of both families.  If the samples track the
 * console it is the gyroscope, whatever its ID byte claims.
 */
static void ctr_gyro_identify(struct device *dev, struct i2c_adapter *adap,
			      u16 flags, u16 addr)
{
	const struct ctr_gyro_variant *v;
	u8 id = 0, blk[8];
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ctr_gyro_variants); i++) {
		v = &ctr_gyro_variants[i];
		ret = ctr_gyro_read_addr(adap, addr, flags, v->id_reg, &id, 1);
		if (!ret)
			dev_info(dev, "  0x%02x: ID reg 0x%02x = 0x%02x%s\n",
				 addr, v->id_reg, id,
				 id == v->id_val ? " (matches)" : "");
	}

	ret = ctr_gyro_read_addr(adap, addr, flags, ITG1010_OUT_XOUT_H, blk, 6);
	if (!ret)
		dev_info(dev, "  0x%02x: regs 0x43..0x48: %6ph\n", addr, blk);

	/* 0x80 sets the ST auto-increment bit in the sub-address. */
	ret = ctr_gyro_read_addr(adap, addr, flags, 0x80 | L3G_OUT_X_L, blk, 6);
	if (!ret)
		dev_info(dev, "  0x%02x: regs 0x28..0x2d: %6ph\n", addr, blk);

	ret = ctr_gyro_read_addr(adap, addr, flags, 0x80 | L3G_CTRL1, blk, 8);
	if (!ret)
		dev_info(dev, "  0x%02x: regs 0x20..0x27: %8ph\n", addr, blk);
}

static int ctr_gyro_try_alt_addr(struct i2c_client *client)
{
	struct i2c_board_info info;
	struct i2c_client *alt;
	const struct ctr_gyro_variant *var = NULL;
	u16 found = 0;
	u8 id = 0;
	int i, ret = -ENODEV, known = 0;

	if (ctr_gyro_alt_probed)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(ctr_gyro_probe_addrs); i++)
		if (ctr_gyro_probe_addrs[i] == client->addr)
			known = 1;
	if (!known)
		return -ENODEV;
	ctr_gyro_alt_probed = true;

	for (i = 0; i < ARRAY_SIZE(ctr_gyro_probe_addrs); i++) {
		u16 addr = ctr_gyro_probe_addrs[i];

		if (addr == client->addr)
			continue;
		if (!ctr_gyro_detect(client->adapter, addr, client->flags,
				     &var, &id)) {
			found = addr;
			break;
		}
	}

	if (!found) {
		dev_err(&client->dev,
			"no gyroscope on bus %d: nothing at 0x%02x, 0x%02x or 0x%02x\n",
			client->adapter->nr, CTR_GYRO_ADDR, CTR_GYRO_ADDR_ALT,
			CTR_GYRO_ADDR_L3G);
		for (i = 0; i < ARRAY_SIZE(ctr_gyro_probe_addrs); i++) {
			u16 addr = ctr_gyro_probe_addrs[i];

			if (addr == client->addr ||
			    !ctr_gyro_addr_acks(client->adapter, addr,
						client->flags))
				continue;
			dev_info(&client->dev, "0x%02x answers, but not as a known gyro:\n",
				 addr);
			ctr_gyro_identify(&client->dev, client->adapter,
					  client->flags, addr);
		}
		if (scan_bus)
			ctr_gyro_scan_buses(&client->dev);
		return ret;
	}

	memset(&info, 0, sizeof(info));
	info.addr = found;
	strscpy(info.type, DRIVER_NAME, sizeof(info.type));
	alt = i2c_new_client_device(client->adapter, &info);
	if (IS_ERR(alt)) {
		dev_err(&client->dev, "cannot add a client at 0x%02x: %ld\n",
			found, PTR_ERR(alt));
		return PTR_ERR(alt);
	}

	dev_info(&client->dev,
		 "%s answers at 0x%02x, not 0x%02x: using 0x%02x\n",
		 var->model, found, client->addr, found);
	return -ENODEV;
}

static int ctr_gyro_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	const struct ctr_gyro_variant *var = NULL;
	struct ctr_gyro *gyro;
	struct iio_dev *indio_dev;
	u8 id_byte = 0;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*gyro));
	if (!indio_dev)
		return -ENOMEM;

	gyro = iio_priv(indio_dev);
	gyro->client = client;
	mutex_init(&gyro->lock);
	i2c_set_clientdata(client, indio_dev);

	ret = ctr_gyro_detect(client->adapter, client->addr, client->flags,
			      &var, &id_byte);
	if (ret) {
		if (ret == -ENODEV) {
			dev_info(&client->dev,
				 "0x%02x answers, but its ID byte is 0x%02x, not a supported gyro\n",
				 client->addr, id_byte);
			ctr_gyro_identify(&client->dev, client->adapter,
					  client->flags, client->addr);
		} else {
			dev_dbg(&client->dev,
				"nothing at 0x%02x on bus %d (%d)\n",
				client->addr, client->adapter->nr, ret);
		}
		return ctr_gyro_try_alt_addr(client);
	}
	gyro->var = var;

	ret = ctr_gyro_configure(gyro);
	if (ret) {
		dev_err(&client->dev,
			"%s at 0x%02x did not accept its configuration (%d)\n",
			var->model, client->addr, ret);
		return ret;
	}

	if (selftest)
		ctr_gyro_selftest(gyro);

	/* The HAL turns the sensor on when an app requests it. */
	ret = ctr_gyro_set_power(gyro, false);
	if (ret)
		return ret;

	indio_dev->name = var->name;
	indio_dev->info = &ctr_gyro_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ctr_gyro_channels;
	indio_dev->num_channels = ARRAY_SIZE(ctr_gyro_channels);

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret)
		return ret;

	dev_info(&client->dev,
		 "%s ready on bus %d addr 0x%02x (+/-2000 dps, 100 Hz)\n",
		 var->model, client->adapter->nr, client->addr);
	return 0;
}

static int ctr_gyro_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ctr_gyro *gyro = iio_priv(indio_dev);

	mutex_lock(&gyro->lock);
	ctr_gyro_set_power(gyro, false);
	mutex_unlock(&gyro->lock);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ctr_gyro_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ctr_gyro *gyro = iio_priv(indio_dev);
	int ret = 0;

	mutex_lock(&gyro->lock);
	gyro->resume_enabled = gyro->enabled;
	if (gyro->enabled)
		ret = ctr_gyro_set_power(gyro, false);
	mutex_unlock(&gyro->lock);
	return ret;
}

static int ctr_gyro_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ctr_gyro *gyro = iio_priv(indio_dev);
	int ret = 0;

	mutex_lock(&gyro->lock);
	if (gyro->resume_enabled)
		ret = ctr_gyro_set_power(gyro, true);
	mutex_unlock(&gyro->lock);
	return ret;
}
#endif

static SIMPLE_DEV_PM_OPS(ctr_gyro_pm_ops, ctr_gyro_suspend, ctr_gyro_resume);

static const struct i2c_device_id ctr_gyro_id[] = {
	{ "3ds-gyro", 0 },
	{ "itg1010", 0 },
	{ "l3gd20h", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ctr_gyro_id);

static const struct of_device_id ctr_gyro_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{ .compatible = "invensense,itg1010" },
	{ .compatible = "st,l3gd20h" },
	{ }
};
MODULE_DEVICE_TABLE(of, ctr_gyro_of_match);

static struct i2c_driver ctr_gyro_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = &ctr_gyro_pm_ops,
		.of_match_table = of_match_ptr(ctr_gyro_of_match),
	},
	.probe = ctr_gyro_probe,
	.remove = ctr_gyro_remove,
	.id_table = ctr_gyro_id,
};
module_i2c_driver(ctr_gyro_driver);

MODULE_DESCRIPTION("Nintendo New 3DS gyroscope (ITG-1010 on 2:D2h/D0h, L3GD20H on 2:D6h)");
MODULE_AUTHOR("Android 3DS port");
MODULE_LICENSE("GPL");
