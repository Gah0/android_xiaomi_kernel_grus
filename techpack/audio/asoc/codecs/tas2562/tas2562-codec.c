/*
** =============================================================================
** Copyright (c) 2016  Texas Instruments Inc.
**
** This program is free software; you can redistribute it and/or modify it under
** the terms of the GNU General Public License as published by the Free Software
** Foundation; version 2.
**
** This program is distributed in the hope that it will be useful, but WITHOUT
** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
** FOR A PARTICULAR PURPOSE.See the GNU General Public License for more details.
**
** File:
**     tas2562-codec.c
**
** Description:
**     ALSA SoC driver for Texas Instruments TAS2562 High Performance 4W Smart
**     Amplifier
**
** =============================================================================
*/

#ifdef CONFIG_TAS2562_CODEC
#define DEBUG 5
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/firmware.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "tas2562.h"

#define TAS2562_MDELAY 0xFFFFFFFE
#define TAS2562_MSLEEP 0xFFFFFFFD

static char const *iv_enable_text[] = {"Off", "On"};
static int tas2562iv_enable = 1;
static int mbMute;
static const struct soc_enum tas2562_enum[] = {
    SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(iv_enable_text), iv_enable_text),
};
static int tas2562_set_fmt(struct tas2562_priv *pTAS2562, unsigned int fmt);

static int tas2562_i2c_load_data(struct tas2562_priv *pTAS2562, unsigned int *pData);
static int tas2562_mute_ctrl_get(struct snd_kcontrol *pKcontrol,
	struct snd_ctl_elem_value *pValue);
static int tas2562_mute_ctrl_put(struct snd_kcontrol *pKcontrol,
	struct snd_ctl_elem_value *pValue);
static int tas2562_load_init(struct tas2562_priv *pTAS2562);
static unsigned int p_tas2562_classH_D_data[] = {
		/* reg address			size	values */
	TAS2562_ClassHHeadroom, 0x4, 0x09, 0x99, 0x99, 0x9a,
	TAS2562_ClassHHysteresis, 0x4, 0x0, 0x0, 0x0, 0x0,
	TAS2562_ClassHMtct, 0x4, 0xb, 0x0, 0x0, 0x0,
	TAS2562_VBatFilter, 0x1, 0x38,
	TAS2562_ClassHReleaseTimer, 0x1, 0x3c,
	TAS2562_BoostSlope, 0x1, 0x78,
	TAS2562_TestPageConfiguration, 0x1, 0xd,
	TAS2562_ClassDConfiguration3, 0x1, 0x8e,
	TAS2562_ClassDConfiguration2, 0x1, 0x49,
	TAS2562_ClassDConfiguration4, 0x1, 0x21,
	TAS2562_ClassDConfiguration1, 0x1, 0x80,
	TAS2562_EfficiencyConfiguration, 0x1, 0xc1,
	0xFFFFFFFF, 0xFFFFFFFF
};


static unsigned int tas2562_codec_read(struct snd_soc_codec *codec,
		unsigned int reg)
{
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);
	int nResult = 0;
	unsigned int value = 0;

	nResult = pTAS2562->read(pTAS2562, reg, &value);

	if (nResult < 0)
		dev_err(pTAS2562->dev, "%s, ERROR, reg=0x%x, E=%d\n",
			__func__, reg, nResult);
	else
		dev_dbg(pTAS2562->dev, "%s, reg: 0x%x, value: 0x%x\n",
				__func__, reg, value);

	if (nResult >= 0)
		return value;
	else
		return nResult;
}

