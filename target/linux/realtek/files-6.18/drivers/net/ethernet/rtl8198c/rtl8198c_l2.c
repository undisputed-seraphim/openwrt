// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L2: FDB, VLAN, netif table
 *
 * TACI table access, the L2 hash table, VLAN/PB-VLAN programming, the
 * netif table, aging, and the FDB/VLAN netdev operations.
 *
 * BSP counterparts: rtl865x_asicL2.c (rtl865x_initAsicL2 and the
 * rtl8651_*AsicL2Table/AsicVlan entry points).
 */
#include "rtl8198c.h"

/* ===== [BSP: l2Driver] L2 switching — FDB, VLAN, STP, QoS, netif table ===== */

/* Clear every switch table and the MIB counters.  MEMCR 0x7F covers the L2,
 * VLAN, ACL and IP tables plus the hash and MIB blocks; the wait is for the
 * clear to report completion in MEMCR bits 10 and 13.  This wipes the netif,
 * VLAN and static FDB entries, so callers must re-create them (see the
 * ordering note above taci_lock).  The MIB counters clear, but their register
 * block is not decoded on this SoC, so switch-level drops stay invisible. */
static void l2_table_clear(void)
{
	sys_w32(MEMCR, 0);
	sys_w32(MEMCR, 0x7F);	/* bootloader: MEMCR=0x7F */
	while ((sys_r32(MEMCR) & ((1 << 10) | (1 << 13))) != ((1 << 10) | (1 << 13)));
	sys_w32(MIB_CONTROL, 0x0007FFFF);
}

/* Every switch table goes through this one engine: stop the table lookup
 * unit, fill the TCRs, issue the command, restart it.  Two interleaved
 * sequences would clobber each other's command and data registers, so all
 * table access is serialised by taci_lock.
 *
 * Ordering contract for callers: a clear is global to a table, not
 * per-entry.  Anything that clears tables (MEMCR bits, l2_table_clear())
 * must re-create the netif entry, the VLAN entries and the static FDB
 * entries afterwards, or the board silently loses connectivity.
 */
static DEFINE_MUTEX(taci_lock);

/* TACI table write helper — TLU stop, fill TCRs, set TAA, issue CMD, TLU start.
 * CMD_BITS: 0x9 = CMD_FORCE, 0x3 = CMD_ADD, 0x1 = CMD_START (read). */
void rtl8198c_taci_write_entry(u32 taa, const u32 e[8], u32 cmd_bits)
{
	u32 sw;
	int i;

	mutex_lock(&taci_lock);
	sw = sys_r32(SWTCR0);
	sys_w32(SWTCR0, sw | BIT(18));
	while (!(sys_r32(SWTCR0) & BIT(19)));
	while (sys_r32(SWTACR) & TACI_ACTION);
	for (i = 0; i < 8; i++)
		sys_w32(TCR0 + i * 4, e[i]);
	sys_w32(SWTAA, taa);
	sys_w32(SWTACR, cmd_bits);
	while (sys_r32(SWTACR) & TACI_ACTION);
	sys_w32(SWTCR0, sw & ~BIT(18));
	mutex_unlock(&taci_lock);
}

/* Read one table entry.  The switch learns and ages entries asynchronously,
 * so a single read can catch the hardware in the middle of an update: read
 * the entry twice and retry until both copies agree. */
static void taci_read_entry(u32 taa, u32 e[8])
{
	u32 again[8];
	int i, tries;

	mutex_lock(&taci_lock);
	while (sys_r32(SWTACR) & TACI_ACTION)
		;
	for (tries = 0; tries < 4; tries++) {
		for (i = 0; i < 8; i++)
			e[i] = sys_r32(taa + i * 4);
		for (i = 0; i < 8; i++)
			again[i] = sys_r32(taa + i * 4);
		if (!memcmp(e, again, sizeof(again)))
			break;
	}
	mutex_unlock(&taci_lock);
}

/* ===== L2 FDB entry encoding =====
 *
 * Hardware layout (rtl865xc_tblAsic_l2Table_t, little-endian):
 *   word 0: octets 1-4 (octet1 in 31:24 ... octet4 in 7:0)
 *   word 1: [7:0] octet 0, [13:8] physical member mask (ports 0-5),
 *           [16:14] extension member mask (bit 0 = CPU), [17] toCPU,
 *           [18] isStatic, [20:19] aging
 *
 * The row is the XOR of the six MAC octets (fid 0 has a zero hash salt),
 * so the last octet is implied by the row.  Each row holds four columns,
 * which makes the TACI index row * 4 + column.
 */
#define L2_ROW_STRIDE		128	/* four 32-byte columns */

static const u8 l2_fid_salt[L2_FIDS] = { 0x00, 0x0F, 0xF0, 0xFF };

static u32 l2_row(const u8 *mac, u32 fid)
{
	return (mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5] ^
		l2_fid_salt[fid & 0x3]) & 0xFF;
}

static void l2_entry_build(u32 e[8], const u8 *mac, u32 member, u32 ext,
			   bool to_cpu, bool is_static, u32 fid)
{
	memset(e, 0, 8 * sizeof(u32));
	e[0] = ((u32)mac[1] << 24) | ((u32)mac[2] << 16) |
	       ((u32)mac[3] << 8) | (u32)mac[4];
	e[1] = (u32)mac[0] | ((member & 0x3F) << 8) | ((ext & 0x7) << 14) |
	       (to_cpu ? BIT(17) : 0) | (is_static ? BIT(18) : 0) |
	       ((fid & 0x3) << 23);
}

/* ===== FDB access =====
 *
 * The L2 table has 256 rows of 4 columns.  The row is fixed by the MAC hash,
 * so a lookup has to scan the row's four columns.  An entry is free when it
 * is neither static nor alive (agingTime == 0).
 */
#define L2_ROWS		256
#define L2_COLS		4
#define L2_AGE_STEP	150	/* seconds per agingTime step */

struct l2_fdb_entry {
	u8	mac[ETH_ALEN];
	u8	member;		/* physical port mask, ports 0-5 */
	u8	ext;		/* extension mask, bit 0 = CPU */
	u8	aging;		/* 0-3, in steps of L2_AGE_STEP seconds */
	u8	fid;		/* filtering database (per-VLAN MAC table) */
	bool	to_cpu;
	bool	is_static;
	u16	row;
	u8	col;
};

