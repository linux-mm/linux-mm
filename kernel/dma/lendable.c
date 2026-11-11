// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lendable DMA Contiguous Memory Pool
 *
 * Prevents CPU speculative reads to DMA memory (e.g. when lent to a secure
 * world) by unmapping it from the kernel direct map. This works if the
 * architecture allows modifying the direct map globally (can_set_direct_map())
 * or if the memory belongs to a reserved-memory CMA pool compatible with
 * "lendable-dma-pool".
 *
 * Copyright (C) 2026 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#define pr_fmt(fmt) "dma-lendable: " fmt

#include <linux/atomic.h>
#include <linux/bsearch.h>
#include <linux/cma.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/of_reserved_mem.h>
#include <linux/set_memory.h>
#include <linux/sort.h>
#include <linux/suspend.h>

#include "contiguous.h"

#define DMA_LENDABLE_DISABLED	-1

static struct cma *lendable_areas[MAX_CMA_AREAS] __ro_after_init;
static unsigned int nr_lendable_areas __ro_after_init;
static atomic_t dma_lendable_active;

static int __lendable_cmp(const void *a, const void *b)
{
	const struct cma *cma_a = *(struct cma * const *)a;
	const struct cma *cma_b = *(struct cma * const *)b;

	return cmp_int(cma_get_base(cma_a), cma_get_base(cma_b));
}

static bool cma_is_lendable(struct cma *cma)
{
	return cma && bsearch(&cma, lendable_areas, nr_lendable_areas,
			      sizeof(lendable_areas[0]), __lendable_cmp);
}

static bool cma_has_range(struct cma *cma, struct page *page, unsigned int nr_pages)
{
	phys_addr_t start = page_to_phys(page);
	phys_addr_t end = start + ((phys_addr_t)nr_pages << PAGE_SHIFT);
	phys_addr_t base = cma_get_base(cma);

	return start >= base && end <= base + cma_get_size(cma);
}

/**
 * dma_zap_direct_map() - Remove a DMA page range from the kernel direct map
 * @dev:	Device that allocated the pages (or NULL for default CMA pool)
 * @page:	First page of the memory region
 * @size:	Page-aligned size of the memory region in bytes
 *
 * Cleans and invalidates caches and removes the memory range from the kernel
 * direct map to prevent speculative CPU accesses. Must be paired with
 * dma_restore_direct_map().
 *
 * Context: Any context if allocated from a lendable pool; process context
 *          (may sleep) otherwise.
 * Return:
 * * 0		- Success.
 * * -EINVAL	- @size is 0 or not page-aligned.
 * * -ENODEV	- Direct map cannot be modified and @dev has no lendable pool.
 * * -EFAULT	- @dev has a lendable pool, but [@page, @page + @size) is
 *		  outside that pool.
 * * -EBUSY	- Direct map zapping is temporarily disabled (hibernation).
 */
int dma_zap_direct_map(struct device *dev, struct page *page, size_t size)
{
	unsigned int nr_pages = size >> PAGE_SHIFT;
	bool from_lendable, has_lendable;
	struct cma *cma;
	int ret;

	if (!size || !PAGE_ALIGNED(size))
		return -EINVAL;

	cma = dev_get_cma_area(dev);
	has_lendable = cma_is_lendable(cma);
	from_lendable = has_lendable && cma_has_range(cma, page, nr_pages);

	if (!from_lendable && !can_set_direct_map())
		return has_lendable ? -EFAULT : -ENODEV;

	/* Provides full ordering */
	if (!atomic_add_unless(&dma_lendable_active, 1, DMA_LENDABLE_DISABLED))
		return -EBUSY;

	ret = arch_dma_zap_direct_map(page, nr_pages, from_lendable);
	if (ret)
		atomic_dec(&dma_lendable_active);

	return ret;
}
EXPORT_SYMBOL_GPL(dma_zap_direct_map);