static int tas2562_iv_enable(struct tas2562_priv *pTAS2562, int enable)
{
	int nResult;

	if (enable) {
		pr_debug("%s: tas2562iv_enable \n", __func__);
		nResult = pTAS2562->update_bits(pTAS2562, TAS2562_PowerControl,
		    TAS2562_PowerControl_ISNSPower_Mask |
		    TAS2562_PowerControl_VSNSPower_Mask,
		    TAS2562_PowerControl_VSNSPower_Active |
		    TAS2562_PowerControl_ISNSPower_Active);
	} else {
		pr_debug("%s: tas2562iv_disable \n", __func__);
		nResult = pTAS2562->update_bits(pTAS2562, TAS2562_PowerControl,
			TAS2562_PowerControl_ISNSPower_Mask |
			TAS2562_PowerControl_VSNSPower_Mask,
			TAS2562_PowerControl_VSNSPower_PoweredDown |
			TAS2562_PowerControl_ISNSPower_PoweredDown);
	}
	tas2562iv_enable = enable;

	return nResult;
}

static int tas2562iv_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);
	int iv_enable = 0, nResult = 0;

    if (codec == NULL) {
		pr_err("%s: codec is NULL \n",  __func__);
		return 0;
    }

    iv_enable = ucontrol->value.integer.value[0];

	nResult = tas2562_iv_enable(pTAS2562, iv_enable);

	pr_debug("%s: tas2562iv_enable = %d\n", __func__, tas2562iv_enable);

	return nResult;
}

static int tas2562iv_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
   ucontrol->value.integer.value[0] = tas2562iv_enable;
   return 0;
}

static const struct snd_kcontrol_new tas2562_controls[] = {
SOC_ENUM_EXT("TAS2562 IVSENSE ENABLE", tas2562_enum[0],
		    tas2562iv_get, tas2562iv_put),
};

static int tas2562_codec_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	int nResult = 0;

	nResult = pTAS2562->write(pTAS2562, reg, value);
	if (nResult < 0) {
		dev_err(pTAS2562->dev, "%s, ERROR, reg=0x%x, E=%d\n",
			__func__, reg, nResult);
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(2));
	}
	else
		dev_dbg(pTAS2562->dev, "%s, reg: 0x%x, 0x%x\n",
			__func__, reg, value);

	return nResult;

}
static int tas2562_i2c_load_data(struct tas2562_priv *pTAS2562, unsigned int *pData)
{
	unsigned int nRegister;
	unsigned int *nData;
	unsigned char Buf[128];
	unsigned int nLength = 0;
	unsigned int i = 0;
	unsigned int nSize = 0;
	int nResult = 0;
	do {
		nRegister = pData[nLength];
		nSize = pData[nLength + 1];
		nData = &pData[nLength + 2];
		if (nRegister == TAS2562_MSLEEP) {
			msleep(nData[0]);
			dev_dbg(pTAS2562->dev, "%s, msleep = %d\n",
				__func__, nData[0]);
		} else if (nRegister == TAS2562_MDELAY) {
			mdelay(nData[0]);
			dev_dbg(pTAS2562->dev, "%s, mdelay = %d\n",
				__func__, nData[0]);
		} else {
			if (nRegister != 0xFFFFFFFF) {
				if (nSize > 128) {
					dev_err(pTAS2562->dev,
						"%s, Line=%d, invalid size, maximum is 128 bytes!\n",
						__func__, __LINE__);
					break;
				}
				if (nSize > 1) {
					for (i = 0; i < nSize; i++)
						Buf[i] = (unsigned char)nData[i];
					nResult = pTAS2562->bulk_write(pTAS2562, nRegister, Buf, nSize);
					if (nResult < 0)
						break;
				} else if (nSize == 1) {
					nResult = pTAS2562->write(pTAS2562, nRegister, nData[0]);
					if (nResult < 0)
						break;
				} else {
					dev_err(pTAS2562->dev,
						"%s, Line=%d,invalid size, minimum is 1 bytes!\n",
						__func__, __LINE__);
				}
			}
		}
		nLength = nLength + 2 + pData[nLength + 1];
	} while (nRegister != 0xFFFFFFFF);

	if(nResult < 0)
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(2));
	return nResult;
}
static int tas2562_codec_suspend(struct snd_soc_codec *codec)
{
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	mutex_lock(&pTAS2562->codec_lock);

	dev_dbg(pTAS2562->dev, "%s\n", __func__);
	pTAS2562->runtime_suspend(pTAS2562);

	mutex_unlock(&pTAS2562->codec_lock);
	return ret;
}

