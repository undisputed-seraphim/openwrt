// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L3: IPv4 routing tables
 *
 * Entry encoding for the route, ARP, next-hop and external-IP tables, the
 * raw table access used to bring the block up, and the FIB mirror that
 * decides which routes the hardware gets.
 *
 * The chain is netif (vid -> router MAC, enHWRoute, MTU) -> route (prefix,
 * netif, process) -> ARP block or next-hop group -> L2 entry.
 *
 * Offload is off by default and only gateway routes are taken: a connected
 * prefix needs an ARP block (neighbours), a bridge egress needs the port the
 * gateway sits on (the bridge's FIB), and a route is only installed when
 * every more specific route with a different next hop is installed too, so a
 * partly programmed table cannot shadow what the kernel would do.
 *
 * The route table is a priority list: the lookup walks it from index 0, so
 * the entries have to be sorted longest-prefix-first, and the last index is
 * the default route that catches everything else.  That one points at the
 * CPU, which is what keeps a route miss from being dropped instead
 * of reaching the kernel.
 *
 * The ARP index is not a hash: a prefix owns the contiguous block
 * [start, start + 2^(32-len)) and the hardware looks up
 * arp[start + (ip & ~mask)].  With 512 entries that limits on-link
 * prefixes to /23 and longer, and it is why the route has to exist before
 * its neighbours can be programmed.
 *
 * Addresses are stored as the big-endian 32-bit word the hardware expects:
 * ntohl() of the __be32 value here.
 *
 * Hardware constraints, measured on the board:
 *  - The netif lookup runs by vid and needs exactly one valid entry per
 *    vid.  A second entry on the same vid stops the L3 stage from running
 *    at all, and routed frames go to the CPU as ordinary L2 traffic.
 *  - ALECR.TTL_1Enable must be set, or a routed frame keeps its TTL.  The
 *    IPv4 header checksum is recomputed by the ASIC either way, and the
 *    frame leaves with the route's netif MAC as its source address.
 *  - A route miss and an empty ARP entry both deliver the frame to the
 *    CPU, so a partially programmed table degrades to software routing
 *    instead of dropping.
 *  - Word 0 holds the address as a big-endian word (0xC0A80300 for
 *    192.168.3.0); the byte-swapped value never matches.
 */
#include "rtl8198c.h"

#include <linux/if_vlan.h>
#include <linux/inetdevice.h>
#include <net/arp.h>
#include <net/fib_notifier.h>
#include <net/ip_fib.h>
#include <net/net_namespace.h>
#include <net/netevent.h>
#include <net/nexthop.h>

/* ===== IPv4 route table (type 2) =====
 *
 * Word 0 is the prefix.  Word 1 holds, from bit 0: mask (prefix length
 * minus one, 0 for the default route), valid, process, internal, isDMZ,
 * netif, then the process-specific next-hop field.  The last index of the
 * table is reserved for the default route.
 */
static void route_hdr(u32 e[8], __be32 prefix, u8 len, u8 process,
		      bool internal)
{
	memset(e, 0, 8 * sizeof(u32));
	e[0] = ip_to_word(prefix);
	e[1] = ((u32)(len ? len - 1 : 0) & 0x1F) | BIT(5) |
	       (((u32)process & 0x7) << 6) | (internal ? BIT(9) : 0);
}

static void route_netif(u32 e[8], u8 netif)
{
	e[1] |= ((u32)netif & 0x7) << 11;
}

/* On-link prefix: the next hop comes from an ARP block of `count` entries
 * starting at `start`.  ARPStart/ARPEnd hold the block in units of 8, and
 * the hardware indexes it with the host part of the destination address. */
void rtl8198c_l3_route_arp(u32 e[8], __be32 prefix, u8 len, u8 netif,
			   bool internal, u16 start, u16 count)
{
	route_hdr(e, prefix, len, L3_PROC_ARP, internal);
	route_netif(e, netif);
	e[1] |= ((u32)(start / L3_ARP_STRIDE) & 0x3F) << 14;
	e[1] |= ((u32)((start + count - 1) / L3_ARP_STRIDE) & 0x3F) << 20;
}

/* Direct: the next hop is a known L2 entry (`l2` = row << 2 | column). */
void rtl8198c_l3_route_l2(u32 e[8], __be32 prefix, u8 len, u8 netif,
			  bool internal, u16 l2)
{
	route_hdr(e, prefix, len, L3_PROC_L2, internal);
	route_netif(e, netif);
	e[1] |= ((u32)l2 & 0x3FF) << 14;
}

/* Gateway group: nhStart counts in pairs, nhNum encodes 2/4/8/16/32 as
 * 0..4.  This variant has no netif field - the egress comes from each
 * next-hop entry's dstVid - so `count` must be a power of two, 2..32. */
void rtl8198c_l3_route_nexthop(u32 e[8], __be32 prefix, u8 len,
			       bool internal, u8 start, u8 count)
{
	route_hdr(e, prefix, len, L3_PROC_NEXTHOP, internal);
	e[1] |= (u32)(ilog2(count) - 1) << 11;
	e[1] |= ((u32)start / 2 & 0xF) << 14;
	e[1] |= ((u32)start & 0x1F) << 18;
}

/* Fallbacks: punt to the kernel, or drop in hardware.  Both still carry a
 * netif, because the decision stage uses it. */
void rtl8198c_l3_route_cpu(u32 e[8], __be32 prefix, u8 len, u8 netif,
			   bool internal)
{
	route_hdr(e, prefix, len, L3_PROC_CPU, internal);
	route_netif(e, netif);
}

void rtl8198c_l3_route_drop(u32 e[8], __be32 prefix, u8 len, u8 netif,
			    bool internal)
{
	route_hdr(e, prefix, len, L3_PROC_DROP, internal);
	route_netif(e, netif);
}

/* ===== ARP table (type 1) =====
 *
 * One entry per resolved neighbour: valid, the L2 index of its MAC, and a
 * 5-bit aging field.
 */
void rtl8198c_l3_arp_build(u32 e[8], u16 l2, u8 aging)
{
	memset(e, 0, 8 * sizeof(u32));
	e[0] = BIT(0) | (((u32)l2 & 0x3FF) << 1) |
	       (((u32)aging & 0x1F) << 11);
}

/* ===== Next-hop table (type 13) =====
 *
 * A gateway: the egress netif (dstVid), the L2 index of the gateway's MAC,
 * and optionally the PPPoE session and the external-IP index used by NAPT.
 */
void rtl8198c_l3_nexthop_build(u32 e[8], u8 vid, u8 ip_index, u16 l2,
			       bool pppoe)
{
	memset(e, 0, 8 * sizeof(u32));
	e[0] = (pppoe ? BIT(0) : 0) | (((u32)ip_index & 0xF) << 1) |
	       (((u32)vid & 0x7) << 5) | (((u32)l2 & 0x3FF) << 11);
}

/* ===== External/internal IP table (type 5) =====
 *
 * The source address a next-hop entry may rewrite to (NAPT); unused until
 * NAT is implemented.
 */
void rtl8198c_l3_extip_build(u32 e[8], __be32 int_ip, __be32 ext_ip,
			     u8 nexthop, bool one2one, bool local_public)
{
	memset(e, 0, 8 * sizeof(u32));
	e[0] = ip_to_word(int_ip);
	e[1] = ip_to_word(ext_ip);
	e[2] = BIT(0) | (one2one ? BIT(1) : 0) |
	       (local_public ? BIT(2) : 0) | (((u32)nexthop & 0x1F) << 3);
}

/* ===== Table access ===== */

void rtl8198c_l3_table_write(u32 tbl, u32 index, const u32 e[8])
{
	rtl8198c_taci_write_entry(tbl + index * 32, e, TACI_CMD_FORCE);
}

void rtl8198c_l3_table_clear(u32 tbl, u32 count)
{
	const u32 zero[8] = { 0 };
	u32 i;

	for (i = 0; i < count; i++)
		rtl8198c_l3_table_write(tbl, i, zero);
}

/* The route-miss path: index 7 points at the CPU, so a destination that
 * matches no offloaded prefix reaches the kernel instead of being dropped. */
static void rtl8198c_l3_default_route(void)
{
	u32 e[8];

	rtl8198c_l3_route_cpu(e, htonl(INADDR_ANY), 0, 0, false);
	rtl8198c_l3_table_write(TBL_ROUTE, L3_DEFAULT_ROUTE, e);
}

/* ===== Initialisation =====
 *
 * MEMCR covers the L2, VLAN, ARP/multicast and ACL tables, but not the
 * route, next-hop, external-IP or tunnel tables: an entry left behind by a
 * previous boot would go live the moment a netif enables routing, so they
 * are cleared here.  The default route is then pointed at the CPU, which is
 * the route-miss path.
 *
 * TTL-1 belongs to the route action: without it a routed frame keeps its
 * TTL.  Fragments are left to the CPU, since the core has no working L4
 * stage for them.
 */
void rtl8198c_l3_init(void)
{
	static const struct {
		u32	tbl;
		u32	count;
	} tables[] = {
		{ TBL_ROUTE, L3_ROUTES },
		{ TBL_NEXTHOP, L3_NEXTHOPS },
		{ TBL_EXTIP, L3_EXT_IPS },
		{ TBL_PPPOE, L3_PPPOES },
		{ TBL_DSLITE, L3_DSLITE_ENTRIES },
		{ TBL_6RD, L3_6RD_ENTRIES },
		{ TBL_ROUTE6, L3_ROUTES6 },
		{ TBL_NEXTHOP6, L3_NEXTHOPS6 },
		{ TBL_ARP6, L3_ARPS6 },
		{ TBL_IPMCAST6, L3_IPMCASTS6 },
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(tables); i++)
		rtl8198c_l3_table_clear(tables[i].tbl, tables[i].count);

	rtl8198c_l3_default_route();

	sys_w32(ALECR, (sys_r32(ALECR) | ALECR_TTL1) & ~ALECR_FRAG2CPU);
}

/* ===== L3 interfaces (netif table) =====
 *
 * The switch finds the interface entry by VLAN id, so there is exactly one
 * per vid: a second entry on the same vid stops the L3 stage from running at
 * all (measured).  A vid gets an entry at init (the two native VLANs) or when
 * an address appears on a netdev that maps to it.
 *
 * The MAC and MTU come from the netdev that holds the IPv4 address: that is
 * the address neighbours resolve, so the hardware has to use it both to
 * recognise a frame for the router and as the source it writes on routed
 * frames.
 */

static struct rtl8198c_l3_vid *l3_vid_find(struct rtl8198c_eth *eth, u16 vid)
{
	int i;

	for (i = 0; i < L3_VIDS; i++)
		if (eth->l3_vid[i].used && eth->l3_vid[i].vid == vid)
			return &eth->l3_vid[i];
	return NULL;
}

static struct rtl8198c_l3_vid *l3_vid_get(struct rtl8198c_eth *eth, u16 vid)
{
	struct rtl8198c_l3_vid *v = l3_vid_find(eth, vid);
	int i;

	if (v)
		return v;
	for (i = 0; i < L3_VIDS; i++)
		if (!eth->l3_vid[i].used) {
			v = &eth->l3_vid[i];
			memset(v, 0, sizeof(*v));
			v->vid = vid;
			v->used = true;
			return v;
		}
	return NULL;
}

static int l3_vid_index(struct rtl8198c_eth *eth, u16 vid)
{
	struct rtl8198c_l3_vid *v = l3_vid_find(eth, vid);

	return v ? (int)(v - eth->l3_vid) : -1;
}

static u16 l3_vid_mtu(struct rtl8198c_eth *eth, struct rtl8198c_l3_vid *v)
{
	u16 mtu = 0;
	int i;

	if (v->owner)
		return v->owner->mtu;
	for (i = 0; i < ETH_PORTS; i++)
		if ((i ? VLAN_LAN : VLAN_WAN) == v->vid && eth->port_mtu[i] > mtu)
			mtu = eth->port_mtu[i];
	return mtu ? mtu : 1500;
}

/* Rebuild the netif table from the per-vid state: one entry per vid, an
 * empty slot for the rest. */
void rtl8198c_l3_netif_sync(struct rtl8198c_eth *eth)
{
	struct rtl8198c_l3_vid *v;
	int i;

	for (i = 0; i < L3_VIDS; i++) {
		v = &eth->l3_vid[i];
		if (!v->used) {
			rtl8198c_netif_table_write(eth, i, NULL, 0, 0, false);
			continue;
		}
		rtl8198c_netif_table_write(eth, i, v->mac, v->vid,
					   l3_vid_mtu(eth, v), v->route);
	}
}

/* ===== Device model shim =====
 *
 * Everything else in this file works on vids, ports and filtering databases.
 * These two functions are the only knowledge of how a switch port is
 * represented in the kernel: today a port is a plain netdev, a VLAN device
 * over one, or a bridge whose members are those netdevs.  A DSA port would
 * replace the bodies with dsa_port_from_netdev()/dp->index and dp->bridge
 * (and the address scan at the end of rtl8198c_l3_register() would walk
 * ds->ports instead of init_net); nothing else here would change.
 */
struct rtl8198c_l3_ops {
	int (*vid_of_dev)(struct rtl8198c_eth *eth, struct net_device *dev);
	int (*port_of_dev)(struct rtl8198c_eth *eth, struct net_device *dev);
};

/* Netdev -> vid, for ports, VLAN devices over a port and bridges whose
 * first member is one of ours.  Everything else is not ours to route.
 * RTNL context (it walks a bridge's lower devices). */
static int l3_vid_of_dev(struct rtl8198c_eth *eth, struct net_device *dev)
{
	struct net_device *lower;
	struct list_head *iter;
	int i;

	if (!dev)
		return -1;
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i] == dev)
			return ((struct rtl8198c_port *)netdev_priv(dev))->vid;
	if (is_vlan_dev(dev)) {
		struct net_device *real = vlan_dev_real_dev(dev);

		for (i = 0; i < ETH_PORTS; i++)
			if (eth->netdev[i] == real)
				return vlan_dev_vlan_id(dev);
	}
	if (netif_is_bridge_master(dev)) {
		netdev_for_each_lower_dev(dev, lower, iter) {
			for (i = 0; i < ETH_PORTS; i++)
				if (eth->netdev[i] == lower)
					return ((struct rtl8198c_port *)
						netdev_priv(lower))->vid;
		}
	}
	return -1;
}

