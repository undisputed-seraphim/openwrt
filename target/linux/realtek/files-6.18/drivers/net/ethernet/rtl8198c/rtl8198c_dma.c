// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- K: descriptor rings, TX/RX, NAPI
 *
 * The pkthdr/mbuf rings, the TX and RX data paths, checksum offload,
 * scatter-gather, the RX queue mapping and the interrupt/NAPI handlers.
 */
#include "rtl8198c.h"

/* ================================================================
 * Legacy pkthdr/mbuf descriptor engine
 * ================================================================ */

/* CPUQDM0/2/4 each hold two 16-bit entries.  Bits 14:12 select the RX
 * ring for the CPU queue; the other three 3-bit fields do the same for
 * the extension ports.  Write them 32-bit: CPUQDM4 clears itself if
 * written as 16-bit. */
static void rx_qmap_program(struct rtl8198c_eth *eth)
{
	int q;

	/* Only queues 0-5 exist: CPUQDM0/2/4 cover two queues each, and the
	 * next 32-bit slot is DMA_CR0. */
	for (q = 0; q < RX_RINGS; q++) {
		u32 off = CPUQDM0 + (q >> 1) * 4;
		u32 shift = (q & 1) ? 0 : 16;
		u32 v = cpu_r32(off);

		v &= ~(0xFFFFu << shift);
		v |= (u32)(eth->rx_qmap[q] * 0x1111) << shift;
		cpu_w32(off, v);
	}
}

static void rx_qmap_reset(struct rtl8198c_eth *eth)
{
	int q;

	for (q = 0; q < 8; q++)
		eth->rx_qmap[q] = q < RX_RINGS ? q : RX_RINGS - 1;
	rx_qmap_program(eth);
}

/* Reserve headroom so the IP header behind the 14-byte Ethernet header is
 * 4-byte aligned: MIPS has no efficient unaligned access and traps into the
 * emulator, which spinlocks under load.  The RISC writes the frame at
 * skb->data, so the buffer handed to the hardware is 2 mod 4; RX_OFFSET
 * is 2 for exactly this reason. */
static void rx_align_skb(struct sk_buff *skb)
{
	int align = ((unsigned long)skb->data + 14) & 3;

	if (align)
		skb_reserve(skb, 4 - align);
}

static struct sk_buff *rx_alloc_skb(struct rtl8198c_eth *eth, gfp_t gfp)
{
	struct sk_buff *skb;

	skb = __netdev_alloc_skb(NULL, eth->rx_bufsz + RX_BUF_SLACK, gfp);
	if (skb)
		rx_align_skb(skb);
	return skb;
}

/* Refill from the NAPI poll: napi_alloc_skb() takes the skb struct from the
 * per-CPU NAPI cache instead of the generic allocator. */
static struct sk_buff *rx_alloc_skb_napi(struct rtl8198c_eth *eth)
{
	struct sk_buff *skb;

	skb = napi_alloc_skb(&eth->napi, eth->rx_bufsz + RX_BUF_SLACK);
	if (skb)
		rx_align_skb(skb);
	return skb;
}

/* RX mbuf ring index for an mbuf pointer (0..RX_RINGS*RX_RING_SIZE-1).
 * The pool is shared with TX, whose mbufs come first. */
static int rx_mbuf_idx(struct rtl8198c_eth *eth, struct mbuf *mb)
{
	return ((u8 *)mb - (u8 *)kseg1(eth->mbuf_pool)) /
		sizeof(struct mbuf) - TX_RINGS * RING_SIZE;
}

/* Point the CPU interface at the rings and start it.
 *
 * CPUTPDCR0/1 read back as the hardware's current descriptor pointer, not
 * the base written here (a ring with 13 entries queued reads as
 * base + 13 * 4), so a ring base can never be verified by reading it back.
 */
void rtl8198c_eth_rings_program(struct rtl8198c_eth *eth)
{
	int i;

	for (i = 0; i < RX_RINGS; i++)
		cpu_w32(CPURPDCR0 + i * 4, (u32)eth->rx_ring +
			i * RX_RING_SIZE * sizeof(u32));
	cpu_w32(CPUTPDCR0, (u32)eth->tx_ring);
	cpu_w32(CPUTPDCR1, (u32)eth->tx_ring + TX_RING_SIZE * sizeof(u32));
	cpu_w32(CPURMDCR0, (u32)eth->mbuf_ring);
	rx_qmap_program(eth);
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
	cpu_w32(CPUICR, BIT(31) | BIT(30) | BIT(3) | MBUF_2048);
	cpu_w32(DMA_CR1, (RING_SIZE - 1) * sizeof(u32));
	cpu_w32(DMA_CR4, cpu_r32(DMA_CR4) |
			TX_RING0_TAIL_AWARE | TX_RING1_TAIL_AWARE);
	cpu_w32(TXRINGCR, TX_RING0_EN | TX_RING1_EN);
}