static int tas2562_codec_resume(struct snd_soc_codec *codec)
{
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	mutex_lock(&pTAS2562->codec_lock);

	dev_dbg(pTAS2562->dev, "%s\n", __func__);
	pTAS2562->runtime_resume(pTAS2562);

	mutex_unlock(&pTAS2562->codec_lock);
	return ret;
}

static const struct snd_kcontrol_new tas2562_asi_controls[] = {
	SOC_DAPM_SINGLE("Left", TAS2562_TDMConfigurationReg2,
		4, 1, 0),
	SOC_DAPM_SINGLE("Right", TAS2562_TDMConfigurationReg2,
		4, 2, 0),
	SOC_DAPM_SINGLE("LeftRightDiv2", TAS2562_TDMConfigurationReg2,
		4, 3, 0),
};

static int tas2562_set_power_state(struct tas2562_priv *pTAS2562, int state)
{
	int nResult = 0;
	/*unsigned int nValue;*/
	int irqreg;

	if ((pTAS2562->mbMute) && (state == TAS2562_POWER_ACTIVE))
		state = TAS2562_POWER_MUTE;
	dev_err(pTAS2562->dev, "set power state: %d\n", state);

	switch (state) {
	case TAS2562_POWER_ACTIVE:
		nResult = tas2562_load_init(pTAS2562);
		if (nResult < 0)
			return nResult;
        //if set format was not called by asoc, then set it default
		if(pTAS2562->mnASIFormat == 0)
			pTAS2562->mnASIFormat = SND_SOC_DAIFMT_CBS_CFS
				| SND_SOC_DAIFMT_IB_NF
				| SND_SOC_DAIFMT_I2S;

		nResult = tas2562_set_fmt(pTAS2562, pTAS2562->mnASIFormat);
		if (nResult < 0)
			goto activer_end;

//Clear latched IRQ before power on

		nResult = pTAS2562->update_bits(pTAS2562, TAS2562_InterruptConfiguration,
					TAS2562_InterruptConfiguration_LTCHINTClear_Mask,
					TAS2562_InterruptConfiguration_LTCHINTClear);
		if (nResult < 0)
			goto activer_end;

		nResult = pTAS2562->read(pTAS2562, TAS2562_LatchedInterruptReg0, &irqreg);
		if (nResult < 0)
			goto activer_end;
		dev_info(pTAS2562->dev, "IRQ reg is: %s %d, %d\n", __func__, irqreg, __LINE__);

activer_end:
		pTAS2562->mbPowerUp = true;
		pTAS2562->mnPowerState = TAS2562_POWER_ACTIVE;
/* irq routine will handle the error, and power on */
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(10));
		break;

	case TAS2562_POWER_MUTE:
		nResult = pTAS2562->update_bits(pTAS2562, TAS2562_PowerControl,
			TAS2562_PowerControl_OperationalMode10_Mask |
			TAS2562_PowerControl_ISNSPower_Mask |
			TAS2562_PowerControl_VSNSPower_Mask,
			TAS2562_PowerControl_OperationalMode10_Mute |
			TAS2562_PowerControl_VSNSPower_Active |
			TAS2562_PowerControl_ISNSPower_Active);
			pTAS2562->mbPowerUp = true;
			pTAS2562->mnPowerState = TAS2562_POWER_MUTE;
		break;

	case TAS2562_POWER_SHUTDOWN:
		//pTAS2562->enableIRQ(pTAS2562, false);

		nResult = pTAS2562->update_bits(pTAS2562, TAS2562_PowerControl,
			TAS2562_PowerControl_OperationalMode10_Mask,
			TAS2562_PowerControl_OperationalMode10_Shutdown);
			pTAS2562->mbPowerUp = false;
			pTAS2562->mnPowerState = TAS2562_POWER_SHUTDOWN;
		msleep(20);

		break;

	default:
		dev_err(pTAS2562->dev, "wrong power state setting %d\n", state);

	}

	if(nResult < 0)
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(10));
	return nResult;
}

