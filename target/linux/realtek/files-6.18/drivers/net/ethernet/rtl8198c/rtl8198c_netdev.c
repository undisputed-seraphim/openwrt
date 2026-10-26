// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- K: netdev operations
 *
 * net_device_ops and rtl8198c_ethtool_ops, the MTU/MAC/VLAN handlers, the LAG
 * notifier and the per-port statistics.
 *
 * BSP counterparts: rtl_nic.c (netdev glue).
 */
#include "rtl8198c.h"
#include <linux/if_bridge.h>
#include <net/switchdev.h>

/* ===== net_device_ops ===== */
static int eth_open(struct net_device *dev)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	int ret;

	/* The CPU interface is shared: the first netdev to come up arms the
	 * rings and starts the engine, the last one to go down stops it. */
	if (!eth->open_count) {
		/* Buffers follow the largest port MTU; this runs before the
		 * engine starts, so a resize costs nothing. */
		rtl8198c_eth_rx_bufs_sync(eth);
		ret = rtl8198c_refill_rx(eth);
		if (ret)
			return ret;

		rtl8198c_eth_rings_program(eth);
		rtl8198c_switch_start();
		napi_enable(&eth->napi);
		/* Frames can complete between the RX enable and the interrupt
		 * mask being armed; poll once so they are not stranded. */
		napi_schedule(&eth->napi);
	}
	eth->open_count++;
	/* A cable may already be in: the interrupt only reports changes, so
	 * take the port's link state now. */
	rtl8198c_link_update(eth);
	netif_start_queue(dev);
	return 0;
}

static int eth_stop(struct net_device *dev)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	int i;

	netif_stop_queue(dev);
	/* No link reports while the engine is down, and a closed port has no
	 * carrier to report. */
	netif_carrier_off(dev);
	if (--eth->open_count)
		return 0;

	/* SIRR.TRXRDY is deliberately left set: the PSCR and descriptor
	 * blocks expect it, and the next open does not repeat the hard
	 * reset.  The vendor stop does the same. */
	napi_disable(&eth->napi);
	cpu_w32(CPUIIMR, 0);
	cpu_w32(CPUICR, 0);
	sys_w32(GIMR, sys_r32(GIMR) & ~BSP_SW_IE);
	rtl8198c_tx_clean(eth);
	rtl8198c_free_rx_skbs(eth);
	for (i = 0; i < TX_RINGS; i++)
		eth->tx_head[i] = eth->tx_tail[i] = 0;
	for (i = 0; i < RX_RINGS; i++)
		eth->rx_head[i] = 0;
	return 0;
}

/* The switch decides "this frame is for the CPU" by matching the MAC in the
 * L2 and netif tables, so both have to follow a changed address. */
/* Changing the address has to rewrite two tables: the L2 entry decides
 * "send this to the CPU" and the netif entry decides "this is a local
 * address".  Miss either and the port stops receiving (or keeps answering
 * to the old address until the entry ages out). */
static int eth_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *sa = addr;
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	u8 old[ETH_ALEN];
	int i;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;
	if (ether_addr_equal(sa->sa_data, dev->dev_addr))
		return 0;

	/* Two ports with the same address could not be told apart. */
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i] && eth->netdev[i] != dev &&
		    ether_addr_equal(sa->sa_data, eth->netdev[i]->dev_addr))
			return -EADDRINUSE;

	memcpy(old, dev->dev_addr, ETH_ALEN);
	eth_hw_addr_set(dev, sa->sa_data);

	rtl8198c_l2_fdb_del(old, p->fid);
	rtl8198c_l2_fdb_add(dev->dev_addr, 0, 1, true, p->fid);
	rtl8198c_netif_table_write(eth, p->port, dev->dev_addr, p->vid);
	return 0;
}

static int eth_change_mtu(struct net_device *dev, int new_mtu)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;

	if (new_mtu < ETH_MIN_MTU || new_mtu > JUMBO_MTU)
		return -EINVAL;
	dev->mtu = new_mtu;
	eth->port_mtu[p->port] = new_mtu;
	/* The switch's netif entry carries the MTU it will accept. */
	rtl8198c_netif_table_write(eth, p->port, dev->dev_addr, p->vid);
	/* The RX ring is shared, so a bigger MTU on one port may need bigger
	 * buffers for every frame the CPU receives. */
	if (eth->open_count)
		rtl8198c_eth_rx_restart(eth);
	return 0;
}

