/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL819X_IOREMAP_H_
#define RTL819X_IOREMAP_H_

static inline int is_rtl838x_internal_registers(phys_addr_t offset)
{
	/* IO-Block (KSEG1 addresses) */
	if (offset >= 0xb8000000 && offset < 0xb9000000)
		return 1;
	/* Switch block (KSEG1 addresses) */
	if (offset >= 0xbb000000 && offset < 0xbc000000)
		return 1;
	/* PCIe MEM windows (physical addresses) */
	if (offset >= 0x18000000 && offset < 0x1c000000)
		return 1;
	return 0;
}

static inline void __iomem *plat_ioremap(phys_addr_t offset, unsigned long size,
					 unsigned long flags)
{
	if (offset >= 0xb8000000 && offset < 0xb9000000)
		return (void __iomem *)offset;
	if (offset >= 0xbb000000 && offset < 0xbc000000)
		return (void __iomem *)offset;
	/* PCIe MEM windows: physical -> KSEG1 */
	if (offset >= 0x18000000 && offset < 0x1c000000)
		return (void __iomem *)(0xa0000000 + offset);
	return NULL;
}

static inline int plat_iounmap(const volatile void __iomem *addr)
{
	unsigned long va = (unsigned long)addr;

	if (va >= 0xb8000000 && va < 0xb9000000)
		return 1;
	if (va >= 0xbb000000 && va < 0xbc000000)
		return 1;
	if (va >= 0xb8000000 && va < 0xbc000000)
		return 1;
	return 0;
}

#endif
