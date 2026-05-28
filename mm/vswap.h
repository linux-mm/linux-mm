/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual swap space
 *
 * Copyright (C) 2026 Nhat Pham
 */
#ifndef _MM_VSWAP_H
#define _MM_VSWAP_H

#include <linux/swap.h>

#ifdef CONFIG_VSWAP

extern struct swap_info_struct *vswap_si;

static inline bool swap_is_vswap(struct swap_info_struct *si)
{
	return si->flags & SWP_VSWAP;
}

#else

static inline bool swap_is_vswap(struct swap_info_struct *si)
{
	return false;
}

#endif /* CONFIG_VSWAP */
#endif /* _MM_VSWAP_H */