/* Port behind a netdev: 0..ETH_PORTS-1, -1 when only the L2 entry knows the
 * port (a bridge), -2 when the device is not ours.  Atomic context safe:
 * no lower-device walk. */
static int l3_port_of_dev(struct rtl8198c_eth *eth, struct net_device *dev)
{
	struct net_device *real;
	int i;

	if (!dev)
		return -2;
	real = is_vlan_dev(dev) ? vlan_dev_real_dev(dev) : dev;
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i] == real)
			return i;
	return netif_is_bridge_master(dev) ? -1 : -2;
}

static const struct rtl8198c_l3_ops l3_ops = {
	.vid_of_dev = l3_vid_of_dev,
	.port_of_dev = l3_port_of_dev,
};

/* Egress vid, filtering database and port for a nexthop device.  `port` is
 * -1 when the device is not a single port (a bridge): the port a neighbour
 * sits on is then only known from the L2 entry the switch learned. */
static int l3_egress(struct rtl8198c_eth *eth, struct net_device *dev,
		     int *port, u8 *fid, u16 *vid)
{
	int f, p, v;

	v = l3_ops.vid_of_dev(eth, dev);
	if (v < 0)
		return -1;
	p = l3_ops.port_of_dev(eth, dev);
	if (p == -2)
		return -1;
	for (f = 0; f < L2_FIDS; f++)
		if (eth->fid_vid[f] == v)
			break;
	if (f == L2_FIDS)
		return -1;
	*fid = f;
	*vid = v;
	*port = p;
	return 0;
}

