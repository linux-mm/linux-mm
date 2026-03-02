/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_QPW_H
#define _LINUX_QPW_H

#include "linux/spinlock.h"
#include "linux/local_lock.h"
#include "linux/workqueue.h"

#ifndef CONFIG_QPW

typedef local_lock_t qpw_lock_t;
typedef local_trylock_t qpw_trylock_t;

struct qpw_struct {
	struct work_struct work;
};

#define qpw_lock_init(lock)				\
	local_lock_init(lock)

#define qpw_trylock_init(lock)				\
	local_trylock_init(lock)

#define qpw_lock(lock, cpu)				\
	local_lock(lock)

#define local_qpw_lock(lock)				\
	local_lock(lock)

#define qpw_lock_irqsave(lock, flags, cpu)		\
	local_lock_irqsave(lock, flags)

#define local_qpw_lock_irqsave(lock, flags)		\
	local_lock_irqsave(lock, flags)

#define qpw_trylock(lock, cpu)				\
	local_trylock(lock)

#define local_qpw_trylock(lock)				\
	local_trylock(lock)

#define qpw_trylock_irqsave(lock, flags, cpu)		\
	local_trylock_irqsave(lock, flags)

#define qpw_unlock(lock, cpu)				\
	local_unlock(lock)

#define local_qpw_unlock(lock)				\
	local_unlock(lock)

#define qpw_unlock_irqrestore(lock, flags, cpu)		\
	local_unlock_irqrestore(lock, flags)

#define local_qpw_unlock_irqrestore(lock, flags)	\
	local_unlock_irqrestore(lock, flags)

#define qpw_lockdep_assert_held(lock)			\
	lockdep_assert_held(lock)

#define queue_percpu_work_on(c, wq, qpw)		\
	queue_work_on(c, wq, &(qpw)->work)

#define flush_percpu_work(qpw)				\
	flush_work(&(qpw)->work)

#define qpw_get_cpu(qpw)	smp_processor_id()

#define qpw_is_cpu_remote(cpu)		(false)

#define INIT_QPW(qpw, func, c)				\
	INIT_WORK(&(qpw)->work, (func))

#else /* CONFIG_QPW */

DECLARE_STATIC_KEY_MAYBE(CONFIG_QPW_DEFAULT, qpw_sl);

typedef union {
	spinlock_t sl;
	local_lock_t ll;
} qpw_lock_t;

typedef union {
	spinlock_t sl;
	local_trylock_t ll;
} qpw_trylock_t;

struct qpw_struct {
	struct work_struct work;
	int cpu;
};

#define qpw_lock_init(lock)								\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			spin_lock_init(lock.sl);					\
		else									\
			local_lock_init(lock.ll);					\
	} while (0)

#define qpw_trylock_init(lock)								\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			spin_lock_init(lock.sl);					\
		else									\
			local_trylock_init(lock.ll);					\
	} while (0)

#define qpw_lock(lock, cpu)								\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			spin_lock(per_cpu_ptr(lock.sl, cpu));				\
		else									\
			local_lock(lock.ll);						\
	} while (0)

#define local_qpw_lock(lock)								\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
			migrate_disable();						\
			spin_lock(this_cpu_ptr(lock.sl));				\
		} else									\
			local_lock(lock.ll);						\
	} while (0)

#define qpw_lock_irqsave(lock, flags, cpu)						\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			spin_lock_irqsave(per_cpu_ptr(lock.sl, cpu), flags);		\
		else									\
			local_lock_irqsave(lock.ll, flags);				\
	} while (0)

#define local_qpw_lock_irqsave(lock, flags)						\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
			migrate_disable();						\
			spin_lock_irqsave(this_cpu_ptr(lock.sl), flags);		\
		} else									\
			local_lock_irqsave(lock.ll, flags);				\
	} while (0)


#define qpw_trylock(lock, cpu)                                                          \
	({                                                                              \
		int t;                                                                  \
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))                   \
			t = spin_trylock(per_cpu_ptr(lock.sl, cpu));                    \
		else                                                                    \
			t = local_trylock(lock.ll);                                     \
		t;                                                                      \
	})

#define local_qpw_trylock(lock)								\
	({										\
		int t;									\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
			migrate_disable();						\
			t = spin_trylock(this_cpu_ptr(lock.sl));			\
			if (!t)								\
				migrate_enable();					\
		} else									\
			t = local_trylock(lock.ll);					\
		t;									\
	})

#define qpw_trylock_irqsave(lock, flags, cpu)						\
	({										\
		int t;									\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			t = spin_trylock_irqsave(per_cpu_ptr(lock.sl, cpu), flags);	\
		else									\
			t = local_trylock_irqsave(lock.ll, flags);			\
		t;									\
	})

#define qpw_unlock(lock, cpu)								\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
			spin_unlock(per_cpu_ptr(lock.sl, cpu));				\
		} else {								\
			local_unlock(lock.ll);						\
		}									\
	} while (0)

#define local_qpw_unlock(lock)								\
do {										\
	if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
		spin_unlock(this_cpu_ptr(lock.sl));				\
		migrate_enable();						\
	} else {								\
		local_unlock(lock.ll);						\
	}									\
} while (0)

#define qpw_unlock_irqrestore(lock, flags, cpu)						\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			spin_unlock_irqrestore(per_cpu_ptr(lock.sl, cpu), flags);	\
		else									\
			local_unlock_irqrestore(lock.ll, flags);			\
	} while (0)

#define local_qpw_unlock_irqrestore(lock, flags)					\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
			spin_unlock_irqrestore(this_cpu_ptr(lock.sl), flags);		\
			migrate_enable();						\
		} else									\
			local_unlock_irqrestore(lock.ll, flags);			\
	} while (0)

#define qpw_lockdep_assert_held(lock)							\
	do {										\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl))			\
			lockdep_assert_held(this_cpu_ptr(lock.sl));			\
		else									\
			lockdep_assert_held(this_cpu_ptr(lock.ll));			\
	} while (0)

#define queue_percpu_work_on(c, wq, qpw)						\
	do {										\
		int __c = c;								\
		struct qpw_struct *__qpw = (qpw);					\
		if (static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {			\
			WARN_ON((__c) != __qpw->cpu);					\
			__qpw->work.func(&__qpw->work);					\
		} else {								\
			queue_work_on(__c, wq, &(__qpw)->work);				\
		}									\
	} while (0)

/*
 * Does nothing if QPW is set to use spinlock, as the task is already done at the
 * time queue_percpu_work_on() returns.
 */
#define flush_percpu_work(qpw)								\
	do {										\
		struct qpw_struct *__qpw = (qpw);					\
		if (!static_branch_maybe(CONFIG_QPW_DEFAULT, &qpw_sl)) {		\
			flush_work(&__qpw->work);					\
		}									\
	} while (0)

#define qpw_get_cpu(w)			container_of((w), struct qpw_struct, work)->cpu

#define qpw_is_cpu_remote(cpu)		((cpu) != smp_processor_id())

#define INIT_QPW(qpw, func, c)								\
	do {										\
		struct qpw_struct *__qpw = (qpw);					\
		INIT_WORK(&__qpw->work, (func));					\
		__qpw->cpu = (c);							\
	} while (0)

#endif /* CONFIG_QPW */
#endif /* LINUX_QPW_H */
