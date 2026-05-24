// SPDX-License-Identifier: GPL-2.0-only
//
// ASoC driver for NXP/Goodix TFA9874 speaker amplifier

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define TFA9874_SYS_CTRL		0x00
#define TFA9874_SYS_CTRL_PWDN		0
#define TFA9874_SYS_CTRL_I2CR		1
#define TFA9874_SYS_CTRL_AMPE		3
#define TFA9874_SYS_CTRL_DCA		4
#define TFA9874_SYS_CTRL1		0x01
#define TFA9874_SYS_CTRL1_MANSCONF	BIT(2)
#define TFA9874_AUDIO_CTR		0x02
#define TFA9874_AUDIO_CTR_AUDFS_MSK	GENMASK(3, 0)
#define TFA9874_AUDIO_CTR_DPSA		BIT(7)
#define TFA9874_REVISION		0x03
#define TFA9874_REVISION_REV_MSK	GENMASK(7, 0)
#define TFA9874_REVISION_ID		0x74
#define TFA9874_KEY1			0x0f
#define TFA9874_TDM_CONFIG0		0x20
#define TFA9874_TDM_CONFIG0_TDME	BIT(4)
#define TFA9874_TDM_CONFIG0_TDMCLINV	BIT(6)
#define TFA9874_TDM_CONFIG0_TDMNBCK_MSK	GENMASK(15, 12)
#define TFA9874_TDM_CONFIG1		0x21
#define TFA9874_TDM_CONFIG1_TDMSLOTS_MSK GENMASK(3, 0)
#define TFA9874_TDM_CONFIG1_TDMSLLN_MSK	GENMASK(8, 4)
#define TFA9874_TDM_CONFIG1_TDMBRMG_MSK	GENMASK(13, 9)
#define TFA9874_TDM_CONFIG1_TDMDEL	BIT(14)
#define TFA9874_TDM_CONFIG2		0x22
#define TFA9874_TDM_CONFIG2_TDMSSIZE_MSK GENMASK(6, 2)
#define TFA9874_TDM_CONFIG3		0x23
#define TFA9874_TDM_CONFIG3_TDMSPKE	BIT(0)
#define TFA9874_TDM_CONFIG3_TDMDCE	BIT(1)
#define TFA9874_TDM_CONFIG6		0x26
#define TFA9874_TDM_CONFIG6_TDMSPKS_MSK	GENMASK(3, 0)
#define TFA9874_KEY1_UNLOCK		0x5a6b
#define TFA9874_KEY2_PROTECTED_MSB	0xa0
#define TFA9874_KEY2			0xa1
#define TFA9874_KEY2_VALUE		0x005a
#define TFA9874_KEY2_XOR		0x005a
#define TFA9874_KEY2_READBACK		0xfb

struct tfa9874 {
	struct regmap *regmap;
	struct gpio_desc *reset_gpiod;
};

static const unsigned int tfa9874_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000,
};

static int tfa9874_find_sample_rate(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tfa9874_rates); ++i)
		if (tfa9874_rates[i] == rate)
			return i;

	return -EINVAL;
}