int rtl8198c_alloc_rings(struct rtl8198c_eth *eth)
{
	int i, r;
	struct pkthdr *pkt;
	struct mbuf *mb;
	int total_pkthdr, total_mbuf;
	u32 pkthdr_pool_sz, mbuf_pool_sz, sg_pool_sz;

	/* Ring arrays: u32 pointer arrays for rx, tx, mbuf.  The caller
	 * (eth_probe) frees whatever was allocated if this fails. */
	eth->rx_ring = rtl8198c_uncached_alloc(RX_RINGS * RX_RING_SIZE * sizeof(u32), NULL,
				      &eth->rx_ring_k0);
	if (!eth->rx_ring) return -ENOMEM;
	eth->tx_ring = rtl8198c_uncached_alloc(TX_RINGS * RING_SIZE * sizeof(u32), NULL,
				      &eth->tx_ring_k0);
	if (!eth->tx_ring) return -ENOMEM;
	eth->mbuf_ring = rtl8198c_uncached_alloc(RX_RINGS * RX_RING_SIZE * sizeof(u32), NULL,
					&eth->mbuf_ring_k0);
	if (!eth->mbuf_ring) return -ENOMEM;

	/* Pools: one pkthdr/mbuf per RX and TX descriptor. */
	total_pkthdr = RING_SIZE * (RX_RINGS + TX_RINGS);
	total_mbuf   = RING_SIZE * (RX_RINGS + TX_RINGS);
	pkthdr_pool_sz  = total_pkthdr * sizeof(struct pkthdr);
	mbuf_pool_sz    = total_mbuf * sizeof(struct mbuf);

	eth->pktHdr_pool = kmalloc(pkthdr_pool_sz, GFP_KERNEL);
	if (!eth->pktHdr_pool) return -ENOMEM;
	memset(eth->pktHdr_pool, 0, pkthdr_pool_sz);

	eth->mbuf_pool = kmalloc(mbuf_pool_sz, GFP_KERNEL);
	if (!eth->mbuf_pool) return -ENOMEM;
	memset(eth->mbuf_pool, 0, mbuf_pool_sz);

	sg_pool_sz = TX_RINGS * TX_RING_SIZE * TX_SG_MBUF *
		     sizeof(struct mbuf);
	eth->sg_mbuf_k0 = kmalloc(sg_pool_sz, GFP_KERNEL);
	if (!eth->sg_mbuf_k0) return -ENOMEM;
	memset(eth->sg_mbuf_k0, 0, sg_pool_sz);
	eth->sg_mbuf_pool = (struct mbuf *)kseg1(eth->sg_mbuf_k0);

	/* Init TX pkthdr entries — all pointers converted to KSEG1 (uncached)
	 * so the RISC can read/write them coherently. */
	pkt = (struct pkthdr *)kseg1(eth->pktHdr_pool);
	mb  = (struct mbuf *)kseg1(eth->mbuf_pool);
	for (i = 0; i < TX_RINGS * RING_SIZE; i++) {
		memset(pkt, 0, sizeof(*pkt));
		memset(mb, 0, sizeof(*mb));
		pkt->ph_mbuf = mb;
		pkt->ph_len = 0;
		pkt->ph_flags = PKTHDR_USED | PKT_OUTGOING;
		pkt->ph_type = 0;
		pkt->ph_portlist = 0;
		mb->m_next = NULL;
		mb->m_pkthdr = pkt;
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_data = NULL;
		mb->m_extbuf = NULL;
		mb->m_extsize = 0;
		eth->tx_ring[i] = (u32)pkt | DESC_RISC_OWNED;
		pkt++;
		mb++;
	}
	/* Each TX ring is a ring of its own, so each gets its own wrap bit. */
	for (i = 0; i < TX_RINGS; i++)
		eth->tx_ring[(i + 1) * TX_RING_SIZE - 1] |= DESC_WRAP;

	/* Init RX pkthdr + mbuf entries.  The shared mbuf ring is laid out
	 * ring by ring, so MBUF index = ring * RX_RING_SIZE + slot.  Buffers
	 * are attached by rtl8198c_refill_rx() at open, when the budget size is known
	 * and the CPU engine is still stopped. */
	eth->rx_bufsz = RX_BUF_MIN;
	for (r = 0; r < RX_RINGS; r++) {
		for (i = 0; i < RX_RING_SIZE; i++) {
			int idx = r * RX_RING_SIZE + i;

			memset(pkt, 0, sizeof(*pkt));
			memset(mb, 0, sizeof(*mb));
			pkt->ph_mbuf = mb;
			pkt->ph_len = 0;
			pkt->ph_flags = PKTHDR_USED | PKT_INCOMING;
			pkt->ph_type = 0;
			pkt->ph_portlist = 0;
			mb->m_next = NULL;
			mb->m_pkthdr = pkt;
			mb->m_len = 0;
			mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
			mb->m_extsize = eth->rx_bufsz;
			/* Bound by rtl8198c_refill_rx(): rx_align_skb() reserves 0..2
			 * bytes so the IP header lands on a 4-byte boundary,
			 * and the RISC writes at skb->data. */
			mb->m_data = mb->m_extbuf = NULL;
			eth->rx_ring[idx] = (u32)pkt | DESC_SWCORE_OWNED;
			eth->mbuf_ring[idx] = (u32)mb | DESC_SWCORE_OWNED;
			pkt++;
			mb++;
		}
	}
	for (r = 0; r < RX_RINGS; r++)
		eth->rx_ring[(r + 1) * RX_RING_SIZE - 1] |= DESC_WRAP;
	eth->mbuf_ring[RX_RINGS * RX_RING_SIZE - 1] |= DESC_WRAP;

	/* TX completion tracking; the RX slots are filled by rtl8198c_refill_rx(). */
	for (i = 0; i < RX_RINGS * RX_RING_SIZE; i++)
		eth->rx_skb[i] = NULL;
	for (r = 0; r < TX_RINGS; r++)
		for (i = 0; i < TX_RING_SIZE; i++)
			eth->tx_skb[r][i] = NULL;

	/* Program ring base registers at system (KSEG1) addresses.  The
	 * queue->ring map must be in place before RX is enabled. */
	for (r = 0; r < RX_RINGS; r++)
		cpu_w32(CPURPDCR0 + r * 4, (u32)eth->rx_ring +
			r * RX_RING_SIZE * sizeof(u32));
	cpu_w32(CPUTPDCR0, (u32)eth->tx_ring);
	cpu_w32(CPUTPDCR1, (u32)eth->tx_ring + TX_RING_SIZE * sizeof(u32));
	cpu_w32(CPURMDCR0, (u32)eth->mbuf_ring);
	rx_qmap_reset(eth);
	/* Ring addresses are programmed here but the CPU interface stays
	 * disabled until the first eth_open().  Enabling it now would let the
	 * RISC fill descriptors before the driver starts polling, and the
	 * ring pointers can only be reset while the engine is stopped. */
	cpu_w32(DMA_CR1, (RING_SIZE - 1) * sizeof(u32));
	cpu_w32(DMA_CR4, cpu_r32(DMA_CR4) |
			TX_RING0_TAIL_AWARE | TX_RING1_TAIL_AWARE);
	cpu_w32(TXRINGCR, TX_RING0_EN | TX_RING1_EN);
	cpu_w32(DMA_CR0, cpu_r32(DMA_CR0) | DMA_HSB_ADDR_MARK);

	for (r = 0; r < RX_RINGS; r++) {
		eth->rx_head[r] = 0;
		eth->rx_ring_packets[r] = 0;
	}
	for (r = 0; r < TX_RINGS; r++)
		eth->tx_head[r] = eth->tx_tail[r] = 0;

	return 0;
}

