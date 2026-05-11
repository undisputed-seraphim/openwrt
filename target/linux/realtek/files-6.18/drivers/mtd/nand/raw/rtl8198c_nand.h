/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8198C_NAND_H
#define RTL8198C_NAND_H

#include <linux/io.h>

/* ── NAND flash controller base (KSEG1) ── */
#define NAND_BASE		0xB801A000

/* ── Register offsets (RTL8198C-specific layout) ── */
#define NACFR			0x00	/* flash configuration register */
#define NACR			0x04	/* control register */
#define NACMR			0x08	/* command register */
#define NAADR			0x0C	/* address register */
#define NADCRR			0x10	/* DMA / control register */
#define NADR			0x14	/* data register */
#define NADFSAR			0x18	/* DMA flash start address register */
#define NADFSAR2		0x1C	/* DMA flash start address register 2 (upper bits) */
#define NADRSAR			0x20	/* DMA RAM data start address register */
#define NADTSAR			0x24	/* DMA RAM tag (OOB) start address register */
#define NASR			0x28	/* status register */
#define NANDPReg		0x3C	/* page-size strap register */

/* ── NACR (control register, 0x04) ── */
#define NACR_READY		BIT(31)	/* controller ready / last-op-done */
#define NACR_ECC_EN		BIT(30)	/* enable hardware ECC */
#define NACR_RBO		BIT(29)	/* read byte-order swap enable */
#define NACR_WBO		BIT(28)	/* write byte-order swap enable */

#define NACR_DEFAULT		(NACR_READY | NACR_ECC_EN | 0x000FFFFF)
#define NACR_DMA_READ		(NACR_READY | NACR_ECC_EN | 0x000FFFFF)
#define NACR_DMA_WRITE		(NACR_READY | NACR_ECC_EN | 0x000FFFFF)

/* ── NACMR (command register, 0x08) ── */
#define NACMR_CECS1		BIT(31)
#define NACMR_CECS0		BIT(30)
#define NACMR_CMD_MASK		0x000000FF

/* ── NAADR (address register, 0x0C) ── */
#define NAADR_EN_NEXT		BIT(27)
#define NAADR_AD2EN		BIT(26)
#define NAADR_AD1EN		BIT(25)
#define NAADR_AD0EN		BIT(24)
#define NAADR_AD0_SHIFT		0
#define NAADR_AD1_SHIFT		8
#define NAADR_AD2_SHIFT		16
#define NAADR_ADDR_MASK		0x00FFFFFF

/* ── NADCRR (DMA / descriptor control register, 0x10) ── */
#define NADCRR_TAG_SEL		BIT(7)	/* tag select */
#define NADCRR_TAG_DIS		BIT(6)	/* tag disable */
#define NADCRR_DESC1		BIT(5)	/* descriptor mode 1 */
#define NADCRR_DESC0		BIT(4)	/* descriptor mode 0: sequential page read/write */
#define NADCRR_DMARE		BIT(3)	/* DMA read from flash */
#define NADCRR_DMAWE		BIT(2)	/* DMA write to flash */
#define NADCRR_LBC_128		3	/* loop-back count 128 words */
#define NADCRR_LBC_64		2	/* loop-back count 64 words */
#define NADCRR_LBC_32		1	/* loop-back count 32 words */
#define NADCRR_LBC_16		0	/* loop-back count 16 words */

/* DMA read command: DESC0 + DMARE + LBC_64, TAG enabled */
#define NADCRR_DMA_READ		(NADCRR_DESC0 | NADCRR_DMARE | NADCRR_LBC_64)
/* DMA write command: DESC0 + DMAWE + LBC_64, TAG enabled */
#define NADCRR_DMA_WRITE	(NADCRR_DESC0 | NADCRR_DMAWE | NADCRR_LBC_64)

/* ── NASR (status register, 0x28) ── */
#define NASR_ECC_CNT_MASK	0x000000F0	/* correctable ECC error count (bits 7:4) */
#define NASR_ECC_CNT_SHIFT	4
#define NASR_NECN		BIT(4)	/* need ECC correction */
#define NASR_NRER		BIT(3)	/* NAND read error */
#define NASR_NWER		BIT(2)	/* NAND write error */
#define NASR_NDRS		BIT(1)	/* NAND DMA read status */
#define NASR_NDWS		BIT(0)	/* NAND DMA write status */
#define NASR_CLEAR		0x0000000F

