// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains KASAN shadow initialization code.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 */

#include <linux/memblock.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/pgalloc.h>

#include <asm/page.h>

#include "../pgalloc-track.h"
#include "kasan.h"

/*
 * This page serves two purposes:
 *   - It used as early shadow memory. The entire shadow region populated
 *     with this page, before we will be able to setup normal shadow memory.
 *   - Latter it reused it as zero shadow to cover large ranges of memory
 *     that allowed to access, but not handled by kasan (vmalloc/vmemmap ...).
 */
unsigned char kasan_early_shadow_page[PAGE_SIZE] __bss_pgtbl;

#if CONFIG_PGTABLE_LEVELS > 4
p4d_t kasan_early_shadow_p4d[MAX_PTRS_PER_P4D] __bss_pgtbl;
static inline bool kasan_p4d_table(pgd_t pgd)
{
	return pgd_page(pgd) == virt_to_page(lm_alias(kasan_early_shadow_p4d));
}
#else
static inline bool kasan_p4d_table(pgd_t pgd)
{
	return false;
}
#endif
#if CONFIG_PGTABLE_LEVELS > 3
pud_t kasan_early_shadow_pud[MAX_PTRS_PER_PUD] __bss_pgtbl;
static inline bool kasan_pud_table(p4d_t p4d)
{
	return p4d_page(p4d) == virt_to_page(lm_alias(kasan_early_shadow_pud));
}
#else
static inline bool kasan_pud_table(p4d_t p4d)
{
	return false;
}
#endif
#if CONFIG_PGTABLE_LEVELS > 2
pmd_t kasan_early_shadow_pmd[MAX_PTRS_PER_PMD] __bss_pgtbl;
static inline bool kasan_pmd_table(pud_t pud)
{
	return pud_page(pud) == virt_to_page(lm_alias(kasan_early_shadow_pmd));
}
#else
static inline bool kasan_pmd_table(pud_t pud)
{
	return false;
}
#endif
pte_t kasan_early_shadow_pte[MAX_PTRS_PER_PTE + PTE_HWTABLE_PTRS]
	__bss_pgtbl;

static inline bool kasan_pte_table(pmd_t pmd)
{
	return pmd_page(pmd) == virt_to_page(lm_alias(kasan_early_shadow_pte));
}

static inline bool kasan_early_shadow_page_entry(pte_t pte)
{
	return pte_page(pte) == virt_to_page(lm_alias(kasan_early_shadow_page));
}

static __init void *early_alloc(size_t size, int node)
{
	void *ptr = memblock_alloc_try_nid(size, size, __pa(MAX_DMA_ADDRESS),
					   MEMBLOCK_ALLOC_ACCESSIBLE, node);

	if (!ptr)
		panic("%s: Failed to allocate %zu bytes align=%zx nid=%d from=%llx\n",
		      __func__, size, size, node, (u64)__pa(MAX_DMA_ADDRESS));

	return ptr;
}

static void __ref zero_pte_populate(pmd_t *pmd, unsigned long addr,
				unsigned long end)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);
	pte_t zero_pte;

	zero_pte = pfn_pte(PFN_DOWN(__pa_symbol(kasan_early_shadow_page)),
				PAGE_KERNEL);
	zero_pte = pte_wrprotect(zero_pte);

	while (addr + PAGE_SIZE <= end) {
		set_pte_at(&init_mm, addr, pte, zero_pte);
		addr += PAGE_SIZE;
		pte = pte_offset_kernel(pmd, addr);
	}
}

static int __ref zero_pmd_populate(pud_t *pud, unsigned long addr,
				unsigned long end, pgtbl_mod_mask *mask)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		next = pmd_addr_end(addr, end);

		if (IS_ALIGNED(addr, PMD_SIZE) && end - addr >= PMD_SIZE &&
		    pmd_none(*pmd)) {
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			*mask |= PGTBL_PMD_MODIFIED;
			continue;
		}

		if (pmd_none(*pmd)) {
			if (slab_is_available()) {
				if (!pte_alloc_kernel_track(pmd, addr, mask))
					return -ENOMEM;
			} else {
				pte_t *p;

				p = early_alloc(PAGE_SIZE, NUMA_NO_NODE);
				kernel_pte_init(p);
				pmd_populate_kernel(&init_mm, pmd, p);
				*mask |= PGTBL_PMD_MODIFIED;
			}
		}
		zero_pte_populate(pmd, addr, next);
	} while (pmd++, addr = next, addr != end);

	return 0;
}