/* Pin the L2 entry for `mac`.  The switch normally learned it already - the
 * neighbour's frames arrived before the neighbour event - which is also the
 * only way to find the port behind a bridge.  On a single port the entry is
 * created when it is missing. */
static int l3_l2_pin(struct rtl8198c_eth *eth, const u8 *mac, u8 fid,
		     int port, u16 *l2)
{
	if (!rtl8198c_l2_fdb_lookup(mac, fid, l2))
		return rtl8198c_l2_fdb_pin(mac, fid);
	if (port < 0)
		return -ENOENT;
	if (rtl8198c_l2_fdb_add(mac, BIT(port), 0, false, fid))
		return -ENOENT;
	return rtl8198c_l2_fdb_lookup(mac, fid, l2);
}

/* A neighbour that may matter to us: a port, a VLAN device over one, or a
 * bridge (whose members the recompute checks). */
static bool l3_neigh_ours(struct rtl8198c_eth *eth, struct net_device *dev)
{
	return l3_ops.port_of_dev(eth, dev) != -2;
}

/* ===== On-link neighbours =====
 *
 * The hardware ARP table is indexed by the host part of the destination, so
 * a connected prefix owns a contiguous block of 2^(32-len) entries and the
 * neighbours inside it are written at block + host.  A block needs at most
 * 512 entries, which limits on-link prefixes to /23 and longer; anything
 * shorter keeps going through the kernel.
 *
 * The netevent notifier runs in atomic context, so the cache is a fixed
 * array behind a spinlock and only the data is copied there.
 */
static void l3_neigh_store(struct rtl8198c_eth *eth, struct neighbour *n)
{
	struct rtl8198c_l3_neigh *e;
	__be32 ip = *(const __be32 *)n->primary_key;
	u32 dev = n->dev ? n->dev->ifindex : 0;
	bool valid = !n->dead && (n->nud_state & NUD_VALID);
	unsigned long flags;
	int i, slot = -1;

	if (!l3_neigh_ours(eth, n->dev))
		return;

	spin_lock_irqsave(&eth->l3.neigh_lock, flags);
	for (i = 0; i < L3_NEIGHS; i++) {
		e = &eth->l3.neigh[i];
		if (e->ip == ip && e->dev == dev) {
			if (valid)
				memcpy(e->mac, n->ha, ETH_ALEN);
			e->valid = valid;
			goto out;
		}
		if (slot < 0 && !e->valid)
			slot = i;
	}
	if (valid && slot >= 0) {
		e = &eth->l3.neigh[slot];
		e->ip = ip;
		e->dev = dev;
		memcpy(e->mac, n->ha, ETH_ALEN);
		e->valid = true;
	}
out:
	spin_unlock_irqrestore(&eth->l3.neigh_lock, flags);
}

static int l3_block_alloc(struct rtl8198c_eth *eth, u8 units)
{
	int i, run = 0, start;

	for (i = 0; i < L3_ARP_UNITS; i++) {
		if (eth->l3.block_used[i]) {
			run = 0;
			continue;
		}
		if (++run != units)
			continue;
		start = i - units + 1;
		for (i = start; i < start + units; i++)
			eth->l3.block_used[i] = 1;
		return start * L3_ARP_STRIDE;
	}
	return -1;
}

static struct rtl8198c_l3_block *l3_block_find(struct rtl8198c_eth *eth,
					       __be32 dst, u32 dev, u8 len)
{
	int i;

	for (i = 0; i < L3_BLOCKS; i++)
		if (eth->l3.block[i].used &&
		    eth->l3.block[i].dst == dst &&
		    eth->l3.block[i].dev == dev &&
		    eth->l3.block[i].len == len)
			return &eth->l3.block[i];
	return NULL;
}

/* Find or create the block for an on-link prefix; -ENOSPC when the table
 * has no run left. */
static struct rtl8198c_l3_block *l3_block_get(struct rtl8198c_eth *eth,
					      __be32 dst, u32 dev, u8 len)
{
	struct rtl8198c_l3_block *b = l3_block_find(eth, dst, dev, len);
	u32 size = 1U << (32 - len);
	int start, i;

	if (b)
		return b;
	for (i = 0; i < L3_BLOCKS; i++)
		if (!eth->l3.block[i].used)
			break;
	if (i == L3_BLOCKS)
		return NULL;
	start = l3_block_alloc(eth, size < L3_ARP_STRIDE ? 1 :
			       size / L3_ARP_STRIDE);
	if (start < 0)
		return NULL;
	b = &eth->l3.block[i];
	b->dst = dst;
	b->dev = dev;
	b->len = len;
	b->start = start;
	b->units = size < L3_ARP_STRIDE ? 1 : size / L3_ARP_STRIDE;
	b->used = true;
	return b;
}

static bool l3_pin(struct rtl8198c_eth *eth, const u8 *mac, u8 fid,
		   bool *keep);

/* Write the ARP entries of one block from the neighbour cache and clear the
 * ones whose neighbour went away. */
static void l3_block_sync(struct rtl8198c_eth *eth, struct rtl8198c_l3_block *b,
			  u8 fid, int port, bool *keep)
{
	u32 hostmask = (1U << (32 - b->len)) - 1;
	struct rtl8198c_l3_neigh n;
	u32 ip_word, host, end = b->start + (hostmask + 1);
	u32 e[8];
	u16 l2;
	int i, idx;

	for (idx = b->start; idx < end; idx++)
		if (eth->l3.arp_l2[idx] != 0xffff)
			eth->l3.arp_l2[idx] = 0xfffe;	/* stale */

	for (i = 0; i < L3_NEIGHS; i++) {
		unsigned long flags;

		/* l3_l2_pin takes the L2 mutex, so the cache lock is only
		 * held for the copy. */
		spin_lock_irqsave(&eth->l3.neigh_lock, flags);
		n = eth->l3.neigh[i];
		spin_unlock_irqrestore(&eth->l3.neigh_lock, flags);

		if (!n.valid || n.dev != b->dev)
			continue;
		ip_word = ntohl((__force u32)n.ip);
		if ((ip_word & ~hostmask) !=
		    (ntohl((__force u32)b->dst) & ~hostmask))
			continue;
		host = ip_word & hostmask;
		if (b->start + host >= end)
			continue;
		if (l3_l2_pin(eth, n.mac, fid, port, &l2) ||
		    !l3_pin(eth, n.mac, fid, keep))
			continue;
		rtl8198c_l3_arp_build(e, l2, 12);
		rtl8198c_l3_table_write(TBL_ARP, b->start + host, e);
		eth->l3.arp_l2[b->start + host] = l2;
	}

	for (idx = b->start; idx < end; idx++) {
		if (eth->l3.arp_l2[idx] != 0xfffe)
			continue;
		memset(e, 0, sizeof(e));
		rtl8198c_l3_table_write(TBL_ARP, idx, e);
		eth->l3.arp_l2[idx] = 0xffff;
	}
}

