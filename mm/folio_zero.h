/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MM_FOLIO_ZERO_H
#define MM_FOLIO_ZERO_H

#include <linux/highmem.h>

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_HUGETLBFS)
void folio_zero_user(struct folio *folio, unsigned long addr_hint);
#else
static inline void folio_zero_user(struct folio *folio, unsigned long addr_hint)
{
	unsigned long base = ALIGN_DOWN(addr_hint, folio_size(folio));

	clear_user_highpages(&folio->page, base, folio_nr_pages(folio));
}
#endif

#endif /* MM_FOLIO_ZERO_H */
