// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kpkeys.h>
#include <linux/mm.h>
#include <linux/pagewalk.h>
#include <linux/pgtable.h>
#include <linux/set_memory.h>

#include <kunit/visibility.h>

__ro_after_init DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);
EXPORT_SYMBOL_IF_KUNIT(kpkeys_hardened_pgtables_key);

static int set_pkey_pgtable(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_direct_map_pkey(addr, nr_pages, KPKEYS_PKEY_PGTABLES);

	WARN_ON(ret);
	return ret;
}

static int set_pkey_default(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_direct_map_pkey(addr, nr_pages, 0);

	WARN_ON(ret);
	return ret;
}

struct page *kpkeys_pgtable_alloc(gfp_t gfp, unsigned int order)
{
	struct page *page;
	int ret;

	page = alloc_pages_noprof(gfp, order);
	if (!page)
		return page;

	ret = set_pkey_pgtable(page, 1 << order);
	if (ret) {
		__free_pages(page, order);
		return NULL;
	}

	return page;
}

void kpkeys_pgtable_free(struct page *page, unsigned int order)
{
	set_pkey_default(page, 1 << order);
	__free_pages(page, order);
}

static void __init protect_pgtable_page(void *page)
{
	int ret;

	ret = set_direct_map_pkey((unsigned long)page, 1, KPKEYS_PKEY_PGTABLES);
	WARN_ON_ONCE(ret);
}

static int __init protect_pte_table(pmd_t *pmd, unsigned long addr,
				    unsigned long next, struct mm_walk *walk)
{
	pmd_t entry = pmdp_get(pmd);

	if (!pmd_present(entry) || pmd_leaf(entry))
		return 0;

	protect_pgtable_page(pte_offset_kernel(pmd, 0));

	return 0;
}

static int __init protect_pmd_table(pud_t *pud, unsigned long addr,
				    unsigned long next, struct mm_walk *walk)
{
	pud_t entry;

	if (mm_pmd_folded(walk->mm))
		return 0;

	entry = pudp_get(pud);
	if (!pud_present(entry) || pud_leaf(entry))
		return 0;

	protect_pgtable_page(pmd_offset(pud, 0));

	return 0;
}

static int __init protect_pud_table(p4d_t *p4d, unsigned long addr,
				    unsigned long next, struct mm_walk *walk)
{
	p4d_t entry;

	if (mm_pud_folded(walk->mm))
		return 0;

	entry = p4dp_get(p4d);
	if (!p4d_present(entry) || p4d_leaf(entry))
		return 0;

	protect_pgtable_page(pud_offset(p4d, 0));

	return 0;
}

static int __init protect_p4d_table(pgd_t *pgd, unsigned long addr,
				    unsigned long next, struct mm_walk *walk)
{
	pgd_t entry;

	if (mm_p4d_folded(walk->mm))
		return 0;

	entry = pgdp_get(pgd);
	if (!pgd_present(entry) || pgd_leaf(entry))
		return 0;

	protect_pgtable_page(p4d_offset(pgd, 0));

	return 0;
}

static const struct mm_walk_ops protect_pgtable_ops __initconst = {
	.pmd_entry = protect_pte_table,
	.pud_entry = protect_pmd_table,
	.p4d_entry = protect_pud_table,
	.pgd_entry = protect_p4d_table,
};

/*
 * Protect all installed kernel page tables by walking them and setting the
 * direct mapping of all backing pages to pkey KPKEYS_PKEY_PGTABLES. We assume
 * all non-root levels (PTE to P4D) are page-sized (asserts below).
 *
 * The root PGD page is left untouched; it is normally accessed via the kimg
 * mapping only. On arm64 it cannot be written directly via any mapping.
 *
 * This approach is unsafe if set_direct_map_pkey() may split blocks in the
 * direct map; we rely on the direct map being fully PTE-mapped.
 */
static void __init protect_early_kernel_pgtables(void)
{
	int ret;

	mmap_write_lock(&init_mm);
	/*
	 * [PAGE_OFFSET, ULONG_MAX) covers all early kernel mappings on arm64
	 * and x86 (at least).
	 */
	ret = walk_kernel_page_table_range(PAGE_OFFSET, ULONG_MAX,
					   &protect_pgtable_ops, NULL, NULL);
	mmap_write_unlock(&init_mm);
	WARN_ON_ONCE(ret);
}

static_assert(MAX_PTRS_PER_PTE * sizeof(pte_t) == PAGE_SIZE);

#ifndef __PAGETABLE_PMD_FOLDED
static_assert(MAX_PTRS_PER_PMD * sizeof(pmd_t) == PAGE_SIZE);
#endif

#ifndef __PAGETABLE_PUD_FOLDED
static_assert(MAX_PTRS_PER_PUD * sizeof(pud_t) == PAGE_SIZE);
#endif

#ifndef __PAGETABLE_P4D_FOLDED
static_assert(MAX_PTRS_PER_P4D * sizeof(p4d_t) == PAGE_SIZE);
#endif

void __init kpkeys_hardened_pgtables_init(void)
{
	if (!kpkeys_supported())
		return;

	static_branch_enable(&kpkeys_hardened_pgtables_key);
	protect_early_kernel_pgtables();
}