void rtl8198c_free_rx_skbs(struct rtl8198c_eth *eth)
{
	int i;

	for (i = 0; i < RX_RINGS * RX_RING_SIZE; i++) {
		if (!eth->rx_skb[i])
			continue;
		dev_kfree_skb_any(eth->rx_skb[i]);
		eth->rx_skb[i] = NULL;
	}
}

void rtl8198c_free_rings(struct rtl8198c_eth *eth)
{
	rtl8198c_free_rx_skbs(eth);
	kfree(eth->rx_ring_k0);
	kfree(eth->tx_ring_k0);
	kfree(eth->mbuf_ring_k0);
	kfree(eth->pktHdr_pool);
	kfree(eth->mbuf_pool);
	kfree(eth->sg_mbuf_k0);
}

/* Buffer size for the largest MTU any port is configured with.  The RX
 * ring is shared by all ports, so it has to cover the biggest frame the
 * switch may hand to the CPU. */
static u16 rx_buf_wanted(struct rtl8198c_eth *eth)
{
	u16 mtu = 0;
	int i;

	for (i = 0; i < ETH_PORTS; i++)
		if (eth->port_mtu[i] > mtu)
			mtu = eth->port_mtu[i];
	return mtu + RX_BUF_SLACK > RX_BUF_MIN ? MBUF_BUF_SIZE : RX_BUF_MIN;
}

/* Resize the RX buffers.  The caller must have the CPU engine stopped and
 * refill afterwards; freeing before allocating keeps the peak at one set. */
void rtl8198c_eth_rx_bufs_sync(struct rtl8198c_eth *eth)
{
	u16 want = rx_buf_wanted(eth);

	if (want == eth->rx_bufsz)
		return;
	rtl8198c_free_rx_skbs(eth);
	eth->rx_bufsz = want;
}

/* Point the RX side at its rings and enable it.  TX is deliberately left
 * alone: its ring pointers, head/tail and in-flight descriptors must keep
 * running across an RX buffer resize. */
static void eth_rx_rings_program(struct rtl8198c_eth *eth)
{
	int i;

	for (i = 0; i < RX_RINGS; i++)
		cpu_w32(CPURPDCR0 + i * 4, (u32)eth->rx_ring +
			i * RX_RING_SIZE * sizeof(u32));
	cpu_w32(CPURMDCR0, (u32)eth->mbuf_ring);
	rx_qmap_program(eth);
	/* Status is write-1-to-clear: drop stale bits before the enable so a
	 * frame completing immediately still leaves an interrupt pending. */
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
	cpu_w32(CPUICR, cpu_r32(CPUICR) | RXCMD);
}

/* Rebuild the RX ring after an MTU change.  The engine is pointed at
 * skb->data values that are about to be freed, and its ring pointers only
 * reset while RX is stopped. */
void rtl8198c_eth_rx_restart(struct rtl8198c_eth *eth)
{
	u16 old = eth->rx_bufsz;
	int i;

	napi_disable(&eth->napi);
	cpu_w32(CPUIIMR, 0);
	cpu_w32(CPUICR, cpu_r32(CPUICR) & ~RXCMD);

	/* Reprogramming CPURPDCR puts the RISC back at slot 0, so the
	 * driver's consumer index has to go back with it. */
	for (i = 0; i < RX_RINGS; i++)
		eth->rx_head[i] = 0;

	rtl8198c_free_rx_skbs(eth);
	rtl8198c_eth_rx_bufs_sync(eth);
	if (rtl8198c_refill_rx(eth)) {
		/* Out of memory at the new size: go back to the old one rather
		 * than leave the interface deaf. */
		eth->rx_bufsz = old;
		if (rtl8198c_refill_rx(eth)) {
			/* No buffers at all: leave RX disabled rather than let
			 * the RISC write through the stale m_data pointers.
			 * TX and NAPI stay alive; the next open retries. */
			rtl8198c_switch_start();
			napi_enable(&eth->napi);
			napi_schedule(&eth->napi);
			return;
		}
	}
	eth_rx_rings_program(eth);
	/* Re-arms RX_DONE (and the GIMR gate) after the stop above. */
	rtl8198c_switch_start();
	napi_enable(&eth->napi);
	napi_schedule(&eth->napi);
}

