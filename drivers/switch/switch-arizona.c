/*
 * extcon-arizona.c - Extcon driver Wolfson Arizona devices
 *
 *  Copyright (C) 2012-2014 Wolfson Microelectronics plc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/switch.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/switch-arizona.h>

#include <sound/soc.h>

#include <linux/mfd/arizona/core.h>
#include <linux/mfd/arizona/pdata.h>
#include <linux/mfd/arizona/registers.h>

#define ARIZONA_MAX_MICD_RANGE 8

#define ARIZONA_ACCDET_MODE_MIC     0
#define ARIZONA_ACCDET_MODE_HPL     1
#define ARIZONA_ACCDET_MODE_HPR     2
#define ARIZONA_ACCDET_MODE_HPM     4
#define ARIZONA_ACCDET_MODE_ADC     7
#define ARIZONA_ACCDET_MODE_INVALID 8

#define ARIZONA_MICD_CLAMP_MODE_JDL      0x4
#define ARIZONA_MICD_CLAMP_MODE_JDH      0x5
#define ARIZONA_MICD_CLAMP_MODE_JDL_GP5H 0x9
#define ARIZONA_MICD_CLAMP_MODE_JDH_GP5H 0xb

#define ARIZONA_HPDET_MAX 10000

#define HPDET_DEBOUNCE 500
#define DEFAULT_MICD_TIMEOUT 2000

#define QUICK_HEADPHONE_MAX_OHM 3
#define MICROPHONE_MIN_OHM      1257
#define MICROPHONE_MAX_OHM      30000

#define HP_NORMAL_IMPEDANCE     0
#define HP_LOW_IMPEDANCE        1

#define HP_LOW_IMPEDANCE_LIMIT 13

struct arizona_extcon_info {
	struct device *dev;
	struct arizona *arizona;
	struct mutex lock;
	struct regulator *micvdd;
	struct input_dev *input;

	u16 last_jackdet;

	int micd_mode;
	const struct arizona_micd_config *micd_modes;
	int micd_num_modes;

	const struct arizona_micd_range *micd_ranges;
	int num_micd_ranges;

	bool micd_reva;
	bool micd_clamp;

	struct delayed_work hpdet_work;
	struct delayed_work micd_detect_work;
	struct delayed_work micd_clear_work;
	bool first_clear;

	bool hpdet_retried;
	int hp_imp_level;

	int num_hpdet_res;
	unsigned int hpdet_res[3];

	bool mic;
	bool detecting;
	int jack_flips;

	int hpdet_ip;

	struct switch_dev edev;

	const struct arizona_jd_state *state;
	struct delayed_work state_timeout_work;

	struct wakeup_source detection_wake_lock;
};

static const struct arizona_micd_config micd_default_modes[] = {
	{ ARIZONA_ACCDET_SRC, 1, 0 },
	{ 0,                  2, 1 },
};

static const struct arizona_micd_range micd_default_ranges[] = {
	{ .max =  11, .key = BTN_0 },
	{ .max =  28, .key = BTN_1 },
	{ .max =  54, .key = BTN_2 },
	{ .max = 100, .key = BTN_3 },
	{ .max = 186, .key = BTN_4 },
	{ .max = 430, .key = BTN_5 },
};

/* The number of levels in arizona_micd_levels valid for button thresholds */
#define ARIZONA_NUM_MICD_BUTTON_LEVELS 64

static const int arizona_micd_levels[] = {
	3, 6, 8, 11, 13, 16, 18, 21, 23, 26, 28, 31, 34, 36, 39, 41, 44, 46,
	49, 52, 54, 57, 60, 62, 65, 67, 70, 73, 75, 78, 81, 83, 89, 94, 100,
	105, 111, 116, 122, 127, 139, 150, 161, 173, 186, 196, 209, 220, 245,
	270, 295, 321, 348, 375, 402, 430, 489, 550, 614, 681, 752, 903, 1071,
	1257, 30000,
};

/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static ssize_t arizona_extcon_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf);
DEVICE_ATTR(hp_impedance, S_IRUGO, arizona_extcon_show, NULL);

inline void arizona_extcon_report(struct arizona_extcon_info *info, int state)
{
	switch_set_state(&info->edev, state);
}
EXPORT_SYMBOL_GPL(arizona_extcon_report);

static const struct arizona_jd_state arizona_hpdet_moisture;
static const struct arizona_jd_state arizona_hpdet_acc_id;

static int arizona_jds_get_mode(struct arizona_extcon_info *info)
{
	int mode = ARIZONA_ACCDET_MODE_INVALID;

	if (info->state)
		mode = info->state->mode;

	return mode;
}

