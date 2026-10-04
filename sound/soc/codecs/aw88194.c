// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW88194 smart audio amplifier
 *
 * Settings come from the vendor's ACF container (the aw881xx_acf.bin that
 * the downstream aw881xx driver loads): the register table of the selected
 * profile, plus the firmware and configuration of the on-chip speaker
 * protection DSP.
 *
 * The DSP is bypassed unless the "dsp" module parameter is set: loading it
 * works and it starts executing, but its watchdog and speaker model never
 * come up the way they do under the vendor driver, so it is experimental.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define AW88194_ID			0x00
#define AW88194_CHIP_ID			0x1806

#define AW88194_SYSST			0x01
#define AW88194_SYSST_SWS		BIT(8)
#define AW88194_SYSST_PLLS		BIT(0)

#define AW88194_SYSINTM			0x03
#define AW88194_SYSINTM_ALL		0xffff

#define AW88194_SYSCTRL			0x04
#define AW88194_SYSCTRL_I2SEN		BIT(6)
#define AW88194_SYSCTRL_DSPBY		BIT(2)
#define AW88194_SYSCTRL_AMPPD		BIT(1)
#define AW88194_SYSCTRL_PWDN		BIT(0)

#define AW88194_I2SCTRL			0x05
#define AW88194_I2SCTRL_I2SFS		GENMASK(7, 6)
#define AW88194_I2SCTRL_I2SBCK		GENMASK(5, 4)
#define AW88194_I2SCTRL_I2SSR		GENMASK(3, 0)

#define AW88194_PWMCTRL			0x08
#define AW88194_PWMCTRL_HMUTE		BIT(0)

#define AW88194_HAGCCFG6		0x0f
#define AW88194_HAGCCFG6_VOL		GENMASK(15, 8)

#define AW88194_SYSCTRL2		0x11
#define AW88194_SYSCTRL2_MEMCLK_PLL	BIT(12)

/* DSP memory window and status */
#define AW88194_DSPMADD			0x40
#define AW88194_DSPMDAT			0x41
#define AW88194_WDT			0x42

/* eFuse: current and voltage sense gain trims */
#define AW88194_EFRM			0x78
#define AW88194_EFRM_ISENSE		GENMASK(7, 0)
#define AW88194_PRODUCT_ID		0x79
#define AW88194_PRODUCT_ID_PID		GENMASK(3, 0)
#define AW88194_PRODUCT_ID_VSENSE	GENMASK(15, 10)

#define AW88194_MAX_REG			0x7f

/* DSP memory map */
#define AW88194_DSP_CFG_ADDR		0x8600
#define AW88194_DSP_FW_ADDR		0x8c00
#define AW88194_DSP_VCALB		0x866d

#define AW88194_DSP_CHECK_MAX		3

/* Volume in 0.5 dB steps of attenuation, 0 to -96 dB */
#define AW88194_VOL_STEPS		192
/* Default when unprotected (DSP bypassed): at least 12 dB of attenuation */
#define AW88194_VOL_BYPASS_MIN		24

static bool dsp;
module_param(dsp, bool, 0444);
MODULE_PARM_DESC(dsp, "Load and run the speaker protection DSP (experimental)");

/* ACF container, header version 1.0.0.0 */
#define AW88194_ACF_HDR_V1		0x01000000
#define AW88194_ACF_DATA_REG		0
#define AW88194_ACF_DATA_DSP_CFG	2
#define AW88194_ACF_DATA_DSP_FW		3

struct aw88194_acf_hdr {
	__le32 id;
	char project[24];
	char custom[16];
	char version[4];
	__le32 author_id;
	__le32 ddt_size;
	__le32 ddt_num;
	__le32 hdr_offset;
	__le32 hdr_version;
	__le32 reserved[3];
} __packed;

struct aw88194_acf_dde {
	__le32 type;
	char dev_name[16];
	__le16 dev_index;
	__le16 dev_bus;
	__le16 dev_addr;
	__le16 dev_profile;
	__le32 data_type;
	__le32 data_size;
	__le32 data_offset;
	__le32 data_crc;
	char dev_profile_str[32];
	__le32 chip_id;
	__le32 reserved[4];
} __packed;

struct aw88194_reg {
	u8 reg;
	u16 val;
};

struct aw88194_blob {
	u16 *words;
	unsigned int len;
};