/* Give every RX descriptor a buffer and hand the ring back to the RISC.
 * The buffers exist from rtl8198c_alloc_rings() on a first open; a stop frees them
 * and this reallocates them.  A stop can leave descriptors filled but
 * unread, so clear those too: the restart reprograms the RISC ring
 * pointers, and a stale ph_len read against a rebound buffer would deliver
 * the wrong bytes. */
int rtl8198c_refill_rx(struct rtl8198c_eth *eth)
{
	int r, i;

	for (r = 0; r < RX_RINGS; r++) {
		for (i = 0; i < RX_RING_SIZE; i++) {
			int idx = r * RX_RING_SIZE + i;
			struct pkthdr *ph = (struct pkthdr *)
				(eth->rx_ring[idx] &
				 ~(DESC_OWNED_BIT | DESC_WRAP));
			struct sk_buff *skb = eth->rx_skb[idx];

			if (!skb) {
				skb = rx_alloc_skb(eth, GFP_KERNEL);
				if (!skb) {
					rtl8198c_free_rx_skbs(eth);
					return -ENOMEM;
				}
				eth->rx_skb[idx] = skb;
			}
			/* The RISC rewrites ph_mbuf to the mbuf it took from
			 * the linear mbuf ring; put the descriptor's own mbuf
			 * back so a restart begins from the init mapping. */
			ph->ph_mbuf = (struct mbuf *)(eth->mbuf_ring[idx] &
						      ~(DESC_OWNED_BIT |
							DESC_WRAP));
			ph->ph_len = 0;
			ph->ph_mbuf->m_len = 0;
			ph->ph_mbuf->m_data = ph->ph_mbuf->m_extbuf = skb->data;
			ph->ph_mbuf->m_extsize = eth->rx_bufsz;
			eth->rx_ring[idx] |= DESC_SWCORE_OWNED;
			eth->mbuf_ring[idx] |= DESC_SWCORE_OWNED;
		}
	}
	return 0;
}

/* IPv4 header checksum, 20 bytes.  The ASIC leaves the field at zero even
 * with CSUM_IP set, so the driver has to do this half itself. */
static void ipv4_hdr_csum(struct iphdr *iph)
{
	u32 sum = 0;
	u8 *p = (u8 *)iph;
	int i;

	iph->check = 0;
	for (i = 0; i < sizeof(struct iphdr); i += 2)
		sum += ((u16)p[i] << 8) | p[i + 1];
	sum = (sum & 0xFFFF) + (sum >> 16);
	sum = (sum & 0xFFFF) + (sum >> 16);
	iph->check = htons(~sum);
}

/* Describe the packet to the checksum engine.
 *
 * Only plain Ethernet II IPv4 is handed to the ASIC: the IPv6 side of this
 * descriptor is reserved (the fields are not implemented for little-endian
 * builds), so nothing beyond IPv4 TCP/UDP is worth trusting.  Everything
 * else leaves the fields cleared and returns false, and the caller
 * finishes the checksum in software.
 */
static bool tx_csum_describe(struct sk_buff *skb, struct pkthdr *ph)
{
	u8 proto;

	ph->ph_type = 0;
	ph->ph_vlanTagged = 0;
	ph->ph_LLCTagged = 0;
	ph->ph_pppeTagged = 0;
	ph->ph_ipIpv4 = 0;
	ph->ph_ipIpv4_1st = 0;
	ph->ph_ipIpv6 = 0;
	ph->ph_ipIpv6HdrLen = 0;

	if (skb->protocol != htons(ETH_P_IP))
		return false;

	/* CHECKSUM_PARTIAL means the L4 header is present and the packet is
	 * not fragmented yet (fragmentation happens after the transport
	 * checksum is set up). */
	proto = ip_hdr(skb)->protocol;
	ph->ph_ipIpv4 = 1;
	ph->ph_ipIpv4_1st = 1;

	if (proto == IPPROTO_TCP)
		ph->ph_type = PKTHDR_TCP;
	else if (proto == IPPROTO_UDP)
		ph->ph_type = PKTHDR_UDP;
	else
		return false;

	return true;
}

