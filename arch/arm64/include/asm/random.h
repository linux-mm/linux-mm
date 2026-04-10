/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_RANDOM_H
#define __ASM_RANDOM_H

/* Out of line to avoid recursive include hell */
unsigned long random_get_entropy(void);

#endif
