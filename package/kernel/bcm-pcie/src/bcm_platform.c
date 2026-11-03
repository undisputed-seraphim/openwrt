/*
 * BCM963xx platform glue for the PCIe host controller.
 *
 * The vendor driver built against the BCA BSP (board library, PMC driver,
 * NVRAM, OTP and strap drivers, UBUS). Those subsystems are not part of the
 * OpenWrt kernel, so this file provides the pieces the PCIe driver needs:
 *
 * - PCIe power sequencing through the PMC BPCM keyhole interface, following
 *   the vendor pmc_pcie_power_up()/pmc_pcie_power_down() for BCM63178
 *   (PMC impl 3.x uses direct BPCM register access, not DQM commands).
 * - Chip ID/revision and memory size queries.
 * - No-op implementations of OTP/strap/NVRAM/UBUS lookups.
 *
 * Notes:
 * - 63178 is the only supported SoC here, hence the constant chip ID/rev.
 * - The vendor UBUS helper is itself unimplemented in the GPL tree
 *   (returns -1), so the stub below matches its behaviour.
 * - Resistor-calibration readout needs the PMC firmware; skip it (-1) and
 *   let the PHY use its defaults.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/device.h>

#include <board.h>
#include <shared_utils.h>
#include <bcm_otp.h>
#include <bcm_strap_drv.h>
#include <flash_common.h>
#include <pmc_core_api.h>
#include <pmc_pcie.h>

#define BCM63178_CHIP_ID	0x63178
#define BCM63178_CHIP_REV	0xA0

/* PMC PMB keyhole registers (BCM63178 PMC, procmon block) */
#define PMB_CONFIG		0x0100
#define PMB_NUM_REGS_SHIFT	20
#define PMB_NUM_REGS_MASK	0x3ff
#define PMB_KEYHOLE0_CONTROL	0x0110
#define PMB_KEYHOLE0_WR_DATA	0x0114
#define PMB_KEYHOLE0_RD_DATA	0x011c

#define PMC_PMBM_START		BIT(31)
#define PMC_PMBM_TIMEOUT	BIT(30)
#define PMC_PMBM_BUSY		BIT(28)
#define PMC_PMBM_BUS_SHIFT	20
#define PMC_PMBM_READ		(0 << 24)
#define PMC_PMBM_WRITE		(1 << 24)

/* BPCM registers of a PCIe core (BCM63178 BPCM layout) */
#define BPCM_SR_CONTROL_WORD	5	/* offset 0x14 / 4 */
#define BPCM_ZONE0_CTRL_WORD	32	/* offset 0x80 / 4 */
#define BPCM_ZONE_CTRL_PWR_DN_REQ	BIT(1)
#define BPCM_ZONE_CTRL_PWR_UP_REQ	BIT(2)
#define BPCM_ZONE_CTRL_MEM_PWR_CTL_EN	BIT(3)
#define BPCM_ZONE_CTRL_BLK_RESET_ASSERT	BIT(4)
#define BPCM_ZONE_CTRL_DPG_CTL_EN	BIT(0)
#define BPCM_ZONE_CTRL_PWR_ON_STATE	BIT(18)

#define PCIE0_PMB_DEVADDR	0x1006	/* bus 1, device 6 */
#define SOFT_RST_PCIE_PD	BIT(2)

static void __iomem *pmb_base;
static DEFINE_MUTEX(pmb_lock);

static void __iomem *pmc_procmon_base(void)
{
	struct device_node *np;
	void __iomem *base;

	if (pmb_base)
		return pmb_base;

	np = of_find_compatible_node(NULL, NULL, "brcm,bca-pmc-3-2");
	if (!np)
		return NULL;

	/* reg-names = "pmc", "procmon", ... */
	base = of_iomap(np, 1);
	of_node_put(np);
	if (!base)
		return NULL;

	pmb_base = base;
	return pmb_base;
}

static int pmb_bpcm_read(u32 dev_addr, u32 word_offset, u32 *value)
{
	void __iomem *base;
	u32 bus, addr, ctl;
	int ret = 0;

	mutex_lock(&pmb_lock);
	base = pmc_procmon_base();
	if (!base) {
		mutex_unlock(&pmb_lock);
		return -ENODEV;
	}

	bus = (dev_addr >> 12) & 0x3;
	addr = ((dev_addr & 0xff) *
		((readl(base + PMB_CONFIG) >> PMB_NUM_REGS_SHIFT) &
		 PMB_NUM_REGS_MASK)) | word_offset;

	writel(PMC_PMBM_START | (bus << PMC_PMBM_BUS_SHIFT) | PMC_PMBM_READ |
	       addr, base + PMB_KEYHOLE0_CONTROL);

	ctl = readl(base + PMB_KEYHOLE0_CONTROL);
	while (ctl & PMC_PMBM_BUSY)
		ctl = readl(base + PMB_KEYHOLE0_CONTROL);

	if (ctl & PMC_PMBM_TIMEOUT)
		ret = -ETIMEDOUT;
	else
		*value = readl(base + PMB_KEYHOLE0_RD_DATA);

	mutex_unlock(&pmb_lock);
	return ret;
}