static const struct snd_soc_dapm_widget tas2562_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("ASI1", "ASI1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("Voltage Sense", "ASI1 Capture",  1, TAS2562_PowerControl, 2, 1),
	SND_SOC_DAPM_AIF_OUT("Current Sense", "ASI1 Capture",  0, TAS2562_PowerControl, 3, 1),
	SND_SOC_DAPM_MIXER("ASI1 Sel",
		TAS2562_TDMConfigurationReg2, 4, 0,
		&tas2562_asi_controls[0],
		ARRAY_SIZE(tas2562_asi_controls)),
	SND_SOC_DAPM_DAC("DAC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_SIGGEN("VMON"),
	SND_SOC_DAPM_SIGGEN("IMON")
};

static const struct snd_soc_dapm_route tas2562_audio_map[] = {
	{"ASI1 Sel", "Left", "ASI1"},
	{"ASI1 Sel", "Right", "ASI1"},
	{"ASI1 Sel", "LeftRightDiv2", "ASI1"},
	{"DAC", NULL, "ASI1 Sel"},
	{"OUT", NULL, "DAC"},
	/*{"VMON", NULL, "Voltage Sense"},
	{"IMON", NULL, "Current Sense"},*/
	{"Voltage Sense", NULL, "VMON"},
	{"Current Sense", NULL, "IMON"},
};


static int tas2562_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	dev_dbg(pTAS2562->dev, "%s, %d \n", __func__, mute);

	mutex_lock(&pTAS2562->codec_lock);
	if (mute) {
		tas2562_set_power_state(pTAS2562, TAS2562_POWER_SHUTDOWN);
	} else {
		tas2562_set_power_state(pTAS2562, TAS2562_POWER_ACTIVE);
	}
	mutex_unlock(&pTAS2562->codec_lock);
	return 0;
}

static int tas2562_slot_config(struct snd_soc_codec *codec, struct tas2562_priv *pTAS2562, int blr_clk_ratio)
{
	int ret = 0;
	if(pTAS2562->mnSlot_width == 16)
 		ret = pTAS2562->update_bits(pTAS2562,
 			TAS2562_TDMConfigurationReg5, 0xff, 0x42);
	else
		ret = pTAS2562->update_bits(pTAS2562,
			TAS2562_TDMConfigurationReg5, 0xff, 0x44);
	if(ret < 0)
		goto end;

	pTAS2562->update_bits(pTAS2562,
			TAS2562_TDMConfigurationReg6, 0xff, 0x40);

end:
	return ret;
}

static int tas2562_set_slot(struct snd_soc_codec *codec, int slot_width)
{
	int ret = 0;
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	switch (slot_width) {
	case 16:
	ret = pTAS2562->update_bits(pTAS2562,
		TAS2562_TDMConfigurationReg2,
		TAS2562_TDMConfigurationReg2_RXSLEN10_Mask,
		TAS2562_TDMConfigurationReg2_RXSLEN10_16Bits);
	break;

	case 24:
	ret = pTAS2562->update_bits(pTAS2562,
		TAS2562_TDMConfigurationReg2,
		TAS2562_TDMConfigurationReg2_RXSLEN10_Mask,
		TAS2562_TDMConfigurationReg2_RXSLEN10_24Bits);
	break;

	case 32:
	ret = pTAS2562->update_bits(pTAS2562,
		TAS2562_TDMConfigurationReg2,
		TAS2562_TDMConfigurationReg2_RXSLEN10_Mask,
		TAS2562_TDMConfigurationReg2_RXSLEN10_32Bits);
	break;

	case 0:
	/* Do not change slot width */
	break;

	default:
		dev_err(pTAS2562->dev, "slot width not supported");
		ret = -EINVAL;
	}

	if (ret >= 0)
		pTAS2562->mnSlot_width = slot_width;

	return ret;
}

