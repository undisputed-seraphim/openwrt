// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8198C NAND Flash Controller Driver
 *
 * DMA-based driver for the parallel NAND controller on the Realtek
 * RTL8198C SoC.  Uses hardware ECC (BCH-4 per 512B) via the DMA
 * descriptor engine (DESC0|DMARE for read, DESC0|DMAWE for write).
 * Erase is handled via the PIO command path (0x60/addr/0xD0).
 *
 * Chip detection (ONFI) and geometry are auto-detected by the kernel's
 * raw NAND framework — our cmdfunc provides the PIO command path
 * (READID, STATUS, PARAM, RNDOUT) and ecc.{read,write}_page provide
 * the hardware-accelerated DMA path.
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
#include <asm/cacheflush.h>
#include <asm/io.h>

#include "rtl8198c_nand.h"

struct rtl8198c_nand {
	struct device		*dev;
	void __iomem		*base;
	struct nand_controller	controller;
	struct nand_chip	chip;

	u32			cs;
	int			cmd_status;	/* 0 = OK, NAND_STATUS_FAIL = fail */

	/* DMA bounce buffer — 528 bytes, cacheline-aligned */
	u8			*dma_buf;
	u8			*dma_buf_orig;
	unsigned long		dma_buf_phys;

	/* Page/OOB scratch buffers for OOB-only updates */
	u8			*page_buf;
	u8			*oob_save;
	u8			*oob_raw;

	/* Flash geometry */
	int			page_size;
	int			oob_size;
	int			sectors_per_page;

	/* PIO data read — cache NADR 32-bit word and byte position */
	u32			nadr_word;
	int			nadr_idx;

	spinlock_t		lock;
};

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
	dev_err(nand->dev, "controller ready timeout\n");
}

static void nand_dma_sync(u8 *buf, int len)
{
	dma_cache_wback_inv((unsigned long)buf, len);
}

static unsigned long nand_phys_addr(void *vaddr)
{
	return virt_to_phys(vaddr);
}

static bool nand_buf_all_ff(const u8 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if (buf[i] != 0xff)
			return false;

	return true;
}

/* =================================================================
 *  DMA page read
 * ================================================================= */

static int nand_dma_read_sector(struct rtl8198c_nand *nand,
				unsigned long flash_addr)
{
	unsigned long data_phys = nand->dma_buf_phys;
	unsigned long tag_phys  = data_phys + NAND_SECTOR_SIZE;

	nand_dma_sync(nand->dma_buf, NAND_SECTOR_TOTAL);

	nand_writel(nand, NACR_READY | NACR_ECC_EN | 0x000FFFFF, NACR);
	nand_writel(nand, NASR_CLEAR, NASR);

	nand_writel(nand, data_phys, NADRSAR);
	nand_writel(nand, tag_phys, NADTSAR);
	nand_writel(nand, flash_addr, NADFSAR);

	nand_writel(nand, NADCRR_DMA_READ, NADCRR);
	nfc_wait_ready(nand);

	return 0;
}

/*
 * Raw data-only read with ECC disabled.  Used to distinguish an erased
 * (all-0xff) page from a genuinely corrupt one when the ECC engine reports
 * an uncorrectable error.  The bootloader does the same in
 * rtk_check_pageData()/rtk_check_allone().
 */
static void nand_dma_read_sector_raw(struct rtl8198c_nand *nand,
				     unsigned long flash_addr)
{
	unsigned long data_phys = nand->dma_buf_phys;

	nand_dma_sync(nand->dma_buf, NAND_SECTOR_SIZE);

	nand_writel(nand, NACR_READY | 0x000FFFFF, NACR);
	nand_writel(nand, NASR_CLEAR, NASR);

	nand_writel(nand, data_phys, NADRSAR);
	nand_writel(nand, flash_addr, NADFSAR);

	nand_writel(nand, NADCRR_DESC0 | NADCRR_DMARE |
		    NADCRR_LBC_128 | NADCRR_TAG_DIS, NADCRR);
	nfc_wait_ready(nand);
}

/*
 * Raw data+tag read with ECC disabled, used for the bad-block marker.
 * The marker lives in the first byte of the OOB tag and must not be
 * rejected just because the page data has bad ECC.
 */
