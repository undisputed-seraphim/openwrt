// SPDX-License-Identifier: GPL-2.0-or-later

/* PCIe host controller driver for Realtek SoCs */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "../pci.h"

/* Host config register offsets */
#define RTPCIE_HOSTCFG_CAP	0x70
#define RTPCIE_HOSTCFG_ENABLE	0x80c
#define RTPCIE_ENABLE_BIT17	BIT(17)
#define RTPCIE_LINK_STATUS	0x728
#define RTPCIE_LINKUP_MASK	GENMASK(4, 0)
#define RTPCIE_IS_LINKUP	(BIT(4) | BIT(0))
#define RTPCIE_PCI_CMD_BIT20	BIT(20)

/* RTL8198C system control registers, offsets into the "realtek,sys" syscon */
#define RTL8198C_SYS_CLK_MANAGE		0x0010
#define RTL8198C_SYS_PCIE_PHY0		0x0050
#define RTL8198C_SYS_PCIE_PHY1		0x0054
#define RTL8198C_SYS_PCIE_MUX		0x0104
#define RTL8198C_SYS_PCIE1_RST		0x010c

/*
 * Port F GPIO block of the RTL8198C.  The PCIe1 PERST# line is wired to
 * GPIO 17 there.  It is driven through the syscon instead of the GPIO
 * driver so that the complete reset sequence stays in one place; no other
 * consumer uses that line.
 */
#define RTL8198C_SYS_GPIO_PEFGH_CNR	0x351c
#define RTL8198C_SYS_GPIO_PEFGH_DIR	0x3524
#define RTL8198C_SYS_GPIO_PEFGH_DAT	0x3528
#define RTL8198C_PCIE1_PERST		BIT(17)

#define RTL8198C_CLK_PCIE		(BIT(12) | BIT(13) | BIT(14) | \
					 BIT(16) | BIT(19) | BIT(20))
#define RTL8198C_CLK_PCIE0		BIT(14)
#define RTL8198C_CLK_PCIE1		BIT(16)
#define RTL8198C_CLK_PCIE0_PERST	BIT(26)

#define RTL8198C_PCIE_MUX_MASK		(3 << 20)
#define RTL8198C_PCIE_MUX_SEL		BIT(20)

#define RTPCIE_LINK_POLL_MS		10
#define RTPCIE_LINK_POLL_TRIES		100
#define RTPCIE_LINK_RETRIES		5

/**
 * struct rtpcie_soc_data - per-SoC properties
 * @rtl8198c: RTL8198C needs the SoC level reset sequence, a per function
 *            devcfg aperture, no hostext function select and 32-bit
 *            config space accesses for sub-word requests
 */
struct rtpcie_soc_data {
	bool rtl8198c;
};

/**
 * struct rtpcie_ctrl - Realtek PCIe port information
 * @dev: pointer to PCIe device
 * @hostcfg_base: host config register base
 * @hostext_base: host extension register base (unused on RTL8198C)
 * @devcfg_base: device config register base
 * @sys_map: system control registers (RTL8198C only)
 * @reset_gpio: gpio reset
 * @phy: pointer to PHY control block
 * @soc: per-SoC properties
 * @mem_window: memory window of the PCIe bridge (RTL8198C only)
 * @port: PCIe port number (RTL8198C only)
 * @bus_number: assigned PCI bus number
 */
struct rtpcie_ctrl {
	struct device *dev;
	void __iomem *hostcfg_base;
	void __iomem *hostext_base;
	void __iomem *devcfg_base;
	struct regmap *sys_map;
	struct gpio_desc *reset_gpio;
	struct phy *phy;
	const struct rtpcie_soc_data *soc;
	struct resource *mem_window;
	u32 port;
	u8 bus_number;
};

