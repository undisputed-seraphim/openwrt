// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Realtek RTL8198C
 *
 * Copyright (C) 2025 Tan Li Boon <undisputed.seraphim@gmail.com>
 */

#include "linux/ioport.h"
#include "linux/mod_devicetable.h"
#include "uapi/linux/pci.h"
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include <asm-generic/iomap.h>
#include <asm/delay.h>
#include <asm/pci.h>

//-- bspchip.h

/*
 * Register access macro
 */
#ifndef REG32
#define REG32(reg) (*(volatile unsigned int*)(reg))
#endif

/*
 * PCI-E
 */
#define BSP_PCIE0_H_CFG 0xB8B00000
#define BSP_PCIE0_H_EXT 0xB8B01000
#define BSP_PCIE0_H_MDIO (BSP_PCIE0_H_EXT + 0x00)
#define BSP_PCIE0_H_INTSTR (BSP_PCIE0_H_EXT + 0x04)
#define BSP_PCIE0_H_PWRCR (BSP_PCIE0_H_EXT + 0x08)
#define BSP_PCIE0_H_IPCFG (BSP_PCIE0_H_EXT + 0x0C)
#define BSP_PCIE0_H_MISC (BSP_PCIE0_H_EXT + 0x10)
#define BSP_PCIE0_D_CFG0 0xB8B10000
#define BSP_PCIE0_D_CFG1 0xB8B11000
#define BSP_PCIE0_D_DBG 0xB8B00728
#define BSP_PCIE0_D_MSG 0xB8B12000

#define BSP_PCIE1_H_CFG 0xB8B20000
#define BSP_PCIE1_H_EXT 0xB8B21000
#define BSP_PCIE1_H_MDIO (BSP_PCIE1_H_EXT + 0x00)
#define BSP_PCIE1_H_INTSTR (BSP_PCIE1_H_EXT + 0x04)
#define BSP_PCIE1_H_PWRCR (BSP_PCIE1_H_EXT + 0x08)
#define BSP_PCIE1_H_IPCFG (BSP_PCIE1_H_EXT + 0x0C)
#define BSP_PCIE1_H_MISC (BSP_PCIE1_H_EXT + 0x10)
#define BSP_PCIE1_D_CFG0 0xB8B30000
#define BSP_PCIE1_D_CFG1 0xB8B31000
#define BSP_PCI11_D_DBG 0xB8B20728
#define BSP_PCIE1_D_MSG 0xB8B32000

#define BSP_PCIE0_D_IO 0xB8C00000
#define BSP_PCIE1_D_IO 0xB8E00000
#define BSP_PCIE_FUN_OFS 0xC00000
#define BSP_PCIE0_D_MEM 0xB9000000
#define BSP_PCIE0_F1_D_MEM (BSP_PCIE0_D_MEM + BSP_PCIE_FUN_OFS)
#define BSP_PCIE1_D_MEM 0xBA000000
#define BSP_PCIE1_F1_D_MEM (BSP_PCIE1_D_MEM + BSP_PCIE_FUN_OFS)

//-- pci-fixup.c

// -----------------------rtk PCI init part

#define CLK_MANAGE 0xb8000010
#define GPIO_BASE 0xB8003500
#define PEFGHCNR_REG (0x01C + GPIO_BASE) /* Port EFGH control */
#define PEFGHPTYPE_REG (0x020 + GPIO_BASE) /* Port EFGH type */
#define PEFGHDIR_REG (0x024 + GPIO_BASE) /* Port EFGH direction */
#define PEFGHDAT_REG (0x028 + GPIO_BASE) /* Port EFGH data */

static void PCIE_MDIO_Reset(uintptr_t sys_pcie_phy)
{
	iowrite32(0x8 | 0x0 | 0x0, (void*)sys_pcie_phy); // mdio reset=0,
	iowrite32(0x8 | 0x0 | 0x1, (void*)sys_pcie_phy); // mdio reset=1,
	iowrite32(0x8 | 0x2 | 0x1, (void*)sys_pcie_phy); // bit1 load_done=1
}

static void HostPCIe_SetPhyMdioWrite(uintptr_t mdioaddr, unsigned int regaddr, unsigned short val)
{
#define PCIE_MDIO_DATA_OFFSET (16)
#define PCIE_MDIO_DATA_MASK (0xffff << PCIE_MDIO_DATA_OFFSET)
#define PCIE_MDIO_REG_OFFSET (8)
#define PCIE_MDIO_RDWR_OFFSET (0)
	iowrite32(((regaddr & 0x1f) << PCIE_MDIO_REG_OFFSET) | ((val & 0xffff) << PCIE_MDIO_DATA_OFFSET) | (1 << PCIE_MDIO_RDWR_OFFSET), (void*)mdioaddr);
	mdelay(1);
}

