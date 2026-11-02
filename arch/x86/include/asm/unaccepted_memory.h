#ifndef _ASM_X86_UNACCEPTED_MEMORY_H
#define _ASM_X86_UNACCEPTED_MEMORY_H

#include <linux/efi.h>
#include <asm/e820/api.h>
#include <asm/tdx.h>
#include <asm/sev.h>

static inline void __arch_accept_memory(phys_addr_t start, phys_addr_t end)
{
	/* Platform-specific memory-acceptance call goes here */
	if (cpu_feature_enabled(X86_FEATURE_TDX_GUEST)) {
		if (!tdx_accept_memory(start, end))
			panic("TDX: Failed to accept memory\n");
	} else if (cc_platform_has(CC_ATTR_GUEST_SEV_SNP)) {
		snp_accept_memory(start, end);
	} else {
		panic("Cannot accept memory: unknown platform\n");
	}
}

static inline void arch_accept_memory(phys_addr_t start, phys_addr_t end)
{
	u32 i;

	for (i = 0; i < e820_table->nr_entries; i++) {
		struct e820_entry *entry = &e820_table->entries[i];
		phys_addr_t s, e;

		if (entry->type != E820_TYPE_RAM)
			continue;

		s = max_t(phys_addr_t, start, entry->addr);
		e = min_t(phys_addr_t, end, entry->addr + entry->size);
		if (s >= e)
			continue;

		__arch_accept_memory(s, e);
	}
}

static inline struct efi_unaccepted_memory *efi_get_unaccepted_table(void)
{
	if (efi.unaccepted == EFI_INVALID_TABLE_ADDR)
		return NULL;
	return __va(efi.unaccepted);
}
#endif
