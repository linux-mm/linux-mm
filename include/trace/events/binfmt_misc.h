/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tracepoint for binfmt_misc binary type handler dispatch.
 *
 * Fires once for every exec a binfmt_misc entry matches, after the
 * interpreter and the invocation flags have been selected. Unlike
 * sched_process_exec (which reports bprm->filename) and /proc/PID/exe
 * (which a transparent dispatch sets to the binary), this reports the
 * interpreter that actually runs, so observers can see through a
 * transparent dispatch.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM binfmt_misc

#if !defined(_TRACE_BINFMT_MISC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_BINFMT_MISC_H

#include <linux/tracepoint.h>
#include <linux/binfmts.h>

TRACE_EVENT(binfmt_misc_select,

	TP_PROTO(struct linux_binprm *bprm, const char *interp,
		 const char *handler, unsigned long flags),

	TP_ARGS(bprm, interp, handler, flags),

	TP_STRUCT__entry(
		__string(filename,	bprm->filename)
		__string(interp,	interp)
		__string(handler,	handler)
		__field(unsigned long,	flags)
		__field(pid_t,		pid)
	),

	TP_fast_assign(
		__assign_str(filename);
		__assign_str(interp);
		__assign_str(handler);
		__entry->flags = flags;
		__entry->pid = current->pid;
	),

	TP_printk("pid=%d filename=%s interp=%s handler=%s flags=%#lx",
		  __entry->pid, __get_str(filename), __get_str(interp),
		  __get_str(handler), __entry->flags)
);

#endif /* _TRACE_BINFMT_MISC_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
