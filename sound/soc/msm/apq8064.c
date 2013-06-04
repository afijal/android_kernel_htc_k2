/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/soc-dsp.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include <asm/mach-types.h>
#include <mach/socinfo.h>
#include "msm-pcm-routing.h"
#include "../codecs/wcd9310.h"

/* 8064 machine driver */

#define PM8921_GPIO_BASE		NR_GPIO_IRQS
#define PM8921_GPIO_PM_TO_SYS(pm_gpio)  (pm_gpio - 1 + PM8921_GPIO_BASE)

#define MSM8064_SPK_ON 1
#define MSM8064_SPK_OFF 0

#define MSM_SLIM_0_RX_MAX_CHANNELS		2
#define MSM_SLIM_0_TX_MAX_CHANNELS		4

#define BTSCO_RATE_8KHZ 8000
#define BTSCO_RATE_16KHZ 16000

#define BOTTOM_SPK_AMP_POS	0x1
#define BOTTOM_SPK_AMP_NEG	0x2
#define TOP_SPK_AMP_POS		0x4
#define TOP_SPK_AMP_NEG		0x8

#define GPIO_AUX_PCM_DOUT 43
#define GPIO_AUX_PCM_DIN 44
#define GPIO_AUX_PCM_SYNC 45
#define GPIO_AUX_PCM_CLK 46

#define TABLA_EXT_CLK_RATE 12288000

#define TABLA_MBHC_DEF_BUTTONS 8
#define TABLA_MBHC_DEF_RLOADS 5

/* Shared channel numbers for Slimbus ports that connect APQ to MDM. */
enum {
	SLIM_1_RX_1 = 145, /* BT-SCO and USB TX */
	SLIM_1_TX_1 = 146, /* BT-SCO and USB RX */
	SLIM_3_RX_1 = 151, /* External echo-cancellation ref */
	SLIM_3_RX_2 = 152, /* External echo-cancellation ref */
	SLIM_3_TX_1 = 147, /* HDMI RX */
	SLIM_4_TX_1 = 148, /* In-call recording RX */
	SLIM_4_TX_2 = 149, /* In-call recording RX */
	SLIM_4_RX_1 = 150, /* In-call music delivery TX */
};

enum {
	INCALL_REC_MONO,
	INCALL_REC_STEREO,
};

static u32 top_spk_pamp_gpio  = PM8921_GPIO_PM_TO_SYS(18);
static u32 bottom_spk_pamp_gpio = PM8921_GPIO_PM_TO_SYS(19);
static int msm_spk_control;
static int msm_ext_bottom_spk_pamp;
static int msm_ext_top_spk_pamp;
static int msm_slim_0_rx_ch = 1;
static int msm_slim_0_tx_ch = 1;
static int msm_slim_3_rx_ch = 1;

static int msm_btsco_rate = BTSCO_RATE_8KHZ;
static int msm_btsco_ch = 1;

static int rec_mode = INCALL_REC_MONO;

static struct clk *codec_clk;
static int clk_users;

static int msm_headset_gpios_configured;

static struct snd_soc_jack hs_jack;
static struct snd_soc_jack button_jack;

static int msm_enable_codec_ext_clk(struct snd_soc_codec *codec, int enable,
				    bool dapm);

static struct tabla_mbhc_config mbhc_cfg = {
	.headset_jack = &hs_jack,
	.button_jack = &button_jack,
	.read_fw_bin = false,
	.calibration = NULL,
	.micbias = TABLA_MICBIAS2,
	.mclk_cb_fn = msm_enable_codec_ext_clk,
	.mclk_rate = TABLA_EXT_CLK_RATE,
	.gpio = 0, /* MBHC GPIO is not configured */
	.gpio_irq = 0,
	.gpio_level_insert = 1,
};

static void msm_enable_ext_spk_amp_gpio(u32 spk_amp_gpio)
{
	int ret = 0;

	struct pm_gpio param = {
		.direction      = PM_GPIO_DIR_OUT,
		.output_buffer  = PM_GPIO_OUT_BUF_CMOS,
		.output_value   = 1,
		.pull      = PM_GPIO_PULL_NO,
		.vin_sel	= PM_GPIO_VIN_S4,
		.out_strength   = PM_GPIO_STRENGTH_MED,
		.
			function       = PM_GPIO_FUNC_NORMAL,
	};

	if (spk_amp_gpio == bottom_spk_pamp_gpio) {

		ret = gpio_request(bottom_spk_pamp_gpio, "BOTTOM_SPK_AMP");
		if (ret) {
			pr_err("%s: Error requesting BOTTOM SPK AMP GPIO %u\n",
				__func__, bottom_spk_pamp_gpio);
			return;
		}
		ret = pm8xxx_gpio_config(bottom_spk_pamp_gpio, &param);
		if (ret)
			pr_err("%s: Failed to configure Bottom Spk Ampl"
				" gpio %u\n", __func__, bottom_spk_pamp_gpio);
		else {
			pr_debug("%s: enable Bottom spkr amp gpio\n", __func__);
			gpio_direction_output(bottom_spk_pamp_gpio, 1);
		}

	} else if (spk_amp_gpio == top_spk_pamp_gpio) {

		ret = gpio_request(top_spk_pamp_gpio, "TOP_SPK_AMP");
		if (ret) {
			pr_err("%s: Error requesting GPIO %d\n", __func__,
				top_spk_pamp_gpio);
			return;
		}
		ret = pm8xxx_gpio_config(top_spk_pamp_gpio, &param);
		if (ret)
			pr_err("%s: Failed to configure Top Spk Ampl"
				" gpio %u\n", __func__, top_spk_pamp_gpio);
		else {
			pr_debug("%s: enable Top spkr amp gpio\n", __func__);
			gpio_direction_output(top_spk_pamp_gpio, 1);
		}
	} else {
		pr_err("%s: ERROR : Invalid External Speaker Ampl GPIO."
			" gpio = %u\n", __func__, spk_amp_gpio);
		return;
	}
}

