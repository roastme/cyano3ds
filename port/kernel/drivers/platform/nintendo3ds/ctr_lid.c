// SPDX-License-Identifier: GPL-2.0
/*
 * ctr_lid.c - Nintendo 3DS shell (lid) switch: sleep mode on lid close
 *
 * Part of the Android port to the Nintendo 3DS.
 *
 * Closing the shell puts the console to sleep and opening it wakes it, like
 * the 3DS system software and open_agb_firm's lid sleep (Dominik Leicht,
 * profi200/open_agb_firm PR 295).  The MCU raises "shell closed" (bit 5) and
 * "shell opened" (bit 6) interrupts and reports the current state in bit 1 of
 * register 0x0F (set while open).
 *
 * On close this driver suspends the system (pm_suspend(PM_SUSPEND_MEM)); the
 * platform code in arch/arm/mach-ctr/pm.c then sleeps until the MCU
 * interrupt line says "shell opened".  By default that is a screen-off sleep
 * (backlights off, CPU in WFI); with ctr_deep_sleep=1 on the kernel command
 * line it quiesces the graphics hardware and enters whole-system PDN sleep
 * (experimental: the first real PDN sleep on hardware did not come back).
 *
 * Its suspend/resume callbacks do the MCU part of the sleep sequence, which
 * needs I2C and so has to run before the platform code:
 *   suspend: MCU interrupts masked down to "shell opened", backlights off,
 *            (deep sleep only: LCDs off), power LED "sleep" (breathing)
 *            pattern, pending MCU interrupts cleared, (deep sleep only:
 *            MCU_REG_SYS_PWR bit 4, "entering sleep");
 *   resume:  power LED back to automatic, (deep sleep only: LCDs on),
 *            backlights on, MCU interrupt mask restored.
 * They run for every suspend, including "echo mem > /sys/power/state".
 *
 * The masking matters on real hardware: the MCU acknowledges LCD power and
 * backlight changes with interrupts (bits 24-29), a little after the write.
 * The kernel's MCU interrupt controller does not know those bits, so they stay
 * unmasked; they arrive after the last check in pm.c and would wake the
 * console a few milliseconds into every sleep (wake reason "MCU", one
 * sleep/wake cycle every ~0.7 s while the lid was closed).  The emulator never
 * raises them.  open_agb_firm avoids this the same way: its MCU mask leaves
 * only the events it handles, and power_sleep() clears pending ones last.
 *
 * Before the first sleep of each lid close the driver can run a userspace
 * helper (sysfs "sleep_hook", e.g. the /data flush of the Android port) and
 * waits for it to finish.  The helper must bound its own run time.
 *
 * The lid is deliberately NOT reported to Android as SW_LID: Gingerbread
 * treats SW_LID as a keyboard slider (PhoneWindowManager: lid open forces
 * config_lidOpenRotation and the hard keyboard state), not as a sleep
 * trigger.  State and control are in sysfs instead:
 *   closed          (ro) 1 while the shell is closed
 *   sleep_on_close  (rw) 1 arms lid sleep; 0 at boot, the bring-up init
 *                   arms it 15 s after Android has finished booting
 *   sleep_hook      (rw) absolute path of a program to run before sleeping
 *                        on lid close (empty: none)
 */

#define DRIVER_NAME "3dsmcu-lid"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/umh.h>
#include <linux/workqueue.h>

#define MCU_REG_POWER_STATE	0x0F
#define  MCU_POWER_SHELL_OPEN	BIT(1)
#define MCU_REG_IRQ_STAT	0x10	/* 4 bytes, cleared on read */
#define MCU_REG_IRQ_MASK	0x18	/* 4 bytes, 1 = masked */
#define  MCU_IRQ_SHELL_OPEN	BIT(6)
#define MCU_IRQ_REGS		4
#define MCU_REG_SYS_PWR		0x20
#define  MCU_SYS_PWR_SLEEP	BIT(4)
#define MCU_REG_LCD_PWR		0x22
#define  MCU_LCD_PWR_OFF	BIT(0)
#define  MCU_LCD_PWR_ON		BIT(1)
#define  MCU_LCD_BOT_BL_OFF	BIT(2)
#define  MCU_LCD_BOT_BL_ON	BIT(3)
#define  MCU_LCD_TOP_BL_OFF	BIT(4)
#define  MCU_LCD_TOP_BL_ON	BIT(5)
#define MCU_REG_PWR_LED		0x29
#define  MCU_PWR_LED_AUTO	0
#define  MCU_PWR_LED_SLEEP	2

