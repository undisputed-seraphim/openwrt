/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <asm/dma.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/pcm_params.h>

#include "ipq-adss.h"
#include "ipq-pcm.h"

static struct snd_pcm_hardware ipq_pcm_hardware_playback = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_BLOCK_TRANSFER |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_PAUSE |
				  SNDRV_PCM_INFO_RESUME,
	.formats		= SNDRV_PCM_FMTBIT_S16 |
				  SNDRV_PCM_FMTBIT_S32,
	.rates			= RATE_16000_96000,
	.rate_min		= FREQ_16000,
	.rate_max		= FREQ_96000,
	.channels_min		= CH_STEREO,
	.channels_max		= CH_STEREO,
	.buffer_bytes_max	= IPQ_I2S_BUFF_SIZE,
	.period_bytes_max	= IPQ_I2S_BUFF_SIZE / 2,
	.period_bytes_min	= IPQ_I2S_PERIOD_BYTES_MIN,
	.periods_min		= IPQ_I2S_NO_OF_PERIODS,
	.periods_max		= IPQ_I2S_NO_OF_PERIODS,
	.fifo_size		= 0,
};

static struct snd_pcm_hardware ipq_pcm_hardware_capture = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_BLOCK_TRANSFER |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16 |
				  SNDRV_PCM_FMTBIT_S32,
	.rates			= RATE_16000_96000,
	.rate_min		= FREQ_16000,
	.rate_max		= FREQ_96000,
	.channels_min		= CH_STEREO,
	.channels_max		= CH_STEREO,
	.buffer_bytes_max	= IPQ_I2S_BUFF_SIZE,
	.period_bytes_max	= IPQ_I2S_BUFF_SIZE / 2,
	.period_bytes_min	= IPQ_I2S_PERIOD_BYTES_MIN,
	.periods_min		= IPQ_I2S_NO_OF_PERIODS,
	.periods_max		= IPQ_I2S_NO_OF_PERIODS,
	.fifo_size		= 0,
};

static struct device *ss2dev(struct snd_pcm_substream *substream)
{
	return substream->pcm->card->dev;
}

static size_t ipq_dma_buffer_size(struct snd_pcm_hardware *pcm_hw)
{
	return pcm_hw->buffer_bytes_max +
		pcm_hw->periods_min * sizeof(struct ipq_mbox_desc);
}

static bool ipq_mbox_buf_is_aligned(void *c_ptr, ssize_t size)
{
	u32 ptr = (u32)(uintptr_t)c_ptr;

	return (ptr & 0xF0000000) == (((u32)(uintptr_t)c_ptr + size - 1) & 0xF0000000);
}

static int ipq_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_pcm_hardware *pcm_hw = NULL;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size;
	u8 *area;
	dma_addr_t addr;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		pcm_hw = &ipq_pcm_hardware_playback;
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		pcm_hw = &ipq_pcm_hardware_capture;
		break;
	default:
		dev_err(ss2dev(substream), "Invalid stream: %d\n",
			substream->stream);
		return -EINVAL;
	}

	size = ipq_dma_buffer_size(pcm_hw);
	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;

	area = dma_alloc_coherent(pcm->card->dev, size, &addr, GFP_KERNEL);
	if (!area)
		return -ENOMEM;

	if (!ipq_mbox_buf_is_aligned(area, size)) {
		dev_info(ss2dev(substream),
			"First allocation %p not within 256M region\n", area);
		buf->area = dma_alloc_coherent(pcm->card->dev, size,
					       &buf->addr, GFP_KERNEL);
		dma_free_coherent(pcm->card->dev, size, area, addr);
		if (!buf->area)
			return -ENOMEM;
	} else {
		buf->area = area;
		buf->addr = addr;
	}

	buf->bytes = pcm_hw->buffer_bytes_max;

	return 0;
}

static void ipq_pcm_free_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	size_t size;
	struct snd_pcm_hardware *pcm_hw = NULL;

	substream = pcm->streams[stream].substream;
	buf = &substream->dma_buffer;

	switch (stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		pcm_hw = &ipq_pcm_hardware_playback;
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		pcm_hw = &ipq_pcm_hardware_capture;
		break;
	default:
		return;
	}

	size = ipq_dma_buffer_size(pcm_hw);
	dma_free_coherent(pcm->card->dev, size, buf->area, buf->addr);
	buf->addr = 0;
	buf->area = NULL;
}

static irqreturn_t ipq_pcm_irq(int intrsrc, void *data)
{
	struct snd_pcm_substream *substream = data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;

	if (pcm_rtpriv->mmap_flag)
		pcm_rtpriv->curr_pos =
			ipq_mbox_get_played_offset_set_own(pcm_rtpriv->channel);
	else
		pcm_rtpriv->curr_pos =
			ipq_mbox_get_played_offset(pcm_rtpriv->channel);

	snd_pcm_period_elapsed(substream);

	return IRQ_HANDLED;
}

static snd_pcm_uframes_t ipq_pcm_i2s_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;

	return bytes_to_frames(runtime, pcm_rtpriv->curr_pos);
}