static int tfa9874_set_tdm_bitwidth(struct snd_soc_component *component,
					    unsigned int width, unsigned int channels)
{
	unsigned int nbck, slot_len, sample_size, bits_remaining;
	int ret;

	switch (width) {
	case 16:
		nbck = 0;
		slot_len = 15;
		sample_size = 15;
		break;
	case 24:
	case 32:
		nbck = 2;
		slot_len = 31;
		sample_size = 23;
		break;
	default:
		return -EINVAL;
	}

	if (channels < 1 || channels > 4)
		return -EINVAL;

	bits_remaining = slot_len - sample_size;

	ret = snd_soc_component_update_bits(component, TFA9874_TDM_CONFIG0,
					     TFA9874_TDM_CONFIG0_TDMNBCK_MSK,
					     FIELD_PREP(TFA9874_TDM_CONFIG0_TDMNBCK_MSK, nbck));
	if (ret)
		return ret;

	ret = snd_soc_component_update_bits(component, TFA9874_TDM_CONFIG1,
					     TFA9874_TDM_CONFIG1_TDMSLOTS_MSK |
					     TFA9874_TDM_CONFIG1_TDMSLLN_MSK |
					     TFA9874_TDM_CONFIG1_TDMBRMG_MSK,
					     FIELD_PREP(TFA9874_TDM_CONFIG1_TDMSLOTS_MSK, channels - 1) |
					     FIELD_PREP(TFA9874_TDM_CONFIG1_TDMSLLN_MSK, slot_len) |
					     FIELD_PREP(TFA9874_TDM_CONFIG1_TDMBRMG_MSK, bits_remaining));
	if (ret)
		return ret;

	return snd_soc_component_update_bits(component, TFA9874_TDM_CONFIG2,
					     TFA9874_TDM_CONFIG2_TDMSSIZE_MSK,
					     FIELD_PREP(TFA9874_TDM_CONFIG2_TDMSSIZE_MSK, sample_size));
}

static int tfa9874_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int clkinv, delay;
	int ret;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		clkinv = 0;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		clkinv = TFA9874_TDM_CONFIG0_TDMCLINV;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_LEFT_J:
		delay = 0;
		break;
	default:
		return -EINVAL;
	}

	ret = snd_soc_component_update_bits(component, TFA9874_TDM_CONFIG0,
					     TFA9874_TDM_CONFIG0_TDMCLINV, clkinv);
	if (ret)
		return ret;

	return snd_soc_component_update_bits(component, TFA9874_TDM_CONFIG1,
					     TFA9874_TDM_CONFIG1_TDMDEL, delay);
}

static int tfa9874_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	int sr, ret;

	sr = tfa9874_find_sample_rate(params_rate(params));
	if (sr < 0)
		return sr;

	ret = snd_soc_component_update_bits(component, TFA9874_AUDIO_CTR,
					     TFA9874_AUDIO_CTR_AUDFS_MSK,
					     FIELD_PREP(TFA9874_AUDIO_CTR_AUDFS_MSK, sr));
	if (ret)
		return ret;

	return tfa9874_set_tdm_bitwidth(component, params_width(params),
					     params_channels(params));
}

static const struct snd_soc_dai_ops tfa9874_dai_ops = {
	.set_fmt = tfa9874_set_fmt,
	.hw_params = tfa9874_hw_params,
};

#define TFA9874_RATES		SNDRV_PCM_RATE_8000_96000
#define TFA9874_FORMATS		(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver tfa9874_dai = {
	.name = "tfa9874-hifi",
	.playback = {
		.stream_name	= "HiFi Playback",
		.formats	= TFA9874_FORMATS,
		.rates		= TFA9874_RATES,
		.rate_min	= 8000,
		.rate_max	= 96000,
		.channels_min	= 1,
		.channels_max	= 4,
	},
	.ops = &tfa9874_dai_ops,
};

