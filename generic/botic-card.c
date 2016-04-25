/*
 * ASoC simple sound card support
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

#define ENABLE_EXT_MASTERCLK_44K1 1
#define ENABLE_EXT_MASTERCLK_48K 2
#define ENABLE_EXT_MASTERCLK_SWITCH_INVERT 4
#define ENABLE_EXT_MASTERCLK_SINGLE 8

#define ENABLE_DSD_FORMAT_SWITCH 1
#define ENABLE_DSD_FORMAT_SWITCH_INVERT 2

static int gpio_int_masterclk_enable = -1;
static int gpio_ext_masterclk_switch = -1;
static int gpio_dsd_format_switch = -1;
static int gpio_card_power_switch = -1;

static char *pinconfig = "default";
/* I (I2S only), D (DSD only), M (I2S and DSD), S (SPDIF), R (Record/Capture) */
static char *serconfig = "MMMM";

static int ext_masterclk = ENABLE_EXT_MASTERCLK_44K1 | ENABLE_EXT_MASTERCLK_48K;
static int dsd_format_switch = ENABLE_DSD_FORMAT_SWITCH;
static int dai_format = SND_SOC_DAIFMT_CBS_CFS | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;

static int clk_44k1 = 22579200;
static int clk_48k = 24576000;
static int blr_ratio = 64;

static int is_dsd(snd_pcm_format_t format)
{
    switch (format) {
        case SNDRV_PCM_FORMAT_DSD_U8:
        case SNDRV_PCM_FORMAT_DSD_U16_LE:
        case SNDRV_PCM_FORMAT_DSD_U16_BE:
        case SNDRV_PCM_FORMAT_DSD_U32_LE:
        case SNDRV_PCM_FORMAT_DSD_U32_BE:
            return 1;
            break;

        default:
            return 0;
            break;
    }
}

struct botic_ser_setup {
    int dai_fmt;
    int nch_tx;
    int tx_slots[4];
    int nch_rx;
    int rx_slots[4];
};

static int botic_setup_serializers(struct snd_soc_dai *cpu_dai,
        snd_pcm_format_t format, struct botic_ser_setup *ser_setup)
{
    int n_i2s = 0;
    int n_dsd = 0;
    int n_spdif = 0;
    int i;
    int ret;

    /* clear serializer setup */
    memset(ser_setup, 0, sizeof(*ser_setup));

    for (i = 0; i < 4; i++) {
        switch (serconfig[i]) {
            case 'I':
                if (is_dsd(format)) continue;
                n_i2s++;
                break;
            case 'D':
                if (!is_dsd(format)) continue;
                n_dsd++;
                break;
            case 'M':
                n_i2s++;
                n_dsd++;
                break;
            case 'S':
                n_spdif++;
                break;
            case 'R':
                ser_setup->rx_slots[ser_setup->nch_rx++] = i;
                continue;
            case '-':
                continue;
            default:
                printk(KERN_ERR "botic-card: invalid character '%c'"
                       " in serconfig\n", serconfig[i]);
                return -EINVAL;
                break;
        }
        ser_setup->tx_slots[ser_setup->nch_tx++] = i;
    }

    if (n_spdif > 0 && (n_i2s + n_dsd) != 0) {
        printk(KERN_ERR "botic-card: SPDIF cannot be combined with other formats");
        return -EINVAL;
    }

    if (n_dsd == 0 && is_dsd(format)) {
        printk(KERN_ERR "botic-card: no pins for DSD playback");
        return -EINVAL;
    }

    ret = snd_soc_dai_set_channel_map(cpu_dai, ser_setup->nch_tx,
            ser_setup->tx_slots, ser_setup->nch_rx, ser_setup->rx_slots);
    if (ret < 0)
        return ret;

    ser_setup->dai_fmt = dai_format;
    if (n_spdif > 0) {
#if 1
        ser_setup->dai_fmt = SND_SOC_DAIFMT_DIT;
#else
        printk(KERN_ERR "botic-card: DIT is not supported dai format");
        return -EINVAL;
#endif
    }

    return 0;
}

static int botic_hw_params(struct snd_pcm_substream *substream,
             struct snd_pcm_hw_params *params)
{
    struct snd_soc_pcm_runtime *rtd = substream->private_data;
    struct snd_soc_dai *codec_dai = rtd->codec_dai;
    struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
    unsigned sysclk, bclk, divisor;
    struct botic_ser_setup ser_setup;
    int ret;

