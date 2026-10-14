// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/alarm for the Android 2.3 (CM7) userspace on the Nintendo 3DS port.
 *
 * Gingerbread's AlarmManagerService and libutils' elapsedRealtime() talk to
 * the pre-mainline Android alarm driver (removed from staging in 3.x).
 * Without it AlarmManagerService "falls back to a handler" that posts every
 * alarm with Handler.sendMessageAtTime(msg, alarm.when): RTC alarms carry a
 * wall-clock time in ms, the Handler compares it against uptimeMillis(), so
 * no RTC alarm (ACTION_TIME_TICK, the status-bar clock, sync, ...) ever fires.
 *
 * From-scratch implementation of the same ioctl ABI
 * (external/kernel-headers/original/linux/android_alarm.h) on hrtimers:
 *   RTC / RTC_WAKEUP                       -> CLOCK_REALTIME  (absolute)
 *   ELAPSED_REALTIME / ELAPSED_..._WAKEUP  -> CLOCK_BOOTTIME  (absolute)
 *   SYSTEMTIME                             -> CLOCK_MONOTONIC (absolute)
 * There is no suspend on this port, so the _WAKEUP types behave like the
 * plain ones and no wakelocks are taken.
 */

#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/time32.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

enum {
	ANDROID_ALARM_RTC_WAKEUP,
	ANDROID_ALARM_RTC,
	ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
	ANDROID_ALARM_ELAPSED_REALTIME,
	ANDROID_ALARM_SYSTEMTIME,
	ANDROID_ALARM_TYPE_COUNT,
};

#define ANDROID_ALARM_TIME_CHANGE_MASK	(1U << 16)

/* userspace struct timespec is two 32-bit longs on this 32-bit ARM ABI */
#define ANDROID_ALARM_CLEAR(type)	_IO('a', 0 | ((type) << 4))
#define ANDROID_ALARM_WAIT		_IO('a', 1)
#define ANDROID_ALARM_SET(type)		_IOW('a', 2 | ((type) << 4), struct old_timespec32)
#define ANDROID_ALARM_SET_AND_WAIT(type) _IOW('a', 3 | ((type) << 4), struct old_timespec32)
#define ANDROID_ALARM_GET_TIME(type)	_IOW('a', 4 | ((type) << 4), struct old_timespec32)
#define ANDROID_ALARM_SET_RTC		_IOW('a', 5, struct old_timespec32)
#define ANDROID_ALARM_BASE_CMD(cmd)	((cmd) & ~(_IOC(0, 0, 0xf0, 0)))
#define ANDROID_ALARM_IOCTL_TO_TYPE(cmd) (_IOC_NR(cmd) >> 4)

static DEFINE_SPINLOCK(alarm_lock);
static DECLARE_WAIT_QUEUE_HEAD(alarm_wait_queue);
static struct hrtimer alarm_timers[ANDROID_ALARM_TYPE_COUNT];
static u32 alarm_enabled;
static u32 alarm_pending;
static bool alarm_opened;

static clockid_t alarm_clock(int type)
{
	switch (type) {
	case ANDROID_ALARM_RTC_WAKEUP:
	case ANDROID_ALARM_RTC:
		return CLOCK_REALTIME;
	case ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP:
	case ANDROID_ALARM_ELAPSED_REALTIME:
		return CLOCK_BOOTTIME;
	default:
		return CLOCK_MONOTONIC;
	}
}

static void alarm_get_time(int type, struct timespec64 *ts)
{
	switch (alarm_clock(type)) {
	case CLOCK_REALTIME:
		ktime_get_real_ts64(ts);
		break;
	case CLOCK_BOOTTIME:
		ktime_get_boottime_ts64(ts);
		break;
	default:
		ktime_get_ts64(ts);
		break;
	}
}

static enum hrtimer_restart alarm_fired(struct hrtimer *t)
{
	int type = t - alarm_timers;
	u32 mask = 1U << type;
	unsigned long flags;

	spin_lock_irqsave(&alarm_lock, flags);
	if (alarm_enabled & mask) {
		alarm_enabled &= ~mask;
		alarm_pending |= mask;
		wake_up(&alarm_wait_queue);
	}
	spin_unlock_irqrestore(&alarm_lock, flags);
	return HRTIMER_NORESTART;
}

