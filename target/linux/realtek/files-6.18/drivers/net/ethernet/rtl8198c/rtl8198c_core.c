// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- CPU interface, reset, platform driver
 *
 * Register access, the switch-core reset sequence, the CPU interface
 * start-up, the shared debugfs root and probe/remove.
 */
#include "rtl8198c.h"
#include <net/switchdev.h>

void *rtl8198c_uncached_alloc(size_t size, u32 *phys, void **k0_out)
{
	void *k0 = kmalloc(size, GFP_KERNEL);
	if (!k0) return NULL;
	memset(k0, 0, size);
	if (phys) *phys = (u32)(unsigned long)k0 & 0x1FFFFFFF;
	if (k0_out) *k0_out = k0;
	return kseg1(k0);
}

/* ===== Switch core reset (clock gate only, no FULL_RST) ===== */
static void switch_reset(void)
{
	/* Clock gate toggle only: no FULL_RST, the core stays configured. */
	sys_w32(SYS_CLK_MAG, sys_r32(SYS_CLK_MAG) & ~CM_ACTIVE_SWCORE);
	mdelay(300);
	sys_w32(SYS_CLK_MAG, sys_r32(SYS_CLK_MAG) | CM_ACTIVE_SWCORE);
	mdelay(50);
}

/* Buffer watermarks for flow control and descriptor runout (reset values).
 * The SBFCTR block is not part of PSCR, so it does not need TRXRDY.
 * SBFCR0 is the system descriptor-runout threshold; SBFCR1/2 and PBFCR0-5
 * each pack a pause-off and a pause-on watermark, off in the high half and on
 * in the low half.  These decide when pause is asserted, not how much traffic
 * may pass - the egress token bucket lives in PSCR. */
static void switch_flow_control_init(void)
{
	int i;

	sys_w32(SBFCR0, 0x1E0);
	sys_w32(SBFCR1, (0x190 << 16) | 0x1CC);
	sys_w32(SBFCR2, (0x050 << 16) | 0x06C);
	for (i = 0; i < 6; i++)
		sys_w32(PBFCR0 + i * 4, (0x03C << 16) | 0x05A);
}

/* Put the switch core into the state the rest of the init assumes,
 * rather than relying on the previous state.  Two ordering rules drive
 * the layout below:
 *
 *  - SIRR.TRXRDY is set before any PHY/L2/MAC programming.
 *  - PSCR (leaky buckets, per-queue rate guarantees, WFQ) is programmed last,
 *    after TRXRDY, so its defaults are live even with no rate limit set.
 */