/* arch/arm/mach-ctr/pm.c: whole-system PDN sleep rather than screen-off sleep */
bool ctr_pm_sleep_is_deep(void);

/* log only the first few refused suspends of one lid close */
#define LID_SLEEP_WARNINGS	5

struct ctr_lid {
	struct device *dev;
	struct regmap *map;
	struct work_struct sleep_work;
	struct mutex hook_lock;
	char sleep_hook[128];
	int irq_close, irq_open;
	bool closed;
	bool sleep_on_close;
	bool irq_mask_saved;
	u8 irq_mask[MCU_IRQ_REGS];
};

static bool ctr_lid_read_closed(struct ctr_lid *lid)
{
	unsigned int v;

	if (regmap_read(lid->map, MCU_REG_POWER_STATE, &v))
		return READ_ONCE(lid->closed);
	return !(v & MCU_POWER_SHELL_OPEN);
}

static void ctr_lid_update(struct ctr_lid *lid)
{
	bool closed = ctr_lid_read_closed(lid);

	if (closed != READ_ONCE(lid->closed)) {
		WRITE_ONCE(lid->closed, closed);
		dev_info(lid->dev, "shell %s\n", closed ? "closed" : "opened");
		sysfs_notify(&lid->dev->kobj, NULL, "closed");
	}
}

static void ctr_lid_run_hook(struct ctr_lid *lid)
{
	static char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin:/system/bin",
		NULL
	};
	char path[sizeof(lid->sleep_hook)];
	char *argv[] = { path, "lid-close", NULL };
	int ret;

	mutex_lock(&lid->hook_lock);
	strscpy(path, lid->sleep_hook, sizeof(path));
	mutex_unlock(&lid->hook_lock);
	if (!path[0])
		return;

	ret = call_usermodehelper(path, argv, envp, UMH_WAIT_PROC);
	dev_info(lid->dev, "sleep hook %s returned %d\n", path, ret);
}

static void ctr_lid_sleep_work(struct work_struct *work)
{
	struct ctr_lid *lid = container_of(work, struct ctr_lid, sleep_work);
	int tries = 0, ret;

	if (READ_ONCE(lid->closed) && READ_ONCE(lid->sleep_on_close)) {
		ctr_lid_run_hook(lid);
		ctr_lid_update(lid);
	}

	while (READ_ONCE(lid->closed) && READ_ONCE(lid->sleep_on_close)) {
		dev_info(lid->dev, "shell closed: entering sleep mode\n");
		ret = pm_suspend(PM_SUSPEND_MEM);
		if (ret) {
			/*
			 * Keep trying while the shell stays closed: giving up
			 * would leave the console awake, screens lit, in a bag.
			 */
			if (++tries <= LID_SLEEP_WARNINGS)
				dev_warn(lid->dev, "sleep refused (%d), retrying\n",
					 ret);
			msleep(tries <= LID_SLEEP_WARNINGS ? 1000 : 5000);
		} else {
			tries = 0;
			/*
			 * Woken by the shell or by another MCU event (HOME,
			 * power).  Let the MCU interrupt thread deliver it, then
			 * go back to sleep only if the shell is still closed.
			 */
			msleep(500);
		}
		ctr_lid_update(lid);
	}
}

static irqreturn_t ctr_lid_irq(int irq, void *data)
{
	struct ctr_lid *lid = data;

	ctr_lid_update(lid);
	if (READ_ONCE(lid->closed)) {
		if (READ_ONCE(lid->sleep_on_close))
			queue_work(system_unbound_wq, &lid->sleep_work);
	} else {
		/* abort a suspend that is still in progress */
		pm_wakeup_hard_event(lid->dev);
	}
	return IRQ_HANDLED;
}

