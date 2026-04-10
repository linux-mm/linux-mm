/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASMALPHA_RANDOM_H
#define _ASMALPHA_RANDOM_H

/* Use the cycle counter for entropy. */
static inline unsigned long random_get_entropy(void)
{
	unsigned long ret;

	__asm__ __volatile__ ("rpcc %0" : "=r"(ret));
	return ret;
}

#endif
