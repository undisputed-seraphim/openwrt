// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Realtek RTL8198C
 *
 * Copyright (C) 2026 Tan Li Boon <undisputed.seraphim@gmail.com>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <asm/pci.h>

/*
 * The RTL8198C PCIe host bridge has a hardware quirk where 8/16-bit
 * config reads on the host bridge return wrong values.  Enable the
 * REV_B quirk unconditionally: all sub-32-bit config reads use 32-bit
 * aligned read + shift/mask.  Host bridge config writes are always
 * 32-bit RMW (__raw_readl/__raw_writel — no byteswap, matching the
 * old BSP's volatile KSEG1 path and our HOSTCFG fix in 002fb61f06).
 *
 * Sub-32-bit writes to downstream device BAR2 hang port 0 but work
 * on port 1.  The rtw88 32-bit MMIO patch handles this independently.
 * Device CONFIG space sub-word access (via sized_ioread/sized_iowrite)
 * is fine on both ports.
 */
#define CONFIG_RTL8198C_REV_B_QUIRK

/* SoC-level system registers */
#define CLK_MANAGE		0xB8000010
#define SYS_PCIE_PHY0		0xB8000050
#define SYS_PCIE_PHY1		0xB8000054
#define SYS_STRAP		0xB8000008
#define SYS_PCIE_MUX		0xB8000104
#define SYS_PCIE1_RST		0xB800010C

#define GPIO_BASE		0xB8003500
#define PEFGHCNR_REG		(0x01C + GPIO_BASE)
#define PEFGHDIR_REG		(0x024 + GPIO_BASE)
#define PEFGHDAT_REG		(0x028 + GPIO_BASE)

/* Host config register offsets */
#define HOSTCFG_PCIE_CAP	0x70
#define HOSTCFG_ENABLE		0x80c
#define HOSTCFG_ENABLE_BIT	BIT(17)
#define HOSTCFG_LINK_STATUS	0x728
#define HOSTCFG_LINKUP_MASK	0x1f
#define HOSTCFG_IS_LINKUP	0x11
#define HOSTCFG_CMD_BIT20	BIT(20)

struct rtl8198c_phy_param {
	u8 reg;
	u16 value;
};

static const struct rtl8198c_phy_param phy_params_40m[] __initconst = {
	{ 0x3, 0x7b31 },
	{ 0x6, 0xe258 },
	{ 0xF, 0x400F },
	{ 0xd, 0x1764 },
	{ 0x19, 0xFC70 },
};

static const struct rtl8198c_phy_param phy_params_25m[] __initconst = {
	{ 0x3, 0x3031 },
	{ 0x6, 0xe058 },
	{ 0xF, 0x400F },
	{ 0x19, 0xFC70 },
};

struct rtl8198c_pcie {
	struct device *dev;
	struct pci_controller controller;
	void __iomem *hostcfg_base;
	void __iomem *hostext_base;
	void __iomem *devcfg0_base;
	void __iomem *devcfg1_base;
	const struct rtl8198c_phy_param *phy_params;
	int phy_param_count;
	int slot_nr;
	int irq;
	int bus_nr;
	bool link_up;
};

static struct rtl8198c_pcie *pcie_instances[2];

static inline struct rtl8198c_pcie *
get_pcie_from_bus(struct pci_bus *bus)
{
	struct pci_controller *ctrl = bus->sysdata;

	return container_of(ctrl, struct rtl8198c_pcie, controller);
}

static int get_busno0(void)
{
	return pcie_instances[0] ? pcie_instances[0]->bus_nr : 0xff;
}

static void set_busno0(int busno)
{
	if (pcie_instances[0])
		pcie_instances[0]->bus_nr = busno;
}

static int get_busno1(void)
{
	return pcie_instances[1] ? pcie_instances[1]->bus_nr : 0xff;
}

static void set_busno1(int busno)
{
	if (pcie_instances[1])
		pcie_instances[1]->bus_nr = busno;
}

/* ---- MDIO / PHY helpers ---- */

static void rtl8198c_phy_mdio_write(struct rtl8198c_pcie *pcie, u8 reg, u16 val)
{
	__raw_writel((val << 16) | (reg << 8) | 1, pcie->hostext_base + 0x00);
	mdelay(1);
}

