// SPDX-License-Identifier: GPL-2.0-only
/*
 * Folio split implementation, shared by the file-backed and the anonymous
 * paths.
 *
 * Extracted from mm/huge_memory.c without functional changes, so that the
 * file-backed path can later be built without transparent hugepages.
 */

#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/swapops.h>
#include <linux/backing-dev.h>
#include <linux/dax.h>
#include <linux/pagemap.h>
#include <linux/migrate.h>
#include <linux/memcontrol.h>
#include <linux/shmem_fs.h>
#include <linux/page_owner.h>
#include <linux/pgalloc_tag.h>
#include <linux/vmstat.h>

#include "internal.h"
#include "swap.h"

/*
 * A return value of 0 does not mean that unmapping succeeded. It might
 * still have failed, but remap_anon_folio() must be called afterwards,
 * for anon folios.
 */
static int unmap_folio(struct folio *folio)
{
	enum ttu_flags ttu_flags = TTU_RMAP_LOCKED | TTU_SYNC |
		TTU_BATCH_FLUSH;

	VM_BUG_ON_FOLIO(!folio_test_large(folio), folio);

	/* Racy check if we can split the page, before we split PMDs */
	if (folio_expected_ref_count(folio) != folio_ref_count(folio) - 1)
		return -EAGAIN;

	if (folio_test_pmd_mappable(folio))
		ttu_flags |= TTU_SPLIT_HUGE_PMD;

	/*
	 * Anon pages need migration entries to preserve them, but file
	 * pages can simply be left unmapped, then faulted back on demand.
	 */
	if (folio_test_anon(folio)) {
#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
		try_to_migrate(folio, ttu_flags);
#else
		return -EOPNOTSUPP;
#endif
	} else {
		try_to_unmap(folio, ttu_flags | TTU_IGNORE_MLOCK);
	}

	try_to_unmap_flush();

	return 0;
}

#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
static void remap_anon_folio(struct folio *folio, unsigned long nr, int flags)
{
	int i = 0;

	/*
	 * unmap_folio() installs migration entries only for anon folios,
	 * so currently only anon folios need to be remapped. File folios
	 * stay unmapped after the split and are faulted back on demand.
	 */
	VM_WARN_ON_FOLIO(!folio_test_anon(folio), folio);

	for (;;) {
		remove_migration_ptes(folio, folio, TTU_RMAP_LOCKED | flags);
		i += folio_nr_pages(folio);
		if (i >= nr)
			break;
		folio = folio_next(folio);
	}
}
#endif

static void lru_add_split_folio(struct folio *folio, struct folio *new_folio,
		struct lruvec *lruvec, struct list_head *list)
{
	VM_BUG_ON_FOLIO(folio_test_lru(new_folio), folio);
	lockdep_assert_held(&lruvec->lru_lock);

	if (folio_is_device_private(folio))
		return;

	if (list) {
		/* page reclaim is reclaiming a huge page */
		VM_WARN_ON(folio_test_lru(folio));
		folio_get(new_folio);
		list_add_tail(&new_folio->lru, list);
	} else {
		/* head is still on lru (and we have it frozen) */
		VM_WARN_ON(!folio_test_lru(folio));
		if (folio_test_unevictable(folio))
			new_folio->mlock_count = 0;
		else
			list_add_tail(&new_folio->lru, &folio->lru);
		folio_set_lru(new_folio);
	}
}

static bool page_range_has_hwpoisoned(struct page *page, long nr_pages)
{
	for (; nr_pages; page++, nr_pages--)
		if (PageHWPoison(page))
			return true;
	return false;
}

/*
 * It splits @folio into @new_order folios and copies the @folio metadata to
 * all the resulting folios.
 */
