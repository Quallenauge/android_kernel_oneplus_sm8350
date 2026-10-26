// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-thread scheduling QoS for the ADPF task profiles of the power HAL.
 *
 * Android task profiles can only write a thread id into a fixed file, so
 * expose the WALT per-task knobs like the sched_qos interface of Pixel's
 * vendor scheduler: writing a tid to /proc/sched_qos/<knob>_set or
 * <knob>_clear changes that knob of the thread.
 *
 *   low_latency  WALT_LOW_LATENCY_PROCFS, see walt_procfs_low_latency_task()
 *   boost_mid    per-task boost TASK_BOOST_ON_MID, keeps it off the little
 *                cores; clearing only drops a boost that was set this way
 */
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>

#include "../sched.h"

enum sched_qos_knob {
	QOS_LOW_LATENCY_SET,
	QOS_LOW_LATENCY_CLEAR,
	QOS_BOOST_MID_SET,
	QOS_BOOST_MID_CLEAR,
};

static const char * const sched_qos_names[] = {
	[QOS_LOW_LATENCY_SET]	= "low_latency_set",
	[QOS_LOW_LATENCY_CLEAR]	= "low_latency_clear",
	[QOS_BOOST_MID_SET]	= "boost_mid_set",
	[QOS_BOOST_MID_CLEAR]	= "boost_mid_clear",
};

static void sched_qos_apply(struct task_struct *p, enum sched_qos_knob knob)
{
	switch (knob) {
	case QOS_LOW_LATENCY_SET:
		p->wts.low_latency |= WALT_LOW_LATENCY_PROCFS;
		break;
	case QOS_LOW_LATENCY_CLEAR:
		p->wts.low_latency &= ~WALT_LOW_LATENCY_PROCFS;
		break;
	case QOS_BOOST_MID_SET:
		/* Don't lower a stronger or timed boost set by someone else */
		if (p->wts.boost == TASK_BOOST_NONE) {
			p->wts.boost_period = 0;
			p->wts.boost = TASK_BOOST_ON_MID;
		}
		break;
	case QOS_BOOST_MID_CLEAR:
		if (p->wts.boost == TASK_BOOST_ON_MID && !p->wts.boost_period)
			p->wts.boost = TASK_BOOST_NONE;
		break;
	}
}

static ssize_t sched_qos_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	enum sched_qos_knob knob = (uintptr_t)PDE_DATA(file_inode(file));
	struct task_struct *p;
	pid_t tid;
	int err;

	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	err = kstrtoint_from_user(buf, count, 0, &tid);
	if (err)
		return err;
	if (tid <= 0)
		return -EINVAL;

	rcu_read_lock();
	p = find_task_by_vpid(tid);
	if (p)
		get_task_struct(p);
	rcu_read_unlock();
	if (!p)
		return -ESRCH;

	sched_qos_apply(p, knob);
	put_task_struct(p);

	return count;
}

static const struct file_operations sched_qos_fops = {
	.write	= sched_qos_write,
	.llseek	= noop_llseek,
};

static int __init sched_qos_init(void)
{
	struct proc_dir_entry *dir;
	uintptr_t knob;

	dir = proc_mkdir("sched_qos", NULL);
	if (!dir)
		return -ENOMEM;

	for (knob = 0; knob < ARRAY_SIZE(sched_qos_names); knob++)
		proc_create_data(sched_qos_names[knob], 0200, dir,
				 &sched_qos_fops, (void *)knob);

	return 0;
}
late_initcall(sched_qos_init);