static int pmb_bpcm_write(u32 dev_addr, u32 word_offset, u32 value)
{
	void __iomem *base;
	u32 bus, addr, ctl;
	int ret = 0;

	mutex_lock(&pmb_lock);
	base = pmc_procmon_base();
	if (!base) {
		mutex_unlock(&pmb_lock);
		return -ENODEV;
	}

	bus = (dev_addr >> 12) & 0x3;
	addr = ((dev_addr & 0xff) *
		((readl(base + PMB_CONFIG) >> PMB_NUM_REGS_SHIFT) &
		 PMB_NUM_REGS_MASK)) | word_offset;

	writel(value, base + PMB_KEYHOLE0_WR_DATA);
	writel(PMC_PMBM_START | (bus << PMC_PMBM_BUS_SHIFT) | PMC_PMBM_WRITE |
	       addr, base + PMB_KEYHOLE0_CONTROL);

	ctl = readl(base + PMB_KEYHOLE0_CONTROL);
	while (ctl & PMC_PMBM_BUSY)
		ctl = readl(base + PMB_KEYHOLE0_CONTROL);

	if (ctl & PMC_PMBM_TIMEOUT)
		ret = -ETIMEDOUT;

	mutex_unlock(&pmb_lock);
	return ret;
}

static int bcm_pcie_zone_power_on(u32 dev_addr, unsigned int zone)
{
	u32 word = BPCM_ZONE0_CTRL_WORD + zone * 8;
	u32 ctrl;
	int ret;

	ret = pmb_bpcm_read(dev_addr, word, &ctrl);
	if (ret)
		return ret;

	if (!(ctrl & BPCM_ZONE_CTRL_PWR_ON_STATE)) {
		ctrl &= ~BPCM_ZONE_CTRL_PWR_DN_REQ;
		ctrl |= BPCM_ZONE_CTRL_DPG_CTL_EN |
			BPCM_ZONE_CTRL_PWR_UP_REQ |
			BPCM_ZONE_CTRL_MEM_PWR_CTL_EN |
			BPCM_ZONE_CTRL_BLK_RESET_ASSERT;
		ret = pmb_bpcm_write(dev_addr, word, ctrl);
	}

	return ret;
}

static int bcm_pcie_zone_power_off(u32 dev_addr, unsigned int zone)
{
	u32 word = BPCM_ZONE0_CTRL_WORD + zone * 8;
	u32 ctrl;
	int ret;

	ret = pmb_bpcm_read(dev_addr, word, &ctrl);
	if (ret)
		return ret;

	ctrl |= BPCM_ZONE_CTRL_PWR_DN_REQ;
	ctrl &= ~BPCM_ZONE_CTRL_PWR_UP_REQ;

	return pmb_bpcm_write(dev_addr, word, ctrl);
}

int pmc_pcie_power_up(int unit, int is_dual_lane)
{
	u32 sr_ctrl = 0;

	if (unit != 0)
		return -EINVAL;

	if (bcm_pcie_zone_power_on(PCIE0_PMB_DEVADDR, 0)) {
		pr_err("bcm-pcie: PCIe%d power on failed\n", unit);
		return -EIO;
	}

	mdelay(10);

	if (is_dual_lane) {
		/* bit7: strap override, bit6: 0 = dual lane */
		pmb_bpcm_write(PCIE0_PMB_DEVADDR, BPCM_SR_CONTROL_WORD, 0xff);
		mdelay(10);
		sr_ctrl = 0x80;
	}

	return pmb_bpcm_write(PCIE0_PMB_DEVADDR, BPCM_SR_CONTROL_WORD, sr_ctrl);
}
EXPORT_SYMBOL(pmc_pcie_power_up);

int pmc_pcie_power_down(int unit, int is_dual_lane)
{
	if (unit != 0)
		return -EINVAL;

	if (pmb_bpcm_write(PCIE0_PMB_DEVADDR, BPCM_SR_CONTROL_WORD,
			   SOFT_RST_PCIE_PD))
		return -EIO;

	mdelay(10);

	return bcm_pcie_zone_power_off(PCIE0_PMB_DEVADDR, 0);
}
EXPORT_SYMBOL(pmc_pcie_power_down);

int kerSysGetChipId(void)
{
	return BCM63178_CHIP_ID;
}
EXPORT_SYMBOL(kerSysGetChipId);

unsigned int UtilGetChipRev(void)
{
	return BCM63178_CHIP_REV;
}
EXPORT_SYMBOL(UtilGetChipRev);

unsigned long long kerSysGetMemorySize(void)
{
	struct device_node *np;
	struct resource res;
	unsigned long long size = 0;

	np = of_find_node_by_path("/memory");
	if (np) {
		if (of_address_to_resource(np, 0, &res) == 0)
			size = resource_size(&res);
		of_node_put(np);
	}

	return size;
}
EXPORT_SYMBOL(kerSysGetMemorySize);

/* No NVRAM subsystem in this tree: behave like a missing variable. */
int eNvramGet(char *param, char *value, int len)
{
	(void)param;
	(void)value;
	(void)len;
	return -1;
}
EXPORT_SYMBOL(eNvramGet);

/* No OTP driver: report "not disabled". */
int bcm_otp_is_pcie_port_disabled(unsigned int pcie_port_num, unsigned int *val)
{
	(void)pcie_port_num;
	if (val)
		*val = 0;
	return -1;
}
EXPORT_SYMBOL(bcm_otp_is_pcie_port_disabled);

/* No strap driver: report the property as absent. */
int bcm_strap_parse_and_test(struct device_node *np, const char *consumer_name)
{
	(void)np;
	(void)consumer_name;
	return -1;
}
EXPORT_SYMBOL(bcm_strap_parse_and_test);

/* Unimplemented in the vendor tree as well (see misc/ubus/bcm_ubus.c). */
int ubus_decode_pcie_wnd_cfg(u32 base, u32 size, u32 core)
{
	(void)base;
	(void)size;
	(void)core;
	return -1;
}
EXPORT_SYMBOL(ubus_decode_pcie_wnd_cfg);

/*
 * Resistor calibration lives in the PMC firmware; without it, ask the PHY
 * code to keep its defaults.
 */
int GetRCalSetting_1UM_VERT(int *rcal)
{
	(void)rcal;
	return -ENODEV;
}
EXPORT_SYMBOL(GetRCalSetting_1UM_VERT);
