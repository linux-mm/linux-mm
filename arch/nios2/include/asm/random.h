/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_NIOS2_RANDOM_H
#define _ASM_NIOS2_RANDOM_H

#include <asm/timex.h>

static inline unsigned long random_get_entropy(void)
{
	unsigned long c = get_cycles();

	return c ? c : random_get_entropy_fallback();
}

#endif
