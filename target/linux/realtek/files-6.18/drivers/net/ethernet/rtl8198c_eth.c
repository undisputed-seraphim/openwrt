// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL8198C Switch Core NIC driver.
 *
 * Supports two descriptor formats, selected at compile time:
 * - DESC_FORMAT_LEGACY: pkthdr/mbuf format from bootloader swNic_poll.c
 *   (known working on this hardware)
 * - (default): flat 24-byte format from rtl819x_swNic.c
 *
 * L2/VLAN/PHY init is shared between both formats.
 */
#define DESC_FORMAT_LEGACY

#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/of_net.h>
#include <linux/ethtool.h>
#include <linux/uaccess.h>

#define UNCACHE_MASK		0x20000000

/* ===== System / switch core registers (KSEG1) ===== */
#define SYS_CLK_MAG		0xB8000010
#define CM_ACTIVE_SWCORE	BIT(11)
#define SIRR			0xBB804204
#define SIRR_FULL_RST		BIT(2)
#define SIRR_TRXRDY		BIT(0)
#define GIMR			0xB8003000
#define BSP_SW_IE		BIT(15)

/* ===== CPU Interface registers ===== */
#define CPU_IF_BASE		0xB8010000
#define CPUICR			0x000
#define CPURPDCR0		0x004
#define CPURMDCR0		0x01C
#define CPUTPDCR0		0x020
#define CPUIIMR			0x028
#define CPUIISR			0x02C
#define DMA_CR0			0x03C
#define DMA_CR1			0x040
#define DMA_CR4			0x0A0
#define TXRINGCR		0x078

#define TXCMD			BIT(31)
#define RXCMD			BIT(30)
#define MBUF_2048		(4 << 24)
#define TXFD			BIT(23)
#define DMA_HSB_ADDR_MARK	BIT(16)
#define TX_RING0_EN		BIT(0)
#define TX_RING2_EN		BIT(2)
#define TX_RING3_EN		BIT(3)
#define TX_RING0_TAIL_AWARE	BIT(0)
#define CPUICR1			0x0A4
#define CF_PKT_HDR_TYPE_OFFSET	8
#define CF_PKT_HDR_TYPE_MASK	(3 << 8)
#define TX_PKTHDR_8198C_DEF	(0 << 8)

#define RX_DONE_IP0		BIT(3)
#define TX_DONE_IP0		BIT(1)
#define RX_DONE_IE_ALL		(0x3F << 3)
#define TX_DONE_IE_ALL		(BIT(9)|BIT(10)|BIT(14)|BIT(15))
#define PKTHDR_RUNOUT_IP0	BIT(17)
#define PKTHDR_DESC_RUNOUT_IE_ALL 0x001E0000
#define PKTHDR_DESC_RUNOUT_IP_ALL 0x001E0000

/* ===== Switch core L2 registers ===== */
#define PCRP0			0xBB804104
#define PCRP1			0xBB804108
#define PCRP2			0xBB80410C
#define PCRP3			0xBB804110
#define PCRP4			0xBB804114
#define PCRP6			0xBB80411C
#define MACCR			0xBB804000
#define CSCR			0xBB804048
#define VCR0			0xBB804A00
#define SWTCR0			0xBB804418
#define SWTCR1			0xBB80441C
#define FFCR			0xBB804428
#define ALECR			0xBB80440C
#define PLITIMR			0xBB804420
#define MSCR			0xBB804410
#define RMACR			0xBB804408
#define GDSR0			0xBB806100
#define QNUMCR			0xBB804754
#define PVCR0			0xBB804A08
#define MACCTRL1		0xBB805100

#define MIB_CONTROL		0xBB801000
#define MEMCR			0xBB804234
#define MDCIOCR			0xBB804004
#define MDCIOSR			0xBB804008
#define MDIO_WRITE		BIT(31)
#define MDIO_BUSY		BIT(31)

#define REVR			0xB8000000

/* PCR / MSCR bits */
#define PCR_MacSwReset		BIT(3)
#define PCRP_STP_FORWARDING	(3 << 4)
#define PCRP_STP_MASK		(3 << 4)
#define NWAY_ABILITY_1000MF	BIT(22)
#define NWAY_ABILITY_100MF	BIT(21)
#define NWAY_ABILITY_100MH	BIT(20)
#define NWAY_ABILITY_10MF	BIT(19)
#define NWAY_ABILITY_10MH	BIT(18)
#define NWAY_ABILITY_ALL	(NWAY_ABILITY_1000MF | NWAY_ABILITY_100MF | \
				 NWAY_ABILITY_100MH | NWAY_ABILITY_10MF | \
				 NWAY_ABILITY_10MH)
#define MSCR_EN_L2		BIT(0)
#define MSCR_EN_L3		BIT(1)
#define MSCR_EN_L4		BIT(2)
#define MSCR_EN_OUT_ACL		BIT(3)
#define MSCR_EN_IN_ACL		BIT(4)
#define MSCR_EN_STP		BIT(5)
#define RMACR_MADDR00		BIT(0)
#define PHY_POWER_DOWN		BIT(11)

#define RING_SIZE		64
#define MBUF_BUF_SIZE		2048

#ifdef DESC_FORMAT_LEGACY
/* ===== Legacy pkthdr/mbuf descriptor format (bootloader swNic_poll.c) ===== */

#define BUF_FREE		0x00
#define BUF_USED		0x80
#define BUF_ASICHOLD		0x80
#define BUF_DRIVERHOLD		0xc0

#define DESC_OWNED_BIT		0x0001
#define DESC_RISC_OWNED		0x0000	/* bit0=0: RISC owns it (needs CPU action) */
#define DESC_SWCORE_OWNED	0x0001	/* bit0=1: CPU/SWCORE owns it */
#define DESC_WRAP		0x0002

/* pkthdr: 32-byte packet header, 20 bytes ASIC + 12 bytes driver */
struct pkthdr {
	union {
		struct pkthdr *pkthdr_next;
		struct mbuf *mbuf_first;
	} PKTHDRNXT;
#define ph_nextfree	PKTHDRNXT.pkthdr_next
#define ph_mbuf		PKTHDRNXT.mbuf_first
	u16 ph_len;
	u16 ph_reserved1:1, ph_queueId:3, ph_extPortList:4, ph_reserved2:3,
	    ph_hwFwd:1, ph_isOriginal:1, ph_l2Trans:1, ph_srcExtPortNum:2;
	u16 ph_type:3, ph_vlanTagged:1, ph_LLCTagged:1, ph_pppeTagged:1,
	    ph_pppoeIdx:3, ph_linkID:7;
	u16 ph_reason;
	u16 ph_flags;
#define PKTHDR_FREE	(BUF_FREE << 8)
#define PKTHDR_USED	(BUF_USED << 8)
#define PKT_INCOMING	0x1000
#define PKT_OUTGOING	0x0800
#define PKTHDR_TCP	5
#define PKTHDR_UDP	6
#define CSUM_TCPUDP	0x0001	/* Outgoing: TCP/UDP cksum offload to ASIC */
#define CSUM_IP		0x0002	/* Outgoing: IP header cksum offload to ASIC */
	u8  ph_orgtos;
	u8  ph_portlist;
	u16 ph_vlanId_resv:1, ph_txPriority:3, ph_vlanId:12;
	u16 ph_flags2;
	u8  ph_resv_tx:4, ph_ipIpv6:1, ph_ipIpv4:1, ph_ipIpv4_1st:1;
	u16 ph_ipIpv6HdrLen;
	u8  ph_pad[8];	/* padding to make pkthdr exactly 32 bytes */
};

/* mbuf: 32-byte memory buffer descriptor */
struct mbuf {
	struct mbuf	*m_next;
	struct pkthdr	*m_pkthdr;
	u16		 m_len;
	u8		 m_flags;
	u8		 m_reserved2;
#define MBUF_FREE	BUF_FREE
#define MBUF_USED	BUF_USED
#define MBUF_EXT	0x10
#define MBUF_PKTHDR	0x08
#define MBUF_EOR	0x04
	u8		*m_data;
	u8		*m_extbuf;
	u16		 m_extsize;
	u8		 m_reserved[2];
} __packed;

struct rtl8198c_eth {
	struct net_device *netdev;
	struct device *dev;
	int irq;