/* TX using legacy pkthdr/mbuf format */
netdev_tx_t rtl8198c_eth_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	struct pkthdr *ph;
	void *data_k1;
	unsigned long flags;
	u32 len = skb->len;
	int idx, next, i, ring, nfrags;

	/* 802.1p priorities 4-7 take the second ring. */
	ring = skb->priority >= 4 ? 1 : 0;

	spin_lock_irqsave(&eth->tx_lock, flags);

	idx = eth->tx_head[ring];
	next = (idx + 1) & (TX_RING_SIZE - 1);
	/* Check ring full: next entry must not be the done index (tx_tail) */
	if (next == eth->tx_tail[ring])
		goto ring_full;

	if (len < ETH_ZLEN) len = ETH_ZLEN;

	ph = (struct pkthdr *)(*(volatile u32 *)
				&eth->tx_ring[ring * TX_RING_SIZE + idx] &
				~(DESC_OWNED_BIT | DESC_WRAP));
	if (!ph->ph_mbuf)
		goto ring_full;

	/* The RISC walks mbuf chains but knows nothing about frag_list, so
	 * anything it cannot express is linearised first.  frag_list appears
	 * on forwarded GRO-merged frames; more frags than a chain holds
	 * should not happen with MAX_SKB_FRAGS spare mbufs but is handled. */
	if (skb_is_nonlinear(skb) &&
	    (skb_shinfo(skb)->frag_list ||
	     skb_shinfo(skb)->nr_frags > TX_SG_MBUF)) {
		if (skb_linearize(skb)) {
			dev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			spin_unlock_irqrestore(&eth->tx_lock, flags);
			return NETDEV_TX_OK;
		}
		len = skb->len;
		if (len < ETH_ZLEN)
			len = ETH_ZLEN;
		eth->tx_lin_packets++;
	}
	ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		if (tx_csum_describe(skb, ph)) {
			if (skb->protocol == htons(ETH_P_IP))
				ipv4_hdr_csum(ip_hdr(skb));
			ph->ph_flags |= CSUM_TCPUDP;
		} else if (skb_checksum_help(skb)) {
			dev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			spin_unlock_irqrestore(&eth->tx_lock, flags);
			return NETDEV_TX_OK;
		}
	} else {
		tx_csum_describe(skb, ph);	/* clears the fields */
	}

	/* The buffer is taken after any software checksum fixup, which can
	 * change skb->data (and linearise).  With fragments, the pkthdr's
	 * own mbuf carries
	 * the linear part and m_next chains one spare mbuf per fragment,
	 * the last marked MBUF_EOR and given four extra bytes for the CRC
	 * (the switch appends it after the final buffer). */
	nfrags = skb_shinfo(skb)->nr_frags;
	ph->ph_len = len + 4;
	eth->tx_sg_packets += !!nfrags;
	if (nfrags) {
		struct mbuf *mb = eth->sg_mbuf_pool +
			(ring * TX_RING_SIZE + idx) * TX_SG_MBUF;
		u32 headlen = skb_headlen(skb);

		ph->ph_mbuf->m_extbuf = kseg1(skb->data);
		ph->ph_mbuf->m_data = ph->ph_mbuf->m_extbuf;
		ph->ph_mbuf->m_len = headlen;
		ph->ph_mbuf->m_extsize = headlen;
		ph->ph_mbuf->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR;
		ph->ph_mbuf->m_next = mb;
		cache_wb(skb->data, headlen);

		for (i = 0; i < nfrags; i++) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			void *addr = page_address(skb_frag_page(frag)) +
				     skb_frag_off(frag);
			u32 fsz = skb_frag_size(frag);
			bool last = (i == nfrags - 1);

			mb->m_next = last ? NULL : mb + 1;
			mb->m_pkthdr = ph;
			mb->m_data = mb->m_extbuf = addr;
			mb->m_len = fsz + (last ? 4 : 0);
			mb->m_extsize = mb->m_len;
			mb->m_flags = MBUF_USED | MBUF_EXT |
				      (last ? MBUF_EOR : 0);
			cache_wb(addr, fsz);
			mb++;
		}
	} else {
		data_k1 = kseg1(skb->data);
		ph->ph_mbuf->m_extbuf = data_k1;
		ph->ph_mbuf->m_data   = data_k1;
		ph->ph_mbuf->m_len = ph->ph_len;
		ph->ph_mbuf->m_extsize = ph->ph_len;
		ph->ph_mbuf->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR |
				       MBUF_EOR;
		ph->ph_mbuf->m_next = NULL;
		cache_wb(skb->data, len);  /* RISC reads through its own bus */
	}
	/* Egress only this netdev's port.  ph_portlist is a mask here (the RX
	 * direction uses the same field as a port number), and the switch
	 * trusts it: for CPU-injected frames the RISC does not look the
	 * destination up again, so a frame bridged or routed out of the wrong
	 * netdev leaves through the wrong jack.  It also does not learn from
	 * these frames, so a software bridge cannot poison the FDB with a
	 * source it invented. */
	ph->ph_portlist = BIT(p->port);
	/* The priority class also selects the egress queue, so the switch
	 * scheduler can prefer it over bulk traffic.  QNUMCR gives every
	 * port two queues, one per TX ring; ph_txPriority feeds UPTCMCR1,
	 * which maps 0-3 to queue 0 and 4-7 to queue 1. */
	ph->ph_queueId = ring;
	ph->ph_txPriority = ring ? 5 : 0;

	eth->tx_skb[ring][idx] = skb;
	eth->tx_head[ring] = next;
	eth->tx_ring_packets[ring]++;

	dev_sw_netstats_tx_add(dev, 1, skb->len);

	/* Give the descriptor to RISC (set OWN=1) and kick TXFD.  The
	 * doorbell is required even though DMA_CR4 has TAIL_AWARE set:
	 * measured, removing it queues packets (tx_head advances) but the
	 * RISC never dequeues them and tx_tail stays at 0. */
	*(volatile u32 *)&eth->tx_ring[ring * TX_RING_SIZE + idx] |=
		DESC_SWCORE_OWNED;
	cpu_w32(CPUICR, cpu_r32(CPUICR) | TXFD);
	spin_unlock_irqrestore(&eth->tx_lock, flags);
	return NETDEV_TX_OK;