static const struct snd_soc_dapm_widget tfa9874_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_SUPPLY("POWER", TFA9874_SYS_CTRL, TFA9874_SYS_CTRL_PWDN, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY("DCA", TFA9874_SYS_CTRL, TFA9874_SYS_CTRL_DCA, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("TDM", TFA9874_TDM_CONFIG0, 4, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("AMPE", TFA9874_SYS_CTRL, TFA9874_SYS_CTRL_AMPE, 0, NULL, 0),
	SND_SOC_DAPM_AIF_IN("AIFIN", "HiFi Playback", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route tfa9874_dapm_routes[] = {
	{ "OUT", NULL, "AMPE" },
	{ "AMPE", NULL, "POWER" },
	{ "AMPE", NULL, "DCA" },
	{ "AMPE", NULL, "TDM" },
	{ "AMPE", NULL, "AIFIN" },
};

static const struct snd_soc_component_driver tfa9874_component = {
	.dapm_widgets		= tfa9874_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tfa9874_dapm_widgets),
	.dapm_routes		= tfa9874_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tfa9874_dapm_routes),
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct reg_sequence tfa9874_init_0a74[] = {
	{ 0x02, 0x22a8 },
	{ 0x51, 0x0020 },
	{ 0x52, 0x57dc },
	{ 0x58, 0x16a4 },
	{ 0x61, 0x0110 },
	{ 0x66, 0x0701 },
	{ 0x6f, 0x00a3 },
	{ 0x70, 0x07f8 },
	{ 0x73, 0x0007 },
	{ 0x74, 0x5068 },
	{ 0x75, 0x0d28 },
	{ 0x83, 0x0594 },
	{ 0x84, 0x0001 },
	{ 0x85, 0x0001 },
	{ 0x88, 0x0000 },
	{ 0xc4, 0x2001 },
};

static const struct reg_sequence tfa9874_init_0b74[] = {
	{ 0x02, 0x22a8 },
	{ 0x51, 0x0020 },
	{ 0x52, 0x57dc },
	{ 0x58, 0x16a4 },
	{ 0x61, 0x0110 },
	{ 0x66, 0x0701 },
	{ 0x6f, 0x00a3 },
	{ 0x70, 0x07f8 },
	{ 0x73, 0x0047 },
	{ 0x74, 0x5068 },
	{ 0x75, 0x0d28 },
	{ 0x83, 0x0595 },
	{ 0x84, 0x0001 },
	{ 0x85, 0x0001 },
	{ 0x88, 0x0000 },
	{ 0xc4, 0x2001 },
};

static const struct reg_sequence tfa9874_init_0c74[] = {
	{ 0x02, 0x22c8 },
	{ 0x52, 0x57dc },
	{ 0x53, 0x003e },
	{ 0x56, 0x0400 },
	{ 0x61, 0x0110 },
	{ 0x6f, 0x00a5 },
	{ 0x70, 0x07f8 },
	{ 0x73, 0x0047 },
	{ 0x74, 0x5098 },
	{ 0x75, 0x8d28 },
	{ 0x80, 0x0000 },
	{ 0x83, 0x0799 },
	{ 0x84, 0x0081 },
};

static const struct regmap_config tfa9874_regmap = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
	.cache_type = REGCACHE_RBTREE,
};

static int tfa9874_unlock_protected_regs(struct regmap *regmap)
{
	unsigned int val;
	int ret;

	ret = regmap_write(regmap, TFA9874_KEY1, TFA9874_KEY1_UNLOCK);
	if (ret)
		return ret;

	ret = regmap_read(regmap, TFA9874_KEY2_READBACK, &val);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TFA9874_KEY2_PROTECTED_MSB,
			   (val ^ TFA9874_KEY2_XOR) & 0xffff);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TFA9874_KEY1, TFA9874_KEY1_UNLOCK);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TFA9874_KEY2, TFA9874_KEY2_VALUE);
	if (ret)
		return ret;

	return regmap_write(regmap, TFA9874_KEY1, 0x0000);
}

static int tfa9874_write_init_sequence(struct device *dev, struct regmap *regmap,
				       unsigned int revision)
{
	const struct reg_sequence *sequence;
	unsigned int sequence_len;
	int ret;

	ret = tfa9874_unlock_protected_regs(regmap);
	if (ret)
		return ret;

	switch (revision) {
	case 0x0a74:
		sequence = tfa9874_init_0a74;
		sequence_len = ARRAY_SIZE(tfa9874_init_0a74);
		break;
	case 0x0b74:
		sequence = tfa9874_init_0b74;
		sequence_len = ARRAY_SIZE(tfa9874_init_0b74);
		break;
	case 0x0c74:
		sequence = tfa9874_init_0c74;
		sequence_len = ARRAY_SIZE(tfa9874_init_0c74);
		break;
	default:
		dev_warn(dev, "unknown TFA9874 revision %#x, using latest init sequence\n", revision);
		sequence = tfa9874_init_0c74;
		sequence_len = ARRAY_SIZE(tfa9874_init_0c74);
		break;
	}

	ret = regmap_multi_reg_write(regmap, sequence, sequence_len);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, TFA9874_AUDIO_CTR,
					 TFA9874_AUDIO_CTR_DPSA, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, TFA9874_SYS_CTRL1,
					 TFA9874_SYS_CTRL1_MANSCONF,
					 TFA9874_SYS_CTRL1_MANSCONF);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, TFA9874_TDM_CONFIG3,
				 TFA9874_TDM_CONFIG3_TDMSPKE |
				 TFA9874_TDM_CONFIG3_TDMDCE,
				 TFA9874_TDM_CONFIG3_TDMSPKE);
	if (ret)
		return ret;

	return regmap_update_bits(regmap, TFA9874_TDM_CONFIG6,
				  TFA9874_TDM_CONFIG6_TDMSPKS_MSK, 0);
}

