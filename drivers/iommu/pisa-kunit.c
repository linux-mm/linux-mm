// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * KUnit tests for the opaque struct pisa_meta table API.
 */
#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/pisa.h>
#include <linux/mm.h>

#include "pisa-priv.h"

static void test_meta_init_and_roundtrip(struct kunit *test)
{
	unsigned long pfn, base_pfn;
	struct pisa_meta *m_pfn;
	struct page *page;

	KUNIT_ASSERT_EQ(test, pisa_meta_init(), 0);
	/* Second call must be idempotent. */
	KUNIT_ASSERT_EQ(test, pisa_meta_init(), 0);

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);

	pfn = page_to_pfn(page);
	base_pfn = ALIGN_DOWN(pfn, 1UL << PISA_ORDER);
	__free_page(page);

	KUNIT_ASSERT_EQ(test, pisa_meta_ensure_pfn(pfn, true), 0);
	m_pfn = pisa_meta_of_pfn(pfn);
	KUNIT_EXPECT_EQ(test, pisa_meta_to_pfn(m_pfn), base_pfn);
}

static void test_meta_invalid_phys(struct kunit *test)
{
	struct pisa_meta *meta;

	KUNIT_ASSERT_EQ(test, pisa_meta_init(), 0);

	meta = pisa_meta_of_pfn(PHYS_PFN(PHYS_ADDR_MAX));
	KUNIT_ASSERT_NOT_NULL(test, meta);
	KUNIT_EXPECT_EQ(test, pisa_meta_to_pfn(meta), ULONG_MAX);
	KUNIT_EXPECT_FALSE(test, is_pisa_pfn(ULONG_MAX >> PAGE_SHIFT));
	KUNIT_EXPECT_NE(test, pisa_meta_ensure_pfn(ULONG_MAX >> PAGE_SHIFT, false), 0);
}

static void pisa_meta_set_active(struct pisa_meta *meta, bool active)
{
	WRITE_ONCE(meta->flags, active ? PISA_ACTIVE : 0);
}

static void test_meta_active_toggle(struct kunit *test)
{
	unsigned long pfn, base_pfn;
	struct pisa_meta *meta;
	struct page *page;

	KUNIT_ASSERT_EQ(test, pisa_meta_init(), 0);

	page = alloc_pages(GFP_KERNEL, PISA_ORDER);
	KUNIT_ASSERT_NOT_NULL(test, page);

	pfn = page_to_pfn(page);
	base_pfn = ALIGN_DOWN(pfn, 1UL << PISA_ORDER);
	/* The buddy allocator returns naturally aligned blocks. */
	KUNIT_EXPECT_EQ(test, pfn, base_pfn);
	if (pisa_meta_ensure_pfn(pfn, true)) {
		__free_pages(page, PISA_ORDER);
		KUNIT_FAIL(test, "pisa_meta_ensure_pfn(%lu) failed", pfn);
		return;
	}
	KUNIT_EXPECT_EQ(test, pisa_meta_ensure_pfn(pfn, false), 0);
	meta = pisa_meta_of_pfn(pfn);

	KUNIT_EXPECT_FALSE(test, is_pisa_pfn(pfn));

	pisa_meta_set_active(meta, true);
	KUNIT_EXPECT_TRUE(test, is_pisa_pfn(base_pfn));
	KUNIT_EXPECT_TRUE(test, is_pisa_pfn(pfn));
	KUNIT_EXPECT_TRUE(test, is_pisa_pfn(base_pfn + (1UL << PISA_ORDER) - 1));

	pisa_meta_set_active(meta, false);
	KUNIT_EXPECT_FALSE(test, is_pisa_pfn(pfn));

	__free_pages(page, PISA_ORDER);
}

static struct kunit_case pisa_meta_test_cases[] = {
	KUNIT_CASE(test_meta_init_and_roundtrip),
	KUNIT_CASE(test_meta_invalid_phys),
	KUNIT_CASE(test_meta_active_toggle),
	{}
};

static struct kunit_suite pisa_meta_test_suite = {
	.name = "pisa_meta",
	.test_cases = pisa_meta_test_cases,
};

kunit_test_suite(pisa_meta_test_suite);
MODULE_DESCRIPTION("KUnit tests for struct pisa_meta table");
MODULE_LICENSE("Dual BSD/GPL");