/* Serialises the read-modify-write in rtl8198c_l2_fdb_add() between the netdev FDB
 * ops and the debugfs interface. */
static DEFINE_MUTEX(l2_fdb_lock);

static u32 l2_entry_taa(u16 row, u8 col)
{
	return TBL_L2 + row * L2_ROW_STRIDE + col * 32;
}

static bool l2_entry_valid(const u32 e[8])
{
	return (e[1] & BIT(18)) || ((e[1] >> 19) & 3);
}

static void l2_entry_parse(const u32 e[8], u16 row, u8 col,
			   struct l2_fdb_entry *f)
{
	f->mac[0] = e[1] & 0xFF;
	f->mac[1] = e[0] >> 24;
	f->mac[2] = e[0] >> 16;
	f->mac[3] = e[0] >> 8;
	f->mac[4] = e[0];
	/* The row is the XOR of the MAC octets, salted with the fid, and the
	 * entry itself says which database it belongs to - so the last octet
	 * is recoverable from both. */
	f->fid = (e[1] >> 23) & 0x3;
	f->mac[5] = row ^ l2_fid_salt[f->fid] ^ f->mac[0] ^ f->mac[1] ^
		    f->mac[2] ^ f->mac[3] ^ f->mac[4];
	f->member = (e[1] >> 8) & 0x3F;
	f->ext = (e[1] >> 14) & 0x7;
	f->to_cpu = !!(e[1] & BIT(17));
	f->is_static = !!(e[1] & BIT(18));
	f->aging = (e[1] >> 19) & 0x3;
	f->row = row;
	f->col = col;
}

static int l2_fdb_find(const u8 *mac, u32 fid, struct l2_fdb_entry *f)
{
	u32 e[8];
	u16 row = l2_row(mac, fid);
	u8 col;

	for (col = 0; col < L2_COLS; col++) {
		taci_read_entry(l2_entry_taa(row, col), e);
		if (!l2_entry_valid(e))
			continue;
		l2_entry_parse(e, row, col, f);
		if (ether_addr_equal(f->mac, mac))
			return 0;
	}
	return -ENOENT;
}

/* Install a static entry.  If the MAC is already in the table its column is
 * rewritten, otherwise the first free column of the row is used. */
int rtl8198c_l2_fdb_add(const u8 *mac, u8 member, u8 ext, bool to_cpu, u32 fid)
{
	struct l2_fdb_entry f;
	u16 row = l2_row(mac, fid);
	u8 col, free_col = L2_COLS;
	u32 e[8];
	int err = 0;

	mutex_lock(&l2_fdb_lock);
	for (col = 0; col < L2_COLS; col++) {
		taci_read_entry(l2_entry_taa(row, col), e);
		if (!l2_entry_valid(e)) {
			if (free_col == L2_COLS)
				free_col = col;
			continue;
		}
		l2_entry_parse(e, row, col, &f);
		if (f.fid == fid && ether_addr_equal(f.mac, mac)) {
			l2_entry_build(e, mac, member, ext, to_cpu, true, fid);
			rtl8198c_taci_write_entry(l2_entry_taa(row, col), e,
					 BIT(0) | BIT(3));	/* FORCE */
			goto out;
		}
	}
	if (free_col == L2_COLS) {
		err = -ENOSPC;
		goto out;
	}
	l2_entry_build(e, mac, member, ext, to_cpu, true, fid);
	rtl8198c_taci_write_entry(l2_entry_taa(row, free_col), e, BIT(0) | BIT(3));
out:
	mutex_unlock(&l2_fdb_lock);
	return err;
}

int rtl8198c_l2_fdb_del(const u8 *mac, u32 fid)
{
	struct l2_fdb_entry f;
	u32 zero[8] = { 0 };
	int err;

	mutex_lock(&l2_fdb_lock);
	err = l2_fdb_find(mac, fid, &f);
	/* Forcing an all-zero entry is how the BSP deletes one. */
	if (!err)
		rtl8198c_taci_write_entry(l2_entry_taa(f.row, f.col), zero,
				 BIT(0) | BIT(3));
	mutex_unlock(&l2_fdb_lock);
	return err;
}

/* Drop learned entries, keeping static ones (broadcast, the port MACs and
 * anything pinned through the debugfs interface).  fid == L2_FIDS means every
 * database. */
static void l2_fdb_flush_dynamic(u32 fid)
{
	struct l2_fdb_entry f;
	u32 zero[8] = { 0 };
	u32 e[8];
	u16 row;
	u8 col;

	mutex_lock(&l2_fdb_lock);
	for (row = 0; row < L2_ROWS; row++) {
		for (col = 0; col < L2_COLS; col++) {
			taci_read_entry(l2_entry_taa(row, col), e);
			if (!l2_entry_valid(e))
				continue;
			l2_entry_parse(e, row, col, &f);
			if (f.is_static)
				continue;
			if (fid != L2_FIDS && f.fid != fid)
				continue;
			rtl8198c_taci_write_entry(l2_entry_taa(row, col), zero,
					 BIT(0) | BIT(3));
		}
	}
	mutex_unlock(&l2_fdb_lock);
}

/* Aging runs in 150 s steps and is a global switch setting, not a per-entry
 * timer: the hardware maintains the per-entry age class. */
static void l2_aging_set(const char *mode)
{
	u32 v = sys_r32(TEACR) & ~(L2_AGING_DISABLE | EN_L2_FAST_AGING);

	if (!strcmp(mode, "off"))
		v |= L2_AGING_DISABLE;
	else if (!strcmp(mode, "fast"))
		v |= EN_L2_FAST_AGING;
	sys_w32(TEACR, v);
}

