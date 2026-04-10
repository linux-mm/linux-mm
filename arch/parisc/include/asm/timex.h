/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASMPARISC_TIMEX_H
#define _ASMPARISC_TIMEX_H

#include <asm/special_insns.h>

static inline cycles_t get_cycles(void)
{
	return mfctl(16);
}

#endif
