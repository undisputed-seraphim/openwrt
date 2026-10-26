// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L2: storm, mirror, QoS, rate
 *
 * Broadcast/multicast meters, port mirroring, DSCP-to-priority mapping
 * and per-port/per-queue egress rate limiting.
 *
 * BSP counterparts: rtl865x_asicL2.c storm control, setAsicPortMirror,
 * setAsicDscpPriority and the rate limit tables.
 */
#include "rtl8198c.h"

/* ===== Storm control =====
 *
 * The switch core meters broadcast and multicast only: PCRP bits 8-10 arm
 * the meter per port, and one global BSCR byte budget sets the rate (30360
 * bytes per interval is 100% of a 10M port, so the budget scales with link
 * speed).  There is no per-port rate and no unknown-unicast storm control on
 * this core: unknown unicast is either flooded or trapped to the CPU through
 * FFCR, and neither is rate limited.
 *
 * debugfs "storm":
 *   cat storm                     -> state
 *   echo "broadcast on|off" > storm
 *   echo "multicast on|off" > storm
 *   echo "rate <0-100>" > storm   -> percent of the port speed
 */
#define STORM_BCAST		BIT(0)
#define STORM_MCAST		BIT(1)

static u32 storm_get_rate(void)
{
	return sys_r32(BSCR) * 100 / BSCR_MAX_BYTES;
}

void rtl8198c_storm_set_rate(u32 percent)
{
	if (percent > 100)
		percent = 100;
	sys_w32(BSCR, BSCR_MAX_BYTES * percent / 100);
}

static u32 storm_get_enabled(void)
{
	u32 v = sys_r32(PCRP0), mask = 0;

	if (v & PCRP_BC_SC_BCAST)
		mask |= STORM_BCAST;
	if (v & PCRP_BC_SC_MCAST)
		mask |= STORM_MCAST;
	return mask;
}

static void storm_set_enabled(u32 mask)
{
	u32 port, v;

	for (port = 0; port < ETH_PORTS; port++) {
		v = sys_r32(PCRP0 + port * 4);
		v &= ~(PCRP_EN_BC_SC | PCRP_BC_SC_BCAST | PCRP_BC_SC_MCAST);
		if (mask & STORM_BCAST)
			v |= PCRP_EN_BC_SC | PCRP_BC_SC_BCAST;
		if (mask & STORM_MCAST)
			v |= PCRP_EN_BC_SC | PCRP_BC_SC_MCAST;
		sys_w32(PCRP0 + port * 4, v);
	}
}

static int storm_show(struct seq_file *s, void *unused)
{
	u32 mask = storm_get_enabled();

	seq_printf(s, "broadcast: %s\n", mask & STORM_BCAST ? "on" : "off");
	seq_printf(s, "multicast: %s\n", mask & STORM_MCAST ? "on" : "off");
	seq_printf(s, "rate: %u%% (BSCR 0x%04x)\n", storm_get_rate(),
		   sys_r32(BSCR));
	seq_puts(s, "unknown unicast: not supported by this switch core\n");
	return 0;
}

static ssize_t storm_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char what[16], arg[8], kbuf[32];
	unsigned int percent;
	ssize_t len;
	u32 mask;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (sscanf(kbuf, "rate %u", &percent) == 1) {
		rtl8198c_storm_set_rate(percent);
		return len;
	}
	if (sscanf(kbuf, "%15s %7s", what, arg) != 2)
		return -EINVAL;
	if (strcmp(arg, "on") && strcmp(arg, "off"))
		return -EINVAL;

	mask = storm_get_enabled();
	if (!strcmp(what, "broadcast"))
		mask = !strcmp(arg, "on") ? mask | STORM_BCAST :
					    mask & ~STORM_BCAST;
	else if (!strcmp(what, "multicast"))
		mask = !strcmp(arg, "on") ? mask | STORM_MCAST :
					    mask & ~STORM_MCAST;
	else
		return -EINVAL;
	storm_set_enabled(mask);
	return len;
}

static int storm_open(struct inode *inode, struct file *file)
{ return single_open(file, storm_show, inode->i_private); }
static const struct file_operations storm_fops = {
	.owner = THIS_MODULE, .open = storm_open,
	.read = seq_read, .write = storm_write,
	.llseek = seq_lseek, .release = single_release,
};

