/*
 * rt5628.c  --  RT5628 ALSA SoC audio codec driver
 *
 * Copyright 2014 Realtek Semiconductor Corp.
 * Author: Bard Liao <bardliao@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/jiffies.h>
#include <asm/delay.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include "rt5628.h"
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/devinfo.h>

#define I2S 0

struct rt5628_priv {
	struct regmap *regmap;
	unsigned int sysclk;
	int shutdown_gpio;
	int mute_gpio;
	int amp_status;
};

struct rt5628_priv *g_rt5628 = NULL;

/*
 *   define a virtual reg for misc func
 *   bit0: hpl pga dapm control
 *   bit1: hpr pga dapm control
 */

#define VIRTUAL_REG_FOR_MISC_FUNC 0x84
#if 1
static const struct reg_default rt5628_reg[] = {
	{0x00, 0x0003},
	{0x04, 0x0000},
	{0x0c, 0x5252},
	{0x16, 0x000A},
	{0x18, 0x0000},
	{0x1c, 0x8304},
	{0x34, 0x8000},
	{0x38, 0x2000},
	{0x3a, 0xC130},
	{0x3c, 0x7FF8},
	{0x3e, 0xF6F0},
	{0x40, 0x0100},
	{0x42, 0xC000},
	{0x44, 0x12B0},
	{0x5a, 0x0000},
	{0x5c, 0x0000},
	{0x5e, 0x0000},
	{0x6a, 0x0000},
	{0x6c, 0x0000},
	{0x7c, 0x10ec},
	{0x7e, 0x2700},
	{0x84, 0x0000},
};
#else
static const struct reg_default rt5628_reg[] = {
	{0x00, 0x0003},
	{0x02, 0x9f9f},
	{0x04, 0x9f9f},
	{0x08, 0xc8c8},
	{0x0a, 0xc8c8},
	{0x0c, 0xffff},
	{0x16, 0x0009},
	{0x1c, 0x8004},
	{0x34, 0x8000},
	{0x38, 0x2000},
	{0x3a, 0x0000},
	{0x3c, 0x0000},
	{0x3e, 0x0000},
	{0x40, 0x0100},
	{0x42, 0x0000},
	{0x44, 0x0000},
	{0x5a, 0x0000},
	{0x5c, 0x0000},
	{0x5e, 0x0000},
	{0x6a, 0x0000},
	{0x6c, 0x0000},
	{0x68, 0x100b},
	{0x7c, 0x10ec},
	{0x7e, 0x2700},
	{0x84, 0x0000},
};
#endif
static bool rt5628_volatile_register(struct device *dev,  unsigned int reg)
{
	switch (reg) {
	case RT5628_RESET:
#if 0
	case RT5628_JACK_DET_CTRL:
#endif
	case RT5628_HID_CTRL_INDEX:
	case RT5628_HID_CTRL_DATA:
	case RT5628_VENDOR_ID1:
	case RT5628_VENDOR_ID2:
		return true;
	default:
		return false;
	}
}

static bool rt5628_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RT5628_RESET:
	case RT5628_HP_OUT_VOL:
	case RT5628_STEREO_DAC_VOL:
	case RT5628_SOFT_VOL_CTRL_TIME:
	case RT5628_OUTPUT_MIXER_CTRL:
	case RT5628_AUDIO_DATA_CTRL:
	case RT5628_DAC_CLK_CTRL:
	case RT5628_PWR_MANAG_ADD1:
	case RT5628_PWR_MANAG_ADD2:
	case RT5628_PWR_MANAG_ADD3:
	case RT5628_GEN_CTRL:
	case RT5628_GLOBAL_CLK_CTRL:
	case RT5628_PLL_CTRL:
	case RT5628_GPIO_PIN_CONFIG:
	case RT5628_GPIO_OUTPUT_PIN_CTRL:
	case RT5628_MISC1_CTRL:
	case RT5628_MISC2_CTRL:
	case RT5628_AVC_CTRL:
	case RT5628_HID_CTRL_INDEX:
	case RT5628_HID_CTRL_DATA:
	case RT5628_VENDOR_ID1:
	case RT5628_VENDOR_ID2:
	case VIRTUAL_REG_FOR_MISC_FUNC:
#if 0
	case RT5628_SPK_OUT_VOL:
	case RT5628_AUXIN_VOL:
	case RT5628_LINE_IN_VOL:
	case RT5628_JACK_DET_CTRL:
#endif
		return true;
	default:
		return false;
	}
}

static struct reg_default init_data[] = {
	{RT5628_RESET,0x0003},
	{RT5628_HP_OUT_VOL 	,0x0000},
	{RT5628_STEREO_DAC_VOL,0x5252},//default stereo DAC volume to 0db
	{RT5628_SOFT_VOL_CTRL_TIME,0x000A},//default 32VSYNC
	{RT5628_OUTPUT_MIXER_CTRL   ,0x8304},//default output mixer control
#if 0
	{RT5628_GEN_CTRL		,0x0b00},//set Class D Vmid ratio is 1VDD and DAC have high pass filter
	{RT5628_AUDIO_DATA_CTRL		,0x8009},//set I2S codec to slave mode
#endif
	{RT5628_PWR_MANAG_ADD1,0xC130},
	{RT5628_PWR_MANAG_ADD2,0x7FF8},
	{RT5628_PWR_MANAG_ADD3,0xF6F0},
#if 0
	{RT5628_SPK_OUT_VOL		,0x8080},//default speaker volume to 0db
	{RT5628_HP_OUT_VOL		,0x8888},//default HP volume to -12db
#endif
	{RT5628_GLOBAL_CLK_CTRL,0xC000},
	{RT5628_PLL_CTRL,0x12B0},
};
#define RT5628_INIT_REG_NUM ARRAY_SIZE(init_data)

