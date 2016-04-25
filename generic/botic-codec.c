/*
 * ASoC simple sound codec support
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

#define BOTIC_CODEC_NAME "botic-codec"
#define BOTIC_CODEC_DAI_NAME "dac-hifi"

static struct platform_device *botic_codec_platdev;

#define BOTIC_RATES (\
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

#define BOTIC_FORMATS (\
            SNDRV_PCM_FMTBIT_S16_LE | \
            SNDRV_PCM_FMTBIT_S16_BE | \
            SNDRV_PCM_FMTBIT_S24_3LE | \
            SNDRV_PCM_FMTBIT_S24_3BE | \
            SNDRV_PCM_FMTBIT_S24_LE | \
            SNDRV_PCM_FMTBIT_S24_BE | \
            SNDRV_PCM_FMTBIT_S32_LE | \
            SNDRV_PCM_FMTBIT_S32_BE | \
            SNDRV_PCM_FMTBIT_DSD_U8 | \
            SNDRV_PCM_FMTBIT_DSD_U16_LE | \
            SNDRV_PCM_FMTBIT_DSD_U16_BE | \
            SNDRV_PCM_FMTBIT_DSD_U32_LE | \
            SNDRV_PCM_FMTBIT_DSD_U32_BE | \
            0)

static struct snd_soc_dai_driver botic_dac_dai = {
    .name = BOTIC_CODEC_DAI_NAME,
    .playback = {
        .channels_min = 2,
        .channels_max = 8,
        .rate_min = 11025,
        .rate_max = 768000,
        .rates = BOTIC_RATES,
        .formats = BOTIC_FORMATS,
    },
    .capture = {
        .channels_min = 2,
        .channels_max = 8,
        .rate_min = 11025,
        .rate_max = 768000,
        .rates = BOTIC_RATES,
        .formats = BOTIC_FORMATS,
    },
};

static const struct snd_kcontrol_new botic_codec_controls[] = {
    SOC_DOUBLE("Master Playback Volume", 0, 0, 0, 31, 1),
    SOC_SINGLE("Master Playback Switch", 1, 0, 1, 1),
};

static int botic_codec_probe(struct snd_soc_codec *codec)
{
    return 0;
}

static unsigned int botic_codec_read(struct snd_soc_codec *codec,
        unsigned int reg)
{
    /* TODO */
    return 0;
}

static int botic_codec_write(struct snd_soc_codec *codec,
        unsigned int reg, unsigned int val)
{
    /* TODO */
    return 0;
}

static struct snd_soc_codec_driver botic_codec_socdrv = {
    .probe = botic_codec_probe,
    .read = botic_codec_read,
    .write = botic_codec_write,
    .controls = botic_codec_controls,
    .num_controls = ARRAY_SIZE(botic_codec_controls),
};

static int asoc_botic_codec_probe(struct platform_device *pdev)
{
    return snd_soc_register_codec(&pdev->dev,
            &botic_codec_socdrv, &botic_dac_dai, 1);
}

static int asoc_botic_codec_remove(struct platform_device *pdev)
{
    snd_soc_unregister_codec(&botic_codec_platdev->dev);
    return 0;
}

static void asoc_botic_codec_shutdown(struct platform_device *pdev)
{
}

#ifdef CONFIG_PM_SLEEP
static int asoc_botic_codec_suspend(struct platform_device *pdev, pm_message_t state)
{
    return 0;
}

static int asoc_botic_codec_resume(struct platform_device *pdev)
{
    return 0;
}
#else
#define asoc_botic_codec_suspend NULL
#define asoc_botic_codec_resume NULL
#endif

#if defined(CONFIG_OF)
static const struct of_device_id asoc_botic_codec_dt_ids[] = {
    { .compatible = "botic-audio-codec" },
    { },
};

MODULE_DEVICE_TABLE(of, asoc_botic_codec_dt_ids);
#endif

static struct platform_driver asoc_botic_codec_driver = {
    .probe = asoc_botic_codec_probe,
    .remove = asoc_botic_codec_remove,
    .shutdown = asoc_botic_codec_shutdown,
    .suspend = asoc_botic_codec_suspend,
    .resume = asoc_botic_codec_resume,
    .driver = {
        .name = "asoc-botic-codec",
        .of_match_table = of_match_ptr(asoc_botic_codec_dt_ids),
    },
};

module_platform_driver(asoc_botic_codec_driver);

MODULE_AUTHOR("Miroslav Rudisin");
MODULE_DESCRIPTION("ASoC Botic sound codec");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:asoc-botic-codec");