/**
 * dma_restore_direct_map() - Restore a zapped DMA page range in the direct map
 * @dev:	Device that allocated the pages (or NULL for default CMA pool)
 * @page:	First page of the memory region
 * @size:	Page-aligned size of the memory region in bytes
 *
 * Restores the default kernel direct mapping and invalidates caches for a range
 * previously zapped with dma_zap_direct_map().
 *
 * Context: Any context if allocated from a lendable pool; process context
 *          (may sleep) otherwise.
 */
void dma_restore_direct_map(struct device *dev, struct page *page, size_t size)
{
	unsigned int nr_pages = size >> PAGE_SHIFT;
	bool from_lendable;
	struct cma *cma;

	if (WARN_ON_ONCE(!size || !PAGE_ALIGNED(size)))
		return;

	cma = dev_get_cma_area(dev);
	from_lendable = cma_is_lendable(cma) && cma_has_range(cma, page, nr_pages);

	if (WARN_ON_ONCE(!from_lendable && !can_set_direct_map()))
		return;

	if (WARN_ON_ONCE(arch_dma_restore_direct_map(page, nr_pages, from_lendable)))
		return;

	WARN_ON_ONCE(atomic_dec_return_release(&dma_lendable_active) < 0);
}
EXPORT_SYMBOL_GPL(dma_restore_direct_map);

static int dma_lendable_pm_notify(struct notifier_block *nb,
				  unsigned long mode, void *data)
{
	/* Prevent hibernation while unmapped pages may be inaccessible */
	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		if (atomic_cmpxchg_acquire(&dma_lendable_active, 0, DMA_LENDABLE_DISABLED))
			return notifier_from_errno(-EBUSY);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		atomic_cmpxchg(&dma_lendable_active, DMA_LENDABLE_DISABLED, 0);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block dma_lendable_pm_nb = {
	.notifier_call	= dma_lendable_pm_notify,
};

static int __init dma_lendable_init(void)
{
	unsigned int i, nr = 0;

	for (i = 0; i < nr_lendable_areas; i++) {
		struct cma *cma = lendable_areas[i];

		if (!arch_dma_is_lendable(phys_to_page(cma_get_base(cma)),
					  cma_get_size(cma) >> PAGE_SHIFT)) {
			pr_warn("Ignoring %s: not lendable\n", cma_get_name(cma));
			continue;
		}

		lendable_areas[nr++] = cma;
	}
	nr_lendable_areas = nr;

	sort(lendable_areas, nr_lendable_areas, sizeof(lendable_areas[0]), __lendable_cmp, NULL);

	register_pm_notifier(&dma_lendable_pm_nb);

	return 0;
}
core_initcall(dma_lendable_init);

#ifdef CONFIG_OF_RESERVED_MEM
static int __init rmem_lendable_validate(unsigned long node,
					 phys_addr_t *align)
{
	return rmem_cma_ops.node_validate(node, align);
}

static int __init rmem_lendable_fixup(unsigned long node, phys_addr_t base,
				      phys_addr_t size)
{
	int ret = rmem_cma_ops.node_fixup(node, base, size);

	if (ret)
		return ret;

	return arch_dma_mark_lendable(base, size);
}

static int __init rmem_lendable_setup(unsigned long node,
				      struct reserved_mem *rmem)
{
	int ret = rmem_cma_ops.node_init(node, rmem);

	if (ret)
		return ret;

	lendable_areas[nr_lendable_areas++] = rmem->priv;

	return 0;
}

static int rmem_lendable_device_init(struct reserved_mem *rmem,
				     struct device *dev)
{
	return rmem_cma_ops.device_init(rmem, dev);
}

static void rmem_lendable_device_release(struct reserved_mem *rmem,
					 struct device *dev)
{
	rmem_cma_ops.device_release(rmem, dev);
}

static const struct reserved_mem_ops rmem_lendable_ops = {
	.node_validate	= rmem_lendable_validate,
	.node_fixup	= rmem_lendable_fixup,
	.node_init	= rmem_lendable_setup,
	.device_init	= rmem_lendable_device_init,
	.device_release	= rmem_lendable_device_release,
};

RESERVEDMEM_OF_DECLARE(cma_lendable, "lendable-dma-pool", &rmem_lendable_ops);
#endif /* CONFIG_OF_RESERVED_MEM */
