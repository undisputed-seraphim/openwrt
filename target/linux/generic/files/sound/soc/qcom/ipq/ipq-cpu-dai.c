/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

#include "ipq-mbox.h"
#include "ipq-adss.h"

struct dai_priv_st {
	int stereo_tx;
	int stereo_rx;
	int mbox_tx;
	int mbox_rx;
	int tx_enabled;
	int rx_enabled;
	struct platform_device *pdev;
};

static struct dai_priv_st dai_priv[MAX_INTF];

static struct clk *audio_tx_bclk;
static struct clk *audio_tx_mclk;
static struct clk *audio_rx_bclk;
static struct clk *audio_rx_mclk;

int ipq_get_stereo_id(struct snd_pcm_substream *substream, int intf)
{
	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		return dai_priv[intf].stereo_tx;
	case SNDRV_PCM_STREAM_CAPTURE:
		return dai_priv[intf].stereo_rx;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(ipq_get_stereo_id);

int ipq_get_mbox_id(struct snd_pcm_substream *substream, int intf)
{
	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		return dai_priv[intf].mbox_tx;
	case SNDRV_PCM_STREAM_CAPTURE:
		return dai_priv[intf].mbox_rx;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(ipq_get_mbox_id);

u32 ipq_get_act_bit_width(u32 bit_width)
{
	switch (bit_width) {
	case SNDRV_PCM_FORMAT_S8:
	case SNDRV_PCM_FORMAT_U8:
		return __BIT_8;
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S16_BE:
	case SNDRV_PCM_FORMAT_U16_LE:
	case SNDRV_PCM_FORMAT_U16_BE:
		return __BIT_16;
	case SNDRV_PCM_FORMAT_S24_3LE:
	case SNDRV_PCM_FORMAT_S24_3BE:
	case SNDRV_PCM_FORMAT_U24_3LE:
	case SNDRV_PCM_FORMAT_U24_3BE:
		return __BIT_32;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_BE:
	case SNDRV_PCM_FORMAT_U24_LE:
	case SNDRV_PCM_FORMAT_U24_BE:
		return __BIT_24;
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S32_BE:
	case SNDRV_PCM_FORMAT_U32_LE:
	case SNDRV_PCM_FORMAT_U32_BE:
		return __BIT_32;
	}
	return __BIT_INVAL;
}
EXPORT_SYMBOL(ipq_get_act_bit_width);

static int ipq_audio_clk_set(struct clk *clk, struct device *dev, u32 val)
{
	int ret;

	if (!clk)
		return 0;

	ret = clk_set_rate(clk, val);
	if (ret) {
		dev_err_ratelimited(dev, "Error in setting clk rate\n");
		return ret;
	}

	ret = clk_prepare_enable(clk);
	if (ret)
		dev_err_ratelimited(dev, "Error in enabling clk\n");

	return ret;
}

static void ipq_audio_clk_disable(struct clk **clk, struct device *dev)
{
	if (!*clk)
		return;
	clk_disable_unprepare(*clk);
}

static int ipq_audio_startup(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	u32 intf = dai->driver->id;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		if (dai_priv[intf].tx_enabled != ENABLE)
			return -EFAULT;

		ipq_glb_tx_data_port_en(ENABLE);
		ipq_glb_tx_framesync_port_en(ENABLE);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		if (dai_priv[intf].rx_enabled != ENABLE)
			return -EFAULT;

		ipq_glb_rx_data_port_en(ENABLE);
		ipq_glb_rx_framesync_port_en(ENABLE);
		break;
	default:
		return -EINVAL;
	}

	ipq_glb_audio_mode(I2S, substream->stream);

	return 0;
}

static int ipq_audio_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	u32 bit_width, channels, rate;
	u32 intf = dai->driver->id;
	u32 stereo_id = ipq_get_stereo_id(substream, intf);
	u32 mbox_id = ipq_get_mbox_id(substream, intf);
	u32 bit_act;
	int ret;
	u32 mclk, bclk;
	struct device *dev = &dai_priv[intf].pdev->dev;

	bit_width = params_format(params);
	channels = params_channels(params);
	rate = params_rate(params);
	bit_act = ipq_get_act_bit_width(bit_width);

	bclk = rate * bit_act * channels;
	mclk = bclk * MCLK_MULTI;

	ipq_glb_clk_enable_oe(substream->stream);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ipq_config_master(ENABLE, stereo_id);
	else
		ipq_config_master(DISABLE, stereo_id);

	ret = ipq_cfg_bit_width(bit_width, stereo_id);
	if (ret) {
		pr_err("%s: BitWidth %d not supported ret: %d\n",
		       __func__, bit_width, ret);
		return ret;
	}

	ipq_stereo_config_enable(DISABLE, stereo_id);
	ipq_stereo_config_reset(ENABLE, stereo_id);
	ipq_stereo_config_mic_reset(ENABLE, stereo_id);
	mdelay(5);

	ret = ipq_mbox_fifo_reset(mbox_id);
	if (ret) {
		pr_err("%s: ret: %d Error in dma fifo reset\n", __func__, ret);
		return ret;
	}

	ipq_stereo_config_reset(DISABLE, stereo_id);
	ipq_stereo_config_mic_reset(DISABLE, stereo_id);

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		ret = ipq_audio_clk_set(audio_tx_mclk, dev, mclk);
		if (ret)
			return ret;
		ret = ipq_audio_clk_set(audio_tx_bclk, dev, bclk);
		if (ret)
			return ret;
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		ret = ipq_audio_clk_set(audio_rx_mclk, dev, mclk);
		if (ret)
			return ret;
		ret = ipq_audio_clk_set(audio_rx_bclk, dev, bclk);
		if (ret)
			return ret;
		break;
	}

	return 0;
}