/* One netdev TX queue fronts the whole port, so tx_maxrate is the
 * port's total egress limit.  Per-queue limits use debugfs "rate". */
static int eth_set_tx_maxrate(struct net_device *dev, int queue_index,
			      u32 maxrate)
{
	struct rtl8198c_port *p = netdev_priv(dev);

	if (queue_index != 0)
		return -EINVAL;
	rtl8198c_port_egress_set(p->port, DIV_ROUND_UP(maxrate, 125)); /* B/s -> kbps */
	return 0;
}

const struct net_device_ops rtl8198c_eth_ops = {
	.ndo_open	= eth_open,
	.ndo_stop	= eth_stop,
	.ndo_get_stats64 = dev_get_tstats64,
	.ndo_start_xmit	= rtl8198c_eth_start_xmit,
	.ndo_change_mtu	= eth_change_mtu,
	.ndo_set_tx_maxrate = eth_set_tx_maxrate,
	.ndo_set_mac_address = eth_set_mac_address,
	.ndo_vlan_rx_add_vid = rtl8198c_eth_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = rtl8198c_eth_vlan_rx_kill_vid,
	.ndo_fdb_add	= rtl8198c_eth_fdb_add,
	.ndo_fdb_del	= rtl8198c_eth_fdb_del,
	.ndo_fdb_del_bulk = rtl8198c_eth_fdb_del_bulk,
	.ndo_fdb_dump	= rtl8198c_eth_fdb_dump,
	/* No .ndo_set_rx_mode: the switch cannot express promiscuous or
	 * allmulti.  FFCR's TOCPU bits are global and do not gate CPU
	 * delivery (the CPU receives unknown unicast and multicast because it
	 * is a VLAN member), and IFF_PROMISC is a hardware no-op - measured,
	 * the same injected unknown-unicast frames arrived with promisc off
	 * and on.  Per-group delivery is done with an L2 entry instead, which
	 * is what debugfs "mcast" programs. */
};

/* ===== Bridge / spanning tree =====
 *
 * The switch's STP engine is global, so it is on exactly while at least one
 * port is a bridge member: unbridged ports keep the plain forwarding path
 * and no BPDUs are trapped.  Leaving the last bridge also puts every port
 * back to forwarding - br_del_if() disables the port state before it unlinks
 * the device, and the deferred attribute that carries that state is flushed
 * while the port still looks bridged, so the reset has to come from the
 * unlink side to win.
 */
static void eth_bridge_port(struct rtl8198c_eth *eth, u32 port, bool join)
{
	if (join) {
		eth->bridge_ports++;
		/* All ports are in forwarding state from init. */
		if (eth->bridge_ports == 1)
			rtl8198c_stp_enable(true);
		return;
	}

	if (eth->bridge_ports)
		eth->bridge_ports--;
	rtl8198c_stp_port_state(port, PCRP_STP_FORWARDING);
	if (!eth->bridge_ports) {
		for (port = 0; port < ETH_PORTS; port++)
			rtl8198c_stp_port_state(port, PCRP_STP_FORWARDING);
		rtl8198c_stp_enable(false);
	}
}

/* br_set_state() defers its switchdev attribute to a work item, which is the
 * only path the kernel offers a driver for a bridge port state.  Listening
 * and blocking share the hardware encoding. */
static int eth_switchdev_port_attr(struct net_device *dev,
				   struct switchdev_notifier_port_attr_info *info)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	u32 state;

	if (info->attr->id != SWITCHDEV_ATTR_ID_PORT_STP_STATE)
		return NOTIFY_DONE;

	switch (info->attr->u.stp_state) {
	case BR_STATE_DISABLED:
		state = PCRP_STP_DISABLE;
		break;
	case BR_STATE_LISTENING:
	case BR_STATE_BLOCKING:
		state = PCRP_STP_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		state = PCRP_STP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		state = PCRP_STP_FORWARDING;
		break;
	default:
		return NOTIFY_DONE;
	}

	rtl8198c_stp_port_state(p->port, state);
	info->handled = true;
	return NOTIFY_OK;
}

static int eth_switchdev_blocking_event(struct notifier_block *nb,
					unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);

	if (event != SWITCHDEV_PORT_ATTR_SET ||
	    dev->netdev_ops != &rtl8198c_eth_ops)
		return NOTIFY_DONE;
	return eth_switchdev_port_attr(dev, ptr);
}

