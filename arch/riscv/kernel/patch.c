// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 SiFive
 */

#ifdef CONFIG_RISCV_ESP32P4_CLIC
#include <linux/align.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/panic.h>
#include <linux/sizes.h>
#endif
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/stop_machine.h>
#include <asm/kprobes.h>
#include <asm/cacheflush.h>
#include <asm/fixmap.h>
#include <asm/ftrace.h>
#include <asm/text-patching.h>
#include <asm/sections.h>

struct patch_insn {
	void *addr;
	u32 *insns;
	size_t len;
	atomic_t cpu_count;
};

int riscv_patch_in_stop_machine = false;

#ifdef CONFIG_MMU

static inline bool is_kernel_exittext(uintptr_t addr)
{
	return system_state < SYSTEM_RUNNING &&
		addr >= (uintptr_t)__exittext_begin &&
		addr < (uintptr_t)__exittext_end;
}

/*
 * The fix_to_virt(, idx) needs a const value (not a dynamic variable of
 * reg-a0) or BUILD_BUG_ON failed with "idx >= __end_of_fixed_addresses".
 * So use '__always_inline' and 'const unsigned int fixmap' here.
 */
static __always_inline void *patch_map(void *addr, const unsigned int fixmap)
{
	uintptr_t uintaddr = (uintptr_t) addr;
	phys_addr_t phys;

	if (core_kernel_text(uintaddr) || is_kernel_exittext(uintaddr)) {
		if (!IS_ENABLED(CONFIG_STRICT_KERNEL_RWX))
			return addr;
		phys = __pa_symbol(addr);
	} else if (IS_ENABLED(CONFIG_EXECMEM)) {
		struct page *page = vmalloc_to_page(addr);

		BUG_ON(!page);
		phys = page_to_phys(page) + offset_in_page(addr);
	} else {
		return addr;
	}

	return (void *)set_fixmap_offset(fixmap, phys);
}

static void patch_unmap(int fixmap)
{
	clear_fixmap(fixmap);
}
NOKPROBE_SYMBOL(patch_unmap);

static int __patch_insn_set(void *addr, u8 c, size_t len)
{
	bool across_pages = (offset_in_page(addr) + len) > PAGE_SIZE;
	void *waddr = addr;

	/*
	 * Only two pages can be mapped at a time for writing.
	 */
	if (len + offset_in_page(addr) > 2 * PAGE_SIZE)
		return -EINVAL;
	/*
	 * Before reaching here, it was expected to lock the text_mutex
	 * already, so we don't need to give another lock here and could
	 * ensure that it was safe between each cores.
	 */
	lockdep_assert_held(&text_mutex);

	preempt_disable();

	if (across_pages)
		patch_map(addr + PAGE_SIZE, FIX_TEXT_POKE1);

	waddr = patch_map(addr, FIX_TEXT_POKE0);

	memset(waddr, c, len);

	/*
	 * We could have just patched a function that is about to be
	 * called so make sure we don't execute partially patched
	 * instructions by flushing the icache as soon as possible.
	 */
	local_flush_icache_range((unsigned long)waddr,
				 (unsigned long)waddr + len);

	patch_unmap(FIX_TEXT_POKE0);

	if (across_pages)
		patch_unmap(FIX_TEXT_POKE1);

	preempt_enable();

	return 0;
}
NOKPROBE_SYMBOL(__patch_insn_set);

static int __patch_insn_write(void *addr, const void *insn, size_t len)
{
	bool across_pages = (offset_in_page(addr) + len) > PAGE_SIZE;
	void *waddr = addr;
	int ret;

	/*
	 * Only two pages can be mapped at a time for writing.
	 */
	if (len + offset_in_page(addr) > 2 * PAGE_SIZE)
		return -EINVAL;

	/*
	 * Before reaching here, it was expected to lock the text_mutex
	 * already, so we don't need to give another lock here and could
	 * ensure that it was safe between each cores.
	 *
	 * We're currently using stop_machine() for ftrace & kprobes, and while
	 * that ensures text_mutex is held before installing the mappings it
	 * does not ensure text_mutex is held by the calling thread.  That's
	 * safe but triggers a lockdep failure, so just elide it for that
	 * specific case.
	 */
	if (!riscv_patch_in_stop_machine)
		lockdep_assert_held(&text_mutex);

	preempt_disable();

	if (across_pages)
		patch_map(addr + PAGE_SIZE, FIX_TEXT_POKE1);

	waddr = patch_map(addr, FIX_TEXT_POKE0);

	ret = copy_to_kernel_nofault(waddr, insn, len);

	/*
	 * We could have just patched a function that is about to be
	 * called so make sure we don't execute partially patched
	 * instructions by flushing the icache as soon as possible.
	 */
	local_flush_icache_range((unsigned long)waddr,
				 (unsigned long)waddr + len);

	patch_unmap(FIX_TEXT_POKE0);

	if (across_pages)
		patch_unmap(FIX_TEXT_POKE1);

	preempt_enable();

	return ret;
}
NOKPROBE_SYMBOL(__patch_insn_write);
#else
static int __patch_insn_set(void *addr, u8 c, size_t len)
{
	memset(addr, c, len);

	return 0;
}
NOKPROBE_SYMBOL(__patch_insn_set);

