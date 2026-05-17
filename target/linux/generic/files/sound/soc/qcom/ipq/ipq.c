/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/pinctrl/consumer.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>

#include "ipq-adss.h"

SND_SOC_DAILINK_DEFS(media1,
	DAILINK_COMP_ARRAY(COMP_CPU("qca-i2s-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("rt5628.1-0018", "qca-i2s-codec-dai")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("7709000.qca-pcm-i2s")));

SND_SOC_DAILINK_DEFS(media2,
	DAILINK_COMP_ARRAY(COMP_CPU("qca-i2s-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("cx2092x.1-0041", "qca-i2s-conexant-dai")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("7709000.qca-pcm-i2s")));

static struct snd_soc_dai_link ipq4019_snd_dai[] = {
	{
		.name		= "IPQ Media1",
		.stream_name	= "I2S",
		.dai_fmt	= SND_SOC_DAIFMT_I2S |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBS_CFS,
		SND_SOC_DAILINK_REG(media1),
	},
	{
		.name		= "IPQ Media2",
		.stream_name	= "I2S",
		.dai_fmt	= SND_SOC_DAIFMT_I2S |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBS_CFS,
		SND_SOC_DAILINK_REG(media2),
	},
};

static struct snd_soc_card snd_soc_card_ipq4019 = {
	.name		= "ipq4019_snd_card",
	.dai_link	= ipq4019_snd_dai,
	.num_links	= ARRAY_SIZE(ipq4019_snd_dai),
};

static const struct of_device_id ipq_audio_id_table[] = {
	{ .compatible = "qca,ipq4019-audio", .data = &snd_soc_card_ipq4019 },
	{},
};
MODULE_DEVICE_TABLE(of, ipq_audio_id_table);

static int ipq_audio_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct snd_soc_card *card;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_state;
	int ret;

	match = of_match_device(ipq_audio_id_table, &pdev->dev);
	if (!match)
		return -ENODEV;

	card = (struct snd_soc_card *)match->data;
	card->dev = &pdev->dev;

	pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(&pdev->dev, PTR_ERR(pinctrl),
				     "failed to get pinctrl\n");

	pin_state = pinctrl_lookup_state(pinctrl, "audio");
	if (IS_ERR(pin_state))
		return dev_err_probe(&pdev->dev, PTR_ERR(pin_state),
				     "audio pinctrl state not available\n");

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "snd_soc_register_card() failed\n");

	ipq_audio_adss_init();

	ret = pinctrl_select_state(pinctrl, pin_state);
	if (ret)
		dev_err(&pdev->dev, "failed to select audio pinctrl: %d\n", ret);

	return ret;
}

static struct platform_driver ipq_audio_driver = {
	.probe = ipq_audio_probe,
	.driver = {
		.name = "ipq_audio",
		.of_match_table = ipq_audio_id_table,
	},
};

module_platform_driver(ipq_audio_driver);

MODULE_ALIAS("platform:ipq_audio");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("IPQ4019 Audio Machine Driver");
