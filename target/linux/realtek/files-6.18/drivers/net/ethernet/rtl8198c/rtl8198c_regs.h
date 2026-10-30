// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- hardware register and table map.
 *
 * Addresses are the KSEG1 windows of the SoC unless noted; TACI table
 * indices are 32-byte entries at TBL_* + index * 32.
 */
#ifndef _RTL8198C_REGS_H
#define _RTL8198C_REGS_H

#include <linux/types.h>

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
#define CPUQDM0			0x030	/* queue 0/1 -> RX ring map (32-bit pair) */
#define CPUTPDCR0		0x020
#define CPUTPDCR1		0x024
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
#define TX_RING1_EN		BIT(1)
#define TX_RING2_EN		BIT(2)
#define TX_RING3_EN		BIT(3)
#define TX_RING0_TAIL_AWARE	BIT(0)
#define TX_RING1_TAIL_AWARE	BIT(1)
#define TX_ALL_DONE_IP1		BIT(2)
#define CPUICR1			0x0A4
#define CF_PKT_HDR_TYPE_OFFSET	8
#define CF_PKT_HDR_TYPE_MASK	(3 << 8)
#define TX_PKTHDR_8198C_DEF	(0 << 8)

#define RX_DONE_IP0		BIT(3)
#define TX_DONE_IP0		BIT(1)
#define RX_DONE_IE_ALL		(0x3F << 3)
#define TX_DONE_IE_ALL		(BIT(2)|BIT(9)|BIT(10)|BIT(14)|BIT(15))
#define PKTHDR_RUNOUT_IP0	BIT(17)
#define PKTHDR_DESC_RUNOUT_IE_ALL 0x007E0000	/* rings 0-5, bits 17-22 */
#define PKTHDR_DESC_RUNOUT_IP_ALL 0x007E0000
#define MBUF_DESC_RUNOUT_IP0	BIT(16)
#define MBUF_DESC_RUNOUT_IE0	BIT(11)
/* A port's link state changed.  One bit for the whole switch: the event
 * does not say which port, so the handler rescans all of them. */
#define LINK_CHANGE_IP		BIT(31)
#define LINK_CHANGE_IE		BIT(31)

/* ===== Switch core L2 registers ===== */
#define PCRP0			0xBB804104
#define PCRP1			0xBB804108
#define PCRP2			0xBB80410C
#define PCRP3			0xBB804110
#define PCRP4			0xBB804114
#define PCRP6			0xBB80411C
/* Per-port link state as the switch itself reports it (PCRAM + 0x28).  The
 * link-change interrupt does not name a port, and this register answers for
 * all of them without an MDIO transaction. */
#define PSRP0			0xBB804128
#define PSRP_LINK_UP		BIT(4)
#define MACCR			0xBB804000
#define CSCR			0xBB804048
#define CSCR_ALLOW_L2_ERR	BIT(0)	/* allow frames with L2 CRC errors */
#define CSCR_ALLOW_L3_ERR	BIT(1)	/* allow frames with IP checksum errors */
#define CSCR_ALLOW_L4_ERR	BIT(2)	/* allow frames with TCP/UDP checksum errors */
#define CSCR_EN_L3_CAL		BIT(4)	/* hardware recalculation, IP header */
#define CSCR_EN_L4_CAL		BIT(5)	/* hardware recalculation, TCP/UDP */
#define MACCR_RXIPG		6	/* min IPG for received packets (6..12) */
#define VCR0			0xBB804A00
#define SWTCR0			0xBB804418
#define SWTCR0_NAPT_DROP_REVERSE BIT(0)	/* reverse session miss: drop, not CPU */
#define SWTCR0_NAPT_AUTOLEARN	BIT(1)
#define SWTCR0_NAPT_AUTODELETE	BIT(2)
#define SWTCR0_NAPT_F2CPU	BIT(14)	/* non-TCP/UDP/ICMP to a NAPT netif */
#define LIMDBC_MASK		(3 << 16)
#define LIMDBC_VLAN		(0 << 16)
#define EN_UKVID_TO_CPU		BIT(15)
#define SWTCR1			0xBB80441C
#define SWTCR1_L4_ENHASH1	BIT(13)	/* enhanced hash1: session verify field */
#define DACLRCR			0xBB804424	/* default ACL rule control: ingress
					 * range @7:0/15:8, egress @23:16/31:24 */