/* L2 FDB static entries via TACI with KSEG1 addresses */
static void l2_fdb_init(const u8 macs[][ETH_ALEN], u32 n)
{
	static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	u32 e[8];
	u32 i;

	/* Broadcast: all five physical ports plus a copy to the CPU, once per
	 * database - a lookup is scoped to the ingress VLAN's fid. */
	for (i = 0; i < L2_FIDS; i++) {
		l2_entry_build(e, bcast, 0x3F, 0, true, true, i);
		rtl8198c_taci_write_entry(TBL_L2 + l2_row(bcast, i) * L2_ROW_STRIDE, e,
				 BIT(0) | BIT(3)); /* CMD_FORCE */
	}

	/* Every port netdev owns an address; the switch has to hand frames
	 * addressed to any of them to the CPU, and to no physical port.
	 * This mirrors the BSP _rtl865x_layer2_patch(), which adds the CPU
	 * MAC with a zero member mask and toCPU set.  rtl8198c_l2_fdb_add() picks a
	 * free column in the hash row, so the addresses can share rows. */
	for (i = 0; i < n; i++)
		rtl8198c_l2_fdb_add(macs[i], 0, 1, true, i ? L2_FID_LAN : L2_FID_WAN);
}
/* ===== L2 init (matches rtl865x_initAsicL2 8198C path) ===== */
void rtl8198c_switch_l2_init(const u8 macs[][ETH_ALEN])
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
	/* CSCR (checksum offload) is programmed at the end of this function,
	 * while the L2 engine is still disabled and SIRR.TRXRDY is already
	 * set by switch_hard_reset(). */
	v = sys_r32(MSCR);
	v &= ~MSCR_EN_STP;
	sys_w32(MSCR, v);
	v = sys_r32(RMACR);
	v &= ~RMACR_MADDR00;
	sys_w32(RMACR, v);

	/* SWTCR1: parser stages the ACL and QoS blocks depend on.  Bits
	 * 10/11/13 are the vendor's ENATT2LOG (attribute to logical port),
	 * ENFRAGTOACLPT (fragments to the ACL trap) and L4EnHash1. */
	v = sys_r32(SWTCR1);
	v |= BIT(10)|BIT(11)|BIT(13);
	sys_w32(SWTCR1, v);

	l2_table_clear();

	/* FFCR: clear both TOCPU bits so unknown unicast and multicast are
	 * flooded normally.  Measured: these bits do not gate CPU delivery.
	 * The CPU is a member of the VLAN, so it already receives unknown
	 * unicast, unknown multicast and broadcast through flooding with the
	 * bits set or clear.  They are therefore not the lever for
	 * promiscuous mode or allmulti; see the ndo_set_rx_mode note in
	 * rtl8198c_netdev.c. */
	v = sys_r32(FFCR);
	v &= ~(BIT(0) | BIT(1));
	sys_w32(FFCR, v);

	/* PCRP ports 0-4: MacSwReset cycle + PHY ID + EnablePHYIf */
	for (i = 0; i < 5; i++) {
		u32 phy_id = i;
		v = sys_r32(PCRP0 + i * 4);
		v &= ~(PCR_MacSwReset | (0x1F << 26));
		sys_w32(PCRP0 + i * 4, v);
		v |= (phy_id << 26) | BIT(0) | PCR_MacSwReset |
		     PCRP_ACPT_9K;
		sys_w32(PCRP0 + i * 4, v);
	}

	/* PCRP6 (CPU port): STP forwarding + EnablePHYIf + 9K frames */
	sys_w32(PCRP6, (3 << 4) | BIT(0) | PCRP_ACPT_9K);

	/* All ports (0-5 + CPU=6): STP forwarding state.  The state bits are
	 * ignored while MSCR.EN_STP is clear, and that is how an unbridged
	 * port runs: rtl8198c_stp_enable() starts the engine when a bridge
	 * takes a port over, and rtl8198c_stp_port_state() programs the
	 * states the bridge asks for. */
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

	/* Output queues: two per port, so 802.1p priority classes have
	 * somewhere to go.  The field is "queues - 1", three bits wide and
	 * QNUMCR is shared, so every port has to be written in its own
	 * iteration. */
	for (i = 0; i < 6; i++) {
		v = sys_r32(QNUMCR);
		v = (v & ~(0x7 << (3 * i))) | (1 << (3 * i));
		sys_w32(QNUMCR, v);
	}

	/* One CPU queue per RX ring.  The CPU port is field 6; the field is
	 * "queues - 1". */
	v = sys_r32(QNUMCR);
	v = (v & ~(0x7 << 18)) | ((RX_RINGS - 1) << 18);
	sys_w32(QNUMCR, v);

	/* Priority class n -> CPU queue n; classes 6/7 share queue 5.  The
	 * reset default is all-zero, so without this every frame is queue 0
	 * no matter what the classification tables say. */
	sys_w32(CPUQIDMCR0, 0x55543210);

	/* Every physical port has two egress queues; map priorities 0-3 to
	 * queue 0 and 4-7 to queue 1.  UPTCMCR1 is the table the switch
	 * uses while a port is configured with two queues. */
	sys_w32(UPTCMCR1, 0x00249000);

	/* Bandwidth control: include preamble + IFG in rate measurement.
	 * BSP sets QOSFCR BC_withPIFG for accurate egress rate limiting. */
	sys_w32(QOSFCR, sys_r32(QOSFCR) | BIT(0));

	/* Per-port VLAN / DSCP remarking: use DSCP remarking table with
	 * original priority.  BSP programs V4VLDSCPCR0-8 for ports 0-6 plus
	 * two extension ports. */
	for (i = 0; i <= 8; i++)
		sys_w32(0xBB804464 + i * 4,
			(2 << 28) | (0 << 26)); /* CF_IPM4DSCP_ACT_REMARK | CF_IPM4PRI_ACT_ORIG */

	/* Clear per-port DSCP priority control registers (ports 0-6). */
	for (i = 0; i <= 6; i++)
		sys_w32(0xBB804734 + i * 4, 0);

	/* MACCTRL1: lx_clk select (BSP rtl865x_initAsicL2) */
	sys_w32(MACCTRL1, sys_r32(MACCTRL1) | BIT(0));
	sys_w32(PVCR0, 8);
	sys_w32(0xBB804A0C, 8);
	sys_w32(0xBB804A10, 8);
	sys_w32(0xBB804A14, 8);
	sys_w32(0xBB804A18, 8);
	/* 0x4A1C is PBVCR0 (protocol-based VLAN), not a sixth PVCR: there
	 * are five PVID registers, 0x4A08-0x4A18.  It holds the user
	 * protocol rule's EtherType and stays zero until one is defined. */

	l2_fdb_init(macs, ETH_PORTS);

	/* MAC control state from the BSP init (rtl865x_initAsicL2):
	 * infinite pause frames, CGST mask, and normal backoff with the
	 * exponential parameter.  INFINITE_PAUSE_FRAMES is bit 24; bit 14
	 * is a different function and must not be set. */
	v = sys_r32(MACCR);
	v &= ~(MACCR_NORMAL_BACKOFF | MACCR_BACKOFF_EXP3);
	v |= MACCR_INF_PAUSE | MACCR_DIS_MASK_CGST | MACCR_LONG_TXE;
	sys_w32(MACCR, v);

	/* System clock → 100MHz (BSP: 2'b01 = lx_clk) */
	v = sys_r32(MACCR);
	v = (v & ~(0x3 << 12)) | (0x1 << 12);
	sys_w32(MACCR, v);

	/* Flow control DSC tolerance → 48 pages (default 24).
	 * BSP rtl865x_initAsicL2 prevents drops when pause is triggered. */
	v = sys_r32(MACCR);
	v = (v & ~(0x7f << 4)) | (0x30 << 4);
	sys_w32(MACCR, v);

	/* Checksum offload.  BSP rtl865x_initAsicL2() does this before
	 * MSCR is re-enabled: stop allowing frames with checksum errors
	 * through, and let the hardware recalculate the IP and TCP/UDP
	 * checksums.  Requires TRXRDY, which probe has already set. */
	v = sys_r32(CSCR);
	v &= ~(CSCR_ALLOW_L2_ERR | CSCR_ALLOW_L3_ERR | CSCR_ALLOW_L4_ERR);
	v |= CSCR_EN_L3_CAL | CSCR_EN_L4_CAL;
	sys_w32(CSCR, v);

	/* Re-enable the L2 forwarding engine.  EN_L3 only arms the routing
	 * engine: with no ARP, route or next-hop entries programmed nothing
	 * is routed and forwarding stays L2, so the bit is harmless here. */
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
/* Netif table entry: the MAC the switch should treat as "this netdev",
 * its VLAN and the MTU (1500).  Index is the port. */