static void msm_ext_spk_power_amp_on(u32 spk)
{
	if (spk & (BOTTOM_SPK_AMP_POS | BOTTOM_SPK_AMP_NEG)) {

		if ((msm_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_POS) &&
			(msm_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_NEG)) {

			pr_debug("%s() External Bottom Speaker Ampl already "
				"turned on. spk = 0x%08x\n", __func__, spk);
			return;
		}

		msm_ext_bottom_spk_pamp |= spk;

		if ((msm_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_POS) &&
			(msm_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_NEG)) {

			msm_enable_ext_spk_amp_gpio(bottom_spk_pamp_gpio);
			pr_debug("%s: slepping 4 ms after turning on external "
				" Bottom Speaker Ampl\n", __func__);
			usleep_range(4000, 4000);
		}

	} else if (spk & (TOP_SPK_AMP_POS | TOP_SPK_AMP_NEG)) {

		if ((msm_ext_top_spk_pamp & TOP_SPK_AMP_POS) &&
			(msm_ext_top_spk_pamp & TOP_SPK_AMP_NEG)) {

			pr_debug("%s() External Top Speaker Ampl already"
				"turned on. spk = 0x%08x\n", __func__, spk);
			return;
		}

		msm_ext_top_spk_pamp |= spk;

		if ((msm_ext_top_spk_pamp & TOP_SPK_AMP_POS) &&
			(msm_ext_top_spk_pamp & TOP_SPK_AMP_NEG)) {

			msm_enable_ext_spk_amp_gpio(top_spk_pamp_gpio);
			pr_debug("%s: sleeping 4 ms after turning on "
				" external Top Speaker Ampl\n", __func__);
			usleep_range(4000, 4000);
		}
	} else  {

		pr_err("%s: ERROR : Invalid External Speaker Ampl. spk = 0x%08x\n",
			__func__, spk);
		return;
	}
}

static void msm_ext_spk_power_amp_off(u32 spk)
{
	if (spk & (BOTTOM_SPK_AMP_POS | BOTTOM_SPK_AMP_NEG)) {

		if (!msm_ext_bottom_spk_pamp)
			return;

		gpio_direction_output(bottom_spk_pamp_gpio, 0);
		gpio_free(bottom_spk_pamp_gpio);
		msm_ext_bottom_spk_pamp = 0;

		pr_debug("%s: sleeping 4 ms after turning off external Bottom"
			" Speaker Ampl\n", __func__);

		usleep_range(4000, 4000);

	} else if (spk & (TOP_SPK_AMP_POS | TOP_SPK_AMP_NEG)) {

		if (!msm_ext_top_spk_pamp)
			return;

		gpio_direction_output(top_spk_pamp_gpio, 0);
		gpio_free(top_spk_pamp_gpio);
		msm_ext_top_spk_pamp = 0;

		pr_debug("%s: sleeping 4 ms after turning off external Top"
			" Spkaker Ampl\n", __func__);

		usleep_range(4000, 4000);
	} else  {

		pr_err("%s: ERROR : Invalid Ext Spk Ampl. spk = 0x%08x\n",
			__func__, spk);
		return;
	}
}

static void msm_ext_control(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	pr_debug("%s: msm_spk_control = %d", __func__, msm_spk_control);
	if (msm_spk_control == MSM8064_SPK_ON) {
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Pos");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Neg");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Pos");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Neg");
	} else {
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Bottom Pos");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Bottom Neg");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Top Pos");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Top Neg");
	}

	snd_soc_dapm_sync(dapm);
}

static int msm_get_spk(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_spk_control = %d", __func__, msm_spk_control);
	ucontrol->value.integer.value[0] = msm_spk_control;
	return 0;
}
static int msm_set_spk(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	pr_debug("%s()\n", __func__);
	if (msm_spk_control == ucontrol->value.integer.value[0])
		return 0;

	msm_spk_control = ucontrol->value.integer.value[0];
	msm_ext_control(codec);
	return 1;
}
static int msm_spkramp_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *k, int event)
{
	pr_debug("%s() %x\n", __func__, SND_SOC_DAPM_EVENT_ON(event));

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (!strncmp(w->name, "Ext Spk Bottom Pos", 18))
			msm_ext_spk_power_amp_on(BOTTOM_SPK_AMP_POS);
		else if (!strncmp(w->name, "Ext Spk Bottom Neg", 18))
			msm_ext_spk_power_amp_on(BOTTOM_SPK_AMP_NEG);
		else if (!strncmp(w->name, "Ext Spk Top Pos", 15))
			msm_ext_spk_power_amp_on(TOP_SPK_AMP_POS);
		else if  (!strncmp(w->name, "Ext Spk Top Neg", 15))
			msm_ext_spk_power_amp_on(TOP_SPK_AMP_NEG);
		else {
			pr_err("%s() Invalid Speaker Widget = %s\n",
					__func__, w->name);
			return -EINVAL;
		}

	} else {
		if (!strncmp(w->name, "Ext Spk Bottom Pos", 18))
			msm_ext_spk_power_amp_off(BOTTOM_SPK_AMP_POS);
		else if (!strncmp(w->name, "Ext Spk Bottom Neg", 18))
			msm_ext_spk_power_amp_off(BOTTOM_SPK_AMP_NEG);
		else if (!strncmp(w->name, "Ext Spk Top Pos", 15))
			msm_ext_spk_power_amp_off(TOP_SPK_AMP_POS);
		else if  (!strncmp(w->name, "Ext Spk Top Neg", 15))
			msm_ext_spk_power_amp_off(TOP_SPK_AMP_NEG);
		else {
			pr_err("%s() Invalid Speaker Widget = %s\n",
					__func__, w->name);
			return -EINVAL;
		}
	}
	return 0;
}

static int msm_enable_codec_ext_clk(struct snd_soc_codec *codec, int enable,
				    bool dapm)
{
	pr_debug("%s: enable = %d\n", __func__, enable);
	if (enable) {
		clk_users++;
		pr_debug("%s: clk_users = %d\n", __func__, clk_users);
		if (clk_users != 1)
			return 0;

		if (codec_clk) {
			clk_set_rate(codec_clk, TABLA_EXT_CLK_RATE);
			clk_prepare_enable(codec_clk);
			tabla_mclk_enable(codec, 1, dapm);
		} else {
			pr_err("%s: Error setting Tabla MCLK\n", __func__);
			clk_users--;
			return -EINVAL;
		}
	} else {
		pr_debug("%s: clk_users = %d\n", __func__, clk_users);
		if (clk_users == 0)
			return 0;
		clk_users--;
		if (!clk_users) {
			pr_debug("%s: disabling MCLK. clk_users = %d\n",
					 __func__, clk_users);
			tabla_mclk_enable(codec, 0, dapm);
			clk_disable_unprepare(codec_clk);
		}
	}
	return 0;
}