#define FFCR			0xBB804428
#define ALECR			0xBB80440C
#define PLITIMR			0xBB804420
#define MSCR			0xBB804410
#define RMACR			0xBB804408
#define GDSR0			0xBB806100
#define QNUMCR			0xBB804754
#define CPUQIDMCR0		0xBB804758	/* priority -> CPU queue */
#define DSCPCR0			0xBB804734	/* DSCP -> priority, 10 entries/reg */
#define UPTCMCR0		0xBB804718	/* priority -> QID for 1 output queue */
#define UPTCMCR1		0xBB80471C	/* priority -> QID for 2 output queues */
#define QOSFCR			0xBB804700
#define SBFCR0			0xBB804500
#define SBFCR1			0xBB804504
#define SBFCR2			0xBB804508
#define PBFCR0			0xBB80450C
#define PVCR0			0xBB804A08
#define MACCTRL1		0xBB805100
#define BSCR			0xBB804044	/* broadcast storm control rate */
#define PMCR			0xBB80400C	/* port mirror control */
#define PMCR_DEST_SHIFT		18		/* single destination port bit */
#define PMCR_DEST_MASK		(0x1FF << PMCR_DEST_SHIFT)
#define PMCR_RX_SHIFT		9		/* mirrored ingress port mask */
#define PMCR_RX_MASK		(0x1FF << PMCR_RX_SHIFT)
#define PMCR_TX_SHIFT		0		/* mirrored egress port mask */
#define PMCR_TX_MASK		0x1FF
#define BSCR_MAX_BYTES		30360	/* bytes per interval = 100% on a 10M port */
#define ETH_PORTS		5	/* physical ports 0-4 (internal GPHYs) */
/* PCRP storm-control bits (per port) */
#define PCRP_EN_BC_SC		BIT(8)
#define PCRP_BC_SC_BCAST	BIT(9)
#define PCRP_BC_SC_MCAST	BIT(10)

/* Registers used by switch_hard_reset() */
#define VCR1			0xBB804A04
#define TEACR			0xBB804400	/* table entry aging control */
#define L2_AGING_DISABLE	BIT(0)
#define L4_AGING_DISABLE	BIT(1)
#define EN_L2_FAST_AGING	BIT(5)
/* L4 session/VLAN timeout base: five 6-bit fields, non-linear encoding.
 * The field order is fixed by the hardware; see rtl8198c_l4.c. */
#define TEATCR			0xBB804404
#define TEATCR_TCP_FAST_SHIFT	0
#define TEATCR_TCP_MED_SHIFT	6
#define TEATCR_TCP_LONG_SHIFT	12
#define TEATCR_UDP_SHIFT	18
#define TEATCR_ICMP_SHIFT	24
#define TEATCR_FIELD		0x3F
#define MGFCR_E0R0		0xBB80442C	/* 12 MAC-group forwarding regs */
#define PPMAR			0xBB804010	/* per-port pattern matching action */
#define DOT1XPORTCR		0xBB804B00
#define DOT1XMACCR		0xBB804B04
#define GVGCR			0xBB804B08
#define PBVCR0			0xBB804A1C	/* protocol-based VLAN control (user rule 1) */
#define PBVCR1			0xBB804A20	/* protocol-based VLAN control (user rule 2) */
#define PBVR0_0			0xBB804A24	/* 30 PBVCR rule registers */
#define LAGHPMR0		0xBB804C00	/* link aggregation block: not
						 * usable on this core, see the
						 * debugfs "lag" file */
#define LAGCR0			0xBB804C08
#define PATP0			0xBB804014	/* priority assignment table, 12 regs */
#define QIDDPCR			0xBB804750	/* queue id decision priority */
#define LPTM8021Q		0xBB804730	/* 802.1Q priority mapping */
#define DSCPCR0			0xBB804734	/* 7 per-port DSCP priority regs */
#define RMCR1P			0xBB80476C	/* 802.1P remarking */
#define DSCPRM0			0xBB804770
#define DSCPRM1			0xBB804774
#define RLRC			0xBB804778	/* remarking layer rule */
#define ELBPCR			0xBB804904	/* egress leaky bucket (PSCR) */
#define ELBTTCR			0xBB804908
#define ILBPCR1			0xBB80490C	/* ingress leaky bucket (PSCR) */
#define ILBPCR2			0xBB804910
#define P0Q0RGCR		0xBB804800	/* 42 per-queue rate guarantees (PSCR) */
#define WFQRCRP0		0xBB8048B0	/* 7 ports x 3 WFQ regs (PSCR) */
#define TMCR			0xBB806300
#define ENHSBTESTMODE		BIT(1)