	/* legacy descriptor rings — arrays of u32 pointers with OWN/WRAP bits */
	u32	*rx_ring;	/* ring[0] only */
	u32	*tx_ring;
	u32	*mbuf_ring;
	void	*rx_ring_k0, *tx_ring_k0, *mbuf_ring_k0;

	/* pools */
	struct pkthdr	*pktHdr_pool;
	struct mbuf	*mbuf_pool;
	u8		*cluster_pool;

	struct sk_buff *rx_skb[RING_SIZE];
	struct sk_buff *tx_skb[RING_SIZE];
	int rx_head, tx_head, tx_tail;

	struct napi_struct napi;
	struct dentry *debugfs_dir;
	u8	mac_filter[ETH_ALEN];
	bool	mac_filter_enabled;
	int	mac_filter_entry;
};

#else /* !DESC_FORMAT_LEGACY — flat 24-byte format */

#define DESC_OWN		1
#define DESC_WRAP		2

struct sw_desc {
	u32 opts1;
	u32 addr;
	u32 opts2;
	u32 opts3;
	u32 opts4;
	u32 opts5;
};

struct rtl8198c_eth {
	struct net_device *netdev;
	struct device *dev;
	int irq;

	struct sw_desc	*rx_ring;
	struct sw_desc	*tx_ring;
	void		*rx_ring_k0;
	void		*tx_ring_k0;
	u32		rx_phys, tx_phys;

	int	rx_head, tx_head, tx_tail;
	struct sk_buff *rx_skb[RING_SIZE];
	struct sk_buff *tx_skb[RING_SIZE];
	struct napi_struct napi;
	struct dentry *debugfs_dir;
};
#endif /* DESC_FORMAT_LEGACY */

/* ===== [BSP: AsicDriver] Hardware abstraction — register access, MDIO, GPHY, reset, table ops ===== */

/* ===== Register access (common) ===== */
static inline void cpu_w32(u32 reg, u32 val)
{ *(volatile u32 *)(CPU_IF_BASE + reg) = val; }
static inline u32 cpu_r32(u32 reg)
{ return *(volatile u32 *)(CPU_IF_BASE + reg); }
static inline void sys_w32(u32 addr, u32 val)
{ *(volatile u32 *)(addr) = val; }
static inline u32 sys_r32(u32 addr)
{ return *(volatile u32 *)(addr); }
static void *kseg1(void *k0) { return (void *)((u32)k0 | UNCACHE_MASK); }
#ifndef DESC_FORMAT_LEGACY
static u32 kseg1_phys(void *kseg1_addr) { return (u32)kseg1_addr & 0x1FFFFFFF; }
static void *phys_ptr(u32 phys) { return (void *)(phys | UNCACHE_MASK | 0x80000000); }
#endif

static void cache_wb(void *addr, int size)
{ dma_cache_wback((unsigned long)addr, size); }
#ifndef DESC_FORMAT_LEGACY
static void cache_inv(void *addr, int size)
{ dma_cache_inv((unsigned long)addr, size); }
#endif

static void *uncached_alloc(size_t size, u32 *phys, void **k0_out)
{
	void *k0 = kmalloc(size, GFP_KERNEL);
	if (!k0) return NULL;
	memset(k0, 0, size);
	if (phys) *phys = (u32)(unsigned long)k0 & 0x1FFFFFFF;
	if (k0_out) *k0_out = k0;
	return kseg1(k0);
}

/* ===== Switch core reset (bootloader: clock gate only, no FULL_RST) ===== */
static void switch_reset(void)
{
	/* Bootloader FullAndSemiReset: clock gate toggle only
	 * (no FULL_RST — bootloader configures without it) */
	sys_w32(SYS_CLK_MAG, sys_r32(SYS_CLK_MAG) & ~CM_ACTIVE_SWCORE);
	mdelay(300);
	sys_w32(SYS_CLK_MAG, sys_r32(SYS_CLK_MAG) | CM_ACTIVE_SWCORE);
	mdelay(50);
}

/* ===== switch_start ===== */
static void switch_start(void)
{
	/* ACL table entry 0 read — 8198C workaround to fix "can't receive packet when
	 * lan un-plugged" (BSP rtl865x_start()). */
	sys_w32(0xBB0C0000, sys_r32(0xBB0C0000));

	/* Set descriptor format: TX_PKTHDR_8198C_DEF (bits[9:8]=0) + gather mode.
	 * CF_TX_GATHER lets the NIC combine multiple descriptors into one packet.
	 * Needed for NETIF_F_SG — currently blocked (see eth_start_xmit). */
	cpu_w32(CPUICR1, cpu_r32(CPUICR1) & ~CF_PKT_HDR_TYPE_MASK);
	cpu_w32(CPUICR1, cpu_r32(CPUICR1) | BIT(6));
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
	cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) & ~(0x3F<<3)); /* clear all RX_DONE_IE */
	cpu_w32(CPUIIMR, RX_DONE_IE_ALL | TX_DONE_IE_ALL);
	sys_w32(SIRR, SIRR_TRXRDY);
	sys_w32(GIMR, sys_r32(GIMR) | BSP_SW_IE);
}

/* ===== MDIO helpers ===== */
static void mdio_write(u32 phy, u32 reg, u32 val)
{
	sys_w32(MDCIOCR, MDIO_WRITE | (phy << 24) | (reg << 16) | val);
	while (sys_r32(MDCIOSR) & MDIO_BUSY);
}
static u32 mdio_read(u32 phy, u32 reg)
{
	sys_w32(MDCIOCR, (phy << 24) | (reg << 16));
	while (sys_r32(MDCIOSR) & MDIO_BUSY);
	return sys_r32(MDCIOSR) & 0xffff;
}
static void gphy_page(u32 phy, u32 page) { mdio_write(phy, 31, page); }

static void gphy_wb(u32 page, u32 reg, u32 mask, u32 val)
{
	int i;
	for (i = 0; i < 5; i++) {
		u32 id = (i == 0) ? 8 : i;
		gphy_page(id, page);
		if (mask) {
			u32 v = mdio_read(id, reg);
			mdio_write(id, reg, (v & mask) | val);
		} else {
			mdio_write(id, reg, val);
		}
		gphy_page(id, 0);
	}
}
static void gphy_wb_port(u32 phy, u32 page, u32 reg, u32 mask, u32 val)
{
	u32 id = (phy == 0) ? 8 : phy;
	gphy_page(id, page);
	if (mask) {
		u32 v = mdio_read(id, reg);
		mdio_write(id, reg, (v & mask) | val);
	} else {
		mdio_write(id, reg, val);
	}
	gphy_page(id, 0);
}

static void gphy_clear_irq(u32 page, u32 reg)
{
	int i;
	for (i = 0; i < 5; i++) {
		u32 id = (i == 0) ? 8 : i;
		gphy_page(id, page);
		mdio_read(id, reg);
		gphy_page(id, 0);
	}
}
static void sram98c_write(u32 addr, u32 val)
{
	int i;
	for (i = 0; i < 5; i++) {
		u32 id = (i == 0) ? 8 : i;
		gphy_page(id, 0xa43);
		mdio_write(id, 27, addr);
		mdio_write(id, 28, val);
		gphy_page(id, 0);
	}
}

static const u16 phy_98c_para[] = {
	0xB820, 0x0290, 0xA012, 0x0000, 0xA014, 0x2c04,
	0, 0x2c12, 0, 0x2c14, 0, 0x2c14, 0, 0x8620,
	0, 0xa480, 0, 0x609f, 0, 0x3084, 0, 0x58ae,
	0, 0x2c06, 0, 0xd710, 0, 0x6096, 0, 0xd71e,
	0, 0x7fa4, 0, 0x28ae, 0, 0x8480, 0, 0xa101,
	0, 0x2a65, 0, 0x8104, 0, 0x0800, 0xA01A, 0x0000,
	0xA006, 0x0fff, 0xA004, 0x0fff, 0xA002, 0x05e9,
	0xA000, 0x3a5a, 0xB820, 0x0210,
};

