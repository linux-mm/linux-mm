/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM shmem

#if !defined(_TRACE_SHMEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SHMEM_H

#include <linux/bits.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/tracepoint.h>

#define SHMEM_SWAPIN_LOOKUP	0
#define SHMEM_SWAPIN_CACHE	1
#define SHMEM_SWAPIN_SYNC		2
#define SHMEM_SWAPIN_READAHEAD	3
#define SHMEM_SWAPIN_POISON	4

#define SHMEM_SWAPIN_FALLBACK_NONE		0
#define SHMEM_SWAPIN_FALLBACK_USERFAULTFD	BIT(0)
#define SHMEM_SWAPIN_FALLBACK_ZSWAP		BIT(1)
#define SHMEM_SWAPIN_FALLBACK_SYNC		BIT(2)

#define show_shmem_swapin_fallback(flags)			\
	(flags) ? __print_flags(flags, "|",		\
	{ SHMEM_SWAPIN_FALLBACK_USERFAULTFD, "userfaultfd" },\
	{ SHMEM_SWAPIN_FALLBACK_ZSWAP, "zswap" },	\
	{ SHMEM_SWAPIN_FALLBACK_SYNC, "sync_fail" }) : "none"

TRACE_EVENT(shmem_swapin_folio,

	TP_PROTO(struct inode *inode, pgoff_t index,
		 unsigned int entry_type, pgoff_t entry_offset,
		 int requested_order, int actual_order, unsigned int path,
		 unsigned int fallback, int fallback_error, int error),

	TP_ARGS(inode, index, entry_type, entry_offset, requested_order,
		actual_order, path, fallback, fallback_error, error),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(unsigned long, index)
		__field(unsigned int, entry_type)
		__field(unsigned long, entry_offset)
		__field(int, requested_order)
		__field(int, actual_order)
		__field(unsigned int, path)
		__field(unsigned int, fallback)
		__field(int, fallback_error)
		__field(int, error)
	),

	TP_fast_assign(
		__entry->dev = inode->i_sb->s_dev;
		__entry->ino = inode->i_ino;
		__entry->index = index;
		__entry->entry_type = entry_type;
		__entry->entry_offset = entry_offset;
		__entry->requested_order = requested_order;
		__entry->actual_order = actual_order;
		__entry->path = path;
		__entry->fallback = fallback;
		__entry->fallback_error = fallback_error;
		__entry->error = error;
	),

	TP_printk("dev=%d:%d ino=%lu index=%lu entry=%u:%lu requested_order=%d actual_order=%d path=%s fallback=%s fallback_error=%d error=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->index,
		  __entry->entry_type, __entry->entry_offset,
		  __entry->requested_order, __entry->actual_order,
		  __print_symbolic(__entry->path,
				   { SHMEM_SWAPIN_LOOKUP, "lookup" },
				   { SHMEM_SWAPIN_CACHE, "cache" },
				   { SHMEM_SWAPIN_SYNC, "sync" },
				   { SHMEM_SWAPIN_READAHEAD, "readahead" },
				   { SHMEM_SWAPIN_POISON, "poison" }),
		  show_shmem_swapin_fallback(__entry->fallback),
		  __entry->fallback_error, __entry->error)
);

#define SHMEM_ALLOC_ALLOC	0
#define SHMEM_ALLOC_CHARGE	1
#define SHMEM_ALLOC_CACHE		2
#define SHMEM_ALLOC_ACCOUNT	3
#define SHMEM_ALLOC_DONE		4
#define SHMEM_ALLOC_FILTER	5

TRACE_EVENT(shmem_alloc_folio_finish,

	TP_PROTO(struct inode *inode, pgoff_t index,
		 int requested_order, int suitable_order, int actual_order,
		 unsigned int stage, int error, int ret),

	TP_ARGS(inode, index, requested_order, suitable_order, actual_order,
		stage, error, ret),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(unsigned long, index)
		__field(int, requested_order)
		__field(int, suitable_order)
		__field(int, actual_order)
		__field(unsigned int, stage)
		__field(int, error)
		__field(int, ret)
	),

	TP_fast_assign(
		__entry->dev = inode->i_sb->s_dev;
		__entry->ino = inode->i_ino;
		__entry->index = index;
		__entry->requested_order = requested_order;
		__entry->suitable_order = suitable_order;
		__entry->actual_order = actual_order;
		__entry->stage = stage;
		__entry->error = error;
		__entry->ret = ret;
	),

	TP_printk("dev=%d:%d ino=%lu index=%lu requested_order=%d suitable_order=%d actual_order=%d stage=%s error=%d ret=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->index,
		  __entry->requested_order, __entry->suitable_order,
		  __entry->actual_order,
		  __print_symbolic(__entry->stage,
				   { SHMEM_ALLOC_ALLOC, "alloc" },
				   { SHMEM_ALLOC_CHARGE, "charge" },
				   { SHMEM_ALLOC_CACHE, "cache" },
				   { SHMEM_ALLOC_ACCOUNT, "account" },
				   { SHMEM_ALLOC_DONE, "done" },
				   { SHMEM_ALLOC_FILTER, "filter" }),
		  __entry->error, __entry->ret)
);

#endif /* _TRACE_SHMEM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
