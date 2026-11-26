/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _DRIVERS_IOMMU_PISA_PRIV_H
#define _DRIVERS_IOMMU_PISA_PRIV_H

#include <linux/pisa.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#ifdef CONFIG_PISA

#define PISA_MAX_BLOCKS		512U

#define PISA_BLOCKS(order)	(1U << (PISA_ORDER - (order)))

/**
 * struct pisa_meta - Metadata for a single PISA page in @pisa_meta_array
 * @flags:	PISA_ACTIVE while this page is owned by a pisa_alloc
 */
struct pisa_meta {
	u8 flags;
} __aligned(PISA_META_SIZE);

static_assert(offsetof(struct pisa_meta, flags) == 0);
static_assert(sizeof(struct pisa_meta) == PISA_META_SIZE);

int pisa_meta_ensure_pfn(unsigned long pfn, bool can_block);
struct pisa_meta *pisa_meta_of_pfn(unsigned long pfn);
unsigned long pisa_meta_to_pfn(const struct pisa_meta *meta);

#endif /* CONFIG_PISA */
#endif /* _DRIVERS_IOMMU_PISA_PRIV_H */