/* ===== Switch lookup tables (TACI; KSEG1 base + index * 32) ===== */
#define TBL_L2			0xBB000000
#define TBL_ARP			0xBB010000
#define TBL_ROUTE		0xBB020000	/* IPv4: 8 prefixes, last = default */
#define TBL_IPMCAST		0xBB030000	/* (S,G), L3 mode only */
#define TBL_NETIF		0xBB040000
#define TBL_EXTIP		0xBB050000	/* internal/external IP pairs (NAPT) */
#define TBL_VLAN		0xBB060000
#define TBL_SERVER_PORT		0xBB080000	/* not in the 8198C silicon */
#define TBL_TCP_UDP		0xBB090000	/* NAPT sessions, 1024 entries */
#define TBL_ICMP		0xBB0A0000	/* not in the 8198C silicon */
#define TBL_PPPOE		0xBB0B0000
#define TBL_ACL			0xBB0C0000
#define TBL_NEXTHOP		0xBB0D0000	/* gateway groups, used by process 5 */
#define TBL_ALG			0xBB0F0000	/* not in the 8198C silicon */
#define TBL_DSLITE		0xBB100000
#define TBL_6RD			0xBB110000
#define TBL_ROUTE6		0xBB120000
#define TBL_NEXTHOP6		0xBB130000
#define TBL_ARP6		0xBB140000
#define TBL_IPMCAST6		0xBB150000
#define TACI_BASE		0xBB804D00
#define SWTACR			(TACI_BASE + 0x00)
#define SWTAA			(TACI_BASE + 0x08)
#define TCR0			(TACI_BASE + 0x20)
#define TACI_ACTION		1		/* bit 0: command busy */
#define TACI_CMD_READ		0x1		/* stop TLU and read */
#define TACI_CMD_ADD		0x3		/* insert, hash decides the slot */
#define TACI_CMD_FORCE		0x9		/* write this exact slot */

/* Table sizes (8198C section). */
#define L3_ROUTES		8
#define L3_DEFAULT_ROUTE	(L3_ROUTES - 1)	/* the route-miss path */
#define L3_NEXTHOPS		32
#define L3_ARPS			512
#define L3_ARP_STRIDE		8		/* ARP block granularity */
#define L3_EXT_IPS		16
#define L3_PPPOES		8
#define L3_IPMCASTS		256
#define L3_ROUTES6		8
#define L3_NEXTHOPS6		32
#define L3_ARPS6		256
#define L3_IPMCASTS6		256
#define L3_DSLITE_ENTRIES	8
#define L3_6RD_ENTRIES		8

/* TCP/UDP session table (TACI type 9).  The external-IP table is the one
 * already used by the L3 code (16 entries).  The ICMP echo, server-port and
 * ALG tables have type numbers but no silicon behind them on this core. */
#define L4_SESSIONS		1024
#define L4_HASH_MASK		(L4_SESSIONS - 1)
#define L4_EXT_IPS		16

/* ALE control bits used once the routing engine is armed. */
#define ALECR_TTL1		BIT(16)	/* decrement the TTL of routed frames */
#define ALECR_FRAG2CPU		BIT(15)	/* trap IP fragments to the CPU */

/* Route entry, word 1, process field: where the next hop comes from. */
#define L3_PROC_PPPOE		0
#define L3_PROC_L2		1		/* direct, nextHop holds an L2 index */
#define L3_PROC_ARP		2		/* on-link, range holds an ARP block */
#define L3_PROC_CPU		4		/* punt to the kernel */
#define L3_PROC_NEXTHOP		5		/* gateway group in the next-hop table */
#define L3_PROC_DROP		6