static void l3_block_free(struct rtl8198c_eth *eth, struct rtl8198c_l3_block *b)
{
	u32 e[8] = { 0 };
	int i, end = b->units * L3_ARP_STRIDE;

	for (i = 0; i < end; i++)
		if (eth->l3.arp_l2[b->start + i] != 0xffff) {
			rtl8198c_l3_table_write(TBL_ARP, b->start + i, e);
			eth->l3.arp_l2[b->start + i] = 0xffff;
		}
	for (i = 0; i < b->units; i++)
		eth->l3.block_used[b->start / L3_ARP_STRIDE + i] = 0;
	b->used = false;
}

/* One L3 interface per vid: the first netdev that gets an address keeps it,
 * except that a bridge may take the vid over from a port.  A second address
 * on the same vid is left to the kernel - the hardware has one router MAC
 * per vid, so it can only route for one of them. */
static void l3_vid_owner(struct rtl8198c_eth *eth, u16 vid,
			 struct net_device *dev, bool up)
{
	struct rtl8198c_l3_vid *v = l3_vid_get(eth, vid);
	int i;

	if (!v)
		return;
	if (up) {
		if (v->owner && v->owner != dev &&
		    !(netif_is_bridge_master(dev) &&
		      !netif_is_bridge_master(v->owner)))
			return;
		v->owner = dev;
		memcpy(v->mac, dev->dev_addr, ETH_ALEN);
	} else {
		if (v->owner != dev)
			return;
		v->owner = NULL;
		/* Back to the lowest port of the vid, L2 only. */
		for (i = 0; i < ETH_PORTS; i++)
			if ((i ? VLAN_LAN : VLAN_WAN) == vid) {
				memcpy(v->mac, eth->port_mac[i], ETH_ALEN);
				break;
			}
	}
	v->route = v->owner && eth->l3.enabled;
}

/* The periodic run re-reads the tables: it heals an ARP entry the hardware
 * aged out and any event that was missed while the switch was busy. */
#define L3_REFRESH	(60 * HZ)

static void l3_schedule(struct rtl8198c_eth *eth)
{
	if (eth->l3.registered)
		mod_delayed_work(system_wq, &eth->l3.work, 0);
}

/* The netdev that holds the IPv4 address is the L3 interface for its vid:
 * its MAC is what neighbours resolve, so it is also the address the switch
 * must recognise and use as the source of routed frames. */
static int l3_addr_event(struct notifier_block *nb, unsigned long event,
			 void *ptr)
{
	struct rtl8198c_eth *eth = container_of(nb, struct rtl8198c_eth,
						l3.addr_nb);
	struct in_ifaddr *ifa = ptr;
	struct net_device *dev = ifa->ifa_dev->dev;
	int vid;

	if (event != NETDEV_UP && event != NETDEV_DOWN)
		return NOTIFY_DONE;
	vid = l3_ops.vid_of_dev(eth, dev);
	if (vid < 0)
		return NOTIFY_DONE;
	if (event == NETDEV_DOWN && dev->ip_ptr && dev->ip_ptr->ifa_list)
		return NOTIFY_DONE;	/* another address remains */

	mutex_lock(&eth->l3.lock);
	l3_vid_owner(eth, vid, dev, event == NETDEV_UP);
	rtl8198c_l3_netif_sync(eth);
	mutex_unlock(&eth->l3.lock);
	l3_schedule(eth);
	return NOTIFY_DONE;
}

/* A MAC or MTU change on the interface that owns a vid. */
void rtl8198c_l3_dev_update(struct rtl8198c_eth *eth, struct net_device *dev)
{
	struct rtl8198c_l3_vid *v;
	int vid = l3_ops.vid_of_dev(eth, dev);

	if (vid < 0)
		return;
	mutex_lock(&eth->l3.lock);
	v = l3_vid_find(eth, vid);
	if (v && v->owner == dev)
		memcpy(v->mac, dev->dev_addr, ETH_ALEN);
	rtl8198c_l3_netif_sync(eth);
	mutex_unlock(&eth->l3.lock);
	l3_schedule(eth);
}

/* One netif entry for each native VLAN; address events set the owner. */
void rtl8198c_l3_netif_init(struct rtl8198c_eth *eth)
{
	struct rtl8198c_l3_vid *v;

	memset(eth->l3_vid, 0, sizeof(eth->l3_vid));
	v = l3_vid_get(eth, VLAN_WAN);
	if (v)
		memcpy(v->mac, eth->port_mac[0], ETH_ALEN);
	v = l3_vid_get(eth, VLAN_LAN);
	if (v)
		memcpy(v->mac, eth->port_mac[1], ETH_ALEN);
	rtl8198c_l3_netif_sync(eth);
}

/* ===== FIB mirror =====
 *
 * Every change schedules a rebuild from a full FIB dump: the table is small,
 * and a resync cannot drift the way incremental updates can.  The mirror is
 * only used to decide what the hardware can be trusted with.
 */

static bool l3_nh_same(const struct rtl8198c_l3_fib *a,
		       const struct rtl8198c_l3_fib *b)
{
	return a->dev == b->dev && a->gw == b->gw && a->type == b->type;
}

static bool l3_covers(const struct rtl8198c_l3_fib *inner,
		      const struct rtl8198c_l3_fib *outer)
{
	u32 mask;

	if (inner->len <= outer->len || inner->len > 32)
		return false;
	mask = outer->len ? htonl(0xFFFFFFFFU << (32 - outer->len)) : 0;
	return (inner->dst & mask) == (outer->dst & mask);
}

/* A prefix may only be offloaded when every more specific route with a
 * different next hop is offloaded too, or the hardware would use this entry
 * for traffic the kernel sends elsewhere. */
/* Only entries that can carry traffic out of a port can be shadowed by a
 * less specific hardware entry.  Local, broadcast and anycast addresses are
 * not forwarded at all - the covering on-link route catches them with an ARP
 * miss and sends them to the CPU - so they do not block. */
static bool l3_blocks(const struct rtl8198c_l3_fib *m)
{
	switch (m->type) {
	case RTN_BLACKHOLE:
	case RTN_UNREACHABLE:
	case RTN_PROHIBIT:
	case RTN_THROW:
		return true;
	case RTN_UNICAST:
		return true;
	default:
		return false;
	}
}

static bool l3_blocked(struct rtl8198c_eth *eth,
		       const struct rtl8198c_l3_fib *f)
{
	int i;

	for (i = 0; i < eth->l3.fib_count; i++) {
		const struct rtl8198c_l3_fib *m = &eth->l3.fib[i];

		if (!l3_covers(m, f) || l3_nh_same(m, f) || !l3_blocks(m))
			continue;
		if (m->slot == 0xff)
			return true;
	}
	return false;
}

