/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *	Skb ref helpers.
 *
 */

#ifndef _LINUX_SKBUFF_REF_H
#define _LINUX_SKBUFF_REF_H

#include <linux/skbuff.h>

/**
 * skb_frag_ref - acquire a reference on a paged fragment of an skb
 * @skb: the buffer
 * @f: the fragment offset
 *
 * Acquires a reference on the @f'th fragment of @skb via netmem_inc_ref()
 * (pp_ref_count if page_pool-backed, else non-pp backing refcount), skipping
 * managed zerocopy fragments. Counterpart to skb_frag_unref().
 */
static __always_inline void skb_frag_ref(struct sk_buff *skb, int f)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	if (!skb_zcopy_managed(skb))
		netmem_inc_ref(skb_frag_netmem(&shinfo->frags[f]));
}

/**
 * skb_frag_unref - release a reference on a paged fragment of an skb
 * @skb: the buffer
 * @f: the fragment offset
 *
 * Drops a reference on the @f'th fragment of @skb via netmem_dec_ref()
 * (pp_ref_count if page_pool-backed, else non-pp backing refcount), skipping
 * managed zerocopy fragments. Counterpart to skb_frag_ref().
 */
static __always_inline void skb_frag_unref(struct sk_buff *skb, int f)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	if (!skb_zcopy_managed(skb))
		netmem_dec_ref(skb_frag_netmem(&shinfo->frags[f]));
}

#endif	/* _LINUX_SKBUFF_REF_H */