static int phy_refine(void)
{
	int i, j;
	for (i = 0; i < 5; i++) {
		u32 pid = (i == 0) ? 8 : i;
		int timeout;

		gphy_wb_port(i, 0xb82, 16, ~(1 << 4), 1 << 4);
		timeout = 1000;
		gphy_page(pid, 0xb80);
		while (timeout--) {
			if (mdio_read(pid, 16) & BIT(6))
				break;
		}
		gphy_page(pid, 0);
		if (timeout <= 0) {
			pr_warn("phy_refine port %d: request timeout\n", i);
			gphy_page(pid, 0);
			continue;
		}

		mdio_write(pid, 27, 0x8146);
		mdio_write(pid, 28, 0x4800);
		mdio_write(pid, 27, 0xb82e);
		mdio_write(pid, 28, 0x0001);

		for (j = 0; j < ARRAY_SIZE(phy_98c_para); j += 2)
			mdio_write(pid, phy_98c_para[j], phy_98c_para[j + 1]);

		mdio_write(pid, 27, 0x0000);
		mdio_write(pid, 28, 0x0000);
		gphy_wb_port(i, 0xb82, 23, 0, 0x0000);
		mdio_write(pid, 27, 0x8146);
		mdio_write(pid, 28, 0x0000);

		gphy_wb_port(i, 0xb82, 16, ~(1 << 4), 0 << 4);
		timeout = 1000;
		gphy_page(pid, 0xb80);
		while (timeout--) {
			if (!(mdio_read(pid, 16) & BIT(6)))
				break;
		}
		gphy_page(pid, 0);
		if (timeout <= 0) {
			pr_warn("phy_refine port %d: release timeout\n", i);
			gphy_page(pid, 0);
		}
	}
	return 0;
}

/* BOOTLOADER: GPHY workarounds */
static void switch_gphy_init(void)
{
	int i;
	for (i = 0; i < 5; i++)
		sys_w32(PCRP0 + i*4, sys_r32(PCRP0 + i*4) | BIT(25));
	gphy_wb(0xa42, 18, 0, 0);
	gphy_clear_irq(0xa42, 29);
	gphy_wb(0xBCD, 21, 0, 0x2222);
	gphy_wb(0, 27, 0, 0x8277);
	gphy_wb(0, 28, 0xFFFF - 0xFF00, 0x02 << 8);
	gphy_wb(0, 27, 0, 0x8101);
	gphy_wb(0, 28, 0, 0x4000);
	sram98c_write(0x809a, 0x89); sram98c_write(0x809b, 0x11);
	sram98c_write(0x80a3, 0x92); sram98c_write(0x80a4, 0x33);
	sram98c_write(0x80a0, 0x00); sram98c_write(0x8088, 0x89);
	sram98c_write(0x8089, 0x11); sram98c_write(0x808e, 0x00);
	phy_refine();
	gphy_wb(0xa44, 17, ~(1 << 2), 1 << 2);
	for (i = 0; i < 5; i++)
		sys_w32(PCRP0 + i*4, sys_r32(PCRP0 + i*4) & ~BIT(25));
}
/* Block M: Table & counter clear */
static void l2_table_clear(void)
{
	sys_w32(MEMCR, 0);
	sys_w32(MEMCR, 0x7F);	/* bootloader: MEMCR=0x7F */
	while ((sys_r32(MEMCR) & ((1 << 10) | (1 << 13))) != ((1 << 10) | (1 << 13)));
	sys_w32(MIB_CONTROL, 0x0007FFFF);
}
/* ===== [BSP: l2Driver] L2 switching — FDB, VLAN, STP, QoS, netif table ===== */

/* TACI table write helper — TLU stop, fill TCRs, set TAA, issue CMD, TLU start.
 * CMD_BITS: 0x9 = CMD_FORCE, 0x3 = CMD_ADD, 0x1 = CMD_START (read). */
static void taci_write_entry(u32 taa, const u32 e[8], u32 cmd_bits)
{
	u32 sw;
	int i;
	sw = sys_r32(SWTCR0);
	sys_w32(SWTCR0, sw | BIT(18));
	while (!(sys_r32(SWTCR0) & BIT(19)));
	while (sys_r32(0xBB804D00) & 1);
	for (i = 0; i < 8; i++)
		sys_w32(0xBB804D20 + i * 4, e[i]);
	sys_w32(0xBB804D08, taa);
	sys_w32(0xBB804D00, cmd_bits);
	while (sys_r32(0xBB804D00) & 1);
	sys_w32(SWTCR0, sw & ~BIT(18));
}