static void __split_folio_to_order(struct folio *folio, int old_order,
		int new_order)
{
	/* Scan poisoned pages when split a poisoned folio to large folios */
	const bool handle_hwpoison = folio_test_has_hwpoisoned(folio) && new_order;
	long new_nr_pages = 1 << new_order;
	long nr_pages = 1 << old_order;
	long i;

	folio_clear_has_hwpoisoned(folio);

	/* Check first new_nr_pages since the loop below skips them */
	if (handle_hwpoison &&
	    page_range_has_hwpoisoned(folio_page(folio, 0), new_nr_pages))
		folio_set_has_hwpoisoned(folio);
	/*
	 * Skip the first new_nr_pages, since the new folio from them have all
	 * the flags from the original folio.
	 */
	for (i = new_nr_pages; i < nr_pages; i += new_nr_pages) {
		struct page *new_head = &folio->page + i;
		/*
		 * Careful: new_folio is not a "real" folio before we cleared PageTail.
		 * Don't pass it around before clear_compound_head().
		 */
		struct folio *new_folio = (struct folio *)new_head;

		VM_BUG_ON_PAGE(atomic_read(&new_folio->_mapcount) != -1, new_head);

		/*
		 * Clone page flags before unfreezing refcount.
		 *
		 * After successful get_page_unless_zero() might follow flags change,
		 * for example lock_page() which set PG_waiters.
		 *
		 * Note that for mapped sub-pages of an anonymous THP,
		 * PG_anon_exclusive has been cleared in unmap_folio() and is stored in
		 * the migration entry instead from where remap_anon_folio() will restore it.
		 * We can still have PG_anon_exclusive set on effectively unmapped and
		 * unreferenced sub-pages of an anonymous THP: we can simply drop
		 * PG_anon_exclusive (-> PG_mappedtodisk) for these here.
		 */
		new_folio->flags.f &= ~PAGE_FLAGS_CHECK_AT_PREP;
		new_folio->flags.f |= (folio->flags.f &
				((1L << PG_referenced) |
				 (1L << PG_swapbacked) |
				 (1L << PG_swapcache) |
				 (1L << PG_mlocked) |
				 (1L << PG_uptodate) |
				 (1L << PG_active) |
				 (1L << PG_workingset) |
				 (1L << PG_locked) |
				 (1L << PG_unevictable) |
#ifdef CONFIG_ARCH_USES_PG_ARCH_2
				 (1L << PG_arch_2) |
#endif
#ifdef CONFIG_ARCH_USES_PG_ARCH_3
				 (1L << PG_arch_3) |
#endif
				 (1L << PG_dirty) |
				 (1L << PG_dropbehind) |
				 LRU_GEN_MASK | LRU_REFS_MASK));

		new_folio->mapping = folio->mapping;
		new_folio->index = folio->index + i;

		/*
		 * page->private should not be set in tail pages. Warn once
		 * if private is unexpectedly set. Do it before swap.val assignment
		 * since private overlaps with swap.val.
		 */
		VM_WARN_ON_ONCE_PAGE(new_folio->private, new_head);

		/*
		 * Not all folio fields are valid during a split, so open-code
		 * the swap entry rather than using folio_swap_entry().
		 */
		if (folio_test_swapcache(folio))
			new_folio->swap.val = folio->swap.val + i;

		/* Page flags must be visible before we make the page non-compound. */
		smp_wmb();

		/*
		 * Clear PageTail before unfreezing page refcount.
		 *
		 * After successful get_page_unless_zero() might follow put_page()
		 * which needs correct compound_head().
		 */
		clear_compound_head(new_head);
		if (new_order) {
			prep_compound_page(new_head, new_order);
			folio_set_large_rmappable(new_folio);
		}

		/*
		 * PG_has_hwpoisoned is on the 2nd page, so set it after
		 * the compound head is prepped.
		 */
		if (handle_hwpoison &&
		    page_range_has_hwpoisoned(new_head, new_nr_pages))
			folio_set_has_hwpoisoned(new_folio);

		if (folio_test_young(folio))
			folio_set_young(new_folio);
		if (folio_test_idle(folio))
			folio_set_idle(new_folio);
#ifdef CONFIG_MEMCG
		new_folio->memcg_data = folio->memcg_data;
#endif

		folio_xchg_last_cpupid(new_folio, folio_last_cpupid(folio));
	}

	if (new_order)
		folio_set_order(folio, new_order);
	else
		ClearPageCompound(&folio->page);
}

