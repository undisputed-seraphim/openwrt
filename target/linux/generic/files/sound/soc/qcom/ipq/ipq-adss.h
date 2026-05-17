/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#ifndef __IPQ_ADSS_H__
#define __IPQ_ADSS_H__

#include <sound/pcm.h>

/* ADSS_AUDIO_LOCAL_REG Registers at ADSS_BASE + 0x0 */
#define ADSS_BASE			0x7700000

#define ADSS_GLB_PCM_RST_REG		0x0
#define GLB_PCM_RST_CTRL(x)		((x) << 0)

#define ADSS_GLB_CHIP_CTRL_I2S_REG	0x10
#define GLB_CHIP_CTRL_I2S_INTERFACE_EN		BIT(0)
#define GLB_CHIP_CTRL_I2S_STEREO0_GLB_EN	BIT(1)
#define GLB_CHIP_CTRL_I2S_STEREO1_GLB_EN	BIT(2)
#define GLB_CHIP_CTRL_I2S_STEREO2_GLB_EN	BIT(3)

#define ADSS_GLB_I2S_RST_REG		0x14
#define GLB_I2S_RST_CTRL_MBOX0			BIT(0)
#define GLB_I2S_RST_CTRL_I2S0			BIT(1)
#define GLB_I2S_RST_CTRL_MBOX3			BIT(2)
#define GLB_I2S_RESET_VAL_4019		0xF
#define GLB_I2S_RST_MBOX_RESET_MASK		0x5

#define ADSS_GLB_CLK_I2S_CTRL_REG	0x18
#define GLB_CLK_I2S_CTRL_TX_BCLK_OE		BIT(28)
#define GLB_CLK_I2S_CTRL_RX_BCLK_OE		BIT(27)
#define GLB_CLK_I2S_CTRL_RX_MCLK_OE		BIT(16)
#define GLB_CLK_I2S_CTRL_TX_MCLK_OE		BIT(17)

#define ADSS_GLB_AUDIO_MODE_REG		0x30
#define GLB_AUDIO_MODE_RECV_MASK		BIT(2)
#define GLB_AUDIO_MODE_XMIT_MASK		BIT(0)
#define GLB_AUDIO_MODE_RECV_I2S			(0 << 2)
#define GLB_AUDIO_MODE_XMIT_I2S			(0 << 0)
#define GLB_AUDIO_MODE_RECV_TDM			BIT(2)
#define GLB_AUDIO_MODE_XMIT_TDM			BIT(0)
#define GLB_AUDIO_MODE_I2S0_TXD_OE		(7 << 4)
#define GLB_AUDIO_MODE_I2S0_FS_OE		BIT(7)
#define GLB_AUDIO_MODE_I2S3_FS_OE		BIT(8)
#define GLB_AUDIO_MODE_I2S3_RXD_OE		BIT(9)
#define GLB_AUDIO_MODE_B1K			BIT(28)

/* ADSS_MBOX_STEREO_AUDIO Registers at ADSS_BASE + 0x8000 */
#define ADSS_MBOX0_AUDIO_BASE		0x0
#define ADSS_MBOX1_AUDIO_BASE		0x2000
#define ADSS_MBOX2_AUDIO_BASE		0x4000
#define ADSS_MBOX3_AUDIO_BASE		0x6000

#define ADSS_MBOXn_MBOX_FIFO0_REG		0x0
#define MBOX_FIFO_RESET_TX_INIT			BIT(0)
#define MBOX_FIFO_RESET_RX_INIT			BIT(2)

#define ADSS_MBOXn_MBOX_DMA_POLICY_REG		0x10
#define MBOX_DMA_POLICY_SW_RESET		BIT(31)
#define MBOX_DMA_POLICY_TX_INT_TYPE		BIT(17)
#define MBOX_DMA_POLICY_RX_INT_TYPE		BIT(16)
#define MBOX_DMA_POLICY_RXD_16BIT_SWAP		BIT(10)
#define MBOX_DMA_POLICY_RXD_END_SWAP		BIT(8)
#define ADSS_MBOX_DMA_POLICY_SRAM_AC(x)		((((x) >> 28) & 0xf) << 12)
#define ADSS_MBOX_DMA_POLICY_TX_FIFO_THRESHOLD(x) ((((x) & 0xf) << 4))