/* L2 FDB static entries via TACI with KSEG1 addresses */
static void l2_fdb_init(const u8 *cpu_mac)
{
	/* Entry 1: Broadcast ff:ff:ff:ff:ff:ff -> TRAPCPU, memberPorts 0-4 */
	{
		const u32 e[8] = {
			0xFFFFFFFF,
			0xFF7C7840,
			0, 0, 0, 0, 0, 0
		};
		taci_write_entry(0xBB000000, e, BIT(0) | BIT(3)); /* CMD_FORCE */
	}

	/* Entry 2: CPU MAC -> TRAPCPU, memberPorts 0-4.
	 * Uses the actual board MAC from device tree.
	 * CMD_FORCE at a fixed entry (row 240 = entry 960) avoids
	 * hash collision with the broadcast entry at row 0. */
	{
		u32 e[8] = {0};
		e[0] = ((u32)cpu_mac[3] << 24) | ((u32)cpu_mac[4] << 16) |
		       ((u32)cpu_mac[1] <<  8) |  (u32)cpu_mac[2];
		e[1] = ((u32)cpu_mac[0] << 24) | (0x1F << 18) |
			BIT(14) | BIT(13); /* toCPU=1, isStatic=1, all ports */
		taci_write_entry(0xBB007800, e, BIT(0) | BIT(3)); /* CMD_FORCE */
	}
}
/* ===== L2 init (matches rtl865x_initAsicL2 8198C path) ===== */
static void switch_l2_init(const u8 *mac)
{
	u32 v;
	int i;

	/* MSCR: disable engines, then clear STP */
	v = sys_r32(MSCR);
	v &= ~(MSCR_EN_L2|MSCR_EN_L3|MSCR_EN_L4|MSCR_EN_OUT_ACL|MSCR_EN_IN_ACL);
	sys_w32(MSCR, v);
	v = sys_r32(ALECR);
	v |= BIT(29);
	sys_w32(ALECR, v);
	/* BLOCKED: CSCR checksum offload (0xBB804048).
	 * BSP rtl865x_initAsicL2 writes:
	 *   CSCR &= ~(BIT(0)|BIT(1)|BIT(2));  // disallow L2/L3/L4 cksum errors
	 *   CSCR |= BIT(4)|BIT(5);             // EnL3ChkCal|EnL4ChkCal
	 * Any write to this register kills all Ethernet on this board.
	 * Likely an init-sequence dependency we haven't replicated. */
	v = sys_r32(MSCR);
	v &= ~MSCR_EN_STP;
	sys_w32(MSCR, v);
	v = sys_r32(RMACR);
	v &= ~RMACR_MADDR00;
	sys_w32(RMACR, v);

/* SWTCR1: enable NAT/Frag/L4 */
	v = sys_r32(SWTCR1);
	v |= BIT(10)|BIT(11)|BIT(13);
	sys_w32(SWTCR1, v);

	l2_table_clear();

	/* FFCR: clear both TOCPU bits so unknown unicast and multicast
	 * are flooded to all ports instead of trapped to CPU only.
	 * The broadcast FDB entry (l2_fdb_init) ensures broadcast
	 * frames also reach the CPU.
	 *
	 * BLOCKED: bridge / promiscuous / allmulticast support.
	 * A proper ndo_set_rx_mode would toggle these bits at runtime:
	 *   if (dev->flags & IFF_PROMISC)
	 *       FFCR |= BIT(0)|BIT(1);     // trap unknown to CPU
	 *   else if (dev->flags & IFF_ALLMULTI)
	 *       FFCR |= BIT(1);            // trap unknown multicast only
	 *   else
	 *       FFCR &= ~(BIT(0)|BIT(1)); // current default
	 * The BSP's re865x_set_rx_mode() is a stub ("Not yet implemented").
	 * Must be written from scratch — see net_device_ops below. */
	v = sys_r32(FFCR);
	v &= ~(BIT(0) | BIT(1));
	sys_w32(FFCR, v);

	/* PCRP ports 0-4: MacSwReset cycle + PHY ID + EnablePHYIf */
	for (i = 0; i < 5; i++) {
		u32 phy_id = i;
		v = sys_r32(PCRP0 + i * 4);
		v &= ~(PCR_MacSwReset | (0x1F << 26));
		sys_w32(PCRP0 + i * 4, v);
		v |= (phy_id << 26) | BIT(0) | PCR_MacSwReset;
		sys_w32(PCRP0 + i * 4, v);
	}

	/* PCRP6 (CPU port): STP forwarding + EnablePHYIf */
	sys_w32(PCRP6, (3 << 4) | BIT(0));

	/* All ports (0-5 + CPU=6): STP forwarding state */
	for (i = 0; i < 6; i++) {
		v = sys_r32(PCRP0 + i * 4);
		v = (v & ~PCRP_STP_MASK) | PCRP_STP_FORWARDING;
		sys_w32(PCRP0 + i * 4, v);
	}

	/* Advertise all speeds including 1000M (PCRP NwayAbility bits 22:18).
	 * The BSP GPHY recovery path sets all five capabilities on ports 0-4. */
	for (i = 0; i < 5; i++) {
		v = sys_r32(PCRP0 + i * 4);
		v = (v & ~(0x1F << 18)) | NWAY_ABILITY_ALL;
		sys_w32(PCRP0 + i * 4, v);
	}

	/* Output queue: 1 queue per port */
	for (i = 0; i < 6; i++) {
		v = sys_r32(QNUMCR);
		v &= ~(0x7 << (3 * i));
		sys_w32(QNUMCR, v);
		/* Bandwidth control: include preamble + IFG in rate measurement.
		 * BSP sets QOSFCR BC_withPIFG for accurate egress rate limiting. */
		sys_w32(0xBB804700, sys_r32(0xBB804700) | BIT(0));

		/* Per-port VLAN / DSCP remarking: use DSCP remarking table
		 * with original priority.  BSP programs V4VLDSCPCR0-8 for
		 * ports 0-6 plus two extension ports. */
		for (i = 0; i <= 8; i++)
			sys_w32(0xBB804464 + i * 4,
				(2 << 28) | (0 << 26)); /* CF_IPM4DSCP_ACT_REMARK | CF_IPM4PRI_ACT_ORIG */

		/* Clear per-port DSCP priority control registers (ports 0-6).
		 * BSP writes zero to start clean. */
		for (i = 0; i <= 6; i++)
			sys_w32(0xBB804734 + i * 4, 0);
	}

	/* MACCTRL1: lx_clk select (BSP rtl865x_initAsicL2) */
	sys_w32(MACCTRL1, sys_r32(MACCTRL1) | BIT(0));
	sys_w32(PVCR0, 8);
	sys_w32(0xBB804A0C, 8);
	sys_w32(0xBB804A10, 8);
	sys_w32(0xBB804A14, 8);
	sys_w32(0xBB804A18, 8);
	sys_w32(0xBB804A20, 8);

	l2_fdb_init(mac);

	/* Enable 802.3x flow control (pause frames).
	 * The bootloader does NOT set this, but the datasheet lists it
	 * as a supported feature and the BSP has INF_PAUSE defined. */
	sys_w32(MACCR, sys_r32(MACCR) | BIT(14));

	/* System clock → 100MHz (BSP: 2'b01 = lx_clk) */
	v = sys_r32(MACCR);
	v = (v & ~(0x3 << 12)) | (0x1 << 12);
	sys_w32(MACCR, v);

	/* Flow control DSC tolerance → 48 pages (default 24).
	 * BSP rtl865x_initAsicL2 prevents drops when pause is triggered. */
	v = sys_r32(MACCR);
	v = (v & ~(0x7f << 4)) | (0x30 << 4);
	sys_w32(MACCR, v);

	/* Re-enable L2 forwarding engine */
	/* Bootloader sets MSCR = EN_L2 only (0x01) */
	sys_w32(MSCR, sys_r32(MSCR) | MSCR_EN_L2 | MSCR_EN_L3);

	/* 8198C-specific: PIN_MUX_SEL3, IPV6CR1 */
	sys_w32(0xB800002C, (sys_r32(0xB800002C) & ~0x7FFF) | 0x36DB);
	v = sys_r32(0xB800000C);
	v = (v & ~0x3) | 0x3;
	sys_w32(0xB800000C, v);

	/* Per-port LED indicators in direct mode.
	 * BSP led_init() sets LEDMODE_DIRECT and LED blink scale.
	 * Hardware shows link/speed/duplex on the RJ45 LEDs. */
	sys_w32(0xBB804300, (2 << 20)); /* LEDCR0 = LEDMODE_DIRECT */
	sys_w32(0xBB804314, sys_r32(0xBB804314) | (7 << 0)); /* DIRECTLCR, scale=7 */

	/* PPPoE auto-encapsulation / decapsulation.
	 * The switch hardware can parse and encapsulate/decapsulate
	 * PPPoE headers.  Enable this in the ACL engine. */
	sys_w32(ALECR, sys_r32(ALECR) | BIT(18)); /* EN_PPPOE */
}

/* Netif table + VLAN creation (bootloader: after swNic_init).
 * Also includes runtime L2 debugfs handlers (MAC filter, VLAN egress)
 * since they operate on the same hardware tables. */
static void netif_vlan_init(void)
{
	const u32 vlantbl[8] = { 0x7E3F, 0, 0, 0, 0, 0, 0, 0 };
	const u32 nif_e[8] = {
		0xD757C011, 0x00C0C978,
		0x9C000000, 0x000000BB,
		0, 0, 0, 0
	};

	taci_write_entry(0xBB040000, nif_e, BIT(0) | BIT(3)); /* CMD_FORCE */
	taci_write_entry(0xBB050000 + 8*32, vlantbl, BIT(0) | BIT(1)); /* CMD_ADD */

	/* PORT_BASED decision policy + PLITIMR + EnUkVIDtoCPU */
	sys_w32(SWTCR0, (sys_r32(SWTCR0) & ~(3<<16)) | (1<<16) | BIT(15));
	sys_w32(PLITIMR, 0);
}

static void mac_filter_apply(struct rtl8198c_eth *eth)
{
	u32 e[8] = {0};

	if (eth->mac_filter_enabled) {
		u8 *m = eth->mac_filter;
		e[0] = (m[3] << 24) | (m[4] << 16) | (m[1] << 8) | m[2];
		e[1] = (m[0] << 24) | BIT(13);
	}
	taci_write_entry(0xBB000000, e, BIT(0) | BIT(1)); /* CMD_ADD */
}

static int vlan_egress_show(struct seq_file *s, void *v)
{
	u32 w0 = sys_r32(0xBB050100);
	u32 untag = (w0 >> 9) & 0x3F;
	int i;
	seq_printf(s, "VLAN[8] word0=0x%08X\n", w0);
	seq_printf(s, "Ports 0-5 egress: %s\n",
		   untag == 0x3F ? "all untagged" :
		   untag == 0 ? "all tagged" : "mixed");
	seq_puts(s, "  ");
	for (i = 0; i < 6; i++)
		seq_printf(s, "%d:%s ", i, (untag & BIT(i)) ? "untag" : "tagged");
	seq_puts(s, "\n");
	seq_puts(s, "\nWrite hex word0 value to modify (e.g. 0x7E3F)\n");
	return 0;
}

static ssize_t vlan_egress_write(struct file *filp, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[16];
	ssize_t len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	u32 val;
	u32 vlantbl[8] = {0};

	if (len <= 0) return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n') kbuf[len - 1] = '\0';
	if (sscanf(kbuf, "%x", &val) != 1) return -EINVAL;

	vlantbl[0] = val & 0xFFFFFFFF;
	taci_write_entry(0xBB050100, vlantbl, BIT(0) | BIT(1));
	dev_info(eth->dev, "vlan_egress: set word0=0x%08X\n", val);
	return len;
}