/**
 * __split_frozen_folio() - splits a frozen @folio to lower order folios
 * in two ways: uniform split or non-uniform split.
 * @folio: the to-be-split folio
 * @new_order: the smallest order of the after split folios (since buddy
 *             allocator like split generates folios with orders from @folio's
 *             order - 1 to new_order).
 * @split_at: in buddy allocator like split, the folio containing @split_at
 *            will be split until its order becomes @new_order.
 * @xas: xa_state pointing to folio->mapping->i_pages and locked by caller
 * @split_type: if the split is uniform or not (buddy allocator like split)
 *
 *
 * 1. uniform split: the given @folio into multiple @new_order small folios,
 *    where all small folios have the same order. This is done when
 *    split_type is SPLIT_TYPE_UNIFORM.
 * 2. buddy allocator like (non-uniform) split: the given @folio is split into
 *    half and one of the half (containing the given page) is split into half
 *    until the given @folio's order becomes @new_order. This is done when
 *    split_type is SPLIT_TYPE_NON_UNIFORM.
 *
 * The high level flow for these two methods are:
 *
 * 1. uniform split: @xas is split with no expectation of failure and a single
 *    __split_folio_to_order() is called to split the @folio into @new_order
 *    along with stats update.
 * 2. non-uniform split: folio_order - @new_order calls to
 *    __split_folio_to_order() are expected to be made in a for loop to split
 *    the @folio to one lower order at a time. The folio containing @split_at
 *    is split in each iteration. @xas is split into half in each iteration and
 *    can fail. A failed @xas split leaves split folios as is without merging
 *    them back.
 *
 * After splitting, the caller's folio reference will be transferred to the
 * folio containing @split_at. The caller needs to unlock and/or free
 * after-split folios if necessary.
 *
 * Return: 0 - successful, <0 - failed (if -ENOMEM is returned, @folio might be
 * split but not to @new_order, the caller needs to check)
 */
static int __split_frozen_folio(struct folio *folio, int new_order,
		struct page *split_at, struct xa_state *xas,
		enum split_type split_type)
{
	const bool is_anon = folio_test_anon(folio);
	int old_order = folio_order(folio);
	int start_order = split_type == SPLIT_TYPE_UNIFORM ? new_order : old_order - 1;
	struct folio *old_folio = folio;
	int split_order;

	/* Frozen implies unmapped, callers unmap before splitting. */
	VM_WARN_ON_ONCE_FOLIO(folio_mapped(folio), folio);

	/*
	 * split to new_order one order at a time. For uniform split,
	 * folio is split to new_order directly.
	 */
	for (split_order = start_order;
	     split_order >= new_order;
	     split_order--) {
		int nr_new_folios = 1UL << (old_order - split_order);

		/* order-1 anonymous folio is not supported */
		if (is_anon && split_order == 1)
			continue;

		if (xas) {
			/*
			 * uniform split has xas_split_alloc() called before
			 * irq is disabled to allocate enough memory, whereas
			 * non-uniform split can handle ENOMEM.
			 * Use the to-be-split folio, so that a parallel
			 * folio_try_get() waits on it until xarray is updated
			 * with after-split folios and the original one is
			 * unfrozen.
			 */
			if (split_type == SPLIT_TYPE_UNIFORM) {
				xas_split(xas, old_folio, old_order);
			} else {
				xas_set_order(xas, folio->index, split_order);
				xas_try_split(xas, old_folio, old_order);
				if (xas_error(xas))
					return xas_error(xas);
			}
		}

		folio_split_memcg_refs(folio, old_order, split_order);
		split_page_owner(&folio->page, old_order, split_order);
		pgalloc_tag_split(folio, old_order, split_order);
		__split_folio_to_order(folio, old_order, split_order);

		if (is_anon) {
			mod_mthp_stat(old_order, MTHP_STAT_NR_ANON, -1);
			mod_mthp_stat(split_order, MTHP_STAT_NR_ANON, nr_new_folios);
		}
		/*
		 * If uniform split, the process is complete.
		 * If non-uniform, continue splitting the folio at @split_at
		 * as long as the next @split_order is >= @new_order.
		 */
		folio = page_folio(split_at);
		old_order = split_order;
	}

	return 0;
}

/**
 * folio_check_splittable() - check if a folio can be split to a given order
 * @folio: folio to be split
 * @new_order: the smallest order of the after split folios (since buddy
 *             allocator like split generates folios with orders from @folio's
 *             order - 1 to new_order).
 * @split_type: uniform or non-uniform split
 *
 * folio_check_splittable() checks if @folio can be split to @new_order using
 * @split_type method. The truncated folio check must come first.
 *
 * Context: folio must be locked.
 *
 * Return: 0 - @folio can be split to @new_order, otherwise an error number is
 * returned.
 */
