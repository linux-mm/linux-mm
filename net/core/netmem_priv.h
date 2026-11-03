/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __NETMEM_PRIV_H
#define __NETMEM_PRIV_H

/* Use READ_ONCE() because every netmem_inc/dec_ref() checks netmem_is_pp(),
 * and pp_magic shares a struct page union word with folio->lru.next which MM
 * code may concurrently modify on page-cache folios in TX SKBs.
 */
static inline unsigned long netmem_get_pp_magic(netmem_ref netmem)
{
	return READ_ONCE(netmem_to_nmdesc(netmem)->pp_magic) &
	       ~PP_DMA_INDEX_MASK;
}

static inline void netmem_or_pp_magic(netmem_ref netmem, unsigned long pp_magic)
{
	struct netmem_desc *nmdesc = netmem_to_nmdesc(netmem);

	WRITE_ONCE(nmdesc->pp_magic, READ_ONCE(nmdesc->pp_magic) | pp_magic);
}

static inline void netmem_clear_pp_magic(netmem_ref netmem)
{
	struct netmem_desc *nmdesc = netmem_to_nmdesc(netmem);

	WARN_ON_ONCE(READ_ONCE(nmdesc->pp_magic) & PP_DMA_INDEX_MASK);

	WRITE_ONCE(nmdesc->pp_magic, 0);
}

static inline bool netmem_is_pp(netmem_ref netmem)
{
	return (netmem_get_pp_magic(netmem) & PP_MAGIC_MASK) == PP_SIGNATURE;
}

static inline void netmem_set_pp(netmem_ref netmem, struct page_pool *pool)
{
	WRITE_ONCE(netmem_to_nmdesc(netmem)->pp, pool);
}

static inline void netmem_set_dma_addr(netmem_ref netmem,
				       unsigned long dma_addr)
{
	netmem_to_nmdesc(netmem)->dma_addr = dma_addr;
}

static inline unsigned long netmem_get_dma_index(netmem_ref netmem)
{
	unsigned long magic;

	if (WARN_ON_ONCE(netmem_is_net_iov(netmem)))
		return 0;

	magic = READ_ONCE(netmem_to_nmdesc(netmem)->pp_magic);

	return (magic & PP_DMA_INDEX_MASK) >> PP_DMA_INDEX_SHIFT;
}

static inline void netmem_set_dma_index(netmem_ref netmem,
					unsigned long id)
{
	unsigned long magic;

	if (WARN_ON_ONCE(netmem_is_net_iov(netmem)))
		return;

	magic = netmem_get_pp_magic(netmem) | (id << PP_DMA_INDEX_SHIFT);
	WRITE_ONCE(netmem_to_nmdesc(netmem)->pp_magic, magic);
}
#endif