static void l3_fib_store(struct rtl8198c_eth *eth,
			 const struct fib_entry_notifier_info *info)
{
	struct rtl8198c_l3_fib *f;
	struct fib_nh *nh = NULL;

	if (eth->l3.fib_count >= L3_FIB_MAX) {
		eth->l3.overflow = true;
		return;
	}
	/* A route through a nexthop object has no fib_nh array to read. */
	if (!info->fi->nh)
		nh = fib_info_nh(info->fi, 0);

	f = &eth->l3.fib[eth->l3.fib_count++];
	memset(f, 0, sizeof(*f));
	f->dst = info->dst;
	f->len = info->dst_len;
	f->type = info->type;
	f->tb_id = info->tb_id;
	f->dscp = info->dscp;
	f->nh_obj = info->fi->nh != NULL;
	f->paths = fib_info_num_path(info->fi);
	if (nh) {
		f->lwt = nh->fib_nh_lws != NULL;
		f->dev = nh->fib_nh_dev ? nh->fib_nh_dev->ifindex : 0;
		f->gw = nh->fib_nh_gw4;
	}
	f->slot = 0xff;
}

static int l3_fib_event(struct notifier_block *nb, unsigned long event,
			void *ptr)
{
	struct rtl8198c_eth *eth = container_of(nb, struct rtl8198c_eth,
						l3.fib_nb);
	struct fib_notifier_info *info = ptr;

	if (info->family != AF_INET)
		return NOTIFY_DONE;
	if (eth->l3.resyncing) {
		if (event == FIB_EVENT_ENTRY_REPLACE ||
		    event == FIB_EVENT_ENTRY_ADD)
			l3_fib_store(eth, (const struct fib_entry_notifier_info *)
				     info);
		return NOTIFY_DONE;
	}
	if (!eth->l3.enabled)
		return NOTIFY_DONE;

	switch (event) {
	case FIB_EVENT_ENTRY_ADD:
	case FIB_EVENT_ENTRY_REPLACE:
	case FIB_EVENT_ENTRY_APPEND:
	case FIB_EVENT_ENTRY_DEL:
		l3_schedule(eth);
		break;
	}
	return NOTIFY_DONE;
}

/* A resolved neighbour can make a gateway route installable, a dead one has
 * to take it out again. */
static int l3_neigh_event(struct notifier_block *nb, unsigned long event,
			  void *ptr)
{
	struct rtl8198c_eth *eth = container_of(nb, struct rtl8198c_eth,
						l3.neigh_nb);
	struct neighbour *n = ptr;

	if (event != NETEVENT_NEIGH_UPDATE || n->tbl != &arp_tbl)
		return NOTIFY_DONE;
	l3_neigh_store(eth, n);
	if (eth->l3.enabled)
		l3_schedule(eth);
	return NOTIFY_DONE;
}

/* ===== Programming ===== */

/* Remember which L2 entries the hardware depends on, so they can be
 * released when the route or neighbour that needed them goes away. */
static bool l3_pin(struct rtl8198c_eth *eth, const u8 *mac, u8 fid,
		   bool *keep)
{
	int i, free = -1;

	for (i = 0; i < L3_PINS; i++) {
		if (!eth->l3.pin[i].used) {
			if (free < 0)
				free = i;
			continue;
		}
		if (eth->l3.pin[i].fid == fid &&
		    ether_addr_equal(eth->l3.pin[i].mac, mac)) {
			keep[i] = true;
			return true;
		}
	}
	if (free < 0)
		return false;
	eth->l3.pin[free].fid = fid;
	memcpy(eth->l3.pin[free].mac, mac, ETH_ALEN);
	eth->l3.pin[free].used = true;
	keep[free] = true;
	return true;
}

static void l3_unpin(struct rtl8198c_eth *eth, const bool *keep)
{
	int i;

	for (i = 0; i < L3_PINS; i++)
		if (eth->l3.pin[i].used && !keep[i]) {
			rtl8198c_l2_fdb_del(eth->l3.pin[i].mac,
					    eth->l3.pin[i].fid);
			eth->l3.pin[i].used = false;
		}
}

struct l3_cand {
	int	fib;
	u16	vid;
	int	port;
	u8	fid;
	u16	l2;
	bool	cpu;
	struct rtl8198c_l3_block *block;
};

static void l3_recompute(struct rtl8198c_eth *eth)
{
	struct l3_cand tab[L3_ROUTES];
	bool keep[L3_PINS] = {};
	bool have[L3_ROUTES] = {};
	bool bused[L3_BLOCKS] = {};
	struct rtl8198c_l3_fib *f;
	struct rtl8198c_l3_block *b;
	struct net_device *dev;
	struct neighbour *n;
	u32 e[8];
	int i, len, s, next = 0, vid, port;

	for (i = 0; i < eth->l3.fib_count; i++)
		eth->l3.fib[i].slot = 0xff;

	if (!eth->l3.overflow) {
		for (len = 32; len >= 0; len--) {
			for (i = 0; i < eth->l3.fib_count; i++) {
				f = &eth->l3.fib[i];
				if (f->len != len || f->slot != 0xff)
					continue;
				if (f->nh_obj || f->lwt || f->paths > 1 ||
				    f->dscp || l3_blocked(eth, f))
					continue;
				if (len == 0)
					s = L3_DEFAULT_ROUTE;
				else if (next < L3_DEFAULT_ROUTE)
					s = next;
				else
					continue;

				memset(&tab[s], 0, sizeof(tab[s]));
				tab[s].port = -1;
				tab[s].l2 = 0xffff;

				if (f->tb_id == RT_TABLE_LOCAL) {
					if (f->type != RTN_LOCAL || !f->dev)
						continue;
					dev = dev_get_by_index(&init_net, f->dev);
					if (!dev)
						continue;
					vid = l3_ops.vid_of_dev(eth, dev);
					dev_put(dev);
					if (vid < 0 ||
					    l3_vid_index(eth, vid) < 0)
						continue;
					tab[s].vid = vid;
					tab[s].cpu = true;
				} else if (f->tb_id == RT_TABLE_MAIN) {
					if (f->type != RTN_UNICAST || !f->dev)
						continue;
					dev = dev_get_by_index(&init_net, f->dev);
					if (!dev)
						continue;
					if (l3_egress(eth, dev, &port, &tab[s].fid,
						      &tab[s].vid) < 0 ||
					    l3_vid_index(eth, tab[s].vid) < 0) {
						dev_put(dev);
						continue;
					}
					tab[s].port = port;

					if (!f->gw) {
						/* On-link: the prefix owns a
						 * contiguous ARP block. */
						if (len < 23 ||
						    (1U << (32 - len)) > L3_ARPS) {
							dev_put(dev);
							continue;
						}
						b = l3_block_get(eth, f->dst,
								 f->dev, len);
						dev_put(dev);
						if (!b)
							continue;
						tab[s].block = b;
						bused[b - eth->l3.block] = true;
					} else {
						n = neigh_lookup(&arp_tbl,
								 &f->gw, dev);
						dev_put(dev);
						if (!n)
							continue;
						if (n->dead ||
						    !(n->nud_state & NUD_VALID)) {
							neigh_release(n);
							continue;
						}
						if (l3_l2_pin(eth, n->ha,
							      tab[s].fid, port,
							      &tab[s].l2) ||
						    !l3_pin(eth, n->ha,
							    tab[s].fid, keep)) {
							neigh_release(n);
							continue;
						}
						neigh_release(n);
					}
				} else {
					continue;
				}

				tab[s].fib = i;
				have[s] = true;
				f->slot = s;
				if (len)
					next++;
			}
		}
	}

	l3_unpin(eth, keep);

	for (i = 0; i < L3_BLOCKS; i++)
		if (eth->l3.block[i].used && !bused[i])
			l3_block_free(eth, &eth->l3.block[i]);

	for (s = 0; s < L3_ROUTES; s++) {
		if (!have[s]) {
			if (s == L3_DEFAULT_ROUTE)
				rtl8198c_l3_default_route();
			else {
				memset(e, 0, sizeof(e));
				rtl8198c_l3_table_write(TBL_ROUTE, s, e);
			}
			continue;
		}
		f = &eth->l3.fib[tab[s].fib];
		vid = l3_vid_index(eth, tab[s].vid);
		if (tab[s].cpu) {
			rtl8198c_l3_route_cpu(e, f->dst, f->len, vid,
					      tab[s].vid != VLAN_WAN);
		} else if (tab[s].block) {
			l3_block_sync(eth, tab[s].block, tab[s].fid,
				      tab[s].port, keep);
			rtl8198c_l3_route_arp(e, f->dst, f->len, vid,
					      tab[s].vid != VLAN_WAN,
					      tab[s].block->start,
					      1U << (32 - f->len));
		} else {
			u8 nh = s * 2;

			rtl8198c_l3_nexthop_build(e, vid, 0, tab[s].l2, false);
			rtl8198c_l3_table_write(TBL_NEXTHOP, nh, e);
			rtl8198c_l3_table_write(TBL_NEXTHOP, nh + 1, e);
			rtl8198c_l3_route_nexthop(e, f->dst, f->len,
						  tab[s].vid != VLAN_WAN,
						  nh, 2);
		}
		rtl8198c_l3_table_write(TBL_ROUTE, s, e);
	}

	for (s = 0; s < L3_ROUTES; s++)
		if (!have[s] || tab[s].cpu || tab[s].block) {
			memset(e, 0, sizeof(e));
			rtl8198c_l3_table_write(TBL_NEXTHOP, s * 2, e);
			rtl8198c_l3_table_write(TBL_NEXTHOP, s * 2 + 1, e);
		}
}