int folio_check_splittable(struct folio *folio, unsigned int new_order,
			   enum split_type split_type)
{
	const bool is_anon = folio_test_anon(folio);
	const bool is_swapcache = folio_test_swapcache(folio);

	VM_WARN_ON_FOLIO(!folio_test_locked(folio), folio);
	/*
	 * Folios that just got truncated cannot get split. Signal to the
	 * caller that there was a race.
	 *
	 * TODO: this will also currently refuse folios without a mapping in the
	 * swapcache (shmem or to-be-anon folios).
	 */
	if (!folio->mapping && !is_anon)
		return -EBUSY;

	/* order-1 is not supported for anonymous THP. */
	if (is_anon && new_order == 1)
		return -EINVAL;

	/*
	 * swapcache folio could only be split to order 0
	 *
	 * non-uniform split creates after-split folios with orders from
	 * folio_order(folio) - 1 to new_order, making it not suitable for any
	 * swapcache folio split. Only uniform split to order-0 can be used
	 * here.
	 */
	if ((split_type == SPLIT_TYPE_NON_UNIFORM || new_order) && is_swapcache)
		return -EINVAL;

	if (is_huge_zero_folio(folio))
		return -EINVAL;

	if (folio_test_writeback(folio))
		return -EBUSY;

	/*
	 * A non-anon swapcache folio that still has a mapping can only be a
	 * shmem folio under SWAP IO, it's removed from either swap cache or
	 * shmem mapping afterward. There is little benefit in splitting them
	 * hence reject it here up front before touching anything.
	 */
	if (!is_anon && is_swapcache && folio->mapping)
		return -EBUSY;

	return 0;
}

#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
/* Number of folio references from the swapcache. */
static unsigned int folio_swapcache_ref_count(const struct folio *folio)
{
	if (!folio_test_swapcache(folio))
		return 0;
	return folio_nr_pages(folio);
}
#endif

void folio_reset_partially_mapped(struct folio *folio)
{
	/* Folio must be frozen. */
	VM_WARN_ON_FOLIO(folio_ref_count(folio), folio);

	if (!folio_test_partially_mapped(folio))
		return;

	/*
	 * Order-1 folios have no _deferred_list. The flag is only ever set
	 * on folios that do, so the list can be checked after the flag.
	 */
	VM_WARN_ON_FOLIO(!list_empty(&folio->_deferred_list), folio);

	folio_clear_partially_mapped(folio);
	mod_mthp_stat(folio_order(folio),
		      MTHP_STAT_NR_ANON_PARTIALLY_MAPPED, -1);
}

#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
static int __folio_freeze_split_anon(struct folio *folio,
		unsigned int new_order, struct page *split_at, bool do_lru,
		struct list_head *list, enum split_type split_type)
{
	struct folio *end_folio = folio_next(folio);
	struct swap_cluster_info *ci = NULL;
	const int old_order = folio_order(folio);
	struct folio *new_folio, *next;
	struct anon_vma *anon_vma = NULL;
	enum ttu_flags ttu_flags = 0;
	struct lruvec *lruvec;
	int ret = 0;

	/*
	 * Unmap/remap needs the anon_vma, so we first take a reference on
	 * it to prevent it from disappearing, and lock it for write here,
	 * letting unmap_folio() walk the rmap with TTU_RMAP_LOCKED.
	 *
	 * folio_mapped() is not stable here, but it can only change in
	 * one direction while the folio is locked. The mapcount can drop
	 * to zero at any time, zap_pte_range() takes no folio lock. It
	 * cannot go up: swapin, migration and uffd move all lock the folio
	 * before mapping it, and fork only copies PTEs that already exist.
	 *
	 * So if we see the folio mapped, the worst case is an empty rmap
	 * walk. If we see it unmapped, it stays unmapped and needs neither
	 * the reference nor the lock. Anything else needs a reference
	 * first and folio_ref_freeze() below catches it.
	 *
	 * Note that entirely swapped-out THPs are unmapped but can be split.
	 */
	if (folio_mapped(folio)) {
		anon_vma = folio_get_anon_vma(folio);
		if (!anon_vma)
			return -EBUSY;
		anon_vma_lock_write(anon_vma);
		ret = unmap_folio(folio);
		if (ret)
			goto out_unlock;
	}

