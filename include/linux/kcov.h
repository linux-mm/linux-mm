/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KCOV_H
#define _LINUX_KCOV_H

#include <linux/sched.h>
#include <linux/jump_label.h>
#include <uapi/linux/kcov.h>

struct task_struct;

#ifdef CONFIG_KCOV

enum kcov_mode {
	/* Coverage collection is not enabled yet. */
	KCOV_MODE_DISABLED = 0,
	/* KCOV was initialized, but tracing mode hasn't been chosen yet. */
	KCOV_MODE_INIT = 1,
	/*
	 * Tracing coverage collection mode.
	 * Covered PCs are collected in a per-task buffer.
	 */
	KCOV_MODE_TRACE_PC = 2,
	/* Collecting comparison operands mode. */
	KCOV_MODE_TRACE_CMP = 3,
};

#define KCOV_IN_CTXSW	(1 << 30)

void kcov_task_init(struct task_struct *t);
void kcov_task_exit(struct task_struct *t);

#if defined(CONFIG_KCOV_DATAFLOW_ARGS) || defined(CONFIG_KCOV_DATAFLOW_RET)
void kcov_dataflow_task_init(struct task_struct *t);
void kcov_dataflow_task_exit(struct task_struct *t);
#else
static inline void kcov_dataflow_task_init(struct task_struct *t) {}
static inline void kcov_dataflow_task_exit(struct task_struct *t) {}
#endif

#define kcov_prepare_switch(t)			\
do {						\
	(t)->kcov_mode |= KCOV_IN_CTXSW;	\
} while (0)

#define kcov_finish_switch(t)			\
do {						\
	(t)->kcov_mode &= ~KCOV_IN_CTXSW;	\
} while (0)

/* See Documentation/dev-tools/kcov.rst for usage details. */
void kcov_remote_start(u64 handle);
void kcov_remote_stop(void);
struct kcov_common_handle_id kcov_common_handle(void);

/*
 * Validate a remote handle: it must be a well-formed kcov_remote_handle()
 * encoding, and each caller states which subsystem/instance combinations it
 * accepts. Shared by KCOV_REMOTE_ENABLE and KCOV_DF_REMOTE_ENABLE so both
 * collectors take handles from the same partitioned namespace.
 */
static inline bool kcov_check_handle(u64 handle, bool common_valid,
				     bool uncommon_valid, bool zero_valid)
{
	if (handle & ~(KCOV_SUBSYSTEM_MASK | KCOV_INSTANCE_MASK))
		return false;
	switch (handle & KCOV_SUBSYSTEM_MASK) {
	case KCOV_SUBSYSTEM_COMMON:
		return (handle & KCOV_INSTANCE_MASK) ?
			common_valid : zero_valid;
	case KCOV_SUBSYSTEM_USB:
		return uncommon_valid;
	default:
		return false;
	}
	return false;
}

static inline void kcov_remote_start_common(struct kcov_common_handle_id id)
{
	kcov_remote_start(kcov_remote_handle(KCOV_SUBSYSTEM_COMMON, id.val));
}

static inline void kcov_remote_start_usb(u64 id)
{
	kcov_remote_start(kcov_remote_handle(KCOV_SUBSYSTEM_USB, id));
}

/*
 * The softirq flavor of kcov_remote_*() functions is introduced as a temporary
 * work around for kcov's lack of nested remote coverage sections support in
 * task context. Adding support for nested sections is tracked in:
 * https://bugzilla.kernel.org/show_bug.cgi?id=210337
 */

static inline void kcov_remote_start_usb_softirq(u64 id)
{
	if (in_serving_softirq() && !in_hardirq())
		kcov_remote_start_usb(id);
}

static inline void kcov_remote_stop_softirq(void)
{
	if (in_serving_softirq() && !in_hardirq())
		kcov_remote_stop();
}

#ifdef CONFIG_64BIT
typedef unsigned long kcov_u64;
#else
typedef unsigned long long kcov_u64;
#endif

void __sanitizer_cov_trace_pc(void);
void __sanitizer_cov_trace_cmp1(u8 arg1, u8 arg2);
void __sanitizer_cov_trace_cmp2(u16 arg1, u16 arg2);
void __sanitizer_cov_trace_cmp4(u32 arg1, u32 arg2);
void __sanitizer_cov_trace_cmp8(kcov_u64 arg1, kcov_u64 arg2);
void __sanitizer_cov_trace_const_cmp1(u8 arg1, u8 arg2);
void __sanitizer_cov_trace_const_cmp2(u16 arg1, u16 arg2);
void __sanitizer_cov_trace_const_cmp4(u32 arg1, u32 arg2);
void __sanitizer_cov_trace_const_cmp8(kcov_u64 arg1, kcov_u64 arg2);
void __sanitizer_cov_trace_switch(kcov_u64 val, void *cases);

#else

