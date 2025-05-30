// SPDX-License-Identifier: GPL-2.0+

#include <linux/device.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/module.h>

#include <asm/pci.h>
#include <asm-generic/iomap.h>

#define BSP_PCIE0_H_CFG     0xB8B00000
#define BSP_PCIE0_H_EXT     0xB8B01000
#define BSP_PCIE0_H_MDIO    (BSP_PCIE0_H_EXT + 0x00)
#define BSP_PCIE0_H_INTSTR  (BSP_PCIE0_H_EXT + 0x04)
#define BSP_PCIE0_H_PWRCR   (BSP_PCIE0_H_EXT + 0x08)
#define BSP_PCIE0_H_IPCFG   (BSP_PCIE0_H_EXT + 0x0C)
#define BSP_PCIE0_H_MISC    (BSP_PCIE0_H_EXT + 0x10)
#define BSP_PCIE0_D_CFG0    0xB8B10000
#define BSP_PCIE0_D_CFG1    0xB8B11000
#define BSP_PCIE0_D_MSG     0xB8B12000

#define BSP_PCIE1_H_CFG     0xB8B20000
#define BSP_PCIE1_H_EXT     0xB8B21000
#define BSP_PCIE1_H_MDIO    (BSP_PCIE1_H_EXT + 0x00)
#define BSP_PCIE1_H_INTSTR  (BSP_PCIE1_H_EXT + 0x04)
#define BSP_PCIE1_H_PWRCR   (BSP_PCIE1_H_EXT + 0x08)
#define BSP_PCIE1_H_IPCFG   (BSP_PCIE1_H_EXT + 0x0C)
#define BSP_PCIE1_H_MISC    (BSP_PCIE1_H_EXT + 0x10)
#define BSP_PCIE1_D_CFG0    0xB8B30000
#define BSP_PCIE1_D_CFG1    0xB8B31000
#define BSP_PCIE1_D_MSG     0xB8B32000


#define PCIE0_RC_EXT_BASE (0xb8b01000)
#define PCIE1_RC_EXT_BASE (0xb8b21000)
//RC Extended register
#define PCIE0_MDIO      (PCIE0_RC_EXT_BASE+0x00)
#define PCIE1_MDIO      (PCIE1_RC_EXT_BASE+0x00)
//MDIO
#define PCIE_MDIO_DATA_OFFSET (16)
#define PCIE_MDIO_DATA_MASK (0xffff <<PCIE_MDIO_DATA_OFFSET)
#define PCIE_MDIO_REG_OFFSET (8)
#define PCIE_MDIO_RDWR_OFFSET (0)

#define PCIE0_CLOCK_MANAGE	(void*)0xB8000010
#define PCIE1_CLOCK_MANAGE	(void*)0xB800001C

#define GPIO_BASE			(void*)0xB8003500
#define PEFGHCNR_REG		(0x01C + GPIO_BASE) /* Port EFGH control */
#define PEFGHPTYPE_REG		(0x020 + GPIO_BASE) /* Port EFGH type */
#define PEFGHDIR_REG		(0x024 + GPIO_BASE) /* Port EFGH direction */
#define PEFGHDAT_REG		(0x028 + GPIO_BASE) /* Port EFGH data */

/*
 * IRQ Controller
 */
#define BSP_IRQ_CPU_BASE	0
#define BSP_IRQ_CPU_NUM		8

#define BSP_IRQ_GIC_BASE	(BSP_IRQ_CPU_BASE + BSP_IRQ_CPU_NUM)

#define BSP_PCIE0_IRQ		(BSP_IRQ_GIC_BASE + 31)
#define BSP_PCIE1_IRQ		(BSP_IRQ_GIC_BASE + 32)

static unsigned char pci0_bus_nr = 0xff;
static unsigned char pci1_bus_nr = 0xff;

//========================================================================================

static inline void rtl8198c_host_pcie_set_phy_mdio_write(uintptr_t addr, u32 regaddr, u16 val)
{
	volatile int i;

	iowrite32(((regaddr & 0x1f) << PCIE_MDIO_REG_OFFSET) | ((val & 0xffff) << PCIE_MDIO_DATA_OFFSET) | (1 << PCIE_MDIO_RDWR_OFFSET), (void*)addr);

	for (i = 0; i < 5555; i++) ;
}