#ifdef CONFIG_RISCV_ESP32P4_CLIC
#define ESP32P4_CACHE_BASE		0x3ff10000
#define ESP32P4_CACHE_SYNC_CTRL		0x98
#define ESP32P4_CACHE_SYNC_MAP		0x9c
#define ESP32P4_CACHE_SYNC_ADDR		0xa0
#define ESP32P4_CACHE_SYNC_SIZE		0xa4

#define ESP32P4_CACHE_INVALIDATE	BIT(0)
#define ESP32P4_CACHE_WRITEBACK		BIT(2)
#define ESP32P4_CACHE_SYNC_DONE		BIT(4)
#define ESP32P4_CACHE_SYNC_MAX_POLLS	1000000

#define ESP32P4_CACHE_L1_I0		BIT(0)
#define ESP32P4_CACHE_L1_D		BIT(4)
#define ESP32P4_CACHE_L2		BIT(5)

#define ESP32P4_PSRAM_START		0x48000000UL
#define ESP32P4_PSRAM_END		0x4c000000UL
#define ESP32P4_CACHE_MAX_LINE		128

static int esp32p4_cache_sync(u32 map, u32 start, u32 size, u32 op,
			       unsigned int count)
{
	/* The caller holds local IRQs disabled on the single supported hart. */
	static bool sync_failed;
	void __iomem *base = (void __iomem *)ESP32P4_CACHE_BASE;
	unsigned int polls;

	if (sync_failed)
		return -EIO;

	writel(map, base + ESP32P4_CACHE_SYNC_MAP);
	writel(start, base + ESP32P4_CACHE_SYNC_ADDR);
	writel(size, base + ESP32P4_CACHE_SYNC_SIZE);

	while (count--) {
		writel(op, base + ESP32P4_CACHE_SYNC_CTRL);
		/* Order the sync command before reading its completion status. */
		mb();
		for (polls = 0; polls < ESP32P4_CACHE_SYNC_MAX_POLLS; polls++) {
			if (readl(base + ESP32P4_CACHE_SYNC_CTRL) & ESP32P4_CACHE_SYNC_DONE)
				break;
			cpu_relax();
		}
		if (polls == ESP32P4_CACHE_SYNC_MAX_POLLS) {
			sync_failed = true;
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int esp32p4_cache_writeback(u32 start, u32 size)
{
	int ret;

	/* Repeat writeback to avoid losing a sync with conflicting writes. */
	ret = esp32p4_cache_sync(ESP32P4_CACHE_L1_D, start, size, ESP32P4_CACHE_WRITEBACK, 2);
	if (ret)
		return ret;

	return esp32p4_cache_sync(ESP32P4_CACHE_L2, start, size, ESP32P4_CACHE_WRITEBACK, 2);
}

static int esp32p4_sync_text(void *addr, size_t len, bool fatal)
{
	unsigned long start = (unsigned long)addr;
	unsigned long first, end, flags;

	if (start < ESP32P4_PSRAM_START || start >= ESP32P4_PSRAM_END ||
	    !len || len > ESP32P4_PSRAM_END - start)
		return 0;

	first = ALIGN_DOWN(start, ESP32P4_CACHE_MAX_LINE);
	end = ALIGN(start + len, ESP32P4_CACHE_MAX_LINE);

	/* Allow interrupts between chunks if they were enabled on entry. */
	while (first < end) {
		u32 size = min_t(u32, end - first, SZ_32K);
		int ret;

		local_irq_save(flags);
		ret = esp32p4_cache_writeback(first, size);
		if (!ret)
			ret = esp32p4_cache_sync(ESP32P4_CACHE_L1_I0, first, size,
						 ESP32P4_CACHE_INVALIDATE, 1);
		if (ret) {
			if (fatal)
				panic("ESP32-P4 text cache sync failed: %d\n", ret);
			local_irq_restore(flags);
			return ret;
		}

		local_flush_icache_all();
		local_irq_restore(flags);
		first += size;
	}

	return 0;
}

int esp32p4_sync_user_text(unsigned long start, unsigned long end)
{
	int ret;

	if (end > start) {
		ret = esp32p4_sync_text((void *)start, end - start, false);
		if (ret)
			return ret;
	}

	flush_icache_range(start, end);
	return 0;
}
#endif

static int __patch_insn_write(void *addr, const void *insn, size_t len)
{
	int ret = copy_to_kernel_nofault(addr, insn, len);

#ifdef CONFIG_RISCV_ESP32P4_CLIC
	if (!ret)
		esp32p4_sync_text(addr, len, true);
#endif
	return ret;
}
NOKPROBE_SYMBOL(__patch_insn_write);
#endif /* CONFIG_MMU */

static int patch_insn_set(void *addr, u8 c, size_t len)
{
	size_t size;
	int ret;

	/*
	 * __patch_insn_set() can only work on 2 pages at a time so call it in a
	 * loop with len <= 2 * PAGE_SIZE.
	 */
	while (len) {
		size = min(len, PAGE_SIZE * 2 - offset_in_page(addr));
		ret = __patch_insn_set(addr, c, size);
		if (ret)
			return ret;

		addr += size;
		len -= size;
	}

	return 0;
}
NOKPROBE_SYMBOL(patch_insn_set);

int patch_text_set_nosync(void *addr, u8 c, size_t len)
{
	int ret;

	ret = patch_insn_set(addr, c, len);
	if (!ret)
		flush_icache_range((uintptr_t)addr, (uintptr_t)addr + len);

	return ret;
}
NOKPROBE_SYMBOL(patch_text_set_nosync);

int patch_insn_write(void *addr, const void *insn, size_t len)
{
	size_t size;
	int ret;

	/*
	 * Copy the instructions to the destination address, two pages at a time
	 * because __patch_insn_write() can only handle len <= 2 * PAGE_SIZE.
	 */
	while (len) {
		size = min(len, PAGE_SIZE * 2 - offset_in_page(addr));
		ret = __patch_insn_write(addr, insn, size);
		if (ret)
			return ret;

		addr += size;
		insn += size;
		len -= size;
	}

	return 0;
}
NOKPROBE_SYMBOL(patch_insn_write);

int patch_text_nosync(void *addr, const void *insns, size_t len)
{
	int ret;

	ret = patch_insn_write(addr, insns, len);
	if (!ret)
		flush_icache_range((uintptr_t)addr, (uintptr_t)addr + len);

	return ret;
}
NOKPROBE_SYMBOL(patch_text_nosync);

static int patch_text_cb(void *data)
{
	struct patch_insn *patch = data;
	int ret = 0;

	if (atomic_inc_return(&patch->cpu_count) == num_online_cpus()) {
		ret = patch_insn_write(patch->addr, patch->insns, patch->len);
		/*
		 * Make sure the patching store is effective *before* we
		 * increment the counter which releases all waiting CPUs
		 * by using the release variant of atomic increment. The
		 * release pairs with the call to local_flush_icache_all()
		 * on the waiting CPU.
		 */
		atomic_inc_return_release(&patch->cpu_count);
	} else {
		while (atomic_read(&patch->cpu_count) <= num_online_cpus())
			cpu_relax();

		local_flush_icache_all();
	}

	return ret;
}
NOKPROBE_SYMBOL(patch_text_cb);

int patch_text(void *addr, u32 *insns, size_t len)
{
	int ret;
	struct patch_insn patch = {
		.addr = addr,
		.insns = insns,
		.len = len,
		.cpu_count = ATOMIC_INIT(0),
	};

	/*
	 * kprobes takes text_mutex, before calling patch_text(), but as we call
	 * calls stop_machine(), the lockdep assertion in patch_insn_write()
	 * gets confused by the context in which the lock is taken.
	 * Instead, ensure the lock is held before calling stop_machine(), and
	 * set riscv_patch_in_stop_machine to skip the check in
	 * patch_insn_write().
	 */
	lockdep_assert_held(&text_mutex);
	riscv_patch_in_stop_machine = true;
	ret = stop_machine_cpuslocked(patch_text_cb, &patch, cpu_online_mask);
	riscv_patch_in_stop_machine = false;
	return ret;
}
NOKPROBE_SYMBOL(patch_text);
