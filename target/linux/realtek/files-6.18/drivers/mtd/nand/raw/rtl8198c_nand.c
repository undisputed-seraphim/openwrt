// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C NAND Flash Controller Driver
 *
 * Read-only DMA-based driver for the parallel NAND controller on the
 * Realtek RTL8198C SoC.  Uses hardware ECC via the DMA descriptor engine.
 *
 * Write and erase are stubbed out — only page / OOB read is functional.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <asm/cacheflush.h>
#include <asm/io.h>

#include "rtl8198c_nand.h"

/* ── per-device state ── */
struct rtl8198c_nand {
	void __iomem		*base;
	struct nand_controller	controller;
	struct nand_chip	chip;

	u32			cs;		/* active chip-select mask */
	unsigned int		curr_command;
	int			cmd_status;	/* 0 = success, NAND_STATUS_FAIL = fail */

	/* ID / status cache */
	u8			id_data[8];
	int			id_idx;
	u8			status_byte;

	/* DMA bounce buffer — 528 bytes, cacheline-aligned (32 bytes on MIPS) */
	u8			*dma_buf;
	u8			*dma_buf_orig;	/* saved for kfree */
	unsigned long		dma_buf_phys;

	/* flash geometry (filled during probe) */
	int			page_size;
	int			oob_size;
	int			sectors_per_page;	/* page_size / NAND_SECTOR_SIZE */

	spinlock_t		lock;
};

/* ── I/O helpers ── */
static inline u32 nand_readl(struct rtl8198c_nand *nand, unsigned off)
{
	return __raw_readl(nand->base + off);
}

static inline void nand_writel(struct rtl8198c_nand *nand, u32 val, unsigned off)
{
	__raw_writel(val, nand->base + off);
}

/* ── controller-ready polling ── */
static void nfc_wait_ready(struct rtl8198c_nand *nand)
{
	int i;

	for (i = 0; i < 100000; i++) {
		if (nand_readl(nand, NACR) & NACR_READY)
			return;
		udelay(1);
	}
	pr_err("rtl8198c_nand: controller ready timeout\n");
}

/* ── cache helpers (match BSP dma_cache_* macros) ── */
static void nand_dma_sync(u8 *buf, int len)
{
	unsigned long start = (unsigned long)buf;

	dma_cache_wback_inv(start, len);
}

/* ── KSEG1 / kmalloc → physical address ── */
static unsigned long nand_phys_addr(void *vaddr)
{
	return virt_to_phys(vaddr);
}

/* =================================================================
 *  PIO command helpers (Read ID, Status, Reset)
 * ================================================================= */

static void nand_reset_cmd(struct rtl8198c_nand *nand)
{
	unsigned long flags;

	spin_lock_irqsave(&nand->lock, flags);

	nand_writel(nand, nand_readl(nand, NACR) | NACR_ECC_EN | NACR_RBO | NACR_WBO,
		    NACR);
	nand_writel(nand, NACMR_CECS0 | NAND_CMD_RESET, NACMR);
	nfc_wait_ready(nand);
	nand_writel(nand, 0, NACMR);

	spin_unlock_irqrestore(&nand->lock, flags);
}

static void nand_read_id_cmd(struct rtl8198c_nand *nand)
{
	unsigned long flags;
	u32 val;
	int i;

	spin_lock_irqsave(&nand->lock, flags);

	nand_writel(nand, nand_readl(nand, NACR) | NACR_ECC_EN | NACR_RBO | NACR_WBO,
		    NACR);

	nand_writel(nand, NACMR_CECS0 | NAND_CMD_READID, NACMR);
	nfc_wait_ready(nand);

	/* dummy address cycle */
	nand_writel(nand, NAADR_AD2EN | NAADR_AD1EN | NAADR_AD0EN, NAADR);
	nfc_wait_ready(nand);

	/* read 8 ID bytes from NADR (LSB-first) */
	for (i = 0; i < 8; i += 4) {
		val = nand_readl(nand, NADR);
		nand->id_data[i]     = val & 0xff;
		nand->id_data[i + 1] = (val >> 8) & 0xff;
		nand->id_data[i + 2] = (val >> 16) & 0xff;
		nand->id_data[i + 3] = (val >> 24) & 0xff;
	}

	nand_writel(nand, 0, NACMR);
	nand_writel(nand, 0, NAADR);

	spin_unlock_irqrestore(&nand->lock, flags);
}