static void rtl8198c_mdio_reset(void __iomem *sys_phy)
{
	iowrite32(0x8 | 0x0 | 0x0, sys_phy);
	iowrite32(0x8 | 0x0 | 0x1, sys_phy);
	iowrite32(0x8 | 0x2 | 0x1, sys_phy);
}

static void rtl8198c_phy_init(struct rtl8198c_pcie *pcie)
{
	bool is_40m;
	int i;

	is_40m = !!(ioread32((void __iomem *)SYS_STRAP) & BIT(24));
	pr_debug("rtl8198c pcie: %s crystal\n", is_40m ? "40M" : "25M");

	if (is_40m)
		mdelay(500);

	for (i = 0; i < pcie->phy_param_count; i++)
		rtl8198c_phy_mdio_write(pcie, pcie->phy_params[i].reg,
					pcie->phy_params[i].value);
}

/* ---- PERST (device reset) ---- */

static void rtl8198_pcie0_device_perst(void)
{
	iowrite32(ioread32((void __iomem *)CLK_MANAGE) & ~BIT(26),
		  (void __iomem *)CLK_MANAGE);
	mdelay(1000);
	iowrite32(ioread32((void __iomem *)CLK_MANAGE) | BIT(26),
		  (void __iomem *)CLK_MANAGE);
}

static void rtl8198_pcie1_device_perst(void)
{
	iowrite32((ioread32((void __iomem *)SYS_PCIE1_RST) & ~(7 << 10)) |
			  (3 << 10),
		  (void __iomem *)SYS_PCIE1_RST);
	mdelay(500);

	iowrite32(ioread32((void __iomem *)PEFGHCNR_REG) & ~(0x20000),
		  (void __iomem *)PEFGHCNR_REG);
	iowrite32(ioread32((void __iomem *)PEFGHDIR_REG) | 0x20000,
		  (void __iomem *)PEFGHDIR_REG);
	iowrite32(ioread32((void __iomem *)PEFGHDAT_REG) & ~0x20000,
		  (void __iomem *)PEFGHDAT_REG);
	mdelay(1000);
	iowrite32(ioread32((void __iomem *)PEFGHDAT_REG) | 0x20000,
		  (void __iomem *)PEFGHDAT_REG);
}

static void rtl8198c_phy_reset(struct rtl8198c_pcie *pcie)
{
	void __iomem *pwrcr = pcie->hostext_base + 0x08;

	__raw_writel(0x01, pwrcr);
	__raw_writel(0x81, pwrcr);
}

/* ---- Link training ---- */

static bool rtl8198c_link_up(struct rtl8198c_pcie *pcie)
{
	u32 val;
	int timeout;

	for (timeout = 100; timeout > 0; timeout -= 10) {
		val = __raw_readl(pcie->hostcfg_base + HOSTCFG_LINK_STATUS);
		if ((val & HOSTCFG_LINKUP_MASK) == HOSTCFG_IS_LINKUP)
			return true;
		mdelay(10);
	}
	return false;
}

static void rtl8198c_host_enable(struct rtl8198c_pcie *pcie)
{
	u16 devctl;
	u32 val;

	__raw_writel(HOSTCFG_CMD_BIT20 | PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		     PCI_COMMAND_MASTER,
		     pcie->hostcfg_base + PCI_COMMAND);

	devctl = __raw_readw(pcie->hostcfg_base + HOSTCFG_PCIE_CAP +
			     PCI_EXP_DEVCTL);
	devctl &= ~PCI_EXP_DEVCTL_PAYLOAD;
	__raw_writew(devctl, pcie->hostcfg_base + HOSTCFG_PCIE_CAP +
		     PCI_EXP_DEVCTL);

	val = __raw_readl(pcie->hostcfg_base + HOSTCFG_ENABLE);
	__raw_writel(val | HOSTCFG_ENABLE_BIT,
		     pcie->hostcfg_base + HOSTCFG_ENABLE);
}

/* ---- Full reset & init sequence ---- */