static int __ref zero_pud_populate(p4d_t *p4d, unsigned long addr,
				unsigned long end, pgtbl_mod_mask *mask)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;
	int ret;

	do {
		next = pud_addr_end(addr, end);
		if (IS_ALIGNED(addr, PUD_SIZE) && end - addr >= PUD_SIZE &&
		    pud_none(*pud)) {
			pmd_t *pmd;

			pud_populate(&init_mm, pud,
					lm_alias(kasan_early_shadow_pmd));
			*mask |= PGTBL_PUD_MODIFIED;
			pmd = pmd_offset(pud, addr);
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			*mask |= PGTBL_PMD_MODIFIED;
			continue;
		}

		if (pud_none(*pud)) {
			if (slab_is_available()) {
				if (!pmd_alloc_track(&init_mm, pud, addr, mask))
					return -ENOMEM;
			} else {
				pmd_t *p;

				p = early_alloc(PAGE_SIZE, NUMA_NO_NODE);
				pmd_init(p);
				pud_populate(&init_mm, pud, p);
				*mask |= PGTBL_PUD_MODIFIED;
			}
		}
		ret = zero_pmd_populate(pud, addr, next, mask);
		if (ret)
			return ret;
	} while (pud++, addr = next, addr != end);

	return 0;
}

static int __ref zero_p4d_populate(pgd_t *pgd, unsigned long addr,
				unsigned long end, pgtbl_mod_mask *mask)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;
	int ret;

	do {
		next = p4d_addr_end(addr, end);
		if (IS_ALIGNED(addr, P4D_SIZE) && end - addr >= P4D_SIZE &&
		    p4d_none(*p4d)) {
			pud_t *pud;
			pmd_t *pmd;

			p4d_populate(&init_mm, p4d,
					lm_alias(kasan_early_shadow_pud));
			*mask |= PGTBL_P4D_MODIFIED;
			pud = pud_offset(p4d, addr);
			pud_populate(&init_mm, pud,
					lm_alias(kasan_early_shadow_pmd));
			*mask |= PGTBL_PUD_MODIFIED;
			pmd = pmd_offset(pud, addr);
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			*mask |= PGTBL_PMD_MODIFIED;
			continue;
		}

		if (p4d_none(*p4d)) {
			if (slab_is_available()) {
				if (!pud_alloc_track(&init_mm, p4d, addr, mask))
					return -ENOMEM;
			} else {
				pud_t *p;

				p = early_alloc(PAGE_SIZE, NUMA_NO_NODE);
				pud_init(p);
				p4d_populate(&init_mm, p4d, p);
				*mask |= PGTBL_P4D_MODIFIED;
			}
		}
		ret = zero_pud_populate(p4d, addr, next, mask);
		if (ret)
			return ret;
	} while (p4d++, addr = next, addr != end);

	return 0;
}

/**
 * kasan_populate_early_shadow - populate shadow memory region with
 *                               kasan_early_shadow_page
 * @shadow_start: start of the memory range to populate
 * @shadow_end: end of the memory range to populate
 */
