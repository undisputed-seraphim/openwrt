// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Realtek RTL8198C
 *
 * Copyright (C) 2025 Tan Li Boon <undisputed.seraphim@gmail.com>
 */

#include "linux/printk.h"
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
#ifndef REG16
#define REG16(reg) (*(volatile unsigned short*)(reg))
#endif
#ifndef REG08
#define REG08(reg) (*(volatile unsigned char*)(reg))
#endif

#define WRITE_MEM32(addr, val) (*(volatile unsigned int*)(addr)) = (val)
#define READ_MEM32(addr) (*(volatile unsigned int*)(addr))
#define WRITE_MEM16(addr, val) (*(volatile unsigned short*)(addr)) = (val)
#define READ_MEM16(addr) (*(volatile unsigned short*)(addr))
#define WRITE_MEM8(addr, val) (*(volatile unsigned char*)(addr)) = (val)
#define READ_MEM8(addr) (*(volatile unsigned char*)(addr))

#define PADDR(addr) ((addr)&0x1FFFFFFF)

/*
 * IRQ Controller
 */
#define BSP_IRQ_CPU_BASE 0
#define BSP_IRQ_CPU_NUM 8

#define BSP_IRQ_GIC_BASE (BSP_IRQ_CPU_BASE + BSP_IRQ_CPU_NUM) // 0+8=8

#define BSP_PCIE_IRQ (BSP_IRQ_GIC_BASE + 31)
#define BSP_PCIE2_IRQ (BSP_IRQ_GIC_BASE + 32)

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
#define PCIE0_RC_EXT_BASE (0xb8b01000)
#define PCIE1_RC_EXT_BASE (0xb8b21000)
// RC Extended register
#define PCIE0_MDIO (PCIE0_RC_EXT_BASE + 0x00)
#define PCIE1_MDIO (PCIE1_RC_EXT_BASE + 0x00)
// MDIO
#define PCIE_MDIO_DATA_OFFSET (16)
#define PCIE_MDIO_DATA_MASK (0xffff << PCIE_MDIO_DATA_OFFSET)
#define PCIE_MDIO_REG_OFFSET (8)
#define PCIE_MDIO_RDWR_OFFSET (0)

#define CLK_MANAGE 0xb8000010
#define GPIO_BASE 0xB8003500
#define PEFGHCNR_REG (0x01C + GPIO_BASE) /* Port EFGH control */
#define PEFGHPTYPE_REG (0x020 + GPIO_BASE) /* Port EFGH type */
#define PEFGHDIR_REG (0x024 + GPIO_BASE) /* Port EFGH direction */
#define PEFGHDAT_REG (0x028 + GPIO_BASE) /* Port EFGH data */

void PCIE_MDIO_Reset(unsigned int portnum)
{
#define SYS_PCIE_PHY0 (0xb8000000 + 0x50)
#define SYS_PCIE_PHY1 (0xb8000000 + 0x54)

	unsigned int sys_pcie_phy;

	if (portnum == 0)
		sys_pcie_phy = SYS_PCIE_PHY0;
	else if (portnum == 1)
		sys_pcie_phy = SYS_PCIE_PHY1;
	else
		return;

	// 3.MDIO Reset
	REG32(sys_pcie_phy) = (1 << 3) | (0 << 1) | (0 << 0); // mdio reset=0,
	REG32(sys_pcie_phy) = (1 << 3) | (0 << 1) | (1 << 0); // mdio reset=1,
	REG32(sys_pcie_phy) = (1 << 3) | (1 << 1) | (1 << 0); // bit1 load_done=1
}

void HostPCIe_SetPhyMdioWrite(unsigned int portnum, unsigned int regaddr, unsigned short val)
{
	unsigned int mdioaddr;
	volatile int i;
	if (portnum == 0)
		mdioaddr = PCIE0_MDIO;
	else if (portnum == 1)
		mdioaddr = PCIE1_MDIO;
	else
		return;

	REG32(mdioaddr) = ((regaddr & 0x1f) << PCIE_MDIO_REG_OFFSET) | ((val & 0xffff) << PCIE_MDIO_DATA_OFFSET) | (1 << PCIE_MDIO_RDWR_OFFSET);
	// delay

	for (i = 0; i < 5555; i++)
		;

	mdelay(1);
}