static void l3_clear(struct rtl8198c_eth *eth)
{
	bool keep[L3_PINS] = {};
	u32 e[8] = { 0 };
	int i;

	l3_unpin(eth, keep);
	for (i = 0; i < L3_BLOCKS; i++)
		if (eth->l3.block[i].used)
			l3_block_free(eth, &eth->l3.block[i]);
	for (i = 0; i < L3_ROUTES; i++)
		rtl8198c_l3_table_write(TBL_ROUTE, i, e);
	rtl8198c_l3_default_route();
	for (i = 0; i < L3_NEXTHOPS; i++)
		rtl8198c_l3_table_write(TBL_NEXTHOP, i, e);
}

static void l3_work_fn(struct work_struct *work)
{
	struct rtl8198c_eth *eth = container_of(work, struct rtl8198c_eth,
						l3.work.work);
	int i;

	rtnl_lock();
	mutex_lock(&eth->l3.lock);
	if (eth->l3.enabled) {
		/* A bridge can take a new MAC when its port set changes; the
		 * netif entry has to follow before routes are rebuilt. */
		for (i = 0; i < L3_VIDS; i++)
			if (eth->l3_vid[i].owner)
				memcpy(eth->l3_vid[i].mac,
				       eth->l3_vid[i].owner->dev_addr, ETH_ALEN);
		rtl8198c_l3_netif_sync(eth);
		eth->l3.resyncing = true;
		eth->l3.fib_count = 0;
		eth->l3.overflow = false;
		fib_notify(&init_net, &eth->l3.fib_nb, NULL);
		eth->l3.resyncing = false;
		l3_recompute(eth);
	} else {
		l3_clear(eth);
	}
	mutex_unlock(&eth->l3.lock);
	rtnl_unlock();

	if (eth->l3.enabled && eth->l3.registered)
		schedule_delayed_work(&eth->l3.work, L3_REFRESH);
}

static void l3_enable(struct rtl8198c_eth *eth, bool on)
{
	int i;

	/* The NAPT engine only sees frames the switch routes.  Turning the
	 * routing off must therefore disarm it too: a live session would
	 * otherwise sit in front of a path that no longer reaches it. */
	if (!on && eth->l4.enabled)
		rtl8198c_l4_engine(eth, false);

	mutex_lock(&eth->l3.lock);
	eth->l3.enabled = on;
	for (i = 0; i < L3_VIDS; i++)
		eth->l3_vid[i].route = on && eth->l3_vid[i].owner;
	rtl8198c_l3_netif_sync(eth);
	mutex_unlock(&eth->l3.lock);
	l3_schedule(eth);
}

int rtl8198c_l3_register(struct rtl8198c_eth *eth)
{
	struct net_device *dev;
	int err, i;

	mutex_init(&eth->l3.lock);
	spin_lock_init(&eth->l3.neigh_lock);
	INIT_DELAYED_WORK(&eth->l3.work, l3_work_fn);
	for (i = 0; i < L3_ARPS; i++)
		eth->l3.arp_l2[i] = 0xffff;
	eth->l3.fib_nb.notifier_call = l3_fib_event;
	eth->l3.neigh_nb.notifier_call = l3_neigh_event;
	eth->l3.addr_nb.notifier_call = l3_addr_event;

	eth->l3.resyncing = true;
	err = register_fib_notifier(&init_net, &eth->l3.fib_nb, NULL, NULL);
	eth->l3.resyncing = false;
	if (err)
		return err;
	register_netevent_notifier(&eth->l3.neigh_nb);
	register_inetaddr_notifier(&eth->l3.addr_nb);
	eth->l3.registered = true;

	/* Addresses configured before the driver probed.  The vid lookup
	 * filters the walk, so this stays independent of the device model. */
	rtnl_lock();
	mutex_lock(&eth->l3.lock);
	for_each_netdev(&init_net, dev) {
		struct in_device *in_dev = dev->ip_ptr;
		int vid;

		if (!in_dev || !in_dev->ifa_list)
			continue;
		vid = l3_ops.vid_of_dev(eth, dev);
		if (vid >= 0)
			l3_vid_owner(eth, vid, dev, true);
	}
	rtl8198c_l3_netif_sync(eth);
	mutex_unlock(&eth->l3.lock);
	rtnl_unlock();
	return 0;
}

void rtl8198c_l3_unregister(struct rtl8198c_eth *eth)
{
	if (!eth->l3.registered)
		return;
	eth->l3.registered = false;
	unregister_inetaddr_notifier(&eth->l3.addr_nb);
	unregister_netevent_notifier(&eth->l3.neigh_nb);
	unregister_fib_notifier(&init_net, &eth->l3.fib_nb);
	cancel_delayed_work_sync(&eth->l3.work);

	l3_enable(eth, false);
	rtnl_lock();
	mutex_lock(&eth->l3.lock);
	l3_clear(eth);
	mutex_unlock(&eth->l3.lock);
	rtnl_unlock();
}