/* ===== Port mirroring =====
 *
 * PMCR copies frames from a set of source ports to one destination port.
 * Sources are split into ingress (rx) and egress (tx) masks, so a port can be
 * observed in one direction or both.  The destination is a single port bit;
 * port 6 is the CPU extension port, which makes "mirror port 4 to the CPU"
 * the only way to capture with a single cable.
 *
 * debugfs "mirror":
 *   cat mirror                          -> destination and source masks
 *   echo "0 0x10 0x00" > mirror         -> ingress of port 4 to port 0
 *   echo "6 0x00 0x10" > mirror         -> egress of port 4 to the CPU
 *   echo off > mirror
 */
static void mirror_get(u32 *dest, u32 *rx, u32 *tx)
{
	u32 pmcr = sys_r32(PMCR);

	*dest = (pmcr & PMCR_DEST_MASK) >> PMCR_DEST_SHIFT;
	*rx = (pmcr & PMCR_RX_MASK) >> PMCR_RX_SHIFT;
	*tx = (pmcr & PMCR_TX_MASK) >> PMCR_TX_SHIFT;
}

static int mirror_show(struct seq_file *s, void *unused)
{
	u32 dest, rx, tx;

	mirror_get(&dest, &rx, &tx);
	if (!dest)
		seq_puts(s, "destination: none (disabled)\n");
	else
		seq_printf(s, "destination: port %u%s\n", (u32)__ffs(dest),
			   dest == BIT(6) ? " (CPU)" : "");
	seq_printf(s, "rx sources:  0x%02x\n", rx);
	seq_printf(s, "tx sources:  0x%02x\n", tx);
	seq_printf(s, "PMCR:        0x%08x\n", sys_r32(PMCR));
	return 0;
}

static ssize_t mirror_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	char kbuf[32];
	unsigned int dest, rx = 0, tx = 0;
	ssize_t len;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (!strcmp(kbuf, "off")) {
		sys_w32(PMCR, 0);
		return len;
	}
	if (sscanf(kbuf, "%u %x %x", &dest, &rx, &tx) != 3)
		return -EINVAL;
	/* The destination is one port number (0-5 physical, 6 CPU). */
	if (dest > 8 || rx & ~0x1FF || tx & ~0x1FF)
		return -EINVAL;
	sys_w32(PMCR, (BIT(dest) << PMCR_DEST_SHIFT) |
		      ((rx & 0x1FF) << PMCR_RX_SHIFT) |
		      (tx & 0x1FF));
	return len;
}

static int mirror_open(struct inode *inode, struct file *file)
{ return single_open(file, mirror_show, inode->i_private); }
static const struct file_operations mirror_fops = {
	.owner = THIS_MODULE, .open = mirror_open,
	.read = seq_read, .write = mirror_write,
	.llseek = seq_lseek, .release = single_release,
};

/* ===== DSCP -> priority (debug) =====
 *
 * DSCPCR0-6 hold 10 three-bit priorities each.  The switch derives the
 * frame priority from the DSCP value as soon as the relevant field is
 * non-zero; CPUQIDMCR0 then maps priority to CPU queue and CPUQDM maps
 * queue to RX ring.  Defaults are all zero (every DSCP is priority 0).
 */
static int dscp_show(struct seq_file *s, void *unused)
{
	int dscp, prio;

	for (dscp = 0; dscp < 64; dscp++) {
		u32 v = sys_r32(DSCPCR0 + (dscp / 10) * 4);

		prio = (v >> ((dscp % 10) * 3)) & 0x7;
		seq_printf(s, "%2d:%d%s", dscp, prio,
			   dscp % 10 == 9 ? "\n" : " ");
	}
	return 0;
}

static ssize_t dscp_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	char kbuf[32];
	ssize_t len;
	int dscp, prio;
	u32 reg, shift, v;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (sscanf(kbuf, "%d %d", &dscp, &prio) != 2)
		return -EINVAL;
	if (dscp < 0 || dscp > 63 || prio < 0 || prio > 7)
		return -EINVAL;

	reg = DSCPCR0 + (dscp / 10) * 4;
	shift = (dscp % 10) * 3;
	v = sys_r32(reg);
	v = (v & ~(0x7u << shift)) | ((u32)prio << shift);
	sys_w32(reg, v);
	return len;
}

static int dscp_open(struct inode *inode, struct file *file)
{ return single_open(file, dscp_show, inode->i_private); }
static const struct file_operations dscp_fops = {
	.owner = THIS_MODULE, .open = dscp_open,
	.read = seq_read, .write = dscp_write,
	.llseek = seq_lseek, .release = single_release,
};