void rtl8198c_netif_table_write(struct rtl8198c_eth *eth, u32 idx,
			      const u8 *mac, u32 vid)
{
	u64 m = ether_addr_to_u64(mac);
	u32 acl_end = eth->acl_end;	/* chains always start at entry 0 */
	u32 nif_e[8] = {0};

	nif_e[0] = ((u32)(m & 0x7FFFF) << 13) | (vid << 1) | 1;
	/* mac47_19 | enHWRoute | enHWRouteV6 | inACLStartL */
	nif_e[1] = ((u32)(m >> 19) & 0x1FFFFFFF);
	/* inACLStartH:7 | inACLEnd:8 | outACLStart:8 | outACLEnd:8 |
	 * macMaskL:1.  The ingress chain runs 0..acl_end; the egress range
	 * stays on the reserved permit rule because the egress engine is
	 * never enabled. */
	nif_e[2] = BIT(31) | ((acl_end & 0xFF) << 7) |
		   ((u32)ACL_PERMIT_ALL << 15) | ((u32)ACL_PERMIT_ALL << 23);
	nif_e[3] = ((u32)eth->port_mtu[idx] << 17) |
		   ((u32)eth->port_mtu[idx] << 2) | 3; /* mtuV6, mtu, macMaskH */
	rtl8198c_taci_write_entry(TBL_NETIF + idx * 32, nif_e, BIT(0) | BIT(3)); /* FORCE */
}

static const char * const pbvlan_name[PBV_RULES] = {
	"ipx", "netbios", "pppoe-control", "pppoe-session", "user1", "user2",
};

static void pbvlan_program(struct rtl8198c_eth *eth, u32 rule, u32 port)
{
	u32 addr = PBVR0_0 + rule * 5 * 4 + (port / 2) * 4;
	u32 val = (u32)eth->pbvlan_vid[rule][port] << 1;
	u32 cur;

	if (eth->pbvlan_vid[rule][port])
		val |= 1;		/* valid */
	cur = sys_r32(addr);
	if (port & 1)
		cur = (cur & 0x0000FFFF) | (val << 16);
	else
		cur = (cur & 0xFFFF0000) | val;
	sys_w32(addr, cur);
}

/* Rules 1-4 are fixed in hardware (IPX, NetBIOS, PPPoE control and
 * session); rules 5 and 6 take an EtherType, type field 0 = Ethernet II.
 *
 * The fixed rules match the encapsulation their name implies, not an
 * EtherType: measured, the "ipx" rule ignores an Ethernet-II frame with
 * EtherType 0x8137 and matches an 802.3 LLC frame with DSAP/SSAP 0xE0.
 * Same for NetBIOS and the two PPPoE rules. */
static void pbvlan_define(struct rtl8198c_eth *eth, u32 rule)
{
	if (rule == 4)
		sys_w32(PBVCR0, (u32)eth->pbvlan_ethertype[0] << 2);
	else if (rule == 5)
		sys_w32(PBVCR1, (u32)eth->pbvlan_ethertype[1] << 2);
}

static struct rtl8198c_vlan *vlan_find(struct rtl8198c_eth *eth, u16 vid)
{
	int i;

	for (i = 0; i < MAX_VLANS; i++)
		if (eth->vlans[i].used && eth->vlans[i].vid == vid)
			return &eth->vlans[i];
	return NULL;
}

/* VLAN entry word 0: member[5:0], extMember[8:6], untag[14:9],
 * extUntag[17:15], fid[19:18].  The CPU is port 6: member bit 6, untag
 * bit 15. */
static void vlan_program(struct rtl8198c_eth *eth,
			 const struct rtl8198c_vlan *v)
{
	u32 word0 = (v->member & 0x7F) | ((v->untag & 0x7F) << 9) |
		    ((u32)v->fid << 18);
	u32 e[8] = { word0, 0, 0, 0, 0, 0, 0, 0 };

	rtl8198c_taci_write_entry(TBL_VLAN + v->vid * 32, e, BIT(0) | BIT(3)); /* FORCE */
}

/* A port joins a VLAN.  Its native VLAN stays untagged (access port); any
 * other VLAN is tagged on that port, so its untag bit is cleared and the
 * switch preserves the tag when forwarding to it (trunk port). */
