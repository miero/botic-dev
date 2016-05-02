/*
 * ESS Technology Sabre32 family Audio DAC support
 *
 * Miroslav Rudisin <miero@seznam.cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/of_platform.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

#include <asm/dma.h>
#include <asm/mach-types.h>

#include <linux/edma.h>
#include <linux/delay.h>

#include <linux/of_gpio.h>

#define SABRE32_CODEC_NAME "sabre32-codec"
#define SABRE32_CODEC_DAI_NAME "sabre32-hifi"

struct sabre32_codec_data {
    struct i2c_adapter *i2c_adapter;
    struct i2c_client *i2c_client1;
    struct regmap *client1;
    /* Second I2C channel for the second DAC in the dual mono configuration. */
    struct i2c_client *i2c_client2;
    struct regmap *client2;
    int stream_muted; /* ALSA is not playing */
    int force_mute; /* Master Mute control */
    int mute_mode; /* Mute Codec if not playing? */
    int last_clock_48k;
};

#define SABRE32_RATES (\
            SNDRV_PCM_RATE_CONTINUOUS | \
            SNDRV_PCM_RATE_11025 | \
            SNDRV_PCM_RATE_22050 | \
            SNDRV_PCM_RATE_44100 | \
            SNDRV_PCM_RATE_88200 | \
            SNDRV_PCM_RATE_176400 | \
            SNDRV_PCM_RATE_352800 | \
            SNDRV_PCM_RATE_705600 | \
            SNDRV_PCM_RATE_16000 | \
            SNDRV_PCM_RATE_32000 | \
            SNDRV_PCM_RATE_48000 | \
            SNDRV_PCM_RATE_96000 | \
            SNDRV_PCM_RATE_192000 | \
            SNDRV_PCM_RATE_384000 | \
            SNDRV_PCM_RATE_768000 | \
            0)

#define SABRE32_FORMATS (\
            SNDRV_PCM_FMTBIT_S16_LE | \
            SNDRV_PCM_FMTBIT_S24_3LE | \
            SNDRV_PCM_FMTBIT_S24_LE | \
            SNDRV_PCM_FMTBIT_S32_LE | \
            SNDRV_PCM_FMTBIT_DSD_U8 | \
            SNDRV_PCM_FMTBIT_DSD_U16_LE | \
            SNDRV_PCM_FMTBIT_DSD_U32_LE | \
            0)

static const struct snd_soc_dai_ops sabre32_codec_dai_ops;

static struct snd_soc_dai_driver sabre32_codec_dai = {
    .name = SABRE32_CODEC_DAI_NAME,
    .playback = {
        .channels_min = 2,
        .channels_max = 8,
        .rate_min = 11025,
        .rate_max = 768000,
        .rates = SABRE32_RATES,
        .formats = SABRE32_FORMATS,
    },
    /*
     * TODO: DAC does not have a record capability,
     * move this to another DAI of sound card.
     */
    .capture = {
        .channels_min = 2,
        .channels_max = 8,
        .rate_min = 11025,
        .rate_max = 768000,
        .rates = SABRE32_RATES,
        .formats = SABRE32_FORMATS,
    },
    .ops = &sabre32_codec_dai_ops,
};

#define VOLUME_MAXATTEN 199
#define VOLUME_HALFSTEPS 16

/*
 * VOLUME_HALFSTEPS+1 steps of the half volume
 * ~0.38dB change per step for 16 halfsteps
 */
static const unsigned int volume_steps[] = {
    0x7fffffff,
    0x7a92be89,
    0x75606373,
    0x70666f75,
    0x6ba27e64,
    0x67124609,
    0x62b39507,
    0x5e8451ce,
    0x5a827999,
    0x56ac1f74,
    0x52ff6b54,
    0x4f7a992f,
    0x4c1bf828,
    0x48e1e9b9,
    0x45cae0f1,
    0x42d561b3,
    0x3fffffff,
};

static const char *spdif_input_text[] = {
    "1", "2", "3", "4", "5", "6", "7", "8"
};

static SOC_ENUM_SINGLE_DECL(spdif_input, 2, 0, spdif_input_text);

static const char *bypass_or_use_text[] = {
    "Bypass", "Use"
};

static const char *deemphasis_filter_text[] = {
    "Bypass", "32kHz", "44.1kHz", "48kHz"
};

static SOC_ENUM_SINGLE_DECL(jitter_reduction, 3, 0, bypass_or_use_text);
static SOC_ENUM_SINGLE_DECL(deemphasis_filter, 4, 0, deemphasis_filter_text);