	local_irq_disable();

	if (!folio_ref_freeze(folio, folio_swapcache_ref_count(folio) + 1)) {
		ret = -EAGAIN;
		goto out_no_split;
	}

	/* Take off the deferred split queue while frozen and memcg set */
	folio_unqueue_deferred_split(folio);

	/*
	 * deferred_split_scan() takes the folio off the queue before it
	 * splits it, so the unqueue above finds an empty list and
	 * leaves PG_partially_mapped set.
	 * Clear it here: the flag does not survive the split.
	 */
	folio_reset_partially_mapped(folio);

	if (folio_test_swapcache(folio))
		ci = swap_cluster_get_and_lock(folio);

	if (do_lru)
		lruvec = folio_lruvec_lock(folio);

	ret = __split_frozen_folio(folio, new_order, split_at, NULL, split_type);

	/*
	 * Unfreeze the after-split folios and put them back to the right
	 * place. Keep the head @folio frozen until the end: sub entries
	 * in swap cache must be updated first, so a concurrent
	 * swap_cache_get_folio() cannot return the head folio for a sub
	 * entry (folio_try_get() will fail on the head @folio until unfreeze).
	 */
	for (new_folio = folio_next(folio); new_folio != end_folio;
	     new_folio = next) {
		next = folio_next(new_folio);
		zone_device_private_split_cb(folio, new_folio);
		folio_ref_unfreeze(new_folio,
				   folio_swapcache_ref_count(new_folio) + 1);
		if (do_lru)
			lru_add_split_folio(folio, new_folio, lruvec, list);
		if (ci)
			__swap_cache_replace_folio(ci, folio, new_folio);
	}

	zone_device_private_split_cb(folio, NULL);
	folio_ref_unfreeze(folio, folio_swapcache_ref_count(folio) + 1);

	if (do_lru)
		lruvec_unlock(lruvec);
	if (ci)
		swap_cluster_unlock(ci);
out_no_split:
	local_irq_enable();
	if (anon_vma) {
		if (!ret && !folio_is_device_private(folio))
			ttu_flags = TTU_USE_SHARED_ZEROPAGE;
		remap_anon_folio(folio, 1 << old_order, ttu_flags);
	}
out_unlock:
	if (anon_vma) {
		anon_vma_unlock_write(anon_vma);
		put_anon_vma(anon_vma);
	}

	return ret;
}
#endif

static int __folio_freeze_split_file(struct folio *folio,
		unsigned int new_order, struct page *split_at,
		struct list_head *list, enum split_type split_type)
{
	const long old_nr_pages = folio_nr_pages(folio);
	struct address_space *mapping = folio->mapping;
	XA_STATE(xas, &mapping->i_pages, folio->index);
	struct folio *end_folio = folio_next(folio);
	struct mem_cgroup *memcg, *old_memcg;
	struct folio *new_folio, *next;
	int nr_shmem_dropped = 0;
	unsigned int min_order;
	struct lruvec *lruvec;
	pgoff_t end;
	gfp_t gfp;
	int ret = 0;

	min_order = mapping_min_folio_order(mapping);
	if (new_order < min_order)
		return -EINVAL;

	/*
	 * Switch to folio's memcg as xarray node allocation can happen and
	 * needs to charge to it.
	 */
	memcg = get_mem_cgroup_from_folio(folio);
	old_memcg = set_active_memcg(memcg);

	gfp = current_gfp_context(mapping_gfp_mask(mapping) & GFP_RECLAIM_MASK);
	if (!filemap_release_folio(folio, gfp)) {
		ret = -EBUSY;
		goto fail_free;
	}

	mapping_set_update(&xas, mapping);

	if (split_type == SPLIT_TYPE_UNIFORM) {
		const int old_order = folio_order(folio);

		xas_set_order(&xas, folio->index, new_order);
		xas_split_alloc(&xas, folio, old_order, gfp);
		if (xas_error(&xas)) {
			ret = xas_error(&xas);
			goto fail_free;
		}
	}

	i_mmap_lock_read(mapping);

	/* Currently device private folios can only back anonymous memory. */
	VM_WARN_ON_ONCE_FOLIO(folio_is_device_private(folio), folio);