static void nand_dma_read_sector_raw_oob(struct rtl8198c_nand *nand,
					 unsigned long flash_addr)
{
	unsigned long data_phys = nand->dma_buf_phys;
	unsigned long tag_phys = data_phys + NAND_SECTOR_SIZE;

	nand_dma_sync(nand->dma_buf, NAND_SECTOR_TOTAL);

	nand_writel(nand, NACR_READY | 0x000FFFFF, NACR);
	nand_writel(nand, NASR_CLEAR, NASR);

	nand_writel(nand, data_phys, NADRSAR);
	nand_writel(nand, tag_phys, NADTSAR);
	nand_writel(nand, flash_addr, NADFSAR);

	nand_writel(nand, NADCRR_DMA_READ, NADCRR);
	nfc_wait_ready(nand);
}

static int nand_check_sector(struct rtl8198c_nand *nand, int page, int offset,
			     unsigned long flash_addr)
{
	u32 status;
	int ecnt;
	int i;

	status = nand_readl(nand, NASR);

	if (!(status & NASR_NDRS))
		return 0;

	if (status & NASR_NRER) {
		ecnt = (status & NASR_ECC_CNT_MASK) >> NASR_ECC_CNT_SHIFT;
		if (ecnt > 0 && ecnt <= 4) {
			dev_dbg(nand->dev, "corrected %d ECC errors at page %d, offset %d\n",
				ecnt, page, offset);
			nand_writel(nand, status & ~NASR_NECN, NASR);
			return 1;
		}

		/*
		 * The bootloader treats an all-0xff page as erased even when
		 * the controller reports an uncorrectable ECC error.  Re-read
		 * the sector with ECC disabled to check the raw data, because
		 * the failed ECC path may have left garbage in the bounce
		 * buffer.
		 */
		nand_dma_read_sector_raw(nand, flash_addr);
		if (nand_buf_all_ff(nand->dma_buf, NAND_SECTOR_SIZE)) {
			dev_dbg(nand->dev, "ignoring ECC error on erased page %d, offset %d\n",
				page, offset);
			nand_writel(nand, status & ~NASR_NECN, NASR);
			return 0;
		}

		for (i = 0; i < NAND_SECTOR_SIZE; i++)
			if (nand->dma_buf[i] != 0xff)
				break;

		nand_writel(nand, status & ~NASR_NECN, NASR);
		dev_err(nand->dev, "uncorrectable ECC error at page %d, offset %d, status %#010x (raw byte %d = %#04x)\n",
			page, offset, status, i,
			i < NAND_SECTOR_SIZE ? nand->dma_buf[i] : 0xff);
		return -1;
	}

	nand_writel(nand, status & ~0xf0, NASR);
	return 0;
}

/* =================================================================
 *  ECC hooks — DMA-based page read with HW BCH-4 ECC
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
					sector * NAND_SECTOR_TOTAL, flash_addr);
		if (ret < 0)
			goto out;

		memcpy(buf + sector * NAND_SECTOR_SIZE,
		       nand->dma_buf, NAND_SECTOR_SIZE);
		memcpy(oob + sector * NAND_TAG_SIZE,
		       nand->dma_buf + NAND_SECTOR_SIZE, NAND_TAG_SIZE);

		flash_addr += NAND_SECTOR_TOTAL;
	}

#ifdef RTL8198C_SWAP_BBI
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

static int rtl8198c_nand_read_oob(struct nand_chip *chip, int page)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	uint8_t *oob = chip->oob_poi;
	unsigned long flash_addr;
	int sector;

	memset(oob, 0xff, mtd->oobsize);

	if (mtd->writesize >= 2048)
		flash_addr = (unsigned long)page << (chip->page_shift + 1);
	else
		flash_addr = (unsigned long)page << chip->page_shift;

	for (sector = 0; sector < nand->sectors_per_page; sector++) {
		/*
		 * OOB user bytes are not covered by the data ECC.  Read the
		 * raw tag so a weak data page does not make its cleanmarker
		 * or bad-block marker unreadable.
		 */
		nand_dma_read_sector_raw_oob(nand, flash_addr);

		memcpy(oob + sector * NAND_TAG_SIZE,
		       nand->dma_buf + NAND_SECTOR_SIZE, NAND_TAG_SIZE);

		flash_addr += NAND_SECTOR_TOTAL;
	}

	return 0;
}

