// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L4: NAPT sessions
 *
 * Entry encoding, the 5-tuple hash and the table access for the TCP/UDP
 * session table (TACI type 9) and the external-IP table it points into
 * (type 5), plus the debugfs interface that drives both by hand.
 *
 * A session entry does not store its key.  The switch hashes the packet's
 * 5-tuple to find the table index, and the entry only carries the rewrite:
 * the inside address and port, the external port (split across fields) and
 * the index of the external address in the external-IP table.  The driver
 * therefore has to reproduce the hardware hash exactly, and it keeps the
 * tuple of every entry it installed so that a collision or a stale entry
 * can be told apart from a session of ours.
 *
 * Direction is decided by the entry itself:
 *  - outbound (LAN -> WAN, source rewrite): hashed on the inside tuple and
 *    the peer; holds the external port and the external-IP index.
 *  - inbound (WAN -> LAN, destination rewrite): hashed on the peer and the
 *    external address; holds the inside address and port, the low 10 bits
 *    of the external port and the enhanced-hash1 verification value.
 *
 * Hardware facts (BSP rtl865x_asicL4.c/.h, bootcode swCore.c):
 *  - 1024 sessions, 10-bit index.  Enhanced hash1 (SWTCR1 bit 13) is the
 *    only hash mode this driver uses; four-way hash stays off.
 *  - The verification value ("very") is the hash of the peer address and
 *    port alone, folded with the VERI bit set.  The hardware recomputes it
 *    from the incoming packet and compares it with selEIdx.
 *  - The external port is stored in two pieces: outbound as offset (bits
 *    10-15) and selEIdx (bits 0-9), inbound as offset (bits 0-5) and
 *    selIPIdx (bits 6-9).  The high bits of an inbound port survive only in
 *    the index the entry was written to.
 *  - The ICMP echo, server-port and ALG tables have TACI type numbers but
 *    no silicon behind them on this core ("SERVER_PORT_TABLE, ALG_TABLE and
 *    L4_ICMP_TABLE are removed in real chip", bootcode swCore.c).  ICMP NAT
 *    therefore cannot be offloaded, and an ICMP error is trapped to the CPU
 *    by SWTCR0.NAPTF2CPU instead.
 *  - Entries age out even when isStatic is set; the valid bit is what the
 *    aging engine clears.  The idle timeout is the non-linear 6-bit value
 *    TEATCR stores (120 s and 90 s are the vendor's TCP and UDP defaults).
 *  - With SWTCR0.NAPTR_NOT_FOUND_DROP clear a packet with no session goes
 *    to the CPU, which is what lets software see the first packet of every
 *    flow.  EnNAPTAutoLearn stays off and EnNAPTAutoDelete stays on: the
 *    driver, not the switch, decides what is in the table.
 *
 * BSP counterparts: AsicDriver/rtl865x_asicL4.c
 * (rtl8651_naptTcpUdpTableIndex, rtl8651_setAsicNaptTcpUdpTable,
 * rtl8651_setAsicLiberalNaptTcpUdpTable), l4Driver/rtl865x_nat.c (the
 * entry each direction gets and where the external port comes from),
 * l3Driver/rtl865x_ip.c (external-IP entries).
 */
#include "rtl8198c.h"

#include <linux/inet.h>
#include <linux/jiffies.h>
#include <net/flow_offload.h>

/* ===== Idle-timeout encoding =====
 *
 * TEATCR holds five 6-bit fields (TCP fast/medium/long, UDP, ICMP) whose
 * value is not seconds but a logarithmic unit: each group of eight units
 * quadruples the granularity.  The C is the vendor's
 * _rtl8651_NaptAgingToUnit()/_rtl8651_NaptAgingToSec(), including their
 * rounding, so a value read back from the table decodes the way the
 * vendor's own dump decodes it.
 */
static u8 l4_aging_encode(u32 sec)
{
	u32 value = 0, s = 1;

	sec++;
	sec = sec * 3 / 5;
	while (sec >= (s << 3)) {
		sec -= s << 3;
		s <<= 2;
		value++;
	}
	return (sec % s > (s >> 1)) ? (value << 3) + (sec / s) + 1
				    : (value << 3) + (sec / s);
}

static u32 l4_aging_secs(u8 unit)
{
	u32 v = unit & 0x7, g = unit >> 3, s = 1, u = 0;

	while (g--) {
		u += s << 3;
		s <<= 2;
	}
	u += s * (v + 1);
	return (u - 1) * 5 / 3 - (s > 1 ? s : 0);
}

/* ===== 5-tuple hash =====
 *
 * The BSP documents the hardware fold above
 * rtl8651_naptTcpUdpTableIndex(): every 10-bit slice of
 * {sip, sport, tcp, dip, dport} is XORed together.
 *
 * The input is the value on the wire, not the little-endian memory word:
 * 192.0.2.1 is 0xC0000201 and port 80 is 80.  The vendor passes htonl() of
 * its host-order tuple, which yields exactly that.
 *
 * With the VERI bit the switch hashes what the enhanced hash1 verification
 * needs: the peer side only, destination zeroed.  rtl8198c_l4_hash() with a
 * zero destination computes the same value; the VERI bit only selects the
 * hash1 formula, which is the only one implemented here anyway.
 */
u32 rtl8198c_l4_hash(bool tcp, __be32 sip, __be16 sport, __be32 dip,
		     __be16 dport)
{
	u32 sa = ntohl(sip), da = ntohl(dip);
	u32 sp = ntohs(sport), dp = ntohs(dport);
	u32 t = tcp ? 1 : 0;

	return ((sp & 0x3ff) ^
		(((sp & 0xfc00) >> 10) | ((sa & 0xf) << 6)) ^
		((sa >> 4) & 0x3ff) ^
		((sa >> 14) & 0x3ff) ^
		(((sa >> 24) & 0xff) | (t << 8) | ((dp & 1) << 9)) ^
		((dp >> 1) & 0x3ff) ^
		(((dp >> 11) & 0x1f) | ((da & 0x1f) << 5)) ^
		((da >> 5) & 0x3ff) ^
		((da >> 15) & 0x3ff) ^
		((da >> 25) & 0x7f)) & L4_HASH_MASK;
}

/* ===== Session entry encoding =====
 *
 * Word 1 is fields, word 2 the inside port plus the TCP/UDP bits; the
 * layout is rtl865xc_tblAsic_naptTcpUdpTable_t (little-endian build).  The
 * TCPFlag value is the vendor's dynamic encoding: bit 1 says the entry
 * takes part in both directions, bit 0 names this entry's direction.  With
 * dedicate clear the field means "wait-for" states to the TCP engine; the
 * values below are the ones rtl865x_nat.c writes for a live connection.
 */
void rtl8198c_l4_session_out(u32 e[8], __be32 int_ip, __be16 int_port,
			     __be16 ext_port, u8 ext_idx, bool tcp, u8 aging)
{
	u32 ep = ntohs(ext_port);

	memset(e, 0, 8 * sizeof(u32));
	e[0] = ip_to_word(int_ip);
	e[1] = BIT(0) | BIT(1) |			/* valid, collision */
	       ((u32)(aging & TEATCR_FIELD) << 2) |
	       ((ep >> 10) << 8) |			/* offset */
	       BIT(14) |				/* collision2 */
	       BIT(16) |				/* isStatic */
	       ((u32)(ext_idx & 0xf) << 17) |
	       ((ep & 0x3ff) << 21);			/* selEIdx */
	e[2] = ntohs(int_port) | (3u << 16) | (tcp ? BIT(19) : 0);
}

void rtl8198c_l4_session_in(u32 e[8], __be32 int_ip, __be16 int_port,
			    __be32 rem_ip, __be16 rem_port, __be16 ext_port,
			    bool tcp, u8 aging)
{
	u32 ep = ntohs(ext_port);
	u32 very = rtl8198c_l4_hash(tcp, rem_ip, rem_port, 0, 0);

	memset(e, 0, 8 * sizeof(u32));
	e[0] = ip_to_word(int_ip);
	e[1] = BIT(0) | BIT(1) |
	       ((u32)(aging & TEATCR_FIELD) << 2) |
	       ((ep & 0x3f) << 8) |			/* offset */
	       BIT(14) |
	       BIT(16) |
	       (((ep & 0x3ff) >> 6) << 17) |		/* selIPIdx */
	       (very << 21);				/* selEIdx */
	e[2] = ntohs(int_port) | (2u << 16) | (tcp ? BIT(19) : 0);
}

/* ===== External-IP table =====
 *
 * One entry per NAT address, reference counted by the sessions that use
 * it.  A masquerade entry has no inside address (the session carries that),
 * so the internal-IP field stays zero.
 */
static int l4_extip_get(struct rtl8198c_eth *eth, __be32 ext_ip, u8 *idx)
{
	int free = -1, i;

	for (i = 0; i < L4_EXT_IPS; i++) {
		if (eth->l4.extip[i].used) {
			if (eth->l4.extip[i].ext_ip == ext_ip) {
				eth->l4.extip[i].users++;
				*idx = i;
				return 0;
			}
		} else if (free < 0) {
			free = i;
		}
	}
	if (free < 0)
		return -ENOSPC;

	eth->l4.extip[free].used = true;
	eth->l4.extip[free].users = 1;
	eth->l4.extip[free].ext_ip = ext_ip;
	*idx = free;
	return 0;
}

static void l4_extip_put(struct rtl8198c_eth *eth, __be32 ext_ip)
{
	const u32 zero[8] = { 0 };
	int i;

	for (i = 0; i < L4_EXT_IPS; i++) {
		if (!eth->l4.extip[i].used ||
		    eth->l4.extip[i].ext_ip != ext_ip)
			continue;
		if (--eth->l4.extip[i].users)
			return;
		memset(&eth->l4.extip[i], 0, sizeof(eth->l4.extip[i]));
		rtl8198c_l3_table_write(TBL_EXTIP, i, zero);
		return;
	}
}

static void l4_extip_write(struct rtl8198c_eth *eth, u8 idx)
{
	u32 e[8];

	rtl8198c_l3_extip_build(e, 0, eth->l4.extip[idx].ext_ip, 0,
				false, false);
	rtl8198c_l3_table_write(TBL_EXTIP, idx, e);
}

/* ===== Session table access =====
 *
 * The walk order is the table order, so a session always lands on the index
 * its tuple hashes to; a second tuple that hashes to the same index keeps
 * the flow in software instead of overwriting a live rewrite.
 */
static bool l4_tuple_eq(const struct rtl8198c_l4_tuple *a,
			const struct rtl8198c_l4_tuple *b)
{
	return a->tcp == b->tcp && a->int_ip == b->int_ip &&
	       a->rem_ip == b->rem_ip && a->int_port == b->int_port &&
	       a->rem_port == b->rem_port && a->ext_ip == b->ext_ip &&
	       a->ext_port == b->ext_port;
}

static void l4_session_clear(struct rtl8198c_eth *eth, u32 index)
{
	const u32 zero[8] = { 0 };

	rtl8198c_l3_table_write(TBL_TCP_UDP, index, zero);
	memset(&eth->l4.sess[index], 0, sizeof(eth->l4.sess[index]));
}

static void l4_flush_locked(struct rtl8198c_eth *eth)
{
	const u32 zero[8] = { 0 };
	int i;

	for (i = 0; i < L4_SESSIONS; i++) {
		rtl8198c_l3_table_write(TBL_TCP_UDP, i, zero);
		memset(&eth->l4.sess[i], 0, sizeof(eth->l4.sess[i]));
	}
	for (i = 0; i < L4_EXT_IPS; i++) {
		if (!eth->l4.extip[i].used)
			continue;
		memset(&eth->l4.extip[i], 0, sizeof(eth->l4.extip[i]));
		rtl8198c_l3_table_write(TBL_EXTIP, i, zero);
	}
}

void rtl8198c_l4_flush(struct rtl8198c_eth *eth)
{
	mutex_lock(&eth->l4.lock);
	l4_flush_locked(eth);
	mutex_unlock(&eth->l4.lock);
}

/* An nftables flow is offered once per direction (and once per flowtable
 * device), so the same tuple arrives more than once.  The cookies identify
 * the flow at teardown, when the flow itself may already be gone and the
 * cookie is all the kernel passes back. */
static void l4_cookie_add(struct rtl8198c_l4_session *s, unsigned long cookie)
{
	int i;

	if (!cookie)
		return;
	for (i = 0; i < s->ncookies; i++)
		if (s->cookie[i] == cookie)
			return;
	if (s->ncookies < ARRAY_SIZE(s->cookie))
		s->cookie[s->ncookies++] = cookie;
}

static struct rtl8198c_l4_session *
l4_by_cookie_locked(struct rtl8198c_eth *eth, unsigned long cookie)
{
	int i, j;

	if (!cookie)
		return NULL;
	for (i = 0; i < L4_SESSIONS; i++) {
		struct rtl8198c_l4_session *s = &eth->l4.sess[i];

		if (!s->used)
			continue;
		for (j = 0; j < s->ncookies; j++)
			if (s->cookie[j] == cookie)
				return s;
	}
	return NULL;
}

/* A session is only ours while the engine is armed; installing one with
 * EN_L4 clear would leave a rewrite that goes live the moment the knob is
 * turned on, without the flow that owns it. */
static int l4_flow_add_locked(struct rtl8198c_eth *eth,
			      const struct rtl8198c_l4_tuple *t, u32 aging,
			      unsigned long cookie)
{
	unsigned long cookies[ARRAY_SIZE(eth->l4.sess[0].cookie)] = { };
	u32 out, in;
	u32 e[8];
	u8 ext_idx, age, ncookies = 0;
	bool existing;
	int i, err;

	if (!aging)
		aging = t->tcp ? 120 : 90;
	age = l4_aging_encode(aging);

	out = rtl8198c_l4_hash(t->tcp, t->int_ip, t->int_port,
			       t->rem_ip, t->rem_port);
	in = rtl8198c_l4_hash(t->tcp, t->rem_ip, t->rem_port,
			      t->ext_ip, t->ext_port);
	if (in == out)
		return -EEXIST;

	if (!eth->l4.enabled)
		return -ENODEV;
	if (eth->l4.sess[out].used &&
	    !l4_tuple_eq(&eth->l4.sess[out].t, t))
		return -EEXIST;
	if (eth->l4.sess[in].used &&
	    !l4_tuple_eq(&eth->l4.sess[in].t, t))
		return -EEXIST;

	/* The inbound row carries the inside endpoint against the external
	 * port only; the remote endpoint survives as the verification hash.
	 * Two inside endpoints sharing one external port would therefore
	 * fight over the rewrite, so keep the first and leave the rest in
	 * software (masquerade can collide on a preserved source port). */
	for (i = 0; i < L4_SESSIONS; i++) {
		struct rtl8198c_l4_session *s = &eth->l4.sess[i];

		if (!s->used || s->t.tcp != t->tcp ||
		    s->t.ext_ip != t->ext_ip || s->t.ext_port != t->ext_port)
			continue;
		if (s->t.int_ip != t->int_ip || s->t.int_port != t->int_port)
			return -EEXIST;
	}

	/* A rewrite of the same tuple is not another user of the address. */
	existing = (eth->l4.sess[out].used &&
		    l4_tuple_eq(&eth->l4.sess[out].t, t)) ||
		   (eth->l4.sess[in].used &&
		    l4_tuple_eq(&eth->l4.sess[in].t, t));
	if (existing) {
		struct rtl8198c_l4_session *s = eth->l4.sess[out].used ?
						&eth->l4.sess[out] :
						&eth->l4.sess[in];

		ncookies = s->ncookies;
		memcpy(cookies, s->cookie, sizeof(cookies));
		for (ext_idx = 0; ext_idx < L4_EXT_IPS; ext_idx++)
			if (eth->l4.extip[ext_idx].used &&
			    eth->l4.extip[ext_idx].ext_ip == t->ext_ip)
				break;
		if (ext_idx == L4_EXT_IPS)
			return -ENOENT;
	} else {
		err = l4_extip_get(eth, t->ext_ip, &ext_idx);
		if (err)
			return err;
		l4_extip_write(eth, ext_idx);
	}

	rtl8198c_l4_session_out(e, t->int_ip, t->int_port, t->ext_port,
				ext_idx, t->tcp, age);
	rtl8198c_l3_table_write(TBL_TCP_UDP, out, e);
	rtl8198c_l4_session_in(e, t->int_ip, t->int_port, t->rem_ip,
			       t->rem_port, t->ext_port, t->tcp, age);
	rtl8198c_l3_table_write(TBL_TCP_UDP, in, e);

	eth->l4.sess[out].t = *t;
	eth->l4.sess[out].aging = age;
	memcpy(eth->l4.sess[out].cookie, cookies, sizeof(cookies));
	eth->l4.sess[out].ncookies = ncookies;
	l4_cookie_add(&eth->l4.sess[out], cookie);
	eth->l4.sess[out].used = true;
	eth->l4.sess[in] = eth->l4.sess[out];
	return 0;
}

int rtl8198c_l4_flow_add(struct rtl8198c_eth *eth,
			 const struct rtl8198c_l4_tuple *t, u32 aging)
{
	int err;

	mutex_lock(&eth->l4.lock);
	err = l4_flow_add_locked(eth, t, aging, 0);
	mutex_unlock(&eth->l4.lock);
	return err;
}

static bool l4_flow_del_locked(struct rtl8198c_eth *eth,
			       const struct rtl8198c_l4_tuple *t)
{
	u32 out, in;
	int i;

	for (i = 0; i < L4_SESSIONS; i++) {
		struct rtl8198c_l4_tuple *m = &eth->l4.sess[i].t;
		__be32 ext_ip;

		if (!eth->l4.sess[i].used || m->tcp != t->tcp ||
		    m->int_ip != t->int_ip || m->int_port != t->int_port ||
		    m->rem_ip != t->rem_ip || m->rem_port != t->rem_port)
			continue;
		out = rtl8198c_l4_hash(m->tcp, m->int_ip, m->int_port,
				       m->rem_ip, m->rem_port);
		in = rtl8198c_l4_hash(m->tcp, m->rem_ip, m->rem_port,
				      m->ext_ip, m->ext_port);
		ext_ip = m->ext_ip;
		l4_session_clear(eth, out);
		l4_session_clear(eth, in);
		l4_extip_put(eth, ext_ip);
		return true;
	}
	return false;
}

int rtl8198c_l4_flow_del(struct rtl8198c_eth *eth,
			 const struct rtl8198c_l4_tuple *t)
{
	bool found;

	mutex_lock(&eth->l4.lock);
	found = l4_flow_del_locked(eth, t);
	mutex_unlock(&eth->l4.lock);
	return found ? 0 : -ENOENT;
}

static bool rtl8198c_l4_flow_del_cookie(struct rtl8198c_eth *eth,
					unsigned long cookie)
{
	struct rtl8198c_l4_session *s;
	struct rtl8198c_l4_tuple t;
	bool found = false;

	mutex_lock(&eth->l4.lock);
	s = l4_by_cookie_locked(eth, cookie);
	if (s) {
		t = s->t;
		found = l4_flow_del_locked(eth, &t);
	}
	mutex_unlock(&eth->l4.lock);
	return found;
}

/* ===== Engine =====
 *
 * Arming the L4 stage is the last step: the external-IP and session tables
 * have to be in place first, exactly as the vendor adds them before it
 * calls setAsicOperationLayer(4).  Disarming flushes both tables, because a
 * session left behind would go live again on the next enable without the
 * flow that owns it.
 *
 * NAPT only runs on the hardware-routed path: without route_offload the
 * switch hands every routed frame to the CPU and the session table is never
 * consulted.  The knob therefore depends on route_offload, and l3_enable()
 * disarms this engine when the routing knob goes away.
 */
static void l4_timeout_set(u32 shift, u32 sec)
{
	u32 v = sys_r32(TEATCR) & ~(TEATCR_FIELD << shift);

	sys_w32(TEATCR, v | ((u32)l4_aging_encode(sec) << shift));
}

void rtl8198c_l4_engine(struct rtl8198c_eth *eth, bool on)
{
	u32 v;

	mutex_lock(&eth->l4.lock);
	if (on) {
		if (eth->l4.enabled)
			goto out;

		l4_timeout_set(TEATCR_TCP_FAST_SHIFT, 120);
		l4_timeout_set(TEATCR_TCP_MED_SHIFT, 120);
		l4_timeout_set(TEATCR_TCP_LONG_SHIFT, 120);
		l4_timeout_set(TEATCR_UDP_SHIFT, 90);

		/* A session miss must reach the CPU: that is what lets the
		 * kernel see the first packet of every flow.  The driver owns
		 * the table, so autolearn stays off while autodelete ages the
		 * rows the hardware no longer sees. */
		v = sys_r32(SWTCR0);
		v &= ~(SWTCR0_NAPT_DROP_REVERSE | SWTCR0_NAPT_AUTOLEARN);
		v |= SWTCR0_NAPT_AUTODELETE | SWTCR0_NAPT_F2CPU;
		sys_w32(SWTCR0, v);
		sys_w32(SWTCR1, sys_r32(SWTCR1) | SWTCR1_L4_ENHASH1);
		sys_w32(TEACR, sys_r32(TEACR) & ~L4_AGING_DISABLE);
		eth->l4.enabled = true;
		sys_w32(MSCR, sys_r32(MSCR) | MSCR_EN_L4);
	} else {
		if (!eth->l4.enabled)
			goto out;

		sys_w32(MSCR, sys_r32(MSCR) & ~MSCR_EN_L4);
		eth->l4.enabled = false;
		l4_flush_locked(eth);
	}
out:
	mutex_unlock(&eth->l4.lock);
}

/* MEMCR does not cover the session table and the switch has no per-table
 * reset for it, so entries from a previous boot would be live as soon as
 * EN_L4 is armed.  Runs before the engine is ever enabled. */
void rtl8198c_l4_init(void)
{
	const u32 zero[8] = { 0 };
	int i;

	for (i = 0; i < L4_SESSIONS; i++)
		rtl8198c_l3_table_write(TBL_TCP_UDP, i, zero);
	sys_w32(TEATCR, 0);
}

/* ===== nftables flowtable offload =====
 *
 * nftables offers an established flow as a flow_cls_offload: a match on the
 * pre-NAT tuple plus the mangle actions that rewrite it.  Both directions
 * of a masquerade or a port forward normalise to the same seven values
 * (inside address and port, remote address and port, external address and
 * port), which is exactly what the two ASIC rows need:
 *
 *   out  hash(int, remote) -> rewrite source to ext
 *   in   hash(remote, ext) -> rewrite destination to int
 *
 * The offer also carries the L2 address and checksum rewrites, which the
 * switch does itself.  Anything else (VLAN, PPPoE, a tunnel, priority) or a
 * partial match is declined and the flow stays with nftables in software,
 * which is always the safe fallback.  A flow is offered once per direction
 * and once per flowtable device, so the same tuple arrives repeatedly; the
 * mirror makes the install idempotent and the cookies identify the flow at
 * teardown, when the kernel passes back only the cookie.
 */
static LIST_HEAD(l4_block_cb_list);

static int l4_mangle_ip4(const struct flow_action_entry *a, __be32 *ip)
{
	if (a->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_IP4 || a->mangle.mask)
		return -EOPNOTSUPP;
	memcpy(ip, &a->mangle.val, sizeof(*ip));
	return 0;
}

/* The L4 mangle writes one 16-bit port inside the first word of the TCP or
 * UDP header.  The mask names the half that stays untouched, which is also
 * how the source and destination port mangles are told apart. */
static int l4_mangle_port(const struct flow_action_entry *a, bool src,
			  __be16 *port)
{
	const u8 *p = (const u8 *)&a->mangle.val;
	u32 mask = src ? ~htonl(0xffff0000) : ~htonl(0xffff);

	if (a->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_TCP &&
	    a->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_UDP)
		return -EOPNOTSUPP;
	if (a->mangle.offset || a->mangle.mask != mask)
		return -EOPNOTSUPP;
	memcpy(port, p + (src ? 0 : 2), sizeof(*port));
	return 0;
}

static int l4_rule_tuple(struct flow_rule *rule,
			 struct rtl8198c_l4_tuple *t)
{
	struct flow_match_control ctl;
	struct flow_match_basic basic;
	struct flow_match_ipv4_addrs ip4;
	struct flow_match_ports ports;
	struct flow_action_entry *a;
	__be32 src_ip, dst_ip, new_src_ip = 0, new_dst_ip = 0;
	__be16 src_port, dst_port, new_src_port = 0, new_dst_port = 0;
	bool src_ip_m = false, dst_ip_m = false;
	bool src_port_m = false, dst_port_m = false;
	bool src_side, dst_side;
	int i;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
		return -EOPNOTSUPP;

	flow_rule_match_control(rule, &ctl);
	if (ctl.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS ||
	    ctl.mask->addr_type != 0xffff)
		return -EOPNOTSUPP;

	flow_rule_match_basic(rule, &basic);
	if (basic.key->n_proto != htons(ETH_P_IP) ||
	    basic.mask->n_proto != 0xffff)
		return -EOPNOTSUPP;
	if (basic.key->ip_proto == IPPROTO_TCP)
		t->tcp = true;
	else if (basic.key->ip_proto == IPPROTO_UDP)
		t->tcp = false;
	else
		return -EOPNOTSUPP;

	flow_rule_match_ipv4_addrs(rule, &ip4);
	if (ip4.mask->src != 0xffffffff || ip4.mask->dst != 0xffffffff)
		return -EOPNOTSUPP;
	src_ip = ip4.key->src;
	dst_ip = ip4.key->dst;

	flow_rule_match_ports(rule, &ports);
	if (ports.mask->src != 0xffff || ports.mask->dst != 0xffff)
		return -EOPNOTSUPP;
	src_port = ports.key->src;
	dst_port = ports.key->dst;

	flow_action_for_each(i, a, &rule->action) {
		switch (a->id) {
		case FLOW_ACTION_MANGLE:
			if (a->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH) {
				/* the switch writes the L2 header itself */
				break;
			} else if (a->mangle.htype ==
				   FLOW_ACT_MANGLE_HDR_TYPE_IP4) {
				if (a->mangle.offset ==
				    offsetof(struct iphdr, saddr)) {
					if (l4_mangle_ip4(a, &new_src_ip) ||
					    src_ip_m)
						return -EOPNOTSUPP;
					src_ip_m = true;
				} else if (a->mangle.offset ==
					   offsetof(struct iphdr, daddr)) {
					if (l4_mangle_ip4(a, &new_dst_ip) ||
					    dst_ip_m)
						return -EOPNOTSUPP;
					dst_ip_m = true;
				} else {
					return -EOPNOTSUPP;
				}
			} else if (l4_mangle_port(a, true, &new_src_port) == 0) {
				if (src_port_m)
					return -EOPNOTSUPP;
				src_port_m = true;
			} else if (l4_mangle_port(a, false, &new_dst_port) == 0) {
				if (dst_port_m)
					return -EOPNOTSUPP;
				dst_port_m = true;
			} else {
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_CSUM:
		case FLOW_ACTION_REDIRECT:
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	/* Exactly one side of the match may be rewritten: that side is the
	 * external tuple, the other one the inside endpoint. */
	src_side = src_ip_m || src_port_m;
	dst_side = dst_ip_m || dst_port_m;
	if (src_side == dst_side)
		return -EOPNOTSUPP;

	if (src_side) {
		t->int_ip = src_ip;
		t->int_port = src_port;
		t->rem_ip = dst_ip;
		t->rem_port = dst_port;
		t->ext_ip = src_ip_m ? new_src_ip : src_ip;
		t->ext_port = src_port_m ? new_src_port : src_port;
	} else {
		t->rem_ip = src_ip;
		t->rem_port = src_port;
		t->ext_ip = dst_ip;
		t->ext_port = dst_port;
		t->int_ip = dst_ip_m ? new_dst_ip : dst_ip;
		t->int_port = dst_port_m ? new_dst_port : dst_port;
	}
	return 0;
}

static int l4_flow_add_cookie(struct rtl8198c_eth *eth,
			      const struct rtl8198c_l4_tuple *t,
			      unsigned long cookie)
{
	int err;

	mutex_lock(&eth->l4.lock);
	err = l4_flow_add_locked(eth, t, 0, cookie);
	mutex_unlock(&eth->l4.lock);
	return err;
}

static int l4_offload_stats(struct rtl8198c_eth *eth,
			    struct flow_cls_offload *f)
{
	struct rtl8198c_l4_session *s;
	u32 e[8], out;

	mutex_lock(&eth->l4.lock);
	s = l4_by_cookie_locked(eth, f->cookie);
	if (!s) {
		mutex_unlock(&eth->l4.lock);
		return -ENOENT;
	}
	out = rtl8198c_l4_hash(s->t.tcp, s->t.int_ip, s->t.int_port,
			       s->t.rem_ip, s->t.rem_port);
	rtl8198c_taci_read_entry(TBL_TCP_UDP + out * 32, e);
	if (!(e[1] & BIT(0))) {
		mutex_unlock(&eth->l4.lock);
		return -ENOENT;
	}
	/* There are no per-row counters on this SoC: report the touch so the
	 * software flow's timeout follows the hardware row, and let the row's
	 * own aging decide when the flow went cold. */
	f->stats.lastused = jiffies;
	mutex_unlock(&eth->l4.lock);
	return 0;
}

static int l4_offload_replace(struct rtl8198c_eth *eth,
			      struct flow_cls_offload *f)
{
	struct rtl8198c_l4_tuple t = { };
	int err;

	/* The knob is a runtime switch: while it is off every offer is
	 * declined and the flowtable forwards in software. */
	if (!eth->l4.enabled)
		return -EOPNOTSUPP;
	if (!f->rule)
		return -EINVAL;
	err = l4_rule_tuple(f->rule, &t);
	if (err)
		return err;
	return l4_flow_add_cookie(eth, &t, f->cookie);
}

static int l4_block_cb(enum tc_setup_type type, void *type_data,
		       void *cb_priv)
{
	struct flow_cls_offload *f = type_data;
	struct net_device *dev = cb_priv;
	struct rtl8198c_port *port = netdev_priv(dev);

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return l4_offload_replace(port->sw, f);
	case FLOW_CLS_DESTROY:
		rtl8198c_l4_flow_del_cookie(port->sw, f->cookie);
		return 0;
	case FLOW_CLS_STATS:
		return l4_offload_stats(port->sw, f);
	default:
		return -EOPNOTSUPP;
	}
}

static int l4_block_bind(struct net_device *dev,
			 struct flow_block_offload *f)
{
	struct flow_block_cb *cb;

	cb = flow_block_cb_lookup(f->block, l4_block_cb, dev);
	if (cb) {
		flow_block_cb_incref(cb);
		return 0;
	}
	cb = flow_block_cb_alloc(l4_block_cb, dev, dev, NULL);
	if (IS_ERR(cb))
		return PTR_ERR(cb);
	flow_block_cb_incref(cb);
	flow_block_cb_add(cb, f);
	list_add_tail(&cb->driver_list, &l4_block_cb_list);
	return 0;
}

static int l4_block_unbind(struct rtl8198c_eth *eth, struct net_device *dev,
			   struct flow_block_offload *f)
{
	struct flow_block_cb *cb;

	cb = flow_block_cb_lookup(f->block, l4_block_cb, dev);
	if (!cb)
		return -ENOENT;
	if (!flow_block_cb_decref(cb)) {
		flow_block_cb_remove(cb, f);
		list_del(&cb->driver_list);
	}

	/* The flowtable deletes its flows before unbinding, but a rewrite
	 * must never outlive the rule that owns it: forget everything. */
	rtl8198c_l4_flush(eth);
	return 0;
}

int rtl8198c_l4_setup_tc(struct net_device *dev, enum tc_setup_type type,
			 void *type_data)
{
	struct flow_block_offload *f = type_data;
	struct rtl8198c_port *port = netdev_priv(dev);

	if (type != TC_SETUP_FT || !f)
		return -EOPNOTSUPP;
	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		return l4_block_bind(dev, f);
	case FLOW_BLOCK_UNBIND:
		return l4_block_unbind(port->sw, dev, f);
	default:
		return -EOPNOTSUPP;
	}
}

/* ===== Debugfs =====
 *
 *   cat l4
 *   echo "add tcp 192.168.2.1 40000 10.9.0.1 8080 192.168.3.2 20000" > l4
 *   echo "del tcp 192.168.2.1 40000 10.9.0.1 8080" > l4
 *   echo "engine on" > l4
 *   echo flush > l4
 *
 * The `add` form is the bring-up tool for the encoding before the flowtable
 * drives it: it takes the pre-NAT tuple and lets the driver compute the two
 * indices and the verification value.
 */
static void l4_show_entry(struct seq_file *s, u32 index, const u32 e[8])
{
	__be32 int_ip = word_to_ip(e[0]);
	bool tcp = e[2] & BIT(19);
	bool out = e[2] & BIT(16);
	u32 ep, idx = 0;

	if (out) {
		ep = (((e[1] >> 8) & 0x3f) << 10) | ((e[1] >> 21) & 0x3ff);
		idx = (e[1] >> 17) & 0xf;
	} else {
		ep = ((e[1] >> 8) & 0x3f) | (((e[1] >> 17) & 0xf) << 6);
	}

	seq_printf(s, "  [%u] %s inside %pI4:%u ext port %u",
		   index, out ? "out" : "in ", &int_ip, e[2] & 0xffff, ep);
	if (out)
		seq_printf(s, " ext-ip %u", idx);
	else
		seq_printf(s, "+n*1024 very 0x%03x", (e[1] >> 21) & 0x3ff);
	seq_printf(s, " %s age %u (%u s) flags %u%s\n", tcp ? "tcp" : "udp",
		   (e[1] >> 2) & TEATCR_FIELD,
		   l4_aging_secs((e[1] >> 2) & TEATCR_FIELD),
		   (e[2] >> 16) & 0x7, (e[1] & BIT(16)) ? " static" : "");
}

static int l4_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;
	u32 teatcr = sys_r32(TEATCR);
	int i, used = 0, nflows = 0, nextip = 0;
	u32 e[8];

	for (i = 0; i < L4_EXT_IPS; i++)
		nextip += eth->l4.extip[i].used;
	seq_printf(s, "l4: engine %s, %d ext-ip, MSCR 0x%08x SWTCR0 0x%08x\n",
		   eth->l4.enabled ? "on" : "off", nextip,
		   sys_r32(MSCR), sys_r32(SWTCR0));
	seq_printf(s, "TEATCR 0x%08x: tcp fast %u (%u s) medium %u (%u s) "
		   "long %u (%u s) udp %u (%u s) icmp %u\n", teatcr,
		   (teatcr >> TEATCR_TCP_FAST_SHIFT) & TEATCR_FIELD,
		   l4_aging_secs((teatcr >> TEATCR_TCP_FAST_SHIFT) & TEATCR_FIELD),
		   (teatcr >> TEATCR_TCP_MED_SHIFT) & TEATCR_FIELD,
		   l4_aging_secs((teatcr >> TEATCR_TCP_MED_SHIFT) & TEATCR_FIELD),
		   (teatcr >> TEATCR_TCP_LONG_SHIFT) & TEATCR_FIELD,
		   l4_aging_secs((teatcr >> TEATCR_TCP_LONG_SHIFT) & TEATCR_FIELD),
		   (teatcr >> TEATCR_UDP_SHIFT) & TEATCR_FIELD,
		   l4_aging_secs((teatcr >> TEATCR_UDP_SHIFT) & TEATCR_FIELD),
		   (teatcr >> TEATCR_ICMP_SHIFT) & TEATCR_FIELD);

	mutex_lock(&eth->l4.lock);
	seq_puts(s, "\next-ip:\n");
	for (i = 0; i < L4_EXT_IPS; i++) {
		if (!eth->l4.extip[i].used)
			continue;
		seq_printf(s, "  [%d] -> %pI4 users %u\n", i,
			   &eth->l4.extip[i].ext_ip,
			   eth->l4.extip[i].users);
	}

	seq_puts(s, "\nflows:\n");
	for (i = 0; i < L4_SESSIONS; i++) {
		struct rtl8198c_l4_tuple *t;
		u32 out;

		if (!eth->l4.sess[i].used)
			continue;
		t = &eth->l4.sess[i].t;
		/* Both directions are mirrored at their own index; print the
		 * flow once, from the outbound slot. */
		out = rtl8198c_l4_hash(t->tcp, t->int_ip, t->int_port,
				       t->rem_ip, t->rem_port);
		if (out != i)
			continue;
		nflows++;
		seq_printf(s, "  %pI4:%u -> %pI4:%u nat %pI4:%u %s out[%u] in[%u]",
			   &t->int_ip, ntohs(t->int_port),
			   &t->rem_ip, ntohs(t->rem_port),
			   &t->ext_ip, ntohs(t->ext_port),
			   t->tcp ? "tcp" : "udp",
			   rtl8198c_l4_hash(t->tcp, t->int_ip, t->int_port,
					    t->rem_ip, t->rem_port),
			   rtl8198c_l4_hash(t->tcp, t->rem_ip, t->rem_port,
					    t->ext_ip, t->ext_port));
		seq_printf(s, " cookies %d/%lu/%lu\n",
			   eth->l4.sess[i].ncookies,
			   eth->l4.sess[i].cookie[0],
			   eth->l4.sess[i].cookie[1]);
	}
	seq_printf(s, "  %d flows\n", nflows);

	seq_puts(s, "\nhardware (valid entries not in the mirror):\n");
	for (i = 0; i < L4_SESSIONS; i++) {
		rtl8198c_taci_read_entry(TBL_TCP_UDP + i * 32, e);
		if (!(e[1] & BIT(0)))
			continue;
		used++;
		if (eth->l4.sess[i].used)
			continue;
		l4_show_entry(s, i, e);
	}
	seq_printf(s, "  %d valid, %d ours\n", used, nflows * 2);
	mutex_unlock(&eth->l4.lock);
	return 0;
}

static int l4_open(struct inode *inode, struct file *file)
{
	return single_open(file, l4_show, inode->i_private);
}

static int l4_ip(const char *s, __be32 *ip)
{
	u8 a[4];

	if (!in4_pton(s, -1, a, -1, NULL))
		return -EINVAL;
	memcpy(ip, a, sizeof(a));
	return 0;
}

static int l4_port(const char *s, __be16 *port)
{
	u16 p;

	if (kstrtou16(s, 0, &p) || !p)
		return -EINVAL;
	*port = cpu_to_be16(p);
	return 0;
}

static int l4_parse_flow(char **argv, struct rtl8198c_l4_tuple *t)
{
	if (!strcmp(argv[0], "tcp"))
		t->tcp = true;
	else if (!strcmp(argv[0], "udp"))
		t->tcp = false;
	else
		return -EINVAL;
	if (l4_ip(argv[1], &t->int_ip) || l4_port(argv[2], &t->int_port) ||
	    l4_ip(argv[3], &t->rem_ip) || l4_port(argv[4], &t->rem_port))
		return -EINVAL;
	return 0;
}

static ssize_t l4_write(struct file *file, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = file->f_inode->i_private;
	struct rtl8198c_l4_tuple t = { };
	char buf[192];
	char *args[10], *p;
	u32 aging = 0;
	int n = 0, err;
	ssize_t len;

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
	if (!n)
		return -EINVAL;

	if (!strcmp(args[0], "flush")) {
		rtl8198c_l4_flush(eth);
		return len;
	}
	if (!strcmp(args[0], "engine")) {
		bool on;

		if (n != 2 || kstrtobool(args[1], &on))
			return -EINVAL;
		rtl8198c_l4_engine(eth, on);
		return len;
	}
	if (!strcmp(args[0], "add")) {
		if (n != 8 && n != 9)
			return -EINVAL;
		err = l4_parse_flow(&args[1], &t);
		if (err)
			return err;
		if (l4_ip(args[6], &t.ext_ip) ||
		    l4_port(args[7], &t.ext_port))
			return -EINVAL;
		if (n == 9 && kstrtou32(args[8], 0, &aging))
			return -EINVAL;
		err = rtl8198c_l4_flow_add(eth, &t, aging);
		return err ? err : len;
	}
	if (!strcmp(args[0], "del")) {
		if (n != 6)
			return -EINVAL;
		err = l4_parse_flow(&args[1], &t);
		if (err)
			return err;
		err = rtl8198c_l4_flow_del(eth, &t);
		return err ? err : len;
	}
	return -EINVAL;
}

static const struct file_operations l4_fops = {
	.owner = THIS_MODULE,
	.open = l4_open,
	.read = seq_read,
	.write = l4_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* debugfs "flow_offload": let nftables flowtables program the L4 engine.
 *
 * Off by default.  With it on, the driver accepts the flows a flowtable
 * offers and installs the two NAPT rows that keep the flow in silicon;
 * with it off, every offer is declined and nftables forwards in software.
 * The switch routes the flows itself, so this depends on route_offload:
 * without hardware routing the session table is never consulted, and
 * turning route_offload off disarms this engine too.
 */
static int flow_offload_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;

	seq_printf(s, "%d\n", eth->l4.enabled ? 1 : 0);
	seq_puts(s, "Offloaded flows are NAT'd by the switch and bypass netfilter:\n"
		    "the firewall sees the first packet and the counters of an\n"
		    "offloaded flow.  Needs route_offload=1; ICMP and IPv6 stay\n"
		    "in software.\n");
	return 0;
}

static ssize_t flow_offload_write(struct file *file, const char __user *ubuf,
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
	if (on && !eth->l3.enabled)
		return -EOPNOTSUPP;
	rtl8198c_l4_engine(eth, on);
	return count;
}

static int flow_offload_open(struct inode *inode, struct file *file)
{
	return single_open(file, flow_offload_show, inode->i_private);
}

static const struct file_operations flow_offload_fops = {
	.owner = THIS_MODULE,
	.open = flow_offload_open,
	.read = seq_read,
	.write = flow_offload_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void rtl8198c_l4_debugfs_init(struct rtl8198c_eth *eth)
{
	mutex_init(&eth->l4.lock);
	debugfs_create_file("l4", 0600, eth->debugfs_dir, eth, &l4_fops);
	debugfs_create_file("flow_offload", 0644, eth->debugfs_dir, eth,
			    &flow_offload_fops);
}