static int msm_mclk_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:

		clk_users++;
		pr_debug("%s: clk_users = %d\n", __func__, clk_users);

		if (clk_users != 1)
			return 0;

		if (codec_clk) {
			clk_set_rate(codec_clk, 12288000);
			clk_prepare_enable(codec_clk);
			tabla_mclk_enable(w->codec, 1, true);

		} else {
			pr_err("%s: Error setting Tabla MCLK\n", __func__);
			clk_users--;
			return -EINVAL;
		}
		break;
	case SND_SOC_DAPM_POST_PMD:

		pr_debug("%s: clk_users = %d\n", __func__, clk_users);

		if (clk_users == 0)
			return 0;

		clk_users--;

		if (!clk_users) {
			pr_debug("%s: disabling MCLK. clk_users = %d\n",
					__func__, clk_users);

			tabla_mclk_enable(w->codec, 0, true);
			clk_disable_unprepare(codec_clk);
		}
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget apq8064_dapm_widgets[] = {

	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
	msm_mclk_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SPK("Ext Spk Bottom Pos", msm_spkramp_event),
	SND_SOC_DAPM_SPK("Ext Spk Bottom Neg", msm_spkramp_event),

	SND_SOC_DAPM_SPK("Ext Spk Top Pos", msm_spkramp_event),
	SND_SOC_DAPM_SPK("Ext Spk Top Neg", msm_spkramp_event),

	/************ Analog MICs ************/
	/**
	 * Analog mic7 (Front Top) on Liquid.
	 * Used as Handset mic on CDP.
	 */
	SND_SOC_DAPM_MIC("Analog mic7", NULL),

	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCRight Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCLeft Headset Mic", NULL),

	/*********** Digital Mics ***************/
	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),
	SND_SOC_DAPM_MIC("Digital Mic5", NULL),
	SND_SOC_DAPM_MIC("Digital Mic6", NULL),
};

static const struct snd_soc_dapm_route apq8064_common_audio_map[] = {

	{"RX_BIAS", NULL, "MCLK"},
	{"LDO_H", NULL, "MCLK"},

	{"HEADPHONE", NULL, "LDO_H"},

	/* Speaker path */
	{"Ext Spk Bottom Pos", NULL, "LINEOUT1"},
	{"Ext Spk Bottom Neg", NULL, "LINEOUT3"},

	{"Ext Spk Top Pos", NULL, "LINEOUT2"},
	{"Ext Spk Top Neg", NULL, "LINEOUT4"},

	/************   Analog MIC Paths  ************/

	/* Headset Mic */
	{"AMIC2", NULL, "MIC BIAS2 External"},
	{"MIC BIAS2 External", NULL, "Headset Mic"},

	/* Headset ANC microphones */
	{"AMIC3", NULL, "MIC BIAS3 Internal1"},
	{"MIC BIAS3 Internal1", NULL, "ANCRight Headset Mic"},

	{"AMIC4", NULL, "MIC BIAS1 Internal2"},
	{"MIC BIAS1 Internal2", NULL, "ANCLeft Headset Mic"},
};

static const struct snd_soc_dapm_route apq8064_mtp_audio_map[] = {

	/************   Digital MIC Paths  ************/

	/*
	 * Digital Mic1 (Front bottom Left) on MTP.
	 * Conncted to DMIC1 Input on Tabla codec.
	 */
	{"DMIC1", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic1"},

	/**
	 * Digital Mic2 (Front bottom right) on MTP.
	 * Conncted to DMIC2 Input on Tabla codec.
	 */
	{"DMIC2", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic2"},

	/**
	 * Digital Mic3 (Back bottom) on MTP.
	 * Conncted to DMIC3 Input on Tabla codec.
	 */
	{"DMIC3", NULL, "MIC BIAS3 External"},
	{"MIC BIAS3 External", NULL, "Digital Mic3"},

	/**
	 * Digital Mic4 (Back top) on MTP.
	 * Conncted to DMIC4 Input on Tabla codec.
	 */
	{"DMIC4", NULL, "MIC BIAS3 External"},
	{"MIC BIAS3 External", NULL, "Digital Mic4"},

	/**
	 * Digital Mic5 (Top front Mic) on MTP.
	 * Conncted to DMIC6 Input on Tabla codec.
	 */
	{"DMIC6", NULL, "MIC BIAS4 External"},
	{"MIC BIAS4 External", NULL, "Digital Mic5"},

};

static const struct snd_soc_dapm_route apq8064_liquid_cdp_audio_map[] = {

	/************   Analog MIC Paths  ************/
	/**
	 * Analog mic7 (Front Top Mic) on Liquid.
	 * Used as Handset mic on CDP.
	 * Not there on MTP.
	 */
	{"AMIC1", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Analog mic7"},


	/************   Digital MIC Paths  ************/
	/**
	 * The digital Mic routes are setup considering
	 * Liquid as default device.
	 */

	/**
	 * Digital Mic1 (Front bottom left corner) on Liquid.
	 * Digital Mic2 (Front bottom right) on MTP.
	 * Digital Mic GM1 on CDP mainboard.
	 * Conncted to DMIC2 Input on Tabla codec.
	 */
	{"DMIC2", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic1"},

	/**
	 * Digital Mic2 (Front left side) on Liquid.
	 * Digital Mic GM2 on CDP mainboard.
	 * Not there on MTP.
	 * Conncted to DMIC3 Input on Tabla codec.
	 */
	{"DMIC3", NULL, "MIC BIAS3 External"},
	{"MIC BIAS3 External", NULL, "Digital Mic2"},

	/**
	 * Digital Mic3. Front bottom left of middle on Liquid.
	 * Digital Mic5 (Top front Mic) on MTP.
	 * Digital Mic GM5 on CDP mainboard.
	 * Conncted to DMIC6 Input on Tabla codec.
	 */
	{"DMIC6", NULL, "MIC BIAS4 External"},
	{"MIC BIAS4 External", NULL, "Digital Mic3"},

	/**
	 * Digital Mic4. Back bottom on Liquid.
	 * Digital Mic GM3 on CDP mainboard.
	 * Top Front Mic on MTP.
	 * Conncted to DMIC5 Input on Tabla codec.
	 */
	{"DMIC5", NULL, "MIC BIAS4 External"},
	{"MIC BIAS4 External", NULL, "Digital Mic4"},

	/**
	 * Digital Mic5. Front bottom right of middle on Liquid.
	 * Digital Mic GM6 on CDP mainboard.
	 * Not there on MTP.
	 * Conncted to DMIC4 Input on Tabla codec.
	 */
	{"DMIC4", NULL, "MIC BIAS3 External"},
	{"MIC BIAS3 External", NULL, "Digital Mic5"},

	/* Digital Mic6 (Front bottom right corner) on Liquid.
	 * Digital Mic1 (Front bottom Left) on MTP.
	 * Digital Mic GM4 on CDP.
	 * Conncted to DMIC1 Input on Tabla codec.
	 */
	{"DMIC1", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic6"},
};

static const char *spk_function[] = {"Off", "On"};
static const char *slim0_rx_ch_text[] = {"One", "Two"};
static const char *slim0_tx_ch_text[] = {"One", "Two", "Three", "Four"};

static const struct soc_enum msm_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, spk_function),
	SOC_ENUM_SINGLE_EXT(2, slim0_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(4, slim0_tx_ch_text),
};

static const char *btsco_rate_text[] = {"8000", "16000"};
static const struct soc_enum msm_btsco_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, btsco_rate_text),
};

static int msm_slim_0_rx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_0_rx_ch  = %d\n", __func__,
			msm_slim_0_rx_ch);
	ucontrol->value.integer.value[0] = msm_slim_0_rx_ch - 1;
	return 0;
}