static int rt5628_init_reg(struct snd_soc_component *codec)
{
	struct rt5628_priv *rt5628 = snd_soc_component_get_drvdata(codec);
	int i;

	for (i = 0; i < RT5628_INIT_REG_NUM; i++)
	{
		regmap_write(rt5628->regmap, init_data[i].reg, init_data[i].def);
	}

	return 0;
}

/*
 *    RT5628 Controls
 */
static const char *rt5628_spk_l_source_sel[] = {"LPRN", "LPRP", "LPLN", "MM"};		/*0*/
static const char *rt5628_spk_source_sel[] = {"VMID", "HP Mixer", "Speaker Mixer"};	/*1*/
static const char *rt5628_hpl_source_sel[] = {"VMID", "HP Left Mixer"};				/*2*/
static const char *rt5628_hpr_source_sel[] = {"VMID", "HP Right Mixer"};			/*3*/
static const char *rt5628_classd_ratio_sel[] = {"2.25 VDD", "2.00 VDD", "1.75 VDD", "1.5 VDD"
					"1.25 VDD", "1 VDD"};					/*4*/
static const char *rt5628_direct_out_sel[] = {"Normal", "Direct Out"};				/*5*/

static struct soc_enum rt5628_enum[] = {

SOC_ENUM_SINGLE(RT5628_OUTPUT_MIXER_CTRL, 14, 4,rt5628_spk_l_source_sel),			/*0*/
SOC_ENUM_SINGLE(RT5628_OUTPUT_MIXER_CTRL, 10, 3, rt5628_spk_source_sel),			/*1*/
SOC_ENUM_SINGLE(RT5628_OUTPUT_MIXER_CTRL, 9, 2, rt5628_hpl_source_sel),				/*2*/
SOC_ENUM_SINGLE(RT5628_OUTPUT_MIXER_CTRL, 8, 2, rt5628_hpr_source_sel), 			/*3*/
SOC_ENUM_SINGLE(RT5628_GEN_CTRL, 9, 6, rt5628_classd_ratio_sel),					/*4*/
SOC_ENUM_SINGLE(RT5628_OUTPUT_MIXER_CTRL, 1, 2, rt5628_direct_out_sel), 			/*5*/
};

#define RT5628_HWEQ(xname) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = rt5628_hweq_info, \
	.access = (SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		SNDRV_CTL_ELEM_ACCESS_READWRITE), \
	.get = rt5628_hweq_get, \
	.put = rt5628_hweq_put \
}

static const struct snd_kcontrol_new rt5628_snd_controls[] = {
#if 0
SOC_ENUM("Classd AMP Ratio", rt5628_enum[4]),
SOC_ENUM("SPKL Source", rt5628_enum[0]),
SOC_DOUBLE("SPK Playback Volume", RT5628_SPK_OUT_VOL, 8, 0, 31, 1),
SOC_DOUBLE("SPK Playback Switch", RT5628_SPK_OUT_VOL, 15, 7, 1, 1),
#endif
SOC_DOUBLE("HP Playback Volume", RT5628_HP_OUT_VOL, 8, 0, 31, 1),
SOC_DOUBLE("HP Playback Switch", RT5628_HP_OUT_VOL, 15, 7, 1, 1),
#if 0
SOC_DOUBLE("AUXIN Playback Volume", RT5628_AUXIN_VOL, 8, 0, 31, 1),
SOC_DOUBLE("LINEIN Playback Volume", RT5628_LINE_IN_VOL, 8, 0, 31, 1),
#endif
SOC_DOUBLE("PCMIN Playback Volume", RT5628_STEREO_DAC_VOL, 8, 0, 63, 1),
};

static void hp_depop_mode2(struct snd_soc_component *codec)
{
	struct rt5628_priv *rt5628 = snd_soc_component_get_drvdata(codec);

	pr_err("[%s]\r\n", __func__);
#if 1
	regmap_update_bits(rt5628->regmap, 0x3e, 0x8000, 0x8000);
	regmap_update_bits(rt5628->regmap, 0x04, 0x8080, 0x8080);
	regmap_update_bits(rt5628->regmap, 0x3a, 0x0130, 0x0130);
	//regmap_update_bits(rt5628->regmap, 0x3c, 0x2000, 0x2000);
	regmap_update_bits(rt5628->regmap, 0x3c, 0x7FF8, 0x7FF8);
	regmap_update_bits(rt5628->regmap, 0x3e, 0x0600, 0x0600);
	regmap_update_bits(rt5628->regmap, 0x5e, 0x03E0, 0x03E0);
#endif
	schedule_timeout_uninterruptible(msecs_to_jiffies(300));
}