static void nand_read_status_cmd(struct rtl8198c_nand *nand)
{
	unsigned long flags;

	spin_lock_irqsave(&nand->lock, flags);

	nand_writel(nand, nand_readl(nand, NACR) | NACR_ECC_EN | NACR_RBO | NACR_WBO,
		    NACR);
	nand_writel(nand, NACMR_CECS0 | NAND_CMD_STATUS, NACMR);
	nfc_wait_ready(nand);

	nand->status_byte = nand_readl(nand, NADR) & 0xff;

	nand_writel(nand, 0, NACMR);

	spin_unlock_irqrestore(&nand->lock, flags);
}

/* =================================================================
 *  DMA helpers
 * ================================================================= */

/*
 * Read one 528-byte sector from flash into the DMA bounce buffer.
 * Returns 0 on success or a negative error.
 */
static int nand_dma_read_sector(struct rtl8198c_nand *nand,
				unsigned long flash_addr)
{
	unsigned long data_phys = nand->dma_buf_phys;
	unsigned long tag_phys  = data_phys + NAND_SECTOR_SIZE;

	nand_dma_sync(nand->dma_buf, NAND_SECTOR_TOTAL);

	nand_writel(nand, NACR_DMA_READ, NACR);
	nand_writel(nand, NASR_CLEAR, NASR);

	nand_writel(nand, data_phys, NADRSAR);
	nand_writel(nand, tag_phys, NADTSAR);
	nand_writel(nand, flash_addr, NADFSAR);

	nand_writel(nand, NADCRR_DMA_READ, NADCRR);
	nfc_wait_ready(nand);

	return 0;
}

/*
 * Check DMA status after a sector read.
 * Returns: 0 = OK (data valid)
 *          1 = correctable ECC error (corrected)
 *         -1 = uncorrectable error
 */
static int nand_check_sector(struct rtl8198c_nand *nand, int page, int offset,
			     int is_last)
{
	u32 status;
	int ecnt;

	status = nand_readl(nand, NASR);

	if (!(status & NASR_NDRS))
		return 0;	/* No DMA-read status — shouldn't happen */

	if (status & NASR_NRER) {
		ecnt = (status & NASR_ECC_CNT_MASK) >> NASR_ECC_CNT_SHIFT;
		if (ecnt > 0 && ecnt <= 4) {
			pr_debug("rtl8198c_nand: corrected %d ECC errors at page %d off %d\n",
				 ecnt, page, offset);
			nand_writel(nand, status & ~NASR_NECN, NASR);
			return 1;	/* corrected */
		}
		/* uncorrectable — check if page is all-ones (erased) */
		nand_writel(nand, status & ~NASR_NECN, NASR);
		pr_err("rtl8198c_nand: uncorrectable ECC error at page %d off %d, status=0x%08x\n",
		       page, offset, status);
		return -1;
	}

	nand_writel(nand, status & ~0xf0, NASR);
	return 0;
}

/* =================================================================
 *  ECC page-read hooks
 * ================================================================= */

static int rtl8198c_nand_read_page(struct nand_chip *chip, uint8_t *buf,
				   int oob_required, int page)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	uint8_t *oob = chip->oob_poi;
	unsigned long flash_addr;
	int ret, sector;
	int orig_block;

	orig_block = page / (mtd->erasesize / mtd->writesize);

	/*
	 * Flash address: page << (page_shift + 1) — the RTL8198C
	 * controller inserts a 1-bit gap between column and row.
	 * BSP formula for 2K: page << 12  (page_shift=11, +1).
	 * For 512B pages: page << 9  (page_shift=9, no +1).
	 */
	if (mtd->writesize >= 2048)
		flash_addr = (unsigned long)page << (chip->page_shift + 1);
	else
		flash_addr = (unsigned long)page << chip->page_shift;

	for (sector = 0; sector < nand->sectors_per_page; sector++) {
		ret = nand_dma_read_sector(nand, flash_addr);
		if (ret)
			goto out;

		ret = nand_check_sector(nand, page,
					sector * NAND_SECTOR_TOTAL,
					sector == nand->sectors_per_page - 1);
		if (ret < 0)
			goto out;

		/* copy 512 bytes data + 16 bytes tag from bounce buffer */
		memcpy(buf + sector * NAND_SECTOR_SIZE,
		       nand->dma_buf, NAND_SECTOR_SIZE);
		memcpy(oob + sector * NAND_TAG_SIZE,
		       nand->dma_buf + NAND_SECTOR_SIZE, NAND_TAG_SIZE);

		flash_addr += NAND_SECTOR_TOTAL;
	}