static void switch_hard_reset(void)
{
	int i;

	/* MAC block: back pressure, plus the RX inter-packet gap floor.
	 * A bare LONG_TXE write leaves CF_RXIPG (bits 3:0) at zero, which is
	 * below the documented 6..12 range; the MAC then drops back-to-back
	 * frames.  Measured with a 6 MiB download: 15 s with CF_RXIPG=0,
	 * 1 s with 5, 6 or 12 - TCP collapses to ~4% of line rate while
	 * pings and small transfers still pass.  SYSCLK/FCDSC follow in l2
	 * init. */
	sys_w32(MACCR, MACCR_LONG_TXE | MACCR_RXIPG);
	for (i = 0; i < 12; i++)	/* MAC group forwarding */
		sys_w32(MGFCR_E0R0 + i * 4, 0);

	/* VLAN, protocol VLAN and ALE controls */
	sys_w32(VCR0, 0);
	sys_w32(VCR1, 0);
	for (i = 0; i < 5; i++)
		sys_w32(PVCR0 + i * 4, 0);
	sys_w32(TEACR, 0);
	sys_w32(FFCR, 0);
	sys_w32(DOT1XPORTCR, 0);
	sys_w32(DOT1XMACCR, 0);
	sys_w32(GVGCR, 0);
	sys_w32(SWTCR0, 0);
	sys_w32(SWTCR1, 0);
	sys_w32(PLITIMR, 0);
	sys_w32(PBVCR0, 0);
	sys_w32(PBVCR0 + 4, 0);
	for (i = 0; i < 30; i++)
		sys_w32(PBVR0_0 + i * 4, 0);
	sys_w32(MSCR, 0);

	/* Port interface enable, then the switch transaction engine */
	for (i = 0; i < 9; i++)
		sys_w32(PCRP0 + i * 4, 1);
	sys_w32(PPMAR, 1);
	sys_w32(SIRR, SIRR_TRXRDY);

	/* Queue/priority decisions and remarking */
	sys_w32(QIDDPCR, (1 << 16) | (1 << 12) | (1 << 8) | (1 << 4) | 1);
	sys_w32(LPTM8021Q, 0);
	for (i = 0; i < 7; i++)
		sys_w32(DSCPCR0 + i * 4, 0);
	sys_w32(RMACR, 0);
	sys_w32(ALECR, 0);

	switch_flow_control_init();

	/* Packet scheduling (PSCR): egress/ingress leaky
	 * buckets, per-queue rate guarantees and the weighted fair queue.
	 * PSCR must not be touched before SIRR.TRXRDY is set: the bus hangs.
	 * The reset values use 64 kbps for APR and 1 KiB for the burst size. */
	sys_w32(ELBPCR, (51 << 8) | 38);
	sys_w32(ELBTTCR, 0x400);
	sys_w32(ILBPCR1, 0);
	sys_w32(ILBPCR2, (0x33 << 8) | 0x26);
	for (i = 0; i < 42; i++)
		sys_w32(P0Q0RGCR + i * 4, (7 << 24) | (0xFF << 16) | 0x3FFF);
	for (i = 0; i < 7; i++) {
		sys_w32(WFQRCRP0 + i * 12 + 0, 0x3FFF);
		sys_w32(WFQRCRP0 + i * 12 + 4, 0);
		sys_w32(WFQRCRP0 + i * 12 + 8, 0);
	}
	for (i = 0; i < 12; i++)	/* priority assignment table */
		sys_w32(PATP0 + i * 4, 0xFE12);

	sys_w32(TMCR, sys_r32(TMCR) & ~ENHSBTESTMODE);

	/* CPU interface: leave loopback/test modes, drain interrupts */
	cpu_w32(CPUICR, cpu_r32(CPUICR) &
		~(BIT(19) | BIT(18) | BIT(17) | BIT(16)));
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
	for (i = 0; i < 3; i++)		/* CPUQDM0-5, two 16-bit regs each */
		cpu_w32(0x030 + i * 4, 0);
	sys_w32(RMCR1P, 0);
	sys_w32(DSCPRM0, 0);
	sys_w32(DSCPRM1, 0);
	sys_w32(RLRC, 0);
}

/* ===== rtl8198c_switch_start ===== */
void rtl8198c_switch_start(void)
{
	/* ACL table entry 0 read — 8198C workaround for "can't receive packet
	 * when lan un-plugged". */
	sys_w32(0xBB0C0000, sys_r32(0xBB0C0000));

	/* Set descriptor format: TX_PKTHDR_8198C_DEF (bits[9:8]=0) + gather mode.
	 * CF_TX_GATHER lets the RISC gather a descriptor chain into one packet,
	 * which is what the TX scatter-gather path relies on. */
	cpu_w32(CPUICR1, cpu_r32(CPUICR1) & ~CF_PKT_HDR_TYPE_MASK);
	cpu_w32(CPUICR1, cpu_r32(CPUICR1) | BIT(6));
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
	cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) & ~(0x3F<<3)); /* clear all RX_DONE_IE */
	cpu_w32(CPUIIMR, RX_DONE_IE_ALL | TX_DONE_IE_ALL | LINK_CHANGE_IE);
	sys_w32(SIRR, SIRR_TRXRDY);
	sys_w32(GIMR, sys_r32(GIMR) | BSP_SW_IE);
}

