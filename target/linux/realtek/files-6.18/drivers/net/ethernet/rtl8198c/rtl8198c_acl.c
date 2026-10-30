// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L2: ingress ACL
 *
 * TACI type 12 rule table and the debugfs interface that drives it.
 * Ingress only: the egress engine is disabled.
 */
#include "rtl8198c.h"

/* ===== Hardware ACL (ingress) ===== */

/* A MAC is stored as three big-endian 16-bit halves: word 0 holds bytes
 * 4-5, word 1/word 3 the high half, and words 1/2/4/5 mix in the mask. */
static u16 acl_mac_word(const u8 *m, int off)
{
	return ((u16)m[off] << 8) | m[off + 1];
}

static void acl_entry_mac(u32 e[8], const struct acl_rule *r)
{
	e[0] = acl_mac_word(r->dmac, 4) |
	       ((u32)acl_mac_word(r->dmac, 2) << 16);
	e[1] = acl_mac_word(r->dmac, 0) |
	       ((u32)acl_mac_word(r->dmac_mask, 4) << 16);
	e[2] = acl_mac_word(r->dmac_mask, 2) |
	       ((u32)acl_mac_word(r->dmac_mask, 0) << 16);
	e[3] = acl_mac_word(r->smac, 4) |
	       ((u32)acl_mac_word(r->smac, 2) << 16);
	e[4] = acl_mac_word(r->smac, 0) |
	       ((u32)acl_mac_word(r->smac_mask, 4) << 16);
	e[5] = acl_mac_word(r->smac_mask, 2) |
	       ((u32)acl_mac_word(r->smac_mask, 0) << 16);
	e[6] = r->ethertype | ((u32)r->ethertype_mask << 16);
}

/* L3L4 entries: word 0-3 are src/dst address and mask, word 4 carries the
 * TOS and protocol (or TCP flags for a TCP rule), words 5/6 the port
 * bounds.  Addresses are compared in network order, as they arrive. */
static void acl_entry_l3l4(u32 e[8], const struct acl_rule *r)
{
	e[0] = (u32)r->sip;
	e[1] = (u32)r->sip_mask;
	e[2] = (u32)r->dip;
	e[3] = (u32)r->dip_mask;
	e[4] = r->tos | ((u32)r->tos_mask << 8) |
	       ((u32)r->proto << 16) | ((u32)r->proto_mask << 24);
}

static void acl_entry_build(u32 e[8], const struct acl_rule *r)
{
	memset(e, 0, 8 * sizeof(u32));
	if (r->type == ACL_RULE_MAC)
		acl_entry_mac(e, r);
	else
		acl_entry_l3l4(e, r);
	/* ruleType | actionType | pktOpApp (7 = all layers) */
	e[7] = (r->type & 0xF) | ((u32)(r->action & 0xF) << 4) | (7u << 24);
}

static void acl_entry_default(u32 e[8], u8 action)
{
	memset(e, 0, 8 * sizeof(u32));
	e[7] = ((u32)action << 4) | (7u << 24);
}

static void acl_table_write(u32 idx, const u32 e[8])
{
	rtl8198c_taci_write_entry(TBL_ACL + idx * 32, e, BIT(0) | BIT(3)); /* FORCE */
}

/* Four reserved rules at the top of the table: IPv6 multicast to the CPU,
 * permit all, drop all, to-CPU all.  The same set is used here. */
static void acl_reserved(void)
{
	u32 e[8];

	/* dst MAC 33:33:00:00:00:00, mask ff:ff:00:00:00:00 */
	memset(e, 0, sizeof(e));
	e[1] = 0x3333;		/* dMacP47_32 */
	e[2] = 0xFFFF0000;	/* dMacM47_32 */
	e[7] = (ACL_TOCPU << 4) | (7u << 24);
	acl_table_write(ACL_IPV6_TO_CPU, e);

	acl_entry_default(e, ACL_PERMIT);
	acl_table_write(ACL_PERMIT_ALL, e);
	acl_entry_default(e, ACL_DROP);
	acl_table_write(ACL_DROP_ALL, e);
	acl_entry_default(e, ACL_TOCPU);
	acl_table_write(ACL_ALL_TO_CPU, e);
}

