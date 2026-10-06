// SPDX-License-Identifier: GPL-2.0
/*
 * logger.c - Android /dev/log/* driver for the Nintendo 3DS port
 *
 * Android's liblog writes every message to /dev/log/main, /dev/log/events or
 * /dev/log/radio, and logcat reads them back.  The driver was removed from
 * mainline (and from the Android common kernel) long ago, and without it the
 * Android userspace's logs vanish silently - which made debugging the port's
 * first Android boot impossible: the kernel log showed nothing between "mounted
 * /system" and the hang, because the interesting messages were all going into a
 * device that did not exist.
 *
 * This is a from-scratch implementation of the same ABI, not a backport:
 *
 *   device         : misc device, so devtmpfs (and Android's init coldboot)
 *                    create /dev/log/{main,events,radio} automatically
 *   write()/writev(): the caller (liblog) sends
 *                        struct logger_entry header  (20 bytes)
 *                        followed by the payload (tag + message)
 *                    The header the caller wrote is *ignored*: the driver
 *                    rebuilds it with the real pid/tid/timestamp and the real
 *                    payload length, exactly like the original driver did.
 *   read()         : returns exactly one entry per call:
 *                        struct logger_entry { u16 len; u16 __pad; s32 pid;
 *                                              s32 tid; s32 sec; s32 nsec;
 *                                              char msg[]; }
 *                    Reading consumes the entry.
 *   poll()         : POLLIN when an entry is available, POLLOUT always.
 *   ioctls         : LOGGER_GET_LOG_BUF_SIZE / LOGGER_GET_LOG_LEN /
 *                    LOGGER_GET_NEXT_ENTRY_LEN / LOGGER_FLUSH_LOG.
 *
 * Layout of each device's buffer: a ring of @size bytes holding
 * [u32 length][entry bytes] records, so a reader can always return one whole
 * entry.  A record never straddles the wrap point.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

/*
 * The classic (2009) misc names are log_main/log_events/log_radio.  Android's
 * init special-cases "log_" to put them under /dev/log/, which is what
 * liblog opens; names containing '/' (as an earlier version of this driver
 * used) make init create /dev/main et al. and skip the log permissions.
 */
#define LOGGER_LOG_MAIN		"log_main"
#define LOGGER_LOG_EVENTS	"log_events"
#define LOGGER_LOG_RADIO	"log_radio"

#define LOGGER_ENTRY_MAX_LEN	(4 * 1024)
#define LOGGER_ENTRY_MAX_PAYLOAD \
	(LOGGER_ENTRY_MAX_LEN - sizeof(struct logger_entry))

#define LOGGER_GET_LOG_BUF_SIZE		_IO('l', 1)
#define LOGGER_GET_LOG_LEN		_IO('l', 2)
#define LOGGER_GET_NEXT_ENTRY_LEN	_IO('l', 3)
#define LOGGER_FLUSH_LOG		_IO('l', 4)

struct logger_entry {
	__u16 len;	/* payload length, excluding this header */
	__u16 __pad;
	__s32 pid;
	__s32 tid;
	__s32 sec;
	__s32 nsec;
	char msg[];	/* the payload: priority, tag, message */
} __packed;

struct logger_log {
	const char		*name;
	struct miscdevice	misc;
	u8			*buffer;	/* ring of @size bytes */
	size_t			size;
	size_t			head;		/* read position  */
	size_t			tail;		/* write position */
	struct mutex		lock;
	wait_queue_head_t	wq;
};

#define LOGGER_BUFFER_SIZE	(64 * 1024)

/*
 * ARM9 black-box capture (option B): mirror the record into the display
 * driver's black-box page, in this caller's own context, so Android's log
 * survives a wedged tmpfs/SD path (see ctr_lcd_fb.c).
 */
extern void ctr_cap_log_append(const char *payload, size_t len);

static struct logger_log *file_to_log(struct file *file)
{
	return file->private_data;
}

/* ---------------------------------------------------------------- write */