#define ADSS_MBOXn_MBOXn_DMA_RX_DESCRIPTOR_BASE_REG	0x18
#define ADSS_MBOXn_MBOXn_DMA_RX_CONTROL_REG		0x1C
#define ADSS_MBOXn_DMA_RX_CONTROL_STOP			BIT(0)
#define ADSS_MBOXn_DMA_RX_CONTROL_START			BIT(1)
#define ADSS_MBOXn_DMA_RX_CONTROL_RESUME		BIT(2)

#define ADSS_MBOXn_MBOXn_DMA_TX_DESCRIPTOR_BASE_REG	0x20
#define ADSS_MBOXn_MBOXn_DMA_TX_CONTROL_REG		0x24
#define ADSS_MBOXn_DMA_TX_CONTROL_STOP			BIT(0)
#define ADSS_MBOXn_DMA_TX_CONTROL_START			BIT(1)
#define ADSS_MBOXn_DMA_TX_CONTROL_RESUME		BIT(2)

#define ADSS_MBOXn_MBOX_INT_STATUS_REG		0x44
#define MBOX_INT_STATUS_TX_DMA_COMPLETE		BIT(6)
#define MBOX_INT_STATUS_RX_DMA_COMPLETE		BIT(10)

#define ADSS_MBOXn_MBOX_INT_ENABLE_REG		0x4C
#define MBOX_INT_ENABLE_RX_DMA_COMPLETE		BIT(10)
#define MBOX_INT_ENABLE_TX_DMA_COMPLETE		BIT(6)
#define MBOX_INT_STATUS_RX_UNDERFLOW		BIT(4)
#define MBOX_INT_STATUS_RX_FIFO_UNDERFLOW	BIT(12)
#define MBOX_INT_STATUS_TX_OVERFLOW		BIT(5)
#define MBOX_INT_STATUS_TX_FIFO_OVERFLOW	BIT(13)

#define ADSS_MBOXn_MBOX_FIFO_RESET_REG		0x58

/* ADSS_STEREO_AUDIO Registers at ADSS_BASE + 0x9000/0xB000/0xD000/0xF000 */
#define ADSS_STEREO0_AUDIO_BASE		0x9000
#define ADSS_STEREO1_AUDIO_BASE		0xB000
#define ADSS_STEREO2_AUDIO_BASE		0xD000
#define ADSS_STEREO3_AUDIO_BASE		0xF000

#define ADSS_STEREOn_STEREO0_CONFIG_REG		0x0
#define STEREOn_CONFIG_MIC_SWAP			BIT(24)
#define STEREOn_CONFIG_SPDIF_ENABLE		BIT(23)
#define STEREOn_CONFIG_ENABLE			BIT(21)
#define STEREOn_CONFIG_MIC_RESET		BIT(20)
#define STEREOn_CONFIG_RESET			BIT(19)
#define STEREOn_CONFIG_I2S_DELAY		(0 << 18)
#define STEREOn_CONFIG_MIC_WORD_SIZE_32		BIT(16)
#define STEREOn_CONFIG_MIC_WORD_SIZE_16		(0 << 16)
#define STEREOn_CONFIG_STEREO_MODE		(0 << 14)
#define STEREOn_CONFIG_MONO_MODE		BIT(14)
#define STEREOn_CONFIG_STEREO_MONO_MASK		(3 << 14)
#define STEREOn_CONFIG_DATA_WORD_SIZE(x)	((x) << 12)
#define STEREOn_CONFIG_DATA_WORD_SIZE_MASK	(3 << 12)
#define STEREOn_CONFIG_I2S_WORD_SIZE_32		BIT(11)
#define STEREOn_CONFIG_I2S_WORD_SIZE_16		(0 << 11)
#define STEREOn_CONFIG_MCK_SEL			BIT(10)
#define STEREOn_CONFIG_SAMPLE_CNT_CLEAR_TYPE	BIT(9)
#define STEREOn_CONFIG_MASTER			BIT(8)

