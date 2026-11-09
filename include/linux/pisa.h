/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* See Documentation/core-api/pisa.rst for the architecture overview. */
#ifndef _LINUX_PISA_H
#define _LINUX_PISA_H

#include <linux/bitops.h>
#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/types.h>

#ifdef CONFIG_PISA

#define PISA_SHIFT		21U
#define PISA_SIZE		(1UL << PISA_SHIFT)
#define PISA_MASK		(~(PISA_SIZE - 1))
#define PISA_ORDER		(PISA_SHIFT - PAGE_SHIFT)

/*
 * Each struct pisa_meta entry in pisa_meta_array has size PISA_META_SIZE
 * (256 B) and starts with a u8 flags byte.
 */
#define PISA_META_SHIFT		8
#define PISA_META_SIZE		(1U << PISA_META_SHIFT)
#define PISA_CHUNK_ORDER	(PISA_ORDER + PAGE_SHIFT - PISA_META_SHIFT)

enum {
	PISA_ACTIVE		= BIT(0), /* page is owned by a pisa_alloc */
};

/* Enabled on first pisa_meta_init() to activate fast-path hooks. */
DECLARE_STATIC_KEY_FALSE(pisa_key);

struct pisa_meta;
extern struct pisa_meta *pisa_meta_array;
extern unsigned long pisa_meta_nframes;
extern unsigned long *pisa_chunk_bitmap;

int pisa_meta_init(void);

/* fast membership check for pisa pfn/pages */
static inline bool is_pisa_pfn(unsigned long pfn)
{
	unsigned long frame = pfn >> PISA_ORDER;
	const u8 *flags;

	if (!static_branch_unlikely(&pisa_key))
		return false;
	if (unlikely(frame >= pisa_meta_nframes ||
		     !test_bit_acquire(pfn >> PISA_CHUNK_ORDER, pisa_chunk_bitmap)))
		return false;

	flags = (const u8 *)pisa_meta_array + (frame << PISA_META_SHIFT);
	return READ_ONCE(*flags) & PISA_ACTIVE;
}

#else /* !CONFIG_PISA */

static inline int pisa_meta_init(void)
{
	return -EOPNOTSUPP;
}

static inline bool is_pisa_pfn(unsigned long pfn)
{
	return false;
}

#endif /* CONFIG_PISA */

static inline bool is_pisa_phys(phys_addr_t phys)
{
	return is_pisa_pfn(phys >> PAGE_SHIFT);
}

static inline bool is_pisa_page(const struct page *page)
{
	return is_pisa_pfn(page_to_pfn(page));
}
#endif /* _LINUX_PISA_H */