static int rtl8198c_pcie_reset(struct rtl8198c_pcie *pcie)
{
	int slot = pcie->slot_nr;
	bool link_up;
	int retry;

	dev_info(pcie->dev, "rtl8198c pcie: port = %d\n", slot);

	iowrite32(ioread32((void __iomem *)CLK_MANAGE) |
			  (BIT(12) | BIT(13) | BIT(14) | BIT(16) | BIT(19) |
			   BIT(20)),
		  (void __iomem *)CLK_MANAGE);

	if (slot == 0)
		iowrite32(ioread32((void __iomem *)CLK_MANAGE) | BIT(14),
			  (void __iomem *)CLK_MANAGE);
	else if (slot == 1)
		iowrite32(ioread32((void __iomem *)CLK_MANAGE) | BIT(16),
			  (void __iomem *)CLK_MANAGE);
	else
		return -EINVAL;

	mdelay(500);

	rtl8198c_mdio_reset((slot == 0) ? (void __iomem *)SYS_PCIE_PHY0 :
					  (void __iomem *)SYS_PCIE_PHY1);
	mdelay(500);
	mdelay(500);

	iowrite32(ioread32((void __iomem *)SYS_PCIE_MUX) & ~(3 << 20),
		  (void __iomem *)SYS_PCIE_MUX);
	iowrite32(ioread32((void __iomem *)SYS_PCIE_MUX) | BIT(20),
		  (void __iomem *)SYS_PCIE_MUX);

	rtl8198c_phy_init(pcie);

	link_up = false;
	for (retry = 0; retry < 5 && !link_up; retry++) {
		if (slot == 0)
			rtl8198_pcie0_device_perst();
		else
			rtl8198_pcie1_device_perst();

		rtl8198c_phy_reset(pcie);
		mdelay(1000);

		link_up = rtl8198c_link_up(pcie);
		if (!link_up)
			pr_debug("rtl8198c pcie: port %d retry %d failed\n",
				 slot, retry);
	}
	pcie->link_up = link_up;
	if (!link_up) {
		dev_info(pcie->dev, "rtl8198c pcie: port %d Cannot LinkUP\n", slot);
		return -ENODEV;
	}

	dev_info(pcie->dev, "rtl8198c pcie: Device:Vendor ID = %08x\n",
		ioread32(pcie->devcfg0_base));

	{
		u32 mem_start = pcie->controller.mem_resource->start;
		u32 mem_end = pcie->controller.mem_resource->end;
		u32 io_start = pcie->controller.io_resource->start;
		u32 io_end = pcie->controller.io_resource->end;
		u16 mem_base, mem_limit;

		mem_base = (mem_start >> 20) << 4;
		mem_limit = (mem_end >> 20) << 4;
		__raw_writel((mem_limit << 16) | mem_base,
			     pcie->hostcfg_base + 0x20);
		__raw_writel((mem_limit << 16) | mem_base,
			     pcie->hostcfg_base + 0x24);

		__raw_writel(((((io_start >> 8) & 0xf0) | 0x01) << 0) |
			     ((((io_end >> 8) & 0xf0) | 0x01) << 8),
			     pcie->hostcfg_base + 0x1C);

		pr_debug("rtl8198c pcie: port %d bridge mem window %08x-%08x\n",
			 pcie->slot_nr, mem_start, mem_end);
	}

	rtl8198c_host_enable(pcie);

	/* dump HOSTCFG for diagnostics */
	{
		int i;
		u32 regs[16];

		for (i = 0; i < 16; i++)
			regs[i] = __raw_readl(pcie->hostcfg_base + i * 4);
		pr_info("rtl8198c-pcie port%d HOSTCFG: "
			"00=%08x 04=%08x 08=%08x 0c=%08x "
			"10=%08x 14=%08x 18=%08x 1c=%08x\n",
			pcie->slot_nr,
			regs[0x0], regs[0x1], regs[0x2], regs[0x3],
			regs[0x4], regs[0x5], regs[0x6], regs[0x7]);
		for (i = 0; i < 8; i++)
			regs[i] = __raw_readl(pcie->hostcfg_base + 0x20 + i * 4);
		pr_info("rtl8198c-pcie port%d HOSTCFG: "
			"20=%08x 24=%08x 28=%08x 2c=%08x "
			"30=%08x 34=%08x 38=%08x 3c=%08x\n",
			pcie->slot_nr,
			regs[0], regs[1], regs[2], regs[3],
			regs[4], regs[5], regs[6], regs[7]);
		pr_info("rtl8198c-pcie port%d HOSTCFG: "
			"70=%08x 728=%08x 80c=%08x\n",
			pcie->slot_nr,
			__raw_readl(pcie->hostcfg_base + 0x70),
			__raw_readl(pcie->hostcfg_base + 0x728),
			__raw_readl(pcie->hostcfg_base + 0x80c));
	}

	return 0;
}

