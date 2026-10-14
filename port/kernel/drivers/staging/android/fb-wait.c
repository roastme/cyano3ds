// SPDX-License-Identifier: GPL-2.0
/*
 * /sys/power/wait_for_fb_sleep and /sys/power/wait_for_fb_wake for the
 * Android 2.3 (CM7) userspace on the Nintendo 3DS port.
 *
 * Gingerbread's SurfaceFlinger (DisplayHardwareBase) watches these two files
 * (the pre-mainline "fbearlysuspend" interface) to learn when the screen goes
 * to sleep and comes back.  If they are missing it falls back to a
 * ConsoleManagerThread: it opens /dev/tty0, VT_ACTIVATEs a fresh virtual
 * terminal and sets it to KD_GRAPHICS.  On this port the kernel console
 * (fbcon) lives on the TOP screen while Android draws on the bottom one, so
 * that VT switch replaces the console with an empty, cursor-only screen.
 *
 * The protocol is the one of the original fbearlysuspend driver:
 *   - a read of wait_for_fb_sleep blocks while drawing is allowed and returns
 *     "sleeping" once the screen is going to sleep; SurfaceFlinger then
 *     releases the screen (stops drawing);
 *   - its next read of wait_for_fb_wake acknowledges that, then blocks until
 *     the screen is back and returns "awake"; SurfaceFlinger then re-acquires
 *     the screen and repaints.
 *
 * There is no early suspend in this kernel: the screen sleeps with the whole
 * system (lid sleep, see drivers/platform/nintendo3ds/ctr_lid.c, or
 * "echo mem > /sys/power/state").  A PM notifier therefore asks
 * SurfaceFlinger to stop drawing before tasks are frozen, waits up to a second
 * for the acknowledgement, and lets it draw again after resume.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/wait.h>

enum {
	FB_STATE_DRAWING_OK,
	FB_STATE_REQUEST_STOP_DRAWING,
	FB_STATE_STOPPED_DRAWING,
};

static int fb_state = FB_STATE_DRAWING_OK;
static DEFINE_SPINLOCK(fb_state_lock);
static DECLARE_WAIT_QUEUE_HEAD(fb_state_wq);
/* readers blocked in wait_for_fb_sleep, i.e. a SurfaceFlinger to ask */
static atomic_t fb_sleep_waiters = ATOMIC_INIT(0);

static ssize_t wait_for_fb_sleep_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	int ret;

	atomic_inc(&fb_sleep_waiters);
	ret = wait_event_interruptible(fb_state_wq,
				       READ_ONCE(fb_state) != FB_STATE_DRAWING_OK);
	atomic_dec(&fb_sleep_waiters);
	if (ret && READ_ONCE(fb_state) == FB_STATE_DRAWING_OK)
		return ret;
	return sysfs_emit(buf, "sleeping");
}

static ssize_t wait_for_fb_wake_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	unsigned long flags;
	int ret;

	/* reading this file means the reader has stopped drawing */
	spin_lock_irqsave(&fb_state_lock, flags);
	if (fb_state == FB_STATE_REQUEST_STOP_DRAWING) {
		fb_state = FB_STATE_STOPPED_DRAWING;
		wake_up_all(&fb_state_wq);
	}
	spin_unlock_irqrestore(&fb_state_lock, flags);

	ret = wait_event_interruptible(fb_state_wq,
				       READ_ONCE(fb_state) == FB_STATE_DRAWING_OK);
	if (ret && READ_ONCE(fb_state) != FB_STATE_DRAWING_OK)
		return ret;
	return sysfs_emit(buf, "awake");
}

static void fb_stop_drawing(void)
{
	unsigned long flags;
	bool ask = atomic_read(&fb_sleep_waiters) > 0;

	spin_lock_irqsave(&fb_state_lock, flags);
	fb_state = FB_STATE_REQUEST_STOP_DRAWING;
	spin_unlock_irqrestore(&fb_state_lock, flags);
	wake_up_all(&fb_state_wq);

	if (!ask)
		return;
	if (wait_event_timeout(fb_state_wq,
			READ_ONCE(fb_state) == FB_STATE_STOPPED_DRAWING, HZ))
		pr_info("fb-wait: userspace stopped drawing\n");
	else
		pr_warn("fb-wait: timeout waiting for userspace to stop drawing\n");
}

static void fb_start_drawing(void)
{
	unsigned long flags;

	spin_lock_irqsave(&fb_state_lock, flags);
	fb_state = FB_STATE_DRAWING_OK;
	spin_unlock_irqrestore(&fb_state_lock, flags);
	wake_up_all(&fb_state_wq);
}

static int fb_wait_pm_notify(struct notifier_block *nb, unsigned long action,
			     void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		fb_stop_drawing();
		break;
	case PM_POST_SUSPEND:
		fb_start_drawing();
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block fb_wait_pm_nb = {
	.notifier_call = fb_wait_pm_notify,
};

static struct kobj_attribute wait_for_fb_sleep_attr = __ATTR_RO(wait_for_fb_sleep);
static struct kobj_attribute wait_for_fb_wake_attr = __ATTR_RO(wait_for_fb_wake);

static struct attribute *fb_wait_attrs[] = {
	&wait_for_fb_sleep_attr.attr,
	&wait_for_fb_wake_attr.attr,
	NULL,
};

static const struct attribute_group fb_wait_group = {
	.attrs = fb_wait_attrs,
};

static int __init fb_wait_init(void)
{
	int ret;

	/* power_kobj (/sys/power) is created by pm_init(), a core_initcall */
	if (!power_kobj)
		return -ENODEV;
	ret = sysfs_create_group(power_kobj, &fb_wait_group);
	if (ret)
		return ret;
	return register_pm_notifier(&fb_wait_pm_nb);
}
late_initcall(fb_wait_init);