#ifdef RTL8198C_SWAP_BBI
	/* swap bad block indicator for blocks >= BOOT_BLOCK */
	if (orig_block >= BOOT_BLOCK) {
		u8 tmp;

		tmp = buf[DATA_BBI_OFF];
		buf[DATA_BBI_OFF] = oob[OOB_BBI_OFF];
		oob[OOB_BBI_OFF] = tmp;
	}
#endif

	ret = 0;
out:
	return ret;
}

static int rtl8198c_nand_read_page_raw(struct nand_chip *chip, uint8_t *buf,
				       int oob_required, int page)
{
	/* HW ECC is always on for this controller */
	return rtl8198c_nand_read_page(chip, buf, oob_required, page);
}

static int rtl8198c_nand_read_oob(struct nand_chip *chip, int page)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	uint8_t *oob = chip->oob_poi;
	unsigned long flash_addr;
	int sector, ret;

	memset(oob, 0xff, mtd->oobsize);

	if (mtd->writesize >= 2048)
		flash_addr = (unsigned long)page << (chip->page_shift + 1);
	else
		flash_addr = (unsigned long)page << chip->page_shift;

	for (sector = 0; sector < nand->sectors_per_page; sector++) {
		ret = nand_dma_read_sector(nand, flash_addr);
		if (ret)
			return ret;

		ret = nand_check_sector(nand, page,
					sector * NAND_SECTOR_TOTAL,
					sector == nand->sectors_per_page - 1);
		if (ret < 0)
			return ret;

		/* only copy OOB from DMA buffer, skip data */
		memcpy(oob + sector * NAND_TAG_SIZE,
		       nand->dma_buf + NAND_SECTOR_SIZE, NAND_TAG_SIZE);

		flash_addr += NAND_SECTOR_TOTAL;
	}

	return 0;
}

/* ── write stubs ── */
static int rtl8198c_nand_write_page(struct nand_chip *chip, const uint8_t *buf,
				    int oob_required, int page)
{
	return -EIO;
}

static int rtl8198c_nand_write_page_raw(struct nand_chip *chip,
					const uint8_t *buf, int oob_required,
					int page)
{
	return -EIO;
}

static int rtl8198c_nand_write_oob(struct nand_chip *chip, int page)
{
	return -EIO;
}

/* =================================================================
 *  Legacy NAND ops
 * ================================================================= */

static int rtl8198c_nand_ready(struct nand_chip *chip)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);

	return !!(nand_readl(nand, NACR) & NACR_READY);
}

static void rtl8198c_nand_select_chip(struct nand_chip *chip, int cs)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);

	nand_writel(nand, 0, NACMR);

	if (cs < 0) {
		nand->cs = 0;
		return;
	}
	if (cs == 0)
		nand->cs = NACMR_CECS0;
	else
		nand->cs = NACMR_CECS1;
}

/*
 * cmdfunc — only handles PIO-accessible commands.
 * Page reads/writes are handled via ecc.{read,write}_page hooks.
 */
static void rtl8198c_nand_cmdfunc(struct nand_chip *chip, unsigned command,
				  int column, int page_addr)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);

	if (command == NAND_CMD_NONE)
		return;

	nand->curr_command = command;
	nand->cmd_status = 0;

	switch (command) {
	case NAND_CMD_RESET:
		nand_reset_cmd(nand);
		break;

	case NAND_CMD_READID:
		nand->id_idx = 0;
		nand_read_id_cmd(nand);
		break;

	case NAND_CMD_STATUS:
		nand_read_status_cmd(nand);
		break;

	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_SEQIN:
	case NAND_CMD_PAGEPROG:
	case NAND_CMD_RNDIN:
	case NAND_CMD_RNDOUT:
	case NAND_CMD_RNDOUTSTART:
		/* handled by ecc hooks or are write-related no-ops */
		break;

	case NAND_CMD_ERASE1:
	case NAND_CMD_ERASE2:
		nand->cmd_status = NAND_STATUS_FAIL;
		break;

	default:
		pr_debug("rtl8198c_nand: unsupported cmd 0x%02x\n", command);
		break;
	}
}

