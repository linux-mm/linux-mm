/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _LINUX_KCOV_DATAFLOW_H
#define _LINUX_KCOV_DATAFLOW_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * User space ABI of /sys/kernel/debug/kcov_dataflow, see
 * Documentation/dev-tools/kcov-dataflow.rst.
 *
 * KCOV_DF_INIT_TRACK takes the buffer size in u64 words by value (same
 * convention as KCOV_INIT_TRACE). KCOV_DF_REMOTE_ENABLE takes a pointer to a
 * __u64 remote handle encoded with kcov_remote_handle() (linux/kcov.h), so the
 * full 64-bit value survives 32-bit and compat callers.
 */
#define KCOV_DF_INIT_TRACK	_IOR('d', 1, unsigned long)
#define KCOV_DF_ENABLE		_IO('d', 100)
#define KCOV_DF_DISABLE		_IO('d', 101)
#define KCOV_DF_REMOTE_ENABLE	_IOW('d', 102, __u64)
#define KCOV_DF_REMOTE_DISABLE	_IO('d', 103)

/*
 * Buffer layout (all u64 words):
 *
 *   area[0]                number of record words written after area[0]
 *   area[1 + n ..]         records, back to back, each:
 *
 *     [0] header           see KCOV_DF_HDR_* below
 *     [1] pc               instrumented location; KASLR offset removed, like
 *                          the PCs mainline kcov records
 *     [2] ENTRY/RET: the traced value's address (full pointer); may be a
 *                    NULL/ERR_PTR value the callee received, in which case the
 *                    value words hold KCOV_DF_MAGIC_BAD
 *         CMP:       comparison type, KCOV_CMP_SIZE()/KCOV_CMP_CONST bits
 *                    (linux/kcov.h)
 *     [3 .. 3 + nvals)     value words: the scalar (nvals == 1), the expanded
 *                          struct fields, or the two CMP operands (nvals == 2)
 *
 * The header packs:
 *
 *   bits  0..23  per-task record sequence number
 *   bits 28..31  record type, KCOV_DF_TYPE_*
 *   bits 32..47  nvals, the number of value words that follow word [2]
 *   bits 48..55  ENTRY/RET: size in bytes of the traced argument/return value
 *                (clamped to 255)
 *   bits 56..63  ENTRY: argument index (clamped to 255); RET: 0
 *
 * A consumer walks the buffer as
 *
 *	pos = 1;
 *	while (pos < 1 + area[0]) {
 *		hdr = area[pos];
 *		nvals = KCOV_DF_HDR_NVALS(hdr);
 *		...
 *		pos += KCOV_DF_RECORD_WORDS(nvals);
 *	}
 *
 * area[0] never exceeds the buffer size minus one, and every counted word has
 * been written, so the walk above stays inside the mapping.
 */
#define KCOV_DF_TYPE_CMP	0xC
#define KCOV_DF_TYPE_ENTRY	0xE
#define KCOV_DF_TYPE_RET	0xF

#define KCOV_DF_HDR_SEQ_MASK	0x00FFFFFFULL
#define KCOV_DF_HDR_TYPE_SHIFT	28
#define KCOV_DF_HDR_TYPE_MASK	0xFULL
#define KCOV_DF_HDR_NVALS_SHIFT	32
#define KCOV_DF_HDR_NVALS_MASK	0xFFFFULL
#define KCOV_DF_HDR_SIZE_SHIFT	48
#define KCOV_DF_HDR_SIZE_MASK	0xFFULL
#define KCOV_DF_HDR_ARGIDX_SHIFT 56
#define KCOV_DF_HDR_ARGIDX_MASK	0xFFULL

#define KCOV_DF_HDR_SEQ(h)	((h) & KCOV_DF_HDR_SEQ_MASK)
#define KCOV_DF_HDR_TYPE(h)	(((h) >> KCOV_DF_HDR_TYPE_SHIFT) & KCOV_DF_HDR_TYPE_MASK)
#define KCOV_DF_HDR_NVALS(h)	(((h) >> KCOV_DF_HDR_NVALS_SHIFT) & KCOV_DF_HDR_NVALS_MASK)
#define KCOV_DF_HDR_SIZE(h)	(((h) >> KCOV_DF_HDR_SIZE_SHIFT) & KCOV_DF_HDR_SIZE_MASK)
#define KCOV_DF_HDR_ARGIDX(h)	(((h) >> KCOV_DF_HDR_ARGIDX_SHIFT) & KCOV_DF_HDR_ARGIDX_MASK)

/* Words per record: header, pc, pointer/cmp-type, then the value words. */
#define KCOV_DF_RECORD_HDR_WORDS	3
#define KCOV_DF_RECORD_WORDS(nvals)	(KCOV_DF_RECORD_HDR_WORDS + (nvals))

/* Largest nvals a record can carry; longer field lists are truncated. */
#define KCOV_DF_MAX_VALS	KCOV_DF_HDR_NVALS_MASK

/* Value word written when the traced pointer or a field could not be read. */
#define KCOV_DF_MAGIC_BAD	0xBADADD85ULL

#endif /* _LINUX_KCOV_DATAFLOW_H */