static int hp_pga_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *codec = snd_soc_dapm_to_component(w->dapm);
	int ret = 0;
	u32 reg;
	struct rt5628_priv *rt5628 = NULL;
	rt5628 = g_rt5628;

	ret = regmap_read(codec->regmap, VIRTUAL_REG_FOR_MISC_FUNC, &reg) & 0x3;
	if (reg != 0x3 && reg != 0)
		return 0;

	switch (event)
	{
		case SND_SOC_DAPM_POST_PMD:
			if (rt5628->amp_status == 0)
				return 0;

			pr_err("[%s] SND_SOC_DAPM_POST_PMD\r\n", __func__);
			regmap_update_bits(codec->regmap, 0x3a, 0x0030, 0x0000);
			regmap_update_bits(codec->regmap, 0x04, 0x8080, 0x8080);
#if 0
			snd_soc_update_bits(codec, 0x3e, 0x0600, 0x0000);
#endif
			gpio_direction_output(rt5628->shutdown_gpio, 0);
			gpio_direction_output(rt5628->mute_gpio, 1);

			rt5628->amp_status = 0;

			break;
		case SND_SOC_DAPM_POST_PMU:
			if (rt5628->amp_status == 1)
				return 0;

			pr_err("[%s] SND_SOC_DAPM_POST_PMU\r\n", __func__);
			hp_depop_mode2(codec);
#if 1
			regmap_update_bits(codec->regmap ,0x04, 0x8080, 0x0000 );
#endif
			gpio_direction_output(rt5628->shutdown_gpio, 1);
			gpio_direction_output(rt5628->mute_gpio, 0);

			rt5628->amp_status = 1;

			break;
		default:
			return 0;
	}
	return 0;
}

/*
 * _DAPM_ Controls
 */
/*left hp mixer*/
static const struct snd_kcontrol_new rt5628_left_hp_mixer_controls[] = {

#if 0
SOC_DAPM_SINGLE("Left Linein Playback Switch", RT5628_LINE_IN_VOL, 15, 1, 1),
SOC_DAPM_SINGLE("Left Auxin Playback Switch", RT5628_AUXIN_VOL, 15, 1, 1),
#endif
SOC_DAPM_SINGLE("Left PCM Playback Switch", RT5628_STEREO_DAC_VOL, 15, 1, 1),

};

/*right hp mixer*/
static const struct snd_kcontrol_new rt5628_right_hp_mixer_controls[] = {

#if 0
SOC_DAPM_SINGLE("Right Linein Playback Switch", RT5628_LINE_IN_VOL, 7, 1, 1),
SOC_DAPM_SINGLE("Right Auxin Playback Switch", RT5628_AUXIN_VOL, 7, 1, 1),
#endif
SOC_DAPM_SINGLE("Right PCM Playback Switch", RT5628_STEREO_DAC_VOL, 7, 1, 1),

};


/*spk mixer*/
static const struct snd_kcontrol_new rt5628_spk_mixer_controls[] = {

#if 0
SOC_DAPM_SINGLE("Left Linein Playback Switch", RT5628_LINE_IN_VOL, 14, 1, 1),
SOC_DAPM_SINGLE("Right Linein Playback Switch", RT5628_LINE_IN_VOL, 6, 1, 1),
SOC_DAPM_SINGLE("Left Auxin Playback Switch", RT5628_AUXIN_VOL, 14, 1, 1),
SOC_DAPM_SINGLE("Right Auxin Playback Switch", RT5628_AUXIN_VOL, 6, 1, 1),
#endif
SOC_DAPM_SINGLE("Left PCM Playback Switch", RT5628_STEREO_DAC_VOL, 14, 1, 1),
SOC_DAPM_SINGLE("Right PCM Playback Switch", RT5628_STEREO_DAC_VOL, 6, 1, 1),

};

/*SPK Mux Out*/
#if 0
static const struct snd_kcontrol_new rt5628_spk_mux_out_controls =
SOC_DAPM_ENUM("Route", rt5628_enum[1]);
#endif

/*HPL Mux Out*/
static const struct snd_kcontrol_new rt5628_hpl_mux_out_controls =
SOC_DAPM_ENUM("Route", rt5628_enum[2]);

/*HPR Mux Out*/
static const struct snd_kcontrol_new rt5628_hpr_mux_out_controls =
SOC_DAPM_ENUM("Route", rt5628_enum[3]);