static int ipq_pcm_i2s_copy(struct snd_pcm_substream *substream, int chan,
			    unsigned long pos, struct iov_iter *iter,
			    unsigned long bytes)
{
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	char *hwbuf;
	u32 period_size, i, no_of_descs;
	int offset = pos;

	period_size = pcm_rtpriv->period_size;
	hwbuf = buf->area + offset;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (bytes % period_size)
			memset(hwbuf + bytes, 0, period_size - (bytes % period_size));
		if (copy_from_iter(hwbuf, bytes, iter) != bytes)
			return -EFAULT;
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (copy_to_iter(hwbuf, bytes, iter) != bytes)
			return -EFAULT;
	}

	no_of_descs = (bytes + (period_size - 1)) / period_size;

	for (i = 0; i < no_of_descs; i++) {
		ipq_mbox_desc_own(pcm_rtpriv->channel, offset / period_size, 1);
		offset += period_size;
	}

	if (pcm_rtpriv->dma_started)
		ipq_mbox_dma_resume(pcm_rtpriv->channel);

	return 0;
}

static int ipq_pcm_i2s_mmap(struct snd_pcm_substream *substream,
			    struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;

	pcm_rtpriv->mmap_flag = 1;

	return dma_mmap_coherent(substream->pcm->card->dev, vma,
		runtime->dma_area, runtime->dma_addr, runtime->dma_bytes);
}

static int ipq_pcm_hw_free(struct snd_pcm_substream *substream)
{
	snd_pcm_set_runtime_buffer(substream, NULL);
	return 0;
}

static int ipq_pcm_i2s_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	int ret;

	ret = ipq_mbox_form_ring(pcm_rtpriv->channel,
		substream->dma_buffer.addr,
		substream->dma_buffer.area,
		pcm_rtpriv->period_size,
		pcm_rtpriv->buffer_size,
		(substream->stream == SNDRV_PCM_STREAM_CAPTURE));
	if (ret) {
		dev_dbg(ss2dev(substream),
			"Error dma form ring ret: %d\n", ret);
		return ret;
	}

	ret = ipq_mbox_dma_prepare(pcm_rtpriv->channel);
	if (ret) {
		dev_err(ss2dev(substream),
			"Error in dma prepare: channel: %d ret: %d\n",
			pcm_rtpriv->channel, ret);
		return ret;
	}

	if (pcm_rtpriv->mmap_flag == 1)
		ipq_mbox_get_elapsed_size(pcm_rtpriv->channel);
	pcm_rtpriv->last_played = NULL;

	return 0;
}

static int ipq_pcm_i2s_close(struct snd_pcm_substream *substream)
{
	struct ipq_pcm_rt_priv *pcm_rtpriv = substream->runtime->private_data;
	int ret;

	pcm_rtpriv->mmap_flag = 0;
	pcm_rtpriv->last_played = NULL;

	ret = ipq_mbox_dma_release(pcm_rtpriv->channel);
	if (ret)
		dev_err(ss2dev(substream),
			"Error in dma release. ret: %d\n", ret);

	kfree(pcm_rtpriv);

	return 0;
}

static int ipq_pcm_i2s_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *dai = snd_soc_rtd_to_cpu(rtd, 0);
	u32 intf = dai->driver->id;
	u32 desc_duration;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		ipq_stereo_config_enable(ENABLE,
			ipq_get_stereo_id(substream, intf));
		ret = ipq_mbox_dma_start(pcm_rtpriv->channel);
		if (ret)
			dev_err(ss2dev(substream),
				"Error in dma start. ret: %d\n", ret);
		pcm_rtpriv->dma_started = 1;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = ipq_mbox_dma_resume(pcm_rtpriv->channel);
		if (ret)
			dev_err(ss2dev(substream),
				"Error in dma resume. ret: %d\n", ret);
		pcm_rtpriv->dma_started = 1;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ipq_stereo_config_enable(DISABLE,
			ipq_get_stereo_id(substream, intf));
		fallthrough;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		desc_duration =
			frames_to_bytes(runtime, runtime->period_size) * 1000 /
			(runtime->rate *
			 DIV_ROUND_UP(runtime->sample_bits, 8) *
			 runtime->channels);

		ret = ipq_mbox_dma_stop(pcm_rtpriv->channel, desc_duration);
		if (ret)
			dev_err(ss2dev(substream),
				"Error in dma stop. ret: %d\n", ret);
		pcm_rtpriv->dma_started = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int ipq_pcm_i2s_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv = runtime->private_data;

	pcm_rtpriv->period_size = params_period_bytes(hw_params);
	pcm_rtpriv->buffer_size = params_buffer_bytes(hw_params);

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	runtime->dma_bytes = params_buffer_bytes(hw_params);
	return 0;
}

