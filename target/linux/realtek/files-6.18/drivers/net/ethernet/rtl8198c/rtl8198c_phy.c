// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C switch core NIC -- L1: MDIO, GPHY, link
 *
 * MDIO access, the GPHY SRAM98C patches and NCTL calibration, PHY
 * power/Green Ethernet, and the ethtool link settings.
 */
#include "rtl8198c.h"

/* ===== MDIO helpers ===== */
void rtl8198c_mdio_write(u32 phy, u32 reg, u32 val)
{
	sys_w32(MDCIOCR, MDIO_WRITE | (phy << 24) | (reg << 16) | val);
	while (sys_r32(MDCIOSR) & MDIO_BUSY);
}
/* PHY 0 is strapped to address 8 on this board. */
/* MDIO address of a port's GPHY: port 0 answers at 8, ports 1-4 at their
 * own number.  The same mapping is repeated inline in gphy_wb(),
 * sram98c_write() and phy_refine(), which all walk the five PHYs. */
u32 rtl8198c_port_phy_id(u32 port) { return port ? port : 8; }

u32 rtl8198c_mdio_read(u32 phy, u32 reg)
{
	sys_w32(MDCIOCR, (phy << 24) | (reg << 16));
	while (sys_r32(MDCIOSR) & MDIO_BUSY);
	return sys_r32(MDCIOSR) & 0xffff;
}
static void gphy_page(u32 phy, u32 page) { rtl8198c_mdio_write(phy, 31, page); }

static void gphy_wb(u32 page, u32 reg, u32 mask, u32 val)
{
	int i;
	for (i = 0; i < 5; i++) {
		u32 id = (i == 0) ? 8 : i;
		gphy_page(id, page);
		if (mask) {
			u32 v = rtl8198c_mdio_read(id, reg);
			rtl8198c_mdio_write(id, reg, (v & mask) | val);
		} else {
			rtl8198c_mdio_write(id, reg, val);
		}
		gphy_page(id, 0);
	}
}
static void gphy_wb_port(u32 phy, u32 page, u32 reg, u32 mask, u32 val)
{
	u32 id = (phy == 0) ? 8 : phy;
	gphy_page(id, page);
	if (mask) {
		u32 v = rtl8198c_mdio_read(id, reg);
		rtl8198c_mdio_write(id, reg, (v & mask) | val);
	} else {
		rtl8198c_mdio_write(id, reg, val);
	}
	gphy_page(id, 0);
}

static void gphy_clear_irq(u32 page, u32 reg)
{
	int i;
	for (i = 0; i < 5; i++) {
		u32 id = (i == 0) ? 8 : i;
		gphy_page(id, page);
		rtl8198c_mdio_read(id, reg);
		gphy_page(id, 0);
	}
}
/* Write one SRAM98C word: page 0xa43, register 27 is the word address and
 * 28 the data.  The table is write-only through this indirect port and
 * cannot be read back, which is why the green/power settings are reported
 * as "applied" rather than read from the PHY. */
static void sram98c_write(u32 addr, u32 val)
{
	int i;
	for (i = 0; i < 5; i++) {
		u32 id = (i == 0) ? 8 : i;
		gphy_page(id, 0xa43);
		rtl8198c_mdio_write(id, 27, addr);
		rtl8198c_mdio_write(id, 28, val);
		gphy_page(id, 0);
	}
}

/* Green Ethernet ("link-on" and cable-length power saving) is a set of
 * SRAM98C words, applied to all five PHYs at once.  The values are the two
 * power-saving branches of the GPHY setup. */
struct sram98c_word {
	u16	addr;
	u8	val;
};

static const struct sram98c_word green_on_sram[] = {
	{ 0x809a, 0x89 }, { 0x809b, 0x11 }, { 0x80a3, 0x92 },
	{ 0x80a4, 0x33 }, { 0x80a0, 0x00 }, { 0x8088, 0x89 },
	{ 0x8089, 0x11 }, { 0x808e, 0x00 },
};

