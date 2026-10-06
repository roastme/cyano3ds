// SPDX-License-Identifier: GPL-2.0
/*
 * ctr_extrapad.c - New Nintendo 3DS ZL/ZR buttons (I2C 2:0x54)
 *
 * The New 3DS adds ZL, ZR and the C-stick.  They are NOT in the HID_PAD
 * register (0x10146000) - there bits 14/15 are the IRQ-enable/condition bits.
 * The extra buttons are on an I2C device at 7-bit address 0x2A on **bus 2**,
 * i.e. I2C_BUS2 = 0x10148000 (GBATEK "2:54h" = 0x54/2 = 0x2A).  That is the
 * IR + gyro bus, NOT the MCU bus (the MCU is on bus 1 = 0x10144000).  The
 * `hid` sysmodule reads this chip and republishes ZL/ZR through the ir:rst
 * shared memory [1].
 *
 * NOTE: on the wrong bus address 0x2A does not ACK and every read returns
 * FF FF; `ctr_i2c` does not report that NACK to the i2c core, so the driver
 * checks for the all-ones reply and warns instead of reporting phantom
 * button presses.
 *
 * The chip is read with a plain I2C read (no register address): the first
 * returned byte is a status value (0x80..0x83), the second is the button byte,
 * ZL = bit 2 and ZR = bit 1 [1].  Reading more than 0x1A bytes makes the chip
 * hang, so only 2 bytes are read.  `ctr_i2c` already configures I2C_BUS2_CNTEX
 * ("wait if SCL held low"), which the bootrom does not.
 *
 * Only **ZR** is reported as KEY_SEARCH and **ZL** as KEY_MENU; Android's
 * default qwerty.kl maps those scancodes to the SEARCH and MENU keycodes.
 * (The physical Home button is now KEY_HOME - fix-mcu-home-button.py - so the
 * MENU key had to move off it; ZL is the natural place for it.)
 *
 * [1] http://problemkaputt.de/gbatek-3ds-i2c-new3ds-c-stick-and-zl-zr-buttons.htm
 */

#define DRIVER_NAME "3ds-extrapad"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/* button byte of the 2:0x2A read; ZL -> MENU, ZR -> SEARCH */
#define ZL_BIT		0x04	/* ZL is reported as KEY_MENU */
#define ZR_BIT		0x02	/* ZR is reported as KEY_SEARCH */

#define POLL_MS		20

struct ctr_extrapad {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	bool zl;
	bool zr;
	bool first;
	unsigned errors;
};

static void ctr_extrapad_poll(struct work_struct *work)
{
	struct ctr_extrapad *ep = container_of(to_delayed_work(work),
					       struct ctr_extrapad, work);
	u8 buf[2];
	int err;

	err = i2c_master_recv(ep->client, buf, sizeof(buf));
	if (err == sizeof(buf)) {
		bool zl, zr;

		/*
		 * An all-ones reply means nothing responded on this bus (the
		 * ctr_i2c driver does not report an address NACK), e.g. when the
		 * DT node is attached to the wrong I2C bus.  Do not inject
		 * MENU/SEARCH in that case.
		 */
		if (buf[0] == 0xff && buf[1] == 0xff) {
			if (++ep->errors == 1 || (ep->errors % 500) == 0)
				dev_warn(&ep->client->dev,
					 "all-ones reply (raw ff ff): is this node on the right I2C bus?"
					 " ZL/ZR is on 0x10148000 (bus 2), not 0x10144000\n");
			schedule_delayed_work(&ep->work, msecs_to_jiffies(POLL_MS));
			return;
		}

		zl = !!(buf[1] & ZL_BIT);
		zr = !!(buf[1] & ZR_BIT);

		if (ep->first) {
			ep->first = false;
			dev_info(&ep->client->dev,
				 "first read: raw %02x %02x\n", buf[0], buf[1]);
		}

		if (zl != ep->zl) {
			ep->zl = zl;
			input_report_key(ep->input, KEY_MENU, zl);
			input_sync(ep->input);
			dev_info(&ep->client->dev,
				 "ZL=%d (raw %02x %02x)\n", zl, buf[0], buf[1]);
		}

		if (zr != ep->zr) {
			ep->zr = zr;
			input_report_key(ep->input, KEY_SEARCH, zr);
			input_sync(ep->input);
			dev_info(&ep->client->dev,
				 "ZR=%d (raw %02x %02x)\n", zr, buf[0], buf[1]);
		}
	} else if (++ep->errors == 1 || (ep->errors % 500) == 0) {
		dev_warn(&ep->client->dev, "read failed: %d\n", err);
	}

	schedule_delayed_work(&ep->work, msecs_to_jiffies(POLL_MS));
}

static int ctr_extrapad_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct ctr_extrapad *ep;
	int err;

	ep = devm_kzalloc(&client->dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;
	ep->client = client;
	ep->first = true;

	ep->input = devm_input_allocate_device(&client->dev);
	if (!ep->input)
		return -ENOMEM;
	ep->input->name = "nintendo3ds-extra";
	ep->input->phys = "nintendo3ds/extra";
	ep->input->id.bustype = BUS_I2C;
	input_set_capability(ep->input, EV_KEY, KEY_MENU);
	input_set_capability(ep->input, EV_KEY, KEY_SEARCH);

	err = input_register_device(ep->input);
	if (err)
		return err;

	i2c_set_clientdata(client, ep);
	INIT_DELAYED_WORK(&ep->work, ctr_extrapad_poll);
	schedule_delayed_work(&ep->work, msecs_to_jiffies(500));

	dev_info(&client->dev, "New 3DS ZL/ZR reader ready (bus %d addr 0x%02x, ZL=MENU ZR=SEARCH)\n",
		 client->adapter->nr, client->addr);
	return 0;
}

static int ctr_extrapad_remove(struct i2c_client *client)
{
	struct ctr_extrapad *ep = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&ep->work);
	return 0;
}

static const struct of_device_id ctr_extrapad_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_extrapad_of_match);

static struct i2c_driver ctr_extrapad_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_match_ptr(ctr_extrapad_of_match),
	},
	.probe = ctr_extrapad_probe,
	.remove = ctr_extrapad_remove,
};
module_i2c_driver(ctr_extrapad_driver);

MODULE_DESCRIPTION("Nintendo New 3DS ZL/ZR buttons (I2C 2:0x54), ZL=MENU ZR=SEARCH");
MODULE_AUTHOR("Android 3DS port");
MODULE_LICENSE("GPL");
