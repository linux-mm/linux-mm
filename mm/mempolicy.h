/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mm-internal API for mempolicy.c. Public API lives in
 * include/linux/mempolicy.h.
 */
#ifndef __MM_MEMPOLICY_H
#define __MM_MEMPOLICY_H

#include <linux/gfp.h>
#include <linux/mempolicy.h>

#ifdef CONFIG_NUMA
struct folio *folio_alloc_mpol_noprof(gfp_t gfp, unsigned int order,
		struct mempolicy *mpol, pgoff_t ilx, int nid);
#else
static inline struct folio *folio_alloc_mpol_noprof(gfp_t gfp, unsigned int order,
		struct mempolicy *mpol, pgoff_t ilx, int nid)
{
	return folio_alloc_noprof(gfp, order);
}
#endif

#define folio_alloc_mpol(...)			alloc_hooks(folio_alloc_mpol_noprof(__VA_ARGS__))

unsigned long alloc_pages_bulk_mempolicy_noprof(gfp_t gfp,
				unsigned long nr_pages,
				struct page **page_array);
#define  alloc_pages_bulk_mempolicy(...)				\
	alloc_hooks(alloc_pages_bulk_mempolicy_noprof(__VA_ARGS__))

#endif /* __MM_MEMPOLICY_H */