/* ===== debugfs: live register dump ===== */
static struct dentry *debugfs_root;

static int debugfs_regs_show(struct seq_file *s, void *v)
{
	struct rtl8198c_eth *eth = s->private;
	int i;

	seq_printf(s, "CPUICR:     0x%08X\n", cpu_r32(CPUICR));
	seq_printf(s, "CPUICR1:    0x%08X\n", cpu_r32(CPUICR1));
	seq_printf(s, "CPURPDCR0:  0x%08X\n", cpu_r32(CPURPDCR0));
	seq_printf(s, "CPUTPDCR0:  0x%08X\n", cpu_r32(CPUTPDCR0));
	seq_printf(s, "CPURMDCR0:  0x%08X\n", cpu_r32(CPURMDCR0));
	seq_printf(s, "CPUIIMR:    0x%08X\n", cpu_r32(CPUIIMR));
	seq_printf(s, "CPUIISR:    0x%08X\n", cpu_r32(CPUIISR));
	seq_printf(s, "DMA_CR0:    0x%08X\n", cpu_r32(DMA_CR0));
	seq_printf(s, "DMA_CR1:    0x%08X\n", cpu_r32(DMA_CR1));
	seq_printf(s, "DMA_CR4:    0x%08X\n", cpu_r32(DMA_CR4));
	seq_printf(s, "TXRINGCR:   0x%08X\n", cpu_r32(TXRINGCR));
	seq_printf(s, "GDSR0:      0x%08X\n", sys_r32(GDSR0));
	seq_printf(s, "SIRR:       0x%08X\n", sys_r32(SIRR));
	seq_printf(s, "GIMR:       0x%08X\n", sys_r32(GIMR));
	seq_printf(s, "MSCR:       0x%08X\n", sys_r32(MSCR));
	seq_printf(s, "RMACR:      0x%08X\n", sys_r32(RMACR));
	seq_printf(s, "SWTCR0:     0x%08X\n", sys_r32(SWTCR0));
	seq_printf(s, "FFCR:       0x%08X\n", sys_r32(FFCR));
	seq_printf(s, "VCR0:       0x%08X\n", sys_r32(VCR0));
	seq_printf(s, "MACCR:      0x%08X\n", sys_r32(MACCR));
	seq_printf(s, "CSCR:       0x%08X\n", sys_r32(CSCR));
	seq_printf(s, "QNUMCR:     0x%08X\n", sys_r32(QNUMCR));
	seq_printf(s, "QOSFCR:     0x%08X\n", sys_r32(QOSFCR));
	seq_printf(s, "SBFCR0-2:   0x%08X 0x%08X 0x%08X\n", sys_r32(SBFCR0),
		   sys_r32(SBFCR1), sys_r32(SBFCR2));
	seq_printf(s, "PBFCR0:     0x%08X\n", sys_r32(PBFCR0));
	seq_printf(s, "MACCTRL1:   0x%08X\n", sys_r32(MACCTRL1));
	seq_printf(s, "PCRP0-4:    0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
		   sys_r32(PCRP0), sys_r32(PCRP0 + 4), sys_r32(PCRP0 + 8),
		   sys_r32(PCRP0 + 12), sys_r32(PCRP0 + 16));
	seq_printf(s, "PCRP6:      0x%08X\n", sys_r32(PCRP6));
	seq_printf(s, "PSRP0-4:    0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
		   sys_r32(PSRP0), sys_r32(PSRP0 + 4), sys_r32(PSRP0 + 8),
		   sys_r32(PSRP0 + 12), sys_r32(PSRP0 + 16));
	seq_printf(s, "PVCR0:      0x%08X\n", sys_r32(PVCR0));
	seq_printf(s, "PVCR6:      0x%08X\n", sys_r32(0xBB804A20));
	seq_printf(s, "L2[0,0]:    0x%08X 0x%08X\n", sys_r32(0xBB000000), sys_r32(0xBB000004));
	seq_printf(s, "L2[f0,0]:   0x%08X 0x%08X\n", sys_r32(0xBB007800), sys_r32(0xBB007804));
	seq_printf(s, "VLAN[8]:    0x%08X\n", sys_r32(TBL_VLAN + 8 * 32));
	seq_printf(s, "Ring0:      0x%08X\n", eth->rx_ring ? eth->rx_ring[0] : 0);
	seq_printf(s, "Tring0:     0x%08X\n", eth->tx_ring ? eth->tx_ring[0] : 0);
	seq_printf(s, "Mring0:     0x%08X\n", eth->mbuf_ring ? eth->mbuf_ring[0] : 0);
	seq_puts(s, "rx_head=");
	for (i = 0; i < RX_RINGS; i++)
		seq_printf(s, "%d%s", eth->rx_head[i],
			   i == RX_RINGS - 1 ? "" : "/");
	seq_printf(s, " tx_head=%d/%d tx_tail=%d/%d\n",
		   eth->tx_head[0], eth->tx_head[1], eth->tx_tail[0],
		   eth->tx_tail[1]);

	return 0;
}