static int rtl8198c_nand_write_page(struct nand_chip *chip, const uint8_t *buf,
				    int oob_required, int page)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	uint8_t *oob = chip->oob_poi;
	unsigned long flash_addr;
	u32 status;
	int sector;
#ifdef RTL8198C_SWAP_BBI
	int orig_block;
	bool bbi_swap = false;
	u8 bbi_val = 0xff;
	int bbi_sector = OOB_BBI_OFF / NAND_TAG_SIZE;
	int bbi_tag = OOB_BBI_OFF % NAND_TAG_SIZE;
	int bbi_data = DATA_BBI_OFF - bbi_sector * NAND_SECTOR_SIZE;
#endif

	if (mtd->writesize >= 2048)
		flash_addr = (unsigned long)page << (chip->page_shift + 1);
	else
		flash_addr = (unsigned long)page << chip->page_shift;

#ifdef RTL8198C_SWAP_BBI
	orig_block = page / (mtd->erasesize / mtd->writesize);
	/*
	 * Store the payload byte at DATA_BBI_OFF in the OOB tag and leave
	 * 0xff in the page data, exactly as the bootloader's
	 * rtk_write_ecc_page_a() does.  The controller's ECC is then
	 * calculated over the same bytes the bootloader will read.
	 */
	if (mtd->writesize == 2048 && orig_block >= BOOT_BLOCK) {
		bbi_swap = true;
		bbi_val = buf[DATA_BBI_OFF];
	}
#endif

	nand_writel(nand, NACR_READY | NACR_ECC_EN | 0x000FFFFF, NACR);
	nand_writel(nand, NASR_CLEAR, NASR);

	for (sector = 0; sector < nand->sectors_per_page; sector++) {
		memcpy(nand->dma_buf, buf + sector * NAND_SECTOR_SIZE,
		       NAND_SECTOR_SIZE);

#ifdef RTL8198C_SWAP_BBI
		if (bbi_swap && sector == bbi_sector)
			nand->dma_buf[bbi_data] = 0xff;
#endif

		if (oob_required && oob)
			memcpy(nand->dma_buf + NAND_SECTOR_SIZE,
			       oob + sector * NAND_TAG_SIZE, 6);
		else
			memset(nand->dma_buf + NAND_SECTOR_SIZE, 0xff, 6);

#ifdef RTL8198C_SWAP_BBI
		if (bbi_swap && sector == bbi_sector)
			nand->dma_buf[NAND_SECTOR_SIZE + bbi_tag] = bbi_val;
#endif

		dma_cache_wback_inv((unsigned long)nand->dma_buf,
				    NAND_SECTOR_TOTAL);

		/* clear RBO & WBO before DMA write (BSP § write) */
		nand_writel(nand, nand_readl(nand, NACR) &
			    ~(NACR_RBO | NACR_WBO), NACR);

		nand_writel(nand, nand->dma_buf_phys, NADRSAR);
		nand_writel(nand, nand->dma_buf_phys + NAND_SECTOR_SIZE,
			    NADTSAR);
		nand_writel(nand, flash_addr, NADFSAR);

		/* DESC0 | DMAWE | LBC_128, matching the bootloader */
		nand_writel(nand, NADCRR_DESC0 | NADCRR_DMAWE |
			    NADCRR_LBC_128, NADCRR);
		nfc_wait_ready(nand);

		status = nand_readl(nand, NASR);
		if ((status & NASR_NDWS) && (status & NASR_NWER)) {
			dev_err(nand->dev, "write error at page %d, sector %d, status %#010x\n",
				page, sector, status);
			nand_writel(nand, status & ~NASR_NWER, NASR);
			return -EIO;
		}
		nand_writel(nand, status & ~0x0f, NASR);

		flash_addr += NAND_SECTOR_TOTAL;
	}

	return 0;
}

static int rtl8198c_nand_write_page_raw(struct nand_chip *chip,
					const uint8_t *buf, int oob_required,
					int page)
{
	return rtl8198c_nand_write_page(chip, buf, oob_required, page);
}

