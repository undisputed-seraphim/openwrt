/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#ifndef __IPQ_PCM_H__
#define __IPQ_PCM_H__

#include "ipq-mbox.h"

struct ipq_pcm_rt_priv {
	int channel;
	struct device *dev;
	unsigned int processed_size;
	u32 period_size;
	u32 buffer_size;
	u32 curr_pos;
	int mmap_flag;
	u32 dma_started;
	struct ipq_mbox_desc *last_played;
};

#endif /* __IPQ_PCM_H__ */
