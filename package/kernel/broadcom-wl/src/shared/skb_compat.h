/*
 * Compatibility layer for sk_buff fields removed in Linux 5.x+.
 *
 * The Broadcom DHD driver uses skb->next, skb->prev, and skb->cloned
 * extensively for packet chaining. These fields were removed from
 * upstream struct sk_buff in kernel 5.x.
 *
 * We use the skb->cb[] control buffer to store chain pointers.
 * The cb[] array is 48 bytes on 32-bit ARM, we use bytes 40-44
 * for the chain next pointer and 44-48 for the PKT chain flag.
 */

#ifndef _SKB_COMPAT_H_
#define _SKB_COMPAT_H_

#include <linux/version.h>
#include <linux/skbuff.h>

/*
 * In kernels >= 5.4, skb->next, skb->prev, and skb->cloned are gone.
 * We store a chain "next" pointer in the tail of skb->cb[].
 * skb->cloned is replaced by skb_cloned(skb) inline function.
 */
#define SKB_CB_CHAIN_NEXT_OFFSET	40
#define SKB_CB_CHAIN_PKT_OFFSET		44

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)

#include <linux/poison.h>

static inline struct sk_buff *skb_chain_next(const struct sk_buff *skb)
{
	struct sk_buff *ret;
	memcpy(&ret, skb->cb + SKB_CB_CHAIN_NEXT_OFFSET, sizeof(ret));
	return ret;
}

static inline void skb_chain_set_next(struct sk_buff *skb, struct sk_buff *next)
{
	memcpy(skb->cb + SKB_CB_CHAIN_NEXT_OFFSET, &next, sizeof(next));
}

static inline struct sk_buff *skb_chain_prev(const struct sk_buff *skb __always_unused)
{
	return NULL;
}

static inline void skb_chain_set_prev(struct sk_buff *skb __always_unused,
				      struct sk_buff *prev __always_unused)
{
}

static inline int skb_chain_cloned(const struct sk_buff *skb)
{
	return *(int *)(skb->cb + SKB_CB_CHAIN_PKT_OFFSET);
}

static inline void skb_chain_set_cloned(struct sk_buff *skb, int val)
{
	*(int *)(skb->cb + SKB_CB_CHAIN_PKT_OFFSET) = val;
}

/* Override struct sk_buff to add compatibility fields via accessor macros */
#define skb_compat_next		skb_chain_next
#define skb_compat_set_next	skb_chain_set_next
#define skb_compat_prev		skb_chain_prev
#define skb_compat_set_prev	skb_chain_set_prev
#define skb_compat_cloned	skb_chain_cloned
#define skb_compat_set_cloned	skb_chain_set_cloned

#else /* LINUX_VERSION_CODE < 5.4.0 — legacy kernels with skb->next/prev/cloned */

#define skb_compat_next(skb)		((skb)->next)
#define skb_compat_set_next(skb, n)	((skb)->next = (n))
#define skb_compat_prev(skb)		((skb)->prev)
#define skb_compat_set_prev(skb, p)	((skb)->prev = (p))
#define skb_compat_cloned(skb)		((skb)->cloned)
#define skb_compat_set_cloned(skb, v)	((skb)->cloned = (v))

#endif /* LINUX_VERSION_CODE */

#endif /* _SKB_COMPAT_H_ */