static int ipq_pcm_i2s_open(struct snd_pcm_substream *substream)
{
	int ret;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq_pcm_rt_priv *pcm_rtpriv;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *dai = snd_soc_rtd_to_cpu(rtd, 0);
	u32 intf = dai->driver->id;

	pcm_rtpriv = kmalloc(sizeof(*pcm_rtpriv), GFP_KERNEL);
	if (!pcm_rtpriv)
		return -ENOMEM;

	pcm_rtpriv->dev = substream->pcm->card->dev;
	pcm_rtpriv->channel = ipq_get_mbox_id(substream, intf);
	pcm_rtpriv->curr_pos = 0;
	pcm_rtpriv->mmap_flag = 0;
	pcm_rtpriv->dma_started = 0;
	substream->runtime->private_data = pcm_rtpriv;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		runtime->dma_bytes = ipq_pcm_hardware_playback.buffer_bytes_max;
		snd_soc_set_runtime_hwparams(substream,
			&ipq_pcm_hardware_playback);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		runtime->dma_bytes = ipq_pcm_hardware_capture.buffer_bytes_max;
		snd_soc_set_runtime_hwparams(substream,
			&ipq_pcm_hardware_capture);
		break;
	default:
		dev_err(ss2dev(substream), "Invalid stream: %d\n",
			substream->stream);
		ret = -EINVAL;
		goto error;
	}

	ret = ipq_mbox_dma_init(pcm_rtpriv->dev,
		pcm_rtpriv->channel, ipq_pcm_irq, substream);
	if (ret) {
		dev_err(ss2dev(substream),
			"Error initializing dma. ret: %d\n", ret);
		goto error;
	}

	ret = snd_pcm_hw_constraint_integer(runtime,
		SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto error_hw_const;

	return 0;

error_hw_const:
	ipq_mbox_dma_deinit(pcm_rtpriv->channel);
error:
	kfree(pcm_rtpriv);
	return ret;
}

static const struct snd_pcm_ops ipq_asoc_pcm_i2s_ops = {
	.open		= ipq_pcm_i2s_open,
	.hw_params	= ipq_pcm_i2s_hw_params,
	.hw_free	= ipq_pcm_hw_free,
	.trigger	= ipq_pcm_i2s_trigger,
	.ioctl		= snd_pcm_lib_ioctl,
	.close		= ipq_pcm_i2s_close,
	.prepare	= ipq_pcm_i2s_prepare,
	.mmap		= ipq_pcm_i2s_mmap,
	.pointer	= ipq_pcm_i2s_pointer,
	.copy		= ipq_pcm_i2s_copy,
};

static void ipq_asoc_pcm_i2s_free(struct snd_soc_component *component,
				   struct snd_pcm *pcm)
{
	ipq_pcm_free_dma_buffer(pcm, SNDRV_PCM_STREAM_PLAYBACK);
	ipq_pcm_free_dma_buffer(pcm, SNDRV_PCM_STREAM_CAPTURE);
}

static int ipq_asoc_pcm_i2s_new(struct snd_soc_component *component,
				 struct snd_soc_pcm_runtime *prtd)
{
	struct snd_card *card = prtd->card->snd_card;
	struct snd_pcm *pcm = prtd->pcm;
	int ret = 0, pback = 0;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ipq_asoc_pcm_i2s_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ipq_asoc_pcm_i2s_ops);

	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &card->dev->coherent_dma_mask;

	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		ret = ipq_pcm_preallocate_dma_buffer(pcm,
			SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			return -ENOMEM;
		pback = 1;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		ret = ipq_pcm_preallocate_dma_buffer(pcm,
			SNDRV_PCM_STREAM_CAPTURE);
		if (ret) {
			if (pback)
				ipq_pcm_free_dma_buffer(pcm,
					SNDRV_PCM_STREAM_PLAYBACK);
			return -ENOMEM;
		}
	}

	return ret;
}

static const struct snd_soc_component_driver ipq_asoc_pcm_i2s_platform = {
	.pcm_construct	= ipq_asoc_pcm_i2s_new,
	.pcm_destruct	= ipq_asoc_pcm_i2s_free,
};

static const struct of_device_id ipq_pcm_i2s_id_table[] = {
	{ .compatible = "qca,ipq4019-pcm-i2s" },
	{},
};
MODULE_DEVICE_TABLE(of, ipq_pcm_i2s_id_table);

static int ipq_pcm_i2s_driver_probe(struct platform_device *pdev)
{
	return devm_snd_soc_register_component(&pdev->dev,
		&ipq_asoc_pcm_i2s_platform, NULL, 0);
}

static void ipq_pcm_i2s_driver_remove(struct platform_device *pdev)
{
}

static struct platform_driver ipq_pcm_i2s_driver = {
	.probe = ipq_pcm_i2s_driver_probe,
	.remove_new = ipq_pcm_i2s_driver_remove,
	.driver = {
		.name = "qca-pcm-i2s",
		.owner = THIS_MODULE,
		.of_match_table = ipq_pcm_i2s_id_table,
	},
};

module_platform_driver(ipq_pcm_i2s_driver);

MODULE_ALIAS("platform:qca-pcm-i2s");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("IPQ4019 PCM I2S Platform Driver");