/* ===== Debugfs: table dump and raw entry access =====
 *
 *   cat l3
 *   echo "read 2 7" > taci ; cat taci
 *   echo "write 2 0 0xC0A80200 0xE1 0 0 0 0 0 0" > taci
 *
 * `l3` shows what is programmed; `taci` is the bring-up tool for the
 * experiment before any of it is driven by the FIB.  Type numbers are the
 * TACI table numbers (1 ARP, 2 route, 3 IP multicast, 4 netif, 5 ext-IP,
 * 9 NAPT sessions, 11 PPPoE, 13 next-hop, 18-21 IPv6, plus 0 L2, 6 VLAN,
 * 8/10/15 the absent server-port/ICMP/ALG tables, 12 ACL).
 */

static const char *l3_table_name(u32 type)
{
	switch (type) {
	case 0: return "l2";
	case 1: return "arp";
	case 2: return "route";
	case 3: return "ipmcast";
	case 4: return "netif";
	case 5: return "extip";
	case 6: return "vlan";
	case 8: return "serverport";
	case 9: return "tcpudp";
	case 10: return "icmp";
	case 11: return "pppoe";
	case 12: return "acl";
	case 13: return "nexthop";
	case 15: return "alg";
	case 16: return "dslite";
	case 17: return "6rd";
	case 18: return "route6";
	case 19: return "nexthop6";
	case 20: return "arp6";
	case 21: return "ipmcast6";
	default: return "?";
	}
}

static u32 l3_table_base(u32 type)
{
	switch (type) {
	case 0: return TBL_L2;
	case 1: return TBL_ARP;
	case 2: return TBL_ROUTE;
	case 3: return TBL_IPMCAST;
	case 4: return TBL_NETIF;
	case 5: return TBL_EXTIP;
	case 6: return TBL_VLAN;
	case 8: return TBL_SERVER_PORT;
	case 9: return TBL_TCP_UDP;
	case 10: return TBL_ICMP;
	case 11: return TBL_PPPOE;
	case 12: return TBL_ACL;
	case 13: return TBL_NEXTHOP;
	case 15: return TBL_ALG;
	case 16: return TBL_DSLITE;
	case 17: return TBL_6RD;
	case 18: return TBL_ROUTE6;
	case 19: return TBL_NEXTHOP6;
	case 20: return TBL_ARP6;
	case 21: return TBL_IPMCAST6;
	default: return 0;
	}
}

static const char *l3_process_name(u32 process)
{
	switch (process) {
	case L3_PROC_PPPOE: return "pppoe";
	case L3_PROC_L2: return "l2";
	case L3_PROC_ARP: return "arp";
	case L3_PROC_CPU: return "cpu";
	case L3_PROC_NEXTHOP: return "nexthop";
	case L3_PROC_DROP: return "drop";
	default: return "?";
	}
}

static void l3_show_route(struct seq_file *s, const u32 e[8])
{
	__be32 prefix = word_to_ip(e[0]);
	u32 w1 = e[1];
	u32 len = w1 & 0x1F;
	u32 process = (w1 >> 6) & 0x7;

	if (!(w1 & BIT(5))) {
		seq_puts(s, "empty\n");
		return;
	}

	seq_printf(s, "%pI4/%u ", &prefix, len ? len + 1 : 0);
	seq_printf(s, "%s", l3_process_name(process));
	if (process != L3_PROC_NEXTHOP)
		seq_printf(s, " netif %u", (w1 >> 11) & 0x7);
	if (w1 & BIT(9))
		seq_puts(s, " internal");

	switch (process) {
	case L3_PROC_ARP:
		seq_printf(s, " block %u..%u", ((w1 >> 14) & 0x3F) * 8,
			   (((w1 >> 20) & 0x3F) + 1) * 8 - 1);
		break;
	case L3_PROC_L2:
	case L3_PROC_PPPOE:
		seq_printf(s, " l2 %u:%u", ((w1 >> 14) & 0x3FF) >> 2,
			   ((w1 >> 14) & 0x3FF) & 0x3);
		break;
	case L3_PROC_NEXTHOP:
		seq_printf(s, " nh %u+%u", ((w1 >> 14) & 0xF) * 2,
			   2 << ((w1 >> 11) & 0x7));
		break;
	}
	seq_putc(s, '\n');
}

static void l3_show_arp(struct seq_file *s, u32 index, const u32 e[8])
{
	u32 w0 = e[0];

	seq_printf(s, "  [%u] l2 %u:%u age %u\n", index,
		   ((w0 >> 1) & 0x3FF) >> 2, ((w0 >> 1) & 0x3FF) & 0x3,
		   (w0 >> 11) & 0x1F);
}

static void l3_show_nexthop(struct seq_file *s, u32 index, const u32 e[8])
{
	u32 w0 = e[0];

	seq_printf(s, "  [%u] vid %u l2 %u:%u%s", index, (w0 >> 5) & 0x7,
		   ((w0 >> 11) & 0x3FF) >> 2, ((w0 >> 11) & 0x3FF) & 0x3,
		   (w0 & BIT(0)) ? " pppoe" : "");
	if ((w0 >> 1) & 0xF)
		seq_printf(s, " extip %u", (w0 >> 1) & 0xF);
	seq_putc(s, '\n');
}

static void l3_show_extip(struct seq_file *s, u32 index, const u32 e[8])
{
	__be32 int_ip = word_to_ip(e[0]);
	__be32 ext_ip = word_to_ip(e[1]);

	seq_printf(s, "  [%u] %pI4 -> %pI4%s%s nh %u\n", index,
		   &int_ip, &ext_ip, (e[2] & BIT(1)) ? " one2one" : "",
		   (e[2] & BIT(2)) ? " localpublic" : "", (e[2] >> 3) & 0x1F);
}

static void l3_show_netif(struct seq_file *s, u32 index, const u32 e[8])
{
	u64 m = ((u64)(e[1] & 0x1FFFFFFF) << 19) | ((e[0] >> 13) & 0x7FFFF);
	u8 mac[ETH_ALEN];
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		mac[i] = (m >> (8 * (ETH_ALEN - 1 - i))) & 0xFF;

	seq_printf(s, "  [%u] vid %u %pM mtu %u route %s%s\n", index,
		   (e[0] >> 1) & 0xFFF, mac, (e[3] >> 2) & 0x7FFF,
		   (e[1] & NETIF_EN_HWROUTE) ? "on" : "off",
		   (e[1] & NETIF_EN_HWROUTE_V6) ? " (v6)" : "");
}

static void l3_dump_table(struct seq_file *s, const char *title, u32 tbl,
			  u32 count, bool show_empty,
			  void (*show)(struct seq_file *, u32, const u32 [8]))
{
	u32 e[8], i, used = 0;

	seq_printf(s, "\n%s (%u)\n", title, count);
	for (i = 0; i < count; i++) {
		rtl8198c_taci_read_entry(tbl + i * 32, e);
		if (!e[0] && !e[1] && !e[2] && !e[3]) {
			if (show_empty) {
				seq_printf(s, "  [%u] empty\n", i);
			}
			continue;
		}
		used++;
		show(s, i, e);
	}
	seq_printf(s, "  %u used\n", used);
}

static void l3_show_route_line(struct seq_file *s, u32 index, const u32 e[8])
{
	seq_printf(s, "  [%u] ", index);
	l3_show_route(s, e);
}