#define MAX_STEREO_ENTRIES	4
#define MCLK_MULTI		4

/* I2S Parameters */
#define IPQ_I2S_NO_OF_PERIODS		130
#define IPQ_I2S_PERIOD_BYTES_MIN	4032
#define IPQ_I2S_BUFF_SIZE		(IPQ_I2S_PERIOD_BYTES_MIN * \
						IPQ_I2S_NO_OF_PERIODS)
#define IPQ_I2S_CAPTURE_BUFF_SIZE	(IPQ_I2S_PERIOD_BYTES_MIN * \
						IPQ_I2S_NO_OF_PERIODS)

#define SNDRV_PCM_FMTBIT_S24_3	SNDRV_PCM_FMTBIT_S24_3LE

enum ipq_hw_type {
	IPQ4019,
};

enum ipq_intf {
	I2S,
	MAX_INTF
};

enum ipq_dir {
	PLAYBACK,
	CAPTURE
};

enum ipq_cfg {
	DISABLE,
	ENABLE
};

enum ipq_channels {
	CH_STEREO = 2,
};

enum ipq_samp_freq {
	FREQ_8000 = 8000,
	FREQ_11025 = 11025,
	FREQ_16000 = 16000,
	FREQ_22050 = 22050,
	FREQ_32000 = 32000,
	FREQ_44100 = 44100,
	FREQ_48000 = 48000,
	FREQ_64000 = 64000,
	FREQ_88200 = 88200,
	FREQ_96000 = 96000,
};

#define RATE_16000_96000 \
		(SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 | \
		SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | \
		SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_64000 | \
		SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000)

enum ipq_stereo_ch {
	STEREO0,
	STEREO1,
	STEREO2,
	STEREO3
};

enum ipq_bit_width {
	__BIT_8 = 8,
	__BIT_16 = 16,
	__BIT_24 = 24,
	__BIT_32 = 32,
	__BIT_INVAL = -1
};

struct ipq_intf_pdata {
	u32 data;
	u32 hw;
};

struct ipq_regs_arr {
	u32 reg;
	u32 mask;
};

struct ipq_configs {
	struct ipq_regs_arr txd_oe;
	struct ipq_regs_arr rxd_oe;
	struct ipq_regs_arr i2s0_fs_oe;
	struct ipq_regs_arr i2s3_fs_oe;
	struct ipq_regs_arr i2s_reset_val;
	u32 spdif_enable;
};

/* ADSS APIs */
void ipq_glb_audio_mode(int mode, int dir);
void ipq_glb_tx_data_port_en(u32 enable);
void ipq_glb_rx_data_port_en(u32 enable);
void ipq_glb_tx_framesync_port_en(u32 enable);
void ipq_glb_rx_framesync_port_en(u32 enable);
void ipq_glb_clk_enable_oe(u32 dir);
void ipq_glb_mbox_reset(void);
void ipq_glb_i2s_interface_en(int enable);
void ipq_glb_audio_mode_b1k(void);
void ipq_audio_adss_init(void);

void ipq_audio_adss_writel(u32 val, u32 offset);
u32 ipq_audio_adss_readl(u32 offset);

/* Stereo APIs */
void ipq_stereo_config_reset(u32 reset, u32 stereo_offset);
void ipq_stereo_config_mic_reset(u32 reset, u32 stereo_offset);
void ipq_stereo_config_enable(u32 enable, u32 stereo_offset);
int ipq_cfg_bit_width(u32 bit_width, u32 stereo_offset);
void ipq_config_master(u32 enable, u32 stereo_offset);

/* CPU DAI APIs */
int ipq_get_mbox_id(struct snd_pcm_substream *substream, int intf);
int ipq_get_stereo_id(struct snd_pcm_substream *substream, int intf);
u32 ipq_get_act_bit_width(u32 bit_width);

#endif /* __IPQ_ADSS_H__ */