static int tas2562_set_bitwidth(struct tas2562_priv *pTAS2562, int bitwidth)
{
	int slot_width_tmp = 16;
	int ret = 0;
	dev_info(pTAS2562->dev, "%s %d\n", __func__, bitwidth);

	switch (bitwidth) {
	case SNDRV_PCM_FORMAT_S16_LE:
		ret = pTAS2562->update_bits(pTAS2562,
			TAS2562_TDMConfigurationReg2,
			TAS2562_TDMConfigurationReg2_RXWLEN32_Mask,
			TAS2562_TDMConfigurationReg2_RXWLEN32_16Bits);
			pTAS2562->mnCh_size = 16;
				slot_width_tmp = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ret = pTAS2562->update_bits(pTAS2562,
			TAS2562_TDMConfigurationReg2,
			TAS2562_TDMConfigurationReg2_RXWLEN32_Mask,
			TAS2562_TDMConfigurationReg2_RXWLEN32_24Bits);
			pTAS2562->mnCh_size = 24;
				slot_width_tmp = 32;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		ret = pTAS2562->update_bits(pTAS2562,
			TAS2562_TDMConfigurationReg2,
			TAS2562_TDMConfigurationReg2_RXWLEN32_Mask,
			TAS2562_TDMConfigurationReg2_RXWLEN32_32Bits);
			pTAS2562->mnCh_size = 32;
				slot_width_tmp = 32;
		break;

	default:
		dev_info(pTAS2562->dev, "Not supported params format\n");
	}

	/* If machine driver did not call set slot width */
	//if (pTAS2562->mnSlot_width == 0)
	if (ret < 0)
		goto end;
	ret = tas2562_set_slot(pTAS2562->codec, slot_width_tmp);

end:
	dev_info(pTAS2562->dev, "mnCh_size: %d,  slot_width_tmp: %d\n", pTAS2562->mnCh_size, slot_width_tmp);
	pTAS2562->mnPCMFormat = bitwidth;

	return ret;
}

static int tas2562_set_samplerate(struct tas2562_priv *pTAS2562, int samplerate)
{
	int ret = 0;
	switch (samplerate) {
	case 48000:
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_48KHz);
		if(ret < 0)
			goto end;
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_44_1_48kHz);
		if(ret < 0)
			goto end;
		break;
	case 44100:
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_44_1KHz);
		if(ret < 0)
			goto end;
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_44_1_48kHz);
		if(ret < 0)
			goto end;
		break;
	case 96000:
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_48KHz);
		if(ret < 0)
			goto end;
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_88_2_96kHz);
		if(ret < 0)
			goto end;
		break;
	case 88200:
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_44_1KHz);
		if(ret < 0)
			goto end;
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_88_2_96kHz);
		if(ret < 0)
			goto end;
		break;
	case 19200:
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_48KHz);
		if(ret < 0)
			goto end;
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_176_4_192kHz);
		if(ret < 0)
			goto end;
		break;
	case 17640:
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATERAMP_44_1KHz);
		if(ret < 0)
			goto end;
		ret = pTAS2562->update_bits(pTAS2562,
				TAS2562_TDMConfigurationReg0,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2562_TDMConfigurationReg0_SAMPRATE31_176_4_192kHz);
		if(ret < 0)
			goto end;
		break;
	default:
			dev_info(pTAS2562->dev, "%s, unsupported sample rate, %d\n", __func__, samplerate);

	}

end:
	pTAS2562->mnSamplingRate = samplerate;
	return ret;
}
static int tas2562_mute_ctrl_get(struct snd_kcontrol *pKcontrol,
	struct snd_ctl_elem_value *pValue)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(pKcontrol);
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	pValue->value.integer.value[0] = pTAS2562->mbMute;
	dev_dbg(pTAS2562->dev, "tas2562_mute_ctrl_get = %d\n",
		pTAS2562->mbMute);

	return 0;
}