	/*
	 * The loop below may need to trim off pages beyond
	 * EOF: but on 32-bit, i_size_read() takes an irq-unsafe
	 * seqlock, which cannot be nested inside the page tree lock.
	 * So note end now: i_size itself may be changed at any moment,
	 * but folio lock is good enough to serialize the trimming.
	 */
	end = DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE);
	if (shmem_mapping(mapping))
		end = shmem_fallocend(mapping->host, end);

	ret = unmap_folio(folio);
	if (ret)
		goto fail_mmap_unlock;

	xas_lock_irq(&xas);

	/*
	 * Check if the folio is present in page cache.
	 * We assume all tail are present too, if folio is there.
	 */
	if (xas_load(&xas) != folio) {
		ret = -EAGAIN;
		goto fail;
	}

	if (!folio_ref_freeze(folio, old_nr_pages + 1)) {
		ret = -EAGAIN;
		goto fail;
	}

#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
	if (folio_test_pmd_mappable(folio) && new_order < HPAGE_PMD_ORDER) {
		if (folio_test_swapbacked(folio))
			lruvec_stat_mod_folio(folio, NR_SHMEM_THPS, -old_nr_pages);
		else
			lruvec_stat_mod_folio(folio, NR_FILE_THPS, -old_nr_pages);
	}
#endif

	/* lock lru list/PageCompound, ref frozen by page_ref_freeze */
	lruvec = folio_lruvec_lock(folio);
	ret = __split_frozen_folio(folio, new_order, split_at, &xas, split_type);

	/*
	 * Unfreeze after-split folios and put them back to the right
	 * list. @folio should be kept frozen until page cache
	 * entries are updated with all the other after-split folios
	 * to prevent others seeing stale page cache entries.
	 * As a result, new_folio starts from the next folio of
	 * @folio.
	 */
	for (new_folio = folio_next(folio); new_folio != end_folio;
	     new_folio = next) {
		unsigned long nr_pages = folio_nr_pages(new_folio);

		/* compute next before the folio can be freed below */
		next = folio_next(new_folio);

		folio_ref_unfreeze(new_folio,
				   folio_nr_pages(new_folio) + 1);

		lru_add_split_folio(folio, new_folio, lruvec, list);

		/* Add the new folio to the page cache. */
		if (new_folio->index < end) {
			__xa_store(&mapping->i_pages, new_folio->index,
				   new_folio, 0);
			continue;
		}

		/* Drop folio beyond EOF: ->index >= end */
		if (shmem_mapping(mapping))
			nr_shmem_dropped += nr_pages;
		else if (folio_test_clear_dirty(new_folio))
			folio_account_cleaned(
				new_folio, inode_to_wb(mapping->host));
		__filemap_remove_folio(new_folio, NULL);
		folio_put_refs(new_folio, nr_pages);
	}

	/*
	 * Unfreeze @folio only after all page cache entries, which
	 * used to point to it, have been updated with new folios.
	 * Otherwise, a parallel folio_try_get() can grab @folio
	 * and its caller can see stale page cache entries.
	 */
	folio_ref_unfreeze(folio, folio_nr_pages(folio) + 1);
	lruvec_unlock(lruvec);
fail:
	xas_unlock_irq(&xas);
fail_mmap_unlock:
	if (nr_shmem_dropped)
		shmem_uncharge(mapping->host, nr_shmem_dropped);
	/*
	 * Drop the mapping while the inode is still pinned. @folio stays
	 * locked and present in the page cache, so eviction cannot free
	 * the inode yet, nothing past this point may touch the inode or
	 * the mapping.
	 */
	i_mmap_unlock_read(mapping);
fail_free:
	/* Restore the previously active memcg */
	set_active_memcg(old_memcg);
	mem_cgroup_put(memcg);
	xas_destroy(&xas);
	return ret;
}