struct notifier_block rtl8198c_switchdev_nb = {
	.notifier_call = eth_switchdev_blocking_event,
};

/* Linux bonding and team enslave a port without asking the driver for
 * anything: the netdev API has no LAG offload request (switchdev carries no
 * LAG attribute either), the port is simply linked under the upper device and
 * the kernel hashes into whichever member's netdev it picks.  That works, but
 * the switch knows nothing about the bond, so say so when it happens rather
 * than leave the user wondering why the bond carries no port-to-port traffic.
 * The hardware trunk itself is not usable: see the "lag" debugfs file. */
static const char *eth_lag_mode(const struct netdev_lag_upper_info *lag)
{
	if (!lag)
		return "unknown mode";
	switch (lag->tx_type) {
	case NETDEV_LAG_TX_TYPE_RANDOM:		return "random";
	case NETDEV_LAG_TX_TYPE_BROADCAST:	return "broadcast";
	case NETDEV_LAG_TX_TYPE_ROUNDROBIN:	return "round-robin";
	case NETDEV_LAG_TX_TYPE_ACTIVEBACKUP:	return "active-backup";
	case NETDEV_LAG_TX_TYPE_HASH:		return "hash";
	default:				return "unknown mode";
	}
}

static int eth_netdev_event(struct notifier_block *nb, unsigned long event,
			    void *ptr)
{
	struct netdev_notifier_changeupper_info *info = ptr;
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl8198c_port *p;

	if (event != NETDEV_CHANGEUPPER || dev->netdev_ops != &rtl8198c_eth_ops)
		return NOTIFY_DONE;

	p = netdev_priv(dev);
	if (netif_is_bridge_master(info->upper_dev)) {
		eth_bridge_port(p->sw, p->port, info->linking);
		return NOTIFY_DONE;
	}

	if (!info->linking || !netif_is_lag_master(info->upper_dev))
		return NOTIFY_DONE;

	netdev_info(dev,
		    "joined %s (%s): no hardware trunk on this switch, the bond runs in software; switch forwarding between other ports is unaffected\n",
		    info->upper_dev->name, eth_lag_mode(info->upper_info));
	return NOTIFY_DONE;
}

struct notifier_block rtl8198c_eth_netdev_nb = {
	.notifier_call = eth_netdev_event,
};

/* ===== Statistics =====
 *
 * The RTL865x MIB counter block is documented at SWCORE + 0x1000, but on
 * this SoC that page is not decoded: every per-port counter address reads
 * zero and writes to MIB_CONTROL are ignored, while SWCORE + 0x4000,
 * +0x5000 and +0x6100 (GDSR) all respond normally.  So the switch's own
 * per-port counters are unavailable and ethtool -S exposes what the driver
 * does have: its packet/error counters and the PHY link state per port.
 */
static const char * const eth_stat_names[] = {
	"rx_packets", "tx_packets", "rx_bytes", "tx_bytes",
	"rx_errors", "tx_errors", "rx_dropped", "tx_dropped",
};

#define ETH_STATS_NUM		(ARRAY_SIZE(eth_stat_names) + ETH_PORTS * 3 + \
				 TX_RINGS + RX_RINGS + 8 + 2)

static int ethtool_get_sset_count(struct net_device *dev, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return ETH_STATS_NUM;
}