static void PCIE_Device_PERST(int portnum)
{

	if (portnum == 0) {
		REG32(CLK_MANAGE) &= ~(1 << 26); // perst=0 off.
		mdelay(500); // PCIE standadrd: poweron: 100us, after poweron: 100ms
		mdelay(500);
		REG32(CLK_MANAGE) |= (1 << 26); // PERST=1
	} else if (portnum == 1) {
		/*	PCIE Device Reset
		 *	The pcei1 slot reset register depends on the hw
		 */
//#if defined(CONFIG_RTL_8198C)
		printk("Port 1 DevRESET\r\n");
		REG32(0xb800010c) = (REG32(0xb800010c) & (~(7 << 10))) | (3 << 10);
		mdelay(500);

		REG32(PEFGHCNR_REG) &= ~(0x20000); /*port F bit 4 */
		REG32(PEFGHDIR_REG) |= (0x20000); /*port F bit 4 */

		REG32(PEFGHDAT_REG) &= ~(20000);
		mdelay(500);
		mdelay(500);
		REG32(PEFGHDAT_REG) |= (0x20000); // PERST=1
//#endif
	} else
		return;
}

void PCIE_PHY_Reset(unsigned int portnum)
{
#define PCIE_PHY0 0xb8b01008
#define PCIE_PHY1 0xb8b21008

	unsigned int pcie_phy;

	if (portnum == 0)
		pcie_phy = BSP_PCIE0_H_PWRCR;
	else if (portnum == 1)
		pcie_phy = BSP_PCIE1_H_PWRCR;
	else
		return;

	// PCIE PHY Reset
	REG32(pcie_phy) = 0x01; // bit7:PHY reset=0   bit0: Enable LTSSM=1
	REG32(pcie_phy) = 0x81; // bit7: PHY reset=1   bit0: Enable LTSSM=1
}

int PCIE_Check_Link(unsigned int portnum)
{
	printk("rtl8198c pcie: check link %d", portnum);
	unsigned int dbgaddr;
	unsigned int cfgaddr = 0;
	int i = 10;

	if (portnum == 0)
		dbgaddr = 0xb8b00728;
	else if (portnum == 1)
		dbgaddr = 0xb8b20728;
	else
		return 1;

	// wait for LinkUP

	while (--i) {
		if ((REG32(dbgaddr) & 0x1f) == 0x11)
			break;

		mdelay(100);
	}

	if (i == 0) {
		printk("rtl8198c pcie: i=%x  Cannot LinkUP \n", i);
		return 0;
	} else {
		if (portnum == 0)
			cfgaddr = 0xb8b10000;
		else if (portnum == 1)
			cfgaddr = 0xb8b30000;

		printk("rtl8198c pcie: Find Port=%x Device:Vender ID=%x\n", portnum, REG32(cfgaddr));
	}
	return 1;
}

int PCIE_reset_procedure(int portnum, int Use_External_PCIE_CLK, int mdio_reset)
{
	int result = 0;
	printk("rtl8198c pcie: port = %d \n", portnum);

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
		printk("rtl8198c pcie: Do MDIO_RESET\n");
		// 3.MDIO Reset
		PCIE_MDIO_Reset(portnum);
	}

	mdelay(500);
	mdelay(500);

	if (mdio_reset) {
		// fix 8198 test chip pcie tx problem.
		//#if defined(CONFIG_RTL_8198C)
		REG32(0xb8000104) = (REG32(0xb8000104) & (~0x3 << 20)) | (1 << 20); // PCIe MUX switch to PCIe reset

		{
			int phy40M;
			phy40M = (REG32(0xb8000008) & (1 << 24)) >> 24;
			printk("UPHY: 8198c ASIC u2 of u3 %s phy patch\n", (phy40M == 1) ? "40M" : "25M");
			if (phy40M) {
				mdelay(500);
				HostPCIe_SetPhyMdioWrite(portnum, 0x3, 0x7b31);
				HostPCIe_SetPhyMdioWrite(portnum, 0x6, 0xe258); // e2b8
				HostPCIe_SetPhyMdioWrite(portnum, 0xF, 0x400F);
				HostPCIe_SetPhyMdioWrite(portnum, 0xd, 0x1764); // e2b8
				HostPCIe_SetPhyMdioWrite(portnum, 0x19, 0xFC70);

				printk("\r\n40MHz PCIe Parameters\r\n");

			} else {
				HostPCIe_SetPhyMdioWrite(portnum, 0x3, 0x3031);
				HostPCIe_SetPhyMdioWrite(portnum, 0x6, 0xe058); // Hannah
				HostPCIe_SetPhyMdioWrite(portnum, 0xF, 0x400F);
				HostPCIe_SetPhyMdioWrite(portnum, 0x19, 0xFC70);
				printk("\r\n25MHz PCIe Parameters\r\n");
			}
		}
		//#endif
	}
	//---------------------------------------

	PCIE_Device_PERST(portnum);

	PCIE_PHY_Reset(portnum);
	mdelay(500);
	mdelay(500);
	result = PCIE_Check_Link(portnum);

	return result;
}
// EXPORT_SYMBOL(PCIE_reset_procedure);