ring_full:
	/* One ring serves every port, so stop all queues, not just this one. */
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i])
			netif_stop_queue(eth->netdev[i]);
	spin_unlock_irqrestore(&eth->tx_lock, flags);
	return NETDEV_TX_BUSY;
}

void rtl8198c_tx_clean(struct rtl8198c_eth *eth)
{
	unsigned long flags;
	bool freed = false;
	int i, r;

	spin_lock_irqsave(&eth->tx_lock, flags);
	for (r = 0; r < TX_RINGS; r++) {
		while (eth->tx_tail[r] != eth->tx_head[r]) {
			int idx = eth->tx_tail[r];
			u32 v = *(volatile u32 *)
				&eth->tx_ring[r * TX_RING_SIZE + idx];

			if (v & DESC_OWNED_BIT)
				break;	/* CPU owns = still pending */
			if (eth->tx_skb[r][idx]) {
				dev_kfree_skb_any(eth->tx_skb[r][idx]);
				eth->tx_skb[r][idx] = NULL;
			}
			eth->tx_tail[r] = (idx + 1) & (TX_RING_SIZE - 1);
			freed = true;
		}
	}
	if (freed)
		for (i = 0; i < ETH_PORTS; i++)
			if (eth->netdev[i])
				netif_wake_queue(eth->netdev[i]);
	spin_unlock_irqrestore(&eth->tx_lock, flags);
}

/* Legacy RX poll */
/* Frames addressed to one of our own MACs belong to that port's netdev no
 * matter where they arrived; every
 * other frame - broadcast, multicast, and anything the switch flooded - is
 * attributed to the port it came in on. */
static struct net_device *eth_rx_netdev(struct rtl8198c_eth *eth,
					const void *data, u32 src_port)
{
	const u8 *mac = data;
	struct net_device *srcdev = NULL;
	int i;

	if (src_port < ETH_PORTS)
		srcdev = eth->netdev[src_port];

	/* A bridged port owns every frame that arrives on it.  Matching the
	 * destination MAC first is right for "one address per port", but a
	 * bridge's MAC is one of its member ports' MACs, so a frame for the
	 * bridge that ingressed on another member would be handed to the
	 * wrong netdev: the bridge then learns the sender on that port and
	 * answers out the wrong cable.  Measured with br-lan over eth3+eth4,
	 * bridge MAC = eth3's, host on port 4: the ICMP replies left through
	 * the port 3 cable and the ping got no answer.  Flood-class frames are
	 * then duplicated between the layers: the switch floods them to the
	 * other ports and the software bridge floods its copy again. */
	if (srcdev && netif_is_bridge_port(srcdev))
		return srcdev;

	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i] &&
		    ether_addr_equal(mac, eth->netdev[i]->dev_addr))
			return eth->netdev[i];
	if (srcdev)
		return srcdev;
	return eth->netdev[0];
}

/* Frames the switch itself flooded inside the VLAN: broadcast, multicast
 * and unicast that is not one of our own addresses (the static entries for
 * those point at the CPU only).  The CPU gets a copy, but the other member
 * ports already have the frame, so the bridge must not forward it again. */
static bool eth_rx_flooded(struct rtl8198c_eth *eth, const u8 *data)
{
	int i;

	if (is_multicast_ether_addr(data))
		return true;
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i] &&
		    ether_addr_equal(data, eth->netdev[i]->dev_addr))
			return false;
	return true;
}

/* Drain up to budget frames from one RX ring.  Returns the number of
 * descriptors consumed. */
