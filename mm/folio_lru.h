/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_FOLIO_LRU_H
#define _MM_FOLIO_LRU_H

/*
 * Declarations for mm/folio_lru.c.
 */

#include <linux/atomic.h>
#include <linux/mmzone.h>
#include <linux/page-flags.h>

struct folio;

void lru_note_cost_unlock_irq(struct lruvec *lruvec, bool file,
		unsigned int nr_io, unsigned int nr_rotated) __releases(lruvec->lru_lock);
void lru_note_cost_refault(struct folio *folio);
void folio_add_lru(struct folio *folio);
void folio_add_lru_vma(struct folio *folio, struct vm_area_struct *vma);
void mark_page_accessed(struct page *page);
void folio_mark_accessed(struct folio *folio);

extern atomic_t lru_disable_count;

static inline bool lru_cache_disabled(void)
{
	return atomic_read(&lru_disable_count);
}

static inline void lru_cache_enable(void)
{
	atomic_dec(&lru_disable_count);
}

void lru_cache_disable(void);
void lru_add_drain(void);
void lru_add_drain_cpu(int cpu);
void lru_add_drain_cpu_zone(struct zone *zone);
void lru_add_drain_all(void);
void folio_deactivate(struct folio *folio);
void folio_mark_lazyfree(struct folio *folio);

static inline bool folio_may_be_lru_cached(struct folio *folio)
{
	/*
	 * Holding PMD-sized folios in per-CPU LRU cache unbalances accounting.
	 * Holding small numbers of low-order mTHP folios in per-CPU LRU cache
	 * will be sensible, but nobody has implemented and tested that yet.
	 */
	return !folio_test_large(folio);
}

#endif /* _MM_FOLIO_LRU_H */