static void tfa9874_hw_reset(struct tfa9874 *tfa9874)
{
	if (!tfa9874->reset_gpiod)
		return;

	gpiod_set_value_cansleep(tfa9874->reset_gpiod, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(tfa9874->reset_gpiod, 0);
	usleep_range(10000, 12000);
}

static int tfa9874_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct tfa9874 *tfa9874;
	unsigned int val, revision;
	int ret;

	tfa9874 = devm_kzalloc(dev, sizeof(*tfa9874), GFP_KERNEL);
	if (!tfa9874)
		return -ENOMEM;

	i2c_set_clientdata(i2c, tfa9874);

	tfa9874->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(tfa9874->reset_gpiod))
		return dev_err_probe(dev, PTR_ERR(tfa9874->reset_gpiod),
				     "failed to get reset GPIO\n");

	tfa9874->regmap = devm_regmap_init_i2c(i2c, &tfa9874_regmap);
	if (IS_ERR(tfa9874->regmap))
		return PTR_ERR(tfa9874->regmap);

	tfa9874_hw_reset(tfa9874);

	regcache_cache_bypass(tfa9874->regmap, true);

	ret = regmap_read(tfa9874->regmap, TFA9874_REVISION, &val);
	if (ret) {
		dev_err(dev, "failed to read revision number: %d\n", ret);
		return ret;
	}

	revision = val & 0xffff;
	if ((revision & TFA9874_REVISION_REV_MSK) != TFA9874_REVISION_ID) {
		dev_err(dev, "invalid revision number, expected %#x, got %#x\n",
			TFA9874_REVISION_ID,
			(unsigned int)(revision & TFA9874_REVISION_REV_MSK));
		return -ENODEV;
	}

	ret = regmap_write(tfa9874->regmap, TFA9874_SYS_CTRL,
			   BIT(TFA9874_SYS_CTRL_I2CR));
	if (ret) {
		dev_err(dev, "failed to reset I2C registers: %d\n", ret);
		return ret;
	}

	ret = tfa9874_write_init_sequence(dev, tfa9874->regmap, revision);
	if (ret) {
		dev_err(dev, "failed to initialize registers: %d\n", ret);
		return ret;
	}

	regcache_cache_bypass(tfa9874->regmap, false);

	return devm_snd_soc_register_component(dev, &tfa9874_component,
					       &tfa9874_dai, 1);
}

static const struct i2c_device_id tfa9874_i2c_id[] = {
	{ "tfa9874" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfa9874_i2c_id);

static const struct of_device_id tfa9874_of_match[] = {
	{ .compatible = "nxp,tfa9874" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa9874_of_match);

static struct i2c_driver tfa9874_i2c_driver = {
	.driver = {
		.name = "tfa9874",
		.of_match_table = tfa9874_of_match,
	},
	.probe = tfa9874_i2c_probe,
	.id_table = tfa9874_i2c_id,
};
module_i2c_driver(tfa9874_i2c_driver);

MODULE_DESCRIPTION("ASoC NXP/Goodix TFA9874 speaker amplifier driver");
MODULE_LICENSE("GPL");
