/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HUGETLB_CMA_H
#define _LINUX_HUGETLB_CMA_H

#include <linux/hugetlb.h>

#ifdef CONFIG_CMA
void hugetlb_cma_free_frozen_folio(struct folio *folio);
struct folio *hugetlb_cma_alloc_frozen_folio(int order, gfp_t gfp_mask,
				      int nid, nodemask_t *nodemask);
void *hugetlb_cma_alloc_bootmem(struct hstate *h, int nid, bool node_exact);
bool hugetlb_cma_exclusive_alloc(unsigned int order);
unsigned long hugetlb_cma_size(struct hstate *h);
unsigned long hugetlb_cma_total_size(void);
bool hugetlb_early_cma(struct hstate *h);
#else
static inline void hugetlb_cma_free_frozen_folio(struct folio *folio)
{
}

static inline struct folio *hugetlb_cma_alloc_frozen_folio(int order,
		gfp_t gfp_mask,	int nid, nodemask_t *nodemask)
{
	return NULL;
}

static inline void *hugetlb_cma_alloc_bootmem(struct hstate *h, int nid,
					      bool node_exact)
{
	return NULL;
}

static inline bool hugetlb_cma_exclusive_alloc(unsigned int order)
{
	return false;
}

static inline unsigned long hugetlb_cma_size(struct hstate *h)
{
	return 0;
}

static inline unsigned long hugetlb_cma_total_size(void)
{
	return 0;
}

static inline bool hugetlb_early_cma(struct hstate *h)
{
	return false;
}
#endif
#endif
