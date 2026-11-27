/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PTI_H
#define _ASM_X86_PTI_H

#ifdef CONFIG_TRACK_CR3
#define KERNEL_CR3_IN_USER 0
#define KERNEL_CR3_IN_KERNEL 1
#endif

#ifndef __ASSEMBLER__

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
extern void pti_init(void);
extern void pti_check_boottime_disable(void);
extern void pti_finalize(void);

#ifdef CONFIG_TRACK_CR3
DECLARE_PER_CPU_PAGE_ALIGNED(atomic_t, kernel_cr3_loaded);
#endif

#else
static inline void pti_check_boottime_disable(void) { }
#endif

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_PTI_H */