static int vlan_entry_add(struct rtl8198c_eth *eth, u16 vid, u32 port,
			  u16 native_vid)
{
	struct rtl8198c_vlan *v = vlan_find(eth, vid);
	int i, fid;

	if (!v) {
		for (i = 0; i < MAX_VLANS; i++)
			if (!eth->vlans[i].used)
				break;
		if (i == MAX_VLANS)
			return -ENOSPC;
		for (fid = 0; fid < L2_FIDS; fid++)
			if (!eth->fid_vid[fid])
				break;
		if (fid == L2_FIDS)
			return -ENOSPC;

		v = &eth->vlans[i];
		memset(v, 0, sizeof(*v));
		v->used = true;
		v->vid = vid;
		v->fid = fid;
		eth->fid_vid[fid] = vid;
		v->member = BIT(6);		/* CPU */
		/* The native VLANs reach the kernel untagged, because that is
		 * how the per-port netifs see ordinary traffic.  A VLAN
		 * created on request keeps its tag: the 802.1Q layer needs it
		 * to pick the VLAN device, and for stacked (QinQ) frames the
		 * outer tag is what the switch classified on - stripping it
		 * here would leave only the inner tag, which no VLAN device
		 * on this netdev matches.  Measured: with the untagged mask
		 * set, a double-tagged frame arrived with only the inner tag. */
		v->untag = (vid == VLAN_WAN || vid == VLAN_LAN) ? BIT(6) : 0;
	}

	v->member |= BIT(port);
	if (vid == native_vid)
		v->untag |= BIT(port);
	else
		v->untag &= ~BIT(port);
	vlan_program(eth, v);
	return 0;
}

/* Drop a port from a VLAN; when the CPU is the only member left the entry is
 * deleted and its filter database freed. */
static void vlan_entry_del(struct rtl8198c_eth *eth, u16 vid, u32 port)
{
	struct rtl8198c_vlan *v = vlan_find(eth, vid);
	u32 zero[8] = { 0 };
	int i;


	if (!v)
		return;

	/* Protocol rules pointing at this VLAN on this port would classify
	 * frames into a VLAN the port is no longer in. */
	for (i = 0; i < PBV_RULES; i++)
		if (eth->pbvlan_vid[i][port] == vid) {
			eth->pbvlan_vid[i][port] = 0;
			pbvlan_program(eth, i, port);
		}

	v->member &= ~BIT(port);
	v->untag &= ~BIT(port);
	if (v->member == BIT(6)) {
		rtl8198c_taci_write_entry(TBL_VLAN + vid * 32, zero, BIT(0) | BIT(3));
		eth->fid_vid[v->fid] = 0;
		memset(v, 0, sizeof(*v));
		return;
	}
	vlan_program(eth, v);
}

void rtl8198c_netif_vlan_init(struct rtl8198c_eth *eth,
			    const u8 macs[][ETH_ALEN])
{
	int i;

	/* One netif entry per netdev, matching the vendor's per-port netif
	 * table.  Word 0 is mac18_0:19 | vid:12 | valid:1, word 1 holds
	 * mac47_19.  The previous entry was a dump from another board (wrong
	 * MAC and mtu 46); build it from the running MAC instead. */
	memcpy(eth->port_mac, macs, sizeof(eth->port_mac));
	for (i = 0; i < ETH_PORTS; i++) {
		eth->port_mtu[i] = 1500;
		rtl8198c_netif_table_write(eth, i, eth->port_mac[i],
				  i ? VLAN_LAN : VLAN_WAN);
	}
	/* Native VLANs: port 0 (WAN) alone in VLAN 8, ports 1-4 (LAN)
	 * sharing VLAN 9, the CPU a member of both and everything untagged.
	 * l2_table_clear() clears the table (MEMCR bit 2), so they have to be
	 * re-created here.  A missing entry is invisible from the data path:
	 * an unknown VID is trapped to the CPU, so traffic still passes. */
	eth->fid_vid[L2_FID_WAN] = VLAN_WAN;
	eth->fid_vid[L2_FID_LAN] = VLAN_LAN;
	eth->vlans[0] = (struct rtl8198c_vlan) {
		.used = true, .vid = VLAN_WAN, .fid = L2_FID_WAN,
		.member = BIT(0) | BIT(6), .untag = BIT(0) | BIT(6),
	};
	eth->vlans[1] = (struct rtl8198c_vlan) {
		.used = true, .vid = VLAN_LAN, .fid = L2_FID_LAN,
		.member = 0x1E | BIT(6), .untag = 0x1E | BIT(6),
	};
	vlan_program(eth, &eth->vlans[0]);
	vlan_program(eth, &eth->vlans[1]);

	/* PVIDs: port 0 is the WAN port, ports 1-4 are LAN.  PVCR0-4 are at
	 * 0x4A08+; 0x4A1C and 0x4A20 are PBVCR0/PBVCR1 (protocol-based VLAN),
	 * which the code above writes by mistake.  The bootloader writes both
	 * the low PVID and the 27:16 field, so do the same. */
	sys_w32(PVCR0, ((VLAN_WAN) << 16) | VLAN_WAN);
	for (i = 1; i < ETH_PORTS; i++)
		sys_w32(PVCR0 + i * 4, (VLAN_LAN << 16) | VLAN_LAN);

	/* Enforce VLAN membership on ingress as well as egress.  Without
	 * this a port can inject tagged frames into a VLAN it is not a
	 * member of (measured); the BSP clears the filter, citing mantis
	 * 2656, but with the PVIDs and membership programmed here enabling
	 * it keeps untagged traffic working.  Bits 0-5 are the physical and
	 * extension MACs; the CPU ports (6-8) stay unfiltered so that frames
	 * we transmit are not checked. */
	sys_w32(VCR0, (sys_r32(VCR0) & ~0x1FF) | 0x3F);

	/* VLAN-based decision: the port's PVID selects the VLAN.  The old
	 * port-based mode (LIMDBC_PORT) depends on the PLITIMR port->netif
	 * mapping and left port 3 unswitched/unlearned.  EnUkVIDtoCPU keeps
	 * unknown-VID frames trapped to the CPU. */
	sys_w32(SWTCR0, (sys_r32(SWTCR0) & ~LIMDBC_MASK) | LIMDBC_VLAN |
		EN_UKVID_TO_CPU);
	sys_w32(PLITIMR, 0);
}