static const struct sram98c_word green_off_sram[] = {
	{ 0x809a, 0xa4 }, { 0x809b, 0x44 }, { 0x80a3, 0xa4 },
	{ 0x80a4, 0x44 }, { 0x80a0, 0x23 }, { 0x8088, 0xa4 },
	{ 0x8089, 0x44 }, { 0x808e, 0x23 },
};

static void green_set(bool on)
{
	const struct sram98c_word *w = on ? green_on_sram : green_off_sram;
	int i;

	for (i = 0; i < ARRAY_SIZE(green_on_sram); i++)
		sram98c_write(w[i].addr, w[i].val);
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

/* Upload the GPHY's DSP/NCTL calibration.
 *
 * The table lives in the PHY's DSP memory behind the indirect NCTL port,
 * and the 1000BASE-T receiver does not train without it.  Per port:
 * request access (page 0xb82, bit 4), wait for the NCTL acknowledge (page
 * 0xb80, register 16, bit 6), stream the address/data pairs through
 * registers 27/28, close the block with a zero pair and release.  A port
 * that does not acknowledge within 1000 polls is skipped rather than
 * stalling the rest of the init.
 */
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
			if (rtl8198c_mdio_read(pid, 16) & BIT(6))
				break;
		}
		gphy_page(pid, 0);
		if (timeout <= 0) {
			pr_warn("phy_refine port %d: request timeout\n", i);
			gphy_page(pid, 0);
			continue;
		}

		rtl8198c_mdio_write(pid, 27, 0x8146);
		rtl8198c_mdio_write(pid, 28, 0x4800);
		rtl8198c_mdio_write(pid, 27, 0xb82e);
		rtl8198c_mdio_write(pid, 28, 0x0001);

		for (j = 0; j < ARRAY_SIZE(phy_98c_para); j += 2)
			rtl8198c_mdio_write(pid, phy_98c_para[j], phy_98c_para[j + 1]);

		rtl8198c_mdio_write(pid, 27, 0x0000);
		rtl8198c_mdio_write(pid, 28, 0x0000);
		gphy_wb_port(i, 0xb82, 23, 0, 0x0000);
		rtl8198c_mdio_write(pid, 27, 0x8146);
		rtl8198c_mdio_write(pid, 28, 0x0000);

		gphy_wb_port(i, 0xb82, 16, ~(1 << 4), 0 << 4);
		timeout = 1000;
		gphy_page(pid, 0xb80);
		while (timeout--) {
			if (!(rtl8198c_mdio_read(pid, 16) & BIT(6)))
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

/* ADC offset RAM code, loaded into each calibrated GPHY */
static const u16 phy_98c_ado[] = {
	27, 0x83de, 28, 0xaf83, 28, 0xeaaf, 28, 0x83ed,
	28, 0xaf83, 28, 0xf0af, 28, 0x83f3, 28, 0xaf0c,
	28, 0x0caf, 28, 0x83ed, 28, 0xaf83, 28, 0xf0af,
	28, 0x83f3, 27, 0xb818, 28, 0x0bf1, 27, 0xb81a,
	28, 0xfffd, 27, 0xb81c, 28, 0xfffd, 27, 0xb81e,
	28, 0xfffd, 27, 0xb832, 28, 0x0001,
};

/* Load the ADC offset patch into one GPHY. */
static void gphy_ado_patch(u32 port)
{
	u32 phy = port ? port : 8;
	u32 v;
	int i, timeout;

	/* wait until the patch engine is idle (page 0xa46 reg 21 bits 10:8 = 3) */
	timeout = 20000;
	while (timeout--) {
		gphy_page(phy, 0xa46);
		v = rtl8198c_mdio_read(phy, 21);
		gphy_page(phy, 0);
		if (((v >> 8) & 0x7) == 3)
			break;
	}

	gphy_wb_port(port, 0xb82, 16, ~BIT(4), BIT(4));	/* patch request */

	timeout = 20000;
	while (timeout--) {
		gphy_page(phy, 0xb80);
		v = rtl8198c_mdio_read(phy, 16);
		gphy_page(phy, 0);
		if (v & BIT(6))
			break;
	}

	for (i = 0; i < ARRAY_SIZE(phy_98c_ado); i += 2)
		rtl8198c_mdio_write(phy, phy_98c_ado[i], phy_98c_ado[i + 1]);

	gphy_wb_port(port, 0xb82, 16, ~BIT(4), 0);	/* release */
}

/* Per-port analogue calibration from the SoC efuse.
 *
 * Every GPHY has a 32-bit efuse word: bits 31:28 hold the RC code, bits
 * 27:24 the R code and bits 23:8 the ADC bias offset; bit 7 marks the port
 * as calibrated.  The calibration is always enabled and the values are
 * populated on this board
 * (RC 6, R 9/8).  The PHY defaults are good enough for 10/100 and leave
 * 1000BASE-T unable to train: the receiver never comes up even though
 * master/slave resolves.
 */
static void gphy_efuse_calibrate(void)
{
	u32 efuse[5];
	u32 rc, r, v;
	int i, timeout;

	sys_w32(EFUSE_TIMING_CONTROL, 0x030C174F);
	sys_w32(EFUSE_CONFIG, 0x00040112);

	for (i = 0; i < 5; i++) {
		sys_w32(EFUSE_CONFIG, sys_r32(EFUSE_CONFIG) | EFUSE_CFG_INT_STS);
		sys_w32(EFUSE_CMD, i);
		timeout = 20000;
		while (!(sys_r32(EFUSE_CONFIG) & EFUSE_CFG_INT_STS) && timeout--)
			;
		if (!timeout) {
			pr_warn("rtl8198c-eth: efuse read timeout on port %d\n", i);
			return;
		}
		efuse[i] = sys_r32(EFUSE_RW_DATA);
	}

	/* RC code → page 0xbcd, R code → page 0xbce, both nibble-replicated */
	for (i = 0; i < 5; i++) {
		rc = (efuse[i] >> 28) & 0xF;
		r = (efuse[i] >> 24) & 0xF;

		if (rc && rc != 0xF) {
			v = rc | (rc << 4) | (rc << 8) | (rc << 12);
			gphy_wb_port(i, 0xbcd, 0x16, 0, v);
			gphy_wb_port(i, 0xbcd, 0x17, 0, v);
		}
		if (r && r != 0xF) {
			v = r | (r << 4) | (r << 8) | (r << 12);
			gphy_wb_port(i, 0xbce, 0x10, 0, v);
			gphy_wb_port(i, 0xbce, 0x11, 0, v);
		}
	}

	/* calibrated ports also get the ADC offset patch and bias value */
	for (i = 0; i < 5; i++) {
		if (!(efuse[i] & 0x80))
			continue;
		gphy_ado_patch(i);
		gphy_wb_port(i, 0xbcf, 22, 0, (efuse[i] >> 8) & 0xFFFF);
	}
}

/* GPHY workarounds */
void rtl8198c_switch_gphy_init(void)
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
	phy_refine();
	/* Page 0 register 23 bit 8 lets the GPHY accept 16K jumbo frames
	 * ("PHY not accept the 16kB jumbo frame issue"). */
	for (i = 0; i < 5; i++) {
		u32 pid = (i == 0) ? 8 : i;
		u32 v;

		gphy_page(pid, 0);
		v = rtl8198c_mdio_read(pid, 23);
		rtl8198c_mdio_write(pid, 23, v | BIT(8));
		gphy_page(pid, 0);
	}
	/* Auto-down-speed: with only two pairs in the cable the GPHY drops
	 * its 1000BASE-T advertisement and links at 100M.  That is the PHY
	 * working as intended, not a driver fault. */
	gphy_wb(0xa44, 17, ~(1 << 2), 1 << 2);
	/* Order: NCTL refine, auto-down-speed, efuse calibration, green */
	gphy_efuse_calibrate();
	green_set(true);
	for (i = 0; i < 5; i++)
		sys_w32(PCRP0 + i*4, sys_r32(PCRP0 + i*4) & ~BIT(25));
}

