// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- driver-private types and interfaces.
 */
#ifndef _RTL8198C_H
#define _RTL8198C_H

#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/inet.h>
#include <linux/neighbour.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <net/netlink.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/of_net.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>

#include "rtl8198c_regs.h"

/* ===== IP multicast groups =====
 *
 * The switch forwards IP multicast by MAC: a group is an L2 FDB entry for
 * its derived multicast address (01:00:5e:xx:xx:xx for IPv4, 33:33:xx for
 * IPv6) with a port mask.  That is also what the vendor's IGMP snooping
 * uses in bridge mode (rtl865x_L2multicastCallbackFn() ->
 * rtl_addFilterDatabaseEntryforMCast()), and the IP (S,G) table is
 * disabled in that configuration.  Measured here as well: programming the
 * (S,G) table does not steer traffic on an L2 bridge, an FDB entry does.
 */
#define MCAST_GROUPS		64

struct mcast_group {
	u8	mac[ETH_ALEN];
	u8	fid;
	u16	portmask;
	bool	cpu;
	bool	used;
};

/* One ACL rule, as the debugfs parser stores it before it is turned into a
 * table entry. */
struct acl_rule {
	u8	type;
	u8	action;
	u8	dmac[ETH_ALEN], dmac_mask[ETH_ALEN];
	u8	smac[ETH_ALEN], smac_mask[ETH_ALEN];
	u16	ethertype, ethertype_mask;
	__be32	sip, sip_mask, dip, dip_mask;
	u8	proto, proto_mask;
	u8	tos, tos_mask;
};

/* A programmed VLAN.  One filter database per VLAN, because the core has
 * four and the row hash is salted with the fid: sharing a database would
 * merge two VLANs' MAC tables. */
#define MAX_VLANS		4

struct rtl8198c_vlan {
	u16	vid;
	u8	member;		/* physical port mask, ports 0-5 */
	u8	untag;		/* ports whose tag is stripped on egress */
	u8	fid;
	bool	used;
};

/* One netdev's view of the switch: the shared controller plus its port. */
struct rtl8198c_port {
	struct rtl8198c_eth	*sw;
	u32			port;
	u32			vid;	/* this port's VLAN (PVID) */
	u32			fid;	/* its filtering database */
	bool			offloaded;	/* bridge offload registered */
};

/* ===== L3 state =====
 *
 * One netif entry per VLAN: the switch looks the interface up by vid, and
 * a second entry on the same vid stops the L3 stage from running at all.
 * The MAC and MTU come from the netdev that holds the IPv4 address.
 */
#define L3_VIDS		8	/* netif table entries */
#define L3_FIB_MAX	256	/* routes mirrored before giving up */
#define L3_NEIGHS	128	/* resolved neighbours kept for ARP entries */
#define L3_BLOCKS	8	/* on-link prefixes with an ARP block */
#define L3_PINS		64	/* L2 entries the hardware depends on */
#define L3_ARP_UNITS	(L3_ARPS / L3_ARP_STRIDE)

struct rtl8198c_l3_vid {
	u16	vid;
	bool	used;
	bool	route;			/* enHWRoute */
	u8	mac[ETH_ALEN];
	struct net_device *owner;	/* netdev holding the IPv4 address */
};

/* A route as the kernel sees it, kept only to decide what the hardware can
 * be trusted with.  Rebuilt from a FIB dump on every change. */
struct rtl8198c_l3_fib {
	__be32	dst;
	__be32	gw;
	u32	dev;		/* nexthop ifindex */
	u16	tb_id;
	u8	len;
	u8	type;		/* RTN_* */
	u8	slot;		/* route table index, 0xff = not offloaded */
	u8	dscp;
	bool	nh_obj;
	bool	lwt;		/* lightweight tunnel: CPU encapsulates */
	u8	paths;		/* >1: multipath, would use one path only */
};

struct rtl8198c_l3_pin {
	u8	mac[ETH_ALEN];
	u8	fid;
	bool	used;
};

/* A resolved neighbour, as ARP entries are written from the cache rather
 * than by walking the kernel's table. */
