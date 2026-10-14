// SPDX-License-Identifier: GPL-2.0
/*
 * OOM-related procfs entries
 *
 * This file handles the OOM score, oom_adj, and oom_score_adj interfaces
 * in /proc/<pid>/.
 */

#include <linux/proc_fs.h>
#include <linux/oom.h>
#include <linux/seq_file.h>
#include <linux/capability.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/sched/signal.h>
#include <linux/pid_namespace.h>
#include <trace/events/oom.h>

#include "internal.h"

int proc_oom_score(struct seq_file *m, struct pid_namespace *ns,
		   struct pid *pid, struct task_struct *task)
{
	unsigned long totalpages = totalram_pages() + total_swap_pages;
	unsigned long points = 0;
	long badness;

	badness = oom_badness(task, totalpages);
	/*
	 * Special case OOM_SCORE_ADJ_MIN for all others scale the
	 * badness value into [0, 2000] range which we have been
	 * exporting for a long time so userspace might depend on it.
	 */
	if (badness != LONG_MIN)
		points = (OOM_SCORE_ADJ_MAX +
			  badness * OOM_SCORE_ADJ_MAX / (long)totalpages) * 2 / 3;

	seq_printf(m, "%lu\n", points);

	return 0;
}

static ssize_t oom_adj_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct task_struct *task = get_proc_task(file_inode(file));
	char buffer[PROC_NUMBUF];
	int oom_adj = OOM_ADJUST_MIN;
	size_t len;

	if (!task)
		return -ESRCH;
	if (task->signal->oom_score_adj == OOM_SCORE_ADJ_MAX)
		oom_adj = OOM_ADJUST_MAX;
	else
		oom_adj = (task->signal->oom_score_adj * -OOM_DISABLE) /
			  OOM_SCORE_ADJ_MAX;
	put_task_struct(task);
	if (oom_adj > OOM_ADJUST_MAX)
		oom_adj = OOM_ADJUST_MAX;
	len = snprintf(buffer, sizeof(buffer), "%d\n", oom_adj);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static int __set_oom_adj(struct file *file, int oom_adj, bool legacy)
{
	struct mm_struct *mm = NULL;
	struct task_struct *task;
	int err = 0;

	task = get_proc_task(file_inode(file));
	if (!task)
		return -ESRCH;

	mutex_lock(&oom_adj_mutex);
	if (legacy) {
		if (oom_adj < task->signal->oom_score_adj &&
				!capable(CAP_SYS_RESOURCE)) {
			err = -EACCES;
			goto err_unlock;
		}
		/*
		 * /proc/pid/oom_adj is provided for legacy purposes, ask users to use
		 * /proc/pid/oom_score_adj instead.
		 */
		pr_warn_once("%s (%d): /proc/%d/oom_adj is deprecated, please use /proc/%d/oom_score_adj instead.\n",
			  current->comm, task_pid_nr(current), task_pid_nr(task),
			  task_pid_nr(task));
	} else {
		if ((short)oom_adj < task->signal->oom_score_adj_min &&
				!capable(CAP_SYS_RESOURCE)) {
			err = -EACCES;
			goto err_unlock;
		}
	}

	/*
	 * Make sure we will check other processes sharing the mm if this is
	 * not vfrok which wants its own oom_score_adj.
	 * pin the mm so it doesn't go away and get reused after task_unlock
	 */
	if (!task->vfork_done) {
		struct task_struct *p = find_lock_task_mm(task);

		if (p) {
			if (mm_flags_test(MMF_MULTIPROCESS, p->mm)) {
				mm = p->mm;
				mmgrab(mm);
			}
			task_unlock(p);
		}
	}

	task->signal->oom_score_adj = oom_adj;
	if (!legacy && has_capability_noaudit(current, CAP_SYS_RESOURCE))
		task->signal->oom_score_adj_min = (short)oom_adj;
	trace_oom_score_adj_update(task);

	if (mm) {
		struct task_struct *p;

		rcu_read_lock();
		for_each_process(p) {
			if (same_thread_group(task, p))
				continue;

			/* do not touch kernel threads or the global init */
			if (p->flags & PF_KTHREAD || is_global_init(p))
				continue;

			task_lock(p);
			if (!p->vfork_done && process_shares_mm(p, mm)) {
				p->signal->oom_score_adj = oom_adj;
				if (!legacy && has_capability_noaudit(current, CAP_SYS_RESOURCE))
					p->signal->oom_score_adj_min = (short)oom_adj;
			}
			task_unlock(p);
		}
		rcu_read_unlock();
		mmdrop(mm);
	}
err_unlock:
	mutex_unlock(&oom_adj_mutex);
	put_task_struct(task);
	return err;
}

/*
 * /proc/pid/oom_adj exists solely for backwards compatibility with previous
 * kernels.  The effective policy is defined by oom_score_adj, which has a
 * different scale: oom_adj grew exponentially and oom_score_adj grows linearly.
 * Values written to oom_adj are simply mapped linearly to oom_score_adj.
 * Processes that become oom disabled via oom_adj will still be oom disabled
 * with this implementation.
 *
 * oom_adj cannot be removed since existing userspace binaries use it.
 */
static ssize_t oom_adj_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF] = {};
	int oom_adj;
	int err;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	err = kstrtoint(strstrip(buffer), 0, &oom_adj);
	if (err)
		goto out;
	if ((oom_adj < OOM_ADJUST_MIN || oom_adj > OOM_ADJUST_MAX) &&
	     oom_adj != OOM_DISABLE) {
		err = -EINVAL;
		goto out;
	}

	/*
	 * Scale /proc/pid/oom_score_adj appropriately ensuring that a maximum
	 * value is always attainable.
	 */
	if (oom_adj == OOM_ADJUST_MAX)
		oom_adj = OOM_SCORE_ADJ_MAX;
	else
		oom_adj = (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;

	err = __set_oom_adj(file, oom_adj, true);
out:
	return err < 0 ? err : count;
}

const struct file_operations proc_oom_adj_operations = {
	.read		= oom_adj_read,
	.write		= oom_adj_write,
	.llseek		= generic_file_llseek,
};

static ssize_t oom_score_adj_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct task_struct *task = get_proc_task(file_inode(file));
	char buffer[PROC_NUMBUF];
	short oom_score_adj = OOM_SCORE_ADJ_MIN;
	size_t len;

	if (!task)
		return -ESRCH;
	oom_score_adj = task->signal->oom_score_adj;
	put_task_struct(task);
	len = snprintf(buffer, sizeof(buffer), "%hd\n", oom_score_adj);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t oom_score_adj_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF] = {};
	int oom_score_adj;
	int err;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	err = kstrtoint(strstrip(buffer), 0, &oom_score_adj);
	if (err)
		goto out;
	if (oom_score_adj < OOM_SCORE_ADJ_MIN ||
			oom_score_adj > OOM_SCORE_ADJ_MAX) {
		err = -EINVAL;
		goto out;
	}

	err = __set_oom_adj(file, oom_score_adj, false);
out:
	return err < 0 ? err : count;
}

const struct file_operations proc_oom_score_adj_operations = {
	.read		= oom_score_adj_read,
	.write		= oom_score_adj_write,
	.llseek		= default_llseek,
};