static int debugfs_regs_open(struct inode *inode, struct file *file)
{ return single_open(file, debugfs_regs_show, inode->i_private); }

static const struct file_operations debugfs_regs_fops = {
	.owner = THIS_MODULE,
	.open = debugfs_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/*
 * debugfs "reg": raw 32-bit register access for bring-up.
 *
 *   echo 1B060100 > reg; cat reg        -> 0xBB060100 0xXXXXXXXX
 *   echo 1B804428=0x33F > reg           -> write
 *
 * The address may be a physical SoC IO address or a KSEG1 alias.  It is
 * restricted to the SoC IO windows (0x18000000-0x1BFFFFFF) so a typo
 * cannot reach DRAM through the uncached alias.
 */
#define RTL8198C_IO_PHYS	0x18000000U
#define RTL8198C_IO_END		0x1C000000U
#define RTL8198C_KSEG1		0xA0000000U

static ssize_t debugfs_reg_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = file->private_data;
	char buf[32];
	int len;

	if (!eth->reg_addr)
		return 0;

	len = scnprintf(buf, sizeof(buf), "0x%08X 0x%08X\n",
			eth->reg_addr, *(volatile u32 *)eth->reg_addr);

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t debugfs_reg_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = file->private_data;
	unsigned int addr, val = 0;
	long long addr_l, val_l = 0;
	bool have_val = false;
	char buf[32], *sep;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	sep = strchr(buf, '=');
	if (sep) {
		*sep = '\0';
		have_val = true;
	}

	if (kstrtoll(buf, 0, &addr_l))
		return -EINVAL;
	if (have_val && kstrtoll(sep + 1, 0, &val_l))
		return -EINVAL;

	addr = (u32)addr_l;
	val = (u32)val_l;

	if (addr >= RTL8198C_IO_PHYS && addr < RTL8198C_IO_END)
		addr += RTL8198C_KSEG1;
	else if (addr < RTL8198C_IO_PHYS + RTL8198C_KSEG1 ||
		 addr >= RTL8198C_IO_END + RTL8198C_KSEG1)
		return -EINVAL;

	if (addr & 3)
		return -EINVAL;

	eth->reg_addr = addr;
	if (have_val)
		*(volatile u32 *)addr = val;

	return count;
}

static const struct file_operations debugfs_reg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = debugfs_reg_read,
	.write = debugfs_reg_write,
	.llseek = default_llseek,
};

/* debugfs "lag": what the switch can and cannot do for link aggregation.
 * Read only - both registers are reachable through debugfs "reg" and both
 * make traffic disappear when written. */
