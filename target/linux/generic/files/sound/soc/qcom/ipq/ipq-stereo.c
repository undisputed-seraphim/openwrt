/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

#include "ipq-adss.h"

struct stereo_priv_data {
	void __iomem *stereo_base;
	spinlock_t stereo_lock;
};

static struct stereo_priv_data stereo_priv[MAX_STEREO_ENTRIES];

void ipq_stereo_config_reset(u32 reset, u32 stereo_id)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&stereo_priv[stereo_id].stereo_lock, flags);
	cfg = readl(stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	cfg &= ~STEREOn_CONFIG_RESET;
	if (reset)
		cfg |= STEREOn_CONFIG_RESET;
	writel(cfg, stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	spin_unlock_irqrestore(&stereo_priv[stereo_id].stereo_lock, flags);
}
EXPORT_SYMBOL(ipq_stereo_config_reset);

void ipq_stereo_config_mic_reset(u32 reset, u32 stereo_id)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&stereo_priv[stereo_id].stereo_lock, flags);
	cfg = readl(stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	cfg &= ~STEREOn_CONFIG_MIC_RESET;
	if (reset)
		cfg |= STEREOn_CONFIG_MIC_RESET;
	writel(cfg, stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	spin_unlock_irqrestore(&stereo_priv[stereo_id].stereo_lock, flags);
}
EXPORT_SYMBOL(ipq_stereo_config_mic_reset);

void ipq_stereo_config_enable(u32 enable, u32 stereo_id)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&stereo_priv[stereo_id].stereo_lock, flags);
	cfg = readl(stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	cfg &= ~STEREOn_CONFIG_ENABLE;
	if (enable)
		cfg |= STEREOn_CONFIG_ENABLE;
	writel(cfg, stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	spin_unlock_irqrestore(&stereo_priv[stereo_id].stereo_lock, flags);
}
EXPORT_SYMBOL(ipq_stereo_config_enable);

int ipq_cfg_bit_width(u32 bit_width, u32 stereo_id)
{
	u32 cfg, mask = 0;
	unsigned long flags;

	switch (bit_width) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S16_BE:
		mask |= (STEREOn_CONFIG_DATA_WORD_SIZE(1) |
			STEREOn_CONFIG_I2S_WORD_SIZE_16 |
			STEREOn_CONFIG_MIC_WORD_SIZE_16);
		break;
	case SNDRV_PCM_FORMAT_S24_3LE:
	case SNDRV_PCM_FORMAT_S24_3BE:
		mask |= (STEREOn_CONFIG_DATA_WORD_SIZE(2) |
			STEREOn_CONFIG_I2S_WORD_SIZE_32 |
			STEREOn_CONFIG_MIC_WORD_SIZE_16);
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S32_BE:
		mask |= (STEREOn_CONFIG_DATA_WORD_SIZE(3) |
			STEREOn_CONFIG_I2S_WORD_SIZE_32 |
			STEREOn_CONFIG_MIC_WORD_SIZE_32);
		break;
	default:
		return -ENOTSUPP;
	}

	spin_lock_irqsave(&stereo_priv[stereo_id].stereo_lock, flags);
	cfg = readl(stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	cfg &= ~STEREOn_CONFIG_DATA_WORD_SIZE_MASK;
	cfg &= ~STEREOn_CONFIG_I2S_WORD_SIZE_32;
	cfg &= ~STEREOn_CONFIG_MIC_WORD_SIZE_32;
	cfg |= mask;
	writel(cfg, stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	spin_unlock_irqrestore(&stereo_priv[stereo_id].stereo_lock, flags);

	return 0;
}
EXPORT_SYMBOL(ipq_cfg_bit_width);

void ipq_config_master(u32 enable, u32 stereo_id)
{
	u32 cfg;
	unsigned long flags;

	spin_lock_irqsave(&stereo_priv[stereo_id].stereo_lock, flags);
	cfg = readl(stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	cfg &= ~STEREOn_CONFIG_MASTER;
	if (enable)
		cfg |= STEREOn_CONFIG_MASTER;
	writel(cfg, stereo_priv[stereo_id].stereo_base
			+ ADSS_STEREOn_STEREO0_CONFIG_REG);
	spin_unlock_irqrestore(&stereo_priv[stereo_id].stereo_lock, flags);
}
EXPORT_SYMBOL(ipq_config_master);

static const struct of_device_id ipq_audio_stereo_id_table[] = {
	{ .compatible = "qca,ipq4019-stereo" },
	{},
};
MODULE_DEVICE_TABLE(of, ipq_audio_stereo_id_table);

static int ipq_audio_stereo_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct stereo_priv_data *spd;
	struct device_node *np = pdev->dev.of_node;
	u32 stereo_port_id = 0;

	if (of_property_read_u32(np, "stereo-index", &stereo_port_id)) {
		dev_err(&pdev->dev, "Error reading stereo-index\n");
		return -EINVAL;
	}
	if (stereo_port_id >= MAX_STEREO_ENTRIES)
		return -ENODEV;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spd = &stereo_priv[stereo_port_id];
	spd->stereo_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(spd->stereo_base))
		return PTR_ERR(spd->stereo_base);

	spin_lock_init(&spd->stereo_lock);
	return 0;
}

static struct platform_driver ipq_audio_stereo_driver = {
	.probe = ipq_audio_stereo_probe,
	.driver = {
		.name = "ipq-stereo",
		.of_match_table = ipq_audio_stereo_id_table,
	},
};

module_platform_driver(ipq_audio_stereo_driver);

MODULE_ALIAS("platform:ipq-stereo");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("IPQ4019 Audio Stereo Driver");
