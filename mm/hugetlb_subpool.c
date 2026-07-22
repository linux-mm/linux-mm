// SPDX-License-Identifier: GPL-2.0
/*
 * Subpool and reserve accounting for HugeTLB folios.
 * Extracted from mm/hugetlb.c
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#ifdef __KERNEL__
#include <linux/hugetlb.h>
#endif
#include <linux/spinlock.h>
#include <linux/bug.h>

#include "hugetlb_subpool.h"

static inline bool subpool_is_free(struct hugepage_subpool *spool)
{
	if (spool->count)
		return false;

	return spool->used_hpages == 0;
}

static inline void unlock_or_release_subpool(struct hugepage_subpool *spool,
						unsigned long irq_flags)
{
	bool is_free = subpool_is_free(spool);

	spin_unlock_irqrestore(&spool->lock, irq_flags);

	if (is_free) {
		if (spool->min_hpages != -1)
			hugetlb_acct_memory(spool->hstate,
						-spool->min_hpages);
		kfree(spool);
	}
}

struct hugepage_subpool *hugepage_new_subpool(struct hstate *h, long max_hpages,
						long min_hpages)
{
	struct hugepage_subpool *spool;

	spool = kzalloc_obj(*spool);
	if (!spool)
		return NULL;

	spin_lock_init(&spool->lock);
	spool->count = 1;
	spool->max_hpages = max_hpages;
	spool->hstate = h;
	spool->min_hpages = min_hpages;

	if (min_hpages != -1 && hugetlb_acct_memory(h, min_hpages)) {
		kfree(spool);
		return NULL;
	}
	spool->rsv_hpages = min_hpages;

	return spool;
}

void hugepage_put_subpool(struct hugepage_subpool *spool)
{
	unsigned long flags;

	if (!spool)
		return;

	spin_lock_irqsave(&spool->lock, flags);
	BUG_ON(!spool->count);
	spool->count--;
	unlock_or_release_subpool(spool, flags);
}

/**
 * hugepage_subpool_get_pages - Get pages from a subpool
 * @spool: pointer to subpool structure (may be NULL)
 * @delta: number of pages to allocate or reserve
 *
 * Check and update subpool page usage counts when allocating or
 * reserving @delta hugepages.
 *
 * Context: Takes spool->lock using spin_lock_irq().
 * Return: Non-negative number of reservations that cannot be
 *         satisfied by the subpool, or -ENOMEM if the subpool maximum
 *         limit would be exceeded.
 */
long hugepage_subpool_get_pages(struct hugepage_subpool *spool,
				      long delta)
{
	long ret = delta;

	if (!spool)
		return ret;

	spin_lock_irq(&spool->lock);

	if (spool->max_hpages != -1 &&
	    spool->used_hpages + delta > spool->max_hpages) {
		ret = -ENOMEM;
		goto unlock_ret;
	}

	spool->used_hpages += delta;

	/* minimum size accounting */
	if (spool->min_hpages != -1 && spool->rsv_hpages) {
		if (delta > spool->rsv_hpages) {
			/*
			 * Asking for more reserves than those already taken on
			 * behalf of subpool.  Return difference.
			 */
			ret = delta - spool->rsv_hpages;
			spool->rsv_hpages = 0;
		} else {
			ret = 0;	/* reserves already accounted for */
			spool->rsv_hpages -= delta;
		}
	}

unlock_ret:
	spin_unlock_irq(&spool->lock);
	return ret;
}

/**
 * hugepage_subpool_put_pages - Release pages back to a subpool
 * @spool: pointer to subpool structure (may be NULL)
 * @delta: number of pages to free or unreserve
 *
 * Check and update subpool page usage counts when freeing or
 * unreserving @delta hugepages.
 *
 * Context: Takes spool->lock using spin_lock_irqsave(). May release
 *          and free @spool if its usage count and references reach
 *          zero.
 * Return: Non-negative number of reservations that the subpool cannot
 *         absorb.
 */
long hugepage_subpool_put_pages(struct hugepage_subpool *spool,
				       long delta)
{
	long ret = delta;
	unsigned long flags;

	if (!spool)
		return delta;

	spin_lock_irqsave(&spool->lock, flags);

	spool->used_hpages -= delta;

	 /* minimum size accounting */
	if (spool->min_hpages != -1 && spool->used_hpages < spool->min_hpages) {
		/*
		 * limit is the maximum number of reservations that
		 * can be restored to this subpool.
		 */
		long limit = spool->min_hpages - spool->used_hpages;

		if (spool->rsv_hpages + delta <= limit)
			ret = 0;
		else
			ret = spool->rsv_hpages + delta - limit;

		spool->rsv_hpages += delta;
		if (spool->rsv_hpages > limit)
			spool->rsv_hpages = limit;
	}

	/*
	 * If hugetlbfs_put_super couldn't free spool due to an outstanding
	 * quota reference, free it now.
	 */
	unlock_or_release_subpool(spool, flags);

	return ret;
}