static int rtl8198c_nand_write_oob(struct nand_chip *chip, int page)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret;

	if (!nand->page_buf || !nand->oob_save)
		return -ENOMEM;

	/*
	 * JFFS2 writes its cleanmarker into the OOB of the first page of
	 * each eraseblock.  The controller has no spare-area-only DMA mode,
	 * so read the page, replace the OOB and program it back.  The
	 * JFFS2 target patch reserves that first page, so the 0xff data
	 * written here is never overwritten by a later data node.
	 */
	memcpy(nand->oob_save, chip->oob_poi, mtd->oobsize);

	ret = rtl8198c_nand_read_page(chip, nand->page_buf, 0, page);
	if (ret)
		return ret;

	memcpy(chip->oob_poi, nand->oob_save, mtd->oobsize);

	return rtl8198c_nand_write_page(chip, nand->page_buf, 1, page);
}

/* =================================================================
 *  Legacy ops — cmdfunc, select_chip, read_byte, read_buf, waitfunc
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
	nand->cs = (cs == 0) ? NACMR_CECS0 : NACMR_CECS1;
}

/*
 * cmdfunc — handles all PIO-accessible NAND commands.
 * DMA page/OOB reads are handled via ecc.read_page / ecc.read_oob.
 */
static void rtl8198c_nand_cmdfunc(struct nand_chip *chip, unsigned command,
				  int column, int page_addr)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	unsigned long flags;

	if (command == NAND_CMD_NONE)
		return;

	spin_lock_irqsave(&nand->lock, flags);

	nand->cmd_status = 0;
	nand->nadr_idx = 0;

	/* ensure NACR has ECC/RBO/WBO enabled for all commands */
	nand_writel(nand, nand_readl(nand, NACR) | NACR_ECC_EN | NACR_RBO | NACR_WBO,
		    NACR);

	switch (command) {
	case NAND_CMD_RESET:
		nand_writel(nand, nand->cs | NAND_CMD_RESET, NACMR);
		nfc_wait_ready(nand);
		nand_writel(nand, 0, NACMR);
		break;

	case NAND_CMD_READID:
		nand_writel(nand, nand->cs | NAND_CMD_READID, NACMR);
		nfc_wait_ready(nand);
		nand_writel(nand, NAADR_AD0EN | (column & 0xff), NAADR);
		nfc_wait_ready(nand);
		break;

	case NAND_CMD_STATUS:
		nand_writel(nand, nand->cs | NAND_CMD_STATUS, NACMR);
		nfc_wait_ready(nand);
		break;

	case NAND_CMD_PARAM:
		nand_writel(nand, nand->cs | NAND_CMD_PARAM, NACMR);
		nfc_wait_ready(nand);
		nand_writel(nand, NAADR_AD0EN | (column & 0xff), NAADR);
		nfc_wait_ready(nand);
		break;

	case NAND_CMD_RNDOUT:
		nand_writel(nand, nand->cs | NAND_CMD_RNDOUT, NACMR);
		nfc_wait_ready(nand);
		nand_writel(nand, NAADR_AD1EN | NAADR_AD0EN |
			    ((column >> 8) << NAADR_AD1_SHIFT) |
			    (column & 0xff), NAADR);
		nfc_wait_ready(nand);
		break;

	case NAND_CMD_RNDOUTSTART:
		nand_writel(nand, nand->cs | NAND_CMD_RNDOUTSTART, NACMR);
		nfc_wait_ready(nand);
		nand->nadr_idx = 0;
		break;

	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_SEQIN:
	case NAND_CMD_PAGEPROG:
	case NAND_CMD_READSTART:
		/* handled by ecc hooks or are write-related no-ops */
		break;

	case NAND_CMD_ERASE1:
		/* 0x60 + 3 row address bytes */
		nand_writel(nand, nand->cs | NAND_CMD_ERASE1, NACMR);
		nfc_wait_ready(nand);
		nand_writel(nand, NAADR_AD2EN | NAADR_AD1EN | NAADR_AD0EN |
			    (page_addr & 0xff) |
			    ((page_addr >> NAADR_AD1_SHIFT) & 0xff)
			    << NAADR_AD1_SHIFT |
			    ((page_addr >> NAADR_AD2_SHIFT) & 0xff)
			    << NAADR_AD2_SHIFT, NAADR);
		nfc_wait_ready(nand);
		break;

	case NAND_CMD_ERASE2:
		/* 0xD0 — commit erase; kernel then calls waitfunc to poll status */
		nand_writel(nand, nand->cs | NAND_CMD_ERASE2, NACMR);
		nfc_wait_ready(nand);
		break;

	default:
		dev_dbg(nand->dev, "unsupported command %#04x\n", command);
		break;
	}

	spin_unlock_irqrestore(&nand->lock, flags);
}