struct aw88194 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct aw88194_reg *init;
	unsigned int num_init;
	struct aw88194_blob dsp_fw;
	struct aw88194_blob dsp_cfg;
	bool dsp;		/* DSP loaded and in use */
	unsigned int sysctrl;	/* SYSCTRL from the profile, powered down */
	unsigned int pwmctrl;	/* PWMCTRL from the profile, muted */
	unsigned int vol;	/* attenuation in 0.5 dB steps */
};

static const struct regmap_config aw88194_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW88194_MAX_REG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

/* VOL is split into 6 dB steps (bits 7:4) and 0.5 dB steps (bits 3:0) */
static unsigned int aw88194_vol_to_reg(unsigned int att)
{
	return ((att / 12) << 4) | (att % 12);
}

static unsigned int aw88194_reg_to_vol(unsigned int reg)
{
	return min((reg >> 4) * 12 + (reg & 0xf), AW88194_VOL_STEPS);
}

static int aw88194_dsp_write(struct aw88194 *aw, u16 addr, const u16 *words,
			     unsigned int len)
{
	unsigned int i;
	int ret;

	/* The address auto-increments with each data write */
	ret = regmap_write(aw->regmap, AW88194_DSPMADD, addr);
	for (i = 0; !ret && i < len; i++)
		ret = regmap_write(aw->regmap, AW88194_DSPMDAT, words[i]);

	return ret;
}

static int aw88194_dsp_read(struct aw88194 *aw, u16 addr, unsigned int *val)
{
	int ret;

	ret = regmap_write(aw->regmap, AW88194_DSPMADD, addr);
	if (ret)
		return ret;

	return regmap_read(aw->regmap, AW88194_DSPMDAT, val);
}

static int aw88194_copy_blob(struct aw88194 *aw, struct aw88194_blob *blob,
			     const u8 *data, u32 size)
{
	unsigned int i;

	if (size % 2)
		return -EINVAL;

	blob->words = devm_kcalloc(aw->dev, size / 2, sizeof(u16), GFP_KERNEL);
	if (!blob->words)
		return -ENOMEM;

	/* Stored as little-endian 16-bit words */
	for (i = 0; i < size / 2; i++)
		blob->words[i] = get_unaligned_le16(data + 2 * i);
	blob->len = size / 2;

	return 0;
}

static int aw88194_copy_regs(struct aw88194 *aw, const u8 *data, u32 size)
{
	unsigned int i;

	if (size % 4)
		return -EINVAL;

	aw->init = devm_kcalloc(aw->dev, size / 4, sizeof(*aw->init),
				GFP_KERNEL);
	if (!aw->init)
		return -ENOMEM;

	/* Pairs of little-endian 16-bit address and value */
	for (i = 0; i < size / 4; i++, data += 4) {
		u16 reg = get_unaligned_le16(data);
		u16 val = get_unaligned_le16(data + 2);

		if (reg > AW88194_MAX_REG)
			continue;

		switch (reg) {
		case AW88194_SYSCTRL:
			aw->sysctrl = val;
			continue;
		case AW88194_PWMCTRL:
			aw->pwmctrl = val;
			continue;
		case AW88194_HAGCCFG6:
			aw->vol = aw88194_reg_to_vol(
				FIELD_GET(AW88194_HAGCCFG6_VOL, val));
			break;
		}

		aw->init[aw->num_init].reg = reg;
		aw->init[aw->num_init].val = val;
		aw->num_init++;
	}

	return 0;
}

