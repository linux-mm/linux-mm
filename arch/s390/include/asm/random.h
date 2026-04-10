/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_RANDOM_H
#define _ASM_S390_RANDOM_H

#include <asm/timex.h>

static inline unsigned long random_get_entropy(void)
{
	return (unsigned long)get_tod_clock_monotonic() >> 2;
}

#endif