static const struct snd_soc_dapm_widget rt5628_dapm_widgets[] = {
/*Path before Hp mixer*/
SND_SOC_DAPM_INPUT("Left Line In"),
SND_SOC_DAPM_INPUT("Right Line In"),
SND_SOC_DAPM_INPUT("Left Auxin"),
SND_SOC_DAPM_INPUT("Right Auxin"),
//SND_SOC_DAPM_AIF_IN("AIF1RX", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX", "qca-i2s-playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_DAC("Left DAC", NULL, RT5628_PWR_MANAG_ADD2, 9, 0),
SND_SOC_DAPM_DAC("Right DAC", NULL, RT5628_PWR_MANAG_ADD2, 8, 0),
#if 0
SND_SOC_DAPM_PGA("Left Linein PGA", RT5628_PWR_MANAG_ADD3, 7, 0, NULL, 0),
SND_SOC_DAPM_PGA("Right Linein PGA", RT5628_PWR_MANAG_ADD3, 6, 0, NULL, 0),
SND_SOC_DAPM_PGA("Left Auxin PGA", RT5628_PWR_MANAG_ADD3, 5, 0, NULL, 0),
SND_SOC_DAPM_PGA("Right Auxin PGA", RT5628_PWR_MANAG_ADD3, 4, 0, NULL, 0),
#endif
SND_SOC_DAPM_MIXER("Left HP Mixer", RT5628_PWR_MANAG_ADD2, 5, 0,
	&rt5628_left_hp_mixer_controls[0], ARRAY_SIZE(rt5628_left_hp_mixer_controls)),
SND_SOC_DAPM_MIXER("Right HP Mixer", RT5628_PWR_MANAG_ADD2, 4, 0,
	&rt5628_right_hp_mixer_controls[0], ARRAY_SIZE(rt5628_right_hp_mixer_controls)),
#if 0
SND_SOC_DAPM_MIXER("SPK Mixer", RT5628_PWR_MANAG_ADD2, 3, 0,
	&rt5628_spk_mixer_controls[0], ARRAY_SIZE(rt5628_spk_mixer_controls)),
#endif
/*HP mixer -->SPK out*/
SND_SOC_DAPM_MIXER("HP Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
#if 0
SND_SOC_DAPM_MUX("SPK Mux Out", SND_SOC_NOPM, 0, 0, &rt5628_spk_mux_out_controls),
SND_SOC_DAPM_PGA("Speaker", RT5628_PWR_MANAG_ADD3, 12, 0, NULL, 0),
SND_SOC_DAPM_PGA("SPK AMP", RT5628_PWR_MANAG_ADD2, 14, 0, NULL, 0),
SND_SOC_DAPM_OUTPUT("SPK"),
#endif

/*Path before HP Mux out*/
SND_SOC_DAPM_MUX("HPL Mux Out", SND_SOC_NOPM, 0, 0, &rt5628_hpl_mux_out_controls),
SND_SOC_DAPM_MUX("HPR Mux Out", SND_SOC_NOPM, 0, 0, &rt5628_hpr_mux_out_controls),

SND_SOC_DAPM_PGA_E("HPL Out PGA",VIRTUAL_REG_FOR_MISC_FUNC, 0, 0, NULL, 0,
				hp_pga_event, SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_POST_PMU ),
SND_SOC_DAPM_PGA_E("HPR Out PGA",VIRTUAL_REG_FOR_MISC_FUNC, 1, 0, NULL, 0,
				hp_pga_event, SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_POST_PMU ),
/*hp out*/
SND_SOC_DAPM_OUTPUT("HP"),
};

static const struct snd_soc_dapm_route rt5628_dapm_routes[] = {
	{"Left DAC", NULL, "AIF1RX"},
	{"Right DAC", NULL, "AIF1RX"},
	/*line3 input pga*/

	/*line in pga*/
	//{"Left Linein PGA", NULL, "Left Line In"},
	//{"Right Linein PGA", NULL, "Right Line In"},

	//{"Left HP Mixer", NULL, "Left Line In"},
	//{"Right HP Mixer", NULL, "Right Line In"},

	/*aux in pga*/
	//{"Left Auxin PGA", NULL, "Left Auxin"},
	//{"Right Auxin PGA", NULL, "Right Auxin"},

	/*left hp mixer*/

	//{"Left HP Mixer", "Left Linein Playback Switch", "Left Linein PGA"},
	//{"Left HP Mixer", "Left Auxin Playback Switch", "Left Auxin PGA"},
	{"Left HP Mixer", "Left PCM Playback Switch", "Left DAC"},

	/*right hp mixer*/

	//{"Right HP Mixer", "Right Linein Playback Switch", "Right Linein PGA"},
	//{"Right HP Mixer", "Right Auxin Playback Switch", "Right Auxin PGA"},
	{"Right HP Mixer", "Right PCM Playback Switch", "Right DAC"},

#if 0
	/*spk mixer*/
	{"SPK Mixer", "Left Linein Playback Switch", "Left Linein PGA"},
	{"SPK Mixer", "Right Linein Playback Switch", "Right Linein PGA"},
	{"SPK Mixer", "Left Auxin Playback Switch", "Left Auxin PGA"},
	{"SPK Mixer", "Right Auxin Playback Switch", "Right Auxin PGA"},
	{"SPK Mixer", "Left PCM Playback Switch", "Left DAC"},
	{"SPK Mixer", "Right PCM Playback Switch", "Right DAC"},

	/*HP Mixer virtual*/
	{"HP Mixer", NULL, "Left HP Mixer"},
	{"HP Mixer", NULL, "Right HP Mixer"},

	/*SPK Mux Out*/
	{"SPK Mux Out", "HP Mixer", "HP Mixer"},
	{"SPK Mux Out", "Speaker Mixer", "SPK Mixer"},
#endif

	/*HPL Mux Out*/
	{"HPL Mux Out", "HP Left Mixer", "Left HP Mixer"},

	/*HPR Mux Out*/
	{"HPR Mux Out", "HP Right Mixer", "Right HP Mixer"},

#if 0
	/*SPK Out PGA*/
	{"Speaker", NULL, "SPK Mux Out"},
#endif

	/*HPL Out PGA*/
	{"HPL Out PGA", NULL, "HPL Mux Out"},

	/*HPR Out PGA*/
	{"HPR Out PGA", NULL, "HPR Mux Out"},
#if 0
	/*spk*/
	{"SPK AMP", NULL, "Speaker"},
	{"SPK", NULL, "SPK AMP"},
#endif
	/*HP*/
	{"HP", NULL, "HPL Out PGA"},
	{"HP", NULL, "HPR Out PGA"},
};



/*PLL divisors*/
struct _pll_div {
	u32 pll_in;
	u32 pll_out;
	u16 regvalue;
};

static const struct _pll_div codec_master_pll_div[] = {
	{  2048000,  8192000,	0x0ea0},
	{  3686400,  8192000,	0x4e27},
	{ 12000000,  8192000,	0x456b},
	{ 13000000,  8192000,	0x495f},
	{ 13100000,  8192000,	0x0320},
	{  2048000,  11289600,	0xf637},
	{  3686400,  11289600,	0x2f22},
	{ 12000000,  11289600,	0x3e2f},
	{ 13000000,  11289600,	0x4d5b},
	{ 13100000,  11289600,	0x363b},
	{  2048000,  16384000,	0x1ea0},
	{  3686400,  16384000,	0x9e27},
	{ 12000000,  16384000,	0x452b},
	{ 13000000,  16384000,	0x542f},
	{ 13100000,  16384000,	0x03a0},
	{  2048000,  16934400,	0xe625},
	{  3686400,  16934400,	0x9126},
	{ 12000000,  16934400,	0x4d2c},
	{ 13000000,  16934400,	0x742f},
	{ 13100000,  16934400,	0x3c27},
	{  2048000,  22579200,	0x2aa0},
	{  3686400,  22579200,	0x2f20},
	{ 12000000,  22579200,	0x7e2f},
	{ 13000000,  22579200,	0x742f},
	{ 13100000,  22579200,	0x3c27},
	{  2048000,  24576000,	0x2ea0},
	{  3686400,  24576000,	0xee27},
	{ 12000000,  24576000,	0x2915},
	{ 13000000,  24576000,	0x772e},
	{ 13100000,  24576000,	0x0d20},
};

static const struct _pll_div codec_slave_pll_div[] = {
	{  1024000,  16384000,  0x3ea0},
	{  1411200,  22579200,	0x3ea0},
	{  1536000,  24576000,	0x3ea0},
	{  2048000,  16384000,  0x1ea0},
	{  2822400,  22579200,	0x1ea0},
	{  3072000,  24576000,	0x1ea0},
	{   705600,  11289600,	0x3ea0},
	{   705600,  8467200 , 	0x3ab0},
};

struct _coeff_div{
	u32 mclk;
	u32 rate;
	u16 fs;
	u16 regvalue;
};


static const struct _coeff_div coeff_div[] = {
	/* 8k */
	{ 8192000,  8000, 256*4, 0x4000},
	{12288000,  8000, 384*4, 0x4004},

	/* 11.025k */
	{11289600, 11025, 256*4, 0x4000},
	{16934400, 11025, 384*4, 0x4004},

	/* 16k */
	{16384000, 16000, 256*4, 0x4000},
	{24576000, 16000, 384*4, 0x4004},
	/* 22.05k */
	{11289600, 22050, 256*2, 0x2000},
	{16934400, 22050, 384*2, 0x2004},
	{ 8467200, 22050, 384*1, 0x0004},

	/* 32k */
	{16384000, 32000, 256*2, 0x2000},
	{24576000, 32000, 384*2, 0x2004},

	/* 44.1k */
	{22579200, 44100, 256*2, 0x2000},
	/* 48k */
	{24576000, 48000, 256*2, 0x2000},
};

static int get_coeff(int mclk, int rate)
{
	int i;

	printk("get_coeff mclk=%d,rate=%d\n",mclk,rate);


	for (i = 0; i < ARRAY_SIZE(coeff_div); i++) {
		if (coeff_div[i].rate == rate) // && coeff_div[i].mclk == mclk)
			return i;
	}

	return -EINVAL;
}

static int rt5628_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{

	struct snd_soc_component *codec = codec_dai->component;
	int ret = 0;
	u16 iface = 0;

	/*set master/slave interface*/
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK)
	{
		case SND_SOC_DAIFMT_CBM_CFM:
			iface = 0x0000;
			break;
		case SND_SOC_DAIFMT_CBS_CFS:
			iface = 0x8000;
			break;
		default:
			return -EINVAL;
	}

	/*interface format*/
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK)
	{
		case SND_SOC_DAIFMT_I2S:
			iface |= 0x0000;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			iface |= 0x0001;
			break;
		case SND_SOC_DAIFMT_DSP_A:
			iface |= 0x0002;
			break;
		case SND_SOC_DAIFMT_DSP_B:
			iface |= 0x0003;
			break;
		default:
			return -EINVAL;
	}

	/*clock inversion*/
	switch (fmt & SND_SOC_DAIFMT_INV_MASK)
	{
		case SND_SOC_DAIFMT_NB_NF:
			iface |= 0x0000;
			break;
		case SND_SOC_DAIFMT_IB_NF:
			iface |= 0x0080;
			break;
		default:
			return -EINVAL;
	}

	ret = regmap_write(codec->regmap, RT5628_AUDIO_DATA_CTRL, iface);
	return 0;
}