int __ref kasan_populate_early_shadow(const void *shadow_start,
					const void *shadow_end)
{
	unsigned long start = (unsigned long)shadow_start;
	unsigned long addr = start;
	unsigned long end = (unsigned long)shadow_end;
	pgd_t *pgd = pgd_offset_k(addr);
	unsigned long next;
	pgtbl_mod_mask mask = 0;
	int ret = 0;

	do {
		next = pgd_addr_end(addr, end);

		if (IS_ALIGNED(addr, PGDIR_SIZE) && end - addr >= PGDIR_SIZE &&
		    pgd_none(*pgd)) {
			p4d_t *p4d;
			pud_t *pud;
			pmd_t *pmd;

			/*
			 * kasan_early_shadow_pud should be populated with pmds
			 * at this moment.
			 * [pud,pmd]_populate*() below needed only for
			 * 3,2 - level page tables where we don't have
			 * puds,pmds, so pgd_populate(), pud_populate()
			 * is noops.
			 */
			pgd_populate(&init_mm, pgd,
					lm_alias(kasan_early_shadow_p4d));
			mask |= PGTBL_PGD_MODIFIED;
			p4d = p4d_offset(pgd, addr);
			p4d_populate(&init_mm, p4d,
					lm_alias(kasan_early_shadow_pud));
			mask |= PGTBL_P4D_MODIFIED;
			pud = pud_offset(p4d, addr);
			pud_populate(&init_mm, pud,
					lm_alias(kasan_early_shadow_pmd));
			mask |= PGTBL_PUD_MODIFIED;
			pmd = pmd_offset(pud, addr);
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			mask |= PGTBL_PMD_MODIFIED;
			continue;
		}

		if (pgd_none(*pgd)) {

			if (slab_is_available()) {
				if (!p4d_alloc_track(&init_mm, pgd, addr, &mask)) {
					ret = -ENOMEM;
					break;
				}
			} else {
				pgd_populate(&init_mm, pgd,
					early_alloc(PAGE_SIZE, NUMA_NO_NODE));
				mask |= PGTBL_PGD_MODIFIED;
			}
		}
		ret = zero_p4d_populate(pgd, addr, next, &mask);
		if (ret)
			break;
	} while (pgd++, addr = next, addr != end);

	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);

	return ret;
}

static void kasan_free_pte(pte_t *pte_start, pmd_t *pmd)
{
	pte_t *pte;
	int i;

	if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_PMD_MODIFIED)
		return;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte = pte_start + i;
		if (!pte_none(ptep_get(pte)))
			return;
	}

	pte_free_kernel(&init_mm, pte_start);
	pmd_clear(pmd);
}

static void kasan_free_pmd(pmd_t *pmd_start, pud_t *pud)
{
	pmd_t *pmd;
	int i;

	if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_PUD_MODIFIED)
		return;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd = pmd_start + i;
		if (!pmd_none(*pmd))
			return;
	}

	pmd_free(&init_mm, pmd_start);
	pud_clear(pud);
}

static void kasan_free_pud(pud_t *pud_start, p4d_t *p4d)
{
	pud_t *pud;
	int i;

	if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_P4D_MODIFIED)
		return;

	for (i = 0; i < PTRS_PER_PUD; i++) {
		pud = pud_start + i;
		if (!pud_none(*pud))
			return;
	}

	pud_free(&init_mm, pud_start);
	p4d_clear(p4d);
}

static void kasan_free_p4d(p4d_t *p4d_start, pgd_t *pgd)
{
	p4d_t *p4d;
	int i;

	if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_PGD_MODIFIED)
		return;

	for (i = 0; i < PTRS_PER_P4D; i++) {
		p4d = p4d_start + i;
		if (!p4d_none(*p4d))
			return;
	}

	p4d_free(&init_mm, p4d_start);
	pgd_clear(pgd);
}

static void kasan_remove_pte_table(pte_t *pte, unsigned long addr,
				unsigned long end)
{
	unsigned long next;
	pte_t ptent;

	for (; addr < end; addr = next, pte++) {
		next = (addr + PAGE_SIZE) & PAGE_MASK;
		if (next > end)
			next = end;

		ptent = ptep_get(pte);

		if (!pte_present(ptent))
			continue;

		if (WARN_ON(!kasan_early_shadow_page_entry(ptent)))
			continue;
		pte_clear(&init_mm, addr, pte);
	}
}