static void __iomem *rtpcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				    int where)
{
	struct rtpcie_ctrl *pcie = bus->sysdata;

	if (pcie->bus_number == 0xff)
		pcie->bus_number = bus->number;

	if (bus->number != pcie->bus_number)
		return NULL;

	if (!pcie->soc->rtl8198c)
		writel(PCI_FUNC(devfn), pcie->hostext_base);

	switch (PCI_SLOT(devfn)) {
	case 0:
		return pcie->hostcfg_base + where;
	case 1:
		/*
		 * The RTL8198C endpoint is on the local bus even though the
		 * root port header advertises a secondary bus; its config
		 * space lives one 4 KiB window per function in the device
		 * config aperture.
		 */
		if (pcie->soc->rtl8198c)
			return pcie->devcfg_base + (PCI_FUNC(devfn) << 12) +
			       where;

		return pcie->devcfg_base + where;
	default:
		return NULL;
	}
}

/*
 * Sub-word reads of the host bridge config space return wrong values on the
 * RTL8198C, so they are emulated with a 32-bit aligned read.
 */
static int rtpcie_config_read(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 *val)
{
	struct rtpcie_ctrl *pcie = bus->sysdata;
	void __iomem *addr;

	addr = rtpcie_map_bus(bus, devfn, where);
	if (!addr) {
		*val = ~0;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	if (pcie->soc->rtl8198c && size < 4) {
		u32 data;

		addr = (void __iomem *)((uintptr_t)addr & ~3UL);
		data = ioread32(addr);
		*val = (data >> ((where & 3) << 3)) &
		       (size == 1 ? 0xff : 0xffff);

		return PCIBIOS_SUCCESSFUL;
	}

	switch (size) {
	case 1:
		*val = ioread8(addr);
		break;
	case 2:
		*val = ioread16(addr);
		break;
	default:
		*val = ioread32(addr);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int rtpcie_config_write(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 val)
{
	struct rtpcie_ctrl *pcie = bus->sysdata;
	void __iomem *addr;

	addr = rtpcie_map_bus(bus, devfn, where);
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* Host bridge config writes must be full 32-bit accesses. */
	if (pcie->soc->rtl8198c && PCI_SLOT(devfn) == 0 && size < 4) {
		u32 mask = size == 1 ? 0xff : 0xffff;
		int shift = (where & 3) << 3;
		u32 data;

		addr = (void __iomem *)((uintptr_t)addr & ~3UL);
		data = ioread32(addr);
		data = (data & ~(mask << shift)) | ((val & mask) << shift);
		iowrite32(data, addr);

		return PCIBIOS_SUCCESSFUL;
	}

	switch (size) {
	case 1:
		iowrite8(val, addr);
		break;
	case 2:
		iowrite16(val, addr);
		break;
	default:
		iowrite32(val, addr);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops rtpcie_ops = {
	.map_bus = rtpcie_map_bus,
	.read = rtpcie_config_read,
	.write = rtpcie_config_write,
};

static bool rtpcie_link_up(struct rtpcie_ctrl *pcie)
{
	u32 val;
	int i;

	for (i = 0; i < RTPCIE_LINK_POLL_TRIES; i++) {
		val = readl(pcie->hostcfg_base + RTPCIE_LINK_STATUS);
		if ((val & RTPCIE_LINKUP_MASK) == RTPCIE_IS_LINKUP)
			return true;
		msleep(RTPCIE_LINK_POLL_MS);
	}

	return false;
}

static void rtpcie_host_enable(struct rtpcie_ctrl *pcie)
{
	u32 val;

	val = RTPCIE_PCI_CMD_BIT20 | PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
	      PCI_COMMAND_MASTER;
	writel(val, pcie->hostcfg_base + PCI_COMMAND);

	/*
	 * Clear the maximum payload size bits of DEVCTL.  They are accessed
	 * as a full 32-bit word because the RTL8198C cannot do sub-word
	 * accesses on this aperture; the upper half is DEVCTL2 and is left
	 * untouched by the read-modify-write.
	 */
	val = readl(pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVCTL);
	val &= ~(PCI_EXP_DEVCTL_PAYLOAD << 16);
	writel(val, pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVCTL);

	val = readl(pcie->hostcfg_base + RTPCIE_HOSTCFG_ENABLE);
	writel(val | RTPCIE_ENABLE_BIT17,
	       pcie->hostcfg_base + RTPCIE_HOSTCFG_ENABLE);

	usleep_range(1000, 2000);
}

/*
 * The RTL8198C endpoint is on the same bus as the root port, so the PCI core
 * sees no bridge in between it and never opens the forwarding window of the
 * host bridge.  Worse, the core actively closes it while assigning the
 * (empty) secondary bus, so it has to be opened after resource assignment.
 */
static void rtpcie_rtl8198c_mem_window(struct rtpcie_ctrl *pcie)
{
	struct resource *res = pcie->mem_window;
	u32 base, limit;

	if (!res)
		return;

	base = ((res->start >> 20) << 4) & 0xfff0;
	limit = ((res->end >> 20) << 4) & 0xfff0;

	/* Memory base/limit, mirrored into the prefetchable window. */
	writel((limit << 16) | base, pcie->hostcfg_base + 0x20);
	writel((limit << 16) | base, pcie->hostcfg_base + 0x24);
}

static void rtpcie_print_link_speed(struct rtpcie_ctrl *pcie)
{
	u16 link;

	link = readw(pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_LNKSTA);
	dev_info(pcie->dev, "link up, %s\n",
		 pci_speed_string(pcie_link_speed[FIELD_GET(PCI_EXP_LNKSTA_CLS,
							   link)]));
}

/* RTL8198C: pulse PERST# of the endpoint attached to this port. */
static void rtpcie_rtl8198c_perst(struct rtpcie_ctrl *pcie)
{
	struct regmap *sys = pcie->sys_map;

	if (!pcie->port) {
		regmap_update_bits(sys, RTL8198C_SYS_CLK_MANAGE,
				   RTL8198C_CLK_PCIE0_PERST, 0);
		msleep(1000);
		regmap_update_bits(sys, RTL8198C_SYS_CLK_MANAGE,
				   RTL8198C_CLK_PCIE0_PERST,
				   RTL8198C_CLK_PCIE0_PERST);
		return;
	}

	regmap_update_bits(sys, RTL8198C_SYS_PCIE1_RST, 7 << 10, 3 << 10);
	msleep(500);

	regmap_update_bits(sys, RTL8198C_SYS_GPIO_PEFGH_CNR,
			   RTL8198C_PCIE1_PERST, 0);
	regmap_update_bits(sys, RTL8198C_SYS_GPIO_PEFGH_DIR,
			   RTL8198C_PCIE1_PERST, RTL8198C_PCIE1_PERST);
	regmap_update_bits(sys, RTL8198C_SYS_GPIO_PEFGH_DAT,
			   RTL8198C_PCIE1_PERST, 0);
	msleep(1000);
	regmap_update_bits(sys, RTL8198C_SYS_GPIO_PEFGH_DAT,
			   RTL8198C_PCIE1_PERST, RTL8198C_PCIE1_PERST);
}

static int rtpcie_hw_init_rtl8198c(struct rtpcie_ctrl *pcie)
{
	struct regmap *sys = pcie->sys_map;
	unsigned int phy_rst, clk_port;
	bool link_up = false;
	int ret, retry;

	dev_info(pcie->dev, "RTL8198C PCIe port %u\n", pcie->port);

	phy_rst = pcie->port ? RTL8198C_SYS_PCIE_PHY1 :
			       RTL8198C_SYS_PCIE_PHY0;
	clk_port = pcie->port ? RTL8198C_CLK_PCIE1 : RTL8198C_CLK_PCIE0;

	/* Enable the PCIe clocks and the clock of this port. */
	regmap_update_bits(sys, RTL8198C_SYS_CLK_MANAGE, RTL8198C_CLK_PCIE,
			   RTL8198C_CLK_PCIE);
	regmap_update_bits(sys, RTL8198C_SYS_CLK_MANAGE, clk_port, clk_port);
	msleep(500);

	/* Reset the PHY MDIO interface. */
	regmap_write(sys, phy_rst, 0x8);
	regmap_write(sys, phy_rst, 0x9);
	regmap_write(sys, phy_rst, 0xb);
	msleep(1000);

	/* Switch the PCIe MUX to the reset path. */
	regmap_update_bits(sys, RTL8198C_SYS_PCIE_MUX, RTL8198C_PCIE_MUX_MASK,
			   RTL8198C_PCIE_MUX_SEL);

	/* Program the PHY tuning parameters. */
	ret = phy_init(pcie->phy);
	if (ret) {
		dev_err(pcie->dev, "failed to initialize pcie-phy\n");
		return ret;
	}

	for (retry = 0; retry < RTPCIE_LINK_RETRIES && !link_up; retry++) {
		rtpcie_rtl8198c_perst(pcie);

		/* Release the PHY from reset and enable the LTSSM. */
		phy_power_on(pcie->phy);
		msleep(1000);

		link_up = rtpcie_link_up(pcie);
	}

	if (!link_up) {
		dev_err(pcie->dev, "PCIe port %u failed to link up\n",
			pcie->port);
		phy_exit(pcie->phy);
		return -ENODEV;
	}

	rtpcie_host_enable(pcie);
	rtpcie_print_link_speed(pcie);

	return 0;
}

static int rtpcie_hw_init(struct rtpcie_ctrl *pcie)
{
	u16 devctl, link;
	int err;
	u32 val;

	/* Assert reset pcie */
	gpiod_set_value_cansleep(pcie->reset_gpio, 1);

	err = phy_init(pcie->phy);
	if (err) {
		dev_err(pcie->dev, "failed to initialize pcie-phy\n");
		return err;
	}

	/* Deassert reset pcie */
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);

	/* Wait for Link Up */
	err = readl_poll_timeout(pcie->hostcfg_base + RTPCIE_LINK_STATUS, val,
				 (val & RTPCIE_LINKUP_MASK) == RTPCIE_IS_LINKUP,
				 10000, 100000);

	if (err) {
		dev_err(pcie->dev, "PCIE Link Up Failed!\n");
		phy_exit(pcie->phy);
		return err;
	}

	msleep(100);

	/* Enable PCIE host */
	val = RTPCIE_PCI_CMD_BIT20 | PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	writel(val, pcie->hostcfg_base + PCI_COMMAND);

	/* Clear max payload size bits in DEVCTL */
	devctl = readw(pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVCTL);
	devctl &= ~PCI_EXP_DEVCTL_PAYLOAD;
	writew(devctl, pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVCTL);

	val = readl(pcie->hostcfg_base + RTPCIE_HOSTCFG_ENABLE);
	writel(val | RTPCIE_ENABLE_BIT17, pcie->hostcfg_base + RTPCIE_HOSTCFG_ENABLE);

	usleep_range(1000, 2000);

	link = readw(pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_LNKSTA);
	dev_info(pcie->dev, "link up, %s\n",
		 pci_speed_string(pcie_link_speed[FIELD_GET(PCI_EXP_LNKSTA_CLS, link)]));

	return err;
}

static int rtpcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtpcie_ctrl *pcie;
	struct pci_host_bridge *bridge;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = dev;
	pcie->bus_number = 0xff;
	pcie->soc = of_device_get_match_data(dev);
	if (!pcie->soc)
		return dev_err_probe(dev, -EINVAL, "missing SoC data\n");
	platform_set_drvdata(pdev, pcie);

	pcie->hostcfg_base = devm_platform_ioremap_resource_byname(pdev, "hostcfg");
	if (IS_ERR(pcie->hostcfg_base))
		return dev_err_probe(dev, PTR_ERR(pcie->hostcfg_base),
				     "failed to map hostcfg\n");

	if (pcie->soc->rtl8198c) {
		struct resource_entry *win;

		ret = of_property_read_u32(dev->of_node, "slot-num", &pcie->port);
		if (ret)
			return dev_err_probe(dev, ret, "failed to get slot-num\n");

		pcie->sys_map = syscon_regmap_lookup_by_phandle(dev->of_node,
								"realtek,sys");
		if (IS_ERR(pcie->sys_map))
			return dev_err_probe(dev, PTR_ERR(pcie->sys_map),
					     "failed to get system registers\n");

		resource_list_for_each_entry(win, &bridge->windows) {
			if (resource_type(win->res) == IORESOURCE_MEM) {
				pcie->mem_window = win->res;
				break;
			}
		}
	} else {
		pcie->hostext_base = devm_platform_ioremap_resource_byname(pdev, "hostext");
		if (IS_ERR(pcie->hostext_base))
			return dev_err_probe(dev, PTR_ERR(pcie->hostext_base),
					     "failed to map hostext\n");
	}

	pcie->devcfg_base = devm_platform_ioremap_resource_byname(pdev, "devcfg");
	if (IS_ERR(pcie->devcfg_base))
		return dev_err_probe(dev, PTR_ERR(pcie->devcfg_base),
				     "failed to map devcfg\n");

	pcie->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pcie->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pcie->reset_gpio),
				     "failed to get reset GPIO\n");
	if (!pcie->reset_gpio && !pcie->soc->rtl8198c)
		return dev_err_probe(dev, -ENODEV, "failed to get reset GPIO\n");

	pcie->phy = devm_of_phy_get(dev, dev->of_node, NULL);
	if (IS_ERR(pcie->phy))
		return dev_err_probe(dev, PTR_ERR(pcie->phy), "failed to get pcie-phy");

	if (pcie->soc->rtl8198c)
		ret = rtpcie_hw_init_rtl8198c(pcie);
	else
		ret = rtpcie_hw_init(pcie);
	if (ret)
		return ret;

	bridge->sysdata = pcie;
	bridge->ops = &rtpcie_ops;

	if (pcie->soc->rtl8198c) {
		struct pci_bus *bus, *child;

		/*
		 * Same as pci_host_probe(), but the forwarding window of the
		 * host bridge is opened after resource assignment and before
		 * the endpoint driver is bound.
		 */
		pci_lock_rescan_remove();
		ret = pci_scan_root_bus_bridge(bridge);
		pci_unlock_rescan_remove();
		if (ret) {
			dev_err(dev, "failed to scan root bus\n");
			phy_exit(pcie->phy);
			return ret;
		}

		bus = bridge->bus;
		pci_assign_unassigned_root_bus_resources(bus);
		rtpcie_rtl8198c_mem_window(pcie);

		list_for_each_entry(child, &bus->children, node)
			pcie_bus_configure_settings(child);

		pci_lock_rescan_remove();
		pci_bus_add_devices(bus);
		pci_unlock_rescan_remove();
	} else {
		ret = pci_host_probe(bridge);
		if (ret) {
			phy_exit(pcie->phy);
			return ret;
		}
	}

	return 0;
}

static const struct rtpcie_soc_data rtl9607c_soc_data = {
	.rtl8198c = false,
};

static const struct rtpcie_soc_data rtl8198c_soc_data = {
	.rtl8198c = true,
};

static const struct of_device_id rtpcie_of_match[] = {
	{ .compatible = "realtek,rtl9607c-pcie", .data = &rtl9607c_soc_data },
	{ .compatible = "realtek,rtl8198c-pcie", .data = &rtl8198c_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtpcie_of_match);

static struct platform_driver rtpcie_driver = {
	.probe = rtpcie_probe,
	.driver = {
		.name = "realtek-pcie",
		.of_match_table = rtpcie_of_match,
	},
};
builtin_platform_driver(rtpcie_driver);

MODULE_DESCRIPTION("PCIe host controller driver for Realtek SoC");
MODULE_LICENSE("GPL");