    snd_pcm_format_t format = params_format(params);
    unsigned int rate = params_rate(params);

    /* setup CPU serializers */
    ret = botic_setup_serializers(cpu_dai, format, &ser_setup);
    if (ret < 0)
        return ret;

    /* set codec DAI configuration */
    ret = snd_soc_dai_set_fmt(codec_dai, ser_setup.dai_fmt);
    if ((ret < 0) && (ret != -ENOTSUPP))
        return ret;

    /* set cpu DAI configuration */
    ret = snd_soc_dai_set_fmt(cpu_dai, ser_setup.dai_fmt);
    if (ret < 0)
        return ret;

    /* select correct clock for requested sample rate */
    if ((clk_44k1 != 0) && (clk_44k1 % rate == 0)) {
        sysclk = clk_44k1;
        if (gpio_int_masterclk_enable >= 0) {
            gpio_set_value(gpio_int_masterclk_enable, 0);
        }
        if (gpio_ext_masterclk_switch >= 0) {
            /* set level to LOW for 44k1 sampling rates */
            gpio_set_value(gpio_ext_masterclk_switch,
                    !!(ext_masterclk & ENABLE_EXT_MASTERCLK_SWITCH_INVERT));
        }
    } else if (clk_48k % rate == 0) {
        sysclk = clk_48k;
        if (gpio_ext_masterclk_switch >= 0) {
            /* set level to HIGH for 48k sampling rates */
            gpio_set_value(gpio_ext_masterclk_switch,
                    !(ext_masterclk & ENABLE_EXT_MASTERCLK_SWITCH_INVERT));
        }
        if ((gpio_int_masterclk_enable >= 0) &&
                !(ext_masterclk & ENABLE_EXT_MASTERCLK_48K)) {
            /* 44k1 clock is disabled now, we can enable onboard clock */
            gpio_set_value(gpio_int_masterclk_enable, 1);
        }
    } else if ((dai_format & SND_SOC_DAIFMT_CBM_CFM) == 0) {
        printk("unsupported rate %d\n", rate);
        return -EINVAL;
    } else {
        printk("slave rate %d\n", rate);
        sysclk = 0;
    }

    /* setup DSD format switch */
    if (!(dsd_format_switch & ENABLE_DSD_FORMAT_SWITCH) ||
        (gpio_dsd_format_switch < 0)) {
        /* DSD format switch is disabled or not available */
    } else if (is_dsd(params_format(params))) {
        /* DSD format switch is enabled, set level to HIGH for DSD playback */
        gpio_set_value(gpio_dsd_format_switch,
                !(dsd_format_switch & ENABLE_DSD_FORMAT_SWITCH_INVERT));
    } else {
        /* DSD format switch is enabled, set level to LOW for PCM playback */
        gpio_set_value(gpio_dsd_format_switch,
                !!(dsd_format_switch & ENABLE_DSD_FORMAT_SWITCH_INVERT));
    }

    /* set the codec system clock */
    ret = snd_soc_dai_set_sysclk(codec_dai, 0, sysclk, SND_SOC_CLOCK_IN);
    if ((ret < 0) && (ret != -ENOTSUPP))
        return ret;

    /* use the external clock */
    ret = snd_soc_dai_set_sysclk(cpu_dai, 0, sysclk, SND_SOC_CLOCK_IN);
    if (ret < 0) {
        printk(KERN_WARNING "botic-card: unable to set clock to CPU; ret=%d", ret);
        return ret;
    }

    ret = snd_soc_dai_set_clkdiv(cpu_dai, 0, 1);
    if (ret < 0) {
        printk(KERN_WARNING "botic-card: unsupported set_clkdiv0");
        return ret;
    }

    if ((dai_format & SND_SOC_DAIFMT_CBM_CFM) != 0) {
        printk("slave mode...\n");
        return 0;
    }