struct rtl8198c_l3_neigh {
	__be32	ip;
	u8	mac[ETH_ALEN];
	u32	dev;		/* ifindex */
	bool	valid;		/* NUD_VALID and not dead */
};

/* One on-link prefix owns a contiguous run of ARP entries; the hardware
 * indexes it with the host part of the destination address. */
struct rtl8198c_l3_block {
	__be32	dst;
	u32	dev;
	u16	start;
	u8	len;
	u8	units;		/* 8-entry units */
	bool	used;
};

struct rtl8198c_l3 {
	bool	enabled;	/* route_offload knob, off by default */
	bool	resyncing;	/* FIB dump in progress */
	bool	registered;
	bool	overflow;	/* FIB mirror full: offload nothing */
	struct delayed_work work;
	struct notifier_block fib_nb;
	struct notifier_block neigh_nb;
	struct notifier_block addr_nb;
	struct rtl8198c_l3_fib fib[L3_FIB_MAX];
	int	fib_count;
	struct rtl8198c_l3_pin pin[L3_PINS];
	struct rtl8198c_l3_neigh neigh[L3_NEIGHS];
	spinlock_t neigh_lock;	/* netevent notifiers run in atomic context */
	struct rtl8198c_l3_block block[L3_BLOCKS];
	u8 block_used[L3_ARP_UNITS];
	u16 arp_l2[L3_ARPS];	/* L2 slot per ARP entry, 0xffff = empty */
	struct mutex lock;	/* protects the mirror and the vid table */
};

/* ===== L4 state (NAPT / flow offload) =====
 *
 * The hardware keeps the sessions; the driver keeps the tuple of every
 * entry it installed, because a session entry does not store its own key:
 * the lookup index is a hash of the 5-tuple and the entry only holds the
 * rewrite.  Without the mirror a hash collision, or an entry left behind by
 * an earlier session, is indistinguishable from one of ours.
 *
 * An external-IP entry is reference counted: one per NAT address, shared by
 * every session that masquerades through it.
 */
struct rtl8198c_l4_tuple {
	__be32	int_ip;
	__be32	rem_ip;
	__be32	ext_ip;
	__be16	int_port;
	__be16	rem_port;
	__be16	ext_port;
	bool	tcp;
};

struct rtl8198c_l4_extip {
	__be32	ext_ip;
	u8	users;
	bool	used;
};

struct rtl8198c_l4_session {
	struct rtl8198c_l4_tuple t;
	unsigned long cookie[2];	/* nftables flow identities, one per dir */
	u8	aging;		/* encoded idle timeout */
	u8	ncookies;
	bool	used;
};

struct rtl8198c_l4 {
	bool	enabled;	/* MSCR.EN_L4 armed */
	struct rtl8198c_l4_extip extip[L4_EXT_IPS];
	struct rtl8198c_l4_session sess[L4_SESSIONS];
	struct mutex lock;	/* serialises table access and the mirror */
};

struct rtl8198c_eth {
	struct device *dev;
	int irq;
	struct net_device *netdev[ETH_PORTS];
	int open_count;			/* open netdevs; the CPU engine runs while > 0 */
	u8 bridge_ports;		/* member ports; STP runs while > 0 */
	spinlock_t tx_lock;		/* serialises tx_head and the shared TX ring */
	struct rtl8198c_vlan vlans[MAX_VLANS];
	struct acl_rule acl_rules[ACL_MAX_RULES];
	struct mcast_group mcast[MCAST_GROUPS];
	u16	mcast_count;
	u16	acl_count;		/* user rules, entries 0..count-1 */
	u8	acl_end;		/* last entry of the chain (the catch-all) */
	u8	acl_default;		/* action of the catch-all */
	u8	port_mac[ETH_PORTS][ETH_ALEN];
	u8	switch_id[ETH_ALEN];		/* stable id of this switch */
	struct rtl8198c_l3_vid l3_vid[L3_VIDS];
	struct rtl8198c_l3 l3;
	struct rtl8198c_l4 l4;
	u16	port_mtu[ETH_PORTS];		/* per-port netif-table MTU */
	u16	pbvlan_vid[PBV_RULES][ETH_PORTS];	/* 0 = rule off */
	u16	pbvlan_ethertype[2];			/* user rules 5/6 */
	u16	fid_vid[L2_FIDS];	/* VLAN owning each filter database */