static uint8_t rtl8198c_nand_read_byte(struct nand_chip *chip)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	u8 val;

	if ((nand->nadr_idx & 3) == 0)
		nand->nadr_word = nand_readl(nand, NADR);
	val = (nand->nadr_word >> ((nand->nadr_idx & 3) * 8)) & 0xff;
	nand->nadr_idx++;
	return val;
}

static void rtl8198c_nand_read_buf(struct nand_chip *chip, uint8_t *buf, int len)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	int i;

	for (i = 0; i < len; i++) {
		if ((i & 3) == 0)
			nand->nadr_word = nand_readl(nand, NADR);
		buf[i] = (nand->nadr_word >> ((i & 3) * 8)) & 0xff;
	}
}

/*
 * waitfunc — poll STATUS until the chip is ready (bit 6), then return status.
 * Called by the kernel after ERASE2 and after PAGEPROG.
 */
static int rtl8198c_nand_waitfunc(struct nand_chip *chip)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	unsigned long timeo = jiffies + msecs_to_jiffies(400);
	u8 status;
	unsigned long flags;

	if (nand->cmd_status & NAND_STATUS_FAIL)
		return NAND_STATUS_FAIL;

	do {
		spin_lock_irqsave(&nand->lock, flags);

		nand_writel(nand, nand_readl(nand, NACR) | NACR_ECC_EN |
			    NACR_RBO | NACR_WBO, NACR);
		nand_writel(nand, nand->cs | NAND_CMD_STATUS, NACMR);
		nfc_wait_ready(nand);
		status = nand_readl(nand, NADR) & 0xff;
		nand_writel(nand, 0, NACMR);

		spin_unlock_irqrestore(&nand->lock, flags);

		if (status & NAND_STATUS_READY)
			return status;

		cond_resched();
	} while (time_before(jiffies, timeo));

	dev_err(nand->dev, "waitfunc timeout\n");
	return NAND_STATUS_FAIL;
}

static int rtl8198c_nand_block_bad(struct nand_chip *chip, loff_t ofs)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	unsigned long page = (unsigned long)ofs >> chip->page_shift;
	unsigned long flash_addr = page << (chip->page_shift + 1);
	u8 marker;

	nand_dma_read_sector_raw_oob(nand, flash_addr);
	marker = nand->dma_buf[NAND_SECTOR_SIZE + chip->badblockpos];

	return marker != 0xff;
}

static int rtl8198c_nand_block_markbad(struct nand_chip *chip, loff_t ofs)
{
	struct rtl8198c_nand *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	unsigned long page = (unsigned long)ofs >> chip->page_shift;

	if (!nand->page_buf)
		return -ENOMEM;

	/*
	 * Write the marker without reading the page first.  A weak page may
	 * have bad data ECC, and read-modify-write would fail before the
	 * marker could be programmed.
	 */
	memset(nand->page_buf, 0xff, mtd->writesize);
	memset(chip->oob_poi, 0xff, mtd->oobsize);
	chip->oob_poi[chip->badblockpos] = 0x00;

	return rtl8198c_nand_write_page(chip, nand->page_buf, 1, page);
}

/* =================================================================
 *  OOB layout — 6 user bytes + 10 ECC bytes per 512-byte sector
 * ================================================================= */
static int rtl8198c_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	int sectors = mtd->writesize / NAND_SECTOR_SIZE;

	if (section < 0 || section >= sectors)
		return -ERANGE;

	oobregion->offset = section * NAND_TAG_SIZE + 6;
	oobregion->length = NAND_TAG_SIZE - 6;	/* 10 ECC bytes */

	return 0;
}

