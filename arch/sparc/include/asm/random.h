/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASMsparc_RANDOM_H
#define _ASMsparc_RANDOM_H

#if defined(__sparc__) && defined(__arch64__)

#include <asm/timer.h>

static inline unsigned long random_get_entropy(void)
{
	return tick_ops->get_tick();
}

#endif
#endif