static ssize_t mac_filter_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[32];
	ssize_t len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0) return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n') kbuf[len - 1] = '\0';
	if (strcmp(kbuf, "0") == 0 || strcmp(kbuf, "clear") == 0) {
		if (!eth->mac_filter_enabled) return len;
		eth->mac_filter_enabled = false;
		mac_filter_apply(eth);
		dev_info(eth->dev, "mac_filter: cleared\n");
	} else {
		u8 m[6];
		if (sscanf(kbuf, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
			return -EINVAL;
		memcpy(eth->mac_filter, m, ETH_ALEN);
		eth->mac_filter_enabled = true;
		mac_filter_apply(eth);
		dev_info(eth->dev, "mac_filter: drop %pM\n", m);
	}
	return len;
}

static int mac_filter_show(struct seq_file *s, void *v)
{
	struct rtl8198c_eth *eth = s->private;
	if (eth->mac_filter_enabled)
		seq_printf(s, "%pM (drop active)\n", eth->mac_filter);
	else
		seq_puts(s, "inactive (no filter)\n");
	return 0;
}
static int mac_filter_open(struct inode *inode, struct file *file)
{ return single_open(file, mac_filter_show, inode->i_private); }
static const struct file_operations mac_filter_fops = {
	.owner = THIS_MODULE, .open = mac_filter_open,
	.read = seq_read, .write = mac_filter_write,
	.llseek = seq_lseek, .release = single_release,
};

static int vlan_egress_open(struct inode *inode, struct file *file)
{ return single_open(file, vlan_egress_show, inode->i_private); }
static const struct file_operations vlan_egress_fops = {
	.owner = THIS_MODULE, .open = vlan_egress_open,
	.read = seq_read, .write = vlan_egress_write,
	.llseek = seq_lseek, .release = single_release,
};

/* ================================================================
 * Legacy pkthdr/mbuf descriptor engine (bootloader swNic_poll.c)
 * ================================================================ */
#ifdef DESC_FORMAT_LEGACY

static int alloc_rings(struct rtl8198c_eth *eth)
{
	int i;
	struct pkthdr *pkt;
	struct mbuf *mb;
	u8 *cluster;
	int total_pkthdr, total_mbuf;
	u32 pkthdr_pool_sz, mbuf_pool_sz, cluster_pool_sz;

	/* Ring arrays: u32 pointer arrays for rx, tx, mbuf */
	eth->rx_ring = uncached_alloc(RING_SIZE * sizeof(u32), NULL, &eth->rx_ring_k0);
	if (!eth->rx_ring) return -ENOMEM;
	eth->tx_ring = uncached_alloc(RING_SIZE * sizeof(u32), NULL, &eth->tx_ring_k0);
	if (!eth->tx_ring) { kfree(eth->rx_ring_k0); return -ENOMEM; }
	eth->mbuf_ring = uncached_alloc(RING_SIZE * sizeof(u32), NULL, &eth->mbuf_ring_k0);
	if (!eth->mbuf_ring) { kfree(eth->rx_ring_k0); kfree(eth->tx_ring_k0); return -ENOMEM; }

	/* Pools: pkthdr + mbuf for both RX and TX (2 * RING_SIZE total) */
	total_pkthdr = RING_SIZE * 2;
	total_mbuf   = RING_SIZE * 2;
	pkthdr_pool_sz  = total_pkthdr * sizeof(struct pkthdr);
	mbuf_pool_sz    = total_mbuf * sizeof(struct mbuf);
	cluster_pool_sz = RING_SIZE * MBUF_BUF_SIZE + 8 - 1;

	eth->pktHdr_pool = kmalloc(pkthdr_pool_sz, GFP_KERNEL);
	if (!eth->pktHdr_pool) goto err;
	memset(eth->pktHdr_pool, 0, pkthdr_pool_sz);

	eth->mbuf_pool = kmalloc(mbuf_pool_sz, GFP_KERNEL);
	if (!eth->mbuf_pool) goto err;
	memset(eth->mbuf_pool, 0, mbuf_pool_sz);

	eth->cluster_pool = kmalloc(cluster_pool_sz, GFP_KERNEL);
	if (!eth->cluster_pool) goto err;
	memset(eth->cluster_pool, 0, cluster_pool_sz);
	cluster = (u8 *)(((u32)eth->cluster_pool + 8 - 1) & ~(8 - 1));

	/* Init TX pkthdr entries — all pointers converted to KSEG1 (uncached)
	 * so the RISC can read/write them coherently. */
	pkt = (struct pkthdr *)kseg1(eth->pktHdr_pool);
	mb  = (struct mbuf *)kseg1(eth->mbuf_pool);
	for (i = 0; i < RING_SIZE; i++) {
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
	eth->tx_ring[RING_SIZE - 1] |= DESC_WRAP;

	/* Init RX pkthdr + mbuf entries */
	for (i = 0; i < RING_SIZE; i++) {
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
		mb->m_data = NULL;
		mb->m_extsize = MBUF_BUF_SIZE;
		/* No +2 offset — alignment correction is done in eth_poll via skb_reserve.
		 * Keeping cluster 4-byte aligned here avoids a slow MIPS memcpy path. */
		mb->m_data = mb->m_extbuf = cluster;
		cluster += MBUF_BUF_SIZE;
		eth->rx_ring[i] = (u32)pkt | DESC_SWCORE_OWNED;
		eth->mbuf_ring[i] = (u32)mb | DESC_SWCORE_OWNED;
		pkt++;
		mb++;
	}
	eth->rx_ring[RING_SIZE - 1] |= DESC_WRAP;
	eth->mbuf_ring[RING_SIZE - 1] |= DESC_WRAP;

	/* skb arrays for TX completion tracking */
	for (i = 0; i < RING_SIZE; i++) {
		eth->rx_skb[i] = NULL;
		eth->tx_skb[i] = NULL;
	}

	/* Program ring base registers at system (KSEG1) addresses */
	cpu_w32(CPURPDCR0, (u32)eth->rx_ring);
	cpu_w32(CPUTPDCR0, (u32)eth->tx_ring);
	cpu_w32(CPURMDCR0, (u32)eth->mbuf_ring);
	/* Match BSP: BUSBURST_32WORDS, DMA_CR1/CR4, TXRINGCR */
	cpu_w32(CPUICR, BIT(31) | BIT(30) | BIT(3) | MBUF_2048);
	cpu_w32(DMA_CR1, (RING_SIZE - 1) * sizeof(u32));
	cpu_w32(DMA_CR4, cpu_r32(DMA_CR4) | TX_RING0_TAIL_AWARE);
	cpu_w32(TXRINGCR, TX_RING0_EN); /* only ring0, matching BSP state machine */
	cpu_w32(DMA_CR0, cpu_r32(DMA_CR0) | DMA_HSB_ADDR_MARK);

	eth->rx_head = 0;
	eth->tx_head = 0;
	eth->tx_tail = 0;

	return 0;
err:
	kfree(eth->rx_ring_k0);
	kfree(eth->tx_ring_k0);
	kfree(eth->mbuf_ring_k0);
	kfree(eth->pktHdr_pool);
	kfree(eth->mbuf_pool);
	kfree(eth->cluster_pool);
	return -ENOMEM;
}

static void free_rings(struct rtl8198c_eth *eth)
{
	kfree(eth->rx_ring_k0);
	kfree(eth->tx_ring_k0);
	kfree(eth->mbuf_ring_k0);
	kfree(eth->pktHdr_pool);
	kfree(eth->mbuf_pool);
	kfree(eth->cluster_pool);
}

static int refill_rx(struct rtl8198c_eth *eth)
{
	/* Legacy format pre-allocates skbs in pkthdr/mbuf pools during init.
	 * No refill needed — the ring pools are static. */
	return 0;
}

static void free_rx_skbs(struct rtl8198c_eth *eth)
{
	/* Static pools, nothing to free */
}

/* TX using legacy pkthdr/mbuf format */
static netdev_tx_t eth_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl8198c_eth *eth = netdev_priv(dev);
	int idx = eth->tx_head;
	int next = (idx + 1) & (RING_SIZE - 1);
	struct pkthdr *ph;
	void *data_k1;
	u32 len = skb->len;

	/* Check ring full: next entry must not be the done index (tx_tail) */
	if (next == eth->tx_tail) {
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	if (len < ETH_ZLEN) len = ETH_ZLEN;

	ph = (struct pkthdr *)(*(volatile u32 *)&eth->tx_ring[idx] &
				~(DESC_OWNED_BIT | DESC_WRAP));
	if (!ph->ph_mbuf) {
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}
	data_k1 = kseg1(skb->data);
	ph->ph_mbuf->m_extbuf = data_k1;
	ph->ph_mbuf->m_data   = data_k1;
	ph->ph_len = len + 4;
	ph->ph_mbuf->m_len = ph->ph_len;
	ph->ph_mbuf->m_extsize = ph->ph_len;
	ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		struct iphdr *iph = ip_hdr(skb);
		ph->ph_flags |= CSUM_TCPUDP | CSUM_IP;
		if (skb->protocol == htons(ETH_P_IP)) {
			ph->ph_ipIpv4 = 1;
			ph->ph_ipIpv4_1st = 1;
			ph->ph_ipIpv6 = 0;
		} else {
			ph->ph_ipIpv4 = 0;
			ph->ph_ipIpv6 = 1;
		}
		ph->ph_type = (iph->protocol == IPPROTO_TCP) ? PKTHDR_TCP : PKTHDR_UDP;
	} else {
		ph->ph_type = 0;
		ph->ph_ipIpv4 = 0;
		ph->ph_ipIpv4_1st = 0;
		ph->ph_ipIpv6 = 0;
	}
	ph->ph_portlist = 0x3F; /* ALL_PORT_MASK ports 0-5, matches bootloader */

	/* BLOCKED: scatter-gather fragment chaining.
	 * BSP _swNic_send chains spare mbufs via m_next for page fragments
	 * (skb_shinfo(skb)->nr_frags), with MBUF_EOR on the last fragment.
	 * A circular pool of SG_MBUF_NUM spare mbufs would be allocated in
	 * alloc_rings().  eth_probe would set NETIF_F_SG.
	 * Tested: even minimal TX path rearrangements cause TCP hang.
	 * CF_TX_GATHER (CPUICR1 BIT(6)) already enabled in switch_start(). */

	cache_wb(skb->data, len);  /* flush CPU cache so RISC sees the data */

	eth->tx_skb[idx] = skb;
	eth->tx_head = next;

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	/* Give descriptor to RISC (set OWN=1) and kick TXFD */
	*(volatile u32 *)&eth->tx_ring[idx] |= DESC_SWCORE_OWNED;
	cpu_w32(CPUICR, cpu_r32(CPUICR) | TXFD);
	return NETDEV_TX_OK;
}

static void tx_clean(struct rtl8198c_eth *eth)
{
	while (eth->tx_tail != eth->tx_head) {
		int idx = eth->tx_tail;
		u32 v = *(volatile u32 *)&eth->tx_ring[idx];
		if (v & DESC_OWNED_BIT) break; /* CPU owns = still pending */
		if (eth->tx_skb[idx]) {
			dev_kfree_skb_any(eth->tx_skb[idx]);
			eth->tx_skb[idx] = NULL;
		}
		eth->tx_tail = (eth->tx_tail + 1) & (RING_SIZE - 1);
	}
	if (netif_queue_stopped(eth->netdev)) {
		int next = (eth->tx_head + 1) & (RING_SIZE - 1);
		if (next != eth->tx_tail)
			netif_wake_queue(eth->netdev);
	}
}

/* Legacy RX poll — based on bootloader swNic_receive() */
static int eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl8198c_eth *eth = container_of(napi, struct rtl8198c_eth, napi);
	int rx_done = 0;

	tx_clean(eth);
	while (rx_done < budget) {
		struct pkthdr *ph;
		int idx = eth->rx_head;
		u32 v;

		/* Read descriptor via KSEG1 (uncached) — ring entries are KSEG1 pointers */
		v = *(volatile u32 *)&eth->rx_ring[idx];
		if (v & DESC_OWNED_BIT) break; /* CPU owns = empty */

		ph = (struct pkthdr *)(v & ~(DESC_OWNED_BIT | DESC_WRAP));
		if (!ph->ph_len) break;
		if (!ph->ph_mbuf) break;

		{
			struct sk_buff *skb;
			int len = ph->ph_len - 4; /* ph_len includes CRC */
			void *rx_data = ph->ph_mbuf->m_data;

			if (len <= 0 || len > MBUF_BUF_SIZE) {
				eth->rx_ring[idx] |= DESC_SWCORE_OWNED;
				eth->mbuf_ring[idx] |= DESC_SWCORE_OWNED;
				eth->rx_head = (eth->rx_head + 1) & (RING_SIZE - 1);
				continue;
			}

			skb = netdev_alloc_skb(eth->netdev, len + 4);
			if (!skb) {
				eth->netdev->stats.rx_dropped++;
				eth->rx_ring[idx] |= DESC_SWCORE_OWNED;
				eth->mbuf_ring[idx] |= DESC_SWCORE_OWNED;
				eth->rx_head = (eth->rx_head + 1) & (RING_SIZE - 1);
				continue;
			}
			/* MIPS requires 4-byte aligned access in kernel space.
			 * With NET_SKB_PAD=64 (=0 mod 4), skb->data starts at a
			 * 4-byte boundary, but the IP header at skb->data+14 lands
			 * at a 2-byte boundary, triggering the MIPS unaligned-access
			 * emulator which spinloops under load.
			 * Reserve 0 or 2 bytes so skb->data+14 ≡ 0 (mod 4). */
			{
				int align = ((unsigned long)skb->data + 14) & 3;
				if (align)
					skb_reserve(skb, 4 - align);
			}
			skb_put_data(skb, rx_data, len);
			skb->protocol = eth_type_trans(skb, eth->netdev);
			/* BLOCKED: should be CHECKSUM_UNNECESSARY when CSCR
			 * disallows L3/L4 checksum errors (see eth_probe). */
			skb->ip_summed = CHECKSUM_NONE;
			napi_gro_receive(napi, skb);
			eth->netdev->stats.rx_packets++;
			eth->netdev->stats.rx_bytes += len;
		}

		/* Return descriptor to RISC: set OWN on both pkthdr and mbuf */
		eth->rx_ring[idx] |= DESC_SWCORE_OWNED;
		eth->mbuf_ring[idx] |= DESC_SWCORE_OWNED;

		/* Handle runout */
		if (cpu_r32(CPUIISR) & PKTHDR_DESC_RUNOUT_IP_ALL) {
			cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) | PKTHDR_DESC_RUNOUT_IE_ALL);
			cpu_w32(CPUIISR, PKTHDR_DESC_RUNOUT_IP_ALL);
		}

		eth->rx_head = (eth->rx_head + 1) & (RING_SIZE - 1);
		rx_done++;
	}

	if (rx_done < budget) {
		napi_complete_done(napi, rx_done);
		cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) | RX_DONE_IP0);
	}
	return rx_done;
}

