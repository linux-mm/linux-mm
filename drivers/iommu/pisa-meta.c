// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * PISA sparse per-PISA-frame metadata table.
 * See Documentation/core-api/pisa.rst for the architecture overview.
 */

#include <linux/bitmap.h>
#include <linux/cache.h>
#include <linux/cacheflush.h>
#include <linux/pisa.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pgtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include "pisa-priv.h"

static_assert(PISA_META_SIZE <= PAGE_SIZE);
static_assert(PISA_SIZE <= SZ_2M);
static_assert(PISA_BLOCKS(0) <= PISA_MAX_BLOCKS);
static_assert(PISA_ORDER <= MAX_PAGE_ORDER);
static_assert(PISA_SHIFT <= SUBSECTION_SHIFT);

/* Enabled by pisa_meta_init() to activate fast-path hooks. */
DEFINE_STATIC_KEY_FALSE(pisa_key);
EXPORT_SYMBOL(pisa_key);

/* Parameters are pisa.<name> at boot and in /sys/module/pisa/parameters/. */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "pisa."

/* Physical address bits covered by @pisa_meta_array (0 = max_possible_pfn). */
static unsigned int pisa_max_bits __read_mostly;
module_param_named(max_bits, pisa_max_bits, uint, 0444);

/*
 * Metadata is in a sparse @pisa_meta_array[pfn >> PISA_ORDER] in vmalloc space
 * (256B per 2MB frame). Each 4KB backing page covers a chunk of 16 entries (32MB)
 * and is tracked in @pisa_chunk_bitmap, 1 bit per chunk, 4KB per 1TB, 1MB for 48bit
 * of address space. The total address space is @pisa_meta_nframes, all the variables
 * are set and memory allocated before enabling @pisa_key.
 */

/* Sparse per-PISA-frame metadata table, indexed by pfn >> PISA_ORDER */
struct pisa_meta *pisa_meta_array __read_mostly;
EXPORT_SYMBOL(pisa_meta_array);

/* Number of PISA frames covered by @pisa_meta_array. */
unsigned long pisa_meta_nframes __read_mostly;
EXPORT_SYMBOL(pisa_meta_nframes);

/* One bit per 32MB chunk, set once its metadata page is backed. */
unsigned long *pisa_chunk_bitmap __read_mostly;
EXPORT_SYMBOL(pisa_chunk_bitmap);

/* Shared sink entry returned for unbacked or out-of-range PFNs */
static struct pisa_meta pisa_meta_nil __read_mostly;

/* Protects init and page-table allocation */
static DEFINE_MUTEX(pisa_meta_mutex);

/* Return metadata for @pfn, or shared sink @pisa_meta_nil (flags == 0) if unbacked. */
struct pisa_meta *pisa_meta_of_pfn(unsigned long pfn)
{
	unsigned long frame = pfn >> PISA_ORDER;

	if (unlikely(!static_branch_unlikely(&pisa_key) || frame >= pisa_meta_nframes ||
		     !test_bit_acquire(pfn >> PISA_CHUNK_ORDER, pisa_chunk_bitmap)))
		return &pisa_meta_nil;

	return &pisa_meta_array[frame];
}

/* Base PFN of @meta's PISA frame, or ULONG_MAX if out-of-range/sink. */
unsigned long pisa_meta_to_pfn(const struct pisa_meta *meta)
{
	unsigned long frame = meta - pisa_meta_array;

	if (unlikely(frame >= pisa_meta_nframes))
		return ULONG_MAX;

	return frame << PISA_ORDER;
}

/* PTE for @addr in the array, or NULL if its page table was not allocated. */
static pte_t *pisa_meta_pte(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	if (pgd_none(pgdp_get(pgd)))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(p4dp_get(p4d)))
		return NULL;
	pud = pud_offset(p4d, addr);
	if (pud_none(pudp_get(pud)))
		return NULL;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(pmdp_get(pmd)))
		return NULL;
	return pte_offset_kernel(pmd, addr);
}

/* Back chunk of @pfn in @pisa_meta_array with a zeroed page and set its bitmap bit. */
static int pisa_meta_back(unsigned long pfn, gfp_t gfp)
{
	/* Protects lazy installation of backing pages into preallocated page tables. */
	static DEFINE_SPINLOCK(pisa_meta_pte_lock);
	unsigned long idx = pfn >> PISA_CHUNK_ORDER;
	unsigned long addr = (unsigned long)pisa_meta_array + (idx << PAGE_SHIFT), flags;
	struct page *online = pfn_to_online_page(pfn);
	int nid = online ? page_to_nid(online) : NUMA_NO_NODE;
	pte_t *ptep = pisa_meta_pte(addr);
	struct page *page;

	if (!ptep)
		return -EINVAL;

	page = alloc_pages_node(nid, gfp | __GFP_ZERO | __GFP_NOWARN, 0);
	if (!page)
		return -ENOMEM;

	spin_lock_irqsave(&pisa_meta_pte_lock, flags);
	if (pte_none(ptep_get(ptep))) {
		set_pte_at(&init_mm, addr, ptep, pfn_pte(page_to_pfn(page), PAGE_KERNEL));
		flush_cache_vmap(addr, addr + PAGE_SIZE);
		page = NULL;
		/* Pair with test_bit_acquire() in readers. */
		smp_mb__before_atomic();
		set_bit(idx, pisa_chunk_bitmap);
	}
	spin_unlock_irqrestore(&pisa_meta_pte_lock, flags);

	if (page)
		__free_page(page);
	return 0;
}