    switch (params_format(params)) {
        case SNDRV_PCM_FORMAT_DSD_U8:
            /* Clock rate for DSD matches bitrate */
            ret = snd_soc_dai_set_clkdiv(cpu_dai, 2, 0);
            bclk = 8 * rate;
            break;

        case SNDRV_PCM_FORMAT_DSD_U16_LE:
        case SNDRV_PCM_FORMAT_DSD_U16_BE:
            /* Clock rate for DSD matches bitrate */
            ret = snd_soc_dai_set_clkdiv(cpu_dai, 2, 0);
            bclk = 16 * rate;
            break;

        case SNDRV_PCM_FORMAT_DSD_U32_LE:
        case SNDRV_PCM_FORMAT_DSD_U32_BE:
            /* Clock rate for DSD matches bitrate */
            ret = snd_soc_dai_set_clkdiv(cpu_dai, 2, 0);
            bclk = 32 * rate;
            break;

        default:
            /* PCM */
            ret = snd_soc_dai_set_clkdiv(cpu_dai, 2, blr_ratio);
            if (blr_ratio != 0) {
                bclk = blr_ratio * rate;
            } else {
                bclk = snd_soc_params_to_bclk(params);
            }
            break;
    }
    if (ret < 0) {
        printk(KERN_WARNING "botic-card: unsupported BCLK/LRCLK ratio");
        return ret;
    }

    divisor = (sysclk + (bclk / 2)) / bclk;
    ret = snd_soc_dai_set_clkdiv(cpu_dai, 1, divisor);
    if (ret < 0) {
        printk(KERN_WARNING "botic-card: unsupported set_clkdiv1");
        return ret;
    }

    /* Insert delay needed for enabled clocks. */
    udelay(50);

    return 0;
}

static struct snd_soc_ops botic_ops = {
    .hw_params = botic_hw_params,
};

/* digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link botic_dai = {
    .name = "ExtDAC",
    .stream_name = "external",
    .ops = &botic_ops,
};

static struct snd_soc_card botic_card = {
    .name = "Botic",
    .owner = THIS_MODULE,
    .dai_link = &botic_dai,
    .num_links = 1,
};

static int get_optional_gpio(int *optional_gpio, struct platform_device *pdev,
        const char *gpio_name, unsigned long gpio_flags)
{
    struct device_node *np = pdev->dev.of_node;
    struct property *p;
    int lp;
    int ret;
    int gpio;

    p = of_find_property(np, gpio_name, &lp);
    if (!p) {
        dev_err(&pdev->dev, "entry for %s does not exist\n", gpio_name);
        return -ENOENT;
    }

    if (lp == 0) {
        *optional_gpio = -1;
        return 0;
    }

    ret = of_get_named_gpio(np, gpio_name, 0);
    if (ret < 0) {
        dev_err(&pdev->dev, "failed to read GPIO for %s\n", gpio_name);
        return ret;
    }

    gpio = ret;
    ret = gpio_request_one(gpio, gpio_flags, gpio_name);
    if (ret < 0) {
        dev_err(&pdev->dev, "failed to claim GPIO for %s\n", gpio_name);
        return ret;
    }

    *optional_gpio = gpio;

    return 0;
}

static int asoc_botic_card_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct pinctrl *pctl;
    struct pinctrl_state *pctl_state;
    int ret;

    /* load selected pinconfig */
    pctl = devm_pinctrl_get(&pdev->dev);
    if (IS_ERR(pctl)) {
        ret = PTR_ERR(pctl);
        goto asoc_botic_card_probe_error;
    }
    pctl_state = pinctrl_lookup_state(pctl, pinconfig);
    if (IS_ERR(pctl_state)) {
        dev_err(&pdev->dev, "unable to lookup pinconfig %s\n", pinconfig);
        ret = PTR_ERR(pctl_state);
        goto asoc_botic_card_probe_error;
    }
    ret = pinctrl_select_state(pctl, pctl_state);
    if (ret < 0) {
        dev_err(&pdev->dev, "unable to set pinconfig %s\n", pinconfig);
        goto asoc_botic_card_probe_error;
    }
    dev_info(&pdev->dev, "using '%s' pinconfig\n", pinconfig);

    /*
     * TODO: Move GPIO handling out of the probe, if probe gets
     * deferred, the gpio will have been claimed on previous
     * probe and will fail on the second and susequent probes
     */

    /* request GPIO to control internal 24.576MHz oscillator */
    ret = get_optional_gpio(&gpio_int_masterclk_enable, pdev,
            "int-masterclk-enable", GPIOF_OUT_INIT_LOW);
    if (ret < 0) {
        goto asoc_botic_card_probe_error;
    }

    /* request GPIO to card power switch */
    ret = get_optional_gpio(&gpio_card_power_switch, pdev,
            "card-power-switch", GPIOF_OUT_INIT_LOW);
    if (ret < 0) {
        goto asoc_botic_card_probe_error;
    }

    if (ext_masterclk & (ENABLE_EXT_MASTERCLK_44K1 | ENABLE_EXT_MASTERCLK_48K)) {
        /* request GPIO to switch between external 22.5792MHz and 24.576MHz oscillators */
        ret = get_optional_gpio(&gpio_ext_masterclk_switch, pdev,
                "ext-masterclk-switch", GPIOF_OUT_INIT_HIGH);
        if (ret < 0) {
            goto asoc_botic_card_probe_error;
        }
        switch (ext_masterclk & (ENABLE_EXT_MASTERCLK_44K1 | ENABLE_EXT_MASTERCLK_48K)) {
            case ENABLE_EXT_MASTERCLK_44K1:
                if (ext_masterclk & ENABLE_EXT_MASTERCLK_SINGLE) {
                    clk_48k = 0;
                } else {
                    /* fallback to internal oscillator */
                }
                break;
            case ENABLE_EXT_MASTERCLK_48K:
                clk_44k1 = 0;
                break;
            case ENABLE_EXT_MASTERCLK_44K1 | ENABLE_EXT_MASTERCLK_48K:
                /* use both oscillators */
                break;
        }
    } else {
        ext_masterclk = 0;
        gpio_ext_masterclk_switch = -1;
        /* TODO: which clock to disable */
        clk_44k1 = 0;
    }

    if (dsd_format_switch & ENABLE_DSD_FORMAT_SWITCH) {
        ret = get_optional_gpio(&gpio_dsd_format_switch, pdev,
                "dsd-format-switch", GPIOF_OUT_INIT_HIGH);
        if (ret < 0) {
            goto asoc_botic_card_probe_error;
        }
    } else {
        dsd_format_switch = 0;
        gpio_dsd_format_switch = -1;
    }

    botic_dai.codec_of_node = of_parse_phandle(np, "audio-codec", 0);
    if (botic_dai.codec_of_node) {
        ret = of_property_read_string_index(np, "audio-codec-dai", 0,
                &botic_dai.codec_dai_name);
        if (ret < 0) {
            goto asoc_botic_card_probe_error;
        }
    } else {
        ret = -ENOENT;
        goto asoc_botic_card_probe_error;
    }

    botic_dai.cpu_of_node = of_parse_phandle(np, "audio-port", 0);
    if (!botic_dai.cpu_of_node) {
        ret = -ENOENT;
        goto asoc_botic_card_probe_error;
    }

    /* TODO */
    botic_dai.platform_of_node = botic_dai.cpu_of_node;

    botic_card.dev = &pdev->dev;

    /* register card */
    ret = snd_soc_register_card(&botic_card);
    if (ret) {
        dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
        goto asoc_botic_card_probe_error;
    }

