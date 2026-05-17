/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#ifndef __IPQ_MBOX_H__
#define __IPQ_MBOX_H__

#include <sound/soc.h>

#define ADSS_MBOX_NR_CHANNELS		5
#define ADSS_MBOX_INVALID_PCM		0xFFFFFFFF

#define MBOX_MIN_DESC_NUM	10

struct ipq_mbox_desc {
	unsigned int	length	:12,
			size	:12,
			vuc	:1,
			ei	:1,
			rsvd1	:4,
			EOM	:1,
			OWN	:1;
	unsigned int	BufPtr;
	unsigned int	NextPtr;
	unsigned int	vuc_dword[36];
};

#define IPQ_MBOX_IRQ_ACK(status, mask)	(status & ~mask)

struct ipq_mbox_rt_dir_priv {
	struct ipq_mbox_desc *dma_virt_head;
	dma_addr_t dma_phys_head;
	struct device *dev;
	unsigned int ndescs;
	irq_handler_t callback;
	void *dai_priv;
	unsigned long status;
	u32 channel_id;
	u32 err_stats;
	u32 last_played_is_null;
	u32 write;
	u32 read;
};

struct ipq_mbox_rt_priv {
	int irq_no;
	void __iomem *mbox_reg_base;
	struct ipq_mbox_rt_dir_priv dir_priv[2];
	int mbox_started;
};

int ipq_mbox_fifo_reset(int channel_id);
int ipq_mbox_dma_start(int channel_id);
int ipq_mbox_dma_stop(int channel_id, u32 delay_in_ms);
int ipq_mbox_dma_swap(int channel_id, snd_pcm_format_t format);
int ipq_mbox_dma_prepare(int channel_id);
int ipq_mbox_dma_resume(int channel_id);
int ipq_mbox_form_ring(int channel_id, dma_addr_t baseaddr, u8 *area,
		       int period_bytes, int bufsize, int own_bit);
int ipq_mbox_dma_release(int channel_id);
int ipq_mbox_dma_init(struct device *dev, int channel_id,
		      irq_handler_t callback, void *private_data);
int ipq_mbox_dma_deinit(u32 channel_id);
void ipq_mbox_desc_own(u32 channel_id, int desc_no, int own);
u32 ipq_mbox_get_played_offset(u32 channel_id);
u32 ipq_mbox_get_played_offset_set_own(u32 channel_id);
u32 ipq_mbox_get_elapsed_size(u32 channel_id);
struct ipq_mbox_desc *ipq_mbox_get_last_played(unsigned int channel_id);

static inline u32 ipq_convert_id_to_channel(u32 id)
{
	return id / 2;
}

static inline u32 ipq_convert_id_to_dir(u32 id)
{
	return id % 2;
}

static inline int ipq_get_mbox_descs_duplicate(int ndescs)
{
	int repeat_cnt;

	if (ndescs < MBOX_MIN_DESC_NUM) {
		repeat_cnt = MBOX_MIN_DESC_NUM / ndescs;
		if (MBOX_MIN_DESC_NUM % ndescs)
			repeat_cnt++;
		ndescs *= repeat_cnt;
	}

	return ndescs;
}

#endif /* __IPQ_MBOX_H__ */