static inline void kcov_task_init(struct task_struct *t) {}
static inline void kcov_task_exit(struct task_struct *t) {}
static inline void kcov_prepare_switch(struct task_struct *t) {}
static inline void kcov_finish_switch(struct task_struct *t) {}
static inline void kcov_remote_start(u64 handle) {}
static inline void kcov_remote_stop(void) {}
static inline struct kcov_common_handle_id kcov_common_handle(void)
{
	return (struct kcov_common_handle_id){};
}
static inline void kcov_remote_start_common(struct kcov_common_handle_id id) {}
static inline void kcov_remote_start_usb(u64 id) {}
static inline void kcov_remote_start_usb_softirq(u64 id) {}
static inline void kcov_remote_stop_softirq(void) {}

#endif /* CONFIG_KCOV */

/*
 * kcov_dataflow remote API. The collector is a separate object from mainline
 * kcov and is only linked in when at least one of the two capture modes is
 * configured (see kernel/Makefile), so gate the declarations the same way
 * kcov_dataflow_task_init() above is gated; a caller that brackets a region for
 * both collectors then still builds on a KCOV-only config.
 */
#if defined(CONFIG_KCOV_DATAFLOW_ARGS) || defined(CONFIG_KCOV_DATAFLOW_RET)
void kcov_df_remote_start(u64 handle);
void kcov_df_remote_stop(void);
#else
static inline void kcov_df_remote_start(u64 handle) {}
static inline void kcov_df_remote_stop(void) {}
#endif

/*
 * Handle-typed wrapper mirroring kcov_remote_start_common(), so a subsystem that
 * already routes its mainline kcov remote sections by struct
 * kcov_common_handle_id can open a dataflow section on the very same handle
 * without knowing how it is encoded. The two collectors keep separate per-task
 * state and separate handle tables, so a section of each may be nested around
 * the same region; user space registers the identical handle value with
 * KCOV_REMOTE_ENABLE and KCOV_DF_REMOTE_ENABLE to collect both.
 *
 * Unlike kcov_remote_start(), the dataflow section may only be opened from
 * sleepable task context: kcov_df_remote_start()/kcov_df_remote_stop() take a
 * mutex and may allocate or free the worker's scratch area. Both are no-ops in
 * softirq/hardirq context, so a softirq-bracketing call site collects no
 * dataflow records rather than misbehaving. A call site that is only
 * sometimes atomic (spinlock held, preemption or irqs disabled) must not use
 * this wrapper; CONFIG_DEBUG_ATOMIC_SLEEP reports such a caller.
 *
 * Without CONFIG_KCOV the handle carries no value (see struct
 * kcov_common_handle_id), and dataflow depends on KCOV, so this is a no-op.
 */
#ifdef CONFIG_KCOV
static inline void kcov_df_remote_start_common(struct kcov_common_handle_id id)
{
	kcov_df_remote_start(kcov_remote_handle(KCOV_SUBSYSTEM_COMMON, id.val));
}
#else
static inline void kcov_df_remote_start_common(struct kcov_common_handle_id id)
{
}
#endif
#if defined(CONFIG_KCOV_ENABLE_COMPARISONS) && \
	(defined(CONFIG_KCOV_DATAFLOW_ARGS) || defined(CONFIG_KCOV_DATAFLOW_RET))
/*
 * CONFIG_KCOV_ENABLE_COMPARISONS provides ONE trace-cmp instrumentation shared by
 * mainline kcov and kcov-dataflow. kcov.c's __sanitizer_cov_trace_cmp*() callbacks
 * route each operand pair through kcov_trace_cmp() below, which fans it out:
 * mainline kcov always sees it (write_comp_data() records only when the task is
 * in KCOV_MODE_TRACE_CMP), and a task with a live dataflow session gets a copy in
 * its dataflow buffer as well. The two collectors are independent fds with no
 * cross-exclusion, so a task may collect for both at once, and a dataflow-side
 * drop (inert context, full buffer) never costs mainline kcov a record. kcov.c
 * never references the dataflow side, one cmp symbol feeds both collectors, and
 * there is no separate df_cmp symbol or compiler change.
 *
 * The dataflow branch is gated by a static key so that, while no dataflow session
 * is live, this whole-kernel hot path is a patched-out NOP that costs nothing on
 * top of mainline write_comp_data() (kcov_df_cmp_key is inc'd on dataflow enable
 * in kcov_dataflow.c).
 */
DECLARE_STATIC_KEY_FALSE(kcov_df_cmp_key);
void write_comp_data(u64 type, u64 arg1, u64 arg2, u64 ip);
void kcov_df_trace_cmp(u64 type, u64 arg1, u64 arg2, u64 ip);
static inline notrace void
kcov_trace_cmp(u64 type, u64 arg1, u64 arg2, u64 ip)
{
	write_comp_data(type, arg1, arg2, ip);			/* mainline kcov */
	if (static_branch_unlikely(&kcov_df_cmp_key) && current->kcov_df_enabled)
		kcov_df_trace_cmp(type, arg1, arg2, ip);	/* kcov-dataflow */
}
#elif defined(CONFIG_KCOV_ENABLE_COMPARISONS)
/* Comparisons without a dataflow build: route straight to mainline kcov. */
void write_comp_data(u64 type, u64 arg1, u64 arg2, u64 ip);
static inline notrace void
kcov_trace_cmp(u64 type, u64 arg1, u64 arg2, u64 ip)
{
	write_comp_data(type, arg1, arg2, ip);
}
#endif
#endif /* _LINUX_KCOV_H */
