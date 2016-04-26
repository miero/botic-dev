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

struct botic_codec_data {
    struct i2c_adapter *i2c_adapter;
    struct i2c_client *i2c_client1;
    struct i2c_client *i2c_client2;
    struct regmap *client1;
    struct regmap *client2;
};

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
    SOC_DOUBLE("Master Playback Volume", 0, 0, 0, 31, 0),
    SOC_SINGLE("Master Playback Switch", 1, 0, 1, 1),
};

static const struct regmap_config empty_regmap_config;

static int botic_codec_probe(struct snd_soc_codec *codec)
{
    struct botic_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    struct regmap_config config;

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

static int botic_codec_remove(struct snd_soc_codec *codec)
{
    struct botic_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);

    if (codec_data->i2c_client1 != NULL)
        i2c_unregister_device(codec_data->i2c_client1);

    if (codec_data->i2c_client2 != NULL)
        i2c_unregister_device(codec_data->i2c_client2);

    return 0;
}

static unsigned int botic_codec_read(struct snd_soc_codec *codec,
        unsigned int reg)
{
    struct botic_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    unsigned int v = 0;
    unsigned int t;
    int r = 0;

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
        /* convert 0--0x7fffffff to 0-30 */
        t = v + 1;
        v = 0;
        while (t > 0) {
            v++;
            t &= ~1U;
            t >>= 1;
        }
        v--;
        break;
    }

    if (!r)
        return v;
    else
        return 0;
}

static int botic_codec_write(struct snd_soc_codec *codec,
        unsigned int reg, unsigned int val)
{
    struct botic_codec_data *codec_data = snd_soc_codec_get_drvdata(codec);
    unsigned int t;
    int r = 0;

    switch(reg) {
    case 0: /* Master Volume */
        t = (1U << val) - 1;
        r = regmap_write(codec_data->client1, 20, t & 0xff);
        t >>= 8;
        r |= regmap_write(codec_data->client1, 21, t & 0xff);
        t >>= 8;
        r |= regmap_write(codec_data->client1, 22, t & 0xff);
        t >>= 8;
        r |= regmap_write(codec_data->client1, 23, t);
        break;
    }

    if (!r)
        return 0;
    else
        return -EIO;
}

static struct snd_soc_codec_driver botic_codec_socdrv = {
    .probe = botic_codec_probe,
    .remove = botic_codec_remove,
    .read = botic_codec_read,
    .write = botic_codec_write,
    .controls = botic_codec_controls,
    .num_controls = ARRAY_SIZE(botic_codec_controls),
};

static int asoc_botic_codec_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    struct device_node *adapter_node;
    struct botic_codec_data *codec_data;
    int ret;

    if (!pdev->dev.of_node) {
        dev_err(&pdev->dev, "No device tree data\n");
        return -ENODEV;
    }

    codec_data = devm_kzalloc(&pdev->dev, sizeof(struct botic_codec_data),
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
            &botic_codec_socdrv, &botic_dac_dai, 1);

    if (ret != 0) {
        i2c_put_adapter(codec_data->i2c_adapter);
    }

    return ret;
}

static int asoc_botic_codec_remove(struct platform_device *pdev)
{
    struct botic_codec_data *codec_data = platform_get_drvdata(pdev);

    snd_soc_unregister_codec(&pdev->dev);

    i2c_put_adapter(codec_data->i2c_adapter);

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
