/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_HUGETLB_SUBPOOL_H
#define _MM_HUGETLB_SUBPOOL_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct hstate;
struct hugepage_subpool;

struct hugepage_subpool *hugepage_new_subpool(struct hstate *h, long max_hpages,
						long min_hpages);
void hugepage_put_subpool(struct hugepage_subpool *spool);
long hugepage_subpool_get_pages(struct hugepage_subpool *spool, long delta);
long hugepage_subpool_put_pages(struct hugepage_subpool *spool, long delta);
long hugepage_subpool_free_hpages(struct hugepage_subpool *spool);
long hugepage_subpool_max_hpages(struct hugepage_subpool *spool);
unsigned long long hugepage_subpool_max_size(struct hugepage_subpool *spool);
unsigned long long hugepage_subpool_min_size(struct hugepage_subpool *spool);

#endif /* _MM_HUGETLB_SUBPOOL_H */