int arizona_jds_set_state(struct arizona_extcon_info *info,
			  const struct arizona_jd_state *new_state)
{
	int ret = 0;

	if (new_state != info->state) {
		if (info->state)
			info->state->stop(info);

		info->state = new_state;

		if (info->state) {
			ret = info->state->start(info);
			if (ret < 0)
				info->state = NULL;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(arizona_jds_set_state);

static void arizona_jds_reading(struct arizona_extcon_info *info, int val)
{
	int ret;

	ret = info->state->reading(info, val);

	if (ret == -EAGAIN && info->state->restart)
		info->state->restart(info);
}

static inline bool arizona_jds_cancel_timeout(struct arizona_extcon_info *info)
{
	return cancel_delayed_work_sync(&info->state_timeout_work);
}

static void arizona_jds_start_timeout(struct arizona_extcon_info *info)
{
	const struct arizona_jd_state *state = info->state;

	if (!state)
		return;

	if (state->timeout_ms && state->timeout) {
		int ms = state->timeout_ms(info);

		schedule_delayed_work(&info->state_timeout_work,
				      msecs_to_jiffies(ms));
	}
}

static void arizona_jds_timeout_work(struct work_struct *work)
{
	struct arizona_extcon_info *info =
		container_of(work, struct arizona_extcon_info,
			     state_timeout_work.work);

	mutex_lock(&info->lock);

	info->state->timeout(info);
	arizona_jds_start_timeout(info);

	mutex_unlock(&info->lock);
}

static void arizona_extcon_hp_clamp(struct arizona_extcon_info *info,
				    bool clamp)
{
	struct arizona *arizona = info->arizona;
	unsigned int mask, val = 0;
	int ret;

	switch (arizona->type) {
	case WM1814:
	case WM8998:
		mask = 0;
		break;
	case WM8280:
	case WM5110:
		mask = ARIZONA_HP1L_SHRTO | ARIZONA_HP1L_FLWR |
		       ARIZONA_HP1L_SHRTI;
		if (clamp)
			val = ARIZONA_HP1L_SHRTO;
		else
			val = ARIZONA_HP1L_FLWR | ARIZONA_HP1L_SHRTI;
		break;
	default:
		mask = ARIZONA_RMV_SHRT_HP1L;
		if (clamp)
			val = ARIZONA_RMV_SHRT_HP1L;
		break;
	};

	mutex_lock(&arizona->dapm->card->dapm_mutex);

	arizona->hpdet_clamp = clamp;

	/* Keep the HP output stages disabled while doing the clamp */
	if (clamp) {
		ret = regmap_update_bits(arizona->regmap,
					 ARIZONA_OUTPUT_ENABLES_1,
					 ARIZONA_OUT1L_ENA |
					 ARIZONA_OUT1R_ENA, 0);
		if (ret != 0)
			dev_warn(arizona->dev,
				"Failed to disable headphone outputs: %d\n",
				 ret);
	}

	if (mask) {
		ret = regmap_update_bits(arizona->regmap, ARIZONA_HP_CTRL_1L,
					 mask, val);
		if (ret != 0)
			dev_warn(arizona->dev, "Failed to do clamp: %d\n",
				 ret);

		ret = regmap_update_bits(arizona->regmap, ARIZONA_HP_CTRL_1R,
					 mask, val);
		if (ret != 0)
			dev_warn(arizona->dev, "Failed to do clamp: %d\n",
				 ret);
	}

	/* Restore the desired state while not doing the clamp */
	if (!clamp && (arizona->hp_impedance > arizona->pdata.hpdet_short_circuit_imp)) {
		ret = regmap_update_bits(arizona->regmap,
					 ARIZONA_OUTPUT_ENABLES_1,
					 ARIZONA_OUT1L_ENA |
					 ARIZONA_OUT1R_ENA, arizona->hp_ena);
		if (ret != 0)
			dev_warn(arizona->dev,
				 "Failed to restore headphone outputs: %d\n",
				 ret);
	}

	mutex_unlock(&arizona->dapm->card->dapm_mutex);
}

static void arizona_extcon_set_mode(struct arizona_extcon_info *info, int mode)
{
	struct arizona *arizona = info->arizona;

	mode %= info->micd_num_modes;

	if (arizona->pdata.micd_pol_gpio > 0)
		gpio_set_value_cansleep(arizona->pdata.micd_pol_gpio,
					info->micd_modes[mode].gpio);
	regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
			   ARIZONA_MICD_BIAS_SRC_MASK,
			   info->micd_modes[mode].bias <<
			   ARIZONA_MICD_BIAS_SRC_SHIFT);
	regmap_update_bits(arizona->regmap, ARIZONA_ACCESSORY_DETECT_MODE_1,
			   ARIZONA_ACCDET_SRC, info->micd_modes[mode].src);

	info->micd_mode = mode;

	dev_dbg(arizona->dev, "Set jack polarity to %d\n", mode);
}

static const char *arizona_extcon_get_micbias(struct arizona_extcon_info *info)
{
	switch (info->micd_modes[0].bias) {
	case 1:
		return "MICBIAS1";
	case 2:
		return "MICBIAS2";
	case 3:
		return "MICBIAS3";
	default:
		return "MICVDD";
	}
}

static void arizona_extcon_pulse_micbias(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	const char *widget = arizona_extcon_get_micbias(info);
	struct snd_soc_dapm_context *dapm = arizona->dapm;
	int ret;

	mutex_lock(&dapm->card->dapm_mutex);

	ret = snd_soc_dapm_force_enable_pin(dapm, widget);
	if (ret != 0)
		dev_warn(arizona->dev, "Failed to enable %s: %d\n",
			 widget, ret);

	mutex_unlock(&dapm->card->dapm_mutex);

	snd_soc_dapm_sync(dapm);

	if (arizona->pdata.micd_force_micbias_initial && info->detecting)
		return;

	if (!arizona->pdata.micd_force_micbias) {
		mutex_lock(&dapm->card->dapm_mutex);

		ret = snd_soc_dapm_disable_pin(arizona->dapm, widget);
		if (ret != 0)
			dev_warn(arizona->dev, "Failed to disable %s: %d\n",
				 widget, ret);

		mutex_unlock(&dapm->card->dapm_mutex);

		snd_soc_dapm_sync(dapm);
	}
}

static int arizona_micd_read(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	unsigned int val = 0;
	int ret, i;

	if (info->detecting && arizona->pdata.micd_software_compare) {
		bool micd_ena;
		unsigned int micd_ena_bit;

		/* Must disable MICD before we read the ADCVAL */
		ret = regmap_update_bits_check(arizona->regmap,
					       ARIZONA_MIC_DETECT_1,
					       ARIZONA_MICD_ENA, 0,
					       &micd_ena);
		if (ret != 0) {
			dev_err(arizona->dev,
				"Failed to disable MICD: %d\n",
				ret);
			return ret;
		}

		ret = regmap_read(arizona->regmap, ARIZONA_MIC_DETECT_4, &val);
		if (ret != 0) {
			dev_err(arizona->dev,
				"Failed to read MICDET_ADCVAL: %d\n",
				ret);
			return ret;
		}

		dev_dbg(arizona->dev, "MICDET_ADCVAL: 0x%x\n", val);

		val &= ARIZONA_MICDET_ADCVAL_MASK;
		if (val < ARRAY_SIZE(arizona_micd_levels))
			val = arizona_micd_levels[val];
		else
			val = INT_MAX;

		if (val <= QUICK_HEADPHONE_MAX_OHM)
			val = ARIZONA_MICD_STS | ARIZONA_MICD_LVL_0;
		else if (val <= MICROPHONE_MIN_OHM)
			val = ARIZONA_MICD_STS | ARIZONA_MICD_LVL_1;
		else if (val <= MICROPHONE_MAX_OHM)
			val = ARIZONA_MICD_STS | ARIZONA_MICD_LVL_8;
		else
			val = ARIZONA_MICD_LVL_8;

		if (micd_ena)
			micd_ena_bit = ARIZONA_MICD_ENA;
		else
			micd_ena_bit = 0;

		ret = regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
					 ARIZONA_MICD_ENA, micd_ena_bit);
		if (ret != 0) {
			dev_err(arizona->dev,
				"Failed to restore MICD: %d\n",
				ret);
			return ret;
		}

		return val;
	}

	for (i = 0; i < 10 && !(val & MICD_LVL_0_TO_8); i++) {
		ret = regmap_read(arizona->regmap, ARIZONA_MIC_DETECT_3, &val);
		if (ret != 0) {
			dev_err(arizona->dev,
				"Failed to read MICDET: %d\n",
				ret);
			return ret;
		}

		dev_dbg(arizona->dev, "MICDET: 0x%x\n", val);

		if (!(val & ARIZONA_MICD_VALID)) {
			dev_warn(arizona->dev,
				 "Microphone detection state invalid\n");
			return -EINVAL;
		}
	}

	if (i == 10 && !(val & MICD_LVL_0_TO_8)) {
		dev_err(arizona->dev, "Failed to get valid MICDET value\n");
		return -EINVAL;
	}

	return val;
}

static struct {
	unsigned int threshold;
	unsigned int factor_a;
	unsigned int factor_b;
} arizona_hpdet_b_ranges[] = {
	{ 100,  5528,   362464 },
	{ 169, 11084,  6186851 },
	{ 169, 11065, 65460395 },
};

#define ARIZONA_HPDET_B_RANGE_MAX 0x3fb

static struct {
	int min;
	int max;
} arizona_hpdet_c_ranges[] = {
	{ 0,       30 },
	{ 8,      100 },
	{ 100,   1000 },
	{ 1000, 10000 },
};

static int arizona_hpdet_read(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	unsigned int val, range;
	int ret;

	ret = regmap_read(arizona->regmap, ARIZONA_HEADPHONE_DETECT_2, &val);
	if (ret != 0) {
		dev_err(arizona->dev, "Failed to read HPDET status: %d\n",
			ret);
		return ret;
	}

	switch (info->hpdet_ip) {
	case 0:
		if (!(val & ARIZONA_HP_DONE)) {
			dev_err(arizona->dev, "HPDET did not complete: %x\n",
				val);
			return -EAGAIN;
		}

		val &= ARIZONA_HP_LVL_MASK;
		break;

	case 1:
		if (!(val & ARIZONA_HP_DONE_B)) {
			dev_err(arizona->dev, "HPDET did not complete: %x\n",
				val);
			return -EAGAIN;
		}

		ret = regmap_read(arizona->regmap, ARIZONA_HP_DACVAL, &val);
		if (ret != 0) {
			dev_err(arizona->dev, "Failed to read HP value: %d\n",
				ret);
			return -EAGAIN;
		}

		regmap_read(arizona->regmap, ARIZONA_HEADPHONE_DETECT_1,
			    &range);
		range = (range & ARIZONA_HP_IMPEDANCE_RANGE_MASK)
			   >> ARIZONA_HP_IMPEDANCE_RANGE_SHIFT;

		if (range < ARRAY_SIZE(arizona_hpdet_b_ranges) - 1 &&
		    (val < arizona_hpdet_b_ranges[range].threshold ||
		     val >= ARIZONA_HPDET_B_RANGE_MAX)) {
			range++;
			dev_dbg(arizona->dev, "Moving to HPDET range %d\n",
				range);
			regmap_update_bits(arizona->regmap,
					   ARIZONA_HEADPHONE_DETECT_1,
					   ARIZONA_HP_IMPEDANCE_RANGE_MASK,
					   range <<
					   ARIZONA_HP_IMPEDANCE_RANGE_SHIFT);
			return -EAGAIN;
		}

		/* If we go out of range report top of range */
		if (val < arizona_hpdet_b_ranges[range].threshold ||
		    val >= ARIZONA_HPDET_B_RANGE_MAX) {
			dev_dbg(arizona->dev, "Measurement out of range\n");
			return ARIZONA_HPDET_MAX;
		}

		dev_dbg(arizona->dev, "HPDET read %d in range %d\n",
			val, range);

		val = arizona_hpdet_b_ranges[range].factor_b
			/ ((val * 100) -
			   arizona_hpdet_b_ranges[range].factor_a);
		break;

	default:
		dev_warn(arizona->dev, "Unknown HPDET IP revision %d\n",
			 info->hpdet_ip);
	case 2:
		if (!(val & ARIZONA_HP_DONE_B)) {
			dev_err(arizona->dev, "HPDET did not complete: %x\n",
				val);
			return -EAGAIN;
		}

		val &= ARIZONA_HP_LVL_B_MASK;
		/* Convert to ohms, the value is in 0.5 ohm increments */
		val /= 2;

		regmap_read(arizona->regmap, ARIZONA_HEADPHONE_DETECT_1,
			    &range);
		range = (range & ARIZONA_HP_IMPEDANCE_RANGE_MASK)
			   >> ARIZONA_HP_IMPEDANCE_RANGE_SHIFT;

		/* Skip up a range, or report? */
		if (range < ARRAY_SIZE(arizona_hpdet_c_ranges) - 1 &&
		    (val >= arizona_hpdet_c_ranges[range].max)) {
			range++;
			dev_dbg(arizona->dev, "Moving to HPDET range %d-%d\n",
				arizona_hpdet_c_ranges[range].min,
				arizona_hpdet_c_ranges[range].max);
			regmap_update_bits(arizona->regmap,
					   ARIZONA_HEADPHONE_DETECT_1,
					   ARIZONA_HP_IMPEDANCE_RANGE_MASK,
					   range <<
					   ARIZONA_HP_IMPEDANCE_RANGE_SHIFT);
			return -EAGAIN;
		}

		if (range && (val < arizona_hpdet_c_ranges[range].min)) {
			dev_dbg(arizona->dev, "Reporting range boundary %d\n",
				arizona_hpdet_c_ranges[range].min);
			val = arizona_hpdet_c_ranges[range].min;
		}
	}

	if (info->arizona->pdata.hpdet_ext_res) {

		if (info->arizona->pdata.hpdet_ext_res >=  val) {
			dev_err(arizona->dev,
				"External resistor (%d) >= measurement (%d)\n",
				info->arizona->pdata.hpdet_ext_res,
				val);
		} else {
			dev_dbg(arizona->dev,
				"Compensating for external %d ohm resistor\n",
				info->arizona->pdata.hpdet_ext_res);

			val -= info->arizona->pdata.hpdet_ext_res;
		}
	}

	dev_dbg(arizona->dev, "HP impedance %d ohms\n", val);


	return val;
}

static const struct reg_default wm5110_low_impedance_patch[] = {
	{ 0x460, 0x0C21 },
	{ 0x461, 0xA000 },
	{ 0x462, 0x0C41 },
	{ 0x463, 0x50E5 },
	{ 0x464, 0x0C41 },
	{ 0x465, 0x4040 },
	{ 0x466, 0x0C41 },
	{ 0x467, 0x3940 },
	{ 0x468, 0x0C41 },
	{ 0x469, 0x2418 },
	{ 0x46A, 0x0846 },
	{ 0x46B, 0x1990 },
	{ 0x46C, 0x08C6 },
	{ 0x46D, 0x1450 },
	{ 0x46E, 0x04CE },
	{ 0x46F, 0x1020 },
	{ 0x470, 0x04CE },
	{ 0x471, 0x0CD0 },
	{ 0x472, 0x04CE },
	{ 0x473, 0x0A30 },
	{ 0x474, 0x044E },
	{ 0x475, 0x0660 },
	{ 0x476, 0x044E },
	{ 0x477, 0x0510 },
	{ 0x478, 0x04CE },
	{ 0x479, 0x0400 },
	{ 0x47A, 0x04CE },
	{ 0x47B, 0x0330 },
	{ 0x47C, 0x05DF },
	{ 0x47D, 0x0001 },
	{ 0x47E, 0x07FF },
	{ 0x483, 0x0021 },
};

static const struct reg_default wm5110_normal_impedance_patch[] = {
	{ 0x460, 0x0C40 },
	{ 0x461, 0xA000 },
	{ 0x462, 0x0C42 },
	{ 0x463, 0x50E5 },
	{ 0x464, 0x0842 },
	{ 0x465, 0x4040 },
	{ 0x466, 0x0842 },
	{ 0x467, 0x3940 },
	{ 0x468, 0x0846 },
	{ 0x469, 0x2418 },
	{ 0x46A, 0x0442 },
	{ 0x46B, 0x1990 },
	{ 0x46C, 0x04C6 },
	{ 0x46D, 0x1450 },
	{ 0x46E, 0x04CE },
	{ 0x46F, 0x1020 },
	{ 0x470, 0x04CE },
	{ 0x471, 0x0CD0 },
	{ 0x472, 0x04CE },
	{ 0x473, 0x0A30 },
	{ 0x474, 0x044E },
	{ 0x475, 0x0660 },
	{ 0x476, 0x044E },
	{ 0x477, 0x0510 },
	{ 0x478, 0x04CE },
	{ 0x479, 0x0400 },
	{ 0x47A, 0x04CE },
	{ 0x47B, 0x0330 },
	{ 0x47C, 0x05DF },
	{ 0x47D, 0x0001 },
	{ 0x47E, 0x07FF },
	{ 0x483, 0x0021 },
};

static const struct reg_default wm1814_low_impedance_patch[] = {
	{ 0x46C, 0x0C01 },
	{ 0x46E, 0x0C01 },
	{ 0x470, 0x0C01 },
};

static const struct reg_default wm1814_normal_impedance_patch[] = {
	{ 0x46C, 0x0801 },
	{ 0x46E, 0x0801 },
	{ 0x470, 0x0801 },
};

int arizona_wm5110_tune_headphone(struct arizona_extcon_info *info,
				  int reading)
{
	struct arizona *arizona = info->arizona;
	const struct reg_default *patch;
	int i, ret, size;

	if (reading <= arizona->pdata.hpdet_short_circuit_imp) {
		/* Headphones are always off here so just mark them */
		dev_warn(arizona->dev, "Possible HP short, disabling\n");
		return 0;
	} else if (reading <= HP_LOW_IMPEDANCE_LIMIT) {
		if (info->hp_imp_level == HP_LOW_IMPEDANCE)
			return 0;

		info->hp_imp_level = HP_LOW_IMPEDANCE;

		regmap_update_bits(arizona->regmap,
				   ARIZONA_HP1_SHORT_CIRCUIT_CTRL,
				   ARIZONA_HP1_SC_ENA_MASK, 0);

		patch = wm5110_low_impedance_patch;
		size = ARRAY_SIZE(wm5110_low_impedance_patch);
	} else {
		if (info->hp_imp_level == HP_NORMAL_IMPEDANCE)
			return 0;

		info->hp_imp_level = HP_NORMAL_IMPEDANCE;

		regmap_update_bits(arizona->regmap,
				   ARIZONA_HP1_SHORT_CIRCUIT_CTRL,
				   ARIZONA_HP1_SC_ENA_MASK,
				   ARIZONA_HP1_SC_ENA_MASK);

		patch = wm5110_normal_impedance_patch;
		size = ARRAY_SIZE(wm5110_normal_impedance_patch);
	}

	for (i = 0; i < size; ++i) {
		ret = regmap_write(arizona->regmap,
				   patch[i].reg, patch[i].def);
		if (ret != 0)
			dev_warn(arizona->dev,
				 "Failed to write headphone patch: %x <= %x\n",
				 patch[i].reg, patch[i].def);
	}

	return 0;
}

int arizona_wm1814_tune_headphone(struct arizona_extcon_info *info,
				  int reading)
{
	struct arizona *arizona = info->arizona;
	const struct reg_default *patch;
	int i, ret, size;

	if (reading <= arizona->pdata.hpdet_short_circuit_imp) {
		/* Headphones are always off here so just mark them */
		dev_warn(arizona->dev, "Possible HP short, disabling\n");
		return 0;
	} else if (reading < 15) {
		if (info->hp_imp_level == HP_LOW_IMPEDANCE)
			return 0;

		info->hp_imp_level = HP_LOW_IMPEDANCE;

		patch = wm1814_low_impedance_patch;
		size = ARRAY_SIZE(wm1814_low_impedance_patch);
	} else {
		if (info->hp_imp_level == HP_NORMAL_IMPEDANCE)
			return 0;

		info->hp_imp_level = HP_NORMAL_IMPEDANCE;

		patch = wm1814_normal_impedance_patch;
		size = ARRAY_SIZE(wm1814_normal_impedance_patch);
	}

	for (i = 0; i < size; ++i) {
		ret = regmap_write(arizona->regmap,
				   patch[i].reg, patch[i].def);
		if (ret != 0)
			dev_warn(arizona->dev,
				 "Failed to write headphone patch: %x <= %x\n",
				 patch[i].reg, patch[i].def);
	}

	return 0;
}

int arizona_hpdet_start(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	int ret;

	dev_dbg(arizona->dev, "Starting HPDET\n");

	/* If we specified to assume a fixed impedance skip HPDET */
	if (info->arizona->pdata.fixed_hpdet_imp) {
		int imp = info->arizona->pdata.fixed_hpdet_imp;

		if (arizona->pdata.hpdet_cb)
			arizona->pdata.hpdet_cb(imp);

		switch (arizona->type) {
		case WM5110:
			arizona_wm5110_tune_headphone(info, imp);
			info->arizona->hp_impedance = imp;
			break;
		case WM1814:
			arizona_wm1814_tune_headphone(info, imp);
			info->arizona->hp_impedance = imp;
			break;
		default:
			break;
		}

		ret = -EEXIST;
		goto skip;
	}

	/* Make sure we keep the device enabled during the measurement */
	pm_runtime_get_sync(info->dev);

	arizona_extcon_hp_clamp(info, true);

	ret = regmap_update_bits(arizona->regmap,
				 ARIZONA_ACCESSORY_DETECT_MODE_1,
				 ARIZONA_ACCDET_MODE_MASK,
				 info->state->mode);
	if (ret != 0) {
		dev_err(arizona->dev, "Failed to set HPDET mode (%d): %d\n",
			info->state->mode, ret);
		goto err;
	}

	ret = regmap_update_bits(arizona->regmap, ARIZONA_HEADPHONE_DETECT_1,
				 ARIZONA_HP_POLL, ARIZONA_HP_POLL);
	if (ret != 0) {
		dev_err(arizona->dev, "Can't start HPDET measurement: %d\n",
			ret);
		goto err;
	}

	return 0;

err:
	arizona_extcon_hp_clamp(info, false);

	pm_runtime_put_autosuspend(info->dev);

skip:
	return ret;
}
EXPORT_SYMBOL_GPL(arizona_hpdet_start);

void arizona_hpdet_restart(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;

	/* Reset back to starting range */
	regmap_update_bits(arizona->regmap,
			   ARIZONA_HEADPHONE_DETECT_1,
			   ARIZONA_HP_IMPEDANCE_RANGE_MASK |
			   ARIZONA_HP_POLL, 0);

	regmap_update_bits(arizona->regmap,
			   ARIZONA_HEADPHONE_DETECT_1,
			   ARIZONA_HP_POLL, ARIZONA_HP_POLL);
}
EXPORT_SYMBOL_GPL(arizona_hpdet_restart);

void arizona_hpdet_stop(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;

	/* Reset back to starting range */
	regmap_update_bits(arizona->regmap,
			   ARIZONA_HEADPHONE_DETECT_1,
			   ARIZONA_HP_IMPEDANCE_RANGE_MASK |
			   ARIZONA_HP_POLL, 0);

	/* Reset to default mode */
	regmap_update_bits(arizona->regmap,
			   ARIZONA_ACCESSORY_DETECT_MODE_1,
			   ARIZONA_ACCDET_MODE_MASK, 0);

	arizona_extcon_hp_clamp(info, false);

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);
}
EXPORT_SYMBOL_GPL(arizona_hpdet_stop);