static int aw88194_load_profile(struct aw88194 *aw, const struct firmware *fw,
				const char *dev_name, const char *profile)
{
	const struct aw88194_acf_hdr *hdr = (const void *)fw->data;
	const struct aw88194_acf_dde *dde;
	u32 num, offset, i;
	int ret;

	if (fw->size < sizeof(*hdr) ||
	    le32_to_cpu(hdr->hdr_version) != AW88194_ACF_HDR_V1) {
		dev_err(aw->dev, "unsupported ACF container\n");
		return -EINVAL;
	}

	num = le32_to_cpu(hdr->ddt_num);
	offset = le32_to_cpu(hdr->hdr_offset);
	if (offset > fw->size || num > (fw->size - offset) / sizeof(*dde)) {
		dev_err(aw->dev, "truncated ACF container\n");
		return -EINVAL;
	}

	dde = (const void *)(fw->data + offset);
	for (i = 0; i < num; i++, dde++) {
		u32 size = le32_to_cpu(dde->data_size);
		u32 data = le32_to_cpu(dde->data_offset);

		if (le32_to_cpu(dde->chip_id) != AW88194_CHIP_ID ||
		    strncmp(dde->dev_name, dev_name, sizeof(dde->dev_name)) ||
		    strncmp(dde->dev_profile_str, profile,
			    sizeof(dde->dev_profile_str)))
			continue;

		if (data > fw->size || size > fw->size - data) {
			dev_err(aw->dev, "bad ACF section\n");
			return -EINVAL;
		}

		switch (le32_to_cpu(dde->data_type)) {
		case AW88194_ACF_DATA_REG:
			ret = aw88194_copy_regs(aw, fw->data + data, size);
			break;
		case AW88194_ACF_DATA_DSP_CFG:
			ret = aw88194_copy_blob(aw, &aw->dsp_cfg,
						fw->data + data, size);
			break;
		case AW88194_ACF_DATA_DSP_FW:
			ret = aw88194_copy_blob(aw, &aw->dsp_fw,
						fw->data + data, size);
			break;
		default:
			ret = 0;
			break;
		}
		if (ret) {
			dev_err(aw->dev, "bad ACF section: %d\n", ret);
			return ret;
		}
	}

	if (!aw->num_init) {
		dev_err(aw->dev, "no register profile for %s/%s\n",
			dev_name, profile);
		return -ENOENT;
	}

	aw->dsp = dsp && aw->dsp_fw.len && aw->dsp_cfg.len;
	if (!aw->dsp)
		aw->vol = max(aw->vol, AW88194_VOL_BYPASS_MIN);

	/* Powered down and muted until a stream starts */
	aw->sysctrl |= AW88194_SYSCTRL_I2SEN | AW88194_SYSCTRL_AMPPD |
		       AW88194_SYSCTRL_PWDN;
	aw->pwmctrl |= AW88194_PWMCTRL_HMUTE;

	dev_info(aw->dev, "%s/%s: %u registers, DSP %s\n", dev_name, profile,
		 aw->num_init, aw->dsp ? "firmware loaded" : "bypassed");
	return 0;
}

static int aw88194_set_vcalb(struct aw88194 *aw)
{
	unsigned int efrm, pid;
	int icalk, vcalk;
	u16 vcalb;
	int ret;

	ret = regmap_read(aw->regmap, AW88194_EFRM, &efrm);
	if (!ret)
		ret = regmap_read(aw->regmap, AW88194_PRODUCT_ID, &pid);
	if (ret)
		return ret;

	/* Signed trims: 8-bit current sense, 6-bit voltage sense */
	icalk = 1000 + 2 * sign_extend32(FIELD_GET(AW88194_EFRM_ISENSE, efrm), 7);
	vcalk = 1000 + 5 * sign_extend32(FIELD_GET(AW88194_PRODUCT_ID_VSENSE, pid), 5);

	vcalb = (4096 * 1550 / 426) * vcalk / icalk;
	dev_dbg(aw->dev, "icalk %d vcalk %d vcalb %#06x\n", icalk, vcalk, vcalb);

	return aw88194_dsp_write(aw, AW88194_DSP_VCALB, &vcalb, 1);
}

/*
 * Load the DSP firmware and configuration. The DSP memory runs from the
 * internal oscillator while it is written, so the device must be out of
 * power-down (PWDN = 0) but no I2S clock is needed.
 */