static void rtl8198_pcie0_device_perst(void)
{
	REG32(CLK_MANAGE) &= ~(1 << 26); // perst=0 off.
	mdelay(1000); // PCIE standadrd: poweron: 100us, after poweron: 100ms
	REG32(CLK_MANAGE) |= (1 << 26); // PERST=1
}

static void rtl8198_pcie1_device_perst(void)
{
//#if defined(CONFIG_RTL8198C)
	/* PCIE Device Reset
	 * The pcie1 slot reset register depends on the hw
	 */
	REG32(0xb800010c) = (REG32(0xb800010c) & (~(7 << 10))) | (3 << 10);
	mdelay(500);

	REG32(PEFGHCNR_REG) &= ~(0x20000); /*port F bit 4 */
	REG32(PEFGHDIR_REG) |=  (0x20000); /*port F bit 4 */

	REG32(PEFGHDAT_REG) &= ~(20000);
	mdelay(1000);
	REG32(PEFGHDAT_REG) |= (0x20000); // PERST=1
//#endif
}

static void PCIE_PHY_Reset(uintptr_t pcie_phy)
{
	iowrite32(0x01, (void*)pcie_phy); // bit7: PHY reset=0 bit0: Enable LTSSM=1
	iowrite32(0x81, (void*)pcie_phy); // bit7: PHY reset=1 bit0: Enable LTSSM=1
}

static int PCIE_Check_Link(uintptr_t cfgaddr, uintptr_t dbgaddr)
{
	int i = 10;

	// wait for LinkUP
	while (--i) {
		if ((ioread32((const void*)dbgaddr) & 0x1f) == 0x11)
			break;

		mdelay(100);
	}

	if (i == 0) {
		return 0;
	}
	pr_info("rtl8198c pcie: Device:Vendor ID=%x\n", ioread32((const void*)cfgaddr));
	return 1;
}

static int PCIE_reset_procedure(int portnum, int mdio_reset)
{
	pr_info("rtl8198c pcie: port = %d \n", portnum);

	//#ifdef CONFIG_RTL8198C
	REG32(0xb8000010) = REG32(0xb8000010) | (1 << 12 | 1 << 13 | 1 << 14 | 1 << 16 | 1 << 19 | 1 << 20); //(1<<14);
	//#endif

	if (portnum == 0)
		REG32(CLK_MANAGE) |= (1 << 14); // enable active_pcie0
	else if (portnum == 1)
		REG32(CLK_MANAGE) |= (1 << 16); // enable active_pcie1
	else
		return 0;

	mdelay(500);

	if (mdio_reset) {
		// 3.MDIO Reset
#define SYS_PCIE_PHY0 (0xb8000000 + 0x50)
#define SYS_PCIE_PHY1 (0xb8000000 + 0x54)
		PCIE_MDIO_Reset((portnum == 0) ? SYS_PCIE_PHY0 : SYS_PCIE_PHY1);
	}

	mdelay(1000);

	if (mdio_reset) {
		// fix 8198 test chip pcie tx problem.
		//#if defined(CONFIG_RTL8198C)
		REG32(0xb8000104) = (REG32(0xb8000104) & (~0x3 << 20)) | (1 << 20); // PCIe MUX switch to PCIe reset

		{
			const uintptr_t mdioaddr = (portnum == 0) ? BSP_PCIE0_H_MDIO : BSP_PCIE1_H_MDIO;
			const int phy40M = (ioread32((const void*)0xb8000008) & (1 << 24)) >> 24;
			pr_debug("UPHY: 8198c ASIC u2 of u3 %s phy patch\n", (phy40M == 1) ? "40M" : "25M");
			if (phy40M) {
				mdelay(500);
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0x3, 0x7b31);
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0x6, 0xe258); // e2b8
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0xF, 0x400F);
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0xd, 0x1764); // e2b8
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0x19, 0xFC70);
			} else {
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0x3, 0x3031);
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0x6, 0xe058); // Hannah
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0xF, 0x400F);
				HostPCIe_SetPhyMdioWrite(mdioaddr, 0x19, 0xFC70);
			}
		}
		//#endif
	}
	//---------------------------------------

	if (portnum == 0)
		rtl8198_pcie0_device_perst();
	else
		rtl8198_pcie1_device_perst();

	PCIE_PHY_Reset((portnum == 0) ? BSP_PCIE0_H_PWRCR : BSP_PCIE1_H_PWRCR);
	mdelay(1000);
	pr_info("rtl8198c pcie: check link %d", portnum);
	int result = (portnum == 0) ? PCIE_Check_Link(BSP_PCIE0_D_CFG0, 0xb8b00728) : PCIE_Check_Link(BSP_PCIE1_D_CFG0, 0xb8b20728);
	if (result == 0)
	{
		pr_info("rtl8198c pcie: port %d Cannot LinkUP", portnum);
	}

	return result;
}