	/* legacy descriptor rings — arrays of u32 pointers with OWN/WRAP bits */
	u32	*rx_ring;	/* RX_RINGS blocks of RX_RING_SIZE */
	u32	*tx_ring;
	u32	*mbuf_ring;
	void	*rx_ring_k0, *tx_ring_k0, *mbuf_ring_k0;

	/* pools */
	struct pkthdr	*pktHdr_pool;
	struct mbuf	*mbuf_pool;
	struct mbuf	*sg_mbuf_pool;	/* TX_RINGS*TX_RING_SIZE chains */
	void		*sg_mbuf_k0;

	/* One skb per RX mbuf, indexed by *mbuf* (not by descriptor slot).
	 * The RISC takes its buffer from the linear mbuf ring, writes the
	 * frame into m_data and rewrites ph_mbuf to the mbuf it used, so the
	 * mbuf index, not the pkthdr slot, identifies the received skb. */
	struct sk_buff *rx_skb[RX_RINGS * RX_RING_SIZE];
	u16 rx_bufsz;			/* current RX buffer size */
	struct sk_buff *tx_skb[TX_RINGS][TX_RING_SIZE];
	int rx_head[RX_RINGS], tx_head[TX_RINGS], tx_tail[TX_RINGS];
	u64 tx_ring_packets[TX_RINGS];
	u64 rx_vlan_tagged;		/* debug: frames the ASIC saw as tagged */
	u64 rx_vlan_untagged;
	u32 rx_vlan_last;		/* vid << 1 | tagged */
	u64 tx_sg_packets;		/* frames sent as an mbuf chain */
	u64 tx_lin_packets;		/* non-linear frames linearised */
	u64 rx_ring_packets[RX_RINGS];
	u64 rx_queue_packets[8];
	u8 rx_qmap[8];			/* queue -> RX ring */

	struct napi_struct napi;
	bool	link_change;		/* LINK_CHANGE_IP seen, carrier not yet synced */
	u64	rx_port_packets[ETH_PORTS];	/* frames delivered per source port */
	struct dentry *debugfs_dir;
	u32	reg_addr;	/* last address selected through debugfs "reg" */
	u8	mac_filter[ETH_ALEN];
	bool	mac_filter_enabled;
	bool	green_enabled;
};



/* ===== Register access (common) =====
 *
 * Inline so the per-packet paths (TX and RX) reach the MMIO window without a
 * cross-file call: on this CPU the data path is CPU-bound, not wire-bound. */
static inline void cpu_w32(u32 reg, u32 val)
{ *(volatile u32 *)(CPU_IF_BASE + reg) = val; }
static inline u32 cpu_r32(u32 reg)
{ return *(volatile u32 *)(CPU_IF_BASE + reg); }
static inline void sys_w32(u32 addr, u32 val)
{ *(volatile u32 *)(addr) = val; }
static inline u32 sys_r32(u32 addr)
{ return *(volatile u32 *)(addr); }
static inline void *kseg1(void *k0)
{ return (void *)((u32)k0 | UNCACHE_MASK); }
static inline void cache_wb(void *addr, int size)
{ dma_cache_wback((unsigned long)addr, size); }

/* ===== Table address encoding =====
 *
 * The switch tables compare an IPv4 address as the big-endian 32-bit word
 * the vendor build writes on its big-endian CPU: 192.168.2.0 is 0xC0A80200
 * in the entry.  On this little-endian CPU that is ntohl() of the __be32
 * value. */
static inline u32 ip_to_word(__be32 addr)
{
	return ntohl((__force u32)addr);
}

static inline __be32 word_to_ip(u32 word)
{
	return (__force __be32)htonl(word);
}

/* ===== Interfaces between the block files ===== */