static int aw88194_dsp_load(struct aw88194 *aw)
{
	unsigned int i, val;
	int ret;

	ret = regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
				 AW88194_SYSCTRL_DSPBY, AW88194_SYSCTRL_DSPBY);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW88194_SYSCTRL2,
				 AW88194_SYSCTRL2_MEMCLK_PLL, 0);
	if (ret)
		return ret;

	ret = aw88194_dsp_write(aw, AW88194_DSP_FW_ADDR, aw->dsp_fw.words,
				aw->dsp_fw.len);
	if (!ret)
		ret = aw88194_dsp_write(aw, AW88194_DSP_CFG_ADDR,
					aw->dsp_cfg.words, aw->dsp_cfg.len);
	if (!ret)
		ret = aw88194_set_vcalb(aw);

	/* Replay DSP memory writes from the register table, in order */
	for (i = 0; !ret && i < aw->num_init; i++)
		if (aw->init[i].reg == AW88194_DSPMADD ||
		    aw->init[i].reg == AW88194_DSPMDAT)
			ret = regmap_write(aw->regmap, aw->init[i].reg,
					   aw->init[i].val);

	/* Spot check that the memory took the data */
	if (!ret)
		ret = aw88194_dsp_read(aw, AW88194_DSP_FW_ADDR, &val);
	if (!ret && val != aw->dsp_fw.words[0]) {
		dev_err(aw->dev, "DSP memory readback %#06x, expected %#06x\n",
			val, aw->dsp_fw.words[0]);
		ret = -EIO;
	}

	regmap_update_bits(aw->regmap, AW88194_SYSCTRL2,
			   AW88194_SYSCTRL2_MEMCLK_PLL,
			   AW88194_SYSCTRL2_MEMCLK_PLL);
	return ret;
}

static int aw88194_hw_init(struct aw88194 *aw)
{
	unsigned int i;
	int ret;

	/* Interrupts stay masked; nothing handles them */
	for (i = 0; i < aw->num_init; i++) {
		unsigned int val = aw->init[i].val;

		if (aw->init[i].reg == AW88194_SYSINTM)
			val = AW88194_SYSINTM_ALL;

		ret = regmap_write(aw->regmap, aw->init[i].reg, val);
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(aw->regmap, AW88194_HAGCCFG6,
				 AW88194_HAGCCFG6_VOL,
				 FIELD_PREP(AW88194_HAGCCFG6_VOL,
					    aw88194_vol_to_reg(aw->vol)));
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW88194_PWMCTRL, aw->pwmctrl);
	if (ret)
		return ret;

	if (aw->dsp) {
		/* Out of power-down, amplifier still off, DSP bypassed */
		ret = regmap_write(aw->regmap, AW88194_SYSCTRL,
				   (aw->sysctrl & ~AW88194_SYSCTRL_PWDN) |
				   AW88194_SYSCTRL_DSPBY);
		if (ret)
			return ret;

		usleep_range(2000, 3000);
		ret = aw88194_dsp_load(aw);
		if (ret) {
			dev_warn(aw->dev, "DSP load failed (%d), bypassing it\n",
				 ret);
			aw->dsp = false;
		}
	}

	if (aw->dsp)
		aw->sysctrl &= ~AW88194_SYSCTRL_DSPBY;
	else
		aw->sysctrl |= AW88194_SYSCTRL_DSPBY;

	/* The DSP stays bypassed until the amplifier is running */
	return regmap_write(aw->regmap, AW88194_SYSCTRL,
			    aw->sysctrl | AW88194_SYSCTRL_DSPBY);
}

/* Start the DSP and check that its watchdog sees it running */
static int aw88194_dsp_start(struct aw88194 *aw)
{
	unsigned int wdt;
	int i, ret;

	for (i = 0; i < AW88194_DSP_CHECK_MAX; i++) {
		if (i) {
			dev_warn(aw->dev, "DSP not running, reloading\n");
			ret = aw88194_dsp_load(aw);
			if (ret)
				return ret;
		}

		ret = regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
					 AW88194_SYSCTRL_DSPBY,
					 AW88194_SYSCTRL_DSPBY);
		if (!ret)
			ret = regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
						 AW88194_SYSCTRL_DSPBY, 0);
		if (ret)
			return ret;

		usleep_range(1000, 2000);
		ret = regmap_read(aw->regmap, AW88194_WDT, &wdt);
		if (ret)
			return ret;
		if (wdt)
			return 0;
	}

	/* Play unprotected rather than not at all */
	dev_err(aw->dev, "DSP failed to start, bypassing it\n");
	return regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
				  AW88194_SYSCTRL_DSPBY, AW88194_SYSCTRL_DSPBY);
}