//-- pci-ops.c

#define PCI_8BIT_ACCESS 1
#define PCI_16BIT_ACCESS 2
#define PCI_32BIT_ACCESS 4
#define PCI_ACCESS_READ 8
#define PCI_ACCESS_WRITE 16
#define MAX_NUM_DEV 4

static int pci0_bus_number = 0xff;
static int pci1_bus_number = 0xff;

static int rtl819x_pcibios_config_access(unsigned char access_type,
	unsigned int addr, unsigned int* data)
{
	/* Do 8bit/16bit/32bit access */
	if (access_type & PCI_ACCESS_WRITE) {
		if (access_type & PCI_8BIT_ACCESS)
			WRITE_MEM8(addr, *data);
		else if (access_type & PCI_16BIT_ACCESS)
			WRITE_MEM16(addr, *data);
		else
			WRITE_MEM32(addr, *data);
	} else if (access_type & PCI_ACCESS_READ) {
		if (access_type & PCI_8BIT_ACCESS) {
#ifdef CONFIG_RTL8198_REVISION_B
			unsigned int data_temp = 0;
			int swap[4] = { 0, 8, 16, 24 };
			int diff = addr & 0x3;
			data_temp = READ_MEM32(addr);
			*data = (unsigned int)((data_temp >> swap[diff]) & 0xff);

#else
			*data = READ_MEM8(addr);
#endif
		} else if (access_type & PCI_16BIT_ACCESS) {
#ifdef CONFIG_RTL8198_REVISION_B
			unsigned int data_temp = 0;
			int swap[4] = { 0, 8, 16, 24 };
			int diff = addr & 0x3;
			data_temp = READ_MEM32(addr);
			*data = (unsigned int)((data_temp >> swap[diff]) & 0xffff);

#else
			*data = READ_MEM16(addr);
#endif
		} else
			*data = READ_MEM32(addr);
	}

	/* If need to check for PCIE access timeout, put code here */
	/* ... */

	return 0;
}

static int rtl819x_pcibios0_read(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int* val)
{
	unsigned int data = 0;
	unsigned int addr = 0;

	if (pci0_bus_number == 0xff)
		pci0_bus_number = bus->number;

	printk("rtl8198c pcie: File: %s, Function: %s, Line: %d\n", __FILE__, __FUNCTION__, __LINE__);
	printk("rtl8198c pcie: rtl819x_pcibios0_read Bus: %d, Slot: %d, Func: %d, Where: %d, Size: %d\n", bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, size);

	if (bus->number == pci0_bus_number) {
		/* PCIE host controller */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE0_H_CFG + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | PCI_32BIT_ACCESS, addr & ~(0x3), &data))
				return PCIBIOS_DEVICE_NOT_FOUND;

			if (size == 1)
				*val = (data >> ((where & 3) << 3)) & 0xff;
			else if (size == 2)
				*val = (data >> ((where & 3) << 3)) & 0xffff;
			else
				*val = data;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci0_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE0_D_CFG0 + (PCI_FUNC(devfn) << 12) + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | size, addr, val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (PCI_SLOT(devfn) < MAX_NUM_DEV) {
			WRITE_MEM32(BSP_PCIE0_H_IPCFG, ((bus->number) << 8) | (PCI_SLOT(devfn) << 3) | PCI_FUNC(devfn));
			addr = BSP_PCIE0_D_CFG1 + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | size, addr, val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}
#if 0
   REG32(0xb8000014)=0x800200; //mark_pci
#if DEBUG_PRINTK
   printk("0xb8000014:%x\n",REG32(0xb8000014));
	//printk("File: %s, Function: %s, Line: %d\n", __FILE__, __FUNCTION__, __LINE__);
   //printk("Read Value: 0x%08X\n", *val);
#endif
   printk("PCI_EP_READ: addr= %x, size=%d, value= %x\n", addr, size, *val);
#endif
	return PCIBIOS_SUCCESSFUL;
}

