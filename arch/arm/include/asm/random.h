/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASMARM_RANDOM_H
#define _ASMARM_RANDOM_H

bool delay_read_timer(unsigned long *t);

static inline unsigned long random_get_entropy(void)
{
	unsigned long t;

	return delay_read_timer(&t) ? t : random_get_entropy_fallback();
}

#endif
