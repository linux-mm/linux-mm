/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_RANDOM_H
#define _ASM_RISCV_RANDOM_H

#include <asm/timex.h>

#ifdef CONFIG_RISCV_M_MODE
/*
 * Much like MIPS, we may not have a viable counter to use at an early point
 * in the boot process. Unfortunately we don't have a fallback, so instead
 * invoke the fallback function.
 */
static inline unsigned long random_get_entropy(void)
{
	if (unlikely(clint_time_val == NULL))
		return random_get_entropy_fallback();
	return get_cycles();
}
#else  /* !CONFIG_RISCV_M_MODE */
static inline unsigned long random_get_entropy(void)
{
	return get_cycles();
}
#endif /* CONFIG_RISCV_M_MODE */
#endif /* _ASM_RISCV_RANDOM_H */