/* Netif entry, word 1: this interface takes part in L3 routing. */
#define NETIF_EN_HWROUTE	BIT(29)
#define NETIF_EN_HWROUTE_V6	BIT(30)

#define MIB_CONTROL		0xBB801000	/* also the counter block base */
/* Per-port counters, 0x80 bytes apart */
#define MIB_PORT_STRIDE		0x80
#define MIB_RX_UNICAST		0x108
#define MIB_RX_MULTICAST	0x13C
#define MIB_RX_BROADCAST	0x140
#define MIB_RX_DISCARDS		0x144
#define MIB_RX_DROP_EVENTS	0x148
#define MIB_RX_FCS_ERRORS	0x14C
#define MIB_RX_SYMBOL_ERRORS	0x150
#define MIB_TX_UNICAST		0x808
#define MIB_TX_MULTICAST	0x80C
#define MIB_TX_BROADCAST	0x810

/* 1000BASE-T status bit 13: local receiver OK, i.e. the link really is
 * running at 1G.  Bit 11 (LPA_1000FULL) only says the partner could. */
#define STAT1000_LOCAL_RX_OK	BIT(13)
#define MEMCR			0xBB804234
/* SoC efuse controller: per-port GPHY calibration data */
#define EFUSE_CMD		0xBB000700
#define EFUSE_CONFIG		0xBB000704
#define EFUSE_RW_DATA		0xBB000708
#define EFUSE_TIMING_CONTROL	0xBB00070C
#define EFUSE_CFG_INT_STS	BIT(8)

#define MDCIOCR			0xBB804004
#define MDCIOSR			0xBB804008
#define MDIO_WRITE		BIT(31)
#define MDIO_BUSY		BIT(31)

#define REVR			0xB8000000

/* PCR / MSCR bits */
#define PCR_MacSwReset		BIT(3)
#define PCRP_ACPT_9K		(2 << 1)	/* AcptMaxLen: 9K (9216) frames */
/* 802.1D port state.  Listening shares the blocking encoding, so the
 * hardware has four states for the five the bridge knows. */
#define PCRP_STP_DISABLE	(0 << 4)
#define PCRP_STP_BLOCKING	(1 << 4)
#define PCRP_STP_LEARNING	(2 << 4)
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
/* With EnForceMode set, bits 22:18 become ForceSpeed (20:19) + ForceDuplex
 * (18) instead of the advertisement. */
#define PCRP_EN_FORCE_MODE	BIT(25)
#define PCRP_POLL_LINK		BIT(24)
#define PCRP_FORCE_LINK		BIT(23)
#define PCRP_FORCE_SPEED_MASK	(3 << 19)
#define PCRP_FORCE_SPEED_10M	(0 << 19)
#define PCRP_FORCE_SPEED_100M	(1 << 19)
#define PCRP_FORCE_SPEED_1000M	(2 << 19)
#define PCRP_FORCE_DUPLEX_FULL	BIT(18)
#define MSCR_EN_L2		BIT(0)
#define MSCR_EN_L3		BIT(1)
#define MSCR_EN_L4		BIT(2)
#define MSCR_EN_OUT_ACL		BIT(3)
#define MSCR_EN_IN_ACL		BIT(4)
#define MSCR_EN_STP		BIT(5)
#define RMACR_MADDR00		BIT(0)
#define MACCR_NORMAL_BACKOFF	BIT(28)
#define MACCR_BACKOFF_EXP3	BIT(27)
#define MACCR_INF_PAUSE		BIT(24)
#define MACCR_LONG_TXE		BIT(22)
#define MACCR_DIS_MASK_CGST	BIT(21)
#define PHY_POWER_DOWN		BIT(11)

/* 256 descriptors per ring.  Smaller rings stall at gigabit rates: 64 is
 * the smallest that links at 100M but the TX queue stops in bursts at 1G,
 * and a 4-entry ring wedges the board because the RISC runs out of
 * descriptors before the kernel can refill.  The length reaches the RISC
 * through the WRAP bit on the last entry, so changing it needs no other
 * register change. */
#define RING_SIZE		256
/* Two TX rings: skb->priority >= 4 goes to ring 1.  DMA_CR1 describes one
 * ring length for the whole CPU interface, so every TX ring is as deep as
 * the RX ring. */