static ssize_t logger_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct logger_log *log = file_to_log(iocb->ki_filp);
	struct logger_entry *entry;
	size_t written = iov_iter_count(from);
	size_t payload = written;
	ktime_t now;
	u32 reclen, hdrlen = sizeof(struct logger_entry);
	u8 *p;
	int ret = 0;

	if (payload == 0)
		return -EINVAL;

	if (payload > LOGGER_ENTRY_MAX_PAYLOAD)
		payload = LOGGER_ENTRY_MAX_PAYLOAD;

	reclen = hdrlen + payload;
	if (reclen + sizeof(u32) > log->size)
		return -EINVAL;

	p = kmalloc(reclen, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	/*
	 * Classic 2009 logger ABI: userspace writes ONLY the payload, i.e.
	 * [priority][tag NUL][text], and the kernel fills in the
	 * logger_entry header.  Android's liblog __android_log_write() writev()s
	 * exactly three iovecs (prio, tag, msg) and no header - an earlier
	 * version of this driver skipped 20 bytes here, which silently chopped
	 * the first 20 bytes off every Android log line.  Anything beyond
	 * LOGGER_ENTRY_MAX_PAYLOAD is truncated, but the whole write must
	 * still be consumed or liblog would retry into a half-read iov_iter.
	 */
	if (copy_from_iter(p + hdrlen, payload, from) != payload) {
		ret = -EFAULT;
		goto out;
	}
	/* drop anything we truncated above */
	if (iov_iter_count(from)) {
		char sink[256];
		size_t left = iov_iter_count(from);

		while (left) {
			size_t n = min(left, sizeof(sink));

			if (copy_from_iter(sink, n, from) != n)
				break;
			left -= n;
		}
	}

	entry = (struct logger_entry *)p;
	now = ktime_get_real();
	entry->len = payload;
	entry->__pad = 0;
	entry->pid = task_tgid_nr(current);
	entry->tid = task_pid_nr(current);
	entry->sec = (__s32)ktime_to_timespec64(now).tv_sec;
	entry->nsec = (__s32)ktime_to_timespec64(now).tv_nsec;

	ctr_cap_log_append(p + hdrlen, payload);

	mutex_lock(&log->lock);

	/* skip corrupted entries, then make room if we would overrun a reader */
	if (log->head != log->tail) {
		u32 len;

		memcpy(&len, log->buffer + log->head, sizeof(len));
		if (len + sizeof(u32) > log->size) {
			log->head = 0;
			log->tail = 0;
		}
	}

	if (log->tail + sizeof(u32) + reclen > log->size) {
		/* wrap: readers see the gap as the end of the buffer */
		if (log->head <= log->tail && log->head != 0) {
			/* reader is behind us in the buffer: make room by
			 * dropping the oldest entries */
			while (log->head != 0 && log->head != log->tail) {
				u32 len;

				memcpy(&len, log->buffer + log->head, sizeof(len));
				if (len + sizeof(u32) > log->size)
					break;
				log->head += sizeof(u32) + len;
				if (log->head >= log->size)
					log->head = 0;
			}
		}
		log->tail = 0;
	}

	memcpy(log->buffer + log->tail, &reclen, sizeof(u32));
	memcpy(log->buffer + log->tail + sizeof(u32), p, reclen);
	log->tail += sizeof(u32) + reclen;

	mutex_unlock(&log->lock);
	wake_up_interruptible(&log->wq);
out:
	kfree(p);
	return ret ? ret : (ssize_t)written;
}

/* ----------------------------------------------------------------- read */

static ssize_t logger_read(struct file *file, char __user *buf, size_t count,
			   loff_t *pos)
{
	struct logger_log *log = file_to_log(file);
	u8 *record;
	u32 len;
	int ret = 0;

	for (;;) {
		if (mutex_lock_interruptible(&log->lock))
			return -ERESTARTSYS;

		if (log->head == log->tail) {
			mutex_unlock(&log->lock);
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			ret = wait_event_interruptible(log->wq,
						       log->head != log->tail);
			if (ret)
				return ret;
			continue;
		}

		/*
		 * If the reader is at the very end of the ring there is no record
		 * there (the writer wraps rather than straddling), so rewind and
		 * look again.  The unlock is essential: the top of this loop takes
		 * the mutex again, and without it the reader deadlocks against
		 * itself while holding the lock -- then every Android writer blocks
		 * in logger_write_iter (D state) and the framework freezes.
		 */
		if (log->head + sizeof(u32) > log->size) {
			log->head = 0;
			mutex_unlock(&log->lock);
			continue;
		}
		memcpy(&len, log->buffer + log->head, sizeof(len));
		if (len == 0 || len > LOGGER_ENTRY_MAX_LEN) {
			log->head += sizeof(u32);
			if (log->head >= log->size)
				log->head = 0;
			mutex_unlock(&log->lock);
			continue;
		}
		if (log->head + sizeof(u32) + len > log->size) {
			log->head = 0;
			mutex_unlock(&log->lock);
			continue;
		}
		break;
	}

	if (len > count) {
		/* reader's buffer is too small: don't lose the entry */
		mutex_unlock(&log->lock);
		return -EINVAL;
	}

	record = log->buffer + log->head + sizeof(u32);
	if (copy_to_user(buf, record, len))
		ret = -EFAULT;
	else
		ret = len;

	log->head += sizeof(u32) + len;
	if (log->head >= log->size)
		log->head = 0;

	mutex_unlock(&log->lock);
	return ret;
}

