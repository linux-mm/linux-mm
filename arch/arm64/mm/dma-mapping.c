// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/gfp.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <xen/xen.h>

#include <asm/cacheflush.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include <asm/xen/xen-ops.h>

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);

	dcache_clean_poc_nosync(start, start + size);
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);

	if (dir == DMA_TO_DEVICE)
		return;

	dcache_inval_poc_nosync(start, start + size);
}

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	unsigned long start = (unsigned long)page_address(page);

	dcache_clean_poc(start, start + size);
}

#ifdef CONFIG_DMA_LENDABLE
int __init arch_dma_mark_lendable(phys_addr_t base, phys_addr_t size)
{
	if (!PAGE_ALIGNED(base | size))
		return -EINVAL;

	return memblock_mark_ptemap(base, size);
}

bool __init arch_dma_is_lendable(struct page *page, unsigned int nr_pages)
{
	return direct_map_is_ptemap(page, nr_pages);
}

int arch_dma_zap_direct_map(struct page *page, unsigned int nr_pages,
			    bool from_lendable)
{
	unsigned long start = (unsigned long)page_address(page);
	unsigned long end = start + ((unsigned long)nr_pages << PAGE_SHIFT);
	int ret;

	/*
	 * arch_dma_prep_coherent() only cleans as normal DMA buffers retain a
	 * cacheable linear-map alias. Since that alias is being removed here,
	 * also invalidate to make sure they can't be evicted later on while the
	 * memory is secured.
	 */
	dcache_clean_inval_poc(start, end);

	if (from_lendable) {
		ret = __set_direct_map_invalid_noflush(page, nr_pages);
	} else {
		might_sleep();
		ret = set_direct_map_invalid_noflush(page, nr_pages);
	}

	if (ret)
		return ret;

	flush_tlb_kernel_range(start, end);

	return 0;
}

int arch_dma_restore_direct_map(struct page *page, unsigned int nr_pages,
				bool from_lendable)
{
	unsigned long start = (unsigned long)page_address(page);
	unsigned long end = start + ((unsigned long)nr_pages << PAGE_SHIFT);
	int ret;

	if (from_lendable) {
		ret = __set_direct_map_default_noflush(page, nr_pages);
	} else {
		might_sleep();
		ret = set_direct_map_default_noflush(page, nr_pages);
	}

	if (ret)
		return ret;

	dcache_inval_poc(start, end);

	return 0;
}
#endif /* CONFIG_DMA_LENDABLE */

void arch_setup_dma_ops(struct device *dev, bool coherent)
{
	int cls = cache_line_size_of_cpu();

	if (!coherent && !CLIDR_LOC(read_sysreg(clidr_el1))) {
		dev_warn(dev, "CLIDR_EL1.LoC == 0, treating as coherent\n");
		coherent = true;
	}

	WARN_TAINT(!coherent && cls > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than CTR_EL0.CWG (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, cls);

	dev_assign_dma_coherent(dev, coherent);

	xen_setup_dma_ops(dev);
}