/* ===== PHY power and green Ethernet =====
 *
 * Powering a GPHY down (BMCR bit 11) also has to stop the MAC from reporting
 * a link for that port, so PCRP is put into force mode with link polling
 * while the PHY is off; powering back up clears force mode again.
 * The SRAM98C words behind the green settings cannot be read back, so
 * "green" reports the mode the driver last applied.
 *
 * debugfs "phy_power": 5-bit mask of powered-down ports (bit 0 = port 0)
 *   cat phy_power
 *   echo 0x10 > phy_power     -> power down port 4
 *   echo 0 > phy_power        -> power all ports back up
 * debugfs "green": on|off
 */
static u32 phy_bmcr(u32 port)
{
	u32 phy = rtl8198c_port_phy_id(port);

	rtl8198c_mdio_read(phy, MII_BMCR);	/* a read after a write can be stale */
	return rtl8198c_mdio_read(phy, MII_BMCR);
}

static u32 phy_power_get(void)
{
	u32 port, mask = 0;

	for (port = 0; port < ETH_PORTS; port++)
		if (phy_bmcr(port) & BMCR_PDOWN)
			mask |= BIT(port);
	return mask;
}

/* Power a single PHY down or up.
 *
 * BMCR power-down alone is not enough: the MAC keeps reporting a link, so
 * PCRP is put into force mode with link polling while the PHY is off, and
 * force mode is cleared again on power-up.
 */