static int arizona_hpdet_moisture_start(struct arizona_extcon_info *info)
{
	int ret;

	ret = arizona_hpdet_start(info);

	arizona_extcon_hp_clamp(info, false);

	return ret;
}

static int arizona_hpdet_moisture_reading(struct arizona_extcon_info *info,
					  int val)
{
	struct arizona *arizona = info->arizona;

	if (val < 0) {
		return val;
	} else if (val < arizona->pdata.hpdet_moisture_imp) {
		arizona_jds_set_state(info, &arizona_micd_microphone);
	} else {
		dev_warn(arizona->dev,
			 "Jack detection due to moisture, ignoring\n");
		arizona_jds_set_state(info, NULL);
	}

	return 0;
}

int arizona_hpdet_reading(struct arizona_extcon_info *info, int val)
{
	struct arizona *arizona = info->arizona;

	if (val < 0)
		return val;

	arizona->hp_impedance = val;

	if (arizona->pdata.hpdet_cb)
		arizona->pdata.hpdet_cb(arizona->hp_impedance);

	switch (arizona->type) {
	case WM5110:
		arizona_wm5110_tune_headphone(info, arizona->hp_impedance);
		break;
	case WM1814:
		arizona_wm1814_tune_headphone(info, arizona->hp_impedance);
		break;
	default:
		break;
	}

	if (info->mic) {
		arizona_extcon_report(info, BIT_HEADSET);
		arizona_jds_set_state(info, &arizona_micd_button);
	} else {
		arizona_extcon_report(info, BIT_HEADSET_NO_MIC);
		arizona_jds_set_state(info, NULL);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(arizona_hpdet_reading);

int arizona_micd_start(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	unsigned int mode;
	int ret;

	/* Microphone detection can't use idle mode */
	pm_runtime_get_sync(info->dev);

	ret = regulator_enable(info->micvdd);
	if (ret != 0) {
		dev_err(arizona->dev, "Failed to enable MICVDD: %d\n",
			ret);
	}

	if (info->micd_reva) {
		mutex_lock(&arizona->reg_setting_lock);
		regmap_write(arizona->regmap, 0x80, 0x3);
		regmap_write(arizona->regmap, 0x294, 0);
		regmap_write(arizona->regmap, 0x80, 0x0);
		mutex_unlock(&arizona->reg_setting_lock);
	}

	mode = info->state->mode;
	if (info->detecting && arizona->pdata.micd_software_compare)
		mode = ARIZONA_ACCDET_MODE_ADC;

	regmap_update_bits(arizona->regmap, ARIZONA_ACCESSORY_DETECT_MODE_1,
			   ARIZONA_ACCDET_MODE_MASK, mode);

	arizona_extcon_pulse_micbias(info);

	regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
			   ARIZONA_MICD_ENA, ARIZONA_MICD_ENA);

	return 0;
}
EXPORT_SYMBOL_GPL(arizona_micd_start);

void arizona_micd_stop(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	const char *widget = arizona_extcon_get_micbias(info);
	struct snd_soc_dapm_context *dapm = arizona->dapm;
	int ret;

	regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
			   ARIZONA_MICD_ENA, 0);

	mutex_lock(&dapm->card->dapm_mutex);

	ret = snd_soc_dapm_disable_pin(dapm, widget);
	if (ret != 0)
		dev_warn(arizona->dev,
			 "Failed to disable %s: %d\n",
			 widget, ret);

	mutex_unlock(&dapm->card->dapm_mutex);

	snd_soc_dapm_sync(dapm);

	if (info->micd_reva) {
		mutex_lock(&arizona->reg_setting_lock);
		regmap_write(arizona->regmap, 0x80, 0x3);
		regmap_write(arizona->regmap, 0x294, 2);
		regmap_write(arizona->regmap, 0x80, 0x0);
		mutex_unlock(&arizona->reg_setting_lock);
	}

	/* Reset to default mode */
	regmap_update_bits(arizona->regmap,
			   ARIZONA_ACCESSORY_DETECT_MODE_1,
			   ARIZONA_ACCDET_MODE_MASK, 0);

	regulator_disable(info->micvdd);

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);
}
EXPORT_SYMBOL_GPL(arizona_micd_stop);