static int rt5628_codec_audio_startup(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	dev_dbg(dai->dev, "%s:%d\n", __func__, __LINE__);

	return 0;
}

static int rt5628_pcm_hw_params(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params,
			struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct rt5628_priv *rt5628 = snd_soc_card_get_drvdata(card);

	int ret = 0;
	u32 iface;

	ret = regmap_read(rt5628->regmap, RT5628_AUDIO_DATA_CTRL, &iface) & 0xfff3;
	int coeff = get_coeff(rt5628->sysclk, params_rate(params));

	dev_dbg(dai->dev, "enter %s\n", __func__);

	switch (params_format(params))
	{
		case SNDRV_PCM_FORMAT_S16_LE:
			break;
		case SNDRV_PCM_FORMAT_S20_3LE:
			iface |= 0x0004;
			break;
		case SNDRV_PCM_FORMAT_S24_LE:
		case SNDRV_PCM_FORMAT_S32_LE:
			iface |= 0x0008;
			break;
	}

	ret = regmap_write(rt5628->regmap, RT5628_AUDIO_DATA_CTRL, iface);

	if (coeff >= 0)
	{
		ret = regmap_write(rt5628->regmap, RT5628_DAC_CLK_CTRL, coeff_div[coeff].regvalue);
	}

	return 0;
}