static long alarm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int type = ANDROID_ALARM_IOCTL_TO_TYPE(cmd);
	u32 mask = 1U << type;
	struct old_timespec32 ts32;
	struct timespec64 ts;
	unsigned long flags;
	long rv = 0;

	if (type >= ANDROID_ALARM_TYPE_COUNT)
		return -EINVAL;

	if (ANDROID_ALARM_BASE_CMD(cmd) != ANDROID_ALARM_GET_TIME(0)) {
		if ((file->f_flags & O_ACCMODE) == O_RDONLY)
			return -EPERM;
		/* only one owner (system_server) may set alarms */
		if (!file->private_data && cmd != ANDROID_ALARM_SET_RTC) {
			spin_lock_irqsave(&alarm_lock, flags);
			if (alarm_opened) {
				spin_unlock_irqrestore(&alarm_lock, flags);
				return -EBUSY;
			}
			alarm_opened = true;
			file->private_data = (void *)1;
			spin_unlock_irqrestore(&alarm_lock, flags);
		}
	}

	switch (ANDROID_ALARM_BASE_CMD(cmd)) {
	case ANDROID_ALARM_CLEAR(0):
		hrtimer_cancel(&alarm_timers[type]);
		spin_lock_irqsave(&alarm_lock, flags);
		alarm_enabled &= ~mask;
		alarm_pending &= ~mask;
		spin_unlock_irqrestore(&alarm_lock, flags);
		break;

	case ANDROID_ALARM_SET(0):
	case ANDROID_ALARM_SET_AND_WAIT(0):
		if (get_old_timespec32(&ts, (void __user *)arg))
			return -EFAULT;
		hrtimer_cancel(&alarm_timers[type]);
		spin_lock_irqsave(&alarm_lock, flags);
		alarm_enabled |= mask;
		spin_unlock_irqrestore(&alarm_lock, flags);
		/* times in the past fire immediately */
		hrtimer_start(&alarm_timers[type], timespec64_to_ktime(ts),
			      HRTIMER_MODE_ABS);
		if (ANDROID_ALARM_BASE_CMD(cmd) != ANDROID_ALARM_SET_AND_WAIT(0))
			break;
		fallthrough;
	case ANDROID_ALARM_WAIT:
		rv = wait_event_interruptible(alarm_wait_queue, alarm_pending);
		if (rv)
			return rv;
		spin_lock_irqsave(&alarm_lock, flags);
		rv = alarm_pending;
		alarm_pending = 0;
		spin_unlock_irqrestore(&alarm_lock, flags);
		break;

	case ANDROID_ALARM_SET_RTC:
		if (get_old_timespec32(&ts, (void __user *)arg))
			return -EFAULT;
		rv = do_settimeofday64(&ts);
		if (rv < 0)
			return rv;
		spin_lock_irqsave(&alarm_lock, flags);
		alarm_pending |= ANDROID_ALARM_TIME_CHANGE_MASK;
		wake_up(&alarm_wait_queue);
		spin_unlock_irqrestore(&alarm_lock, flags);
		break;

	case ANDROID_ALARM_GET_TIME(0):
		alarm_get_time(type, &ts);
		ts32.tv_sec = ts.tv_sec;
		ts32.tv_nsec = ts.tv_nsec;
		if (copy_to_user((void __user *)arg, &ts32, sizeof(ts32)))
			return -EFAULT;
		break;

	default:
		return -EINVAL;
	}
	return rv;
}

static int alarm_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int alarm_release(struct inode *inode, struct file *file)
{
	unsigned long flags;
	int i;

	if (!file->private_data)
		return 0;
	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++)
		hrtimer_cancel(&alarm_timers[i]);
	spin_lock_irqsave(&alarm_lock, flags);
	alarm_enabled = 0;
	alarm_pending = 0;
	alarm_opened = false;
	spin_unlock_irqrestore(&alarm_lock, flags);
	return 0;
}

static const struct file_operations alarm_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= alarm_ioctl,
	.open		= alarm_open,
	.release	= alarm_release,
};

static struct miscdevice alarm_device = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "alarm",
	.fops	= &alarm_fops,
	.mode	= 0664,
};

static int __init alarm_dev_init(void)
{
	int i;

	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++) {
		hrtimer_init(&alarm_timers[i], alarm_clock(i), HRTIMER_MODE_ABS);
		alarm_timers[i].function = alarm_fired;
	}
	return misc_register(&alarm_device);
}

static void __exit alarm_dev_exit(void)
{
	int i;

	misc_deregister(&alarm_device);
	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++)
		hrtimer_cancel(&alarm_timers[i]);
}

module_init(alarm_dev_init);
module_exit(alarm_dev_exit);
MODULE_LICENSE("GPL");