static void phy_power_set(u32 port, bool down)
{
	u32 phy = rtl8198c_port_phy_id(port);
	u16 bmcr = phy_bmcr(port);
	u32 pcrp = sys_r32(PCRP0 + port * 4);

	if (down) {
		pcrp |= PCRP_EN_FORCE_MODE | PCRP_POLL_LINK;
		pcrp &= ~PCRP_FORCE_LINK;
		sys_w32(PCRP0 + port * 4, pcrp);
		bmcr |= BMCR_PDOWN;
	} else {
		bmcr &= ~BMCR_PDOWN;
	}
	rtl8198c_mdio_write(phy, MII_BMCR, bmcr);
	if (!down) {
		pcrp &= ~(PCRP_EN_FORCE_MODE | PCRP_POLL_LINK);
		sys_w32(PCRP0 + port * 4, pcrp);
	}
}

static int phy_power_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "0x%02x\n", phy_power_get());
	return 0;
}

static ssize_t phy_power_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	char kbuf[16];
	unsigned int mask;
	ssize_t len;
	u32 port;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';
	if (sscanf(kbuf, "%x", &mask) != 1 || mask > 0x1F)
		return -EINVAL;

	for (port = 0; port < ETH_PORTS; port++)
		phy_power_set(port, !!(mask & BIT(port)));
	return len;
}

static int phy_power_open(struct inode *inode, struct file *file)
{ return single_open(file, phy_power_show, inode->i_private); }
static const struct file_operations phy_power_fops = {
	.owner = THIS_MODULE, .open = phy_power_open,
	.read = seq_read, .write = phy_power_write,
	.llseek = seq_lseek, .release = single_release,
};

static int green_show(struct seq_file *s, void *unused)
{
	struct rtl8198c_eth *eth = s->private;

	seq_printf(s, "%s\n", eth->green_enabled ? "on" : "off");
	return 0;
}

static ssize_t green_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct rtl8198c_eth *eth = filp->f_inode->i_private;
	char kbuf[8];
	ssize_t len;

	len = simple_write_to_buffer(kbuf, sizeof(kbuf) - 1, ppos, buf, count);
	if (len <= 0)
		return len;
	kbuf[len] = '\0';
	if (kbuf[len - 1] == '\n')
		kbuf[len - 1] = '\0';
	if (!strcmp(kbuf, "on"))
		eth->green_enabled = true;
	else if (!strcmp(kbuf, "off"))
		eth->green_enabled = false;
	else
		return -EINVAL;
	green_set(eth->green_enabled);
	return len;
}