/**
 * __folio_split() - split a folio at @split_at to a @new_order folio
 * @folio: folio to split
 * @new_order: the order of the new folio
 * @split_at: a page within the new folio
 * @lock_at: a page within @folio to be left locked to caller
 * @list: after-split folios will be put on it if non NULL
 * @split_type: perform uniform split or not (non-uniform split)
 *
 * It calls __split_frozen_folio() to perform uniform and non-uniform split.
 * It is in charge of checking whether the split is supported or not and
 * preparing @folio for __split_frozen_folio().
 *
 * After splitting, the after-split folio containing @lock_at remains locked
 * and others are unlocked:
 * 1. for uniform split, @lock_at points to one of @folio's subpages;
 * 2. for buddy allocator like (non-uniform) split, @lock_at points to @folio.
 *
 * Return: 0 - successful, <0 - failed (if -ENOMEM is returned, @folio might be
 * split but not to @new_order, the caller needs to check)
 */
static int __folio_split(struct folio *folio, unsigned int new_order,
		struct page *split_at, struct page *lock_at,
		struct list_head *list, enum split_type split_type)
{
	struct folio *end_folio = folio_next(folio);
	const bool is_anon = folio_test_anon(folio);
	const bool is_swapcache = folio_test_swapcache(folio);
	int old_order = folio_order(folio);
	struct folio *new_folio, *next;
	int ret;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_large(folio), folio);

	if (folio != page_folio(split_at) || folio != page_folio(lock_at)) {
		ret = -EINVAL;
		goto out;
	}

	if (new_order >= old_order) {
		ret = -EINVAL;
		goto out;
	}

	ret = folio_check_splittable(folio, new_order, split_type);
	if (ret) {
		VM_WARN_ONCE(ret == -EINVAL, "Tried to split an unsplittable folio");
		goto out;
	}

	if (is_anon) {
#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
		ret = __folio_freeze_split_anon(folio, new_order, split_at,
						true, list, split_type);
#else
		ret = -EOPNOTSUPP;
#endif
	} else {
		ret = __folio_freeze_split_file(folio, new_order, split_at,
						list, split_type);
	}

	/*
	 * Unlock all after-split folios except the one containing
	 * @lock_at page. If @folio is not split, it will be kept locked.
	 */
	for (new_folio = folio; new_folio != end_folio; new_folio = next) {
		next = folio_next(new_folio);
		if (new_folio == page_folio(lock_at))
			continue;

		/*
		 * Subpages whose mapping has been zapped may be freed
		 * earlier, but freeing them requires taking the
		 * lru_lock, so we defer folio_put() on tail pages until
		 * after the split completes.
		 */
		if (is_swapcache && !folio_mapped(new_folio))
			folio_free_swap(new_folio);
		folio_unlock(new_folio);
		folio_put(new_folio);
	}

out:
#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
	if (is_pmd_order(old_order))
		count_vm_event(!ret ? THP_SPLIT_PAGE : THP_SPLIT_PAGE_FAILED);
#endif
	count_mthp_stat(old_order, !ret ? MTHP_STAT_SPLIT : MTHP_STAT_SPLIT_FAILED);
	return ret;
}

#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
/**
 * folio_split_unmapped() - split a large anon folio that is already unmapped
 * @folio: folio to split
 * @new_order: the order of folios after split
 *
 * This function is a helper for splitting folios that have already been
 * unmapped. The use case is that the device or the CPU can refuse to migrate
 * THP pages in the middle of migration, due to allocation issues on either
 * side.
 *
 * The anon rmap lock is not required to be held, mmap_read_lock() or
 * mmap_write_lock() should be held. @folio is expected to be locked by the
 * caller. device-private and non device-private folios are supported along
 * with folios that are in the swapcache. @folio should also be unmapped and
 * isolated from LRU (if applicable)
 *
 * Upon return, the folio is not remapped, split folios are not added to LRU,
 * folio_free_swap() is not called, and new folios remain locked.
 *
 * Return: 0 on success, -EAGAIN if the folio cannot be split (e.g., due to
 *         insufficient reference count or extra pins).
 */