static int msm_slim_0_rx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_0_rx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_slim_0_rx_ch = %d\n", __func__,
			msm_slim_0_rx_ch);
	return 1;
}

static int msm_slim_0_tx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_0_tx_ch  = %d\n", __func__,
			msm_slim_0_tx_ch);
	ucontrol->value.integer.value[0] = msm_slim_0_tx_ch - 1;
	return 0;
}

static int msm_slim_0_tx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_0_tx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_slim_0_tx_ch = %d\n", __func__,
			msm_slim_0_tx_ch);
	return 1;
}

static int msm_slim_3_rx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_3_rx_ch  = %d\n", __func__,
			msm_slim_3_rx_ch);
	ucontrol->value.integer.value[0] = msm_slim_3_rx_ch - 1;
	return 0;
}

static int msm_slim_3_rx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_3_rx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_slim_3_rx_ch = %d\n", __func__,
			msm_slim_3_rx_ch);
	return 1;
}

static int msm_btsco_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_btsco_rate  = %d", __func__,
					msm_btsco_rate);
	ucontrol->value.integer.value[0] = msm_btsco_rate;
	return 0;
}

static int msm_btsco_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm_btsco_rate = BTSCO_RATE_8KHZ;
		break;
	case 1:
		msm_btsco_rate = BTSCO_RATE_16KHZ;
		break;
	default:
		msm_btsco_rate = BTSCO_RATE_8KHZ;
		break;
	}
	pr_debug("%s: msm_btsco_rate = %d\n", __func__,
					msm_btsco_rate);
	return 0;
}

static int msm_incall_rec_mode_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = rec_mode;
	return 0;
}

static int msm_incall_rec_mode_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{

	rec_mode = ucontrol->value.integer.value[0];
	pr_debug("%s: rec_mode:%d\n", __func__, rec_mode);

	return 0;
}

static const struct snd_kcontrol_new tabla_msm_controls[] = {
	SOC_ENUM_EXT("Speaker Function", msm_enum[0], msm_get_spk,
		msm_set_spk),
	SOC_ENUM_EXT("SLIM_0_RX Channels", msm_enum[1],
		msm_slim_0_rx_ch_get, msm_slim_0_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_TX Channels", msm_enum[2],
		msm_slim_0_tx_ch_get, msm_slim_0_tx_ch_put),
};

static const struct snd_kcontrol_new int_btsco_rate_mixer_controls[] = {
	SOC_ENUM_EXT("Internal BTSCO SampleRate", msm_btsco_enum[0],
		msm_btsco_rate_get, msm_btsco_rate_put),
};

static const struct snd_kcontrol_new incall_rec_mode_mixer_controls[] = {
	 SOC_SINGLE_EXT("Incall Rec Mode", SND_SOC_NOPM, 0, 1, 0,
			msm_incall_rec_mode_get, msm_incall_rec_mode_put),
};

static int msm_btsco_init(struct snd_soc_pcm_runtime *rtd)
{
	int err = 0;
	struct snd_soc_platform *platform = rtd->platform;

	err = snd_soc_add_platform_controls(platform,
			int_btsco_rate_mixer_controls,
		ARRAY_SIZE(int_btsco_rate_mixer_controls));
	if (err < 0)
		return err;
	return 0;
}

static const struct snd_kcontrol_new slim_3_mixer_controls[] = {
	SOC_ENUM_EXT("SLIM_3_RX Channels", msm_enum[1],
		msm_slim_3_rx_ch_get, msm_slim_3_rx_ch_put),
};

static int msm_slim_3_init(struct snd_soc_pcm_runtime *rtd)
{
	int err = 0;
	struct snd_soc_platform *platform = rtd->platform;

	err = snd_soc_add_platform_controls(platform,
			slim_3_mixer_controls,
		ARRAY_SIZE(slim_3_mixer_controls));
	if (err < 0)
		return err;
	return 0;
}

static int msm_incall_rec_init(struct snd_soc_pcm_runtime *rtd)
{
	int err = 0;
	struct snd_soc_platform *platform = rtd->platform;

	err = snd_soc_add_platform_controls(platform,
			incall_rec_mode_mixer_controls,
		ARRAY_SIZE(incall_rec_mode_mixer_controls));
	if (err < 0)
		return err;
	return 0;
}