static int l3_show(struct seq_file *s, void *unused)
{
	seq_puts(s, "TACI types: 1=arp 2=route 3=ipmcast 4=netif 5=extip 6=vlan\n"
		    "            8=serverport 9=tcpudp 10=icmp 11=pppoe 12=acl\n"
		    "            13=nexthop 15=alg 16=dslite 17=6rd 18=route6\n"
		    "            19=nexthop6 20=arp6 21=ipmcast6\n");

	l3_dump_table(s, "route (index 7 = default)", TBL_ROUTE, L3_ROUTES,
		      true, l3_show_route_line);
	l3_dump_table(s, "arp", TBL_ARP, L3_ARPS, false, l3_show_arp);
	l3_dump_table(s, "nexthop", TBL_NEXTHOP, L3_NEXTHOPS, false,
		      l3_show_nexthop);
	l3_dump_table(s, "ext-ip", TBL_EXTIP, L3_EXT_IPS, false, l3_show_extip);
	l3_dump_table(s, "netif", TBL_NETIF, 8, false, l3_show_netif);

	{
		struct rtl8198c_eth *eth = s->private;
		struct rtl8198c_l3_fib *f;
		int i;

		mutex_lock(&eth->l3.lock);
		{
			unsigned long flags;
			int nblocks = 0, nneigh = 0;

			for (i = 0; i < L3_BLOCKS; i++)
				nblocks += eth->l3.block[i].used;
			spin_lock_irqsave(&eth->l3.neigh_lock, flags);
			for (i = 0; i < L3_NEIGHS; i++)
				nneigh += eth->l3.neigh[i].valid;
			spin_unlock_irqrestore(&eth->l3.neigh_lock, flags);
			seq_printf(s, "\narp blocks: %d, neighbours: %d\n",
				   nblocks, nneigh);
		}
		seq_printf(s, "fib mirror: %d routes%s, offload %s\n",
			   eth->l3.fib_count,
			   eth->l3.overflow ? " (overflow, nothing offloaded)" : "",
			   eth->l3.enabled ? "on" : "off");
		for (i = 0; i < eth->l3.fib_count; i++) {
			f = &eth->l3.fib[i];
			seq_printf(s, "  %pI4/%u tb %u type %u dev %u",
				   &f->dst, f->len, f->tb_id, f->type, f->dev);
			if (f->gw)
				seq_printf(s, " gw %pI4", &f->gw);
			if (f->slot != 0xff)
				seq_printf(s, " -> slot %u", f->slot);
			seq_putc(s, '\n');
		}
		mutex_unlock(&eth->l3.lock);
	}
	return 0;
}

static int l3_open(struct inode *inode, struct file *file)
{
	return single_open(file, l3_show, inode->i_private);
}

static const struct file_operations l3_fops = {
	.owner = THIS_MODULE,
	.open = l3_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* The last raw entry touched through `taci`, so the sequence is
 * "echo read ... > taci" followed by "cat taci". */
static struct {
	u32	type;
	u32	index;
	u32	words[8];
	bool	valid;
} taci_last;

static void l3_show_entry(struct seq_file *s, u32 type, u32 index,
			  const u32 e[8])
{
	seq_printf(s, "type %u (%s) index %u\n  %08x %08x %08x %08x\n"
		   "  %08x %08x %08x %08x\n",
		   type, l3_table_name(type), index,
		   e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);

	if (type == 2)
		l3_show_route_line(s, index, e);
	else if (type == 1)
		l3_show_arp(s, index, e);
	else if (type == 13)
		l3_show_nexthop(s, index, e);
	else if (type == 5)
		l3_show_extip(s, index, e);
	else if (type == 4)
		l3_show_netif(s, index, e);
}

static int taci_show(struct seq_file *s, void *unused)
{
	if (!taci_last.valid) {
		seq_puts(s, "no entry selected\n");
		return 0;
	}
	l3_show_entry(s, taci_last.type, taci_last.index, taci_last.words);
	return 0;
}

static ssize_t taci_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	char buf[192];
	char *args[12], *p;
	unsigned int type = 0, index = 0, words[8];
	u32 base;
	ssize_t len;
	int i, n = 0;

	len = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, ubuf, count);
	if (len <= 0)
		return len;
	buf[len] = '\0';
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	p = buf;
	while (n < ARRAY_SIZE(args) && (args[n] = strsep(&p, " ")))
		if (*args[n])
			n++;
	if (n < 3)
		return -EINVAL;
	if (kstrtouint(args[1], 0, &type) || kstrtouint(args[2], 0, &index))
		return -EINVAL;
	base = l3_table_base(type);
	if (!base || index >= 2048)
		return -EINVAL;

	if (!strcmp(args[0], "read")) {
		if (n != 3)
			return -EINVAL;
		taci_last.type = type;
		taci_last.index = index;
		rtl8198c_taci_read_entry(base + index * 32, taci_last.words);
		taci_last.valid = true;
		return len;
	}

	if (strcmp(args[0], "write") || n != 11)
		return -EINVAL;
	for (i = 0; i < 8; i++)
		if (kstrtouint(args[3 + i], 0, &words[i]))
			return -EINVAL;
	for (i = 0; i < 8; i++)
		taci_last.words[i] = words[i];
	rtl8198c_taci_write_entry(base + index * 32, taci_last.words,
				  TACI_CMD_FORCE);
	taci_last.type = type;
	taci_last.index = index;
	taci_last.valid = true;
	return len;
}

static int taci_open(struct inode *inode, struct file *file)
{
	return single_open(file, taci_show, inode->i_private);
}

static const struct file_operations taci_fops = {
	.owner = THIS_MODULE,
	.open = taci_open,
	.read = seq_read,
	.write = taci_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* debugfs "route_offload": the switch that lets the ASIC route.
 *
 * Off by default.  A routed frame never reaches the CPU, so it is not
 * filtered, not tracked and not NAT'd - the box behaves like a plain L3
 * router for the routes it offloads.  Routes are only taken when the whole
 * more specific subtree is taken too, so a partially offloaded default
 * route cannot shadow a prefix that the kernel sends elsewhere.
 */
static int route_offload_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;

	seq_printf(s, "%d\n", eth->l3.enabled ? 1 : 0);
	seq_puts(s, "Hardware-routed traffic bypasses netfilter and NAT.  A route is\n"
		    "offloaded only when every more specific route is offloaded too;\n"
		    "everything else keeps going through the kernel.\n");
	return 0;
}

static ssize_t route_offload_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = file->f_inode->i_private;
	char buf[8];
	bool on;

	if (count >= sizeof(buf) || copy_from_user(buf, ubuf, count))
		return -EINVAL;
	buf[count] = '\0';
	if (kstrtobool(buf, &on))
		return -EINVAL;
	l3_enable(eth, on);
	return count;
}

static int route_offload_open(struct inode *inode, struct file *file)
{
	return single_open(file, route_offload_show, inode->i_private);
}

static const struct file_operations route_offload_fops = {
	.owner = THIS_MODULE,
	.open = route_offload_open,
	.read = seq_read,
	.write = route_offload_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void rtl8198c_l3_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("l3", 0400, eth->debugfs_dir, eth, &l3_fops);
	debugfs_create_file("taci", 0600, eth->debugfs_dir, eth, &taci_fops);
	debugfs_create_file("route_offload", 0644, eth->debugfs_dir, eth,
			    &route_offload_fops);
}