static const char *dpll_text[] = {
    "1x Auto",
    "128x Auto",
    "No",
    /* Notice: these values are just a guess from the datasheet info */
    "1x", "2x", "4x", "8x", "16x", "32x", "64x",
    "128x", "256x", "512x", "1024x", "2048x", "4096x", "8192x",
};

static SOC_ENUM_SINGLE_DECL(dpll, 5, 0, dpll_text);

static const char *iir_bw_text[] = {
    "Normal", "50k", "60k", "70k"
};

static SOC_ENUM_SINGLE_DECL(iir_bw, 6, 0, iir_bw_text);

static const char *fir_rolloff_text[] = {
    "Slow", "Fast"
};

static SOC_ENUM_SINGLE_DECL(fir_rolloff, 7, 0, fir_rolloff_text);

static const char *true_mono_text[] = {
    "Left", "Off", "Right"
};

static SOC_ENUM_SINGLE_DECL(true_mono, 8, 0, true_mono_text);

static const char *dpll_phase_text[] = {
    "Normal", "Flip"
};

static SOC_ENUM_SINGLE_DECL(dpll_phase, 9, 0, dpll_phase_text);

static const char *os_filter_text[] = {
    "Use", "Bypass"
};

static SOC_ENUM_SINGLE_DECL(os_filter, 10, 0, os_filter_text);

static const char *mute_mode_text[] = {
    "Never", "On Idle"
};

static SOC_ENUM_SINGLE_DECL(mute_mode, 11, 0, mute_mode_text);

static const char *remap_inputs_text[] = {
    "12345678", "12345676", "12345658", "12345656",
    "12325678", "12325676", "12325658", "12325656",
    "12145678", "12145676", "12145658", "12145656",
    "12125678", "12125676", "12125658", "12125656",
};

static SOC_ENUM_SINGLE_DECL(remap_inputs, 12, 0, remap_inputs_text);

static const char *mclk_notch_text[] = {
    "No Notch", "MCLK/4", "MCLK/8", "MCLK/16", "MCLK/32", "MCLK/64"
};

static SOC_ENUM_SINGLE_DECL(mclk_notch, 13, 0, mclk_notch_text);

static const char *remap_output_text[] = {
    "q6true", "q7pseudo", "q7true", "q8pseudo", "q8true", "q9pseudo"
};

static SOC_ENUM_SINGLE_DECL(remap_output, 14, 0, remap_output_text);

static const struct snd_kcontrol_new sabre32_codec_controls[] = {
    SOC_DOUBLE("Master Playback Volume", 0, 0, 0, VOLUME_MAXATTEN, 1),
    SOC_SINGLE("Master Playback Switch", 1, 0, 1, 1),
    SOC_ENUM("SPDIF Source", spdif_input),
    SOC_ENUM("Jitter Reduction", jitter_reduction),
    SOC_ENUM("De-emphasis Filter", deemphasis_filter),
    SOC_ENUM("DPLL", dpll),
    SOC_ENUM("IIR Bandwidth", iir_bw),
    SOC_ENUM("FIR Rolloff", fir_rolloff),
    SOC_ENUM("True Mono", true_mono),
    SOC_ENUM("DPLL Phase", dpll_phase),
    SOC_ENUM("Oversampling Filter", os_filter),
    SOC_ENUM("Mute Mode", mute_mode),
    SOC_ENUM("Remap Inputs", remap_inputs),
    SOC_ENUM("MCLK Notch", mclk_notch),
    SOC_ENUM("Remap Output", remap_output),
};

static const struct regmap_config empty_regmap_config;