static int aw88194_power_up(struct aw88194 *aw)
{
	unsigned int val;
	int ret;

	/* Bias, oscillator and PLL; the PLL locks to the running I2S clocks */
	ret = regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
				 AW88194_SYSCTRL_PWDN, 0);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(aw->regmap, AW88194_SYSST, val,
				       val & AW88194_SYSST_PLLS, 1000, 50000);
	if (ret) {
		dev_err(aw->dev, "PLL not locked (SYSST %#06x)\n", val);
		goto err_pwdn;
	}

	/* Boost and class D */
	ret = regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
				 AW88194_SYSCTRL_AMPPD, 0);
	if (ret)
		goto err_pwdn;

	ret = regmap_read_poll_timeout(aw->regmap, AW88194_SYSST, val,
				       val & AW88194_SYSST_SWS, 1000, 50000);
	if (ret) {
		dev_err(aw->dev, "amplifier not switching (SYSST %#06x)\n", val);
		goto err_pwdn;
	}

	if (aw->dsp) {
		ret = aw88194_dsp_start(aw);
		if (ret)
			goto err_pwdn;
	}

	return regmap_update_bits(aw->regmap, AW88194_PWMCTRL,
				  AW88194_PWMCTRL_HMUTE, 0);

err_pwdn:
	regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
			   AW88194_SYSCTRL_AMPPD | AW88194_SYSCTRL_PWDN,
			   AW88194_SYSCTRL_AMPPD | AW88194_SYSCTRL_PWDN);
	return ret;
}

static void aw88194_power_down(struct aw88194 *aw)
{
	regmap_update_bits(aw->regmap, AW88194_PWMCTRL, AW88194_PWMCTRL_HMUTE,
			   AW88194_PWMCTRL_HMUTE);
	regmap_update_bits(aw->regmap, AW88194_SYSCTRL,
			   AW88194_SYSCTRL_DSPBY | AW88194_SYSCTRL_AMPPD |
			   AW88194_SYSCTRL_PWDN,
			   AW88194_SYSCTRL_DSPBY | AW88194_SYSCTRL_AMPPD |
			   AW88194_SYSCTRL_PWDN);
}

static int aw88194_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct aw88194 *aw = snd_soc_component_get_drvdata(dai->component);

	if (mute) {
		aw88194_power_down(aw);
		return 0;
	}

	return aw88194_power_up(aw);
}

static int aw88194_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct aw88194 *aw = snd_soc_component_get_drvdata(dai->component);
	unsigned int sr, fs, bck;

	switch (params_rate(params)) {
	case 8000:	sr = 0; break;
	case 11025:	sr = 1; break;
	case 12000:	sr = 2; break;
	case 16000:	sr = 3; break;
	case 22050:	sr = 4; break;
	case 24000:	sr = 5; break;
	case 32000:	sr = 6; break;
	case 44100:	sr = 7; break;
	case 48000:	sr = 8; break;
	case 96000:	sr = 9; break;
	default:
		return -EINVAL;
	}

	/* Data width, and bit clocks per frame from the slot width */
	switch (params_width(params)) {
	case 16:	fs = 0; break;
	case 20:	fs = 1; break;
	case 24:	fs = 2; break;
	case 32:	fs = 3; break;
	default:
		return -EINVAL;
	}

	switch (params_physical_width(params)) {
	case 16:	bck = 0; break;
	case 24:	bck = 1; break;
	case 32:	bck = 2; break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(aw->regmap, AW88194_I2SCTRL,
				  AW88194_I2SCTRL_I2SFS | AW88194_I2SCTRL_I2SBCK |
				  AW88194_I2SCTRL_I2SSR,
				  FIELD_PREP(AW88194_I2SCTRL_I2SFS, fs) |
				  FIELD_PREP(AW88194_I2SCTRL_I2SBCK, bck) |
				  FIELD_PREP(AW88194_I2SCTRL_I2SSR, sr));
}

static int aw88194_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops aw88194_dai_ops = {
	.set_fmt = aw88194_set_fmt,
	.hw_params = aw88194_hw_params,
	.mute_stream = aw88194_mute_stream,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver aw88194_dai = {
	.name = "aw88194-aif",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000 | SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &aw88194_dai_ops,
};

static int aw88194_vol_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88194 *aw = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = AW88194_VOL_STEPS - aw->vol;
	return 0;
}

static int aw88194_vol_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88194 *aw = snd_soc_component_get_drvdata(component);
	long val = ucontrol->value.integer.value[0];
	int ret;

	if (val < 0 || val > AW88194_VOL_STEPS)
		return -EINVAL;
	if (AW88194_VOL_STEPS - val == aw->vol)
		return 0;

	ret = regmap_update_bits(aw->regmap, AW88194_HAGCCFG6,
				 AW88194_HAGCCFG6_VOL,
				 FIELD_PREP(AW88194_HAGCCFG6_VOL,
					    aw88194_vol_to_reg(AW88194_VOL_STEPS - val)));
	if (ret)
		return ret;

	aw->vol = AW88194_VOL_STEPS - val;
	return 1;
}