/* Write the chain (user rules then the catch-all), point both ACL ranges at
 * it and refresh the netif entries.  Nothing is enabled here: the engine is
 * switched on once at probe, after the netif table exists. */
static void acl_program(struct rtl8198c_eth *eth)
{
	u32 e[8];
	int i;

	for (i = 0; i < eth->acl_count; i++) {
		acl_entry_build(e, &eth->acl_rules[i]);
		acl_table_write(i, e);
	}
	acl_entry_default(e, eth->acl_default);
	acl_table_write(eth->acl_count, e);
	eth->acl_end = eth->acl_count;

	/* Clear the entry the previous chain ended at, if any. */
	if (eth->acl_count + 1 < ACL_RULES) {
		memset(e, 0, sizeof(e));
		acl_table_write(eth->acl_count + 1, e);
	}

	/* Bridged traffic has no netif decision, so DACLRCR's ingress range
	 * is the one that applies to it.  The egress range stays on the
	 * reserved permit rule: the egress engine is never enabled. */
	sys_w32(DACLRCR, eth->acl_end | ((u32)eth->acl_end << 8) |
		((u32)ACL_PERMIT_ALL << 16) | ((u32)ACL_PERMIT_ALL << 24));

	/* The netif entries carry the chain range; rebuild them per VID. */
	rtl8198c_l3_netif_sync(eth);
}

void rtl8198c_acl_init(struct rtl8198c_eth *eth)
{
	eth->acl_count = 0;
	eth->acl_default = ACL_PERMIT;
	acl_reserved();
	acl_program(eth);
	/* Enable only after the table and both ranges are in place: the
	 * default action of an empty engine is not "permit". */
	sys_w32(MSCR, sys_r32(MSCR) | MSCR_EN_IN_ACL);
}


/* ===== debugfs "acl": the hardware ingress ACL =====
 *
 *   cat acl
 *   echo "add mac dst 01:00:5e:00:00:01/ff:ff:ff:00:00:00 action drop" > acl
 *   echo "add ip src 192.168.2.0/24 dst 8.8.8.8/32 proto 17 action drop" > acl
 *   echo "add ip proto 1 action drop" > acl
 *   echo "add mac ethertype 0x0806 action drop" > acl
 *   echo "del 0" > acl
 *   echo "default drop" > acl
 *   echo "flush" > acl
 *
 * Rules are matched in order and the first match wins; every chain ends
 * with a catch-all whose action is the "default" setting.  Addresses use
 * prefix lengths or dotted masks and MACs accept a mask after "/".  Only
 * ingress is filtered (the egress engine is not enabled, and the TCP/UDP
 * rule types need the L4 stage which this configuration does not run;
 * both are measured and recorded in RTL8198C.md).
 */
static const char * const acl_type_name[] = {
	[ACL_RULE_MAC] = "mac", [ACL_RULE_IP] = "ip",
};

static const char *acl_action_name(u8 a)
{
	switch (a) {
	case ACL_PERMIT: return "permit";
	case ACL_DROP:	return "drop";
	case ACL_TOCPU:	return "tocpu";
	default:	return "?";
	}
}