static int sabre32_codec_probe(struct snd_soc_codec *codec)
{
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    struct regmap_config config;
    int ret;

    if (codec_data->i2c_adapter == NULL)
        return 0;

    codec_data->i2c_client1 = i2c_new_dummy(codec_data->i2c_adapter, 0x48);
    if (codec_data->i2c_client1 == NULL)
        return -EBUSY;

    codec_data->i2c_client2 = i2c_new_dummy(codec_data->i2c_adapter, 0x49);
    if (codec_data->i2c_client1 == NULL) {
        i2c_unregister_device(codec_data->i2c_client1);
        return -EBUSY;
    }

    /* ES9018 DAC */
    config = empty_regmap_config;
    config.val_bits = 8;
    config.reg_bits = 8;
    config.max_register = 72;

    codec_data->client1 =
        devm_regmap_init_i2c(codec_data->i2c_client1, &config);

    codec_data->client2 =
        devm_regmap_init_i2c(codec_data->i2c_client2, &config);

    /* Mute DAC */

    ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x01);
    if (ret != 0) {
        dev_warn(codec->dev, "DAC#1 not found\n");
        i2c_unregister_device(codec_data->i2c_client1);
        codec_data->i2c_client1 = NULL;
    }
    ret = regmap_update_bits(codec_data->client2, 10, 0x01, 0x01);
    if (ret != 0) {
        dev_warn(codec->dev, "DAC#2 not found\n");
        i2c_unregister_device(codec_data->i2c_client2);
        codec_data->i2c_client2 = NULL;
    }

    /* Initialize codec params. */
    codec_data->force_mute = 0;
    codec_data->stream_muted = 1;
    codec_data->mute_mode = 1;
    codec_data->last_clock_48k = -1; /* force relock on the first use */

    if (0) {
        int i;
        int r;
        unsigned int v;
        for (i=0;i<72;i++) {
            r = regmap_read(codec_data->client1, i, &v);
            printk(" %c%02x", (r == 0 ? ' ' : '*'), v);
            if (i % 8 == 7) printk("\n");
        }
    }


    return 0;
}

static int sabre32_codec_remove(struct snd_soc_codec *codec)
{
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);

    if (codec_data->i2c_client1 != NULL)
        i2c_unregister_device(codec_data->i2c_client1);

    if (codec_data->i2c_client2 != NULL)
        i2c_unregister_device(codec_data->i2c_client2);

    return 0;
}

static unsigned int sabre32_codec_read(struct snd_soc_codec *codec,
        unsigned int reg)
{
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    unsigned int v = 0;
    unsigned int t, t2;
    int r = 0;

    if (codec_data->client1 == NULL)
        return 0;

    switch(reg) {
    case 0: /* Master Volume */
        r = regmap_read(codec_data->client1, 23, &t);
        v = t & 0x7f;
        v <<= 8;
        r |= regmap_read(codec_data->client1, 22, &t);
        v |= t;
        v <<= 8;
        r |= regmap_read(codec_data->client1, 21, &t);
        v |= t;
        v <<= 8;
        r |= regmap_read(codec_data->client1, 20, &t);
        v |= t;
        /* convert 0x7fffffff--0 to 0-VOLUME_MAXATTEN */
        t = v;
        if (t != 0) {
            v = 0;
            while (t <= volume_steps[VOLUME_HALFSTEPS]) {
                t <<= 1;
                v += VOLUME_HALFSTEPS;
            }
            for (t2 = 1; t2 < VOLUME_HALFSTEPS; t2++) {
                if (t > volume_steps[t2])
                    break;
                v++;
            }
            if (v > VOLUME_MAXATTEN)
                v = VOLUME_MAXATTEN;
        } else
            v = VOLUME_MAXATTEN;
        break;
    case 1: /* Master Volume Mute */
        v = codec_data->force_mute;
        break;
    case 2: /* SPDIF Source */
        v = 0;
        r = regmap_read(codec_data->client1, 18, &t);
        while (t > 0) {
            v++;
            t &= ~1U;
            t >>= 1;
        }
        v--;
        break;
    case 3: /* Jitter reduction */
        r = regmap_read(codec_data->client1, 10, &t);
        v = !!(t & 0x04);
        break;
    case 4: /* De-emphasis filter */
        r = regmap_read(codec_data->client1, 10, &t);
        v = !(t & 0x02);
        if (!r && v) {
            r = regmap_read(codec_data->client1, 11, &t);
            v = (t & 0x3) + 1;
            if (v > 3) {
                /* skip reserved value */
                v = 0;
            }
        }
        break;
    case 5: /* DPLL */
        r = regmap_read(codec_data->client1, 25, &t);
        if (!r) {
            if (t & 0x02) {
                /* Auto */
                v = t & 0x01;
            } else {
                r = regmap_read(codec_data->client1, 11, &t2);
                v = (t2 & 0x1c) >> 2;
                if ((v > 0) && !!(t & 0x01))
                    v += 7;
                v += 2;
            }
        }
        break;
    case 6: /* IIR Bandwidth */
        r = regmap_read(codec_data->client1, 14, &t);
        v = (t & 0x06) >> 1;
        break;
    case 7: /* FIR Rolloff */
        r = regmap_read(codec_data->client1, 14, &t);
        v = t & 0x01;
        break;
    case 8: /* True Mono */
        r = regmap_read(codec_data->client1, 17, &t);
        if (!r) {
            if (t & 0x01)
                v = 2 * !!(t & 0x80);
            else
                v = 1;
        }
        break;
    case 9: /* DPLL Phase */
        r = regmap_read(codec_data->client1, 17, &t);
        v = !!(t & 0x02);
        break;
    case 10: /* Oversampling Filter */
        r = regmap_read(codec_data->client1, 17, &t);
        v = !!(t & 0x40);
        break;
    case 11: /* Mute Mode */
        v = codec_data->mute_mode;
        break;
    case 12: /* Remap Inputs */
        r = regmap_read(codec_data->client1, 14, &t);
        v = (t & 0xf0) >> 4;
        break;
    case 13: /* MCLK Notch */
        r = regmap_read(codec_data->client1, 12, &t);
        t = t & 0x1f;
        v = 0;
        while (t > 0) {
            t = (t & ~1U) >> 1;
            v++;
        }
        break;
    case 14: /* Remap Output (Quantizer & Differential) */
        r = regmap_read(codec_data->client1, 15, &t);
        v = 2 * (t & 0x03);
        if (!r) {
            r = regmap_read(codec_data->client1, 14, &t);
            if ((t & 0x08) == 0)
                v--;
        }
        /* Notice: Hides bad configuration as "6 True". */
        if (v < 0 || v > 5)
            v = 0;
        break;
    }

    if (!r)
        return v;
    else
        return 0;
}

