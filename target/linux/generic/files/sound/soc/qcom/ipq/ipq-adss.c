/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/of_device.h>
#include <linux/mod_devicetable.h>
#include <sound/pcm.h>

#include "ipq-adss.h"

static void __iomem *adss_audio_local_base;
static struct reset_control *audio_blk_rst;
static DEFINE_SPINLOCK(i2s_ctrl_lock);
static DEFINE_SPINLOCK(glb_mode_lock);

static struct ipq_configs ipq4019_cfgs = {
	.txd_oe = {
		.reg = ADSS_GLB_AUDIO_MODE_REG,
		.mask = GLB_AUDIO_MODE_I2S0_TXD_OE,
	},
	.rxd_oe = {
		.reg = ADSS_GLB_AUDIO_MODE_REG,
		.mask = GLB_AUDIO_MODE_I2S3_RXD_OE,
	},
	.i2s0_fs_oe = {
		.reg = ADSS_GLB_AUDIO_MODE_REG,
		.mask = GLB_AUDIO_MODE_I2S0_FS_OE,
	},
	.i2s3_fs_oe = {
		.reg = ADSS_GLB_AUDIO_MODE_REG,
		.mask = GLB_AUDIO_MODE_I2S3_FS_OE,
	},
	.i2s_reset_val = {
		.reg = ADSS_GLB_I2S_RST_REG,
		.mask = GLB_I2S_RESET_VAL_4019,
	},
	.spdif_enable = 0,
};

static struct ipq_configs *ipq_cfgs = &ipq4019_cfgs;

void ipq_audio_adss_writel(u32 val, u32 offset)
{
	if (!adss_audio_local_base) {
		pr_err("%s: adss_audio_local_base not mapped\n", __func__);
		return;
	}
	writel(val, adss_audio_local_base + offset);
}
EXPORT_SYMBOL(ipq_audio_adss_writel);

u32 ipq_audio_adss_readl(u32 offset)
{
	if (adss_audio_local_base)
		return readl(adss_audio_local_base + offset);
	pr_err("%s: adss_audio_local_base not mapped\n", __func__);
	return 0;
}
EXPORT_SYMBOL(ipq_audio_adss_readl);

void ipq_glb_i2s_interface_en(int enable)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&i2s_ctrl_lock, flags);
	cfg = readl(adss_audio_local_base + ADSS_GLB_CHIP_CTRL_I2S_REG);
	cfg &= ~GLB_CHIP_CTRL_I2S_INTERFACE_EN;
	if (enable)
		cfg |= GLB_CHIP_CTRL_I2S_INTERFACE_EN;
	writel(cfg, adss_audio_local_base + ADSS_GLB_CHIP_CTRL_I2S_REG);
	spin_unlock_irqrestore(&i2s_ctrl_lock, flags);
	mdelay(5);
}
EXPORT_SYMBOL(ipq_glb_i2s_interface_en);

static void ipq_glb_i2s_reset(void)
{
	writel(ipq_cfgs->i2s_reset_val.mask,
		adss_audio_local_base + ipq_cfgs->i2s_reset_val.reg);
	mdelay(5);
	writel(0x0, adss_audio_local_base + ADSS_GLB_I2S_RST_REG);
	mdelay(5);
}

void ipq_glb_mbox_reset(void)
{
	writel(GLB_I2S_RST_MBOX_RESET_MASK,
		adss_audio_local_base + ipq_cfgs->i2s_reset_val.reg);
	mdelay(5);
	writel(0x0, adss_audio_local_base + ADSS_GLB_I2S_RST_REG);
	mdelay(5);
}
EXPORT_SYMBOL(ipq_glb_mbox_reset);

void ipq_glb_audio_mode(int mode, int dir)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&glb_mode_lock, flags);
	cfg = readl(adss_audio_local_base + ADSS_GLB_AUDIO_MODE_REG);
	if (mode == I2S && dir == PLAYBACK) {
		cfg &= ~GLB_AUDIO_MODE_XMIT_MASK;
		cfg |= GLB_AUDIO_MODE_XMIT_I2S;
	} else if (mode == I2S && dir == CAPTURE) {
		cfg &= ~GLB_AUDIO_MODE_RECV_MASK;
		cfg |= GLB_AUDIO_MODE_RECV_I2S;
	}
	writel(cfg, adss_audio_local_base + ADSS_GLB_AUDIO_MODE_REG);
	spin_unlock_irqrestore(&glb_mode_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_audio_mode);