static inline void rtl8198c_pcie_phy_reset(uintptr_t phy)
{
	iowrite32(0x01, (void*)phy); //bit7: PHY reset=0	bit0: Enable LTSSM=1
	iowrite32(0x81, (void*)phy); //bit7: PHY reset=1	bit0: Enable LTSSM=1
}

static inline void rtl8198c_set_phy_mdio_write(unsigned int regaddr, unsigned short val, uintptr_t mdioaddr)
{
	volatile int i;
	iowrite32(((regaddr & 0x1f) << 8) | ((val & 0xffff) << 16) | (1 << 0), (void*)mdioaddr);
}

static inline void rtl8198c_pcie0_device_perst(void)
{
	iowrite32(ioread32(PCIE0_CLOCK_MANAGE) & ~(1 << 26), PCIE0_CLOCK_MANAGE);
	mdelay(1000);
	iowrite32(ioread32(PCIE0_CLOCK_MANAGE) |  (1 << 26), PCIE0_CLOCK_MANAGE);
}

static inline void rtl8198c_pcie1_device_perst(void)
{
	iowrite32(ioread32(PCIE1_CLOCK_MANAGE) & (~(7 << 10) | (3 << 10)), PCIE1_CLOCK_MANAGE);
	mdelay(1000);
	iowrite32(ioread32(PEFGHCNR_REG) & ~0x20000, PEFGHCNR_REG);
	iowrite32(ioread32(PEFGHDIR_REG) |  0x20000, PEFGHDIR_REG);
	iowrite32(ioread32(PEFGHDAT_REG) & ~0x20000, PEFGHDAT_REG);
	mdelay(1000);
	iowrite32(ioread32(PEFGHDAT_REG) |  0x20000, PEFGHDAT_REG);
}

static inline void rtl8198c_pcie_reset(uintptr_t h_pwrcr, uintptr_t mdio, u32 magic1, u32 magic2)
{
	int result = 0;
	int phy40m = 0;
	printk("Resetting PCIE port.");

	iowrite32(ioread32(PCIE0_CLOCK_MANAGE) | (1<<12|1<<13|1<<14|1<<16|1<<19|1<<20), PCIE0_CLOCK_MANAGE);
	iowrite32(ioread32(PCIE0_CLOCK_MANAGE) | (1<<magic1), PCIE0_CLOCK_MANAGE);
	mdelay(500);

	// MDIO_reset
	iowrite32((1<<3) | (0<<1) | (0<<0), (void*)0xb8000000 + magic2);
	iowrite32((1<<3) | (0<<1) | (1<<0), (void*)0xb8000000 + magic2);
	iowrite32((1<<3) | (1<<1) | (1<<0), (void*)0xb8000000 + magic2);

	mdelay(1000);

	// MDIO_reset_2
#define PCIE_MUX	(void*)0xb8000104
	iowrite32(ioread32(PCIE_MUX) & (~0x3 << 20) | (1 << 20), PCIE_MUX); //PCIe MUX switch to PCIe reset
	phy40m = (ioread32((void*) 0xb8000008) & (1 << 24)) >> 24;
	if (phy40m)
	{
		//40MHz PCIe Parameters
		mdelay(500);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0x3, 0x7b31);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0x6, 0xe258);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0xf, 0x400F);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0xd, 0x1764);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0x19, 0xfc70);
	}
	else
	{
		//25MHz PCIe Parameters
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0x3, 0x3031);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0x6, 0xe058);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0xf, 0x400F);
		rtl8198c_host_pcie_set_phy_mdio_write(mdio, 0x19, 0xfc70);
	}

	rtl8198c_pcie_phy_reset(h_pwrcr);
	mdelay(1000);
}
static inline void rtl8198c_pcie0_reset(void)
{
	rtl8198c_pcie_reset(BSP_PCIE0_H_PWRCR, PCIE0_MDIO, 14, 0x50);
}
static inline void rtl8198c_pcie1_reset(void)
{
	rtl8198c_pcie_reset(BSP_PCIE1_H_PWRCR, PCIE1_MDIO, 16, 0x54);
}

//========================================================================================

static inline u32 pci_read_size(int size, uintptr_t addr)
{
	void* address = (void*)addr;
	u32 ret = 0;
	switch (size) {
	case 1:
		ret = ioread8(address);
		break;
	case 2:
		ret = ioread16(address);
		break;
	case 4:
		ret = ioread32(address);
		break;
	}
	return ret;
}