static void ethtool_get_strings(struct net_device *dev, u32 sset, u8 *data)
{
	int i, p;

	if (sset != ETH_SS_STATS)
		return;
	for (i = 0; i < ARRAY_SIZE(eth_stat_names); i++)
		strscpy(data + i * ETH_GSTRING_LEN, eth_stat_names[i],
			ETH_GSTRING_LEN);
	for (p = 0; p < ETH_PORTS; p++) {
		snprintf(data + (ARRAY_SIZE(eth_stat_names) + p * 3) *
			 ETH_GSTRING_LEN, ETH_GSTRING_LEN, "port%d_link", p);
		snprintf(data + (ARRAY_SIZE(eth_stat_names) + p * 3 + 1) *
			 ETH_GSTRING_LEN, ETH_GSTRING_LEN, "port%d_gigabit", p);
		snprintf(data + (ARRAY_SIZE(eth_stat_names) + p * 3 + 2) *
			 ETH_GSTRING_LEN, ETH_GSTRING_LEN, "port%d_rx_packets",
			 p);
	}
	for (p = 0; p < TX_RINGS; p++)
		snprintf(data + (ARRAY_SIZE(eth_stat_names) + ETH_PORTS * 3 + p) *
			 ETH_GSTRING_LEN, ETH_GSTRING_LEN, "tx_ring%d_packets", p);
	for (p = 0; p < RX_RINGS; p++)
		snprintf(data + (ARRAY_SIZE(eth_stat_names) + ETH_PORTS * 3 +
				 TX_RINGS + p) * ETH_GSTRING_LEN, ETH_GSTRING_LEN,
			 "rx_ring%d_packets", p);
	for (p = 0; p < 8; p++)
		snprintf(data + (ARRAY_SIZE(eth_stat_names) + ETH_PORTS * 3 +
				 TX_RINGS + RX_RINGS + p) * ETH_GSTRING_LEN,
			 ETH_GSTRING_LEN, "rx_queue%d_packets", p);
	strscpy(data + (ETH_STATS_NUM - 2) * ETH_GSTRING_LEN,
		"tx_sg_packets", ETH_GSTRING_LEN);
	strscpy(data + (ETH_STATS_NUM - 1) * ETH_GSTRING_LEN,
		"tx_linearized", ETH_GSTRING_LEN);
}

/* ethtool -S: the driver's counters plus, per physical port, whether that
 * port has a link and whether it trained at 1G.
 *
 * The switch's own per-port MIB counters are not here because that block
 * does not decode on this SoC: SWCORE + 0x1000 reads zero and ignores
 * MIB_CONTROL, while +0x4000 (MACCR), +0x5000 (PSCR98C) and +0x6100
 * (GDSR0) all respond - the register window only exists from +0x4000 up.
 */
static void ethtool_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct rtl8198c_port *priv = netdev_priv(dev);
	struct rtl8198c_eth *eth = priv->sw;
	struct net_device_stats *st = &dev->stats;
	struct rtnl_link_stats64 ls;
	int p, n = 0;

	dev_get_stats(dev, &ls);
	data[n++] = ls.rx_packets;
	data[n++] = ls.tx_packets;
	data[n++] = ls.rx_bytes;
	data[n++] = ls.tx_bytes;
	data[n++] = st->rx_errors;
	data[n++] = st->tx_errors;
	data[n++] = st->rx_dropped;
	data[n++] = st->tx_dropped;

	for (p = 0; p < ETH_PORTS; p++) {
		u32 phy = rtl8198c_port_phy_id(p);
		u32 bmsr = rtl8198c_mdio_read(phy, 1);
		bool up = !!(bmsr & BIT(2));

		/* 1000BASE-T status bit 13 = local receiver OK, i.e. the
		 * link really is at 1G; other speeds are not reported by a
		 * standard register. */
		data[n++] = up;
		data[n++] = up && (rtl8198c_mdio_read(phy, MII_STAT1000) &
				   STAT1000_LOCAL_RX_OK);
		data[n++] = eth->rx_port_packets[p];
	}

	for (p = 0; p < TX_RINGS; p++)
		data[n++] = eth->tx_ring_packets[p];
	for (p = 0; p < RX_RINGS; p++)
		data[n++] = eth->rx_ring_packets[p];
	for (p = 0; p < 8; p++)
		data[n++] = eth->rx_queue_packets[p];
	data[n++] = eth->tx_sg_packets;
	data[n++] = eth->tx_lin_packets;
}

/* ===== ethtool ops ===== */
static void ethtool_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "rtl8198c-eth", sizeof(info->driver));
	strscpy(info->version, "1.0", sizeof(info->version));
}

static u32 ethtool_get_link(struct net_device *dev) { return netif_running(dev); }

const struct ethtool_ops rtl8198c_ethtool_ops = {
	.get_drvinfo	= ethtool_get_drvinfo,
	.get_link	= ethtool_get_link,
	.get_sset_count	= ethtool_get_sset_count,
	.get_strings	= ethtool_get_strings,
	.get_ethtool_stats = ethtool_get_ethtool_stats,
	.get_link_ksettings = rtl8198c_ethtool_get_link_ksettings,
	.set_link_ksettings = rtl8198c_ethtool_set_link_ksettings,
	.nway_reset	= rtl8198c_ethtool_nway_reset,
};