#else /* !DESC_FORMAT_LEGACY — flat 24-byte format */

static int alloc_rings(struct rtl8198c_eth *eth)
{
	int i;

	eth->rx_ring = uncached_alloc(RING_SIZE * sizeof(struct sw_desc), &eth->rx_phys, &eth->rx_ring_k0);
	if (!eth->rx_ring) return -ENOMEM;
	eth->tx_ring = uncached_alloc(RING_SIZE * sizeof(struct sw_desc), &eth->tx_phys, &eth->tx_ring_k0);
	if (!eth->tx_ring) return -ENOMEM;

	for (i = 0; i < RING_SIZE; i++) {
		u32 w = (MBUF_BUF_SIZE << 16);
		if (i == RING_SIZE - 1) w |= DESC_WRAP;
		w |= 0x1;
		eth->rx_ring[i].opts1 = w;
	}

	return 0;
}

static void free_rings(struct rtl8198c_eth *eth)
{
	kfree(eth->rx_ring_k0);
	kfree(eth->tx_ring_k0);
}

static int refill_rx(struct rtl8198c_eth *eth)
{
	int i;
	for (i = 0; i < RING_SIZE; i++) {
		struct sk_buff *skb;
		if (eth->rx_skb[i]) continue;
		skb = netdev_alloc_skb(eth->netdev, MBUF_BUF_SIZE);
		if (!skb) return -ENOMEM;
		eth->rx_skb[i] = skb;
		eth->rx_ring[i].addr = kseg1_phys(skb->data);
	}
	return 0;
}

static void free_rx_skbs(struct rtl8198c_eth *eth)
{
	int i;
	for (i = 0; i < RING_SIZE; i++) {
		if (!eth->rx_skb[i]) continue;
		dev_kfree_skb_any(eth->rx_skb[i]);
		eth->rx_skb[i] = NULL;
	}
}

static netdev_tx_t eth_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl8198c_eth *eth = netdev_priv(dev);
	struct sw_desc *txd;
	int idx, len;

	idx = eth->tx_head;
	txd = &eth->tx_ring[idx];
	if (txd->opts1 & DESC_OWN) {
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}
	len = skb->len;
	if (len < ETH_ZLEN) len = ETH_ZLEN;

	txd->opts1 = (1 << 3) | (1 << 2) | ((len + 4) << 6);
	if (idx == RING_SIZE - 1) txd->opts1 |= DESC_WRAP;
	txd->opts1 |= DESC_OWN;
	txd->addr = kseg1_phys(skb->data);
	txd->opts4 = 0x01;

	cache_wb(skb->data, len);

	eth->tx_skb[idx] = skb;
	eth->tx_head = (eth->tx_head + 1) & (RING_SIZE - 1);
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	cpu_w32(CPUICR, cpu_r32(CPUICR) | TXFD);
	return NETDEV_TX_OK;
}