static int tas2562_mute_ctrl_put(struct snd_kcontrol *pKcontrol,
	struct snd_ctl_elem_value *pValue)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(pKcontrol);
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	mbMute = pValue->value.integer.value[0];

	dev_dbg(pTAS2562->dev, "tas2562_mute_ctrl_put = %d\n", mbMute);

	pTAS2562->mbMute = !!mbMute;

	return 0;
}

static int tas2562_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);
	int blr_clk_ratio;
	int ret = 0;

	dev_dbg(pTAS2562->dev, "%s, format: %d\n", __func__,
		params_format(params));

	mutex_lock(&pTAS2562->codec_lock);

	ret = tas2562_set_bitwidth(pTAS2562, params_format(params));
	if(ret < 0)
	{
		dev_info(pTAS2562->dev, "set bitwidth failed, %d\n", ret);
		goto end;
	}

	blr_clk_ratio = params_channels(params) * pTAS2562->mnCh_size;
	dev_info(pTAS2562->dev, "blr_clk_ratio: %d\n", blr_clk_ratio);
	if(blr_clk_ratio != 0) {
		ret = tas2562_slot_config(pTAS2562->codec, pTAS2562, blr_clk_ratio);
		if(ret < 0)
			goto end;
	}

	dev_info(pTAS2562->dev, "%s, sample rate: %d\n", __func__,
		params_rate(params));

	ret = tas2562_set_samplerate(pTAS2562, params_rate(params));

end:
	mutex_unlock(&pTAS2562->codec_lock);
	if(pTAS2562->mnErrCode & ERROR_DEVA_I2C_COMM)
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(10));
	return 0;
}

static int tas2562_set_fmt(struct tas2562_priv *pTAS2562, unsigned int fmt)
{
	u8 tdm_rx_start_slot = 0, asi_cfg_1 = 0;
	int ret = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		asi_cfg_1 = 0x00;
		break;
	default:
		dev_err(pTAS2562->dev, "ASI format master is not found\n");
		ret = -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		dev_info(pTAS2562->dev, "INV format: NBNF\n");
		asi_cfg_1 |= TAS2562_TDMConfigurationReg1_RXEDGE_Rising;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		dev_info(pTAS2562->dev, "INV format: IBNF\n");
		asi_cfg_1 |= TAS2562_TDMConfigurationReg1_RXEDGE_Falling;
		break;
	default:
		dev_err(pTAS2562->dev, "ASI format Inverse is not found\n");
		ret = -EINVAL;
	}

	ret = pTAS2562->update_bits(pTAS2562, TAS2562_TDMConfigurationReg1,
		TAS2562_TDMConfigurationReg1_RXEDGE_Mask,
		asi_cfg_1);
	if(ret < 0)
		goto end;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case (SND_SOC_DAIFMT_I2S):
		tdm_rx_start_slot = 1;
		break;
	case (SND_SOC_DAIFMT_DSP_A):
	case (SND_SOC_DAIFMT_DSP_B):
		tdm_rx_start_slot = 1;
		break;
	case (SND_SOC_DAIFMT_LEFT_J):
		tdm_rx_start_slot = 0;
		break;
	default:
		dev_err(pTAS2562->dev, "DAI Format is not found, fmt=0x%x\n", fmt);
		ret = -EINVAL;
		break;
	}

	ret = pTAS2562->update_bits(pTAS2562, TAS2562_TDMConfigurationReg1,
		TAS2562_TDMConfigurationReg1_RXOFFSET51_Mask,
		(tdm_rx_start_slot << TAS2562_TDMConfigurationReg1_RXOFFSET51_Shift));
	if(ret < 0)
		goto end;
	ret = pTAS2562->write(pTAS2562, TAS2562_TDMConfigurationReg4, 0x01);
	if(ret < 0)
		goto end;

	pTAS2562->mnASIFormat = fmt;