static void kasan_remove_pmd_table(pmd_t *pmd, unsigned long addr,
				unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next, pmd++) {
		pte_t *pte;

		next = pmd_addr_end(addr, end);

		if (!pmd_present(*pmd))
			continue;

		if (kasan_pte_table(*pmd)) {
			if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_PMD_MODIFIED)
				continue;

			if (IS_ALIGNED(addr, PMD_SIZE) &&
			    IS_ALIGNED(next, PMD_SIZE)) {
				pmd_clear(pmd);
				continue;
			}
		}
		pte = pte_offset_kernel(pmd, addr);
		kasan_remove_pte_table(pte, addr, next);
		kasan_free_pte(pte_offset_kernel(pmd, 0), pmd);
	}
}

static void kasan_remove_pud_table(pud_t *pud, unsigned long addr,
				unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next, pud++) {
		pmd_t *pmd, *pmd_base;

		next = pud_addr_end(addr, end);

		if (!pud_present(*pud))
			continue;

		if (kasan_pmd_table(*pud)) {
			if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_PUD_MODIFIED)
				continue;

			if (IS_ALIGNED(addr, PUD_SIZE) &&
			    IS_ALIGNED(next, PUD_SIZE)) {
				pud_clear(pud);
				continue;
			}
		}
		pmd = pmd_offset(pud, addr);
		pmd_base = pmd_offset(pud, 0);
		kasan_remove_pmd_table(pmd, addr, next);
		kasan_free_pmd(pmd_base, pud);
	}
}

static void kasan_remove_p4d_table(p4d_t *p4d, unsigned long addr,
				unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next, p4d++) {
		pud_t *pud;

		next = p4d_addr_end(addr, end);

		if (!p4d_present(*p4d))
			continue;

		if (kasan_pud_table(*p4d)) {
			if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_P4D_MODIFIED)
				continue;

			if (IS_ALIGNED(addr, P4D_SIZE) &&
			    IS_ALIGNED(next, P4D_SIZE)) {
				p4d_clear(p4d);
				continue;
			}
		}
		pud = pud_offset(p4d, addr);
		kasan_remove_pud_table(pud, addr, next);
		kasan_free_pud(pud_offset(p4d, 0), p4d);
	}
}

void kasan_remove_zero_shadow(void *start, unsigned long size)
{
	unsigned long addr, end, next;
	pgd_t *pgd;

	addr = (unsigned long)kasan_mem_to_shadow(start);
	end = addr + (size >> KASAN_SHADOW_SCALE_SHIFT);

	if (WARN_ON((unsigned long)start % KASAN_MEMORY_PER_SHADOW_PAGE) ||
	    WARN_ON(size % KASAN_MEMORY_PER_SHADOW_PAGE))
		return;

	for (; addr < end; addr = next) {
		p4d_t *p4d;

		next = pgd_addr_end(addr, end);

		pgd = pgd_offset_k(addr);
		if (!pgd_present(*pgd))
			continue;

		if (kasan_p4d_table(*pgd)) {
			if (ARCH_PAGE_TABLE_SYNC_MASK & PGTBL_PGD_MODIFIED)
				continue;

			if (IS_ALIGNED(addr, PGDIR_SIZE) &&
			    IS_ALIGNED(next, PGDIR_SIZE)) {
				pgd_clear(pgd);
				continue;
			}
		}

		p4d = p4d_offset(pgd, addr);
		kasan_remove_p4d_table(p4d, addr, next);
		kasan_free_p4d(p4d_offset(pgd, 0), pgd);
	}
}

int kasan_add_zero_shadow(void *start, unsigned long size)
{
	int ret;
	void *shadow_start, *shadow_end;

	shadow_start = kasan_mem_to_shadow(start);
	shadow_end = shadow_start + (size >> KASAN_SHADOW_SCALE_SHIFT);

	if (WARN_ON((unsigned long)start % KASAN_MEMORY_PER_SHADOW_PAGE) ||
	    WARN_ON(size % KASAN_MEMORY_PER_SHADOW_PAGE))
		return -EINVAL;

	ret = kasan_populate_early_shadow(shadow_start, shadow_end);
	if (ret)
		kasan_remove_zero_shadow(start, size);
	return ret;
}