static int sabre32_codec_write(struct snd_soc_codec *codec,
        unsigned int reg, unsigned int val)
{
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    unsigned int t;
    int ret = 0;

    if (codec_data->client1 == NULL)
        return 0;

    switch(reg) {
    case 0: /* Master Volume */
        if (val < VOLUME_MAXATTEN)
            t = volume_steps[val % VOLUME_HALFSTEPS] >>
                (val / VOLUME_HALFSTEPS);
        else
            t = 0;
        ret = regmap_write(codec_data->client1, 20, t & 0xff);
        t >>= 8;
        ret |= regmap_write(codec_data->client1, 21, t & 0xff);
        t >>= 8;
        ret |= regmap_write(codec_data->client1, 22, t & 0xff);
        t >>= 8;
        ret |= regmap_write(codec_data->client1, 23, t);
        break;
    case 1: /* Master Volume Mute */
        codec_data->force_mute = val;
        if (codec_data->force_mute)
            ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x01);
        else if (!codec_data->stream_muted)
            ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x00);
        break;
    case 2: /* SPDIF Source */
        ret = regmap_write(codec_data->client1, 18, 1U << val);
        break;
    case 3: /* Jitter reduction */
        ret = regmap_update_bits(codec_data->client1, 10, 0x04, 0x04 * val);
        break;
    case 4: /* De-emphasis filter */
        ret = 0;
        if (val > 0)
            ret = regmap_update_bits(codec_data->client1, 11, 0x03, val - 1);
        if (!ret)
            ret = regmap_update_bits(codec_data->client1, 10, 0x02, (!val) << 1);
        break;
    case 5: /* DPLL */
        if (val < 2) {
            /* Auto */
            ret = regmap_update_bits(codec_data->client1, 11, 0x1c, 0);
            if (!ret)
                ret = regmap_update_bits(codec_data->client1, 25, 0x03, 2 + val);
        } else {
            val -= 2;
            if (val <= 7) {
                ret = regmap_update_bits(codec_data->client1, 11, 0x1c, val << 2);
                val = 0;
            } else {
                val -= 7;
                ret = regmap_update_bits(codec_data->client1, 11, 0x1c, val << 2);
                val = 1;
            }
            if (!ret)
                ret = regmap_update_bits(codec_data->client1, 25, 0x03, val);
        }
        break;
    case 6: /* IIR Bandwidth */
        ret = regmap_update_bits(codec_data->client1, 14, 0x06, val << 1);
        break;
    case 7: /* FIR Rolloff */
        ret = regmap_update_bits(codec_data->client1, 14, 0x01, val);
        break;
    case 8: /* True Mono */
        if (val == 1)
            ret = regmap_update_bits(codec_data->client1, 17, 0x81, 0);
        else
            ret = regmap_update_bits(codec_data->client1, 17, 0x81,
                    (0x80 * !!val) + 1);
        break;
    case 9: /* DPLL Phase */
        ret = regmap_update_bits(codec_data->client1, 17, 0x02, 0x02 * !!val);
        break;
    case 10: /* Oversampling Filter */
        ret = regmap_update_bits(codec_data->client1, 17, 0x40, 0x40 * !!val);
        break;
    case 11: /* Mute Mode */
        codec_data->mute_mode = val;
        if (codec_data->mute_mode != 0) {
            if (codec_data->stream_muted)
                ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x01);
        } else {
            if (!codec_data->force_mute)
                ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x00);
        }
        break;
    case 12: /* Remap Inputs */
        ret = regmap_update_bits(codec_data->client1, 14, 0xf0, val << 4);
        break;
    case 13: /* MCLK Notch */
        ret = regmap_update_bits(codec_data->client1, 12, 0x1f,
                (1U << val) - 1);
        break;
    case 14: /* Remap Output (Quantizer & Differential) */
        ret = regmap_update_bits(codec_data->client1, 14, 0x08,
                0x08 * !(val % 2));
        if (!ret)
            ret = regmap_update_bits(codec_data->client1, 15, 0xff,
                    0x55 * ((val + 1) / 2));
        break;
    }

    if (!ret)
        return 0;
    else {
        dev_warn(codec->dev, "unable to configure Codec via I2C; e=%d\n", ret);
        return -EIO;
    }
}