//========================================================================================
static int rtl819x_pcibios0_write(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int val)
{
	unsigned int data = 0;
	unsigned int addr = 0;

	static int pci0_bus_number = 0xff;
	if (pci0_bus_number == 0xff)
		pci0_bus_number = bus->number;

	printk("rtl8198c pcie: File: %s, Function: %s, Line: %d\n", __FILE__, __FUNCTION__, __LINE__);
	printk("rtl8198c pcie: Bus: %d, Slot: %d, Func: %d, Where: %d, Size: %d\n", bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, size);

	if (bus->number == pci0_bus_number) {
		/* PCIE host controller */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE0_H_CFG + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | PCI_32BIT_ACCESS, addr & ~(0x3), &data))
				return PCIBIOS_DEVICE_NOT_FOUND;

			if (size == 1)
				data = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
			else if (size == 2)
				data = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
			else
				data = val;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_WRITE | PCI_32BIT_ACCESS, addr & ~(0x3), &data))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci0_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE0_D_CFG0 + (PCI_FUNC(devfn) << 12) + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_WRITE | size, addr, &val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (PCI_SLOT(devfn) < MAX_NUM_DEV) {
			WRITE_MEM32(BSP_PCIE0_H_IPCFG, ((bus->number) << 8) | (PCI_SLOT(devfn) << 3) | PCI_FUNC(devfn));
			addr = BSP_PCIE0_D_CFG1 + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_WRITE | size, addr, &val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int rtl819x_pcibios1_read(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int* val)
{
	unsigned int data = 0;
	unsigned int addr = 0;

	if (pci1_bus_number == 0xff)
		pci1_bus_number = bus->number;

	printk("rtl8198c pcie: File: %s, Function: %s, Line: %d\n", __FILE__, __FUNCTION__, __LINE__);
	printk("rtl8198c pcie: rtl819x_pcibios1_read Bus: %d, Slot: %d, Func: %d, Where: %d, Size: %d\n", bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, size);

	if (bus->number == pci1_bus_number) {
		/* PCIE host controller */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE1_H_CFG + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | PCI_32BIT_ACCESS, addr & ~(0x3), &data))
				return PCIBIOS_DEVICE_NOT_FOUND;

			if (size == 1)
				*val = (data >> ((where & 3) << 3)) & 0xff;
			else if (size == 2)
				*val = (data >> ((where & 3) << 3)) & 0xffff;
			else
				*val = data;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci1_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE1_D_CFG0 + (PCI_FUNC(devfn) << 12) + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | size, addr, val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (PCI_SLOT(devfn) < MAX_NUM_DEV) {
			WRITE_MEM32(BSP_PCIE1_H_IPCFG, ((bus->number) << 8) | (PCI_SLOT(devfn) << 3) | PCI_FUNC(devfn));
			addr = BSP_PCIE1_D_CFG1 + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | size, addr, val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}
#if 0
   REG32(0xb8000014)=0x800200; //mark_pci
#if DEBUG_PRINTK
   printk("0xb8000014:%x\n",REG32(0xb8000014));
	//printk("File: %s, Function: %s, Line: %d\n", __FILE__, __FUNCTION__, __LINE__);
   //printk("Read Value: 0x%08X\n", *val);
#endif
   printk("PCI_EP_READ: addr= %x, size=%d, value= %x\n", addr, size, *val);
#endif
	return PCIBIOS_SUCCESSFUL;
}