/* Config-space read/write via the HOSTCFG and DEVCFG apertures.
 * Host bridge config reads always use __raw_readl (no byteswap).
 * Host bridge config writes always use 32-bit RMW via __raw_readl +
 * __raw_writel.  Downstream device config space goes through
 * sized_ioread/sized_iowrite — safe for PCI config space on both ports.
 */

#define MAX_NUM_DEV	4

static unsigned int sized_ioread(int size, void __iomem *addr)
{
#ifdef CONFIG_RTL8198C_REV_B_QUIRK
	if (size == 1 || size == 2) {
		void __iomem *aligned = (void __iomem *)((uintptr_t)addr & ~3);
		u32 v = ioread32(aligned);
		int shift = ((uintptr_t)addr & 3) * 8;

		return (v >> shift) & (size == 1 ? 0xff : 0xffff);
	}
	return ioread32(addr);
#else
	switch (size) {
	case 1:
		return ioread8(addr);
	case 2:
		return ioread16(addr);
	default:
		return ioread32(addr);
	}
#endif
}

static void sized_iowrite(int size, void __iomem *addr, unsigned int data)
{
	switch (size) {
	case 1:
		iowrite8(data, addr);
		break;
	case 2:
		iowrite16(data, addr);
		break;
	default:
		iowrite32(data, addr);
		break;
	}
}