int arizona_micd_button_reading(struct arizona_extcon_info *info,
				int val)
{
	struct arizona *arizona = info->arizona;
	int lvl, i, key;

	if (val < 0)
		return val;

	if (val & MICD_LVL_0_TO_7) {
		dev_dbg(arizona->dev, "Mic button detected\n");

		lvl = val & ARIZONA_MICD_LVL_MASK;
		lvl >>= ARIZONA_MICD_LVL_SHIFT;

		for (i = 0; i < info->num_micd_ranges; i++)
			input_report_key(info->input,
					 info->micd_ranges[i].key, 0);

		if (lvl && ffs(lvl) - 1 < info->num_micd_ranges) {
			key = info->micd_ranges[ffs(lvl) - 1].key;
			input_report_key(info->input, key, 1);
			input_sync(info->input);
		} else {
			dev_warn(arizona->dev,
				 "Button level %u out of range\n", lvl);
		}
	} else {
		dev_dbg(arizona->dev, "Mic button released\n");
		for (i = 0; i < info->num_micd_ranges; i++)
			input_report_key(info->input,
					 info->micd_ranges[i].key, 0);
		input_sync(info->input);
		arizona_extcon_pulse_micbias(info);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(arizona_micd_button_reading);

int arizona_micd_mic_start(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	int ret;

	info->detecting = true;

	ret = regulator_allow_bypass(info->micvdd, false);
	if (ret != 0) {
		dev_err(arizona->dev,
			"Failed to regulate MICVDD: %d\n",
			ret);
	}

	return arizona_micd_start(info);
}
EXPORT_SYMBOL_GPL(arizona_micd_mic_start);

void arizona_micd_mic_stop(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	int ret;

	arizona_micd_stop(info);

	ret = regulator_allow_bypass(info->micvdd, true);
	if (ret != 0) {
		dev_err(arizona->dev, "Failed to bypass MICVDD: %d\n",
			ret);
	}

	info->detecting = false;
}
EXPORT_SYMBOL_GPL(arizona_micd_mic_stop);

int arizona_micd_mic_reading(struct arizona_extcon_info *info, int val)
{
	struct arizona *arizona = info->arizona;
	int ret;

	if (val < 0)
		return val;

	/* Due to jack detect this should never happen */
	if (!(val & ARIZONA_MICD_STS)) {
		dev_warn(arizona->dev, "Detected open circuit\n");
		info->mic = arizona->pdata.micd_open_circuit_declare;
		goto done;
	}

	/* If we got a high impedence we should have a headset, report it. */
	if (val & ARIZONA_MICD_LVL_8) {
		dev_dbg(arizona->dev, "Detected headset\n");
		info->mic = true;
		goto done;
	}

	/* If we detected a lower impedence during initial startup
	 * then we probably have the wrong polarity, flip it.  Don't
	 * do this for the lowest impedences to speed up detection of
	 * plain headphones.  If both polarities report a low
	 * impedence then give up and report headphones.
	 */
	if (val & MICD_LVL_1_TO_7) {
		if (info->jack_flips >= info->micd_num_modes * 10) {
			dev_dbg(arizona->dev, "Detected HP/line\n");
			goto done;
		} else {
			info->micd_mode++;
			if (info->micd_mode == info->micd_num_modes)
				info->micd_mode = 0;
			arizona_extcon_set_mode(info, info->micd_mode);

			info->jack_flips++;

			return 0;
		}
	}

	/*
	 * If we're still detecting and we detect a short then we've
	 * got a headphone.
	 */
	dev_dbg(arizona->dev, "Headphone detected\n");

done:
	pm_runtime_mark_last_busy(info->dev);

	if (arizona->pdata.hpdet_channel)
		ret = arizona_jds_set_state(info, &arizona_hpdet_right);
	else
		ret = arizona_jds_set_state(info, &arizona_hpdet_left);
	if (ret < 0) {
		if (info->mic)
			arizona_extcon_report(info, BIT_HEADSET);
		else
			arizona_extcon_report(info, BIT_HEADSET_NO_MIC);
	}

	if (arizona->pdata.micd_cb)
		arizona->pdata.micd_cb(info->mic);

	return 0;
}
EXPORT_SYMBOL_GPL(arizona_micd_mic_reading);

int arizona_micd_mic_timeout_ms(struct arizona_extcon_info *info)
{
	if (info->arizona->pdata.micd_timeout)
		return info->arizona->pdata.micd_timeout;
	else
		return DEFAULT_MICD_TIMEOUT;
}
EXPORT_SYMBOL_GPL(arizona_micd_mic_timeout_ms);

void arizona_micd_mic_timeout(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	int ret;

	dev_dbg(info->arizona->dev, "MICD timed out, reporting HP\n");

	if (arizona->pdata.hpdet_channel)
		ret = arizona_jds_set_state(info, &arizona_hpdet_right);
	else
		ret = arizona_jds_set_state(info, &arizona_hpdet_left);
	if (ret < 0)
		arizona_extcon_report(info, BIT_HEADSET_NO_MIC);
}
EXPORT_SYMBOL_GPL(arizona_micd_mic_timeout);

static int arizona_hpdet_acc_id_reading(struct arizona_extcon_info *info,
					int reading)
{
	struct arizona *arizona = info->arizona;
	int id_gpio = arizona->pdata.hpdet_id_gpio;

	if (reading < 0)
		return reading;

	/*
	 * When we're using HPDET for accessory identification we need
	 * to take multiple measurements, step through them in sequence.
	 */
	info->hpdet_res[info->num_hpdet_res++] = reading;

	/* Only check the mic directly if we didn't already ID it */
	if (id_gpio && info->num_hpdet_res == 1) {
		dev_dbg(arizona->dev, "Measuring mic\n");

		regmap_update_bits(arizona->regmap,
				   ARIZONA_ACCESSORY_DETECT_MODE_1,
				   ARIZONA_ACCDET_SRC |
				   ARIZONA_ACCDET_MODE_MASK,
				   info->micd_modes[0].src |
				   ARIZONA_ACCDET_MODE_HPR);

		gpio_set_value_cansleep(id_gpio, 1);

		return -EAGAIN;
	}

	/* OK, got both.  Now, compare... */
	dev_dbg(arizona->dev, "HPDET measured %d %d\n",
		info->hpdet_res[0], info->hpdet_res[1]);

	/* Take the headphone impedance for the main report */
	reading = info->hpdet_res[0];

	/* Sometimes we get false readings due to slow insert */
	if (reading >= ARIZONA_HPDET_MAX && !info->hpdet_retried) {
		dev_dbg(arizona->dev, "Retrying high impedance\n");

		info->num_hpdet_res = 0;
		info->hpdet_retried = true;

		regmap_update_bits(arizona->regmap,
				   ARIZONA_ACCESSORY_DETECT_MODE_1,
				   ARIZONA_ACCDET_SRC |
				   ARIZONA_ACCDET_MODE_MASK,
				   info->micd_modes[0].src |
				   ARIZONA_ACCDET_MODE_HPL);

		return -EAGAIN;
	}

	if (!id_gpio || info->hpdet_res[1] > 50) {
		dev_dbg(arizona->dev, "Detected mic\n");

		arizona_jds_set_state(info, &arizona_micd_microphone);
	} else {
		dev_dbg(arizona->dev, "Detected headphone\n");

		arizona_extcon_report(info, BIT_HEADSET_NO_MIC);

		arizona_jds_set_state(info, NULL);
	}

	return 0;
}

static int arizona_hpdet_acc_id_start(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	int hp_reading = 32;
	int ret;

	dev_dbg(arizona->dev, "Starting identification via HPDET\n");

	/* Make sure we keep the device enabled during the measurement */
	pm_runtime_get_sync(info->dev);

	arizona_extcon_hp_clamp(info, true);

	ret = regmap_update_bits(arizona->regmap,
				 ARIZONA_ACCESSORY_DETECT_MODE_1,
				 ARIZONA_ACCDET_SRC | ARIZONA_ACCDET_MODE_MASK,
				 info->micd_modes[0].src |
				 ARIZONA_ACCDET_MODE_HPL);
	if (ret != 0) {
		dev_err(arizona->dev, "Failed to set HPDETL mode: %d\n", ret);
		goto err;
	}

	if (arizona->pdata.hpdet_acc_id_line) {
		ret = regmap_update_bits(arizona->regmap,
					 ARIZONA_HEADPHONE_DETECT_1,
					 ARIZONA_HP_POLL, ARIZONA_HP_POLL);
		if (ret != 0) {
			dev_err(arizona->dev,
				"Can't start HPDETL measurement: %d\n",
				ret);
			goto err;
		}
	} else {
		/**
		 * If we are not identifying line outputs fake the first
		 * reading at 32 ohms
		 */
		arizona_hpdet_acc_id_reading(info, hp_reading);
	}

	return 0;

err:
	arizona_extcon_hp_clamp(info, false);

	pm_runtime_put_autosuspend(info->dev);

	/* Just report headphone */
	arizona_extcon_report(info, BIT_HEADSET_NO_MIC);

	return ret;
}

static void arizona_hpdet_acc_id_stop(struct arizona_extcon_info *info)
{
	struct arizona *arizona = info->arizona;
	int id_gpio = arizona->pdata.hpdet_id_gpio;

	/* Make sure everything is reset back to the real polarity */
	regmap_update_bits(arizona->regmap,
			   ARIZONA_ACCESSORY_DETECT_MODE_1,
			   ARIZONA_ACCDET_SRC,
			   info->micd_modes[0].src);

	if (id_gpio)
		gpio_set_value_cansleep(id_gpio, 0);

	/* Rest of the clean is identical to standard hpdet */
	arizona_hpdet_stop(info);
}

static irqreturn_t arizona_hpdet_handler(int irq, void *data)
{
	struct arizona_extcon_info *info = data;
	struct arizona *arizona = info->arizona;
	int ret;

	arizona_jds_cancel_timeout(info);

	mutex_lock(&info->lock);

	switch (arizona_jds_get_mode(info)) {
	case ARIZONA_ACCDET_MODE_HPL:
	case ARIZONA_ACCDET_MODE_HPR:
	case ARIZONA_ACCDET_MODE_HPM:
		break;
	default:
		dev_warn(arizona->dev, "Spurious HPDET IRQ\n");
		arizona_jds_start_timeout(info);
		mutex_unlock(&info->lock);
		return IRQ_NONE;
	}

	ret = arizona_hpdet_read(info);
	if (ret == -EAGAIN)
		goto out;

	arizona_jds_reading(info, ret);

out:
	arizona_jds_start_timeout(info);

	pm_runtime_mark_last_busy(info->dev);

	mutex_unlock(&info->lock);

	return IRQ_HANDLED;
}

static void arizona_micd_handler(struct work_struct *work)
{
	struct arizona_extcon_info *info =
		container_of(work,
			     struct arizona_extcon_info,
			     micd_detect_work.work);
	struct arizona *arizona = info->arizona;
	int ret;

	arizona_jds_cancel_timeout(info);

	mutex_lock(&info->lock);

	switch (arizona_jds_get_mode(info)) {
	case ARIZONA_ACCDET_MODE_MIC:
	case ARIZONA_ACCDET_MODE_ADC:
		break;
	default:
		dev_warn(arizona->dev, "Spurious MICDET IRQ\n");
		arizona_jds_start_timeout(info);
		mutex_unlock(&info->lock);
		return;
	}

	ret = arizona_micd_read(info);
	if (ret == -EAGAIN)
		goto out;

	arizona_jds_reading(info, ret);

out:
	arizona_jds_start_timeout(info);

	pm_runtime_mark_last_busy(info->dev);

	mutex_unlock(&info->lock);
}

static void arizona_micd_input_clear(struct work_struct *work)
{
	struct arizona_extcon_info *info = container_of(work,
							struct arizona_extcon_info,
							micd_clear_work.work);
	struct arizona *arizona = info->arizona;

	arizona_florida_clear_input(arizona);

	mutex_lock(&info->lock);
	if (info->first_clear) {
		schedule_delayed_work(&info->micd_clear_work,
				      msecs_to_jiffies(900));
		info->first_clear = false;
	}
	mutex_unlock(&info->lock);
}

static irqreturn_t arizona_micdet(int irq, void *data)
{
	struct arizona_extcon_info *info = data;
	struct arizona *arizona = info->arizona;
	int debounce = arizona->pdata.micd_detect_debounce;

	cancel_delayed_work_sync(&info->micd_detect_work);
	cancel_delayed_work_sync(&info->micd_clear_work);

	mutex_lock(&info->lock);

	if (!info->detecting)
		debounce = 0;

	switch (arizona->type) {
	case WM5110:
		info->first_clear = true;
		schedule_delayed_work(&info->micd_clear_work,
				      msecs_to_jiffies(80));
		break;
	default:
		break;
	}

	mutex_unlock(&info->lock);

	if (debounce)
		schedule_delayed_work(&info->micd_detect_work,
				      msecs_to_jiffies(debounce));
	else
		arizona_micd_handler(&info->micd_detect_work.work);

	return IRQ_HANDLED;
}

static const struct arizona_jd_state arizona_hpdet_moisture = {
	.mode = ARIZONA_ACCDET_MODE_HPL,
	.start = arizona_hpdet_moisture_start,
	.reading = arizona_hpdet_moisture_reading,
	.stop = arizona_hpdet_stop,
};

const struct arizona_jd_state arizona_hpdet_left = {
	.mode = ARIZONA_ACCDET_MODE_HPL,
	.start = arizona_hpdet_start,
	.reading = arizona_hpdet_reading,
	.stop = arizona_hpdet_stop,
};
EXPORT_SYMBOL_GPL(arizona_hpdet_left);

const struct arizona_jd_state arizona_hpdet_right = {
	.mode = ARIZONA_ACCDET_MODE_HPR,
	.start = arizona_hpdet_start,
	.reading = arizona_hpdet_reading,
	.stop = arizona_hpdet_stop,
};
EXPORT_SYMBOL_GPL(arizona_hpdet_right);

const struct arizona_jd_state arizona_micd_button = {
	.mode = ARIZONA_ACCDET_MODE_MIC,
	.start = arizona_micd_start,
	.reading = arizona_micd_button_reading,
	.stop = arizona_micd_stop,
};
EXPORT_SYMBOL_GPL(arizona_micd_button);

const struct arizona_jd_state arizona_micd_microphone = {
	.mode = ARIZONA_ACCDET_MODE_MIC,
	.start = arizona_micd_mic_start,
	.reading = arizona_micd_mic_reading,
	.stop = arizona_micd_mic_stop,

	.timeout_ms = arizona_micd_mic_timeout_ms,
	.timeout = arizona_micd_mic_timeout,
};
EXPORT_SYMBOL_GPL(arizona_micd_microphone);

static const struct arizona_jd_state arizona_hpdet_acc_id = {
	.mode = ARIZONA_ACCDET_MODE_HPL,
	.start = arizona_hpdet_acc_id_start,
	.restart = arizona_hpdet_restart,
	.reading = arizona_hpdet_acc_id_reading,
	.stop = arizona_hpdet_acc_id_stop,
};

static void arizona_hpdet_work(struct work_struct *work)
{
	struct arizona_extcon_info *info = container_of(work,
							struct arizona_extcon_info,
							hpdet_work.work);

	mutex_lock(&info->lock);
	arizona_jds_set_state(info, &arizona_hpdet_acc_id);
	mutex_unlock(&info->lock);
}

static irqreturn_t arizona_jackdet(int irq, void *data)
{
	struct arizona_extcon_info *info = data;
	struct arizona *arizona = info->arizona;
	unsigned int val, present, mask;
	bool cancelled_hp, cancelled_state;
	int ret, i;

	cancelled_hp = cancel_delayed_work_sync(&info->hpdet_work);
	cancelled_state = arizona_jds_cancel_timeout(info);

	pm_runtime_get_sync(info->dev);

	mutex_lock(&info->lock);

	if (arizona->pdata.jd_gpio5) {
		mask = ARIZONA_MICD_CLAMP_STS;
		if (arizona->pdata.jd_invert)
			present = ARIZONA_MICD_CLAMP_STS;
		else
			present = 0;
	} else {
		mask = ARIZONA_JD1_STS;
		if (arizona->pdata.jd_invert)
			present = 0;
		else
			present = ARIZONA_JD1_STS;
	}

	ret = regmap_read(arizona->regmap, ARIZONA_AOD_IRQ_RAW_STATUS, &val);
	if (ret != 0) {
		dev_err(arizona->dev, "Failed to read jackdet status: %d\n",
			ret);
		mutex_unlock(&info->lock);
		pm_runtime_put_autosuspend(info->dev);
		return IRQ_NONE;
	}

	val &= mask;
	if (val == info->last_jackdet) {
		dev_dbg(arizona->dev, "Suppressing duplicate JACKDET\n");
		if (cancelled_hp)
			schedule_delayed_work(&info->hpdet_work,
					      msecs_to_jiffies(HPDET_DEBOUNCE));

		if (cancelled_state)
			arizona_jds_start_timeout(info);

		goto out;
	}
	info->last_jackdet = val;

	if (info->last_jackdet == present) {
		dev_info(arizona->dev, "Detected jack\n");

		if (arizona->pdata.jd_wake_time)
			__pm_wakeup_event(&info->detection_wake_lock,
				arizona->pdata.jd_wake_time);

		if (!arizona->pdata.hpdet_acc_id) {
			info->mic = false;
			info->jack_flips = 0;

			if (arizona->pdata.init_mic_delay)
				msleep(arizona->pdata.init_mic_delay);

			if (arizona->pdata.custom_jd)
				arizona_jds_set_state(info,
						      arizona->pdata.custom_jd);
			else if (arizona->pdata.hpdet_moisture_imp)
				arizona_jds_set_state(info,
						      &arizona_hpdet_moisture);
			else
				arizona_jds_set_state(info,
						      &arizona_micd_microphone);

			arizona_jds_start_timeout(info);
		} else {
			schedule_delayed_work(&info->hpdet_work,
					      msecs_to_jiffies(HPDET_DEBOUNCE));
		}

		regmap_update_bits(arizona->regmap,
				   ARIZONA_JACK_DETECT_DEBOUNCE,
				   ARIZONA_MICD_CLAMP_DB | ARIZONA_JD1_DB, 0);
	} else {
		dev_info(arizona->dev, "Detected jack removal\n");

		info->num_hpdet_res = 0;
		for (i = 0; i < ARRAY_SIZE(info->hpdet_res); i++)
			info->hpdet_res[i] = 0;
		info->mic = false;
		info->hpdet_retried = false;
		arizona->hp_impedance = 0;
		arizona_jds_set_state(info, NULL);

		for (i = 0; i < info->num_micd_ranges; i++)
			input_report_key(info->input,
					 info->micd_ranges[i].key, 0);
		input_sync(info->input);

		arizona_extcon_report(info, BIT_NO_HEADSET);

		regmap_update_bits(arizona->regmap,
				   ARIZONA_JACK_DETECT_DEBOUNCE,
				   ARIZONA_MICD_CLAMP_DB | ARIZONA_JD1_DB,
				   ARIZONA_MICD_CLAMP_DB | ARIZONA_JD1_DB);

		switch (arizona->type) {
		case WM5110:
			arizona_wm5110_tune_headphone(info, ARIZONA_HP_Z_OPEN);
			break;
		case WM1814:
			arizona_wm1814_tune_headphone(info, ARIZONA_HP_Z_OPEN);
			break;
		default:
			break;
		}

		/* Use a sufficiently large number to indicate open circuit */
		if (arizona->pdata.hpdet_cb) {
			arizona->pdata.hpdet_cb(ARIZONA_HP_Z_OPEN);
		}

		if (arizona->pdata.micd_cb)
			arizona->pdata.micd_cb(false);
	}

out:
	/* Clear trig_sts to make sure DCVDD is not forced up */
	regmap_write(arizona->regmap, ARIZONA_AOD_WKUP_AND_TRIG,
		     ARIZONA_MICD_CLAMP_FALL_TRIG_STS |
		     ARIZONA_MICD_CLAMP_RISE_TRIG_STS |
		     ARIZONA_JD1_FALL_TRIG_STS |
		     ARIZONA_JD1_RISE_TRIG_STS);

	mutex_unlock(&info->lock);

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);

	return IRQ_HANDLED;
}

/* Map a level onto a slot in the register bank */
static void arizona_micd_set_level(struct arizona *arizona, int index,
				   unsigned int level)
{
	int reg;
	unsigned int mask;

	reg = ARIZONA_MIC_DETECT_LEVEL_4 - (index / 2);

	if (!(index % 2)) {
		mask = 0x3f00;
		level <<= 8;
	} else {
		mask = 0x3f;
	}

	/* Program the level itself */
	regmap_update_bits(arizona->regmap, reg, mask, level);
}

#ifdef CONFIG_OF
static int arizona_extcon_of_get_pdata(struct arizona *arizona)
{
	struct arizona_pdata *pdata = &arizona->pdata;

	arizona_of_read_u32(arizona, "wlf,micd-detect-debounce", false,
			    &pdata->micd_detect_debounce);

	pdata->micd_pol_gpio = arizona_of_get_named_gpio(arizona,
							 "wlf,micd-pol-gpio",
							 false);

	arizona_of_read_u32(arizona, "wlf,micd-bias-start-time", false,
			    &pdata->micd_bias_start_time);

	arizona_of_read_u32(arizona, "wlf,micd-rate", false,
			    &pdata->micd_rate);

	arizona_of_read_u32(arizona, "wlf,micd-dbtime", false,
			    &pdata->micd_dbtime);

	arizona_of_read_u32(arizona, "wlf,micd-timeout", false,
			    &pdata->micd_timeout);

	pdata->micd_force_micbias =
		of_property_read_bool(arizona->dev->of_node,
				      "wlf,micd-force-micbias");

	pdata->micd_force_micbias_initial =
		of_property_read_bool(arizona->dev->of_node,
				      "wlf,micd-force-micbias-initial");
	pdata->micd_software_compare =
			of_property_read_bool(arizona->dev->of_node,
					      "wlf,micd-software-compare");

	pdata->micd_open_circuit_declare =
			of_property_read_bool(arizona->dev->of_node,
					      "wlf,micd-open-circuit-declare");

	pdata->jd_gpio5 = of_property_read_bool(arizona->dev->of_node,
						"wlf,use-jd-gpio");

	pdata->jd_gpio5_nopull = of_property_read_bool(arizona->dev->of_node,
						       "wlf,jd-gpio-nopull");

	pdata->jd_invert = of_property_read_bool(arizona->dev->of_node,
						 "wlf,jd-invert");

	arizona_of_read_u32(arizona, "wlf,gpsw", false, &pdata->gpsw);

	arizona_of_read_u32(arizona, "wlf,init-mic-delay", false,
			    &pdata->init_mic_delay);

	arizona_of_read_u32(arizona, "wlf,fixed-hpdet-imp", false,
			    &pdata->fixed_hpdet_imp);

	arizona_of_read_u32(arizona, "wlf,hpdet-moisture-imp", false,
			    &pdata->hpdet_moisture_imp);

	arizona_of_read_u32(arizona, "wlf,hpdet-short-circuit-imp", false,
			    &pdata->hpdet_short_circuit_imp);

	arizona_of_read_u32(arizona, "wlf,hpdet-channel", false,
			    &pdata->hpdet_channel);

	arizona_of_read_u32(arizona, "wlf,jd-wake-time", false,
			    &pdata->jd_wake_time);

	return 0;
}
#else
static inline int arizona_extcon_of_get_pdata(struct arizona *arizona)
{
	return 0;
}
#endif

static ssize_t arizona_extcon_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct arizona_extcon_info *info = platform_get_drvdata(pdev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", info->arizona->hp_impedance);
}

static int arizona_extcon_probe(struct platform_device *pdev)
{
	struct arizona *arizona = dev_get_drvdata(pdev->dev.parent);
	struct arizona_pdata *pdata = &arizona->pdata;
	struct arizona_extcon_info *info;
	unsigned int val;
	unsigned int clamp_mode;
	int jack_irq_fall, jack_irq_rise;
	int ret, mode, i, j;

	if (!arizona->dapm || !arizona->dapm->card)
		return -EPROBE_DEFER;

	if (pdata->hpdet_short_circuit_imp < 1)
		pdata->hpdet_short_circuit_imp = ARIZONA_HP_SHORT_IMPEDANCE;
	else if	(pdata->hpdet_short_circuit_imp >= HP_LOW_IMPEDANCE_LIMIT)
		pdata->hpdet_short_circuit_imp = HP_LOW_IMPEDANCE_LIMIT - 1;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	if (IS_ENABLED(CONFIG_OF)) {
		if (!dev_get_platdata(arizona->dev)) {
			ret = arizona_extcon_of_get_pdata(arizona);
			if (ret < 0)
				return ret;
		}
	}

	/* Set of_node to parent from the SPI device to allow
	 * location regulator supplies */
	pdev->dev.of_node = arizona->dev->of_node;

	info->micvdd = devm_regulator_get(&pdev->dev, "MICVDD");
	if (IS_ERR(info->micvdd)) {
		ret = PTR_ERR(info->micvdd);
		dev_err(arizona->dev, "Failed to get MICVDD: %d\n", ret);
		return ret;
	}

	mutex_init(&info->lock);
	wakeup_source_init(&info->detection_wake_lock, "arizona-jack-detection");
	info->arizona = arizona;
	info->dev = &pdev->dev;
	info->last_jackdet = ~(ARIZONA_MICD_CLAMP_STS | ARIZONA_JD1_STS);
	INIT_DELAYED_WORK(&info->hpdet_work, arizona_hpdet_work);
	INIT_DELAYED_WORK(&info->micd_detect_work, arizona_micd_handler);
	INIT_DELAYED_WORK(&info->micd_clear_work, arizona_micd_input_clear);
	INIT_DELAYED_WORK(&info->state_timeout_work, arizona_jds_timeout_work);
	platform_set_drvdata(pdev, info);

	switch (arizona->type) {
	case WM5102:
		switch (arizona->rev) {
		case 0:
			info->micd_reva = true;
			break;
		default:
			info->micd_clamp = true;
			info->hpdet_ip = 1;
			break;
		}
		break;
	case WM8280:
	case WM5110:
		switch (arizona->rev) {
		case 0 ... 2:
			break;
		default:
			info->micd_clamp = true;
			info->hpdet_ip = 2;
			break;
		}
		break;
	case WM8998:
	case WM1814:
		info->micd_clamp = true;
		info->hpdet_ip = 2;
		break;
	default:
		break;
	}

	info->edev.name = "h2w";

	ret = switch_dev_register(&info->edev);
	if (ret < 0) {
		dev_err(arizona->dev, "extcon_dev_register() failed: %d\n",
			ret);
		goto err_wakelock;
	}

	info->input = devm_input_allocate_device(&pdev->dev);
	if (!info->input) {
		dev_err(arizona->dev, "Can't allocate input dev\n");
		ret = -ENOMEM;
		goto err_register;
	}

	info->input->name = "Headset";
	info->input->phys = "arizona/extcon";
	info->input->dev.parent = &pdev->dev;

	if (pdata->num_micd_configs) {
		info->micd_modes = pdata->micd_configs;
		info->micd_num_modes = pdata->num_micd_configs;
	} else {
		info->micd_modes = micd_default_modes;
		info->micd_num_modes = ARRAY_SIZE(micd_default_modes);
	}

	if (arizona->pdata.gpsw > 0)
		regmap_update_bits(arizona->regmap, ARIZONA_GP_SWITCH_1,
				   ARIZONA_SW1_MODE_MASK, arizona->pdata.gpsw);

	if (arizona->pdata.micd_pol_gpio > 0) {
		if (info->micd_modes[0].gpio)
			mode = GPIOF_OUT_INIT_HIGH;
		else
			mode = GPIOF_OUT_INIT_LOW;

		ret = devm_gpio_request_one(&pdev->dev,
					    arizona->pdata.micd_pol_gpio,
					    mode,
					    "MICD polarity");
		if (ret != 0) {
			dev_err(arizona->dev, "Failed to request GPIO%d: %d\n",
				arizona->pdata.micd_pol_gpio, ret);
			goto err_register;
		}
	}

	if (arizona->pdata.hpdet_id_gpio > 0) {
		ret = devm_gpio_request_one(&pdev->dev,
					    arizona->pdata.hpdet_id_gpio,
					    GPIOF_OUT_INIT_LOW,
					    "HPDET");
		if (ret != 0) {
			dev_err(arizona->dev, "Failed to request GPIO%d: %d\n",
				arizona->pdata.hpdet_id_gpio, ret);
			goto err_register;
		}
	}

	if (arizona->pdata.micd_bias_start_time)
		regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
				   ARIZONA_MICD_BIAS_STARTTIME_MASK,
				   arizona->pdata.micd_bias_start_time
				   << ARIZONA_MICD_BIAS_STARTTIME_SHIFT);

	if (arizona->pdata.micd_rate)
		regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
				   ARIZONA_MICD_RATE_MASK,
				   arizona->pdata.micd_rate
				   << ARIZONA_MICD_RATE_SHIFT);

	if (arizona->pdata.micd_dbtime)
		regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_1,
				   ARIZONA_MICD_DBTIME_MASK,
				   arizona->pdata.micd_dbtime
				   << ARIZONA_MICD_DBTIME_SHIFT);

	BUILD_BUG_ON(ARRAY_SIZE(arizona_micd_levels) <
		     ARIZONA_NUM_MICD_BUTTON_LEVELS);

	if (arizona->pdata.num_micd_ranges) {
		info->micd_ranges = pdata->micd_ranges;
		info->num_micd_ranges = pdata->num_micd_ranges;
	} else {
		info->micd_ranges = micd_default_ranges;
		info->num_micd_ranges = ARRAY_SIZE(micd_default_ranges);
	}

	if (arizona->pdata.num_micd_ranges > ARIZONA_MAX_MICD_RANGE) {
		dev_err(arizona->dev, "Too many MICD ranges: %d\n",
			arizona->pdata.num_micd_ranges);
	}

	if (info->num_micd_ranges > 1) {
		for (i = 1; i < info->num_micd_ranges; i++) {
			if (info->micd_ranges[i - 1].max >
			    info->micd_ranges[i].max) {
				dev_err(arizona->dev,
					"MICD ranges must be sorted\n");
				ret = -EINVAL;
				goto err_input;
			}
		}
	}

	/* Disable all buttons by default */
	regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_2,
			   ARIZONA_MICD_LVL_SEL_MASK, 0x81);

	/* Set up all the buttons the user specified */
	for (i = 0; i < info->num_micd_ranges; i++) {
		for (j = 0; j < ARIZONA_NUM_MICD_BUTTON_LEVELS; j++)
			if (arizona_micd_levels[j] >= info->micd_ranges[i].max)
				break;

		if (j == ARIZONA_NUM_MICD_BUTTON_LEVELS) {
			dev_err(arizona->dev, "Unsupported MICD level %d\n",
				info->micd_ranges[i].max);
			ret = -EINVAL;
			goto err_input;
		}

		dev_dbg(arizona->dev, "%d ohms for MICD threshold %d\n",
			arizona_micd_levels[j], i);

		arizona_micd_set_level(arizona, i, j);
		input_set_capability(info->input, EV_KEY,
				     info->micd_ranges[i].key);

		/* Enable reporting of that range */
		regmap_update_bits(arizona->regmap, ARIZONA_MIC_DETECT_2,
				   1 << i, 1 << i);
	}

	/* Set all the remaining keys to a maximum */
	for (; i < ARIZONA_MAX_MICD_RANGE; i++)
		arizona_micd_set_level(arizona, i, 0x3f);

	/*
	 * If we have a clamp use it, activating in conjunction with
	 * GPIO5 if that is connected for jack detect operation.
	 */
	if (info->micd_clamp) {
		if (arizona->pdata.jd_gpio5) {
			/* Put the GPIO into input mode with optional pull */
			val = 0xc101;
			if (arizona->pdata.jd_gpio5_nopull)
				val &= ~ARIZONA_GPN_PU;

			regmap_write(arizona->regmap, ARIZONA_GPIO5_CTRL,
				     val);

			if (arizona->pdata.jd_invert)
				clamp_mode = ARIZONA_MICD_CLAMP_MODE_JDH_GP5H;
			else
				clamp_mode = ARIZONA_MICD_CLAMP_MODE_JDL_GP5H;
		} else {
			if (arizona->pdata.jd_invert)
				clamp_mode = ARIZONA_MICD_CLAMP_MODE_JDH;
			else
				clamp_mode = ARIZONA_MICD_CLAMP_MODE_JDL;
		}

		regmap_update_bits(arizona->regmap,
				   ARIZONA_MICD_CLAMP_CONTROL,
				   ARIZONA_MICD_CLAMP_MODE_MASK, clamp_mode);

		regmap_update_bits(arizona->regmap,
				   ARIZONA_JACK_DETECT_DEBOUNCE,
				   ARIZONA_MICD_CLAMP_DB,
				   ARIZONA_MICD_CLAMP_DB);
	}

	arizona_extcon_set_mode(info, 0);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_idle(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	if (arizona->pdata.jd_gpio5) {
		jack_irq_rise = ARIZONA_IRQ_MICD_CLAMP_RISE;
		jack_irq_fall = ARIZONA_IRQ_MICD_CLAMP_FALL;
	} else {
		jack_irq_rise = ARIZONA_IRQ_JD_RISE;
		jack_irq_fall = ARIZONA_IRQ_JD_FALL;
	}

	ret = arizona_request_irq(arizona, jack_irq_rise,
				  "JACKDET rise", arizona_jackdet, info);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to get JACKDET rise IRQ: %d\n",
			ret);
		goto err_input;
	}

	ret = arizona_set_irq_wake(arizona, jack_irq_rise, 1);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to set JD rise IRQ wake: %d\n",
			ret);
		goto err_rise;
	}

	ret = arizona_request_irq(arizona, jack_irq_fall,
				  "JACKDET fall", arizona_jackdet, info);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to get JD fall IRQ: %d\n", ret);
		goto err_rise_wake;
	}

	ret = arizona_set_irq_wake(arizona, jack_irq_fall, 1);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to set JD fall IRQ wake: %d\n",
			ret);
		goto err_fall;
	}

	ret = arizona_request_irq(arizona, ARIZONA_IRQ_MICDET,
				  "MICDET", arizona_micdet, info);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to get MICDET IRQ: %d\n", ret);
		goto err_fall_wake;
	}

	ret = arizona_request_irq(arizona, ARIZONA_IRQ_HPDET,
				  "HPDET", arizona_hpdet_handler, info);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to get HPDET IRQ: %d\n", ret);
		goto err_micdet;
	}

	arizona_clk32k_enable(arizona);
	regmap_update_bits(arizona->regmap, ARIZONA_JACK_DETECT_DEBOUNCE,
			   ARIZONA_JD1_DB, ARIZONA_JD1_DB);
	regmap_update_bits(arizona->regmap, ARIZONA_JACK_DETECT_ANALOGUE,
			   ARIZONA_JD1_ENA, ARIZONA_JD1_ENA);

	ret = regulator_allow_bypass(info->micvdd, true);
	if (ret != 0)
		dev_warn(arizona->dev, "Failed to set MICVDD to bypass: %d\n",
			 ret);

	pm_runtime_put(&pdev->dev);

	ret = input_register_device(info->input);
	if (ret) {
		dev_err(&pdev->dev, "Can't register input device: %d\n", ret);
		goto err_hpdet;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_hp_impedance);
	if (ret != 0)
		dev_err(&pdev->dev,
			"Failed to create sysfs node for hp_impedance %d\n",
			ret);

	return 0;