/* Allocate the page tables for the entries of [@start_pfn, @end_pfn). */
static int pisa_meta_prealloc(struct pisa_meta *array, unsigned long nframes,
			      unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long start, end;

	end_pfn = min(end_pfn, nframes << PISA_ORDER);
	if (start_pfn >= end_pfn)
		return 0;

	start = round_down((unsigned long)&array[start_pfn >> PISA_ORDER], PAGE_SIZE);
	end = round_up((unsigned long)&array[((end_pfn - 1) >> PISA_ORDER) + 1], PAGE_SIZE);
	return apply_to_page_range(&init_mm, start, end - start, NULL, NULL);
}

struct pisa_meta_range {
	struct pisa_meta *array;
	unsigned long nframes;
};

static int pisa_meta_prealloc_ram(unsigned long start_pfn, unsigned long nr_pages, void *arg)
{
	struct pisa_meta_range *r = arg;

	return pisa_meta_prealloc(r->array, r->nframes, start_pfn, start_pfn + nr_pages);
}

/* Page tables for memory about to go online; failure keeps it out of the allocators. */
static int pisa_meta_memory_cb(struct notifier_block *nb, unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;

	if (action != MEM_GOING_ONLINE)
		return NOTIFY_OK;

	mutex_lock(&pisa_meta_mutex);
	if (pisa_meta_array)
		pisa_meta_prealloc(pisa_meta_array, pisa_meta_nframes, mn->start_pfn,
				   mn->start_pfn + mn->nr_pages);
	mutex_unlock(&pisa_meta_mutex);
	return NOTIFY_OK;
}

/* Memory hotplug notifier for preallocating metadata page tables. */
static struct notifier_block pisa_meta_memory_nb = {
	.notifier_call = pisa_meta_memory_cb,
};

int pisa_meta_ensure_pfn(unsigned long pfn, bool can_block)
{
	unsigned long idx = pfn >> PISA_CHUNK_ORDER;

	if (!static_branch_unlikely(&pisa_key) || (pfn >> PISA_ORDER) >= pisa_meta_nframes)
		return -ERANGE;

	if (likely(test_bit_acquire(idx, pisa_chunk_bitmap)))
		return 0;

	/* Memory onlined before the notifier was registered has no page table. */
	if (can_block && !pisa_meta_pte((unsigned long)pisa_meta_array + (idx << PAGE_SHIFT))) {
		int err;

		mutex_lock(&pisa_meta_mutex);
		err = pisa_meta_prealloc(pisa_meta_array, pisa_meta_nframes, pfn, pfn + 1);
		mutex_unlock(&pisa_meta_mutex);
		if (err)
			return err;
	}

	return pisa_meta_back(pfn, can_block ? GFP_KERNEL : GFP_ATOMIC);
}

/* Reserve the sparse @pisa_meta_array and preallocate page tables for present RAM. */
int pisa_meta_init(void)
{
	unsigned long top_pfn, nframes, nchunks, size;
	struct pisa_meta_range range;
	struct pisa_meta *array;
	struct vm_struct *vm;
	int ret = 0;

	if (static_branch_unlikely(&pisa_key))
		return 0;

	mutex_lock(&pisa_meta_mutex);
	if (pisa_meta_array)
		goto out_unlock;

	if (pisa_max_bits)
		top_pfn = 1UL << (clamp_t(unsigned int, pisa_max_bits, PISA_SHIFT,
					   MAX_PHYSMEM_BITS) - PAGE_SHIFT);
	else
		top_pfn = max_t(unsigned long, max_pfn, max_possible_pfn);
	nframes = DIV_ROUND_UP(top_pfn, 1UL << PISA_ORDER);
	size = PAGE_ALIGN(nframes << PISA_META_SHIFT);
	nchunks = size >> PAGE_SHIFT;

	/* kvalloc() to avoid kmalloc limitations on large layouts */
	pisa_chunk_bitmap = kvcalloc(BITS_TO_LONGS(nchunks), sizeof(unsigned long), GFP_KERNEL);
	if (!pisa_chunk_bitmap) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	vm = get_vm_area(size, VM_MAP);
	if (!vm) {
		ret = -ENOMEM;
		goto out_free_bitmap;
	}
	array = vm->addr;

	range.array = array;
	range.nframes = nframes;
	ret = walk_system_ram_range(0, nframes << PISA_ORDER, &range, pisa_meta_prealloc_ram);
	if (ret)
		goto out_free_vm;

	pisa_meta_nframes = nframes;
	pisa_meta_array = array;
	static_branch_enable(&pisa_key);

	pr_info("pisa: %lu frames, %lu KB of KVA\n", nframes, size / 1024);
	mutex_unlock(&pisa_meta_mutex);

	/* Register outside pisa_meta_mutex (taken by the notifier under chain lock). */
	register_memory_notifier(&pisa_meta_memory_nb);
	return 0;

out_free_vm:
	free_vm_area(vm);
out_free_bitmap:
	kvfree(pisa_chunk_bitmap);
	pisa_chunk_bitmap = NULL;
out_unlock:
	mutex_unlock(&pisa_meta_mutex);
	return ret;
}
EXPORT_SYMBOL(pisa_meta_init);