static int green_open(struct inode *inode, struct file *file)
{ return single_open(file, green_show, inode->i_private); }
static const struct file_operations green_fops = {
	.owner = THIS_MODULE, .open = green_open,
	.read = seq_read, .write = green_write,
	.llseek = seq_lseek, .release = single_release,
};

/* ===== Link settings =====
 *
 * One netdev per port, so these act on that port's GPHY and PCRP and
 * report that port's link.  The driver has no carrier state of its own, so
 * a port without a cable reports unknown speed and duplex.
 */
static int eth_port_linked(u32 port)
{
	return !!(rtl8198c_mdio_read(rtl8198c_port_phy_id(port), MII_BMSR) & BMSR_LSTATUS);
}

static int eth_port_is_gigabit(u32 port)
{
	return !!(rtl8198c_mdio_read(rtl8198c_port_phy_id(port), MII_STAT1000) &
		  STAT1000_LOCAL_RX_OK);
}

/* Report every port's link to its netdev.  Called when a port opens and from
 * the NAPI poll after LINK_CHANGE_IP; the switch only interrupts on a
 * transition and PSRP already holds the answer, so no MDIO is needed.  A
 * port that is not admin-up keeps the state for its next open. */
void rtl8198c_link_update(struct rtl8198c_eth *eth)
{
	int i;

	for (i = 0; i < ETH_PORTS; i++) {
		struct net_device *dev = eth->netdev[i];

		if (!dev)
			continue;
		if (sys_r32(PSRP0 + i * 4) & PSRP_LINK_UP)
			netif_carrier_on(dev);
		else
			netif_carrier_off(dev);
	}
}

/* PHY advertisement for the requested link modes (10/100 in reg 4,
 * 1000BASE-T in reg 9) plus the matching PCRP advertisement bits. */
static void eth_port_advertise(u32 port, unsigned long *modes, u32 *pcrp_bits)
{
	u32 phy = rtl8198c_port_phy_id(port);
	u16 adv = ADVERTISE_CSMA, ctrl1000 = 0;
	u32 bits = 0;

	if (linkmode_test_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, modes)) {
		adv |= ADVERTISE_10HALF;
		bits |= NWAY_ABILITY_10MH;
	}
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, modes)) {
		adv |= ADVERTISE_10FULL;
		bits |= NWAY_ABILITY_10MF;
	}
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, modes)) {
		adv |= ADVERTISE_100HALF;
		bits |= NWAY_ABILITY_100MH;
	}
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, modes)) {
		adv |= ADVERTISE_100FULL;
		bits |= NWAY_ABILITY_100MF;
	}
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, modes)) {
		ctrl1000 |= ADVERTISE_1000FULL;
		bits |= NWAY_ABILITY_1000MF;
	}
	if (!bits)		/* nothing selected: advertise everything */
		bits = NWAY_ABILITY_ALL, adv |= ADVERTISE_10HALF |
			ADVERTISE_10FULL | ADVERTISE_100HALF | ADVERTISE_100FULL,
			ctrl1000 |= ADVERTISE_1000FULL;

	rtl8198c_mdio_write(phy, MII_ADVERTISE, adv);
	rtl8198c_mdio_write(phy, MII_CTRL1000, ctrl1000);
	*pcrp_bits = bits;
}