#if 0
    /* a hack for removing unsupported rates from the codec */
    if (clk_44k1 == 0) {
        botic_card.rtd[0].codec_dai->driver->playback.rates &=
            ~(SNDRV_PCM_RATE_5512 | SNDRV_PCM_RATE_11025 |
              SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_44100 |
              SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_176400 |
              SNDRV_PCM_RATE_352800 | SNDRV_PCM_RATE_705600 |
              0);
    }
    if (clk_48k == 0) {
        botic_card.rtd[0].codec_dai->driver->playback.rates &=
            ~(SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
              SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |
              SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000 |
              SNDRV_PCM_RATE_384000 | SNDRV_PCM_RATE_768000 |
              0);
    }
#endif

    dev_info(&pdev->dev, "48k %s, 44k1 %s, %s format switch\n",
            (ext_masterclk & ENABLE_EXT_MASTERCLK_48K) ? "ext" : (
                clk_48k != 0 ? "int" : "none"),
            (ext_masterclk & ENABLE_EXT_MASTERCLK_44K1) ? "ext" : "none",
            (dsd_format_switch & ENABLE_DSD_FORMAT_SWITCH) ? "use" : "do not use");

    if (gpio_card_power_switch >= 0) {
        /* switch the card on */
        gpio_set_value(gpio_card_power_switch, 1);
    }