static int rtl8198c_pci_read(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 *val)
{
	struct rtl8198c_pcie *pcie = get_pcie_from_bus(bus);
	u32 data;
	void __iomem *addr;

	if (pcie->bus_nr == 0xff)
		pcie->bus_nr = bus->number;

	if (bus->number == pcie->bus_nr) {
		if (PCI_SLOT(devfn) == 0) {
			addr = pcie->hostcfg_base + (where & ~3);
			data = __raw_readl(addr);

			switch (size) {
			case 1:
				*val = (data >> ((where & 3) << 3)) & 0xff;
				break;
			case 2:
				*val = (data >> ((where & 3) << 3)) & 0xffff;
				break;
			default:
				*val = data;
				break;
			}
		} else if (PCI_SLOT(devfn) == 1) {
			addr = pcie->devcfg0_base +
			       (PCI_FUNC(devfn) << 12) + where;
			*val = sized_ioread(size, addr);
		} else {
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
	} else {
		if (PCI_SLOT(devfn) >= MAX_NUM_DEV)
			return PCIBIOS_DEVICE_NOT_FOUND;

		data = (bus->number << 8) | (PCI_SLOT(devfn) << 3) |
		       PCI_FUNC(devfn);
		iowrite32(data, pcie->hostext_base + 0x0C);

		addr = pcie->devcfg1_base + where;
		*val = sized_ioread(size, addr);
	}

	return PCIBIOS_SUCCESSFUL;
}

static int rtl8198c_pci_write(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 val)
{
	struct rtl8198c_pcie *pcie = get_pcie_from_bus(bus);
	u32 data;
	void __iomem *addr;

	if (pcie->bus_nr == 0xff)
		pcie->bus_nr = bus->number;

	if (bus->number == pcie->bus_nr) {
		if (PCI_SLOT(devfn) == 0) {
			addr = pcie->hostcfg_base + (where & ~3);
			data = __raw_readl(addr);

			switch (size) {
			case 1:
				data = (data & ~(0xff << ((where & 3) << 3))) |
				       (val << ((where & 3) << 3));
				break;
			case 2:
				data = (data &
					~(0xffff << ((where & 3) << 3))) |
				       (val << ((where & 3) << 3));
				break;
			default:
				data = val;
				break;
			}
			__raw_writel(data, addr);
		} else if (PCI_SLOT(devfn) == 1) {
			addr = pcie->devcfg0_base +
			       (PCI_FUNC(devfn) << 12) + where;
			sized_iowrite(size, addr, val);
		} else {
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
	} else {
		if (PCI_SLOT(devfn) >= MAX_NUM_DEV)
			return PCIBIOS_DEVICE_NOT_FOUND;

		data = (bus->number << 8) | (PCI_SLOT(devfn) << 3) |
		       PCI_FUNC(devfn);
		iowrite32(data, pcie->hostext_base + 0x0C);

		addr = pcie->devcfg1_base + where;
		sized_iowrite(size, addr, val);
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops rtl8198c_pci_ops = {
	.read = rtl8198c_pci_read,
	.write = rtl8198c_pci_write,
};

/* ---- pcibios hooks ---- */

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	return 0;
}

int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct rtl8198c_pcie *pcie = get_pcie_from_bus(dev->bus);

	return pcie->irq;
}

/* ---- Probe / platform driver ---- */

static int rtl8198c_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl8198c_pcie *pcie;
	int err;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->dev = dev;

	pcie->hostcfg_base =
		devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(pcie->hostcfg_base))
		return dev_err_probe(dev, PTR_ERR(pcie->hostcfg_base),
				     "failed to map hostcfg\n");

	pcie->hostext_base =
		devm_platform_get_and_ioremap_resource(pdev, 1, NULL);
	if (IS_ERR(pcie->hostext_base))
		return dev_err_probe(dev, PTR_ERR(pcie->hostext_base),
				     "failed to map hostext\n");

	pcie->devcfg0_base =
		devm_platform_get_and_ioremap_resource(pdev, 2, NULL);
	if (IS_ERR(pcie->devcfg0_base))
		return dev_err_probe(dev, PTR_ERR(pcie->devcfg0_base),
				     "failed to map devcfg0\n");

	pcie->devcfg1_base =
		devm_platform_get_and_ioremap_resource(pdev, 3, NULL);
	if (IS_ERR(pcie->devcfg1_base))
		return dev_err_probe(dev, PTR_ERR(pcie->devcfg1_base),
				     "failed to map devcfg1\n");

	err = of_property_read_u32(dev->of_node, "slot-num", &pcie->slot_nr);
	if (err)
		pcie->slot_nr = 0;

	pcie->irq = platform_get_irq(pdev, 0);
	if (pcie->irq < 0)
		return dev_err_probe(dev, pcie->irq, "failed to get IRQ\n");

	if (ioread32((void __iomem *)SYS_STRAP) & BIT(24)) {
		pcie->phy_params = phy_params_40m;
		pcie->phy_param_count = ARRAY_SIZE(phy_params_40m);
	} else {
		pcie->phy_params = phy_params_25m;
		pcie->phy_param_count = ARRAY_SIZE(phy_params_25m);
	}

	pcie->controller.pci_ops = &rtl8198c_pci_ops;
	pcie->controller.io_resource =
		devm_kzalloc(dev, sizeof(struct resource), GFP_KERNEL);
	pcie->controller.mem_resource =
		devm_kzalloc(dev, sizeof(struct resource), GFP_KERNEL);
	if (!pcie->controller.io_resource || !pcie->controller.mem_resource)
		return -ENOMEM;

	pcie->bus_nr = 0xff;

	if (pcie->slot_nr == 0) {
		pcie->controller.get_busno = get_busno0;
		pcie->controller.set_busno = set_busno0;
	} else {
		pcie->controller.get_busno = get_busno1;
		pcie->controller.set_busno = set_busno1;
	}

	pcie_instances[pcie->slot_nr] = pcie;

	pci_load_of_ranges(&pcie->controller, dev->of_node);

	/* Expand ioport_resource to cover the PCI IO window.  The default
	 * IO_SPACE_LIMIT of 0xFFFF is too small for our physical IO range.
	 */
	if (pcie->controller.io_resource->start < ioport_resource.start)
		ioport_resource.start = pcie->controller.io_resource->start;
	if (pcie->controller.io_resource->end > ioport_resource.end)
		ioport_resource.end = pcie->controller.io_resource->end;

	rtl8198c_pcie_reset(pcie);

	if (pcie->link_up)
		register_pci_controller(&pcie->controller);

	return 0;
}

static const struct of_device_id rtl8198c_pci_match[] = {
	{ .compatible = "realtek,rtl8198c-pci" },
	{},
};

static struct platform_driver rtl8198c_pci_driver = {
	.probe = rtl8198c_pci_probe,
	.driver = {
		.name = "pci-rtl8198c",
		.of_match_table = rtl8198c_pci_match,
	},
};

static int __init bsp_pcie_init(void)
{
	return platform_driver_register(&rtl8198c_pci_driver);
}

arch_initcall(bsp_pcie_init);