static int lag_show(struct seq_file *s, void *unused)
{
	seq_puts(s, "Hardware link aggregation: not supported.\n\n");
	seq_puts(s, "The BSP's LAGCR0/LAGHPMR0 block (0xBB804C00) makes frames to\n"
		    "the ports named in the mask disappear, with any hashed-index\n"
		    "mapping, and the 8198C bootloader register map does not list\n"
		    "the block at all (the VLAN block ends at 0x4A98, 802.1X\n"
		    "starts at 0x4B00, TACI at 0x4D00).  Vendor firmware for this\n"
		    "board contains neither /proc/link_aggregation nor any other\n"
		    "reference to it, so there is nothing to verify against.\n\n");
	seq_puts(s, "Linux bonding/team still work in software: the kernel hashes\n"
		    "into each member's egress port and the switch only sees that\n"
		    "port.  Traffic forwarded between other switch ports does not\n"
		    "take part in the bond.\n\n");
	seq_printf(s, "LAGCR0   = 0x%08X\n", sys_r32(LAGCR0));
	seq_printf(s, "LAGHPMR0 = 0x%08X\n", sys_r32(LAGHPMR0));
	return 0;
}

static int lag_open(struct inode *inode, struct file *file)
{ return single_open(file, lag_show, inode->i_private); }
static const struct file_operations lag_fops = {
	.owner = THIS_MODULE, .open = lag_open,
	.read = seq_read, .llseek = seq_lseek, .release = single_release,
};

static void debugfs_init(struct rtl8198c_eth *eth)
{
	if (!debugfs_root) {
		debugfs_root = debugfs_create_dir("rtl8198c-eth", NULL);
		if (IS_ERR_OR_NULL(debugfs_root))
			return;
	}
	/* One switch, one set of knobs: they all act on shared state, so
	 * the directory is not tied to a port netdev. */
	eth->debugfs_dir = debugfs_create_dir("switch", debugfs_root);
	if (IS_ERR_OR_NULL(eth->debugfs_dir))
		return;
	debugfs_create_file("regs", 0400, eth->debugfs_dir, eth, &debugfs_regs_fops);
	debugfs_create_file("reg", 0600, eth->debugfs_dir, eth, &debugfs_reg_fops);
	debugfs_create_file("lag", 0400, eth->debugfs_dir, eth, &lag_fops);

	/* Every other knob is registered by the block that implements it, so
	 * each file owns its own interface. */
	rtl8198c_l2_debugfs_init(eth);
	rtl8198c_l3_debugfs_init(eth);
	rtl8198c_l4_debugfs_init(eth);
	rtl8198c_phy_debugfs_init(eth);
	rtl8198c_qos_debugfs_init(eth);
	rtl8198c_acl_debugfs_init(eth);
	rtl8198c_mcast_debugfs_init(eth);
	rtl8198c_dma_debugfs_init(eth);
}

static void debugfs_cleanup(struct rtl8198c_eth *eth)
{
	debugfs_remove_recursive(eth->debugfs_dir);
}

/* ===== [Platform Driver] probe, remove, module init/exit ===== */