end:
	return ret;
}

static int tas2562_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	dev_dbg(pTAS2562->dev, "%s, format=0x%x\n", __func__, fmt);

	ret = tas2562_set_fmt(pTAS2562, fmt);
	if(ret < 0)
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(10));
	return 0;
}

static int tas2562_set_dai_tdm_slot(struct snd_soc_dai *dai,
		unsigned int tx_mask, unsigned int rx_mask,
		int slots, int slot_width)
{
	int ret = 0;
	struct snd_soc_codec *codec = dai->codec;
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	dev_dbg(pTAS2562->dev, "%s, tx_mask:%d, rx_mask:%d, slots:%d, slot_width:%d",
			__func__, tx_mask, rx_mask, slots, slot_width);

	ret = tas2562_set_slot(codec, slot_width);
	if(ret < 0)
		schedule_delayed_work(&pTAS2562->irq_work, msecs_to_jiffies(10));

	return 0;
}

static struct snd_soc_dai_ops tas2562_dai_ops = {
	.digital_mute = tas2562_mute,
	.hw_params  = tas2562_hw_params,
	.set_fmt    = tas2562_set_dai_fmt,
	.set_tdm_slot = tas2562_set_dai_tdm_slot,
};

#define TAS2562_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
		SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

#define TAS2562_RATES (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 \
						SNDRV_PCM_RATE_88200 |\
						SNDRV_PCM_RATE_96000 |\
						SNDRV_PCM_RATE_176400 |\
						SNDRV_PCM_RATE_192000\
						)

static struct snd_soc_dai_driver tas2562_dai_driver[] = {
	{
		.name = "tas2562 ASI1",
		.id = 0,
		.playback = {
			.stream_name    = "ASI1 Playback",
			.channels_min   = 2,
			.channels_max   = 2,
			.rates      = SNDRV_PCM_RATE_8000_192000,
			.formats    = TAS2562_FORMATS,
		},
		.capture = {
			.stream_name    = "ASI1 Capture",
			.channels_min   = 0,
			.channels_max   = 2,
			.rates          = SNDRV_PCM_RATE_8000_192000,
			.formats    = TAS2562_FORMATS,
		},
		.ops = &tas2562_dai_ops,
		.symmetric_rates = 1,
	},
};

static int tas2562_load_init(struct tas2562_priv *pTAS2562)
{
	int ret;

#ifdef TAS2558_CODEC
/* Max voltage to 9V */
	ret = pTAS2562->update_bits(pTAS2562, TAS2562_BoostConfiguration2,
					TAS2562_BoostConfiguration2_BoostMaxVoltage_Mask,
					0x7);
	if(ret < 0)
		return ret;

        ret = pTAS2562->update_bits(pTAS2562, TAS2562_PlaybackConfigurationReg0,
                                        TAS2562_PlaybackConfigurationReg0_AmplifierLevel51_Mask,
                                        0xd << 1);
        if(ret < 0)
                return ret;

#endif

	ret = pTAS2562->write(pTAS2562, TAS2562_MiscConfigurationReg0, 0xcf);
	if(ret < 0)
		return ret;
	ret = pTAS2562->write(pTAS2562, TAS2562_TDMConfigurationReg4, 0x01);
	if(ret < 0)
		return ret;
	ret = pTAS2562->write(pTAS2562, TAS2562_ClockConfiguration, 0x0c);
	if(ret < 0)
		return ret;
	ret = tas2562_i2c_load_data(pTAS2562, p_tas2562_classH_D_data);

	return ret;
}

static int tas2562_codec_probe(struct snd_soc_codec *codec)
{
	int ret;
	struct tas2562_priv *pTAS2562 = snd_soc_codec_get_drvdata(codec);

	ret = snd_soc_add_codec_controls(codec, tas2562_controls,
					 ARRAY_SIZE(tas2562_controls));
	if (ret < 0) {
		pr_err("%s: add_codec_controls failed, err %d\n",
			__func__, ret);
		return ret;
	}

	tas2562_load_init(pTAS2562);
	tas2562_iv_enable(pTAS2562, 1);
	pTAS2562->codec = codec;
	dev_err(pTAS2562->dev, "%s\n", __func__);

	return 0;
}