void ipq_glb_tx_data_port_en(u32 enable)
{
	u32 cfg;
	unsigned long flags;
	u32 reg = ipq_cfgs->txd_oe.reg;
	u32 val = ipq_cfgs->txd_oe.mask;

	spin_lock_irqsave(&glb_mode_lock, flags);
	cfg = readl(adss_audio_local_base + reg);
	cfg &= ~val;
	if (enable)
		cfg |= val;
	writel(cfg, adss_audio_local_base + reg);
	spin_unlock_irqrestore(&glb_mode_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_tx_data_port_en);

void ipq_glb_rx_data_port_en(u32 enable)
{
	u32 cfg;
	unsigned long flags;
	u32 reg = ipq_cfgs->rxd_oe.reg;
	u32 val = ipq_cfgs->rxd_oe.mask;

	spin_lock_irqsave(&glb_mode_lock, flags);
	cfg = readl(adss_audio_local_base + reg);
	cfg &= ~val;
	if (enable)
		cfg |= val;
	writel(cfg, adss_audio_local_base + reg);
	spin_unlock_irqrestore(&glb_mode_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_rx_data_port_en);

void ipq_glb_audio_mode_b1k(void)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&glb_mode_lock, flags);
	cfg = readl(adss_audio_local_base + ADSS_GLB_AUDIO_MODE_REG);
	cfg |= GLB_AUDIO_MODE_B1K;
	writel(cfg, adss_audio_local_base + ADSS_GLB_AUDIO_MODE_REG);
	spin_unlock_irqrestore(&glb_mode_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_audio_mode_b1k);

void ipq_glb_tx_framesync_port_en(u32 enable)
{
	u32 cfg;
	unsigned long flags;
	u32 reg = ipq_cfgs->i2s0_fs_oe.reg;
	u32 val = ipq_cfgs->i2s0_fs_oe.mask;

	spin_lock_irqsave(&glb_mode_lock, flags);
	cfg = readl(adss_audio_local_base + reg);
	cfg &= ~val;
	if (enable)
		cfg |= val;
	writel(cfg, adss_audio_local_base + reg);
	spin_unlock_irqrestore(&glb_mode_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_tx_framesync_port_en);

void ipq_glb_rx_framesync_port_en(u32 enable)
{
	u32 cfg;
	unsigned long flags;
	u32 reg = ipq_cfgs->i2s3_fs_oe.reg;
	u32 val = ipq_cfgs->i2s3_fs_oe.mask;

	spin_lock_irqsave(&glb_mode_lock, flags);
	cfg = readl(adss_audio_local_base + reg);
	cfg &= ~val;
	if (enable)
		cfg |= val;
	writel(cfg, adss_audio_local_base + reg);
	spin_unlock_irqrestore(&glb_mode_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_rx_framesync_port_en);

void ipq_glb_clk_enable_oe(u32 dir)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&i2s_ctrl_lock, flags);
	cfg = readl(adss_audio_local_base + ADSS_GLB_CLK_I2S_CTRL_REG);

	if (dir == PLAYBACK) {
		cfg |= (GLB_CLK_I2S_CTRL_TX_BCLK_OE |
			GLB_CLK_I2S_CTRL_TX_MCLK_OE);
	} else {
		cfg |= (GLB_CLK_I2S_CTRL_RX_BCLK_OE |
			GLB_CLK_I2S_CTRL_RX_MCLK_OE);
	}
	writel(cfg, adss_audio_local_base + ADSS_GLB_CLK_I2S_CTRL_REG);
	spin_unlock_irqrestore(&i2s_ctrl_lock, flags);
}
EXPORT_SYMBOL(ipq_glb_clk_enable_oe);

void ipq_audio_adss_init(void)
{
	ipq_glb_i2s_reset();
	ipq_glb_i2s_interface_en(ENABLE);
	ipq_glb_audio_mode_b1k();
}
EXPORT_SYMBOL(ipq_audio_adss_init);

static const struct of_device_id ipq_audio_adss_id_table[] = {
	{ .compatible = "qca,ipq4019-audio-adss", },
	{},
};
MODULE_DEVICE_TABLE(of, ipq_audio_adss_id_table);

static int ipq_audio_adss_probe(struct platform_device *pdev)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	adss_audio_local_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(adss_audio_local_base))
		return PTR_ERR(adss_audio_local_base);

	audio_blk_rst = devm_reset_control_get(&pdev->dev, "blk_rst");
	if (IS_ERR(audio_blk_rst))
		return PTR_ERR(audio_blk_rst);

	return 0;
}

static void ipq_audio_adss_remove(struct platform_device *pdev)
{
	ipq_glb_i2s_interface_en(DISABLE);
}

static struct platform_driver ipq_audio_adss_driver = {
	.probe = ipq_audio_adss_probe,
	.remove_new = ipq_audio_adss_remove,
	.driver = {
		.name = "ipq-adss",
		.of_match_table = ipq_audio_adss_id_table,
	},
};

module_platform_driver(ipq_audio_adss_driver);

MODULE_ALIAS("platform:ipq-adss");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("IPQ4019 Audio Subsystem Driver");