/* debugfs knobs, one initialiser per block */
void rtl8198c_l2_debugfs_init(struct rtl8198c_eth *eth);
void rtl8198c_phy_debugfs_init(struct rtl8198c_eth *eth);
void rtl8198c_qos_debugfs_init(struct rtl8198c_eth *eth);
void rtl8198c_acl_debugfs_init(struct rtl8198c_eth *eth);
void rtl8198c_mcast_debugfs_init(struct rtl8198c_eth *eth);
void rtl8198c_dma_debugfs_init(struct rtl8198c_eth *eth);

/* core */
void *rtl8198c_uncached_alloc(size_t size, u32 *phys, void **k0_out);
void rtl8198c_switch_start(void);

/* phy */
int rtl8198c_ethtool_get_link_ksettings(struct net_device *dev, struct ethtool_link_ksettings *cmd);
int rtl8198c_ethtool_nway_reset(struct net_device *dev);
int rtl8198c_ethtool_set_link_ksettings(struct net_device *dev, const struct ethtool_link_ksettings *cmd);
u32 rtl8198c_mdio_read(u32 phy, u32 reg);
u32 rtl8198c_port_phy_id(u32 port);
void rtl8198c_mdio_write(u32 phy, u32 reg, u32 val);
void rtl8198c_link_update(struct rtl8198c_eth *eth);
void rtl8198c_switch_gphy_init(void);

/* l2 */
int rtl8198c_eth_fdb_add(struct ndmsg *ndm, struct nlattr *tb[], struct net_device *dev, const unsigned char *addr, u16 vid, u16 flags, bool *notified, struct netlink_ext_ack *extack);
int rtl8198c_eth_fdb_del(struct ndmsg *ndm, struct nlattr *tb[], struct net_device *dev, const unsigned char *addr, u16 vid, bool *notified, struct netlink_ext_ack *extack);
int rtl8198c_eth_fdb_del_bulk(struct nlmsghdr *nlh, struct net_device *dev, struct netlink_ext_ack *extack);
int rtl8198c_eth_fdb_dump(struct sk_buff *skb, struct netlink_callback *cb, struct net_device *dev, struct net_device *filter_dev, int *idx);
int rtl8198c_eth_vlan_rx_add_vid(struct net_device *dev, __be16 proto, u16 vid);
int rtl8198c_eth_vlan_rx_kill_vid(struct net_device *dev, __be16 proto, u16 vid);
int rtl8198c_l2_fdb_add(const u8 *mac, u8 member, u8 ext, bool to_cpu, u32 fid);
int rtl8198c_l2_fdb_lookup(const u8 *mac, u32 fid, u16 *l2);
int rtl8198c_l2_fdb_pin(const u8 *mac, u32 fid);
int rtl8198c_l2_fdb_del(const u8 *mac, u32 fid);
void rtl8198c_netif_table_write(struct rtl8198c_eth *eth, u32 idx, const u8 *mac, u32 vid, u32 mtu, bool route);
void rtl8198c_netif_vlan_init(struct rtl8198c_eth *eth, const u8 macs[][ETH_ALEN]);
void rtl8198c_switch_l2_init(const u8 macs[][ETH_ALEN]);
void rtl8198c_taci_write_entry(u32 taa, const u32 e[8], u32 cmd_bits);
void rtl8198c_taci_read_entry(u32 taa, u32 e[8]);
void rtl8198c_stp_enable(bool on);
void rtl8198c_stp_port_state(u32 port, u32 state);

/* acl */
void rtl8198c_acl_init(struct rtl8198c_eth *eth);