static inline void pci_write_size(int size, uintptr_t addr, u32 val)
{
	void* address = (void*)addr;
	switch (size) {
	case 1:
		iowrite8(val, address);
		break;
	case 2:
		iowrite16(val, address);
		break;
	case 4:
		iowrite32(val, address);
		break;
	}
}


#define MAX_NUM_DEV			4

static int pci0_config_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	unsigned int slot = PCI_SLOT(devfn);
	u8 func = PCI_FUNC(devfn);
	uintptr_t addr = 0;
	unsigned int data = 0;

	if (pci0_bus_nr == 0xff)
		pci0_bus_nr = bus->number;

	if (bus->number == pci0_bus_nr)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE host controller */
		addr = (BSP_PCIE0_H_CFG + where) & (~0x3);
		data = ioread32((void*)addr);

		if (size == 1)
			*val = (data >> ((where & 3) << 3)) & 0xff;
		else if (size == 2)
			*val = (data >> ((where & 3) << 3)) & 0xffff;
		else
			*val = data;
	}
	else if (bus->number == pci0_bus_nr + 1)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE devices directly connected */
		addr = BSP_PCIE0_D_CFG0 + (func << 12);
		*val = pci_read_size(size, addr + where);
	}
	else
	{
		if (slot >= MAX_NUM_DEV)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* Devices connected through bridge */
		iowrite32(((bus->number) << 8) | (slot << 3) | func, (void*)BSP_PCIE0_H_IPCFG);
		addr = BSP_PCIE0_D_CFG1 + where;
		*val = pci_read_size(size, addr + where);
	}
	return PCIBIOS_SUCCESSFUL;
}

static int pci0_config_write(struct pci_bus *bus, unsigned int devfn, int where, int size, unsigned int val)
{
	unsigned int slot = PCI_SLOT(devfn);
	u8 func = PCI_FUNC(devfn);
	uintptr_t addr = 0;
	unsigned int data = 0;

	if (pci0_bus_nr == 0xff)
		pci0_bus_nr = bus->number;

	if (bus->number == pci0_bus_nr)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE host controller */
		addr = (BSP_PCIE0_H_CFG + where) & (~0x3);
		data = ioread32((void*)addr);

		if (size == 1)
			data = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
		else if (size == 2)
			data = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
		else
			data = val;

		iowrite32(data, (void*)addr);
	}
	else if (bus->number == pci0_bus_nr + 1)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE devices directly connected */
		addr = BSP_PCIE0_D_CFG0 + (func << 12) + where;
		pci_write_size(size, addr, val);
	}
	else
	{
		if (slot >= MAX_NUM_DEV)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* Devices connected through bridge */
		iowrite32((bus->number << 8) | (slot << 3) | func, (void*)BSP_PCIE0_H_IPCFG);
		addr = BSP_PCIE0_D_CFG1 + where;
		pci_write_size(size, addr, val);
	}
	return PCIBIOS_SUCCESSFUL;
}

//========================================================================================
static int pci1_config_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	unsigned int slot = PCI_SLOT(devfn);
	u8 func = PCI_FUNC(devfn);
	uintptr_t addr = 0;
	unsigned int data = 0;

	if (pci1_bus_nr == 0xff)
		pci1_bus_nr = bus->number;

	if (bus->number == pci1_bus_nr)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE host controller */
		addr = (BSP_PCIE1_H_CFG + where) & (~0x3);
		data = ioread32((void*)addr);

		if (size == 1)
			*val = (data >> ((where & 3) << 3)) & 0xff;
		else if (size == 2)
			*val = (data >> ((where & 3) << 3)) & 0xffff;
		else
			*val = data;
	}
	else if (bus->number == pci1_bus_nr + 1)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE devices directly connected */
		*val = pci_read_size(size, (BSP_PCIE1_D_CFG0 + (func << 12) + where));
	}
	else
	{
		if (slot >= MAX_NUM_DEV)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* Devices connected through bridge */
		iowrite32((bus->number << 8) | (slot << 3) | func, (void*)BSP_PCIE1_H_IPCFG);
		addr = BSP_PCIE1_D_CFG1 + where;
		*val = pci_read_size(size, addr);
	}
	return PCIBIOS_SUCCESSFUL;
}