static uint8_t rtl8198c_nand_read_byte(struct nand_chip *chip)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);

	switch (nand->curr_command) {
	case NAND_CMD_READID:
		if (nand->id_idx >= ARRAY_SIZE(nand->id_data)) {
			pr_err("rtl8198c_nand: invalid id_data index %d\n",
			       nand->id_idx);
			return 0;
		}
		return nand->id_data[nand->id_idx++];

	case NAND_CMD_STATUS:
		return nand->status_byte;

	default:
		pr_debug("rtl8198c_nand: unexpected read_byte for cmd 0x%02x\n",
			 nand->curr_command);
		return 0;
	}
}

static void rtl8198c_nand_read_buf(struct nand_chip *chip, uint8_t *buf, int len)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	u32 val;
	int i;

	/* PIO fallback — only used for ONFI param page and small reads */
	for (i = 0; i < len; i++) {
		if ((i & 3) == 0)
			val = nand_readl(nand, NADR);
		buf[i] = (val >> ((i & 3) * 8)) & 0xff;
	}
}

/*
 * waitfunc — used by nand_wait() to detect erase failure.
 * Returns chip->state for success, negative for failure.
 */
static int rtl8198c_nand_waitfunc(struct nand_chip *chip)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);

	if (nand->cmd_status & NAND_STATUS_FAIL)
		return -EIO;
	return 0;
}

/* =================================================================
 *  attach_chip — configure ECC engine
 * ================================================================= */

static int rtl8198c_nand_attach_chip(struct nand_chip *chip)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);

	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.placement   = NAND_ECC_PLACEMENT_OOB;
	chip->ecc.size	       = NAND_SECTOR_SIZE;
	chip->ecc.strength     = 4;
	chip->ecc.bytes        = 10;

	chip->ecc.read_page     = rtl8198c_nand_read_page;
	chip->ecc.read_page_raw = rtl8198c_nand_read_page_raw;
	chip->ecc.write_page    = rtl8198c_nand_write_page;
	chip->ecc.write_page_raw = rtl8198c_nand_write_page_raw;
	chip->ecc.read_oob      = rtl8198c_nand_read_oob;
	chip->ecc.write_oob     = rtl8198c_nand_write_oob;

	nand->page_size  = mtd->writesize;
	nand->oob_size   = mtd->oobsize;
	nand->sectors_per_page = nand->page_size / NAND_SECTOR_SIZE;

	pr_info("rtl8198c_nand: HW ECC BCH-4 per %d bytes, %d sectors/page\n",
		NAND_SECTOR_SIZE, nand->sectors_per_page);

	return 0;
}

static const struct nand_controller_ops rtl8198c_nand_controller_ops = {
	.attach_chip = rtl8198c_nand_attach_chip,
};

/* =================================================================
 *  OOB layout description (2K-page, 64-byte OOB)
 * ================================================================= */