static int rx_poll_ring(struct rtl8198c_eth *eth, int ring, int budget)
{
	int base = ring * RX_RING_SIZE;
	int done = 0;

	while (done < budget) {
		struct pkthdr *ph;
		struct mbuf *mb;
		int idx = eth->rx_head[ring];
		int midx = -1;
		u32 v, src_port;

		/* Read descriptor via KSEG1 (uncached) — ring entries are KSEG1 pointers */
		v = *(volatile u32 *)&eth->rx_ring[base + idx];
		if (v & DESC_OWNED_BIT) break; /* CPU owns = empty */

		ph = (struct pkthdr *)(v & ~(DESC_OWNED_BIT | DESC_WRAP));
		if (!ph->ph_len) break;
		mb = ph->ph_mbuf;
		if (!mb) break;

		/* RX ph_portlist holds the source port number on 8198C, not a
		 * mask (ph_portlist & 0x7). */
		src_port = ph->ph_portlist & 0x7;
		if (src_port < ETH_PORTS)
			eth->rx_port_packets[src_port]++;
		/* The switch reports the queue it assigned; keep the count and
		 * pass it up as the packet priority. */
		eth->rx_queue_packets[ph->ph_queueId & 7]++;
		eth->rx_ring_packets[ring]++;
		if (ph->ph_vlanTagged)
			eth->rx_vlan_tagged++;
		else
			eth->rx_vlan_untagged++;
		eth->rx_vlan_last = (ph->ph_vlanId << 1) | ph->ph_vlanTagged;

		{
			struct net_device *rdev;
			struct sk_buff *skb, *new_skb;
			bool flooded;
			int len = ph->ph_len - 4; /* ph_len includes CRC */

			midx = rx_mbuf_idx(eth, mb);

			/* The frame is in the buffer of the mbuf the RISC used,
			 * not the descriptor's own mbuf: the RISC takes its
			 * buffer from the shared *linear* mbuf ring (one entry
			 * per frame, whatever the RX ring) and rewrites ph_mbuf
			 * to the entry it took.  So the skb is keyed by mbuf
			 * index, and the fresh buffer goes back on that same
			 * mbuf, which the RISC reuses a full lap later. */
			if (unlikely(midx < 0 ||
				     midx >= RX_RINGS * RX_RING_SIZE)) {
				rdev = eth->netdev[0];
				rdev->stats.rx_dropped++;
				goto next;
			}
			skb = eth->rx_skb[midx];
			if (unlikely(!skb)) {
				eth->netdev[0]->stats.rx_dropped++;
				goto next;
			}
			rdev = eth_rx_netdev(eth, skb->data, src_port);
			new_skb = (len > 0 && len <= eth->rx_bufsz) ?
				rx_alloc_skb_napi(eth) : NULL;
			if (!new_skb) {
				/* Drop the frame; the old buffer stays bound. */
				rdev->stats.rx_dropped++;
				goto next;
			}

			mb->m_data = mb->m_extbuf = new_skb->data;
			mb->m_extsize = eth->rx_bufsz;
			eth->rx_skb[midx] = new_skb;
			mb->m_len = 0;

			skb_put(skb, len);
			/* Read the destination before eth_type_trans() pulls
			 * the MAC header. */
			flooded = eth_rx_flooded(eth, skb->data);
			skb->protocol = eth_type_trans(skb, rdev);
			/* Already flooded by the switch: lets the bridge skip
			 * its own flood instead of sending a second copy. */
			if (flooded)
				skb->offload_fwd_mark = 1;
			/* eth_type_trans() only sets the MAC header; the checksum
			 * and priority paths below parse the IP header. */
			skb_reset_network_header(skb);
			skb->ip_summed = CHECKSUM_NONE;
			/* The switch reports the IPv4 header and TCP/UDP
			 * checksum results in ph_flags bits 0/1.  Measured with
			 * crafted frames: good IPv4 TCP/UDP has both bits, a bad
			 * L4 checksum leaves only IP_OK and a bad IP header only
			 * TCPUDP_OK.  CSCR's own drop-on-error bit does not cover
			 * CPU-bound frames, but these per-frame flags are
			 * accurate.  Trust them only for unfragmented IPv4
			 * TCP/UDP with both bits set; everything else (other
			 * protocols, IPv6, fragments, tagged frames) stays
			 * software-verified. */
			if (skb->protocol == htons(ETH_P_IP) &&
			    skb->len >= sizeof(struct iphdr)) {
				const struct iphdr *iph = ip_hdr(skb);

				if ((iph->protocol == IPPROTO_TCP ||
				     iph->protocol == IPPROTO_UDP) &&
				    !(ntohs(iph->frag_off) & 0x3fff) &&	/* MF|offset */
				    (ph->ph_flags &
				     (CSUM_TCPUDP_OK | CSUM_IP_OK)) ==
				    (CSUM_TCPUDP_OK | CSUM_IP_OK))
					skb->ip_summed = CHECKSUM_UNNECESSARY;
			}
			skb->priority = ph->ph_queueId;
			napi_gro_receive(&eth->napi, skb);
			dev_sw_netstats_rx_add(rdev, len);
		}

next:
		/* Return both halves to the RISC.  The mbuf is re-armed at
		 * the index the RISC took from its linear mbuf ring, which
		 * is not the descriptor slot once the ring has wrapped. */
		if (midx >= 0 && midx < RX_RINGS * RX_RING_SIZE)
			eth->mbuf_ring[midx] |= DESC_SWCORE_OWNED;
		eth->rx_ring[base + idx] |= DESC_SWCORE_OWNED;

		/* Acknowledge any runout the drain just made stale.  The
		 * status bits are write-1-to-clear, so write them blindly
		 * instead of reading the register back.  The
		 * runout interrupts stay masked: re-enabling one would arm an
		 * interrupt that keeps firing while the ring is full, which is
		 * exactly when it must not.  RX_DONE is the wakeup instead. */
		cpu_w32(CPUIISR, MBUF_DESC_RUNOUT_IP0 |
				PKTHDR_DESC_RUNOUT_IP_ALL);

		eth->rx_head[ring] = (idx + 1) & (RX_RING_SIZE - 1);
		done++;
	}

	return done;
}

/* Service every RX ring, highest ring first.  Each ring gets a share of
 * what is left so a flooded ring cannot starve the others; the loop keeps
 * going while any ring makes progress and the budget lasts. */
int rtl8198c_eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl8198c_eth *eth = container_of(napi, struct rtl8198c_eth, napi);
	int rx_done = 0, r;

	rtl8198c_tx_clean(eth);

	if (eth->link_change) {
		eth->link_change = false;
		rtl8198c_link_update(eth);
	}

	while (rx_done < budget) {
		int progress = 0;

		/* Highest ring first, each taking a share of what is left, so
		 * a flooded ring cannot starve the others. */
		for (r = RX_RINGS - 1; r >= 0 && rx_done < budget; r--) {
			int share = (budget - rx_done) / (r + 1);
			int n;

			if (share < 1)
				share = 1;
			n = rx_poll_ring(eth, r, share);
			rx_done += n;
			progress += n;
		}
		if (!progress)
			break;
	}

	if (rx_done < budget) {
		napi_complete_done(napi, rx_done);
		/* rtl8198c_eth_irq() masks these before scheduling NAPI, so
		 * re-arm all of them.  The TX bits matter as much as the RX
		 * ones: without them tx_clean() only runs when RX traffic
		 * happens to poll the queue, so a quiet RX path stalls TX. */
		cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) | RX_DONE_IE_ALL |
				TX_DONE_IP0 | TX_ALL_DONE_IP1 | LINK_CHANGE_IE);
	}
	return rx_done;
}


