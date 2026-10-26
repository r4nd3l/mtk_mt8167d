// SPDX-License-Identifier: GPL-2.0+
/*
 * Lenovo Tab E7 (TB-7104F) 1024x600 MIPI-DSI panel (Fitipower EK79007,
 * "ek79007_wsvga_xy" in the vendor LCM list, picked by LK for lcd_id=0).
 *
 * Init table, power sequence and timings from the vendor
 * drivers/misc/mediatek/lcm/ek79007_wsvga_dsi_vdo/ek79007_wsvga_dsi_vdo.c.
 * The EK79007 only takes video: no sleep out / display on is sent.
 *
 * Original skeleton: Luca Leonardo Scorcia <l.scorcia@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>

struct tb7104f_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	enum drm_panel_orientation orientation;
	struct regulator *vdd;
	struct gpio_desc *reset;
	struct gpio_desc *enable;
};

static inline struct tb7104f_panel *to_tb7104f(struct drm_panel *panel)
{
	return container_of(panel, struct tb7104f_panel, base);
}

static int tb7104f_prepare(struct drm_panel *panel)
{
	struct tb7104f_panel *ctx = to_tb7104f(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	ret = regulator_enable(ctx->vdd);
	if (ret)
		return ret;
	msleep(20);

	/* vendor: RST high 20 ms, low 20 ms, high 50 ms (reset-gpios is active low) */
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(50);

	/* vendor dsi_set_cmdq_V2: cmd < 0xB0 is a DCS write, the rest generic */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80, 0x8b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x81, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x82, 0xaf);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x83, 0xdf);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x84, 0x97);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x85, 0x9c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x86, 0xb9);
	mipi_dsi_msleep(&dsi_ctx, 150);
	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		regulator_disable(ctx->vdd);
		return dsi_ctx.accum_err;
	}

	gpiod_set_value_cansleep(ctx->enable, 1);
	msleep(20);

	return 0;
}

static int tb7104f_unprepare(struct drm_panel *panel)
{
	struct tb7104f_panel *ctx = to_tb7104f(panel);

	gpiod_set_value_cansleep(ctx->enable, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(30);
	regulator_disable(ctx->vdd);

	return 0;
}

/* vendor: hfp 160, hsa 10, hbp 160, vfp 12, vsa 1, vbp 23, 3 lanes at 420 Mbps */
static const struct drm_display_mode tb7104f_mode = {
	.clock		= (1024 + 160 + 10 + 160) * (600 + 12 + 1 + 23) * 60 / 1000,
	.hdisplay	= 1024,
	.hsync_start	= 1024 + 160,
	.hsync_end	= 1024 + 160 + 10,
	.htotal		= 1024 + 160 + 10 + 160,
	.vdisplay	= 600,
	.vsync_start	= 600 + 12,
	.vsync_end	= 600 + 12 + 1,
	.vtotal		= 600 + 12 + 1 + 23,
	.width_mm	= 154,
	.height_mm	= 86,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int tb7104f_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &tb7104f_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static enum drm_panel_orientation tb7104f_get_orientation(struct drm_panel *panel)
{
	return to_tb7104f(panel)->orientation;
}

static const struct drm_panel_funcs tb7104f_funcs = {
	.prepare = tb7104f_prepare,
	.unprepare = tb7104f_unprepare,
	.get_modes = tb7104f_get_modes,
	.get_orientation = tb7104f_get_orientation,
};

static int tb7104f_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tb7104f_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct tb7104f_panel, base, &tb7104f_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 3;

	/* LK leaves the panel running: take the lines over as they are */
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "no reset GPIO\n");

	ctx->enable = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(ctx->enable))
		return dev_err_probe(dev, PTR_ERR(ctx->enable), "no enable GPIO\n");

	ctx->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd))
		return dev_err_probe(dev, PTR_ERR(ctx->vdd), "no vdd regulator\n");

	ret = drm_of_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	ret = drm_panel_of_backlight(&ctx->base);
	if (ret)
		return ret;

	/* the init table goes out in LP mode before the video stream starts */
	ctx->base.prepare_prev_first = true;
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_add(&ctx->base);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->base);

	return ret;
}

static void tb7104f_remove(struct mipi_dsi_device *dsi)
{
	struct tb7104f_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->base);
}

static const struct of_device_id tb7104f_of_match[] = {
	{ .compatible = "lenovo,tb7104f-panel" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tb7104f_of_match);

static struct mipi_dsi_driver tb7104f_driver = {
	.probe = tb7104f_probe,
	.remove = tb7104f_remove,
	.driver = {
		.name = "lenovo-tb7104f-panel",
		.of_match_table = tb7104f_of_match,
	},
};
module_mipi_dsi_driver(tb7104f_driver);

MODULE_DESCRIPTION("Lenovo TB-7104F (EK79007) DSI panel");
MODULE_LICENSE("GPL");