static void *def_tabla_mbhc_cal(void)
{
	void *tabla_cal;
	struct tabla_mbhc_btn_detect_cfg *btn_cfg;
	u16 *btn_low, *btn_high;
	u8 *n_ready, *n_cic, *gain;

	tabla_cal = kzalloc(TABLA_MBHC_CAL_SIZE(TABLA_MBHC_DEF_BUTTONS,
						TABLA_MBHC_DEF_RLOADS),
			    GFP_KERNEL);
	if (!tabla_cal) {
		pr_err("%s: out of memory\n", __func__);
		return NULL;
	}

#define S(X, Y) ((TABLA_MBHC_CAL_GENERAL_PTR(tabla_cal)->X) = (Y))
	S(t_ldoh, 100);
	S(t_bg_fast_settle, 100);
	S(t_shutdown_plug_rem, 255);
	S(mbhc_nsa, 4);
	S(mbhc_navg, 4);
#undef S
#define S(X, Y) ((TABLA_MBHC_CAL_PLUG_DET_PTR(tabla_cal)->X) = (Y))
	S(mic_current, TABLA_PID_MIC_5_UA);
	S(hph_current, TABLA_PID_MIC_5_UA);
	S(t_mic_pid, 100);
	S(t_ins_complete, 250);
	S(t_ins_retry, 200);
#undef S
#define S(X, Y) ((TABLA_MBHC_CAL_PLUG_TYPE_PTR(tabla_cal)->X) = (Y))
	S(v_no_mic, 30);
	S(v_hs_max, 1550);
#undef S
#define S(X, Y) ((TABLA_MBHC_CAL_BTN_DET_PTR(tabla_cal)->X) = (Y))
	S(c[0], 62);
	S(c[1], 124);
	S(nc, 1);
	S(n_meas, 3);
	S(mbhc_nsc, 11);
	S(n_btn_meas, 1);
	S(n_btn_con, 2);
	S(num_btn, TABLA_MBHC_DEF_BUTTONS);
	S(v_btn_press_delta_sta, 100);
	S(v_btn_press_delta_cic, 50);
#undef S
	btn_cfg = TABLA_MBHC_CAL_BTN_DET_PTR(tabla_cal);
	btn_low = tabla_mbhc_cal_btn_det_mp(btn_cfg, TABLA_BTN_DET_V_BTN_LOW);
	btn_high = tabla_mbhc_cal_btn_det_mp(btn_cfg, TABLA_BTN_DET_V_BTN_HIGH);
	btn_low[0] = -50;
	btn_high[0] = 10;
	btn_low[1] = 11;
	btn_high[1] = 38;
	btn_low[2] = 39;
	btn_high[2] = 64;
	btn_low[3] = 65;
	btn_high[3] = 91;
	btn_low[4] = 92;
	btn_high[4] = 115;
	btn_low[5] = 116;
	btn_high[5] = 141;
	btn_low[6] = 142;
	btn_high[6] = 163;
	btn_low[7] = 164;
	btn_high[7] = 250;
	n_ready = tabla_mbhc_cal_btn_det_mp(btn_cfg, TABLA_BTN_DET_N_READY);
	n_ready[0] = 48;
	n_ready[1] = 38;
	n_cic = tabla_mbhc_cal_btn_det_mp(btn_cfg, TABLA_BTN_DET_N_CIC);
	n_cic[0] = 60;
	n_cic[1] = 47;
	gain = tabla_mbhc_cal_btn_det_mp(btn_cfg, TABLA_BTN_DET_GAIN);
	gain[0] = 11;
	gain[1] = 9;

	return tabla_cal;
}

static int msm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	unsigned int rx_ch_cnt = 0, tx_ch_cnt = 0;
	unsigned int user_set_tx_ch = 0;


	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {

		pr_debug("%s: rx_0_ch=%d\n", __func__, msm_slim_0_rx_ch);

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
				msm_slim_0_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai, 0, 0,
				msm_slim_0_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
								__func__);
			goto end;
		}
	} else {

		if (codec_dai->id  == 2)
			user_set_tx_ch =  msm_slim_0_tx_ch;
		else if (codec_dai->id  == 4)
			user_set_tx_ch =  params_channels(params);
		else if (codec_dai->id == 5) {
			/* DAI 5 is used for external EC reference from codec.
			 * Since Rx is fed as reference for EC, the config of
			 * this DAI is based on that of the Rx path.
			 */
			user_set_tx_ch =  msm_slim_0_rx_ch;
		}

		pr_debug("%s: %s_tx_dai_id_%d_ch=%d\n", __func__,
			codec_dai->name, codec_dai->id, user_set_tx_ch);

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai,
				user_set_tx_ch, tx_ch, 0 , 0);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai,
				user_set_tx_ch, tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
								__func__);
			goto end;
		}


	}
end:
	return ret;
}

static int msm_stubrx_init(struct snd_soc_pcm_runtime *rtd)
{
	rtd->pmdown_time = 0;

	return 0;
}

static int msm_slimbus_1_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch = SLIM_1_RX_1, tx_ch = SLIM_1_TX_1;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pr_debug("%s: APQ BT/USB TX -> SLIMBUS_1_RX -> MDM TX shared ch %d\n",
			__func__, rx_ch);

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0, 1, &rx_ch);
		if (ret < 0) {
			pr_err("%s: Erorr %d setting SLIM_1 RX channel map\n",
				__func__, ret);

			goto end;
		}
	} else {
		pr_debug("%s: MDM RX -> SLIMBUS_1_TX -> APQ BT/USB Rx shared ch %d\n",
			__func__, tx_ch);

		ret = snd_soc_dai_set_channel_map(cpu_dai, 1, &tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: Erorr %d setting SLIM_1 TX channel map\n",
				__func__, ret);

			goto end;
		}
	}

end:
	return ret;
}

static int msm_slimbus_3_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch[2] = {SLIM_3_RX_1, SLIM_3_RX_2};

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pr_debug("%s: slim_3_rx_ch %d, sch %d %d\n",
			 __func__, msm_slim_3_rx_ch,
				 rx_ch[0], rx_ch[1]);

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
				msm_slim_3_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: Erorr %d setting SLIM_3 RX channel map\n",
				__func__, ret);

			goto end;
		}
	} else {
		pr_err("%s: SLIMBUS_3_TX not defined for this DAI\n", __func__);
	}