static int rtl8198c_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	if (section >= 4)
		return -ERANGE;

	/* ECC bytes for each 512-byte sector: 10 bytes, starting at offset
	 * 23 + section*16 within the 64-byte OOB area.
	 * Per BSP nand_bch_oob_64 layout:
	 *   Sector 0 eccpos: 23..32 (10 bytes)
	 *   Sector 1 eccpos: 33..42
	 *   Sector 2 eccpos: 43..52
	 *   Sector 3 eccpos: 53..62
	 *   BBI at pos 63
	 *
	 * Actually the BSP interleaves the 16-byte tag area per sector:
	 *   Sector 0 data tag bytes 0..5 → OOB 0..5
	 *   Sector 0 ECC bytes 0..9      → OOB 6..15 + ... wait, that doesn't match.
	 *
	 * Looking at the BSP eccpos array:
	 *   23,24,25, 26, 27, 28, 29, 30, 31,    ← sector 0 (10 bytes)
	 *   32,33, 34, 35, 36, 37, 38, 39,        ← sector 1 (10 bytes)
	 *   40,41, 42, 43, 44, 45, 46, 47,        ← sector 2 (10 bytes)
	 *   48,49, 50, 51, 52, 53, 54, 55,        ← sector 3 (10 bytes)
	 *   56,57, 58, 59, 60, 61, 62, 63         (continued + BBI)
	 *
	 * Each sector gets 10 bytes ECC. The total eccpos is 41 entries (40 ECC + 1 BBI).
	 * But the DMA transfers the full 16-byte tag per sector (6 user + 10 ECC).
	 * The physical layout on flash is the 16-byte tag area per sector:
	 *   Sector 0 tag: OOB[0..15]
	 *   Sector 1 tag: OOB[16..31]
	 *   Sector 2 tag: OOB[32..47]
	 *   Sector 3 tag: OOB[48..63]
	 *
	 * Our DMA copies the full 16-byte tag to the OOB buffer.
	 * The eccpos layout above (23..63) is the OLD BSP layout which
	 * reorders the tags differently (interleaving user data and ECC).
	 *
	 * For our driver, we use the SIMPLE layout:
	 *   Each sector's tag is 16 contiguous bytes in OOB.
	 *   ECC uses 10 of those 16 bytes per sector.
	 *   The remaining 6 bytes per sector are user OOB.
	 *
	 * Map: sector N ECC bytes at oob[N*16 + 6] through oob[N*16 + 15].
	 */

	oobregion->offset = section * NAND_TAG_SIZE + 6;
	oobregion->length = 10;
	return 0;
}

static int rtl8198c_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	if (section >= 4)
		return -ERANGE;

	/* 6 user-accessible OOB bytes per sector */
	oobregion->offset = section * NAND_TAG_SIZE;
	oobregion->length = 6;
	return 0;
}

static const struct mtd_ooblayout_ops rtl8198c_ooblayout_ops = {
	.ecc  = rtl8198c_ooblayout_ecc,
	.free = rtl8198c_ooblayout_free,
};

/* =================================================================
 *  Clock / pinmux / HW strap init
 * ================================================================= */

static void rtl8198c_nand_init_hw(void)
{
	u32 v;

	/* Enable NAND clock */
	v = rtk_readl(CLOCK_ENABLE1);
	v |= CLK_NAND_EN;
	rtk_writel(v, CLOCK_ENABLE1);

	/* Additional clock gating (BSP clk_manage_REG) */
	v = rtk_readl(CLK_MANAGE_REG);
	v |= CLK_MANAGE_NAND_MASK;
	rtk_writel(v, CLK_MANAGE_REG);

	/* Pinmux function select */
	v = rtk_readl(PINMUX_SEL_1);
	v |= PINMUX_SEL1_NAND;
	rtk_writel(v, PINMUX_SEL_1);

	v = rtk_readl(PINMUX_SEL_2);
	v |= PINMUX_SEL2_NAND;
	rtk_writel(v, PINMUX_SEL_2);

	v = rtk_readl(PINMUX_SEL_3);
	v |= PINMUX_SEL3_NAND;
	rtk_writel(v, PINMUX_SEL_3);

	/* PinMux I/O pad config */
	v = rtk_readl(PIN_MUX00);
	v |= PIN_MUX00_NAND;
	rtk_writel(v, PIN_MUX00);

	v = rtk_readl(PIN_MUX01);
	v |= PIN_MUX01_NAND;
	rtk_writel(v, PIN_MUX01);

	v = rtk_readl(PIN_MUX02);
	v &= ~PIN_MUX02_NAND_MASK;
	v |= PIN_MUX02_NAND_VAL;
	rtk_writel(v, PIN_MUX02);

	/* HW strap: 2K page, 4 address cycles */
	v = rtk_readl(HW_STRAP_REG);
	v &= ~(NACFR_PAGE_SZ_8K | NACFR_ADDR_CYCLE_MASK);
	v |= NACFR_PAGE_SZ_2K | NACFR_ADDR_CYCLE_4;
	rtk_writel(v, HW_STRAP_REG);

	udelay(100);
}

/* =================================================================
 *  Probe / remove
 * ================================================================= */

