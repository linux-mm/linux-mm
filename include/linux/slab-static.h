/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SLAB_STATIC_H
#define _LINUX_SLAB_STATIC_H

#include <linux/init.h>
#include <generated/kmem_cache_size.h>

/* same size and alignment as struct kmem_cache: */
struct kmem_cache_opaque {
	unsigned char opaque[KMEM_CACHE_SIZE];
} __aligned(KMEM_CACHE_ALIGN);

#endif