err_hpdet:
	arizona_free_irq(arizona, ARIZONA_IRQ_HPDET, info);
err_micdet:
	arizona_free_irq(arizona, ARIZONA_IRQ_MICDET, info);
err_fall_wake:
	arizona_set_irq_wake(arizona, jack_irq_fall, 0);
err_fall:
	arizona_free_irq(arizona, jack_irq_fall, info);
err_rise_wake:
	arizona_set_irq_wake(arizona, jack_irq_rise, 0);
err_rise:
	arizona_free_irq(arizona, jack_irq_rise, info);
err_input:
err_register:
	pm_runtime_disable(&pdev->dev);
	switch_dev_unregister(&info->edev);
err_wakelock:
	wakeup_source_trash(&info->detection_wake_lock);
	return ret;
}

static int arizona_extcon_remove(struct platform_device *pdev)
{
	struct arizona_extcon_info *info = platform_get_drvdata(pdev);
	struct arizona *arizona = info->arizona;
	int jack_irq_rise, jack_irq_fall;

	pm_runtime_disable(&pdev->dev);

	regmap_update_bits(arizona->regmap,
			   ARIZONA_MICD_CLAMP_CONTROL,
			   ARIZONA_MICD_CLAMP_MODE_MASK, 0);

	if (arizona->pdata.jd_gpio5) {
		jack_irq_rise = ARIZONA_IRQ_MICD_CLAMP_RISE;
		jack_irq_fall = ARIZONA_IRQ_MICD_CLAMP_FALL;
	} else {
		jack_irq_rise = ARIZONA_IRQ_JD_RISE;
		jack_irq_fall = ARIZONA_IRQ_JD_FALL;
	}

	arizona_set_irq_wake(arizona, jack_irq_rise, 0);
	arizona_set_irq_wake(arizona, jack_irq_fall, 0);
	arizona_free_irq(arizona, ARIZONA_IRQ_HPDET, info);
	arizona_free_irq(arizona, ARIZONA_IRQ_MICDET, info);
	arizona_free_irq(arizona, jack_irq_rise, info);
	arizona_free_irq(arizona, jack_irq_fall, info);
	cancel_delayed_work_sync(&info->hpdet_work);
	regmap_update_bits(arizona->regmap, ARIZONA_JACK_DETECT_ANALOGUE,
			   ARIZONA_JD1_ENA, 0);
	arizona_clk32k_disable(arizona);

	device_remove_file(&pdev->dev, &dev_attr_hp_impedance);
	switch_dev_unregister(&info->edev);
	wakeup_source_trash(&info->detection_wake_lock);

	return 0;
}

static struct platform_driver arizona_extcon_driver = {
	.driver		= {
		.name	= "arizona-extcon",
		.owner	= THIS_MODULE,
	},
	.probe		= arizona_extcon_probe,
	.remove		= arizona_extcon_remove,
};

module_platform_driver(arizona_extcon_driver);

MODULE_DESCRIPTION("Arizona Extcon driver");
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:extcon-arizona");