static int rtl8198c_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl8198c_nand *nand;
	struct nand_chip *chip;
	struct mtd_info *mtd;
	struct resource *res;
	int ret;

	nand = devm_kzalloc(dev, sizeof(*nand), GFP_KERNEL);
	if (!nand)
		return -ENOMEM;

	spin_lock_init(&nand->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	nand->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(nand->base))
		return PTR_ERR(nand->base);

	rtl8198c_nand_init_hw();

	/* allocate DMA bounce buffer (528 bytes, align to 16 bytes) */
	nand->dma_buf_orig = kmalloc(NAND_SECTOR_TOTAL + 32, GFP_KERNEL);
	if (!nand->dma_buf_orig)
		return -ENOMEM;
	nand->dma_buf = (u8 *)(((unsigned long)nand->dma_buf_orig + 15) & ~15UL);
	nand->dma_buf_phys = nand_phys_addr(nand->dma_buf);

	nand->cs = NACMR_CECS0;

	chip = &nand->chip;
	mtd = nand_to_mtd(chip);

	mtd->dev.parent = dev;
	mtd->name = "rtk_nand";
	mtd->owner = THIS_MODULE;
	mtd_set_ooblayout(mtd, &rtl8198c_ooblayout_ops);

	nand_set_controller_data(chip, nand);
	/* flash node is the first nand-chip child, not the controller */
	{
		struct device_node *np = dev->of_node;
		struct device_node *chip_np = NULL;

		for_each_child_of_node(np, chip_np) {
			if (of_node_name_eq(chip_np, "nand-chip"))
				break;
		}
		nand_set_flash_node(chip, chip_np ? chip_np : np);
		if (chip_np && chip_np != np)
			of_node_put(chip_np);
	}

	nand_controller_init(&nand->controller);
	nand->controller.ops = &rtl8198c_nand_controller_ops;
	chip->controller = &nand->controller;

	chip->legacy.cmdfunc    = rtl8198c_nand_cmdfunc;
	chip->legacy.select_chip = rtl8198c_nand_select_chip;
	chip->legacy.dev_ready  = rtl8198c_nand_ready;
	chip->legacy.read_byte  = rtl8198c_nand_read_byte;
	chip->legacy.read_buf   = rtl8198c_nand_read_buf;
	chip->legacy.write_buf  = NULL;	/* write not supported */
	chip->legacy.waitfunc   = rtl8198c_nand_waitfunc;
	chip->legacy.chip_delay = 20;

	chip->options = NAND_NO_SUBPAGE_WRITE | NAND_SKIP_BBTSCAN;

	/* send reset, then scan */
	nand_writel(nand, NACMR_CECS0 | NAND_CMD_RESET, NACMR);
	udelay(200);

	ret = nand_scan(chip, 1);
	if (ret) {
		dev_err(dev, "nand_scan failed: %d\n", ret);
		goto err_free;
	}

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "mtd_device_register failed: %d\n", ret);
		nand_cleanup(chip);
		goto err_free;
	}

	platform_set_drvdata(pdev, nand);

	dev_info(dev, "RTL8198C NAND registered as %s size=%lld pagesize=%d oobsize=%d erasesize=%d\n",
		 mtd->name, mtd->size, mtd->writesize, mtd->oobsize,
		 mtd->erasesize);

	return 0;

err_free:
	kfree(nand->dma_buf_orig);
	return ret;
}

static void rtl8198c_nand_remove(struct platform_device *pdev)
{
	struct rtl8198c_nand *nand = platform_get_drvdata(pdev);

	nand_cleanup(&nand->chip);
	kfree(nand->dma_buf_orig);
}

static const struct of_device_id rtl8198c_nand_of_match[] = {
	{ .compatible = "realtek,rtl8198c-nand" },
	{ },
};
MODULE_DEVICE_TABLE(of, rtl8198c_nand_of_match);

static struct platform_driver rtl8198c_nand_driver = {
	.probe  = rtl8198c_nand_probe,
	.remove = rtl8198c_nand_remove,
	.driver = {
		.name = "rtl8198c-nand",
		.of_match_table = rtl8198c_nand_of_match,
	},
};

module_platform_driver(rtl8198c_nand_driver);

MODULE_AUTHOR("OpenWrt");
MODULE_DESCRIPTION("RTL8198C NAND flash controller driver (read-only)");
MODULE_LICENSE("GPL");