static ssize_t closed_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(lid->closed));
}
static DEVICE_ATTR_RO(closed);

static ssize_t sleep_on_close_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(lid->sleep_on_close));
}

static ssize_t sleep_on_close_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	WRITE_ONCE(lid->sleep_on_close, on);
	if (on && READ_ONCE(lid->closed))
		queue_work(system_unbound_wq, &lid->sleep_work);
	return count;
}
static DEVICE_ATTR_RW(sleep_on_close);

static ssize_t sleep_hook_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&lid->hook_lock);
	ret = sysfs_emit(buf, "%s\n", lid->sleep_hook);
	mutex_unlock(&lid->hook_lock);
	return ret;
}

static ssize_t sleep_hook_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);
	size_t len = count;

	while (len && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		len--;
	if (len >= sizeof(lid->sleep_hook))
		return -ENAMETOOLONG;
	if (len && buf[0] != '/')
		return -EINVAL;

	mutex_lock(&lid->hook_lock);
	memcpy(lid->sleep_hook, buf, len);
	lid->sleep_hook[len] = '\0';
	mutex_unlock(&lid->hook_lock);
	return count;
}
static DEVICE_ATTR_RW(sleep_hook);

static struct attribute *ctr_lid_attrs[] = {
	&dev_attr_closed.attr,
	&dev_attr_sleep_on_close.attr,
	&dev_attr_sleep_hook.attr,
	NULL
};
ATTRIBUTE_GROUPS(ctr_lid);

static int ctr_lid_request_irq(struct ctr_lid *lid, struct platform_device *pdev,
			       const char *name, int *irq)
{
	int ret;

	*irq = platform_get_irq_byname(pdev, name);
	if (*irq < 0)
		return *irq;
	ret = devm_request_threaded_irq(&pdev->dev, *irq, NULL, ctr_lid_irq,
					IRQF_ONESHOT, name, lid);
	if (ret)
		dev_err(&pdev->dev, "cannot request the shell-%s IRQ: %d\n",
			name, ret);
	return ret;
}

static int ctr_lid_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ctr_lid *lid;
	int ret;

	if (!dev->parent)
		return -ENODEV;

	lid = devm_kzalloc(dev, sizeof(*lid), GFP_KERNEL);
	if (!lid)
		return -ENOMEM;

	lid->dev = dev;
	lid->map = dev_get_regmap(dev->parent, NULL);
	if (!lid->map)
		return -ENODEV;
	/*
	 * Not armed at boot: a console booted (or booting) with the lid shut
	 * must finish starting Android first.  Userspace arms it by writing
	 * sleep_on_close, which also sleeps at once if the lid is shut then.
	 */
	lid->sleep_on_close = false;
	mutex_init(&lid->hook_lock);
	INIT_WORK(&lid->sleep_work, ctr_lid_sleep_work);
	platform_set_drvdata(pdev, lid);

	lid->closed = ctr_lid_read_closed(lid);

	ret = ctr_lid_request_irq(lid, pdev, "close", &lid->irq_close);
	if (ret)
		return ret;
	ret = ctr_lid_request_irq(lid, pdev, "open", &lid->irq_open);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);

	dev_info(dev, "shell %s, sleep on close %s\n",
		 lid->closed ? "closed" : "open",
		 IS_ENABLED(CONFIG_SUSPEND) ?
		 "available (armed later through sleep_on_close)" : "unavailable");
	return 0;
}