static void ipq_audio_shutdown(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	u32 intf = dai->driver->id;
	struct device *dev = &dai_priv[intf].pdev->dev;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		ipq_glb_tx_data_port_en(DISABLE);
		ipq_glb_tx_framesync_port_en(DISABLE);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		ipq_glb_rx_data_port_en(DISABLE);
		ipq_glb_rx_framesync_port_en(DISABLE);
		ipq_audio_clk_disable(&audio_rx_bclk, dev);
		ipq_audio_clk_disable(&audio_rx_mclk, dev);
		break;
	}
}

static struct snd_soc_dai_ops ipq_audio_ops = {
	.startup	= ipq_audio_startup,
	.hw_params	= ipq_audio_hw_params,
	.shutdown	= ipq_audio_shutdown,
};

static struct snd_soc_dai_driver ipq4019_cpu_dais[] = {
	{
		.playback = {
			.rates		= RATE_16000_96000,
			.formats	= SNDRV_PCM_FMTBIT_S16 |
					  SNDRV_PCM_FMTBIT_S32,
			.channels_min	= CH_STEREO,
			.channels_max	= CH_STEREO,
			.rate_min	= FREQ_16000,
			.rate_max	= FREQ_96000,
		},
		.capture = {
			.rates		= RATE_16000_96000,
			.formats	= SNDRV_PCM_FMTBIT_S16 |
					  SNDRV_PCM_FMTBIT_S32,
			.channels_min	= CH_STEREO,
			.channels_max	= CH_STEREO,
			.rate_min	= FREQ_16000,
			.rate_max	= FREQ_96000,
		},
		.ops = &ipq_audio_ops,
		.id = I2S,
		.name = "qca-i2s-dai",
	},
};

static const struct snd_soc_component_driver ipq_i2s_component = {
	.name = "qca-cpu-dai",
};

static struct ipq_intf_pdata ipq4019_i2s_pdata = {
	.data = I2S,
	.hw = IPQ4019,
};

static const struct of_device_id ipq_cpu_dai_id_table[] = {
	{ .compatible = "qca,ipq4019-i2s", .data = &ipq4019_i2s_pdata },
	{},
};
MODULE_DEVICE_TABLE(of, ipq_cpu_dai_id_table);

static int ipq_dai_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct device_node *np = pdev->dev.of_node;
	struct ipq_intf_pdata *pdata;
	int ret, intf;

	match = of_match_device(ipq_cpu_dai_id_table, &pdev->dev);
	if (!match)
		return -ENODEV;

	pdata = (struct ipq_intf_pdata *)match->data;
	intf = pdata->data;

	if (!(of_property_read_u32(np, "dma-tx-channel",
				   &dai_priv[intf].mbox_tx)
	    || of_property_read_u32(np, "stereo-tx-port",
				    &dai_priv[intf].stereo_tx)))
		dai_priv[intf].tx_enabled = ENABLE;

	if (!of_property_read_u32(np, "dma-rx-channel",
				  &dai_priv[intf].mbox_rx)
	    && !of_property_read_u32(np, "stereo-rx-port",
				     &dai_priv[intf].stereo_rx))
		dai_priv[intf].rx_enabled = ENABLE;

	if (!(dai_priv[intf].tx_enabled || dai_priv[intf].rx_enabled)) {
		dev_err(&pdev->dev, "%s: error reading node properties\n",
			np->name);
		return -EFAULT;
	}

	audio_tx_mclk = devm_clk_get(&pdev->dev, "audio_tx_mclk");
	if (IS_ERR(audio_tx_mclk)) {
		dev_dbg(&pdev->dev,
			"tx_mclk not available, ADCC driver may be missing\n");
		audio_tx_mclk = NULL;
	}

	audio_tx_bclk = devm_clk_get(&pdev->dev, "audio_tx_bclk");
	if (IS_ERR(audio_tx_bclk)) {
		dev_dbg(&pdev->dev,
			"tx_bclk not available, ADCC driver may be missing\n");
		audio_tx_bclk = NULL;
	}

	audio_rx_mclk = devm_clk_get(&pdev->dev, "audio_rx_mclk");
	if (IS_ERR(audio_rx_mclk)) {
		dev_dbg(&pdev->dev,
			"rx_mclk not available, ADCC driver may be missing\n");
		audio_rx_mclk = NULL;
	}

	audio_rx_bclk = devm_clk_get(&pdev->dev, "audio_rx_bclk");
	if (IS_ERR(audio_rx_bclk)) {
		dev_dbg(&pdev->dev,
			"rx_bclk not available, ADCC driver may be missing\n");
		audio_rx_bclk = NULL;
	}

	dai_priv[intf].pdev = pdev;

	ret = snd_soc_register_component(&pdev->dev, &ipq_i2s_component,
			ipq4019_cpu_dais, ARRAY_SIZE(ipq4019_cpu_dais));
	if (ret)
		dev_err(&pdev->dev,
			"ret: %d error registering soc dais\n", ret);

	return ret;
}

static void ipq_dai_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
}

static struct platform_driver ipq_dai_driver = {
	.probe = ipq_dai_probe,
	.remove_new = ipq_dai_remove,
	.driver = {
		.name = "qca-cpu-dai",
		.of_match_table = ipq_cpu_dai_id_table,
	},
};

module_platform_driver(ipq_dai_driver);

MODULE_ALIAS("platform:qca-cpu-dai");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("IPQ4019 CPU DAI Driver");