/* ===== probe / remove ===== */
static int eth_probe(struct platform_device *pdev)
{
	struct rtl8198c_port *port;
	struct rtl8198c_eth *eth;
	struct net_device *netdev;
	int irq, ret, i;
	u8 base[ETH_ALEN];
	u8 macs[ETH_PORTS][ETH_ALEN];

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) return irq;
	eth = kzalloc(sizeof(*eth), GFP_KERNEL);
	if (!eth) return -ENOMEM;
	eth->dev = &pdev->dev;
	eth->irq = irq;
	eth->mac_filter_enabled = false;
	eth->green_enabled = true;	/* rtl8198c_switch_gphy_init() applies green */
	spin_lock_init(&eth->tx_lock);

	/* One address per port: the switch has to tell the netdevs apart, and
	 * each address needs its own static FDB entry towards the CPU.  The
	 * device-tree base is a placeholder - the board's factory address is
	 * not read from EEPROM or NAND. */
	if (of_get_mac_address(pdev->dev.of_node, base))
		eth_random_addr(base);
	for (i = 0; i < ETH_PORTS; i++) {
		memcpy(macs[i], base, ETH_ALEN);
		macs[i][5] += i;
	}
	/* One switch, one id: the bridge uses it to tell which ports share a
	 * forwarding domain, so it must not follow a port's MAC changes. */
	memcpy(eth->switch_id, base, ETH_ALEN);

	switch_reset();
	/* Switch core defaults, TRXRDY, PSCR block */
	switch_hard_reset();
	rtl8198c_switch_l2_init(macs);
	/* BSCR comes out of reset as 0, which would read back as "0%":
	 * seed the 100% default.  Storm control stays off. */
	rtl8198c_storm_set_rate(100);
	/* GPHY workarounds + NCTL calibration */
	rtl8198c_switch_gphy_init();
	/* N-way restart AFTER GPHY patches (phy_refine enables gigabit DSP path).
	 * Explicitly set PHY 1000BASE-T FD advertisement — the PCRP NwayAbility
	 * bits control the MAC side but the GPHY reg 9 needs an explicit write. */
	{
		u32 id, v2;
		int pi;
		for (pi = 0; pi < 5; pi++) {
			id = (pi == 0) ? 8 : pi;
			v2 = rtl8198c_mdio_read(id, 9);
			rtl8198c_mdio_write(id, 9, v2 | BIT(9));
			v2 = rtl8198c_mdio_read(id, 0);
			rtl8198c_mdio_write(id, 0, v2 | BIT(9));
		}
	}

	ret = rtl8198c_alloc_rings(eth);
	if (ret) goto err_free;

	/* The netif and VLAN entries are created after the descriptor rings;
	 * keep that order. */
	rtl8198c_netif_vlan_init(eth, macs);
	/* One netif entry per VLAN, then the ACL engine rewrites the same
	 * entries to carry the ingress chain range. */
	rtl8198c_l3_netif_init(eth);
	rtl8198c_acl_init(eth);
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));

	rtl8198c_switch_start();

	dev_info(eth->dev, "rings RX=%px TX=%px CPUICR=0x%08x FFCR=0x%08x\n",
		 eth->rx_ring, eth->tx_ring,
		 cpu_r32(CPUICR), sys_r32(FFCR));

	ret = devm_request_irq(eth->dev, irq, rtl8198c_eth_irq, 0, "rtl8198c-eth", eth);
	if (ret) goto err_free;

	for (i = 0; i < ETH_PORTS; i++) {
		netdev = alloc_etherdev(sizeof(*port));
		if (!netdev) {
			ret = -ENOMEM;
			goto err_netdevs;
		}
		port = netdev_priv(netdev);
		port->sw = eth;
		port->port = i;
		port->vid = i ? VLAN_LAN : VLAN_WAN;
		port->fid = i ? L2_FID_LAN : L2_FID_WAN;
		eth->netdev[i] = netdev;

		/* 64-bit byte counters: dev->stats is 32-bit here and wraps
		 * in under a minute at gigabit rates.  Per-CPU so the RX and
		 * TX paths do not share a cache line. */
		netdev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
		if (!netdev->tstats) {
			ret = -ENOMEM;
			goto err_netdevs;
		}

		SET_NETDEV_DEV(netdev, eth->dev);
		netdev->netdev_ops = &rtl8198c_eth_ops;
		netdev->ethtool_ops = &rtl8198c_ethtool_ops;
		netdev->max_mtu = JUMBO_MTU;
		eth_hw_addr_set(netdev, macs[i]);
		/* Until the port is opened nothing reads its link state, and a
		 * guessed carrier would be wrong for a port with no cable. */
		netif_carrier_off(netdev);

		/* Hardware checksum offload (TX).  rtl8198c_switch_l2_init() enables
		 * CSCR.EN_L3/L4_CHKSUM_REC and rtl8198c_eth_start_xmit() describes each
		 * CHECKSUM_PARTIAL packet to the ASIC; packets the ASIC cannot
		 * parse (VLAN/LLC/PPPoE tagged, IPv6 extension headers, non
		 * TCP/UDP) are finished with skb_checksum_help() in software.
		 *
		 * Not enabled:
		 *
		 * RX checksum verification uses the per-frame ph_flags bits for
		 *   unfragmented IPv4 TCP/UDP (see rtl8198c_eth_poll); CSCR itself still
		 *   does not drop corrupt CPU-bound frames.
		 *
		 * Scatter-gather (NETIF_F_SG) is enabled: the frag loop in
		 *   rtl8198c_eth_start_xmit chains spare mbufs, CF_TX_GATHER is set.
		 *   TSO is not: LSO lives in the new-descriptor path this
		 *   core does not use. */
		netdev->hw_features |= NETIF_F_HW_CSUM | NETIF_F_SG |
				       NETIF_F_HW_VLAN_CTAG_FILTER;
		netdev->features |= NETIF_F_HW_CSUM | NETIF_F_SG |
				    NETIF_F_HW_VLAN_CTAG_FILTER;
	}

	netif_napi_add_weight(eth->netdev[0], &eth->napi, rtl8198c_eth_poll, 64);
	platform_set_drvdata(pdev, eth);

	for (i = 0; i < ETH_PORTS; i++) {
		ret = register_netdev(eth->netdev[i]);
		if (ret) {
			while (i--)
				unregister_netdev(eth->netdev[i]);
			goto err_netdevs;
		}
	}

	debugfs_init(eth);
	rtl8198c_l3_register(eth);
	dev_info(eth->dev, "registered %s..%s (%d ports) IRQ %d\n",
		 eth->netdev[0]->name, eth->netdev[ETH_PORTS - 1]->name,
		 ETH_PORTS, irq);
	return 0;

