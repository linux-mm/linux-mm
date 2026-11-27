/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_DMA_CONTIGUOUS_H
#define _KERNEL_DMA_CONTIGUOUS_H

#ifdef CONFIG_OF_RESERVED_MEM
struct reserved_mem_ops;
extern const struct reserved_mem_ops rmem_cma_ops;
#endif

#endif /* _KERNEL_DMA_CONTIGUOUS_H */
