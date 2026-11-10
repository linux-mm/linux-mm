/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *	Skb ref helpers.
 *
 */

#ifndef _LINUX_SKBUFF_REF_H
#define _LINUX_SKBUFF_REF_H

#include <linux/skbuff.h>

bool napi_pp_get_page(netmem_ref netmem);

/**
 * skb_netmem_ref - acquire a fragment reference on a netmem
 * @netmem: netmem to reference
 * @recycle: whether page_pool recycling is enabled (e.g. skb->pp_recycle)
 *
 * Acquires pp_ref_count via napi_pp_get_page() if @recycle is true and
 * @netmem belongs to a page_pool; otherwise acquires a non-pp backing
 * reference via netmem_inc_ref(). Counterpart to skb_netmem_unref().
 */
static __always_inline void skb_netmem_ref(netmem_ref netmem, bool recycle)
{
#ifdef CONFIG_PAGE_POOL
	if (recycle && napi_pp_get_page(netmem))
		return;
#endif
	netmem_inc_ref(netmem);
}

/**
 * skb_frag_ref - acquire a reference on a paged fragment of an skb
 * @skb: the buffer
 * @f: the fragment offset
 *
 * Acquires a reference on the @f'th fragment of @skb via skb_netmem_ref()
 * (pp_ref_count if skb->pp_recycle and page_pool-backed, else non-pp
 * backing refcount). Counterpart to skb_frag_unref().
 */
static __always_inline void skb_frag_ref(struct sk_buff *skb, int f)
{
	skb_netmem_ref(skb_frag_netmem(&skb_shinfo(skb)->frags[f]),
		       skb->pp_recycle);
}

/**
 * skb_frag_try_ref - try to acquire a reference on a paged fragment of an skb
 * @skb: the buffer
 * @f: the fragment offset
 *
 * Like skb_frag_ref(), but acquires non-page_pool page references using
 * try_get_page(), refusing additional references to a page whose refcount is
 * at its limit.
 *
 * Return: True if a reference was acquired, otherwise false.
 */
static __always_inline __must_check bool skb_frag_try_ref(struct sk_buff *skb,
							  int f)
{
	netmem_ref netmem = skb_frag_netmem(&skb_shinfo(skb)->frags[f]);

#ifdef CONFIG_PAGE_POOL
	if (skb->pp_recycle && napi_pp_get_page(netmem))
		return true;
#endif
	if (netmem_is_net_iov(netmem)) {
		net_iov_inc_ref(netmem_to_net_iov(netmem));
		return true;
	}
	return try_get_page(netmem_to_page(netmem));
}

bool napi_pp_put_page(netmem_ref netmem);

/**
 * skb_netmem_unref - release a fragment reference on a netmem
 * @netmem: netmem to unreference
 * @recycle: whether page_pool recycling is enabled (e.g. skb->pp_recycle)
 *
 * Drops pp_ref_count via napi_pp_put_page() if @recycle is true and @netmem
 * belongs to a page_pool; otherwise drops a non-pp backing reference via
 * netmem_dec_ref(). Counterpart to skb_netmem_ref().
 */
static __always_inline void skb_netmem_unref(netmem_ref netmem, bool recycle)
{
#ifdef CONFIG_PAGE_POOL
	if (recycle && napi_pp_put_page(netmem))
		return;
#endif
	netmem_dec_ref(netmem);
}

/**
 * skb_frag_unref - release a reference on a paged fragment of an skb
 * @skb: the buffer
 * @f: the fragment offset
 *
 * Drops a reference on the @f'th fragment of @skb via skb_netmem_unref()
 * (pp_ref_count if skb->pp_recycle and page_pool-backed, else non-pp
 * backing refcount), skipping managed zerocopy fragments. Counterpart to
 * skb_frag_ref().
 */
static __always_inline void skb_frag_unref(struct sk_buff *skb, int f)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	if (!skb_zcopy_managed(skb))
		skb_netmem_unref(skb_frag_netmem(&shinfo->frags[f]),
				 skb->pp_recycle);
}

/**
 * skb_frags_try_ref - try to acquire references on all paged fragments
 * @skb: the buffer
 *
 * Try to acquire a reference on each paged fragment of @skb using
 * skb_frag_try_ref(). On failure, acquired references are dropped.
 *
 * Must not be used on skbs with managed frag references.
 *
 * Return: True if all references were acquired, otherwise false.
 */
static inline __must_check bool skb_frags_try_ref(struct sk_buff *skb)
{
	int i;

	DEBUG_NET_WARN_ON_ONCE(skb_zcopy_managed(skb));
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		if (unlikely(!skb_frag_try_ref(skb, i))) {
			while (i--)
				skb_frag_unref(skb, i);
			return false;
		}
	}
	return true;
}

#endif	/* _LINUX_SKBUFF_REF_H */