end:
	return ret;
}

static int msm_slimbus_4_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch = SLIM_4_RX_1, tx_ch[2];

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pr_debug("%s: APQ Incall Playback SLIMBUS_4_RX -> MDM TX shared ch %d\n",
			__func__, rx_ch);

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0, 1, &rx_ch);
		if (ret < 0) {
			pr_err("%s: Erorr %d setting SLIM_4 RX channel map\n",
				__func__, ret);

		}
	} else {
		if (rec_mode == INCALL_REC_STEREO) {
			tx_ch[0] = SLIM_4_TX_1;
			tx_ch[1] = SLIM_4_TX_2;
			ret = snd_soc_dai_set_channel_map(cpu_dai, 2,
							tx_ch, 0, 0);
		} else {
			tx_ch[0] = SLIM_4_TX_1;
			ret = snd_soc_dai_set_channel_map(cpu_dai, 1,
							tx_ch, 0, 0);
		}
		pr_debug("%s: Incall Record shared tx_ch[0]:%d, tx_ch[1]:%d\n",
			__func__, tx_ch[0], tx_ch[1]);

		if (ret < 0) {
			pr_err("%s: Erorr %d setting SLIM_4 TX channel map\n",
				__func__, ret);

		}
	}

	return ret;
}


static int msm_audrx_init(struct snd_soc_pcm_runtime *rtd)
{
	int err;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_debug("%s(), dev_name%s\n", __func__, dev_name(cpu_dai->dev));

	/*if (machine_is_msm_liquid()) {
		top_spk_pamp_gpio = (PM8921_GPIO_PM_TO_SYS(19));
		bottom_spk_pamp_gpio = (PM8921_GPIO_PM_TO_SYS(18));
	}*/

	rtd->pmdown_time = 0;

	err = snd_soc_add_controls(codec, tabla_msm_controls,
				ARRAY_SIZE(tabla_msm_controls));
	if (err < 0)
		return err;

	snd_soc_dapm_new_controls(dapm, apq8064_dapm_widgets,
				ARRAY_SIZE(apq8064_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, apq8064_common_audio_map,
		ARRAY_SIZE(apq8064_common_audio_map));

	if (machine_is_apq8064_mtp()) {
		snd_soc_dapm_add_routes(dapm, apq8064_mtp_audio_map,
			ARRAY_SIZE(apq8064_mtp_audio_map));
	} else  {
		snd_soc_dapm_add_routes(dapm, apq8064_liquid_cdp_audio_map,
			ARRAY_SIZE(apq8064_liquid_cdp_audio_map));
	}

	snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Pos");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Neg");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Pos");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Neg");

	snd_soc_dapm_sync(dapm);

	err = snd_soc_jack_new(codec, "Headset Jack",
		(SND_JACK_HEADSET | SND_JACK_OC_HPHL | SND_JACK_OC_HPHR),
		&hs_jack);
	if (err) {
		pr_err("failed to create new jack\n");
		return err;
	}

	err = snd_soc_jack_new(codec, "Button Jack",
			       TABLA_JACK_BUTTON_MASK, &button_jack);
	if (err) {
		pr_err("failed to create new jack\n");
		return err;
	}

	codec_clk = clk_get(cpu_dai->dev, "osr_clk");

	err = tabla_hs_detect(codec, &mbhc_cfg);

	return err;
}

static struct snd_soc_dsp_link lpa_fe_media = {
	.playback = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link fe_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link slimbus0_hl_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link int_fm_hl_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static int msm_slim_0_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm_slim_0_rx_ch;

	return 0;
}

static int msm_slim_0_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm_slim_0_tx_ch;

	return 0;
}

static int msm_slim_3_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm_slim_3_rx_ch;

	return 0;
}

static int msm_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;

	return 0;
}

static int msm_hdmi_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s channels->min %u channels->max %u ()\n", __func__,
			channels->min, channels->max);

	rate->min = rate->max = 48000;

	return 0;
}

static int msm_btsco_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm_btsco_rate;
	channels->min = channels->max = msm_btsco_ch;

	return 0;
}
static int msm_auxpcm_be_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	/* PCM only supports mono output with 8khz sample rate */
	rate->min = rate->max = 8000;
	channels->min = channels->max = 1;

	return 0;
}

static int msm_aux_pcm_get_gpios(void)
{
	int ret = 0;

	pr_debug("%s\n", __func__);

	ret = gpio_request(GPIO_AUX_PCM_DOUT, "AUX PCM DOUT");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM DOUT",
				__func__, GPIO_AUX_PCM_DOUT);
		goto fail_dout;
	}

	ret = gpio_request(GPIO_AUX_PCM_DIN, "AUX PCM DIN");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM DIN",
				__func__, GPIO_AUX_PCM_DIN);
		goto fail_din;
	}

	ret = gpio_request(GPIO_AUX_PCM_SYNC, "AUX PCM SYNC");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM SYNC",
				__func__, GPIO_AUX_PCM_SYNC);
		goto fail_sync;
	}
	ret = gpio_request(GPIO_AUX_PCM_CLK, "AUX PCM CLK");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM CLK",
				__func__, GPIO_AUX_PCM_CLK);
		goto fail_clk;
	}

	return 0;

fail_clk:
	gpio_free(GPIO_AUX_PCM_SYNC);
fail_sync:
	gpio_free(GPIO_AUX_PCM_DIN);
fail_din:
	gpio_free(GPIO_AUX_PCM_DOUT);
fail_dout:

	return ret;
}

static int msm_aux_pcm_free_gpios(void)
{
	gpio_free(GPIO_AUX_PCM_DIN);
	gpio_free(GPIO_AUX_PCM_DOUT);
	gpio_free(GPIO_AUX_PCM_SYNC);
	gpio_free(GPIO_AUX_PCM_CLK);

	return 0;
}
static int msm_startup(struct snd_pcm_substream *substream)
{
	pr_debug("%s(): substream = %s  stream = %d\n", __func__,
		 substream->name, substream->stream);
	return 0;
}