asoc_botic_card_probe_error:
    if (ret != 0) {
        if (gpio_int_masterclk_enable >= 0) {
            gpio_free(gpio_int_masterclk_enable);
        }
        if (gpio_card_power_switch >= 0) {
            gpio_free(gpio_card_power_switch);
        }
        if (gpio_ext_masterclk_switch >= 0) {
            gpio_free(gpio_ext_masterclk_switch);
        }
        if (gpio_dsd_format_switch >= 0) {
            gpio_free(gpio_dsd_format_switch);
        }
    }

    return ret;
}

static int asoc_botic_card_remove(struct platform_device *pdev)
{
    struct snd_soc_card *card = platform_get_drvdata(pdev);

    snd_soc_unregister_card(card);

    if (gpio_int_masterclk_enable >= 0) {
        /* switch the oscillator off first */
        gpio_set_value(gpio_int_masterclk_enable, 0);
        gpio_free(gpio_int_masterclk_enable);
    }
    if (gpio_card_power_switch >= 0) {
        /* switch the card off first */
        gpio_set_value(gpio_card_power_switch, 0);
        gpio_free(gpio_card_power_switch);
    }
    if (gpio_ext_masterclk_switch >= 0) {
        gpio_free(gpio_ext_masterclk_switch);
    }
    if (gpio_dsd_format_switch >= 0) {
        gpio_free(gpio_dsd_format_switch);
    }

    return 0;
}

static void asoc_botic_card_shutdown(struct platform_device *pdev)
{
    if (gpio_card_power_switch >= 0) {
        /* switch the card off first */
        gpio_set_value(gpio_card_power_switch, 0);
        /* sleep until card will be powered down safely */
        mdelay(1000);
    }
}

#ifdef CONFIG_PM_SLEEP
static int asoc_botic_card_suspend(struct platform_device *pdev, pm_message_t state)
{
    if (gpio_card_power_switch >= 0) {
        /* switch the card off before going suspend */
        gpio_set_value(gpio_card_power_switch, 0);
    }

    return 0;
}

static int asoc_botic_card_resume(struct platform_device *pdev)
{
    if (gpio_card_power_switch >= 0) {
        /* switch the card on after resuming from suspend */
        gpio_set_value(gpio_card_power_switch, 1);
    }

    return 0;
}
#else
#define asoc_botic_card_suspend NULL
#define asoc_botic_card_resume NULL
#endif

static const struct of_device_id asoc_botic_card_dt_ids[] = {
    { .compatible = "botic-audio-card" },
    { },
};

MODULE_DEVICE_TABLE(of, asoc_botic_card_dt_ids);

static struct platform_driver asoc_botic_card_driver = {
    .probe = asoc_botic_card_probe,
    .remove = asoc_botic_card_remove,
    .shutdown = asoc_botic_card_shutdown,
    .suspend = asoc_botic_card_suspend,
    .resume = asoc_botic_card_resume,
    .driver = {
        .name = "asoc-botic-card",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(asoc_botic_card_dt_ids),
    },
};

static int __init botic_card_init(void)
{
    platform_driver_register(&asoc_botic_card_driver);

    return 0;
}

static void __exit botic_card_exit(void)
{
    platform_driver_unregister(&asoc_botic_card_driver);
}

module_init(botic_card_init);
module_exit(botic_card_exit);

module_param(pinconfig, charp, 0444);
MODULE_PARM_DESC(pinconfig, "selected pin configuration");

module_param(ext_masterclk, int, 0444);
MODULE_PARM_DESC(ext_masterclk, "available external masterclocks");

module_param(dsd_format_switch, int, 0444);
MODULE_PARM_DESC(dsd_format_switch, "mode of dsd format switch");

module_param(serconfig, charp, 0644);
MODULE_PARM_DESC(serconfig, "serializer configuration");

module_param(dai_format, int, 0644);
MODULE_PARM_DESC(dai_format, "output format and clock sources configuration");

module_param(clk_44k1, int, 0644);
MODULE_PARM_DESC(clk_44k1, "frequency of crystal for 44k1 modes");

module_param(clk_48k, int, 0644);
MODULE_PARM_DESC(clk_48k, "frequency of crystal for 48k modes");

module_param(blr_ratio, int, 0644);
MODULE_PARM_DESC(blr_ratio, "force BCLK/LRCLK ratio");

MODULE_AUTHOR("Miroslav Rudisin");
MODULE_DESCRIPTION("ASoC Botic sound card");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:asoc-botic-card");
