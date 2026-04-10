/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#ifndef _ASM_RANDOM_H
#define _ASM_RANDOM_H

#include <asm/timex.h>

static inline unsigned long random_get_entropy(void)
{
	return get_cycles();
}

#endif /*  _ASM_RANDOM_H */
