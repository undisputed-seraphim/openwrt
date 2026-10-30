// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L2: IP multicast groups
 *
 * Groups are L2 FDB entries for the derived multicast MAC, which is
 * also how IGMP/MLD snooping steers them in bridge mode.
 */
#include "rtl8198c.h"

/* ===== IP multicast groups ===== */

static struct mcast_group *mcast_find(struct rtl8198c_eth *eth,
				      const u8 *mac, u8 fid)
{
	int i;

	for (i = 0; i < MCAST_GROUPS; i++)
		if (eth->mcast[i].used && eth->mcast[i].fid == fid &&
		    ether_addr_equal(eth->mcast[i].mac, mac))
			return &eth->mcast[i];
	return NULL;
}

static int mcast_group_add(struct rtl8198c_eth *eth, const u8 *mac, u8 fid,
			   u16 portmask, bool cpu)
{
	struct mcast_group *g = mcast_find(eth, mac, fid);
	int i, err;

	for (i = 0; !g && i < MCAST_GROUPS; i++)
		if (!eth->mcast[i].used)
			g = &eth->mcast[i];
	if (!g)
		return -ENOSPC;

	err = rtl8198c_l2_fdb_add(mac, portmask & L2_MEMBER_ALL, 0, cpu, fid);
	if (err)
		return err;
	if (!g->used)
		eth->mcast_count++;
	memcpy(g->mac, mac, ETH_ALEN);
	g->fid = fid;
	g->portmask = portmask;
	g->cpu = cpu;
	g->used = true;
	return 0;
}

static int mcast_group_del(struct rtl8198c_eth *eth, const u8 *mac, u8 fid)
{
	struct mcast_group *g = mcast_find(eth, mac, fid);

	if (!g)
		return -ENOENT;
	rtl8198c_l2_fdb_del(mac, fid);
	memset(g, 0, sizeof(*g));
	eth->mcast_count--;
	return 0;
}


/* ===== debugfs "mcast": IP multicast groups =====
 *
 *   cat mcast
 *   echo "add 239.1.2.3 ports 0x08" > mcast
 *   echo "add 239.1.2.3 ports 0x18 fid 1 cpu" > mcast
 *   echo "add 33:33:00:00:00:01 ports 0x10" > mcast
 *   echo "del 239.1.2.3" > mcast
 *   echo "flush" > mcast
 *
 * A group is an IPv4 multicast address (the destination MAC is derived) or
 * a multicast MAC directly, which is how MLD groups are given.  Each one
 * becomes an L2 FDB entry in the selected filtering database: frames for
 * it go only to the ports in the mask, plus the CPU when "cpu" is set.
 * Groups without an entry keep flooding.  The kernel bridge cannot drive
 * this (no switchdev), so the groups are programmed here.
 */
static int mcast_parse_group(const char *s, u8 mac[ETH_ALEN])
{
	unsigned int b[ETH_ALEN];
	u8 a[4];
	u32 g;
	int i;

	if (strchr(s, ':')) {
		if (sscanf(s, "%x:%x:%x:%x:%x:%x",
			   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != ETH_ALEN)
			return -EINVAL;
		for (i = 0; i < ETH_ALEN; i++) {
			if (b[i] > 0xFF)
				return -EINVAL;
			mac[i] = b[i];
		}
	} else {
		if (!in4_pton(s, -1, a, '\0', NULL))
			return -EINVAL;
		g = ((u32)a[0] << 24) | ((u32)a[1] << 16) |
		    ((u32)a[2] << 8) | a[3];
		if ((g >> 28) != 0xE)
			return -EINVAL;
		mac[0] = 0x01;
		mac[1] = 0x00;
		mac[2] = 0x5E;
		mac[3] = a[1] & 0x7F;
		mac[4] = a[2];
		mac[5] = a[3];
	}
	if (!is_multicast_ether_addr(mac))
		return -EINVAL;
	return 0;
}

static ssize_t mcast_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[96];
	u8 mac[ETH_ALEN];
	unsigned int ports = 0, fid = L2_FID_LAN;
	char *p, *key, *val;
	bool cpu = false;
	ssize_t len;
	int i, err;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (!strcmp(kbuf, "flush")) {
		for (i = 0; i < MCAST_GROUPS; i++)
			if (eth->mcast[i].used)
				mcast_group_del(eth, eth->mcast[i].mac,
						eth->mcast[i].fid);
		return len;
	}

	if (strncmp(kbuf, "add ", 4) && strncmp(kbuf, "del ", 4))
		return -EINVAL;
	p = kbuf + 4;
	key = strsep(&p, " ");
	if (!key || mcast_parse_group(key, mac))
		return -EINVAL;

	while ((key = strsep(&p, " "))) {
		if (!*key)
			continue;
		if (!strcmp(key, "cpu")) {
			cpu = true;
			continue;
		}
		val = strsep(&p, " ");
		if (!val)
			return -EINVAL;
		if (!strcmp(key, "ports")) {
			if (kstrtouint(val, 0, &ports) || ports > 0x3F)
				return -EINVAL;
		} else if (!strcmp(key, "fid")) {
			if (kstrtouint(val, 0, &fid) || fid >= L2_FIDS)
				return -EINVAL;
		} else {
			return -EINVAL;
		}
	}

	if (kbuf[0] == 'd') {
		err = mcast_group_del(eth, mac, fid);
		return err ? err : len;
	}
	err = mcast_group_add(eth, mac, fid, ports, cpu);
	return err ? err : len;
}

static int mcast_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;
	const struct mcast_group *g;
	int i;

	for (i = 0; i < MCAST_GROUPS; i++) {
		g = &eth->mcast[i];
		if (!g->used)
			continue;
		seq_printf(s, "%pM fid %u ports 0x%02x%s\n", g->mac, g->fid,
			   g->portmask, g->cpu ? " cpu" : "");
	}
	seq_printf(s, "%u groups\n", eth->mcast_count);
	return 0;
}

static int mcast_open(struct inode *inode, struct file *file)
{ return single_open(file, mcast_show, inode->i_private); }
static const struct file_operations mcast_fops = {
	.owner = THIS_MODULE, .open = mcast_open,
	.read = seq_read, .write = mcast_write,
	.llseek = seq_lseek, .release = single_release,
};

/* The debugfs knobs this block owns; the registration itself is central so
 * probe creates one directory. */
void rtl8198c_mcast_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("mcast", 0600, eth->debugfs_dir, eth, &mcast_fops);
}