#define TX_RINGS		2
#define TX_RING_SIZE		RING_SIZE
/* The CPU interface has six RX rings, one per switch queue.  Queue n is
 * mapped to ring n by default; debugfs "rxmap" changes that at runtime. */
#define RX_RINGS		6
#define RX_RING_SIZE		RING_SIZE
#define JUMBO_MTU		9000
/* One frame per RX buffer: the legacy engine is not given scatter-gather
 * here, so the buffer has to hold the largest frame plus CRC.  Each buffer
 * is an skb head, so the frame reaches the stack without a copy.  The size
 * follows the largest port MTU: a 2 KB buffer covers a 1500-MTU frame and
 * napi_alloc_skb() can serve it from the page-fragment cache, while 10 KB
 * is a kmalloc per frame. */
#define MBUF_BUF_SIZE		10240	/* jumbo buffer */
#define RX_BUF_MIN		2048
#define RX_BUF_SLACK		64	/* header, CRC, VLAN tag */
/* Spare mbufs per TX descriptor for scatter-gather.  One chain per slot
 * (not one shared pool) so a descriptor's fragments can never collide
 * with another's, and MAX_SKB_FRAGS covers the largest non-linear skb. */
#define TX_SG_MBUF		MAX_SKB_FRAGS

/* ===== Legacy pkthdr/mbuf descriptor format ===== */

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
#define CSUM_TCPUDP_OK	0x0001	/* Incoming: TCP/UDP checksum verified */
#define CSUM_IP_OK	0x0002	/* Incoming: IPv4 header checksum verified */
#define CSUM_TCPUDP	0x0001	/* Outgoing: TCP/UDP cksum offload to ASIC */
#define CSUM_IP		0x0002	/* Outgoing: IP header cksum — the ASIC does
				 * not fill it (measured: it stays zero), so
				 * the driver computes it in software. */
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

/* The switch has four filtering databases.  The row hash is salted with the
 * fid, so an entry learned in one database is invisible in another: distinct
 * fids give per-VLAN MAC tables.  The salts are fixed by the hardware. */
#define L2_FIDS		4
/* Member/untag masks cover ports 0-5 plus the CPU extension port (member
 * bit 6, untag bit 15).  This is the WAN/LAN split: port 0 sits in
 * its own VLAN 8, the four LAN ports share VLAN 9, and the CPU is a member of
 * both.  LAN-to-LAN traffic therefore still forwards in hardware. */
#define VLAN_WAN	8
#define VLAN_LAN	9
#define L2_MEMBER_ALL	0x3F	/* ports 0-5, same mask as broadcast */
#define L2_FID_WAN	0
#define L2_FID_LAN	1

/* Protocol-based VLAN: six ingress rules (four fixed, two user-defined),
 * each assignable per port to a VLAN.  Register layout: the rule's VLAN is
 * 16 bits per port, two ports per register, value = vid << 1 | valid. */
#define PBV_RULES	6

/* ===== Hardware ACL =====
 *
 * TACI type 12, 252 usable entries (252-255 are the reserved rules).  The
 * engine filters after the netif decision: the netif's inAclStart..inAclEnd
 * range is walked, and when the decision misses (bridged traffic) the
 * default range in DACLRCR is used.  Rules are evaluated in order and the
 * first match wins (comb = 0), so a range has to end with a catch-all.
 * Only ingress ACLs are used: the egress engine is disabled because of a
 * hardware bug. */
#define ACL_RULES		252
#define ACL_IPV6_TO_CPU		252
#define ACL_PERMIT_ALL		253
#define ACL_DROP_ALL		254
#define ACL_ALL_TO_CPU		255
#define ACL_MAX_RULES		64	/* user rules the driver stores */

#define ACL_RULE_MAC		0
#define ACL_RULE_IP		2
/* Rule types 6 (TCP) and 7 (UDP) exist in the hardware but are matched at
 * the L4/NAPT stage, which this L2 configuration never runs: measured, a
 * "tcp dport" and even a bare "tcp" rule drop neither CPU-bound nor bridged
 * traffic while MAC and IP rules do.  They are not offered. */

#define ACL_PERMIT		0
#define ACL_DROP		2
#define ACL_TOCPU		3


#endif /* _RTL8198C_REGS_H */