/* ---------------------------------------------------------------- misc  */

static __poll_t logger_poll(struct file *file, poll_table *wait)
{
	struct logger_log *log = file_to_log(file);
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;	/* always writable */

	poll_wait(file, &log->wq, wait);
	if (log->head != log->tail)
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static long logger_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct logger_log *log = file_to_log(file);
	u32 len;

	switch (cmd) {
	case LOGGER_GET_LOG_BUF_SIZE:
		return put_user((u32)log->size, (u32 __user *)arg) ? -EFAULT : 0;
	case LOGGER_GET_LOG_LEN:
		len = (u32)(log->tail - log->head);
		return put_user(len, (u32 __user *)arg) ? -EFAULT : 0;
	case LOGGER_GET_NEXT_ENTRY_LEN:
		if (log->head == log->tail)
			return put_user(0, (u32 __user *)arg) ? -EFAULT : 0;
		if (log->head + sizeof(u32) > log->size)
			return put_user(0, (u32 __user *)arg) ? -EFAULT : 0;
		memcpy(&len, log->buffer + log->head, sizeof(len));
		return put_user(len, (u32 __user *)arg) ? -EFAULT : 0;
	case LOGGER_FLUSH_LOG:
		mutex_lock(&log->lock);
		log->head = log->tail = 0;
		mutex_unlock(&log->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

static int logger_open(struct inode *inode, struct file *file)
{
	struct logger_log *log;

	/* each misc device carries its own log in misc.this_device */
	log = container_of(file->private_data, struct logger_log, misc);
	file->private_data = log;
	return nonseekable_open(inode, file);
}

static const struct file_operations logger_fops = {
	.owner		= THIS_MODULE,
	.open		= logger_open,
	.read		= logger_read,
	.write_iter	= logger_write_iter,
	.poll		= logger_poll,
	.unlocked_ioctl	= logger_ioctl,
	.compat_ioctl	= logger_ioctl,
	.llseek		= noop_llseek,
};

/* ---------------------------------------------------------------- init  */

static struct logger_log log_main = { .name = LOGGER_LOG_MAIN };
static struct logger_log log_events = { .name = LOGGER_LOG_EVENTS };
static struct logger_log log_radio = { .name = LOGGER_LOG_RADIO };

static struct logger_log *all_logs[] = { &log_main, &log_events, &log_radio };

static int __init logger_init_log(struct logger_log *log)
{
	log->buffer = vzalloc(LOGGER_BUFFER_SIZE);
	if (!log->buffer)
		return -ENOMEM;
	log->size = LOGGER_BUFFER_SIZE;
	log->head = log->tail = 0;
	mutex_init(&log->lock);
	init_waitqueue_head(&log->wq);

	log->misc.minor = MISC_DYNAMIC_MINOR;
	log->misc.name = log->name;
	log->misc.fops = &logger_fops;

	return misc_register(&log->misc);
}

static int __init logger_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(all_logs); i++) {
		ret = logger_init_log(all_logs[i]);
		if (ret) {
			pr_err("logger: failed to register /dev/%s (%d)\n",
			       all_logs[i]->name, ret);
			return ret;
		}
	}
	pr_info("logger: Android /dev/log/{main,events,radio} ready\n");
	return 0;
}
device_initcall(logger_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Android logger for the Nintendo 3DS port");