static int rt5628_set_dai_sysclk(struct snd_soc_dai *codec_dai, int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_component *codec = codec_dai->component;
	struct rt5628_priv *rt5628 = snd_soc_component_get_drvdata(codec);

//	printk(KERN_DEBUG "%s clk_id: %ld, freq: %ld\n", __func__);

	if ((freq >= (256 * 8000)) && (freq <= (512 *48000))) {
		rt5628->sysclk = freq;
		return 0;
	}

	dev_err(codec->dev, "unsupported sysclk freq %d\n", freq);
	return 0;
}



static int rt5628_set_dai_pll(struct snd_soc_dai *codec_dai, int pll_id,int source,unsigned int freq_in, unsigned int freq_out)
{
	int i;
	int ret = -EINVAL;
	struct snd_soc_component *codec = codec_dai->component;

	if (pll_id < RT5628_PLL_FR_MCLK || pll_id > RT5628_PLL_FR_BCLK)
		return -EINVAL;


	if (!freq_in || !freq_out)
	{
		return 0;
	}

	if (RT5628_PLL_FR_MCLK == pll_id)
	{
		for (i = 0; i < ARRAY_SIZE(codec_master_pll_div); i ++)
		{
			if ((freq_in == codec_master_pll_div[i].pll_in) && (freq_out == codec_master_pll_div[i].pll_out))
			{
				ret = regmap_write(codec->regmap, RT5628_GLOBAL_CLK_CTRL, 0x0000);    			/*PLL source from MCLK*/
				ret = regmap_write(codec->regmap, RT5628_PLL_CTRL, codec_master_pll_div[i].regvalue);   	/*set pll code*/
				ret = regmap_update_bits(codec->regmap, RT5628_PWR_MANAG_ADD2, 0x1000, 0x1000);        	/*enable pll power*/
				ret = 0;
			}
		}
	}
	else     /*slave mode*/
	{
		for (i = 0; i < ARRAY_SIZE(codec_slave_pll_div); i ++)
		{
			if ((freq_in == codec_slave_pll_div[i].pll_in) && (freq_out == codec_slave_pll_div[i].pll_out))
			{
				ret = regmap_write(codec->regmap, RT5628_GLOBAL_CLK_CTRL, 0x4000);    			/*PLL source from BCLK*/
				ret = regmap_write(codec->regmap, RT5628_PLL_CTRL, codec_slave_pll_div[i].regvalue);  	 /*set pll code*/
				ret = regmap_update_bits(codec->regmap, RT5628_PWR_MANAG_ADD2, 0x1000, 0x1000);       	 /*enable pll power*/
				ret = 0;
			}
		}
	}

	ret = regmap_update_bits(codec->regmap, RT5628_GLOBAL_CLK_CTRL, 0x8000, 0x8000);

	return ret;
}

static int rt5628_codec_audio_prepare(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	dev_dbg(dai->dev, "%s:%d\n", __func__, __LINE__);
	return 0;
}

static void rt5628_codec_audio_shutdown(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	dev_dbg(dai->dev, "%s:%d\n", __func__, __LINE__);
}

#define RT5628_RATES SNDRV_PCM_RATE_48000

#if 0
#define RT5628_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
	SNDRV_PCM_FMTBIT_S24_LE)
#else
#define RT5628_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE)
#endif

static struct snd_soc_dai_ops rt5628_hifi_ops = {
	.startup	= rt5628_codec_audio_startup,
	.hw_params = rt5628_pcm_hw_params,
	.prepare	= rt5628_codec_audio_prepare,
	.shutdown	= rt5628_codec_audio_shutdown,
	.set_fmt = rt5628_set_dai_fmt,
	.set_sysclk = rt5628_set_dai_sysclk,
	.set_pll = rt5628_set_dai_pll,

};

#if 0
static struct snd_soc_dai_driver rt5628_dai = {
	.name = "rt5628-aif1",
	.playback = {
		.stream_name = "AIF1 Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = RT5628_RATES,
		.formats = RT5628_FORMATS,
	},
	.ops = &rt5628_hifi_ops,
};
#else
static struct snd_soc_dai_driver rt5628_dai = {
	.name = "qca-i2s-codec-dai",
	.playback = {
		.stream_name = "qca-i2s-playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = RT5628_RATES,
		.formats = RT5628_FORMATS,
	},
#if 0
	.capture = {
		.stream_name = "qca-i2s-capture",
		.channels_min = 2, // Mono, Right&Left fill same data
		.channels_max = 2, // Mono, Right&Left fill same data
		.rates = RT5628_RATES,
		.formats = RT5628_FORMATS,
	},
#endif
	.ops = &rt5628_hifi_ops,
	.id = I2S,
};
#endif

static int rt5628_set_bias_level(struct snd_soc_component *codec,
				enum snd_soc_bias_level level)
{
	int ret = 0;

	switch (level) {
	case SND_SOC_BIAS_ON:
	case SND_SOC_BIAS_PREPARE:

		break;
	case SND_SOC_BIAS_STANDBY:
		break;
	case SND_SOC_BIAS_OFF:
		pr_err("[%s] SND_SOC_BIAS_OFF\r\n", __func__);
#if 0
		snd_soc_update_bits(codec, 0x02, 0x8080, 0x8080);
#endif
		ret = regmap_update_bits(codec->regmap, 0x04, 0x8080, 0x8080);
		ret = regmap_update_bits(codec->regmap, 0x3e, 0x8600, 0x0000);
		ret = regmap_update_bits(codec->regmap, 0x3c, 0x7FF8, 0x0000);
		ret = regmap_update_bits(codec->regmap, 0x3a, 0x0100, 0x0000);
		break;

	}
	codec->dapm.bias_level = level;
	return 0;
}