/* ── NACFR strap bits (boot-time hardware strap) ── */
#define NACFR_PAGE_SZ_512	(0 << 30)
#define NACFR_PAGE_SZ_2K	(1 << 30)
#define NACFR_PAGE_SZ_4K	(2 << 30)
#define NACFR_PAGE_SZ_8K	(3 << 30)
#define NACFR_ADDR_CYCLE_MASK	(3 << 26)
#define NACFR_ADDR_CYCLE_3	(0 << 26)
#define NACFR_ADDR_CYCLE_4	(1 << 26)
#define NACFR_ADDR_CYCLE_5	(2 << 26)

/* ── Realtek-specific NAND command aliases (standard names from rawnand.h) ── */
#define CMD_READ_ID		0x90
#define CMD_STATUS		0x70
#define CMD_PAGE_READ_1		0x00
#define CMD_PAGE_READ_2		0x30
#define CMD_PAGE_PROG_1		0x80
#define CMD_PAGE_PROG_2		0x10
#define CMD_BLOCK_ERASE_1	0x60
#define CMD_BLOCK_ERASE_2	0xD0

/* ── Clock / pinmux / system registers (KSEG1) ── */
#define CLOCK_ENABLE1		0xB800000C
#define CLK_NAND_EN		BIT(23)

#define CLK_MANAGE_REG		0xB8000010
#define CLK_MANAGE_NAND_MASK	((1 << 28) | (3 << 12) | (7 << 18))

#define PINMUX_SEL_1		0xB8000100
#define PINMUX_SEL_2		0xB8000104
#define PINMUX_SEL_3		0xB8000108

#define PINMUX_SEL1_NAND	BIT(26)
#define PINMUX_SEL2_NAND	BIT(23)
#define PINMUX_SEL3_NAND	(BIT(15) | BIT(18) | BIT(21) | BIT(24) | BIT(27) | BIT(30))

#define PIN_MUX00		0xB8000800
#define PIN_MUX00_NAND		BIT(30)
#define PIN_MUX01		0xB8000804
#define PIN_MUX01_NAND		BIT(11)
#define PIN_MUX02		0xB8000808
#define PIN_MUX02_NAND_MASK	0xF0000000
#define PIN_MUX02_NAND_VAL	0x60000000

#define HW_STRAP_REG		0xB8000008

/* KSEG1 → physical address mask */
#define KSEG1_MASK		0x1FFFFFFF

/* ── Flash geometry constants (2K-page NAND) ── */
#define NAND_SECTOR_SIZE	512	/* bytes per ECC sector */
#define NAND_TAG_SIZE		16	/* OOB bytes per sector (tag area) */
#define NAND_SECTOR_TOTAL	(NAND_SECTOR_SIZE + NAND_TAG_SIZE)  /* 528 */
#define NAND_MAX_SECTORS	16	/* max sectors for 8K pages */

/*
 * SWAP_2K_DATA — Bad Block Indicator relocation.
 *
 * The old BSP relocates the factory bad-block marker for boot blocks ≥ 2:
 *   data[DATA_BBI_OFF] <-> oob[OOB_BBI_OFF]
 *
 * Define RTL8198C_SWAP_BBI to enable this behaviour.
 * Comment it out if it causes trouble during bringup.
 */
#define RTL8198C_SWAP_BBI

#ifdef RTL8198C_SWAP_BBI
/* BBI positions for 2K-page NAND (4 sectors of 512+16) */
#define BOOT_BLOCK		2
#define DATA_BBI_OFF		((512 * 4) - 48)	/* = 2000 */
#define OOB_BBI_OFF		23
#endif

/* ── Inline helpers for raw register I/O ── */
static inline u32 rtk_readl(unsigned long offset)
{
	return __raw_readl((void __iomem *)offset);
}

static inline void rtk_writel(u32 val, unsigned long offset)
{
	__raw_writel(val, (void __iomem *)offset);
}

#endif /* RTL8198C_NAND_H */