static int ctr_lid_remove(struct platform_device *pdev)
{
	struct ctr_lid *lid = platform_get_drvdata(pdev);

	WRITE_ONCE(lid->sleep_on_close, false);
	cancel_work_sync(&lid->sleep_work);
	device_init_wakeup(&pdev->dev, false);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ctr_lid_suspend(struct device *dev)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);
	unsigned int v;
	int i;

	/*
	 * Only "shell opened" may raise the MCU interrupt line while asleep;
	 * the LCD/backlight acknowledgements of the writes below must not.
	 */
	lid->irq_mask_saved = true;
	for (i = 0; i < MCU_IRQ_REGS; i++) {
		if (regmap_read(lid->map, MCU_REG_IRQ_MASK + i, &v)) {
			lid->irq_mask_saved = false;
			break;
		}
		lid->irq_mask[i] = v;
	}
	if (lid->irq_mask_saved) {
		u32 sleep_mask = ~(u32)MCU_IRQ_SHELL_OPEN;

		for (i = 0; i < MCU_IRQ_REGS; i++)
			regmap_write(lid->map, MCU_REG_IRQ_MASK + i,
				     (sleep_mask >> (8 * i)) & 0xff);
	} else {
		dev_warn(dev, "cannot read the MCU interrupt mask\n");
	}

	/*
	 * The MCU half of open_agb_firm's GFX_sleep() + power_sleep().  The
	 * screen-off sleep only switches the backlights off: powering the
	 * panels down as well would need the LCD controllers re-initialised
	 * on wake (libn3ds LCD_init(), not done here).
	 */
	regmap_write(lid->map, MCU_REG_LCD_PWR,
		     MCU_LCD_TOP_BL_OFF | MCU_LCD_BOT_BL_OFF);
	if (ctr_pm_sleep_is_deep())
		regmap_write(lid->map, MCU_REG_LCD_PWR, MCU_LCD_PWR_OFF);
	regmap_write(lid->map, MCU_REG_PWR_LED, MCU_PWR_LED_SLEEP);

	/* like power_sleep(): drop whatever is already pending (read clears) */
	for (i = 0; i < MCU_IRQ_REGS; i++)
		regmap_read(lid->map, MCU_REG_IRQ_STAT + i, &v);

	/* "the application processor enters sleep": only for PDN sleep */
	if (ctr_pm_sleep_is_deep())
		regmap_write(lid->map, MCU_REG_SYS_PWR, MCU_SYS_PWR_SLEEP);

	/*
	 * No enable_irq_wake(): the wake-up is done by the PDN hardware (wake
	 * sources set in pm.c), and the MCU interrupt controller's parent, a
	 * GPIO3 pin, cannot be marked as a wake IRQ.
	 */
	return 0;
}

static int ctr_lid_resume(struct device *dev)
{
	struct ctr_lid *lid = dev_get_drvdata(dev);
	int i;

	/* the display controllers are running again (pm.c), so the LCDs can start */
	regmap_write(lid->map, MCU_REG_PWR_LED, MCU_PWR_LED_AUTO);
	if (ctr_pm_sleep_is_deep()) {
		regmap_write(lid->map, MCU_REG_LCD_PWR, MCU_LCD_PWR_ON);
		msleep(20);
	}
	regmap_write(lid->map, MCU_REG_LCD_PWR,
		     MCU_LCD_TOP_BL_ON | MCU_LCD_BOT_BL_ON);

	/*
	 * Restore the mask last, after the LCD acknowledgements are out (the
	 * bits the kernel does not handle are masked by then).  A pending
	 * "shell opened" stays pending and is delivered once it is unmasked.
	 */
	msleep(10);
	if (lid->irq_mask_saved) {
		unsigned int v;

		/*
		 * Byte 3 holds only the LCD/backlight acknowledgements (bits
		 * 24-29), which nothing in the kernel handles: clear them.
		 * Bytes 0-2 stay pending for the MCU interrupt controller.
		 */
		regmap_read(lid->map, MCU_REG_IRQ_STAT + 3, &v);
		for (i = 0; i < MCU_IRQ_REGS; i++)
			regmap_write(lid->map, MCU_REG_IRQ_MASK + i,
				     lid->irq_mask[i]);
		lid->irq_mask_saved = false;
	}
	ctr_lid_update(lid);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(ctr_lid_pm_ops, ctr_lid_suspend, ctr_lid_resume);

static const struct of_device_id ctr_lid_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_lid_of_match);

static struct platform_driver ctr_lid_driver = {
	.probe = ctr_lid_probe,
	.remove = ctr_lid_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_lid_of_match,
		.dev_groups = ctr_lid_groups,
		.pm = &ctr_lid_pm_ops,
	},
};
module_platform_driver(ctr_lid_driver);

MODULE_DESCRIPTION("Nintendo 3DS shell (lid) switch and lid sleep");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