static const DECLARE_TLV_DB_SCALE(aw88194_vol_tlv, -9600, 50, 0);

static const struct snd_kcontrol_new aw88194_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.tlv.p = aw88194_vol_tlv,
		.info = snd_soc_info_volsw,
		.get = aw88194_vol_get,
		.put = aw88194_vol_put,
		.private_value = SOC_SINGLE_VALUE(SND_SOC_NOPM, 0, 0,
						  AW88194_VOL_STEPS, 0, 0),
	},
};

static const struct snd_soc_dapm_widget aw88194_widgets[] = {
	SND_SOC_DAPM_AIF_IN("AIF_RX", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("SPK"),
};

static const struct snd_soc_dapm_route aw88194_routes[] = {
	{ "SPK", NULL, "AIF_RX" },
};

static const struct snd_soc_component_driver aw88194_component = {
	.controls = aw88194_controls,
	.num_controls = ARRAY_SIZE(aw88194_controls),
	.dapm_widgets = aw88194_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw88194_widgets),
	.dapm_routes = aw88194_routes,
	.num_dapm_routes = ARRAY_SIZE(aw88194_routes),
	.endianness = 1,
};

static int aw88194_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	const char *fw_name = "aw881xx_acf.bin";
	const char *dev_name;
	const struct firmware *fw;
	struct aw88194 *aw;
	unsigned int id, val;
	int ret;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->dev = dev;
	i2c_set_clientdata(i2c, aw);

	aw->regmap = devm_regmap_init_i2c(i2c, &aw88194_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(dev, PTR_ERR(aw->regmap),
				     "failed to init regmap\n");

	/* The device is held in reset (power-down mode) until this point */
	aw->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset))
		return dev_err_probe(dev, PTR_ERR(aw->reset),
				     "failed to get reset gpio\n");
	if (aw->reset) {
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(aw->reset, 0);
		usleep_range(2000, 3000);
	}

	ret = regmap_read(aw->regmap, AW88194_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");
	if (id != AW88194_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "unknown chip ID %#06x\n", id);

	/* Start from the reset values in case the profile omits these */
	ret = regmap_read(aw->regmap, AW88194_SYSCTRL, &aw->sysctrl);
	if (!ret)
		ret = regmap_read(aw->regmap, AW88194_PWMCTRL, &aw->pwmctrl);
	if (!ret)
		ret = regmap_read(aw->regmap, AW88194_HAGCCFG6, &val);
	if (!ret)
		ret = regmap_read(aw->regmap, AW88194_PRODUCT_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read defaults\n");
	aw->vol = aw88194_reg_to_vol(FIELD_GET(AW88194_HAGCCFG6_VOL, val));

	/* Product ID 1 is the AW88194, anything else the AW88194A */
	dev_name = FIELD_GET(AW88194_PRODUCT_ID_PID, id) == 1 ?
		   "aw88194" : "aw88194A";

	device_property_read_string(dev, "firmware-name", &fw_name);
	device_property_read_string(dev, "awinic,dev-name", &dev_name);

	ret = request_firmware(&fw, fw_name, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to load %s\n", fw_name);

	ret = aw88194_load_profile(aw, fw, dev_name, "Music");
	release_firmware(fw);
	if (ret)
		return ret;

	ret = aw88194_hw_init(aw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize device\n");

	return devm_snd_soc_register_component(dev, &aw88194_component,
					       &aw88194_dai, 1);
}

static const struct of_device_id aw88194_of_match[] = {
	{ .compatible = "awinic,aw88194" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw88194_of_match);

static const struct i2c_device_id aw88194_i2c_id[] = {
	{ "aw88194" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw88194_i2c_id);

static struct i2c_driver aw88194_i2c_driver = {
	.driver = {
		.name = "aw88194",
		.of_match_table = aw88194_of_match,
	},
	.probe = aw88194_probe,
	.id_table = aw88194_i2c_id,
};
module_i2c_driver(aw88194_i2c_driver);

MODULE_DESCRIPTION("Awinic AW88194 smart amplifier driver");
MODULE_LICENSE("GPL");