int folio_split_unmapped(struct folio *folio, unsigned int new_order)
{
	VM_WARN_ON_ONCE_FOLIO(folio_mapped(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_large(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_anon(folio), folio);

	return __folio_freeze_split_anon(folio, new_order, &folio->page,
					 false, NULL, SPLIT_TYPE_UNIFORM);
}
#endif

/*
 * This function splits a large folio into smaller folios of order @new_order.
 * @page can point to any page of the large folio to split. The split operation
 * does not change the position of @page.
 *
 * Prerequisites:
 *
 * 1) The caller must hold a reference on the @page's owning folio, also known
 *    as the large folio.
 *
 * 2) The large folio must be locked.
 *
 * 3) The folio must not be pinned. Any unexpected folio references, including
 *    GUP pins, will result in the folio not getting split; instead, the caller
 *    will receive an -EAGAIN.
 *
 * 4) @new_order > 1, usually. Splitting to order-1 anonymous folios is not
 *    supported for non-file-backed folios, because folio->_deferred_list, which
 *    is used by partially mapped folios, is stored in subpage 2, but an order-1
 *    folio only has subpages 0 and 1. File-backed order-1 folios are supported,
 *    since they do not use _deferred_list.
 *
 * After splitting, the caller's folio reference will be transferred to @page,
 * resulting in a raised refcount of @page after this call. The other pages may
 * be freed if they are not mapped.
 *
 * If @list is null, tail pages will be added to LRU list, otherwise, to @list.
 *
 * Pages in @new_order will inherit the mapping, flags, and so on from the
 * huge page.
 *
 * Returns 0 if the huge page was split successfully.
 *
 * Returns -EAGAIN if the folio has unexpected reference (e.g., GUP) or if
 * the folio was concurrently removed from the page cache.
 *
 * Returns -EBUSY when trying to split the huge zeropage, if the folio is
 * under writeback, if fs-specific folio metadata cannot currently be
 * released, or if some unexpected race happened (e.g., anon VMA disappeared,
 * truncation).
 *
 * Callers should ensure that the order respects the address space mapping
 * min-order if one is set for non-anonymous folios.
 *
 * Returns -EINVAL when trying to split to an order that is incompatible
 * with the folio. Splitting to order 0 is compatible with all folios.
 */
int __split_huge_page_to_list_to_order(struct page *page, struct list_head *list,
				     unsigned int new_order)
{
	struct folio *folio = page_folio(page);

	return __folio_split(folio, new_order, &folio->page, page, list,
			     SPLIT_TYPE_UNIFORM);
}

/**
 * folio_split() - split a folio at @split_at to a @new_order folio
 * @folio: folio to split
 * @new_order: the order of the new folio
 * @split_at: a page within the new folio
 * @list: after-split folios are added to @list if not null, otherwise to LRU
 *        list
 *
 * It has the same prerequisites and returns as
 * split_huge_page_to_list_to_order().
 *
 * Split a folio at @split_at to a new_order folio, leave the
 * remaining subpages of the original folio as large as possible. For example,
 * in the case of splitting an order-9 folio at its third order-3 subpages to
 * an order-3 folio, there are 2^(9-3)=64 order-3 subpages in the order-9 folio.
 * After the split, there will be a group of folios with different orders and
 * the new folio containing @split_at is marked in bracket:
 * [order-4, {order-3}, order-3, order-5, order-6, order-7, order-8].
 *
 * After split, folio is left locked for caller.
 *
 * Return: 0 - successful, <0 - failed (if -ENOMEM is returned, @folio might be
 * split but not to @new_order, the caller needs to check)
 */
int folio_split(struct folio *folio, unsigned int new_order,
		struct page *split_at, struct list_head *list)
{
	return __folio_split(folio, new_order, split_at, &folio->page, list,
			     SPLIT_TYPE_NON_UNIFORM);
}

/**
 * min_order_for_split() - get the minimum order @folio can be split to
 * @folio: folio to split
 *
 * min_order_for_split() tells the minimum order @folio can be split to.
 * If a file-backed folio is truncated, 0 will be returned. Any subsequent
 * split attempt should get -EBUSY from split checking code.
 *
 * Context: @folio must be locked.
 *
 * Return: @folio's minimum order for split
 */
unsigned int min_order_for_split(struct folio *folio)
{
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);

	if (folio_test_anon(folio))
		return 0;

	/*
	 * If the folio got truncated, we don't know the previous mapping and
	 * consequently the old min order. But it doesn't matter, as any split
	 * attempt will immediately fail with -EBUSY as the folio cannot get
	 * split until freed.
	 */
	if (!folio->mapping)
		return 0;

	return mapping_min_folio_order(folio->mapping);
}

int split_folio_to_list(struct folio *folio, struct list_head *list)
{
	return split_huge_page_to_list_to_order(&folio->page, list, 0);
}
