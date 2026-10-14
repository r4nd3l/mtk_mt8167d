// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal ASoC machine driver for MT8167 boards that use only the SoC's
 * internal codec (headphone, microphones) and the MT6392 PMIC's speaker
 * amplifier, e.g. the Lenovo Tab E7 (TB-7104F).
 *
 * ponytail: one playback and one capture front end on the INT ADDA back end;
 * no jack detection (ACCDET), no I2S/HDMI/BT links. Add them when needed.
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

SND_SOC_DAILINK_DEFS(playback,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture,
	DAILINK_COMP_ARRAY(COMP_CPU("VUL")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(adda,
	DAILINK_COMP_ARRAY(COMP_CPU("INT ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "mt8167-codec-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt8167_mt6392_links[] = {
	{
		.name = "Playback",
		.stream_name = "Playback",
		.trigger = { SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST },
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback),
	},
	{
		.name = "Capture",
		.stream_name = "Capture",
		.trigger = { SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST },
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture),
	},
	{
		.name = "INT ADDA",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(adda),
	},
};

/* external speaker amplifier, vendor "extamp" pin (GPIO2 on the TB-7104F) */
static struct gpio_desc *ext_amp;

static int mt8167_mt6392_ext_amp_event(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *k, int event)
{
	gpiod_set_value_cansleep(ext_amp, SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static const struct snd_soc_dapm_widget mt8167_mt6392_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", mt8167_mt6392_ext_amp_event),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route mt8167_mt6392_routes[] = {
	{ "Headphone", NULL, "AU_HPL" },
	{ "Headphone", NULL, "AU_HPR" },
	{ "MT6392 AIF RX", NULL, "AU_LOL" },
	/* the external speaker amp is fed from the headphone output, not line out */
	{ "Speaker", NULL, "AU_HPL" },
	{ "Speaker", NULL, "AU_HPR" },
	{ "AU_VIN0", NULL, "Internal Mic" },
	{ "AU_VIN1", NULL, "Headset Mic" },
	/* ADDA clocks */
	{ "AIF TX", NULL, "audio" },
	{ "AIF TX", NULL, "aud_adc" },
	{ "AIF RX", NULL, "audio" },
	{ "AIF RX", NULL, "aud_dac" },
};

static const struct snd_kcontrol_new mt8167_mt6392_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
};

static struct snd_soc_aux_dev mt8167_mt6392_aux = {
	.dlc = COMP_AUX(NULL),
};

static struct snd_soc_card mt8167_mt6392_card = {
	.name = "mt8167-mt6392",
	.owner = THIS_MODULE,
	.dai_link = mt8167_mt6392_links,
	.num_links = ARRAY_SIZE(mt8167_mt6392_links),
	.aux_dev = &mt8167_mt6392_aux,
	.num_aux_devs = 1,
	.dapm_widgets = mt8167_mt6392_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt8167_mt6392_widgets),
	.dapm_routes = mt8167_mt6392_routes,
	.num_dapm_routes = ARRAY_SIZE(mt8167_mt6392_routes),
	.controls = mt8167_mt6392_controls,
	.num_controls = ARRAY_SIZE(mt8167_mt6392_controls),
	.fully_routed = false,
};

static int mt8167_mt6392_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt8167_mt6392_card;
	struct device *dev = &pdev->dev;
	struct device_node *platform, *codec, *spk;
	struct snd_soc_dai_link *link;
	int i, ret;

	platform = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	codec = of_parse_phandle(dev->of_node, "mediatek,audio-codec", 0);
	spk = of_parse_phandle(dev->of_node, "mediatek,speaker-codec", 0);
	if (!platform || !codec || !spk) {
		ret = dev_err_probe(dev, -EINVAL,
				    "need mediatek,platform, audio-codec and speaker-codec\n");
		goto out;
	}

	for_each_card_prelinks(card, i, link) {
		link->cpus->of_node = platform;
		link->platforms->of_node = platform;
	}
	mt8167_mt6392_links[2].codecs->of_node = codec;

	ext_amp = devm_gpiod_get_optional(dev, "ext-amp", GPIOD_OUT_LOW);
	if (IS_ERR(ext_amp)) {
		ret = dev_err_probe(dev, PTR_ERR(ext_amp), "ext-amp gpio\n");
		goto out;
	}
	mt8167_mt6392_aux.dlc.of_node = spk;

	card->dev = dev;
	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		dev_err_probe(dev, ret, "failed to register card\n");
out:
	/* the card core keeps using the nodes, so references are kept on success */
	if (ret) {
		of_node_put(platform);
		of_node_put(codec);
		of_node_put(spk);
	}
	return ret;
}

static const struct of_device_id mt8167_mt6392_dt_match[] = {
	{ .compatible = "mediatek,mt8167-mt6392-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt8167_mt6392_dt_match);

static struct platform_driver mt8167_mt6392_driver = {
	.driver = {
		.name = "mt8167-mt6392",
		.of_match_table = mt8167_mt6392_dt_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = mt8167_mt6392_probe,
};
module_platform_driver(mt8167_mt6392_driver);

MODULE_DESCRIPTION("MT8167 + MT6392 internal codec ASoC machine driver");
MODULE_LICENSE("GPL");