static int pci1_config_write(struct pci_bus *bus, unsigned int devfn, int where, int size, unsigned int val)
{
	unsigned int slot = PCI_SLOT(devfn);
	u8 func = PCI_FUNC(devfn);
	uintptr_t addr = 0;
	unsigned int data = 0;

	if (pci1_bus_nr == 0xff)
		pci1_bus_nr = bus->number;

	if (bus->number == pci1_bus_nr)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE host controller */
		addr = (BSP_PCIE1_H_CFG + where) & (~0x3);
		data = ioread32((void*)addr);

		if (size == 1)
			data = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
		else if (size == 2)
			data = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
		else
			data = val;

		iowrite32(data, (void*)addr);
	}
	else if (bus->number == pci1_bus_nr + 1)
	{
		if (slot != 0)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* PCIE devices directly connected */
		addr = BSP_PCIE1_D_CFG0 + (func << 12) + where;
		pci_write_size(size, addr, val);
	}
	else
	{
		if (slot >= MAX_NUM_DEV)
		{
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		/* Devices connected through bridge */
		iowrite32((bus->number << 8) | (slot << 3) | func, (void*)BSP_PCIE1_H_IPCFG);
		addr = BSP_PCIE1_D_CFG1 + where;
		pci_write_size(size, addr, val);
	}
	return PCIBIOS_SUCCESSFUL;
}

//========================================================================================

#define BSP_PCIE_FUN_OFS    0xC00000

#define BSP_PCIE0_D_IO      0xB8C00000
#define BSP_PCIE0_D_MEM     0xB9000000
//#define BSP_PCIE0_F1_D_MEM  (BSP_PCIE0_D_MEM + BSP_PCIE_FUN_OFS)
#define BSP_PCIE1_D_IO      0xB8E00000
#define BSP_PCIE1_D_MEM     0xBA000000
//#define BSP_PCIE1_F1_D_MEM  (BSP_PCIE1_D_MEM + BSP_PCIE_FUN_OFS)

#define PADDR(addr)  ((addr) & 0x1FFFFFFF)

static int rtl8198c_pci_probe(struct platform_device *pdev)
{
	// TODO: Add reset procedure

	static struct pci_ops pci_ops0 = {
		.read = pci0_config_read,
		.write = pci0_config_write,
	};
	static struct resource mem_resource0 = {
		.name = "PCIe Memory resources",
		.start = PADDR(BSP_PCIE0_D_MEM),
		.end = PADDR(BSP_PCIE0_D_MEM + 0xFFFFFF),
		.flags = IORESOURCE_MEM,
	};
	static struct resource io_resource0 = {
		.name = "PCIe I/O resources",
		.start = PADDR(BSP_PCIE0_D_IO),
		.end = PADDR(BSP_PCIE0_D_IO + 0x1FFFFF),
		.flags = IORESOURCE_IO,
	};
	static struct pci_controller controller0 = {
		.pci_ops = &pci_ops0,
		.mem_resource = &mem_resource0,
		.io_resource = &io_resource0,
	};
	register_pci_controller(&controller0);

	static struct pci_ops pci_ops1 = {
		.read = pci1_config_read,
		.write = pci1_config_write,
	};
	static struct resource mem_resource1 = {
		.name = "PCIe1 Memory resources",
		.start = PADDR(BSP_PCIE1_D_MEM),
		.end = PADDR(BSP_PCIE1_D_MEM + 0xFFFFFF),
		.flags = IORESOURCE_MEM,
	};
	static struct resource io_resource1 = {
		.name = "PCIe1 I/O resources",
		.start = PADDR(BSP_PCIE1_D_IO),
		.end = PADDR(BSP_PCIE1_D_IO + 0x1FFFFF),
		.flags = IORESOURCE_IO,
	};
	static struct pci_controller controller1 = {
		.pci_ops = &pci_ops1,
		.mem_resource = &mem_resource1,
		.io_resource = &io_resource1,
	};
	register_pci_controller(&controller1);

	return 0;
}

int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	return 0;
}

int pcibios_plat_dev_init(struct pci_dev *pdev)
{
	return 0;
}

static const struct of_device_id rtl8198c_pci_ids[] = {
	{ .compatible = "realtek,rtl8198c-pci" },
	{},
};

MODULE_DEVICE_TABLE(of, rtl8198c_pci_ids);

static struct platform_driver rtl8198c_pci_driver = {
	.probe = rtl8198c_pci_probe,
	.driver = {
		.name = "rtl8198c-pci",
		.of_match_table = of_match_ptr(rtl8198c_pci_ids),
	},
};

static int __init rtl8198c_pci_init(void)
{
	return platform_driver_register(&rtl8198c_pci_driver);
}

arch_initcall(rtl8198c_pci_init);