static int msm_auxpcm_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;

	pr_debug("%s(): substream = %s\n", __func__, substream->name);
	ret = msm_aux_pcm_get_gpios();
	if (ret < 0) {
		pr_err("%s: Aux PCM GPIO request failed\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static void msm_auxpcm_shutdown(struct snd_pcm_substream *substream)
{

	pr_debug("%s(): substream = %s\n", __func__, substream->name);
	msm_aux_pcm_free_gpios();
}

static void msm_shutdown(struct snd_pcm_substream *substream)
{
	pr_debug("%s(): substream = %s  stream = %d\n", __func__,
		 substream->name, substream->stream);
}

static struct snd_soc_ops msm_be_ops = {
	.startup = msm_startup,
	.hw_params = msm_hw_params,
	.shutdown = msm_shutdown,
};

static struct snd_soc_ops msm_auxpcm_be_ops = {
	.startup = msm_auxpcm_startup,
	.shutdown = msm_auxpcm_shutdown,
};

static struct snd_soc_ops msm_slimbus_1_be_ops = {
	.startup = msm_startup,
	.hw_params = msm_slimbus_1_hw_params,
	.shutdown = msm_shutdown,
};

static struct snd_soc_ops msm_slimbus_3_be_ops = {
	.startup = msm_startup,
	.hw_params = msm_slimbus_3_hw_params,
	.shutdown = msm_shutdown,
};

static struct snd_soc_ops msm_slimbus_4_be_ops = {
	.startup = msm_startup,
	.hw_params = msm_slimbus_4_hw_params,
	.shutdown = msm_shutdown,
};

/* Digital audio interface glue - connects codec <---> CPU */
static struct snd_soc_dai_link msm_dai[] = {
	/* FrontEnd DAI Links */
	{
		.name = "MSM8960 Media1",
		.stream_name = "MultiMedia1",
		.cpu_dai_name	= "MultiMedia1",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{
		.name = "MSM8960 Media2",
		.stream_name = "MultiMedia2",
		.cpu_dai_name	= "MultiMedia2",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpu_dai_name   = "CS-VOICE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_CS_VOICE,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpu_dai_name	= "VoIP",
		.platform_name  = "msm-voip-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_VOIP,
	},
	{
		.name = "MSM8960 LPA",
		.stream_name = "LPA",
		.cpu_dai_name	= "MultiMedia3",
		.platform_name  = "msm-pcm-lpa",
		.dynamic = 1,
		.dsp_link = &lpa_fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA3,
	},
	/* Hostless PMC purpose */
	{
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.cpu_dai_name	= "SLIMBUS0_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &slimbus0_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* .be_id = do not care */
	},
	{
		.name = "INT_FM Hostless",
		.stream_name = "INT_FM Hostless",
		.cpu_dai_name	= "INT_FM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &int_fm_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* .be_id = do not care */
	},
	{
		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.cpu_dai_name = "msm-dai-q6.241",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.cpu_dai_name = "msm-dai-q6.240",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "Voice Stub",
		.stream_name = "Voice Stub",
		.cpu_dai_name = "VOICE_STUB",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.be_id = MSM_FRONTEND_DAI_VOICE_STUB,
	},
	/* Backend DAI Links */
	{
		.name = LPASS_BE_SLIMBUS_0_RX,
		.stream_name = "Slimbus Playback",
		.cpu_dai_name = "msm-dai-q6.16384",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla_codec",
		.codec_dai_name	= "tabla_rx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_RX,
		.init = &msm_audrx_init,
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		.ops = &msm_be_ops,
	},
	{
		.name = LPASS_BE_SLIMBUS_0_TX,
		.stream_name = "Slimbus Capture",
		.cpu_dai_name = "msm-dai-q6.16385",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla_codec",
		.codec_dai_name	= "tabla_tx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_TX,
		.be_hw_params_fixup = msm_slim_0_tx_be_hw_params_fixup,
		.ops = &msm_be_ops,
	},
	/* Backend BT/FM DAI Links */
	{
		.name = LPASS_BE_INT_BT_SCO_RX,
		.stream_name = "Internal BT-SCO Playback",
		.cpu_dai_name = "msm-dai-q6.12288",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-rx",
		.init = &msm_btsco_init,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_RX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_BT_SCO_TX,
		.stream_name = "Internal BT-SCO Capture",
		.cpu_dai_name = "msm-dai-q6.12289",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_TX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_RX,
		.stream_name = "Internal FM Playback",
		.cpu_dai_name = "msm-dai-q6.12292",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_TX,
		.stream_name = "Internal FM Capture",
		.cpu_dai_name = "msm-dai-q6.12293",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
	},
	/* HDMI BACK END DAI Link */
	{
		.name = LPASS_BE_HDMI,
		.stream_name = "HDMI Playback",
		.cpu_dai_name = "msm-dai-q6-hdmi.8",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_HDMI_RX,
		.be_hw_params_fixup = msm_hdmi_be_hw_params_fixup,
	},
	/* Backend AFE DAI Links */
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.cpu_dai_name = "msm-dai-q6.224",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_codec = 1,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_RX,
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.cpu_dai_name = "msm-dai-q6.225",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_codec = 1,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_TX,
	},
	/* AUX PCM Backend DAI Links */
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6.2",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_auxpcm_be_ops,
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6.3",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
	},
	{
		.name = LPASS_BE_STUB_RX,
		.stream_name = "Stub Playback",
		.cpu_dai_name = "msm-dai-stub",
		.platform_name = "msm-pcm-routing",
		.codec_name = "tabla_codec",
		.codec_dai_name = "tabla_rx2",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_EXTPROC_RX,
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		.init = &msm_stubrx_init,
		.ops = &msm_be_ops,
	},
	{
		.name = LPASS_BE_STUB_TX,
		.stream_name = "Stub Capture",
		.cpu_dai_name = "msm-dai-stub",
		.platform_name = "msm-pcm-routing",
		.codec_name = "tabla_codec",
		.codec_dai_name = "tabla_tx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_EXTPROC_TX,
		.be_hw_params_fixup = msm_slim_0_tx_be_hw_params_fixup,
		.ops = &msm_be_ops,
	},
	{
		.name = LPASS_BE_SLIMBUS_1_RX,
		.stream_name = "Slimbus1 Playback",
		.cpu_dai_name = "msm-dai-q6.16386",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_1_RX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
		.ops = &msm_slimbus_1_be_ops,

	},
	{
		.name = LPASS_BE_SLIMBUS_1_TX,
		.stream_name = "Slimbus1 Capture",
		.cpu_dai_name = "msm-dai-q6.16387",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_1_TX,
		.be_hw_params_fixup =  msm_btsco_be_hw_params_fixup,
		.ops = &msm_slimbus_1_be_ops,
	},
	{
		.name = "SLIMBUS_2 Hostless",
		.stream_name = "SLIMBUS_2 Hostless",
		.cpu_dai_name = "msm-dai-q6.16389",
		.platform_name = "msm-pcm-hostless",
		.codec_name = "tabla_codec",
		.codec_dai_name = "tabla_tx2",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm_be_ops,
	},

	/* Incall Music Back End DAI Link */
	{
		.name = LPASS_BE_SLIMBUS_4_RX,
		.stream_name = "Slimbus4 Playback",
		.cpu_dai_name = "msm-dai-q6.16392",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_4_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ops = &msm_slimbus_4_be_ops,
	},
	/* Incall Record Back End DAI Link */
	{
		.name = LPASS_BE_SLIMBUS_4_TX,
		.stream_name = "Slimbus4 Capture",
		.cpu_dai_name = "msm-dai-q6.16393",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_4_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.init = &msm_incall_rec_init,
		.ops = &msm_slimbus_4_be_ops,
	},
	{
		.name = LPASS_BE_STUB_1_TX,
		.stream_name = "Stub1 Capture",
		.cpu_dai_name = "msm-dai-stub",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla_codec",
		.codec_dai_name	= "tabla_tx3",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_EXTPROC_EC_TX,
		/* This BE is used for external EC reference from codec. Since
		 * Rx is fed as reference for EC, the config of this DAI is
		 * based on that of the Rx path.
		 */
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		.ops = &msm_be_ops,
	},
	{

		.name = LPASS_BE_SLIMBUS_3_RX,
		.stream_name = "Slimbus3 Playback",
		.cpu_dai_name = "msm-dai-q6.16390",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.init = &msm_slim_3_init,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_3_RX,
		.be_hw_params_fixup = msm_slim_3_rx_be_hw_params_fixup,
		.ops = &msm_slimbus_3_be_ops,
	},
};

struct snd_soc_card snd_soc_card_msm = {
	.name		= "apq8064-tabla-snd-card",
	.dai_link	= msm_dai,
	.num_links	= ARRAY_SIZE(msm_dai),
};

static struct platform_device *msm_snd_device;

static int msm_configure_headset_mic_gpios(void)
{
	int ret;
	struct pm_gpio param = {
		.direction      = PM_GPIO_DIR_OUT,
		.output_buffer  = PM_GPIO_OUT_BUF_CMOS,
		.output_value   = 1,
		.pull	   = PM_GPIO_PULL_NO,
		.vin_sel	= PM_GPIO_VIN_S4,
		.out_strength   = PM_GPIO_STRENGTH_MED,
		.function       = PM_GPIO_FUNC_NORMAL,
	};

	ret = gpio_request(PM8921_GPIO_PM_TO_SYS(23), "AV_SWITCH");
	if (ret) {
		pr_err("%s: Failed to request gpio %d\n", __func__,
			PM8921_GPIO_PM_TO_SYS(23));
		return ret;
	}

	ret = pm8xxx_gpio_config(PM8921_GPIO_PM_TO_SYS(23), &param);
	if (ret)
		pr_err("%s: Failed to configure gpio %d\n", __func__,
			PM8921_GPIO_PM_TO_SYS(23));
	else
		gpio_direction_output(PM8921_GPIO_PM_TO_SYS(23), 0);

	ret = gpio_request(PM8921_GPIO_PM_TO_SYS(35), "US_EURO_SWITCH");
	if (ret) {
		pr_err("%s: Failed to request gpio %d\n", __func__,
			PM8921_GPIO_PM_TO_SYS(35));
		gpio_free(PM8921_GPIO_PM_TO_SYS(23));
		return ret;
	}
	ret = pm8xxx_gpio_config(PM8921_GPIO_PM_TO_SYS(35), &param);
	if (ret)
		pr_err("%s: Failed to configure gpio %d\n", __func__,
			PM8921_GPIO_PM_TO_SYS(35));
	else
		gpio_direction_output(PM8921_GPIO_PM_TO_SYS(35), 0);

	return 0;
}
static void msm_free_headset_mic_gpios(void)
{
	if (msm_headset_gpios_configured) {
		gpio_free(PM8921_GPIO_PM_TO_SYS(23));
		gpio_free(PM8921_GPIO_PM_TO_SYS(35));
	}
}

static int __init msm_audio_init(void)
{
	int ret;

	if (!cpu_is_apq8064() || (socinfo_get_id() == 130)) {
		pr_err("%s: Not the right machine type\n", __func__);
		return -ENODEV;
	}

	mbhc_cfg.calibration = def_tabla_mbhc_cal();
	if (!mbhc_cfg.calibration) {
		pr_err("Calibration data allocation failed\n");
		return -ENOMEM;
	}

	msm_snd_device = platform_device_alloc("soc-audio", 0);
	if (!msm_snd_device) {
		pr_err("Platform device allocation failed\n");
		kfree(mbhc_cfg.calibration);
		return -ENOMEM;
	}

	platform_set_drvdata(msm_snd_device, &snd_soc_card_msm);
	ret = platform_device_add(msm_snd_device);
	if (ret) {
		platform_device_put(msm_snd_device);
		kfree(mbhc_cfg.calibration);
		return ret;
	}

	if (msm_configure_headset_mic_gpios()) {
		pr_err("%s Fail to configure headset mic gpios\n", __func__);
		msm_headset_gpios_configured = 0;
	} else
		msm_headset_gpios_configured = 1;

	return ret;

}
module_init(msm_audio_init);

static void __exit msm_audio_exit(void)
{
	if (!cpu_is_apq8064() || (socinfo_get_id() == 130)) {
		pr_err("%s: Not the right machine type\n", __func__);
		return ;
	}
	msm_free_headset_mic_gpios();
	platform_device_unregister(msm_snd_device);
	kfree(mbhc_cfg.calibration);
}
module_exit(msm_audio_exit);

MODULE_DESCRIPTION("ALSA SoC msm");
MODULE_LICENSE("GPL v2");