/* ===== Rate limiting =====
 *
 * Per-port egress:  WFQRCRP<port> APR, 64 kbps units, 0x3FFF = off.
 * Per-queue egress: PnQmRGCR at port*0x18 + queue*4; APR is the average
 *                   rate (64 kbps, 0x3FFF = off), L1 the bucket burst
 *                   (1 KB units, 0xFF = maximum) and PPR the peak rate
 *                   (peak = 2^PPR x APR, 7 = no peak bucket).
 *
 * Per-port ingress (IBCR0/1/2) is not wired up: measured inert on this
 * core (16 kbps on every port left 941 Mbit/s flowing through both the
 * CPU path and hardware-forwarded traffic).
 */
#define APR_UNLIMITED		0x3FFF
#define APR_MAX			0x3FFE

static u32 kbps_to_apr(u32 kbps)
{
	if (!kbps)
		return APR_UNLIMITED;
	return clamp(DIV_ROUND_UP(kbps, 64), 1, APR_MAX);
}

static u32 apr_to_kbps(u32 apr)
{
	return apr == APR_UNLIMITED ? 0 : apr * 64;
}

void rtl8198c_port_egress_set(int port, u32 kbps)
{
	u32 reg = WFQRCRP0 + port * 12;

	sys_w32(reg, (sys_r32(reg) & ~0x3FFFu) | kbps_to_apr(kbps));
}

static u32 port_egress_get(int port)
{
	return apr_to_kbps(sys_r32(WFQRCRP0 + port * 12) & 0x3FFF);
}

static void queue_rate_set(int port, int q, u32 kbps, u32 burst_kb, u32 ppr)
{
	u32 reg = P0Q0RGCR + port * 0x18 + q * 4;

	if (!kbps) {
		burst_kb = 0xFF;
		ppr = 7;
	}
	sys_w32(reg, (ppr << 24) | (burst_kb << 16) | kbps_to_apr(kbps));
}

static void queue_rate_get(int port, int q, u32 *kbps, u32 *burst, u32 *ppr)
{
	u32 v = sys_r32(P0Q0RGCR + port * 0x18 + q * 4);

	*kbps = apr_to_kbps(v & 0x3FFF);
	*burst = (v >> 16) & 0xFF;
	*ppr = (v >> 24) & 0x7;
}

static int rate_show(struct seq_file *s, void *unused)
{
	int p, q;

	for (p = 0; p < ETH_PORTS; p++) {
		u32 eg = port_egress_get(p);

		seq_printf(s, "port %d egress  %u kbps%s\n", p, eg,
			   eg ? "" : " (off)");
		for (q = 0; q < 2; q++) {
			u32 kbps, burst, ppr;

			queue_rate_get(p, q, &kbps, &burst, &ppr);
			seq_printf(s, "port %d queue %d %u kbps burst %u KB ppr %u%s\n",
				   p, q, kbps, burst, ppr,
				   kbps ? "" : " (off)");
		}
	}
	return 0;
}

static ssize_t rate_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	char kbuf[64], what[16];
	ssize_t len;
	unsigned int p, q = 0, kbps, burst = 32, ppr = 7;
	int n;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';

	if (!strcmp(kbuf, "reset")) {
		for (p = 0; p < ETH_PORTS; p++) {
			rtl8198c_port_egress_set(p, 0);
			queue_rate_set(p, 0, 0, 0, 0);
			queue_rate_set(p, 1, 0, 0, 0);
		}
		return len;
	}

	n = sscanf(kbuf, "port %u %15s %u", &p, what, &kbps);
	if (n == 3 && p < ETH_PORTS) {
		if (!strcmp(what, "egress")) {
			rtl8198c_port_egress_set(p, kbps);
			return len;
		}
		return -EINVAL;
	}

	n = sscanf(kbuf, "queue %u %u %u %u %u", &p, &q, &kbps, &burst, &ppr);
	if (n >= 3 && p < ETH_PORTS && q < 2 && burst <= 255 && ppr <= 7) {
		queue_rate_set(p, q, kbps, burst, ppr);
		return len;
	}
	return -EINVAL;
}

static int rate_open(struct inode *inode, struct file *file)
{ return single_open(file, rate_show, inode->i_private); }
static const struct file_operations rate_fops = {
	.owner = THIS_MODULE, .open = rate_open,
	.read = seq_read, .write = rate_write,
	.llseek = seq_lseek, .release = single_release,
};

/* The debugfs knobs this block owns; the registration itself is central so
 * probe creates one directory. */
void rtl8198c_qos_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("storm", 0600, eth->debugfs_dir, eth, &storm_fops);
	debugfs_create_file("mirror", 0600, eth->debugfs_dir, eth, &mirror_fops);
	debugfs_create_file("dscp", 0600, eth->debugfs_dir, eth, &dscp_fops);
	debugfs_create_file("rate", 0600, eth->debugfs_dir, eth, &rate_fops);
}
