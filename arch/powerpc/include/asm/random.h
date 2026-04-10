/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_RANDOM_H
#define _ASM_POWERPC_RANDOM_H

#include <asm/cputable.h>
#include <asm/vdso/timebase.h>

static inline unsigned long random_get_entropy(void)
{
	return mftb();
}

#endif	/* _ASM_POWERPC_RANDOM_H */