static int rt5628_suspend(struct snd_soc_component *codec)
{
	struct rt5628_priv *rt5628 = snd_soc_component_get_drvdata(codec);

	regcache_cache_only(rt5628->regmap, true);
	regcache_mark_dirty(rt5628->regmap);

	return 0;
}

static int rt5628_resume(struct snd_soc_component *codec)
{
	struct rt5628_priv *rt5628 = snd_soc_component_get_drvdata(codec);

	regcache_cache_only(rt5628->regmap, false);
	regcache_sync(codec->regmap);

	return 0;
}

static int rt5628_probe(struct snd_soc_component *codec)
{
	pr_info("RT5628 Audio Codec probed\n");

	hp_depop_mode2(codec);

	rt5628_init_reg(codec);

	codec->dapm.bias_level = SND_SOC_BIAS_STANDBY;

	rt5628_set_bias_level(codec, SND_SOC_BIAS_OFF);

	return 0;
}

static struct snd_soc_component_driver soc_component_dev_rt5628 = {
	.probe = rt5628_probe,
	.suspend = rt5628_suspend,
	.resume = rt5628_resume,
	.set_bias_level = rt5628_set_bias_level,
	.idle_bias_on = false,
	.controls = rt5628_snd_controls,
	.num_controls = ARRAY_SIZE(rt5628_snd_controls),
	.dapm_widgets = rt5628_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rt5628_dapm_widgets),
	.dapm_routes = rt5628_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(rt5628_dapm_routes),
};

static const struct regmap_config rt5628_regmap = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = VIRTUAL_REG_FOR_MISC_FUNC,
	.use_single_read = true,
	.use_single_write = true,
	.reg_stride = 2,
	.volatile_reg = rt5628_volatile_register,
	.readable_reg = rt5628_readable_register,
	.cache_type = REGCACHE_RBTREE,
	.reg_defaults = rt5628_reg,
	.num_reg_defaults = ARRAY_SIZE(rt5628_reg),
};

#if 1
static ssize_t show_reg(struct device *dev, struct device_attribute *attr, char *buf)
{
	u32 ret1, ret2, ret3, ret4, ret5, ret6, ret7, ret8, ret9, ret10, ret11, ret12;
	u32 ret13, ret14, ret15, ret16, ret17, ret18, ret19, ret20, ret21, ret22, ret23;
	struct rt5628_priv *rt5628 = NULL;

	if(g_rt5628 == NULL)
	{
		return 0;
	}

	rt5628 = g_rt5628;

	regmap_read(rt5628->regmap, RT5628_RESET,                &ret1);
	regmap_read(rt5628->regmap, RT5628_HP_OUT_VOL,           &ret2);
	regmap_read(rt5628->regmap, RT5628_STEREO_DAC_VOL,       &ret3);
	regmap_read(rt5628->regmap, RT5628_SOFT_VOL_CTRL_TIME,   &ret4);
	regmap_read(rt5628->regmap, RT5628_OUTPUT_MIXER_CTRL,    &ret5);
	regmap_read(rt5628->regmap, RT5628_AUDIO_DATA_CTRL,      &ret6);
	regmap_read(rt5628->regmap, RT5628_DAC_CLK_CTRL,         &ret7);
	regmap_read(rt5628->regmap, RT5628_PWR_MANAG_ADD1,       &ret8);
	regmap_read(rt5628->regmap, RT5628_PWR_MANAG_ADD2,       &ret9);
	regmap_read(rt5628->regmap, RT5628_PWR_MANAG_ADD3,       &ret10);
	regmap_read(rt5628->regmap, RT5628_GEN_CTRL,             &ret11);
	regmap_read(rt5628->regmap, RT5628_GLOBAL_CLK_CTRL,      &ret12);
	regmap_read(rt5628->regmap, RT5628_PLL_CTRL,             &ret13);
	regmap_read(rt5628->regmap, RT5628_GPIO_PIN_CONFIG,      &ret14);
	regmap_read(rt5628->regmap, RT5628_GPIO_OUTPUT_PIN_CTRL, &ret15);
	regmap_read(rt5628->regmap, RT5628_MISC1_CTRL,           &ret16);
	regmap_read(rt5628->regmap, RT5628_MISC2_CTRL,           &ret17);
	regmap_read(rt5628->regmap, RT5628_AVC_CTRL,             &ret18);
	regmap_read(rt5628->regmap, RT5628_HID_CTRL_INDEX,       &ret19);
	regmap_read(rt5628->regmap, RT5628_HID_CTRL_DATA,        &ret20);
	regmap_read(rt5628->regmap, RT5628_VENDOR_ID1,           &ret21);
	regmap_read(rt5628->regmap, RT5628_VENDOR_ID2,           &ret22);
	regmap_read(rt5628->regmap, VIRTUAL_REG_FOR_MISC_FUNC,   &ret23);

	return sprintf(buf, "%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n%02x  0x%04x\r\n"
						"%02x  0x%04x\r\n%02x  0x%04x\r\n",
						RT5628_RESET, ret1, RT5628_HP_OUT_VOL, ret2,
						RT5628_STEREO_DAC_VOL, ret3, RT5628_SOFT_VOL_CTRL_TIME, ret4,
						RT5628_OUTPUT_MIXER_CTRL, ret5, RT5628_AUDIO_DATA_CTRL, ret6,
						RT5628_DAC_CLK_CTRL, ret7, RT5628_PWR_MANAG_ADD1, ret8,
						RT5628_PWR_MANAG_ADD2, ret9, RT5628_PWR_MANAG_ADD3, ret10,
						RT5628_GEN_CTRL, ret11,	RT5628_GLOBAL_CLK_CTRL, ret12,
						RT5628_PLL_CTRL, ret13,	RT5628_GPIO_PIN_CONFIG, ret14,
						RT5628_GPIO_OUTPUT_PIN_CTRL, ret15,	RT5628_MISC1_CTRL, ret16,
						RT5628_MISC2_CTRL, ret17, RT5628_AVC_CTRL, ret18,
						RT5628_HID_CTRL_INDEX, ret19, RT5628_HID_CTRL_DATA, ret20,
						RT5628_VENDOR_ID1, ret21, RT5628_VENDOR_ID2, ret22,
						VIRTUAL_REG_FOR_MISC_FUNC, ret23);
}