static void eth_link_ksettings(struct net_device *dev, u32 speed, u32 duplex,
			       bool autoneg, const unsigned long *modes)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	u32 port = p->port;
	u32 phy = rtl8198c_port_phy_id(port);
	u32 pcrp = sys_r32(PCRP0 + port * 4);
	u16 bmcr;

	if (autoneg) {
		u32 bits;

		eth_port_advertise(port, (unsigned long *)modes, &bits);
		pcrp &= ~(PCRP_EN_FORCE_MODE | NWAY_ABILITY_ALL);
		pcrp |= bits;
		sys_w32(PCRP0 + port * 4, pcrp);
		bmcr = BMCR_ANENABLE | BMCR_ANRESTART;
		rtl8198c_mdio_write(phy, MII_BMCR, bmcr);
		return;
	}

	pcrp |= PCRP_EN_FORCE_MODE | PCRP_FORCE_LINK;
	pcrp &= ~(PCRP_FORCE_SPEED_MASK | PCRP_FORCE_DUPLEX_FULL);
	switch (speed) {
	case SPEED_1000:
		pcrp |= PCRP_FORCE_SPEED_1000M;
		break;
	case SPEED_100:
		pcrp |= PCRP_FORCE_SPEED_100M;
		break;
	default:
		pcrp |= PCRP_FORCE_SPEED_10M;
		break;
	}
	if (duplex == DUPLEX_FULL)
		pcrp |= PCRP_FORCE_DUPLEX_FULL;
	sys_w32(PCRP0 + port * 4, pcrp);

	bmcr = 0;
	if (speed == SPEED_100 || speed == SPEED_1000)
		bmcr |= BMCR_SPEED100;	/* no-op for 1000BASE-T */
	if (duplex == DUPLEX_FULL)
		bmcr |= BMCR_FULLDPLX;
	rtl8198c_mdio_write(phy, MII_BMCR, bmcr);
}

int rtl8198c_ethtool_get_link_ksettings(struct net_device *dev,
				      struct ethtool_link_ksettings *cmd)
{
	struct rtl8198c_port *p = netdev_priv(dev);
	u32 port = p->port;

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT,
			 cmd->link_modes.supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT,
			 cmd->link_modes.supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT,
			 cmd->link_modes.supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT,
			 cmd->link_modes.supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
			 cmd->link_modes.supported);
	linkmode_copy(cmd->link_modes.advertising, cmd->link_modes.supported);

	cmd->base.autoneg = AUTONEG_ENABLE;
	cmd->base.port = PORT_MII;
	cmd->base.phy_address = rtl8198c_port_phy_id(port);

	/* 1G is detectable (STAT1000 bit 13); a 10M link is not
	 * distinguishable from 100M by a standard register, so report 100
	 * for anything that is not 1G. */
	if (eth_port_linked(port)) {
		cmd->base.speed = eth_port_is_gigabit(port) ? SPEED_1000 :
				  SPEED_100;
		cmd->base.duplex = DUPLEX_FULL;
	} else {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
	}
	return 0;
}

int rtl8198c_ethtool_set_link_ksettings(struct net_device *dev,
				      const struct ethtool_link_ksettings *cmd)
{
	bool autoneg = cmd->base.autoneg == AUTONEG_ENABLE;

	if (!autoneg) {
		if (cmd->base.duplex != DUPLEX_FULL &&
		    cmd->base.duplex != DUPLEX_HALF)
			return -EINVAL;
		if (cmd->base.speed != SPEED_10 && cmd->base.speed != SPEED_100 &&
		    cmd->base.speed != SPEED_1000)
			return -EINVAL;
	}
	eth_link_ksettings(dev, cmd->base.speed, cmd->base.duplex, autoneg,
			   cmd->link_modes.advertising);
	return 0;
}

int rtl8198c_ethtool_nway_reset(struct net_device *dev)
{
	struct rtl8198c_port *p = netdev_priv(dev);

	rtl8198c_mdio_write(rtl8198c_port_phy_id(p->port), MII_BMCR,
			   BMCR_ANENABLE | BMCR_ANRESTART);
	return 0;
}

/* The debugfs knobs this block owns; the registration itself is central so
 * probe creates one directory. */
void rtl8198c_phy_debugfs_init(struct rtl8198c_eth *eth)
{
	debugfs_create_file("phy_power", 0600, eth->debugfs_dir, eth, &phy_power_fops);
	debugfs_create_file("green", 0600, eth->debugfs_dir, eth, &green_fops);
}