/* A static entry with a zero member mask and toCPU clear is matched by the
 * switch and forwarded nowhere, i.e. the frame is dropped.  It has to exist
 * in every database, otherwise it only blocks the VLAN whose fid it landed
 * in. */
static void mac_filter_apply(struct rtl8198c_eth *eth)
{
	u32 fid;

	for (fid = 0; fid < L2_FIDS; fid++) {
		if (eth->mac_filter_enabled)
			rtl8198c_l2_fdb_add(eth->mac_filter, 0, 0, false, fid);
		else
			rtl8198c_l2_fdb_del(eth->mac_filter, fid);
	}
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

/* debugfs "fdb": the switch's own L2 table.
 *
 *   cat fdb                             -> every valid entry
 *   echo "add <mac> [portmask]" > fdb   -> static entry, portmask defaults
 *                                          to all LAN ports (0x3f)
 *   echo "del <mac>" > fdb
 *   echo flush > fdb                    -> drop every learned entry
 *
 * This is the raw interface: it takes an explicit port mask, which the
 * netdev FDB ops cannot express - they name one port.
 */
static int fdb_show(struct seq_file *s, void *v)
{
	struct l2_fdb_entry f;
	u32 e[8];
	u16 row;
	u8 col;

	for (row = 0; row < L2_ROWS; row++) {
		for (col = 0; col < L2_COLS; col++) {
			taci_read_entry(l2_entry_taa(row, col), e);
			if (!l2_entry_valid(e))
				continue;
			l2_entry_parse(e, row, col, &f);
			seq_printf(s, "%3u:%u fid=%u %pM ports=0x%02x cpu=%u %s age=%us\n",
				   row, col, f.fid, f.mac, f.member, f.to_cpu,
				   f.is_static ? "static" : "learned",
				   f.aging * L2_AGE_STEP);
		}
	}
	return 0;
}

static ssize_t fdb_parse_mac(const char *str, u8 mac[ETH_ALEN])
{
	unsigned int b[ETH_ALEN];
	int i;

	if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
		   &b[4], &b[5]) != ETH_ALEN)
		return -EINVAL;
	for (i = 0; i < ETH_ALEN; i++) {
		if (b[i] > 0xFF)
			return -EINVAL;
		mac[i] = b[i];
	}
	return 0;
}

static ssize_t fdb_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[64];
	unsigned int mask = L2_MEMBER_ALL, fid = 0;
	u8 mac[ETH_ALEN];
	ssize_t len;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (!strcmp(kbuf, "flush")) {
		l2_fdb_flush_dynamic(L2_FIDS);
		return len;
	}

	if (!strncmp(kbuf, "add ", 4)) {
		char *p = kbuf + 4;
		char *sp = strchr(p, ' ');

		if (sp) {
			*sp = '\0';
			if (sscanf(sp + 1, "%x %x", &mask, &fid) < 1)
				return -EINVAL;
		}
		if (fdb_parse_mac(p, mac))
			return -EINVAL;
		if (fid >= L2_FIDS)
			return -EINVAL;
		/* An explicit port mask means "pin to these ports only", so
		 * the CPU gets no copy. */
		if (rtl8198c_l2_fdb_add(mac, mask & L2_MEMBER_ALL, 0, false, fid))
			return -ENOSPC;
		return len;
	}

	if (!strncmp(kbuf, "del ", 4)) {
		char *p = kbuf + 4;
		char *sp = strchr(p, ' ');
		int i;

		if (sp) {
			*sp = '\0';
			if (sscanf(sp + 1, "%x", &fid) != 1 || fid >= L2_FIDS)
				return -EINVAL;
		}
		if (fdb_parse_mac(p, mac))
			return -EINVAL;
		if (is_broadcast_ether_addr(mac))
			return -EPERM;
		for (i = 0; i < ETH_PORTS; i++)
			if (eth->netdev[i] &&
			    ether_addr_equal(mac, eth->netdev[i]->dev_addr))
				return -EPERM;
		return rtl8198c_l2_fdb_del(mac, fid) ? -ENOENT : len;
	}

	return -EINVAL;
}

static int fdb_open(struct inode *inode, struct file *file)
{ return single_open(file, fdb_show, inode->i_private); }
static const struct file_operations fdb_fops = {
	.owner = THIS_MODULE, .open = fdb_open,
	.read = seq_read, .write = fdb_write,
	.llseek = seq_lseek, .release = single_release,
};

/* debugfs "l2_aging": global L2 aging mode (on / off / fast).  The switch
 * ages in 150 s steps; there is no global aging-time register on this core. */
static int l2_aging_show(struct seq_file *s, void *unused)
{
	u32 teacr = sys_r32(TEACR);

	seq_printf(s, "%s\n", (teacr & L2_AGING_DISABLE) ? "off" :
		   (teacr & EN_L2_FAST_AGING) ? "fast" : "on");
	return 0;
}

static ssize_t l2_aging_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	char kbuf[8];
	ssize_t len;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';
	if (strcmp(kbuf, "on") && strcmp(kbuf, "off") && strcmp(kbuf, "fast"))
		return -EINVAL;
	l2_aging_set(kbuf);
	return len;
}

static int l2_aging_open(struct inode *inode, struct file *file)
{ return single_open(file, l2_aging_show, inode->i_private); }
static const struct file_operations l2_aging_fops = {
	.owner = THIS_MODULE, .open = l2_aging_open,
	.read = seq_read, .write = l2_aging_write,
	.llseek = seq_lseek, .release = single_release,
};

/* debugfs "pbvlan": protocol-based VLAN.
 *
 *   cat pbvlan                          -> rules, EtherTypes, assignments
 *   echo "user1 ethertype 0x88b5" > pbvlan
 *   echo "user1 port 3 vid 20" > pbvlan
 *   echo "user1 port 3 off" > pbvlan
 *
 * An untagged frame whose protocol matches a rule is classified into that
 * VLAN instead of the port's PVID.  Rules 1-4 (ipx, netbios, pppoe-control,
 * pppoe-session) are fixed in hardware; user1/user2 take an EtherType.
 * The target VLAN must exist and the port must be a member. */