//-- pci-ops.c

#define MAX_NUM_DEV 4

static int pci0_bus_number = 0xff;
static int pci1_bus_number = 0xff;

static unsigned int sized_ioread(unsigned char size, uintptr_t addr)
{
	unsigned int data;
	const void* const reg = (const void*)addr;
	switch (size) {
	case 1:
		data = ioread8(reg);
		break;
	case 2:
		data = ioread16(reg);
		break;
	default:
		data = ioread32(reg);
		break;
	}
	return data;
}
static void sized_iowrite(unsigned char size, uintptr_t addr, unsigned int data)
{
	void* const reg = (void*)addr;
	switch (size) {
	case 1:
		iowrite8(data, reg);
		break;
	case 2:
		iowrite16(data, reg);
		break;
	default:
		iowrite32(data, reg);
		break;
	}
}

//========================================================================================
static int rtl819x_pcibios0_read(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int* val)
{
	const int func = PCI_FUNC(devfn);
	const int slot = PCI_SLOT(devfn);

	unsigned int data = 0;
	uintptr_t addr = 0;

	if (pci0_bus_number == 0xff)
		pci0_bus_number = bus->number;

	if (bus->number == pci0_bus_number) {
		/* PCIE host controller */
		if (slot == 0) {
			addr = (BSP_PCIE0_H_CFG + where) & ~0x3;
			data = ioread32((void*)addr);

			switch (size) {
			case 1:
				*val = (data >> ((where & 3) << 3)) & 0xff;
				break;
			case 2:
				*val = (data >> ((where & 3) << 3)) & 0xffff;
				break;
			default:
				*val = data;
			}
		} else {
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
	} else if (bus->number == (pci0_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (slot == 0) {
			addr = BSP_PCIE0_D_CFG0 + (func << 12) + where;
			*val = sized_ioread(size, addr);
		} else {
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
	} else {
		/* Devices connected through bridge */
		if (slot < MAX_NUM_DEV) {
			addr = BSP_PCIE0_H_IPCFG;
			data = ((bus->number) << 8) | (slot << 3) | func;
			iowrite32(data, (void*)addr);

			addr = BSP_PCIE0_D_CFG1 + where;
			*val = sized_ioread(size, addr);
		} else {
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
	}
	return PCIBIOS_SUCCESSFUL;
}

static int rtl819x_pcibios0_write(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int val)
{
	const int func = PCI_FUNC(devfn);
	const int slot = PCI_SLOT(devfn);

	unsigned int data = 0;
	uintptr_t addr = 0;

	if (pci0_bus_number == 0xff)
		pci0_bus_number = bus->number;

	if (bus->number == pci0_bus_number) {
		/* PCIE host controller */
		if (slot == 0) {
			addr = (BSP_PCIE0_H_CFG + where) & ~0x3;
			data = ioread32((void*)addr);

			switch (size) {
			case 1:
				data = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
				break;
			case 2:
				data = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
				break;
			default:
				data = val;
			}

			iowrite32(data, (void*)addr);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci0_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (slot == 0) {
			addr = BSP_PCIE0_D_CFG0 + (func << 12) + where;
			sized_iowrite(size, addr, val);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (slot < MAX_NUM_DEV) {
			addr = BSP_PCIE0_H_IPCFG;
			data = ((bus->number) << 8) | (slot << 3) | func;
			iowrite32(data, (void*)addr);

			addr = BSP_PCIE0_D_CFG1 + where;
			sized_iowrite(size, addr, val);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}

	return PCIBIOS_SUCCESSFUL;
}

//========================================================================================
static int rtl819x_pcibios1_read(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int* val)
{
	uintptr_t addr = 0;
	unsigned int data = 0;

	const int func = PCI_FUNC(devfn);
	const int slot = PCI_SLOT(devfn);

	if (pci1_bus_number == 0xff)
		pci1_bus_number = bus->number;

	if (bus->number == pci1_bus_number) {
		/* PCIE host controller */
		if (slot == 0) {
			addr = (BSP_PCIE1_H_CFG + where) & ~0x3;
			data = ioread32((void*)addr);

			switch (size) {
			case 1:
				*val = (data >> ((where & 3) << 3)) & 0xff;
				break;
			case 2:
				*val = (data >> ((where & 3) << 3)) & 0xffff;
				break;
			default:
				*val = data;
			}
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci1_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (slot == 0) {
			addr = BSP_PCIE1_D_CFG0 + (func << 12) + where;
			*val = sized_ioread(size, addr);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (slot < MAX_NUM_DEV) {
			addr = BSP_PCIE1_H_IPCFG;
			data = ((bus->number) << 8) | (slot << 3) | func;
			iowrite32(data, (void*)addr);

			addr = BSP_PCIE1_D_CFG1 + where;
			*val = sized_ioread(size, addr);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}
	return PCIBIOS_SUCCESSFUL;
}

static int rtl819x_pcibios1_write(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int val)
{
	uintptr_t addr = 0;
	unsigned int data = 0;

	const int func = PCI_FUNC(devfn);
	const int slot = PCI_SLOT(devfn);

	if (pci1_bus_number == 0xff)
		pci1_bus_number = bus->number;

	if (bus->number == pci1_bus_number) {
		/* PCIE host controller */
		if (slot == 0) {
			addr = (BSP_PCIE1_H_CFG + where) & ~0x3;
			data = ioread32((void*)addr);

			switch (size) {
			case 1:
				data = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
				break;
			case 2:
				data = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
				break;
			default:
				data = val;
			}

			iowrite32(data, (void*)addr);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci1_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (slot == 0) {
			addr = BSP_PCIE1_D_CFG0 + (func << 12) + where;
			sized_iowrite(size, addr, val);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (slot < MAX_NUM_DEV) {
			addr = BSP_PCIE1_H_IPCFG;
			data = ((bus->number) << 8) | (slot << 3) | func;
			iowrite32(data, (void*)addr);

			addr = BSP_PCIE1_D_CFG1 + where;
			sized_iowrite(size, addr, val);
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}

	return PCIBIOS_SUCCESSFUL;
}
//========================================================================================

struct pci_ops bsp_pcie_ops = {
	.read = rtl819x_pcibios0_read,
	.write = rtl819x_pcibios0_write,
};

struct pci_ops bsp_pcie_ops1 = {
	.read = rtl819x_pcibios1_read,
	.write = rtl819x_pcibios1_write,
};

/* Do platform specific device initialization at pci_enable_device() time */

int pcibios_plat_dev_init(struct pci_dev* dev)
{
	pr_info("rtl8198c pcie: pcibios_plat_dev_init INFO: bus=%d \n", dev->bus->number);
	return 0;
}

int pcibios_map_irq(const struct pci_dev* dev, u8 slot, u8 pin)
{
	int irq;
	if (dev->bus->number < pci1_bus_number)
	{
		// TODO: How to get this from the device tree??
		irq = 8 + 31;
	}
	else
	{
		irq = 8 + 32;
	}
	pr_info("INFO: map_irq: bus=%d ,slot=%d, pin=%d  irq=%d\n", dev->bus->number, slot, pin, irq);
	return irq;
}

static struct resource rtl8198c_pci_io_resource[2];
static struct resource rtl8198c_pci_mem_resource[2];
static struct pci_controller rtl8198c_pci_controller[2] = {
{
	.io_resource = &rtl8198c_pci_io_resource[0],
	.io_offset = 0,
	.mem_resource = &rtl8198c_pci_mem_resource[0],
	.mem_offset = 0,
	.pci_ops = &bsp_pcie_ops,
}, {
	.io_resource = &rtl8198c_pci_io_resource[1],
	.io_offset = 0,
	.mem_resource = &rtl8198c_pci_mem_resource[1],
	.mem_offset = 0,
	.pci_ops = &bsp_pcie_ops1,
}};

static int get_slot_nr(struct device_node* node)
{
	for (struct property* prop = node->properties; prop; prop = prop->next)
	{
		if (strncmp(prop->name, "slot", 4) == 0)
		{
			return *(int*)(prop->value);
		}
	}
	pr_err("No slot nr found for device.");
	return PCIBIOS_DEVICE_NOT_FOUND;
}

static int rtl8198c_pci_probe(struct platform_device* pdev)
{
	int slot = get_slot_nr(pdev->dev.of_node);

	PCIE_reset_procedure(slot, 1);

	pci_load_of_ranges(&rtl8198c_pci_controller[slot], pdev->dev.of_node);

	// TODO Not sure how to handle these but they need to be set.
	ioport_resource.start = 0x18C00000;
	ioport_resource.end   = 0x19000000;

	// TODO: init IRQ

	struct pci_controller *ctrl = &rtl8198c_pci_controller[slot];
	pr_info("Slot #%d io: 0x%08x - 0x%08x (offset %lu); mem: 0x%08x - 0x%08x (offset %lu)", slot,
		ctrl->io_resource->start, ctrl->io_resource->end, ctrl->io_offset,
		ctrl->mem_resource->start, ctrl->mem_resource->end, ctrl->mem_offset
	);

	register_pci_controller(&rtl8198c_pci_controller[slot]);
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