err_netdevs:
	netif_napi_del(&eth->napi);
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i]) {
			free_percpu(eth->netdev[i]->tstats);
			free_netdev(eth->netdev[i]);
			eth->netdev[i] = NULL;
		}
err_free:
	rtl8198c_free_rings(eth);
	kfree(eth);
	return ret;
}

static void eth_remove(struct platform_device *pdev)
{
	struct rtl8198c_eth *eth = platform_get_drvdata(pdev);
	int i;

	if (!eth) return;
	rtl8198c_l3_unregister(eth);
	debugfs_cleanup(eth);
	for (i = 0; i < ETH_PORTS; i++)
		if (eth->netdev[i]) {
			unregister_netdev(eth->netdev[i]);
			free_percpu(eth->netdev[i]->tstats);
			free_netdev(eth->netdev[i]);
		}
	rtl8198c_free_rings(eth);
	kfree(eth);
}

static const struct of_device_id eth_match[] = {
	{ .compatible = "realtek,rtl819x-eth" }, {},
};
MODULE_DEVICE_TABLE(of, eth_match);

static struct platform_driver eth_driver = {
	.probe = eth_probe, .remove = eth_remove,
	.driver = { .name = "rtl8198c-eth", .of_match_table = eth_match },
};

static int __init eth_init(void)
{
	int ret = register_netdevice_notifier(&rtl8198c_eth_netdev_nb);

	if (ret)
		return ret;
	/* Blocking chain: the bridge defers port attributes to a work item. */
	ret = register_switchdev_blocking_notifier(&rtl8198c_switchdev_nb);
	if (ret) {
		unregister_netdevice_notifier(&rtl8198c_eth_netdev_nb);
		return ret;
	}
	ret = platform_driver_register(&eth_driver);
	if (ret) {
		unregister_switchdev_blocking_notifier(&rtl8198c_switchdev_nb);
		unregister_netdevice_notifier(&rtl8198c_eth_netdev_nb);
	}
	return ret;
}
late_initcall(eth_init);
static void __exit eth_exit(void)
{
	platform_driver_unregister(&eth_driver);
	unregister_switchdev_blocking_notifier(&rtl8198c_switchdev_nb);
	unregister_netdevice_notifier(&rtl8198c_eth_netdev_nb);
}
module_exit(eth_exit);
MODULE_LICENSE("GPL");
