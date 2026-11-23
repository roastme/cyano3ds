#!/usr/bin/env python3
"""
fix-i2c-ack.py - make ctr_i2c.c report (and recover from) a missing device

ctr_i2c.c (linux-3ds) has three problems that make a wrong or missing I2C
address invisible instead of an error:

  1. the ACK bit is only looked at after data bytes, never after the address
     byte, so a read from an address nobody answers returns FF FF ... and
     looks like a successful transfer.  That is why the ZL/ZR chip, the
     gyroscope and i2cdetect all "worked" on addresses with no device.
  2. a NAK aborts with BUSY|IRQEN|PAUSE but without STOP, so the controller
     stays paused mid-byte and the bus is never released.  The ARM9 firmware
     ends a failed transfer with STOP|PAUSE (Luma3DS i2c.c: ENABLE |
     IRQ_ENABLE | I2C_ERROR | I2C_STOP); do the same.
  3. a zero-length message (SMBus quick command) sends the address and then
     nothing - no STOP - which leaves the bus busy.

It also advertises the emulated SMBus transfers, so busybox i2cdetect /
i2cget / i2cset can be used to look at a bus from the bring-up console.

Idempotent: running it twice changes nothing.

Usage: fix-i2c-ack.py [kernel-tree]
"""

import pathlib
import sys

KD = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                  pathlib.Path.home() / "p3ds/src/linux-3ds")
SRC = KD / "drivers/platform/nintendo3ds/ctr_i2c.c"

OLD_SELECT = '''static int ctr_i2c_select_device(struct ctr_i2c *i2c, struct i2c_msg *msg)
{
	return ctr_i2c_send(i2c, i2c_8bit_addr_from_msg(msg), I2C_CNT_START);
}
'''

NEW_SELECT = '''/*
 * A device that does not acknowledge leaves the controller paused in the
 * middle of a byte.  The ARM9 firmware ends such a transfer with STOP|PAUSE
 * and so must we: without the STOP the bus is never released and the next
 * transfer on it fails too.
 */
static int ctr_i2c_abort(struct ctr_i2c *i2c)
{
	ctr_i2c_write_cnt(i2c, I2C_CNT_BUSY | I2C_CNT_IRQEN |
			       I2C_CNT_PAUSE | I2C_CNT_LAST);
	ctr_i2c_wait_busy(i2c);
	return -ENXIO;
}

static int ctr_i2c_select_device(struct ctr_i2c *i2c, struct i2c_msg *msg,
				 bool stop)
{
	int err = ctr_i2c_send(i2c, i2c_8bit_addr_from_msg(msg),
			       I2C_CNT_START | (stop ? I2C_CNT_LAST : 0));

	if (err)
		return err;
	/* ERRACK is set when the addressed device acknowledged its address. */
	if (!(ctr_i2c_read_cnt(i2c) & I2C_CNT_ERRACK))
		return ctr_i2c_abort(i2c);
	return 0;
}
'''

OLD_WRITE = '''static int ctr_i2c_msg_write(struct ctr_i2c *i2c, u8 *buf, int len, bool last)
{
	int i;
	for (i = 0; i < len; i++) {
		unsigned flag = (last && (i == (len - 1))) ? I2C_CNT_LAST : 0;
		if (ctr_i2c_send(i2c, buf[i], flag))
			return i;

		if (!(ctr_i2c_read_cnt(i2c) & I2C_CNT_ERRACK)) {
			ctr_i2c_write_cnt(i2c, I2C_CNT_BUSY | I2C_CNT_IRQEN |
						       I2C_CNT_PAUSE |
						       I2C_CNT_WRITE);
			return i;
		}
	}
	return len;
}
'''

NEW_WRITE = '''static int ctr_i2c_msg_write(struct ctr_i2c *i2c, u8 *buf, int len, bool last)
{
	int i;
	for (i = 0; i < len; i++) {
		unsigned flag = (last && (i == (len - 1))) ? I2C_CNT_LAST : 0;
		if (ctr_i2c_send(i2c, buf[i], flag))
			return i;

		if (!(ctr_i2c_read_cnt(i2c) & I2C_CNT_ERRACK))
			return i ? i : ctr_i2c_abort(i2c);
	}
	return len;
}
'''

OLD_XFER = '''	int i, plen;
	struct i2c_msg *msg;
	struct ctr_i2c *i2c = container_of(adap, struct ctr_i2c, adap);

	if (!num)
		return 0;

	for (i = 0; i < num; i++) {
		msg = &msgs[i];

		if (!(msg->flags & I2C_M_NOSTART))
			ctr_i2c_select_device(i2c, msg);

		if (msg->len != 0) {
			bool last = i == (num - 1);
			if (msg->flags & I2C_M_RD) {
				plen = ctr_i2c_msg_read(i2c, msg->buf, msg->len,
							last);
			} else {
				plen = ctr_i2c_msg_write(i2c, msg->buf,
							 msg->len, last);
			}

			if (plen != msg->len)
				break;
		}

		msg++;
	}

	return i;
}
'''

NEW_XFER = '''	int i, plen, err;
	struct i2c_msg *msg;
	struct ctr_i2c *i2c = container_of(adap, struct ctr_i2c, adap);

	if (!num)
		return 0;

	for (i = 0; i < num; i++) {
		msg = &msgs[i];

		/*
		 * A zero-length message is an SMBus quick command: address the
		 * device and stop.
		 */
		if (!(msg->flags & I2C_M_NOSTART)) {
			err = ctr_i2c_select_device(i2c, msg, msg->len == 0);
			if (err)
				return i ? i : err;
		}

		if (msg->len != 0) {
			bool last = i == (num - 1);
			if (msg->flags & I2C_M_RD) {
				plen = ctr_i2c_msg_read(i2c, msg->buf, msg->len,
							last);
			} else {
				plen = ctr_i2c_msg_write(i2c, msg->buf,
							 msg->len, last);
			}

			if (plen != msg->len)
				return i ? i : (plen < 0 ? plen : -EIO);
		}
	}

	return num;
}
'''

OLD_FUNC = '''static u32 ctr_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART;
}
'''

NEW_FUNC = '''static u32 ctr_i2c_functionality(struct i2c_adapter *adap)
{
	/*
	 * The controller does raw I2C only, but the i2c core emulates SMBus
	 * transfers on top of master_xfer.  Advertising that (as i2c-gpio
	 * does) makes i2cdetect/i2cget/i2cset usable for bring-up.
	 */
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_EMUL;
}
'''


def main():
    if not SRC.is_file():
        sys.exit(f"fix-i2c-ack: no ctr_i2c.c at {SRC}")

    s = SRC.read_text()
    if "ctr_i2c_abort" in s and "I2C_FUNC_SMBUS_EMUL" in s:
        print("    ctr_i2c.c: ACK handling already fixed")
        return

    for old, new in ((OLD_SELECT, NEW_SELECT),
                     (OLD_WRITE, NEW_WRITE),
                     (OLD_XFER, NEW_XFER),
                     (OLD_FUNC, NEW_FUNC)):
        if old not in s:
            sys.exit("fix-i2c-ack: ctr_i2c.c does not match the expected "
                     "linux-3ds source (already patched differently?)")
        s = s.replace(old, new, 1)

    SRC.write_text(s)
    print("    ctr_i2c.c: NAK is now -ENXIO + STOP|PAUSE, quick commands stop, "
          "emulated SMBus advertised")


if __name__ == "__main__":
    main()