/* ===== [Kernel Interface] IRQ, net_device_ops, NAPI, ethtool, debugfs ===== */

/* ===== IRQ (common) ===== */
irqreturn_t rtl8198c_eth_irq(int irq, void *dev_id)
{
	struct rtl8198c_eth *eth = dev_id;
	u32 status = cpu_r32(CPUIISR);

	if (!status)
		return IRQ_NONE;
	cpu_w32(CPUIISR, status);

	/* A descriptor runout means the switch has no buffer left to receive
	 * into.  Freeing descriptors is the only way out, so it has to wake
	 * NAPI just like a completion does: the pending bit stays asserted
	 * until the ring is drained, and ignoring it becomes an interrupt
	 * storm that starves the NAPI which would have cleared it.
	 *
	 * A link change goes the same way rather than being handled here: the
	 * carrier update belongs with the other deferred work, and NAPI re-arms
	 * the interrupt at the end of the poll. */
	if (status & (RX_DONE_IE_ALL | TX_DONE_IP0 | TX_ALL_DONE_IP1 |
		      MBUF_DESC_RUNOUT_IP0 | PKTHDR_DESC_RUNOUT_IP_ALL |
		      LINK_CHANGE_IP)) {
		cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) &
			~(RX_DONE_IE_ALL | TX_DONE_IP0 | TX_ALL_DONE_IP1 |
			  MBUF_DESC_RUNOUT_IE0 |
			  PKTHDR_DESC_RUNOUT_IE_ALL | LINK_CHANGE_IE));
		if (status & LINK_CHANGE_IP)
			eth->link_change = true;
		napi_schedule(&eth->napi);
	}
	return IRQ_HANDLED;
}

/* ===== RX descriptor state (debug) ===== */

static int rxstate_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;
	int r, i;

	for (r = 0; r < RX_RINGS; r++) {
		u32 base = (u32)eth->rx_ring + r * RX_RING_SIZE * sizeof(u32);
		u32 cur = cpu_r32(CPURPDCR0 + r * 4);

		seq_printf(s, "ring%d head=%d hw=%u packets=%llu\n", r,
			   eth->rx_head[r], (cur - base) / sizeof(u32),
			   eth->rx_ring_packets[r]);
	}
	seq_printf(s, "vlan: tagged=%llu untagged=%llu last=%#x (vid %u %s)\n",
		   eth->rx_vlan_tagged, eth->rx_vlan_untagged, eth->rx_vlan_last,
		   eth->rx_vlan_last >> 1,
		   (eth->rx_vlan_last & 1) ? "tagged" : "untagged");
	/* The buffers rotate through the shared mbuf ring, so show the
	 * mbuf index the RISC is about to use, not just the slot. */
	for (i = 0; i < 4; i++) {
		int idx = (eth->rx_head[0] + i) & (RX_RING_SIZE - 1);
		u32 v = *(volatile u32 *)&eth->rx_ring[idx];
		struct pkthdr *ph = (struct pkthdr *)(v & ~3u);
		struct mbuf *mb = ph->ph_mbuf;
		int mbi = mb ? rx_mbuf_idx(eth, mb) : -1;

		seq_printf(s, " [%3d] v=%08x len=%u mb=%d skb=%px\n", idx, v,
			   ph->ph_len, mbi,
			   (mbi >= 0 && mbi < RX_RINGS * RX_RING_SIZE) ?
			   eth->rx_skb[mbi] : NULL);
	}
	return 0;
}

static int rxstate_open(struct inode *inode, struct file *file)
{ return single_open(file, rxstate_show, inode->i_private); }
static const struct file_operations rxstate_fops = {
	.owner = THIS_MODULE, .open = rxstate_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

/* ===== RX queue -> ring mapping (debug) ===== */

static int rxmap_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;
	int q, r;

	for (q = 0; q < RX_RINGS; q++)
		seq_printf(s, "queue %d -> ring %d\n", q, eth->rx_qmap[q]);
	for (r = 0; r < RX_RINGS; r++)
		seq_printf(s, "ring %d: %llu packets, head %d\n", r,
			   eth->rx_ring_packets[r], eth->rx_head[r]);
	return 0;
}

static ssize_t rxmap_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[32];
	ssize_t len;
	int q, r;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (!strcmp(kbuf, "reset")) {
		rx_qmap_reset(eth);
		return len;
	}
	if (sscanf(kbuf, "%d %d", &q, &r) != 2)
		return -EINVAL;
	if (q < 0 || q >= RX_RINGS || r < 0 || r >= RX_RINGS)
		return -EINVAL;
	eth->rx_qmap[q] = r;
	rx_qmap_program(eth);
	return len;
}

static int rxmap_open(struct inode *inode, struct file *file)
{ return single_open(file, rxmap_show, inode->i_private); }
static const struct file_operations rxmap_fops = {
	.owner = THIS_MODULE, .open = rxmap_open,
	.read = seq_read, .write = rxmap_write,
	.llseek = seq_lseek, .release = single_release,
};

/* The debugfs knobs this block owns; the registration itself is central so
 * probe creates one directory. */
void rtl8198c_dma_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("rxmap", 0600, eth->debugfs_dir, eth, &rxmap_fops);
	debugfs_create_file("rxstate", 0400, eth->debugfs_dir, eth, &rxstate_fops);
}