static ssize_t store_reg(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int reg, value;
	struct rt5628_priv *rt5628 = NULL;

	if(g_rt5628 == NULL)
	{
		return 0;
	}

	rt5628 = g_rt5628;

	sscanf(buf, "0x%x 0x%x", (int *)&reg, (int *)&value);

	pr_err("Set register (0x%x) to value (0x%x)\r\n", reg, value);

	regmap_write(rt5628->regmap, reg, value);

	return count;
}

static DEVICE_ATTR(reg , S_IWUSR | S_IRUGO, show_reg, store_reg);
#endif

static int rt5628_i2c_probe(struct i2c_client *i2c)
{
	struct rt5628_priv *rt5628;
	int ret;
	struct dev_pin_info *pins;
	struct pinctrl_state *pin_state;

	rt5628 = devm_kzalloc(&i2c->dev, sizeof(*rt5628),
				GFP_KERNEL);
	if (rt5628 == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, rt5628);

	rt5628->regmap = devm_regmap_init_i2c(i2c, &rt5628_regmap);
	if (IS_ERR(rt5628->regmap)) {
		ret = PTR_ERR(rt5628->regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	regmap_read(rt5628->regmap, RT5628_VENDOR_ID1, &ret);
	if (ret != 0x10ec) {
		dev_err(&i2c->dev,
			"Device with ID register %x is not rt5628\n", ret);
		return -ENODEV;
	}

	regmap_write(rt5628->regmap, RT5628_RESET, 0);
	regmap_write(rt5628->regmap, RT5628_PWR_MANAG_ADD3, PWR_MAIN_BIAS);
	regmap_write(rt5628->regmap, RT5628_PWR_MANAG_ADD2, PWR_VREF);
	pins = i2c->dev.pins;

	pin_state = pinctrl_lookup_state(pins->p, "default");
	if (IS_ERR(pin_state)) {
		dev_err(&i2c->dev, "speaker pinctrl state not available\n");
		return PTR_ERR(pin_state);
	}

	pinctrl_select_state(pins->p, pin_state);

	rt5628->shutdown_gpio = -1;
	rt5628->shutdown_gpio = of_get_named_gpio(i2c->dev.of_node, "shutdown-gpio", 0);

	if (gpio_is_valid(rt5628->shutdown_gpio)) {
		ret = gpio_request(rt5628->shutdown_gpio, "ALC1304 SHUTDOWN");
		if (ret != 0) {
			dev_err(&i2c->dev, "gpio_request failed, gpio = %d\n", rt5628->shutdown_gpio);
			return -EBUSY;
		}
	}

	gpio_direction_output(rt5628->shutdown_gpio, 0);
	rt5628->mute_gpio = -1;
	rt5628->mute_gpio = of_get_named_gpio(i2c->dev.of_node, "mute-gpio", 0);

	if (gpio_is_valid(rt5628->mute_gpio)) {
		ret = gpio_request(rt5628->mute_gpio, "ALC1304 MUTE");
		if (ret != 0) {
			dev_err(&i2c->dev, "gpio_request failed, gpio = %d\n", rt5628->mute_gpio);
			return -EBUSY;
		}
	}

	gpio_direction_output(rt5628->mute_gpio, 1);

	rt5628->amp_status = 0;

	device_create_file(&i2c->dev, &dev_attr_reg);

	g_rt5628 = rt5628;

	return snd_soc_register_component(&i2c->dev, &soc_component_dev_rt5628,
			&rt5628_dai, 1);
}

static void rt5628_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_component(&client->dev);

	device_remove_file(&client->dev, &dev_attr_reg);

	g_rt5628 = NULL;
}

static const struct of_device_id rt5628_dt_ids[] = {
	{ .compatible = "realtek,rt5628", },
	{ .compatible = "realtek,alc5628", },
	{ }
};

static const struct i2c_device_id rt5628_i2c_id[] = {
	{"rt5628", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, rt5628_i2c_id);

static struct i2c_driver rt5628_i2c_driver = {
	.driver = {
		.name = "rt5628",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(rt5628_dt_ids),
	},
	.id_table = rt5628_i2c_id,
	.probe = rt5628_i2c_probe,
	.remove = rt5628_i2c_remove,
};
module_i2c_driver(rt5628_i2c_driver);

MODULE_DESCRIPTION("ASoC RT5628 driver");
MODULE_AUTHOR("Bard Liao <bardliao@realtek.com>");
MODULE_LICENSE("GPL v2");