/* l3 */
void rtl8198c_l3_init(void);
void rtl8198c_l3_netif_init(struct rtl8198c_eth *eth);
void rtl8198c_l3_netif_sync(struct rtl8198c_eth *eth);
void rtl8198c_l3_dev_update(struct rtl8198c_eth *eth, struct net_device *dev);
int rtl8198c_l3_register(struct rtl8198c_eth *eth);
void rtl8198c_l3_unregister(struct rtl8198c_eth *eth);
void rtl8198c_l3_table_write(u32 tbl, u32 index, const u32 e[8]);
void rtl8198c_l3_table_clear(u32 tbl, u32 count);
void rtl8198c_l3_route_arp(u32 e[8], __be32 prefix, u8 len, u8 netif, bool internal, u16 start, u16 count);
void rtl8198c_l3_route_l2(u32 e[8], __be32 prefix, u8 len, u8 netif, bool internal, u16 l2);
void rtl8198c_l3_route_nexthop(u32 e[8], __be32 prefix, u8 len, bool internal, u8 start, u8 count);
void rtl8198c_l3_route_cpu(u32 e[8], __be32 prefix, u8 len, u8 netif, bool internal);
void rtl8198c_l3_route_drop(u32 e[8], __be32 prefix, u8 len, u8 netif, bool internal);
void rtl8198c_l3_arp_build(u32 e[8], u16 l2, u8 aging);
void rtl8198c_l3_nexthop_build(u32 e[8], u8 vid, u8 ip_index, u16 l2, bool pppoe);
void rtl8198c_l3_extip_build(u32 e[8], __be32 int_ip, __be32 ext_ip, u8 nexthop, bool one2one, bool local_public);
void rtl8198c_l3_debugfs_init(struct rtl8198c_eth *eth);

/* l4 */
void rtl8198c_l4_init(void);
void rtl8198c_l4_engine(struct rtl8198c_eth *eth, bool on);
void rtl8198c_l4_flush(struct rtl8198c_eth *eth);
int rtl8198c_l4_flow_add(struct rtl8198c_eth *eth,
			 const struct rtl8198c_l4_tuple *t, u32 aging);
int rtl8198c_l4_flow_del(struct rtl8198c_eth *eth,
			 const struct rtl8198c_l4_tuple *t);
int rtl8198c_l4_setup_tc(struct net_device *dev, enum tc_setup_type type,
			 void *type_data);
u32 rtl8198c_l4_hash(bool tcp, __be32 sip, __be16 sport, __be32 dip,
		     __be16 dport);
void rtl8198c_l4_session_out(u32 e[8], __be32 int_ip, __be16 int_port,
			     __be16 ext_port, u8 ext_idx, bool tcp, u8 aging);
void rtl8198c_l4_session_in(u32 e[8], __be32 int_ip, __be16 int_port,
			    __be32 rem_ip, __be16 rem_port, __be16 ext_port,
			    bool tcp, u8 aging);
void rtl8198c_l4_debugfs_init(struct rtl8198c_eth *eth);

/* mcast */
int rtl8198c_mcast_mdb_add(struct rtl8198c_eth *eth, const u8 *mac, u8 fid, u32 port);
int rtl8198c_mcast_mdb_del(struct rtl8198c_eth *eth, const u8 *mac, u8 fid, u32 port);

/* qos */
void rtl8198c_port_egress_set(int port, u32 kbps);
void rtl8198c_storm_set_rate(u32 percent);

/* dma */
int rtl8198c_alloc_rings(struct rtl8198c_eth *eth);
int rtl8198c_eth_poll(struct napi_struct *napi, int budget);
int rtl8198c_refill_rx(struct rtl8198c_eth *eth);
irqreturn_t rtl8198c_eth_irq(int irq, void *dev_id);
netdev_tx_t rtl8198c_eth_start_xmit(struct sk_buff *skb, struct net_device *dev);
void rtl8198c_eth_rings_program(struct rtl8198c_eth *eth);
void rtl8198c_eth_rx_bufs_sync(struct rtl8198c_eth *eth);
void rtl8198c_eth_rx_restart(struct rtl8198c_eth *eth);
void rtl8198c_free_rings(struct rtl8198c_eth *eth);
void rtl8198c_free_rx_skbs(struct rtl8198c_eth *eth);
void rtl8198c_tx_clean(struct rtl8198c_eth *eth);

/* netdev */
extern const struct ethtool_ops rtl8198c_ethtool_ops;
extern const struct net_device_ops rtl8198c_eth_ops;
extern struct notifier_block rtl8198c_eth_netdev_nb;
extern struct notifier_block rtl8198c_switchdev_nb;

#endif /* _RTL8198C_H */