static void tx_clean(struct rtl8198c_eth *eth)
{
	while (eth->tx_tail != eth->tx_head) {
		int idx = eth->tx_tail;
		if (eth->tx_ring[idx].opts1 & DESC_OWN) break;
		if (!eth->tx_skb[idx]) break;
		dev_kfree_skb_any(eth->tx_skb[idx]);
		eth->tx_skb[idx] = NULL;
		eth->tx_tail = (eth->tx_tail + 1) & (RING_SIZE - 1);
	}
	if (netif_queue_stopped(eth->netdev) &&
	    !(eth->tx_ring[eth->tx_head].opts1 & DESC_OWN))
		netif_wake_queue(eth->netdev);
}

static int eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl8198c_eth *eth = container_of(napi, struct rtl8198c_eth, napi);
	int rx_done = 0;

	tx_clean(eth);
	while (rx_done < budget) {
		struct sw_desc *rxd;
		struct sk_buff *skb, *new_skb;
		int idx = eth->rx_head, len;

		rxd = phys_ptr(eth->rx_phys + idx * sizeof(struct sw_desc));
		if (rxd->opts1 & DESC_OWN) break;
		skb = eth->rx_skb[idx];
		if (!skb) break;
		cache_inv(skb->data, MBUF_BUF_SIZE);

		len = (rxd->opts2 & 0x3FFF);
		if (len > MBUF_BUF_SIZE) len = MBUF_BUF_SIZE;
		new_skb = netdev_alloc_skb(eth->netdev, MBUF_BUF_SIZE);
		if (!new_skb) { eth->netdev->stats.rx_dropped++; new_skb = skb; goto refill; }
		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, eth->netdev);
		skb->ip_summed = CHECKSUM_COMPLETE;
		napi_gro_receive(napi, skb);
		eth->netdev->stats.rx_packets++;
		eth->netdev->stats.rx_bytes += len;
		skb = new_skb;
refill:
		eth->rx_skb[idx] = skb;
		rxd->addr = kseg1_phys(skb->data);
		rxd->opts1 = (MBUF_BUF_SIZE << 16) | DESC_OWN;
		if (idx == RING_SIZE - 1) rxd->opts1 |= DESC_WRAP;
		cpu_w32(CPUIISR, cpu_r32(CPUIISR) | PKTHDR_RUNOUT_IP0);
		eth->rx_head = (eth->rx_head + 1) & (RING_SIZE - 1);
		rx_done++;
	}
	if (rx_done < budget) {
		napi_complete_done(napi, rx_done);
		cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) | RX_DONE_IP0);
	}
	return rx_done;
}
#endif /* DESC_FORMAT_LEGACY */

/* ===== [Kernel Interface] IRQ, net_device_ops, NAPI, ethtool, debugfs ===== */

/* ===== IRQ (common) ===== */
static irqreturn_t eth_irq(int irq, void *dev_id)
{
	struct rtl8198c_eth *eth = dev_id;
	u32 status = cpu_r32(CPUIISR);
	if (!status) return IRQ_NONE;
	cpu_w32(CPUIISR, status);
	if (status & (RX_DONE_IP0 | TX_DONE_IP0)) {
		cpu_w32(CPUIIMR, cpu_r32(CPUIIMR) & ~(RX_DONE_IP0 | TX_DONE_IP0));
		napi_schedule(&eth->napi);
	}
	return IRQ_HANDLED;
}

/* ===== net_device_ops ===== */
static int eth_open(struct net_device *dev)
{
	struct rtl8198c_eth *eth = netdev_priv(dev);
	int ret;

	ret = refill_rx(eth);
	if (ret) return ret;

#ifdef DESC_FORMAT_LEGACY
	cpu_w32(CPURPDCR0, (u32)eth->rx_ring);
	cpu_w32(CPUTPDCR0, (u32)eth->tx_ring);
	cpu_w32(CPURMDCR0, (u32)eth->mbuf_ring);
	cpu_w32(CPUICR, BIT(31) | BIT(30) | BIT(3) | MBUF_2048);
	cpu_w32(DMA_CR1, (RING_SIZE - 1) * sizeof(u32));
	cpu_w32(DMA_CR4, cpu_r32(DMA_CR4) | TX_RING0_TAIL_AWARE);
	cpu_w32(TXRINGCR, TX_RING0_EN); /* only ring0, matching BSP state machine */
#else
	cpu_w32(CPURPDCR0, eth->rx_phys);
	cpu_w32(CPUTPDCR0, eth->tx_phys);
	cpu_w32(DMA_CR0, cpu_r32(DMA_CR0) | DMA_HSB_ADDR_MARK);
	cpu_w32(DMA_CR1, (RING_SIZE - 1) * sizeof(struct sw_desc));
	cpu_w32(DMA_CR4, BIT(0));
	cpu_w32(TXRINGCR, cpu_r32(TXRINGCR) & ~(TX_RING2_EN | TX_RING3_EN));
	cpu_w32(TXRINGCR, cpu_r32(TXRINGCR) | TX_RING0_EN);
#endif

	switch_start();

	napi_enable(&eth->napi);
	netif_start_queue(dev);
	return 0;
}

static int eth_stop(struct net_device *dev)
{
	struct rtl8198c_eth *eth = netdev_priv(dev);
	netif_stop_queue(dev);
	napi_disable(&eth->napi);
	cpu_w32(CPUIIMR, 0);
	cpu_w32(CPUICR, 0);
	sys_w32(GIMR, sys_r32(GIMR) & ~BSP_SW_IE);
	tx_clean(eth);
	free_rx_skbs(eth);
	eth->tx_head = eth->tx_tail = eth->rx_head = 0;
	return 0;
}

static const struct net_device_ops eth_ops = {
	.ndo_open	= eth_open,
	.ndo_stop	= eth_stop,
	.ndo_start_xmit	= eth_start_xmit,
	/* BLOCKED:   .ndo_set_rx_mode = eth_set_rx_mode,
	 * Required for promiscuous / allmulticast / bridge support.
	 * The BSP's re865x_set_rx_mode() is a stub — must write from scratch.
	 * Implementation: toggle FFCR TOCPU bits based on dev->flags
	 * (see switch_l2_init FFCR comment for bit definitions). */
};

/* ===== ethtool ops ===== */
static void ethtool_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "rtl8198c-eth", sizeof(info->driver));
	strscpy(info->version, "1.0", sizeof(info->version));
}

static u32 ethtool_get_link(struct net_device *dev) { return netif_running(dev); }

static const struct ethtool_ops ethtool_ops = {
	.get_drvinfo	= ethtool_get_drvinfo,
	.get_link	= ethtool_get_link,
};

/* ===== debugfs: live register dump ===== */
static struct dentry *debugfs_root;