static int acl_parse_mac(const char *s, u8 mac[ETH_ALEN], u8 mask[ETH_ALEN])
{
	char tmp[40], *slash;
	unsigned int b[ETH_ALEN], m[ETH_ALEN];
	int i;

	strscpy(tmp, s, sizeof(tmp));
	memset(m, 0xFF, sizeof(m));
	slash = strchr(tmp, '/');
	if (slash) {
		*slash++ = '\0';
		if (sscanf(slash, "%x:%x:%x:%x:%x:%x",
			   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != ETH_ALEN)
			return -EINVAL;
	}
	if (sscanf(tmp, "%x:%x:%x:%x:%x:%x",
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != ETH_ALEN)
		return -EINVAL;
	for (i = 0; i < ETH_ALEN; i++) {
		if (b[i] > 0xFF || m[i] > 0xFF)
			return -EINVAL;
		mac[i] = b[i];
		mask[i] = m[i];
	}
	return 0;
}

static int acl_parse_ip(const char *s, __be32 *addr, __be32 *mask)
{
	char tmp[40], *slash;
	u8 a[4], m[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	unsigned long len, v;
	int i;

	strscpy(tmp, s, sizeof(tmp));
	slash = strchr(tmp, '/');
	if (slash)
		*slash++ = '\0';
	if (!in4_pton(tmp, -1, a, '\0', NULL))
		return -EINVAL;
	if (slash) {
		if (strchr(slash, '.')) {
			if (!in4_pton(slash, -1, m, '\0', NULL))
				return -EINVAL;
		} else {
			if (kstrtoul(slash, 10, &len) || len > 32)
				return -EINVAL;
			for (i = 0; i < 4; i++) {
				v = len > 8 * i ? len - 8 * i : 0;
				m[i] = v >= 8 ? 0xFF : (u8)(0xFF << (8 - v));
			}
		}
	}
	memcpy(addr, a, 4);
	memcpy(mask, m, 4);
	return 0;
}

static int acl_action_parse(const char *s)
{
	if (!strcmp(s, "permit"))
		return ACL_PERMIT;
	if (!strcmp(s, "drop"))
		return ACL_DROP;
	if (!strcmp(s, "tocpu"))
		return ACL_TOCPU;
	return -EINVAL;
}

static ssize_t acl_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[128];
	struct acl_rule *r;
	char *p, *key, *val;
	ssize_t len;
	unsigned long idx;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (!strcmp(kbuf, "flush")) {
		eth->acl_count = 0;
		eth->acl_default = ACL_PERMIT;
		acl_program(eth);
		return len;
	}

	if (!strncmp(kbuf, "default ", 8)) {
		int a = acl_action_parse(kbuf + 8);

		if (a < 0 || a == ACL_TOCPU)
			return -EINVAL;
		eth->acl_default = a;
		acl_program(eth);
		return len;
	}

	if (!strncmp(kbuf, "del ", 4)) {
		if (kstrtoul(kbuf + 4, 10, &idx) || idx >= eth->acl_count)
			return -EINVAL;
		memmove(&eth->acl_rules[idx], &eth->acl_rules[idx + 1],
			(eth->acl_count - idx - 1) * sizeof(*r));
		eth->acl_count--;
		acl_program(eth);
		return len;
	}

	if (strncmp(kbuf, "add ", 4))
		return -EINVAL;
	if (eth->acl_count >= ACL_MAX_RULES)
		return -ENOSPC;

	r = &eth->acl_rules[eth->acl_count];
	memset(r, 0, sizeof(*r));
	p = kbuf + 4;
	key = strsep(&p, " ");
	if (!key)
		return -EINVAL;
	if (!strcmp(key, "mac"))
		r->type = ACL_RULE_MAC;
	else if (!strcmp(key, "ip"))
		r->type = ACL_RULE_IP;
	else if (!strcmp(key, "tcp") || !strcmp(key, "udp"))
		return -EOPNOTSUPP;	/* L4 stage does not run, see above */
	else
		return -EINVAL;

	r->action = 0xFF;
	while ((key = strsep(&p, " "))) {
		if (!*key)
			continue;
		val = strsep(&p, " ");
		if (!val)
			return -EINVAL;

		if (!strcmp(key, "action")) {
			int a = acl_action_parse(val);

			if (a < 0)
				return -EINVAL;
			r->action = a;
		} else if (!strcmp(key, "dst")) {
			if (r->type == ACL_RULE_MAC) {
				if (acl_parse_mac(val, r->dmac, r->dmac_mask))
					return -EINVAL;
			} else if (acl_parse_ip(val, &r->dip, &r->dip_mask)) {
				return -EINVAL;
			}
		} else if (!strcmp(key, "src")) {
			if (r->type == ACL_RULE_MAC) {
				if (acl_parse_mac(val, r->smac, r->smac_mask))
					return -EINVAL;
			} else if (acl_parse_ip(val, &r->sip, &r->sip_mask)) {
				return -EINVAL;
			}
		} else if (!strcmp(key, "ethertype") && r->type == ACL_RULE_MAC) {
			char *slash = strchr(val, '/');
			unsigned int v, m = 0xFFFF;

			if (slash) {
				*slash++ = '\0';
				if (sscanf(slash, "%x", &m) != 1)
					return -EINVAL;
			}
			if (sscanf(val, "%x", &v) != 1 || v > 0xFFFF ||
			    m > 0xFFFF)
				return -EINVAL;
			r->ethertype = v;
			r->ethertype_mask = m;
		} else if (!strcmp(key, "tos") && r->type == ACL_RULE_IP) {
			char *slash = strchr(val, '/');
			unsigned int v, m = 0xFF;

			if (slash) {
				*slash++ = '\0';
				if (sscanf(slash, "%x", &m) != 1)
					return -EINVAL;
			}
			if (sscanf(val, "%x", &v) != 1 || v > 0xFF || m > 0xFF)
				return -EINVAL;
			r->tos = v;
			r->tos_mask = m;
		} else if (!strcmp(key, "proto") && r->type == ACL_RULE_IP) {
			char *slash = strchr(val, '/');
			unsigned int v, m = 0xFF;

			if (slash) {
				*slash++ = '\0';
				if (sscanf(slash, "%x", &m) != 1)
					return -EINVAL;
			}
			if (sscanf(val, "%x", &v) != 1 || v > 0xFF || m > 0xFF)
				return -EINVAL;
			r->proto = v;
			r->proto_mask = m;
		} else {
			return -EINVAL;
		}
	}
	if (r->action == 0xFF)
		return -EINVAL;

	eth->acl_count++;
	acl_program(eth);
	return len;
}

static void acl_show_rule(struct seq_file *s, int idx,
			  const struct acl_rule *r)
{
	seq_printf(s, "%2d %-4s %-6s", idx, acl_type_name[r->type],
		   acl_action_name(r->action));
	if (r->type == ACL_RULE_MAC) {
		if (!is_zero_ether_addr(r->dmac_mask))
			seq_printf(s, " dst %pM/%pM", r->dmac, r->dmac_mask);
		if (!is_zero_ether_addr(r->smac_mask))
			seq_printf(s, " src %pM/%pM", r->smac, r->smac_mask);
		if (r->ethertype_mask)
			seq_printf(s, " ethertype 0x%04x/0x%04x",
				   r->ethertype, r->ethertype_mask);
	} else {
		if (r->sip_mask)
			seq_printf(s, " src %pI4/%pI4", &r->sip, &r->sip_mask);
		if (r->dip_mask)
			seq_printf(s, " dst %pI4/%pI4", &r->dip, &r->dip_mask);
		if (r->type == ACL_RULE_IP && r->proto_mask)
			seq_printf(s, " proto 0x%02x/0x%02x",
				   r->proto, r->proto_mask);
		if (r->tos_mask)
			seq_printf(s, " tos 0x%02x/0x%02x", r->tos, r->tos_mask);
	}
	seq_putc(s, '\n');
}

static int acl_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;
	int i;

	for (i = 0; i < eth->acl_count; i++)
		acl_show_rule(s, i, &eth->acl_rules[i]);
	seq_printf(s, "%2u default %-6s (catch-all)\n", eth->acl_count,
		   acl_action_name(eth->acl_default));
	seq_printf(s, "ingress only, entries 0..%u, engine %s\n",
		   eth->acl_end,
		   (sys_r32(MSCR) & MSCR_EN_IN_ACL) ? "enabled" : "disabled");
	return 0;
}

static int acl_open(struct inode *inode, struct file *file)
{ return single_open(file, acl_show, inode->i_private); }
static const struct file_operations acl_fops = {
	.owner = THIS_MODULE, .open = acl_open,
	.read = seq_read, .write = acl_write,
	.llseek = seq_lseek, .release = single_release,
};

/* The debugfs knobs this block owns; the registration itself is central so
 * probe creates one directory. */
void rtl8198c_acl_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("acl", 0600, eth->debugfs_dir, eth, &acl_fops);
}