static int pbvlan_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;
	int r, p;

	for (r = 0; r < PBV_RULES; r++) {
		if (r >= 4)
			seq_printf(s, "%-14s ethertype 0x%04x", pbvlan_name[r],
				   eth->pbvlan_ethertype[r - 4]);
		else
			seq_printf(s, "%-14s (fixed)          ", pbvlan_name[r]);
		for (p = 0; p < ETH_PORTS; p++) {
			if (eth->pbvlan_vid[r][p])
				seq_printf(s, " p%d:v%u", p,
					   eth->pbvlan_vid[r][p]);
			else
				seq_printf(s, " p%d:-", p);
		}
		seq_putc(s, '\n');
	}
	return 0;
}

static ssize_t pbvlan_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	struct rtl8198c_vlan *v;
	char kbuf[64], name[16], what[8];
	unsigned int port, vid, etype;
	ssize_t len;
	int r;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (sscanf(kbuf, "%15s ethertype %x", name, &etype) == 2) {
		for (r = 0; r < PBV_RULES; r++)
			if (!strcmp(name, pbvlan_name[r]))
				break;
		if (r < 4 || r == PBV_RULES || etype > 0xFFFF)
			return -EINVAL;
		eth->pbvlan_ethertype[r - 4] = etype;
		pbvlan_define(eth, r);
		return len;
	}

	if (sscanf(kbuf, "%15s port %u vid %u", name, &port, &vid) == 3) {
		for (r = 0; r < PBV_RULES; r++)
			if (!strcmp(name, pbvlan_name[r]))
				break;
		if (r == PBV_RULES || port >= ETH_PORTS || vid == 0)
			return -EINVAL;
		v = vlan_find(eth, vid);
		if (!v || !(v->member & BIT(port)))
			return -EINVAL;
		eth->pbvlan_vid[r][port] = vid;
		pbvlan_program(eth, r, port);
		return len;
	}

	if (sscanf(kbuf, "%15s port %u %7s", name, &port, what) == 3 &&
	    !strcmp(what, "off")) {
		for (r = 0; r < PBV_RULES; r++)
			if (!strcmp(name, pbvlan_name[r]))
				break;
		if (r == PBV_RULES || port >= ETH_PORTS)
			return -EINVAL;
		eth->pbvlan_vid[r][port] = 0;
		pbvlan_program(eth, r, port);
		return len;
	}

	return -EINVAL;
}

static int pbvlan_open(struct inode *inode, struct file *file)
{ return single_open(file, pbvlan_show, inode->i_private); }
static const struct file_operations pbvlan_fops = {
	.owner = THIS_MODULE, .open = pbvlan_open,
	.read = seq_read, .write = pbvlan_write,
	.llseek = seq_lseek, .release = single_release,
};

/* ===== FDB interface =====
 *
 * Every port has its own netdev, so an entry added here is installed on that
 * netdev's port - "bridge fdb add <mac> dev eth4" pins the MAC to port 4 -
 * and a dump lists the switch's own table, scoped to the calling device.
 * The netlink API names a single port, so a mask needs debugfs "fdb".
 */
int rtl8198c_eth_fdb_add(struct ndmsg *ndm, struct nlattr *tb[],
		       struct net_device *dev, const unsigned char *addr,
		       u16 vid, u16 flags, bool *notified,
		       struct netlink_ext_ack *extack)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	struct rtl8198c_vlan *v;
	u32 fid = p->fid;

	if (vid) {
		v = vlan_find(eth, vid);
		if (!v) {
			NL_SET_ERR_MSG_MOD(extack, "VLAN is not configured");
			return -EOPNOTSUPP;
		}
		fid = v->fid;
	}
	if (is_broadcast_ether_addr(addr) ||
	    ether_addr_equal(addr, dev->dev_addr)) {
		NL_SET_ERR_MSG_MOD(extack, "refusing to rewrite a reserved entry");
		return -EPERM;
	}
	/* Static entry for the port the caller named: forward there and only
	 * there.  A CPU copy is not wanted - the switch forwards the frame. */
	return rtl8198c_l2_fdb_add(addr, BIT(p->port), 0, false, fid);
}

int rtl8198c_eth_fdb_del(struct ndmsg *ndm, struct nlattr *tb[],
		       struct net_device *dev, const unsigned char *addr,
		       u16 vid, bool *notified,
		       struct netlink_ext_ack *extack)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	struct rtl8198c_vlan *v;
	u32 fid = p->fid;

	if (vid) {
		v = vlan_find(eth, vid);
		if (!v) {
			NL_SET_ERR_MSG_MOD(extack, "VLAN is not configured");
			return -EOPNOTSUPP;
		}
		fid = v->fid;
	}
	if (is_broadcast_ether_addr(addr) ||
	    ether_addr_equal(addr, dev->dev_addr)) {
		NL_SET_ERR_MSG_MOD(extack, "refusing to delete a reserved entry");
		return -EPERM;
	}
	return rtl8198c_l2_fdb_del(addr, fid);
}

/* "bridge fdb flush": delete learned entries, keep static ones. */
int rtl8198c_eth_fdb_del_bulk(struct nlmsghdr *nlh, struct net_device *dev,
			    struct netlink_ext_ack *extack)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	struct ndmsg *ndm = nlmsg_data(nlh);
	struct nlattr *tb[NDA_MAX + 1];
	u32 fid = L2_FIDS;	/* all databases */
	int err;

	err = nlmsg_parse(nlh, sizeof(*ndm), tb, NDA_MAX, NULL, extack);
	if (err)
		return err;
	if (tb[NDA_VLAN]) {
		struct rtl8198c_vlan *v = vlan_find(eth,
						    nla_get_u16(tb[NDA_VLAN]));

		if (!v) {
			NL_SET_ERR_MSG_MOD(extack, "VLAN is not configured");
			return -EOPNOTSUPP;
		}
		fid = v->fid;
	}
	l2_fdb_flush_dynamic(fid);
	return 0;
}

static u16 fid_vid(const struct rtl8198c_eth *eth, u32 fid)
{
	return fid < L2_FIDS ? eth->fid_vid[fid] : 0;
}