static int tas2562_codec_remove(struct snd_soc_codec *codec)
{
	return 0;
}

static DECLARE_TLV_DB_SCALE(tas2562_digital_tlv, 1100, 50, 0);

static const struct snd_kcontrol_new tas2562_snd_controls[] = {
	SOC_SINGLE_TLV("Amp Output Level", TAS2562_PlaybackConfigurationReg0,
		0, 0x16, 0,
		tas2562_digital_tlv),
	SOC_SINGLE_EXT("SmartPA Mute", SND_SOC_NOPM, 0, 0x0001, 0,
			tas2562_mute_ctrl_get, tas2562_mute_ctrl_put),
};

static struct snd_soc_codec_driver soc_codec_driver_tas2562 = {
	.probe			= tas2562_codec_probe,
	.remove			= tas2562_codec_remove,
	.read			= tas2562_codec_read,
	.write			= tas2562_codec_write,
	.suspend		= tas2562_codec_suspend,
	.resume			= tas2562_codec_resume,
	.component_driver = {
	.controls		= tas2562_snd_controls,
	.num_controls		= ARRAY_SIZE(tas2562_snd_controls),
		.dapm_widgets		= tas2562_dapm_widgets,
		.num_dapm_widgets	= ARRAY_SIZE(tas2562_dapm_widgets),
		.dapm_routes		= tas2562_audio_map,
		.num_dapm_routes	= ARRAY_SIZE(tas2562_audio_map),
	},
};

int tas2562_register_codec(struct tas2562_priv *pTAS2562)
{
	int nResult = 0;

	dev_info(pTAS2562->dev, "%s, enter\n", __func__);
	nResult = snd_soc_register_codec(pTAS2562->dev,
		&soc_codec_driver_tas2562,
		tas2562_dai_driver, ARRAY_SIZE(tas2562_dai_driver));
	return nResult;
}

int tas2562_deregister_codec(struct tas2562_priv *pTAS2562)
{
	snd_soc_unregister_codec(pTAS2562->dev);

	return 0;
}

void tas2562_LoadConfig(struct tas2562_priv *pTAS2562)
{
	int ret = 0;

	pTAS2562->hw_reset(pTAS2562);
	msleep(2);
	pTAS2562->write(pTAS2562, TAS2562_SoftwareReset,
			TAS2562_SoftwareReset_SoftwareReset_Reset);
	msleep(3);

	ret = tas2562_slot_config(pTAS2562->codec, pTAS2562, 1);
	if(ret < 0) {
		goto end;
	}

	tas2562_load_init(pTAS2562);
	tas2562_iv_enable(pTAS2562, 1);

	ret = tas2562_set_slot(pTAS2562->codec, pTAS2562->mnSlot_width);
	if (ret < 0)
		goto end;

	ret = tas2562_set_fmt(pTAS2562, pTAS2562->mnASIFormat);
	if (ret < 0)
		goto end;

	ret = tas2562_set_bitwidth(pTAS2562, pTAS2562->mnPCMFormat);
	if (ret < 0)
		goto end;

	ret = tas2562_set_samplerate(pTAS2562, pTAS2562->mnSamplingRate);
	if (ret < 0)
		goto end;

	ret = tas2562_set_power_state(pTAS2562, pTAS2562->mnPowerState);
	if (ret < 0)
		goto end;

end:
/* power up failed, restart later */
	if (ret < 0)
		schedule_delayed_work(&pTAS2562->irq_work,
				msecs_to_jiffies(1000));
}

MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("TAS2562 ALSA SOC Smart Amplifier driver");
MODULE_LICENSE("GPL v2");
#endif /* CONFIG_TAS2562_CODEC */