//========================================================================================
static int rtl819x_pcibios1_write(struct pci_bus* bus, unsigned int devfn,
	int where, int size, unsigned int val)
{
	unsigned int data = 0;
	unsigned int addr = 0;

	if (pci1_bus_number == 0xff)
		pci1_bus_number = bus->number;

	printk("rtl8198c pcie: File: %s, Function: %s, Line: %d\n", __FILE__, __FUNCTION__, __LINE__);
	printk("rtl8198c pcie: Bus: %d, Slot: %d, Func: %d, Where: %d, Size: %d\n", bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, size);

	if (bus->number == pci1_bus_number) {
		/* PCIE host controller */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE1_H_CFG + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_READ | PCI_32BIT_ACCESS, addr & ~(0x3), &data))
				return PCIBIOS_DEVICE_NOT_FOUND;

			if (size == 1)
				data = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
			else if (size == 2)
				data = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
			else
				data = val;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_WRITE | PCI_32BIT_ACCESS, addr & ~(0x3), &data))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else if (bus->number == (pci1_bus_number + 1)) {
		/* PCIE devices directly connected */
		if (PCI_SLOT(devfn) == 0) {
			addr = BSP_PCIE1_D_CFG0 + (PCI_FUNC(devfn) << 12) + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_WRITE | size, addr, &val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	} else {
		/* Devices connected through bridge */
		if (PCI_SLOT(devfn) < MAX_NUM_DEV) {
			WRITE_MEM32(BSP_PCIE1_H_IPCFG, ((bus->number) << 8) | (PCI_SLOT(devfn) << 3) | PCI_FUNC(devfn));
			addr = BSP_PCIE1_D_CFG1 + where;

			if (rtl819x_pcibios_config_access(PCI_ACCESS_WRITE | size, addr, &val))
				return PCIBIOS_DEVICE_NOT_FOUND;
		} else
			return PCIBIOS_DEVICE_NOT_FOUND;
	}

	return PCIBIOS_SUCCESSFUL;
}

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
#if 0
	unsigned int val;

	val = REG32(0xbb004000);
	if (val != 0x3)
		return 0;

	val = REG32(BSP_PCIE_EP_CFG+0x78);
	printk("INFO: Address %lx = 0x%x\n", BSP_PCIE_EP_CFG + 0x78, val);

	REG32(BSP_PCIE_EP_CFG + 0x78) = 0x105030;
	printk("INFO: Set PCIE payload to 128\n");

	val = REG32(BSP_PCIE_EP_CFG+0x78);
	printk("INFO: Address %lx = 0x%x\n",BSP_PCIE_EP_CFG + 0x78, val);
#endif
	printk("rtl8198c pcie: pcibios_plat_dev_init INFO: bus=%d \n", dev->bus->number);
	return 0;
}

int pcibios_map_irq(const struct pci_dev* dev, u8 slot, u8 pin)
{
	int irq;

	if (dev->bus->number < pci1_bus_number)
		irq = BSP_PCIE_IRQ;
	else
		irq = BSP_PCIE2_IRQ;

	printk("rtl8198c pcie: map_irq: bus=%d ,slot=%d, pin=%d  irq=%d\n", dev->bus->number, slot, pin, irq);
	return irq;
}

//-- pci.c

static struct resource pcie_mem_resource = {
	.name = "PCIe Memory resources",
	.start = PADDR(BSP_PCIE0_D_MEM),
	.end = PADDR(BSP_PCIE0_D_MEM + 0xFFFFFF),
	.flags = IORESOURCE_MEM,
};

static struct resource pcie_io_resource = {
	.name = "PCIe I/O resources",
	.start = PADDR(BSP_PCIE0_D_IO),
	.end = PADDR(BSP_PCIE0_D_IO + 0x1FFFFF),
	.flags = IORESOURCE_IO,
};

static struct pci_controller bsp_pcie_controller = {
	.pci_ops = &bsp_pcie_ops,
	.mem_resource = &pcie_mem_resource,
	.io_resource = &pcie_io_resource,
};

static struct resource pcie_mem_resource1 = {
	.name = "PCIe1 Memory resources",
	.start = PADDR(BSP_PCIE1_D_MEM),
	.end = PADDR(BSP_PCIE1_D_MEM + 0xFFFFFF),
	.flags = IORESOURCE_MEM,
};

static struct resource pcie_io_resource1 = {
	.name = "PCIe1 I/O resources",
	.start = PADDR(BSP_PCIE1_D_IO),
	.end = PADDR(BSP_PCIE1_D_IO + 0x1FFFFF),
	.flags = IORESOURCE_IO,
};

static struct pci_controller bsp_pcie_controller1 = {
	.pci_ops = &bsp_pcie_ops1,
	.mem_resource = &pcie_mem_resource1,
	.io_resource = &pcie_io_resource1,
};

static int __init bsp_pcie_init(void)
{
	int pci_exist0 = 0;
	int pci_exist1 = 0;

	if (PCIE_reset_procedure(0, 0, 1)) // (port,externalClk,mdio_reset)
		pci_exist0 = 1;

	if (PCIE_reset_procedure(1, 0, 1)) // (port,externalClk,mdio_reset)
		pci_exist1 = 1;

	if (pci_exist0) {
		printk("rtl8198c pcie: registering 0");
		register_pci_controller(&bsp_pcie_controller);
	}

	if (pci_exist1) {
		printk("rtl8198c pcie: registering 1");
		register_pci_controller(&bsp_pcie_controller1);
	}

	return 0;
}
arch_initcall(bsp_pcie_init);