static int rtl8198c_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	int sectors = mtd->writesize / NAND_SECTOR_SIZE;

	if (section < 0 || section >= sectors)
		return -ERANGE;

	if (section == 0) {
		/* Leave OOB bytes 0..1 for the bad-block marker */
		oobregion->offset = 2;
		oobregion->length = 4;
	} else {
		oobregion->offset = section * NAND_TAG_SIZE;
		oobregion->length = 6;
	}

	return 0;
}

static const struct mtd_ooblayout_ops rtl8198c_ooblayout_ops = {
	.ecc = rtl8198c_ooblayout_ecc,
	.free = rtl8198c_ooblayout_free,
};

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

	chip->ecc.read_page      = rtl8198c_nand_read_page;
	chip->ecc.read_page_raw  = rtl8198c_nand_read_page;
	chip->ecc.write_page     = rtl8198c_nand_write_page;
	chip->ecc.write_page_raw = rtl8198c_nand_write_page_raw;
	chip->ecc.read_oob       = rtl8198c_nand_read_oob;
	chip->ecc.write_oob      = rtl8198c_nand_write_oob;
	chip->legacy.block_bad     = rtl8198c_nand_block_bad;
	chip->legacy.block_markbad = rtl8198c_nand_block_markbad;

	mtd_set_ooblayout(mtd, &rtl8198c_ooblayout_ops);

	nand->page_size  = mtd->writesize;
	nand->oob_size   = mtd->oobsize;
	nand->sectors_per_page = nand->page_size / NAND_SECTOR_SIZE;

	nand->page_buf = devm_kzalloc(nand->dev, mtd->writesize, GFP_KERNEL);
	nand->oob_save = devm_kzalloc(nand->dev, mtd->oobsize, GFP_KERNEL);
	nand->oob_raw = devm_kzalloc(nand->dev, mtd->oobsize, GFP_KERNEL);
	if (!nand->page_buf || !nand->oob_save || !nand->oob_raw)
		return -ENOMEM;

	dev_info(nand->dev, "using BCH-4 hardware ECC, %d sectors per page\n",
		 nand->sectors_per_page);

	return 0;
}

static const struct nand_controller_ops rtl8198c_nand_controller_ops = {
	.attach_chip = rtl8198c_nand_attach_chip,
};

/* =================================================================
 *  Clock / pinmux / HW strap init
 * ================================================================= */

static void rtl8198c_nand_init_hw(void)
{
	u32 v;

	v = rtk_readl(CLOCK_ENABLE1);
	v |= CLK_NAND_EN;
	rtk_writel(v, CLOCK_ENABLE1);

	v = rtk_readl(CLK_MANAGE_REG);
	v |= CLK_MANAGE_NAND_MASK;
	rtk_writel(v, CLK_MANAGE_REG);

	v = rtk_readl(PINMUX_SEL_1);
	v |= PINMUX_SEL1_NAND;
	rtk_writel(v, PINMUX_SEL_1);

	v = rtk_readl(PINMUX_SEL_2);
	v |= PINMUX_SEL2_NAND;
	rtk_writel(v, PINMUX_SEL_2);

	v = rtk_readl(PINMUX_SEL_3);
	v |= PINMUX_SEL3_NAND;
	rtk_writel(v, PINMUX_SEL_3);

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

	nand->dev = dev;
	spin_lock_init(&nand->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	nand->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(nand->base))
		return PTR_ERR(nand->base);

	rtl8198c_nand_init_hw();

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

	chip->legacy.cmdfunc     = rtl8198c_nand_cmdfunc;
	chip->legacy.select_chip = rtl8198c_nand_select_chip;
	chip->legacy.dev_ready   = rtl8198c_nand_ready;
	chip->legacy.read_byte   = rtl8198c_nand_read_byte;
	chip->legacy.read_buf    = rtl8198c_nand_read_buf;
	chip->legacy.write_buf   = NULL;
	chip->legacy.waitfunc    = rtl8198c_nand_waitfunc;
	chip->legacy.chip_delay  = 20;

	chip->options = NAND_NO_SUBPAGE_WRITE | NAND_SKIP_BBTSCAN;

	/*
	 * Send a reset pulse before scanning.
	 * The ONFI detection path in nand_scan will handle chip ID
	 * discovery and geometry setup automatically.
	 */
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
MODULE_DESCRIPTION("RTL8198C NAND flash controller driver");
MODULE_LICENSE("GPL");