static int debugfs_regs_show(struct seq_file *s, void *v)
{
	struct rtl8198c_eth *eth = s->private;

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
	seq_printf(s, "SWTCR0:     0x%08X\n", sys_r32(SWTCR0));
	seq_printf(s, "FFCR:       0x%08X\n", sys_r32(FFCR));
	seq_printf(s, "VCR0:       0x%08X\n", sys_r32(VCR0));
	seq_printf(s, "MACCR:      0x%08X\n", sys_r32(MACCR));
	seq_printf(s, "CSCR:       0x%08X\n", sys_r32(CSCR));
	seq_printf(s, "QNUMCR:     0x%08X\n", sys_r32(QNUMCR));
	seq_printf(s, "MACCTRL1:   0x%08X\n", sys_r32(MACCTRL1));
	seq_printf(s, "PCRP0:      0x%08X\n", sys_r32(PCRP0));
	seq_printf(s, "PCRP1:      0x%08X\n", sys_r32(PCRP1));
	seq_printf(s, "PCRP3:      0x%08X\n", sys_r32(PCRP3));
	seq_printf(s, "PCRP6:      0x%08X\n", sys_r32(PCRP6));
	seq_printf(s, "PVCR0:      0x%08X\n", sys_r32(PVCR0));
	seq_printf(s, "PVCR6:      0x%08X\n", sys_r32(0xBB804A20));
	seq_printf(s, "L2[0,0]:    0x%08X 0x%08X\n", sys_r32(0xBB000000), sys_r32(0xBB000004));
	seq_printf(s, "L2[f0,0]:   0x%08X 0x%08X\n", sys_r32(0xBB007800), sys_r32(0xBB007804));
	seq_printf(s, "VLAN[1]:    0x%08X\n", sys_r32(0xBB050020));
#ifdef DESC_FORMAT_LEGACY
	seq_printf(s, "Ring0:      0x%08X\n", eth->rx_ring ? eth->rx_ring[0] : 0);
	seq_printf(s, "Tring0:     0x%08X\n", eth->tx_ring ? eth->tx_ring[0] : 0);
	seq_printf(s, "Mring0:     0x%08X\n", eth->mbuf_ring ? eth->mbuf_ring[0] : 0);
#endif
	seq_printf(s, "rx_head=%d tx_head=%d tx_tail=%d\n", eth->rx_head, eth->tx_head, eth->tx_tail);

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

static void debugfs_init(struct rtl8198c_eth *eth)
{
	if (!debugfs_root) {
		debugfs_root = debugfs_create_dir("rtl8198c-eth", NULL);
		if (IS_ERR_OR_NULL(debugfs_root))
			return;
	}
	eth->debugfs_dir = debugfs_create_dir(eth->netdev->name, debugfs_root);
	if (IS_ERR_OR_NULL(eth->debugfs_dir))
		return;
	debugfs_create_file("regs", 0400, eth->debugfs_dir, eth, &debugfs_regs_fops);
	debugfs_create_file("mac_filter", 0600, eth->debugfs_dir, eth, &mac_filter_fops);
	debugfs_create_file("vlan_egress", 0600, eth->debugfs_dir, eth, &vlan_egress_fops);
}

static void debugfs_cleanup(struct rtl8198c_eth *eth)
{
	debugfs_remove_recursive(eth->debugfs_dir);
}

/* ===== [Platform Driver] probe, remove, module init/exit ===== */

/* ===== probe / remove ===== */
static int eth_probe(struct platform_device *pdev)
{
	struct rtl8198c_eth *eth;
	struct net_device *netdev;
	int irq, ret;
	u8 mac[ETH_ALEN];

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) return irq;
	netdev = alloc_etherdev(sizeof(*eth));
	if (!netdev) return -ENOMEM;
	eth = netdev_priv(netdev);
	eth->netdev = netdev;
	eth->dev = &pdev->dev;
	eth->irq = irq;
	eth->mac_filter_enabled = false;
	eth->mac_filter_entry = 128;	/* entry index 128 (row 32, col 0) */

	/* MAC: device tree first, fallback to hardcoded */
	if (of_get_mac_address(pdev->dev.of_node, mac))
		eth_random_addr(mac);

	switch_reset();
	switch_l2_init(mac);
	/* BOOTLOADER: GPHY workarounds + BSP NCTL calibration */
	switch_gphy_init();
	/* N-way restart AFTER GPHY patches (phy_refine enables gigabit DSP path).
	 * Explicitly set PHY 1000BASE-T FD advertisement — the PCRP NwayAbility
	 * bits control the MAC side but the GPHY reg 9 needs an explicit write. */
	{
		u32 id, v2;
		int pi;
		for (pi = 0; pi < 5; pi++) {
			id = (pi == 0) ? 8 : pi;
			v2 = mdio_read(id, 9);
			mdio_write(id, 9, v2 | BIT(9));
			v2 = mdio_read(id, 0);
			mdio_write(id, 0, v2 | BIT(9));
		}
	}

	/* Bootloader sets SIRR TRXRDY BEFORE swNic_init.
	 * The RISC may latch ring bases at TRXRDY time. */
	sys_w32(SIRR, sys_r32(SIRR) | SIRR_TRXRDY);

	ret = alloc_rings(eth);
	if (ret) goto err_free;

	/* Bootloader creates netif + VLAN AFTER swNic_init.
	 * Create them here matching the exact order. */
	netif_vlan_init();
#ifdef DESC_FORMAT_LEGACY
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
#else
	cpu_w32(CPURPDCR0, eth->rx_phys);
	cpu_w32(CPUTPDCR0, eth->tx_phys);
	cpu_w32(0x030, 0);
	cpu_w32(DMA_CR0, cpu_r32(DMA_CR0) | DMA_HSB_ADDR_MARK);
	cpu_w32(DMA_CR1, (RING_SIZE - 1) * sizeof(struct sw_desc));
	cpu_w32(DMA_CR4, BIT(0));
	cpu_w32(TXRINGCR, cpu_r32(TXRINGCR) & ~(TX_RING2_EN | TX_RING3_EN));
	cpu_w32(TXRINGCR, cpu_r32(TXRINGCR) | TX_RING0_EN);
	cpu_w32(CPUIIMR, 0);
	cpu_w32(CPUIISR, cpu_r32(CPUIISR));
#endif

	switch_start();

	dev_info(eth->dev, "rings RX=%px TX=%px CPUICR=0x%08x FFCR=0x%08x\n",
		 eth->rx_ring, eth->tx_ring,
		 cpu_r32(CPUICR), sys_r32(FFCR));

	ret = devm_request_irq(eth->dev, irq, eth_irq, 0, "rtl8198c-eth", eth);
	if (ret) goto err_free;

	SET_NETDEV_DEV(netdev, eth->dev);
	netdev->netdev_ops = &eth_ops;
	netdev->ethtool_ops = &ethtool_ops;
	eth_hw_addr_set(netdev, mac);
	netif_napi_add_weight(netdev, &eth->napi, eth_poll, 16);
	platform_set_drvdata(pdev, eth);

	/* BLOCKED: NETIF_F_HW_CSUM + NETIF_F_SG + CHECKSUM_UNNECESSARY RX.
	 *
	 * Checksum offload (NETIF_F_HW_CSUM):
	 *   dev->features |= NETIF_F_HW_CSUM;
	 *   BSP _swNic_send sets CSUM_IP|CSUM_TCPUDP in ph_flags when
	 *   skb->ip_summed == CHECKSUM_PARTIAL (see eth_start_xmit below).
	 *   Requires CSCR EnL3ChkCal|EnL4ChkCal — blocked (see switch_l2_init).
	 *
	 * RX checksum verification (CHECKSUM_UNNECESSARY):
	 *   skb->ip_summed = CHECKSUM_UNNECESSARY;  // in eth_poll
	 *   Requires CSCR to drop error packets — blocked.
	 *
	 * Scatter-gather (NETIF_F_SG):
	 *   dev->features |= NETIF_F_SG;
	 *   Requires frag loop in eth_start_xmit with m_next mbuf chaining
	 *   and a spare mbuf pool (BSP _swNic_send chain via m_next + MBUF_EOR).
	 *   Tested: any TX path changes cause TCP hang, even with flag removed.
	 *   CF_TX_GATHER (CPUICR1 BIT(6)) already enabled in switch_start(). */
	netdev->hw_features = netdev->features;

	ret = register_netdev(netdev);
	if (ret) goto err_free;
	debugfs_init(eth);
	dev_info(eth->dev, "registered as %s IRQ %d\n", netdev->name, irq);
	return 0;

err_free:
	free_rings(eth);
	free_netdev(netdev);
	return ret;
}

static void eth_remove(struct platform_device *pdev)
{
	struct rtl8198c_eth *eth = platform_get_drvdata(pdev);
	if (!eth) return;
	debugfs_cleanup(eth);
	if (eth->netdev) unregister_netdev(eth->netdev);
	free_rings(eth);
	if (eth->netdev) free_netdev(eth->netdev);
}

static const struct of_device_id eth_match[] = {
	{ .compatible = "realtek,rtl819x-eth" }, {},
};
MODULE_DEVICE_TABLE(of, eth_match);

static struct platform_driver eth_driver = {
	.probe = eth_probe, .remove = eth_remove,
	.driver = { .name = "rtl8198c-eth", .of_match_table = eth_match },
};

static int __init eth_init(void) { return platform_driver_register(&eth_driver); }
late_initcall(eth_init);
static void __exit eth_exit(void) { platform_driver_unregister(&eth_driver); }
module_exit(eth_exit);
MODULE_LICENSE("GPL");