static struct snd_soc_codec_driver sabre32_codec_socdrv = {
    .probe = sabre32_codec_probe,
    .remove = sabre32_codec_remove,
    .read = sabre32_codec_read,
    .write = sabre32_codec_write,
    .controls = sabre32_codec_controls,
    .num_controls = ARRAY_SIZE(sabre32_codec_controls),
};

static int sabre32_codec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
    struct snd_soc_codec *codec = dai->codec;
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    int ret = 0;

    if (codec_data->client1 == NULL)
        return 0;

    /* Mute the DAC before adjusting the parameters. */
    (void)regmap_update_bits(codec_data->client1, 10, 0x01, 0x01);

    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
    case SND_SOC_DAIFMT_DIT:
        ret = regmap_update_bits(codec_data->client1, 8, 0x80, 0x80);
        if (!ret)
            ret = regmap_update_bits(codec_data->client1, 17, 0x08, 0x08);
        break;
    default:
        ret = regmap_update_bits(codec_data->client1, 8, 0x80, 0x00);
        if (!ret)
            ret = regmap_update_bits(codec_data->client1, 17, 0x08, 0x00);
        break;
    }

    if (ret != 0)
        return ret;

    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
    case SND_SOC_DAIFMT_DIT:
    case SND_SOC_DAIFMT_I2S:
        ret = regmap_update_bits(codec_data->client1, 10, 0x30, 0x00);
        break;
    case SND_SOC_DAIFMT_LEFT_J:
        ret = regmap_update_bits(codec_data->client1, 10, 0x30, 0x10);
        break;
    case SND_SOC_DAIFMT_RIGHT_J:
        ret = regmap_update_bits(codec_data->client1, 10, 0x30, 0x20);
        break;
    default:
        dev_warn(codec->dev, "unsupported DAI fmt %d", fmt);
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int sabre32_codec_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
    struct snd_soc_codec *codec = dai->codec;
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    int ret = 0;

    if (codec_data->client1 == NULL)
        return 0;

    if (stream != SNDRV_PCM_STREAM_PLAYBACK)
        return ret;

    codec_data->stream_muted = mute;

    if (mute) {
        /* Reconfigure the DAC for a SPDIF playback from an external device. */

        /* Mute the DAC first. */
        (void)regmap_update_bits(codec_data->client1, 10, 0x01, 0x01);

        /* Force SPDIF input. */
        (void)regmap_update_bits(codec_data->client1, 8, 0x80, 0x80);
        /* Re-enable SPDIF autodetect. */
        (void)regmap_update_bits(codec_data->client1, 17, 0x08, 0x08);
        /* TODO: other parameters, e.g. DPLL */

        /* Unmute the DAC after reconfiguration. */
        if (codec_data->mute_mode == 0)
            ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x00);
    } else if (!codec_data->force_mute) {
        /* Unmute the DAC if it is not muted by user. */
        ret = regmap_update_bits(codec_data->client1, 10, 0x01, 0x00);
    }

    return ret;
}

