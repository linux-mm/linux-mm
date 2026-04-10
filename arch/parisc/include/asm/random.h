/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASMPARISC_RANDOM_H
#define _ASMPARISC_RANDOM_H

#include <asm/timex.h>

static inline unsigned long random_get_entropy(void)
{
	return get_cycles();
}

#endif
