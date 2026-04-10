/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASMm68K_RANDOM_H
#define _ASMm68K_RANDOM_H

extern unsigned long (*mach_random_get_entropy)(void);

static inline unsigned long random_get_entropy(void)
{
	if (mach_random_get_entropy)
		return mach_random_get_entropy();
	return random_get_entropy_fallback();
}

#endif