static int sabre32_codec_hw_params(struct snd_pcm_substream *substream,
        struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
    struct snd_soc_codec *codec = dai->codec;
    struct sabre32_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    unsigned int rate = params_rate(params);
    int clock_48k;
    int ret;

    if (codec_data->client1 == NULL)
        return 0;

    clock_48k = (rate % 12000 == 0);
    if (clock_48k != codec_data->last_clock_48k) {
        codec_data->last_clock_48k = clock_48k;
        /* Force DPLL lock reset. */
        (void)regmap_update_bits(codec_data->client1, 17, 0x20, 0x20);
        ret = regmap_update_bits(codec_data->client1, 17, 0x20, 0x00);
        if (ret)
            return ret;
    }

    switch (params_format(params)) {
    case SNDRV_PCM_FORMAT_S16_LE:
        ret = regmap_update_bits(codec_data->client1, 10, 0xc0, 0x80);
        break;

    case SNDRV_PCM_FORMAT_S24_3LE:
    case SNDRV_PCM_FORMAT_S24_LE:
        ret = regmap_update_bits(codec_data->client1, 10, 0xc0, 0x00);
        break;

    case SNDRV_PCM_FORMAT_S32_LE:
    case SNDRV_PCM_FORMAT_DSD_U8:
    case SNDRV_PCM_FORMAT_DSD_U16_LE:
    case SNDRV_PCM_FORMAT_DSD_U32_LE:
        ret = regmap_update_bits(codec_data->client1, 10, 0xc0, 0xc0);
        break;

    default:
        dev_warn(codec->dev, "unsupported PCM format %d", params_format(params));
        ret = -EINVAL;
    }

    return ret;
}

static const struct snd_soc_dai_ops sabre32_codec_dai_ops = {
    .set_fmt = sabre32_codec_set_fmt,
    .mute_stream = sabre32_codec_mute_stream,
    .hw_params = sabre32_codec_hw_params,
};

static int asoc_sabre32_codec_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    struct device_node *adapter_node;
    struct sabre32_codec_data *codec_data;
    int ret;

    if (!pdev->dev.of_node) {
        dev_err(&pdev->dev, "No device tree data\n");
        return -ENODEV;
    }

    codec_data = devm_kzalloc(&pdev->dev, sizeof(struct sabre32_codec_data),
            GFP_KERNEL);

    adapter_node = of_parse_phandle(node, "i2c-bus", 0);
    if (adapter_node) {
        codec_data->i2c_adapter = of_get_i2c_adapter_by_node(adapter_node);
        if (codec_data->i2c_adapter == NULL) {
            dev_err(&pdev->dev, "failed to parse i2c-bus\n");
            return -EPROBE_DEFER;
        }
    }

    dev_set_drvdata(&pdev->dev, codec_data);

    ret = snd_soc_register_codec(&pdev->dev,
            &sabre32_codec_socdrv, &sabre32_codec_dai, 1);

    if (ret != 0) {
        i2c_put_adapter(codec_data->i2c_adapter);
    }

    return ret;
}

static int asoc_sabre32_codec_remove(struct platform_device *pdev)
{
    struct sabre32_codec_data *codec_data = platform_get_drvdata(pdev);

    snd_soc_unregister_codec(&pdev->dev);

    i2c_put_adapter(codec_data->i2c_adapter);

    return 0;
}

static void asoc_sabre32_codec_shutdown(struct platform_device *pdev)
{
}

#ifdef CONFIG_PM_SLEEP
static int asoc_sabre32_codec_suspend(struct platform_device *pdev, pm_message_t state)
{
    return 0;
}

static int asoc_sabre32_codec_resume(struct platform_device *pdev)
{
    return 0;
}
#else
#define asoc_sabre32_codec_suspend NULL
#define asoc_sabre32_codec_resume NULL
#endif

#if defined(CONFIG_OF)
static const struct of_device_id asoc_sabre32_codec_dt_ids[] = {
    { .compatible = "sabre32-audio-codec" },
    { },
};

MODULE_DEVICE_TABLE(of, asoc_sabre32_codec_dt_ids);
#endif

static struct platform_driver asoc_sabre32_codec_driver = {
    .probe = asoc_sabre32_codec_probe,
    .remove = asoc_sabre32_codec_remove,
    .shutdown = asoc_sabre32_codec_shutdown,
    .suspend = asoc_sabre32_codec_suspend,
    .resume = asoc_sabre32_codec_resume,
    .driver = {
        .name = "asoc-sabre32-codec",
        .of_match_table = of_match_ptr(asoc_sabre32_codec_dt_ids),
    },
};

module_platform_driver(asoc_sabre32_codec_driver);

MODULE_AUTHOR("Miroslav Rudisin");
MODULE_DESCRIPTION("ESS Technology Sabre32 Audio DAC");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:asoc-sabre32-codec");