static int eth_fdb_dump_one(struct sk_buff *skb, struct netlink_callback *cb,
			    struct net_device *dev,
			    const struct l2_fdb_entry *f)
{
	u32 portid = NETLINK_CB(cb->skb).portid;
	u32 seq = cb->nlh->nlmsg_seq;
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;

	nlh = nlmsg_put(skb, portid, seq, RTM_NEWNEIGH, sizeof(*ndm),
			NLM_F_MULTI);
	if (!nlh)
		return -EMSGSIZE;
	ndm = nlmsg_data(nlh);
	memset(ndm, 0, sizeof(*ndm));
	ndm->ndm_family = AF_BRIDGE;
	ndm->ndm_flags = NTF_SELF;
	ndm->ndm_ifindex = dev->ifindex;
	ndm->ndm_state = f->is_static ? NUD_NOARP : NUD_REACHABLE;
	if (nla_put(skb, NDA_LLADDR, ETH_ALEN, f->mac) ||
	    nla_put_u16(skb, NDA_VLAN,
			fid_vid(((struct rtl8198c_port *)netdev_priv(dev))->sw,
				f->fid))) {
		nlmsg_cancel(skb, nlh);
		return -EMSGSIZE;
	}
	nlmsg_end(skb, nlh);
	return 0;
}

int rtl8198c_eth_fdb_dump(struct sk_buff *skb, struct netlink_callback *cb,
			struct net_device *dev, struct net_device *filter_dev,
			int *idx)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;
	struct rtl8198c_vlan *v;
	struct ndo_fdb_dump_context *ctx = (void *)cb->ctx;
	struct l2_fdb_entry f;
	u32 e[8];
	u16 row;
	u8 col;
	int n = 0, err;

	for (row = 0; row < L2_ROWS; row++) {
		for (col = 0; col < L2_COLS; col++) {
			taci_read_entry(l2_entry_taa(row, col), e);
			if (!l2_entry_valid(e))
				continue;
			l2_entry_parse(e, row, col, &f);
			/* The table is switch-wide.  Report an entry under the
			 * port it forwards to, and a port's own CPU-facing
			 * address under that port; dumping all of it on every
			 * netdev made "bridge fdb show" repeat every entry on
			 * eth0-eth4 and once per bridge port. */
			if (!(f.to_cpu &&
			      ether_addr_equal(f.mac, dev->dev_addr))) {
				v = vlan_find(eth, eth->fid_vid[f.fid]);
				if (!v || !(v->member & BIT(p->port)) ||
				    !(f.member & BIT(p->port)))
					continue;
			}
			if (n >= ctx->fdb_idx) {
				err = eth_fdb_dump_one(skb, cb, dev, &f);
				if (err) {
					*idx = n;
					return err;
				}
			}
			n++;
		}
	}
	*idx = n;
	return 0;
}

/* VLAN devices on a port register their vid here.  The switch has to know the
 * vid to forward its frames between ports and to give it a filter database of
 * its own; the kernel's software tag path then works unchanged because the
 * CPU path keeps tags. */
int rtl8198c_eth_vlan_rx_add_vid(struct net_device *dev, __be16 proto, u16 vid)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;

	/* Only 802.1Q.  The switch parses nothing but an 0x8100 outer tag:
	 * measured, an 0x88a8 frame is classified as untagged into the
	 * port's PVID, so an 802.1ad VLAN device would silently place that
	 * traffic in the native VLAN instead of the S-VID.  The VLAN table
	 * has member/untag/fid fields only - there is no outer-tag or TPID
	 * register - and the BSP has no stacking path either. */
	if (proto != htons(ETH_P_8021Q))
		return -EOPNOTSUPP;
	if (!vid || vid >= 4096)
		return -EINVAL;
	/* The native VLAN is always a member; a VLAN device on top of it
	 * needs nothing programmed. */
	if (vid == p->vid)
		return 0;
	return vlan_entry_add(eth, vid, p->port, p->vid);
}

int rtl8198c_eth_vlan_rx_kill_vid(struct net_device *dev, __be16 proto, u16 vid)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	struct rtl8198c_eth *eth = p->sw;

	if (proto != htons(ETH_P_8021Q))
		return -EOPNOTSUPP;	/* no 802.1ad, see add_vid */
	if (vid == p->vid)
		return 0;
	vlan_entry_del(eth, vid, p->port);
	return 0;
}

/* ===== Spanning tree =====
 *
 * The switch enforces the 802.1D port states only while MSCR.EN_STP is set;
 * with the bit clear a "blocking" port still forwards (measured).  BPDUs are
 * a separate path: RMACR.MADDR00 traps 01:80:c2:00:00:00 to the CPU for
 * every ingress port whatever its state, which is what lets a blocked port
 * take part in the election - without the trap the frame is dropped at the
 * blocked port and never reaches the bridge.  Both bits are what the BSP's
 * rtl8651_setAsicSpanningEnable() sets; the MAC itself (link, speed,
 * duplex) is not touched by a state change.
 *
 * The state write is rtl865xC_setAsicSpanningTreePortState(); the
 * EnForceMode double toggle it ends with is a 8196C-revision-B workaround
 * that compiles to nothing for this chip, and the state latches without it
 * (measured: the port stops forwarding the moment EN_STP goes up, and the
 * link stays at 1G).
 */
void rtl8198c_stp_enable(bool on)
{
	u32 v;

	v = sys_r32(MSCR);
	v = on ? (v | MSCR_EN_STP) : (v & ~MSCR_EN_STP);
	sys_w32(MSCR, v);
	v = sys_r32(RMACR);
	v = on ? (v | RMACR_MADDR00) : (v & ~RMACR_MADDR00);
	sys_w32(RMACR, v);
}

/* One port's hardware state, as PCRP_STP_*.  The caller has already mapped
 * the bridge's five states onto the hardware's four. */
void rtl8198c_stp_port_state(u32 port, u32 state)
{
	u32 v;

	if (port >= ETH_PORTS)
		return;
	v = sys_r32(PCRP0 + port * 4);
	sys_w32(PCRP0 + port * 4, (v & ~PCRP_STP_MASK) | state);
}

/* The debugfs knobs this block owns; the registration itself is central so
 * probe creates one directory. */
void rtl8198c_l2_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("mac_filter", 0600, eth->debugfs_dir, eth, &mac_filter_fops);
	debugfs_create_file("fdb", 0600, eth->debugfs_dir, eth, &fdb_fops);
	debugfs_create_file("l2_aging", 0600, eth->debugfs_dir, eth, &l2_aging_fops);
	debugfs_create_file("pbvlan", 0600, eth->debugfs_dir, eth, &pbvlan_fops);
}
