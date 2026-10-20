// SPDX-License-Identifier: GPL-2.0
/*
 * GalaxyCore GC030A 0.3 MP (VGA) MIPI CSI-2 RAW sensor.
 *
 * Front camera of the Lenovo Tab E7 (TB-7104F). One mode, 640x480 RAW10 BGGR
 * on one data lane. The register sequence is the vendor one
 * (imgsensor/src/mt8167/gc030amipi_raw, kernel-4.4 GPL release).
 *
 * ponytail: single fixed mode, exposure and analogue gain only; no flip
 * controls, no frame rate control. Add them when a user needs them.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define GC030A_REG_PAGE		0xfe
#define GC030A_REG_ID_H		0xf0
#define GC030A_REG_ID_L		0xf1
#define GC030A_CHIP_ID		0x030a
#define GC030A_REG_EXP_H	0x03	/* page 0, [11:8] */
#define GC030A_REG_EXP_L	0x04
#define GC030A_REG_MIPI_EN	0x10	/* page 3: 0x90 streams, 0x00 stops */

#define GC030A_WIDTH		640
#define GC030A_HEIGHT		480
#define GC030A_PIXEL_RATE	23800000
#define GC030A_LINK_FREQ	119000000	/* RAW10, 1 lane: 23.8 Mpx * 10 / 2 */
/* vendor: line length 1561, frame length 506 at 23.8 MHz = 30 fps */
#define GC030A_HBLANK		(1561 - GC030A_WIDTH)
#define GC030A_VBLANK		(506 - GC030A_HEIGHT)
#define GC030A_EXP_MAX		0xfff
#define GC030A_EXP_DEF		0x1c8
#define GC030A_GAIN_MIN		0x40	/* 1.0x, units of 1/64 */
#define GC030A_GAIN_MAX		0x200	/* 8x, keeps the vendor gain table range */
#define GC030A_GAIN_DEF		0x40
#define GC030A_XVCLK		13000000	/* vendor .mclk = 13 */

static const char * const gc030a_supplies[] = { "dovdd", "avdd", "dvdd" };

struct gc030a {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct regmap *regmap;
	struct clk *xvclk;
	struct gpio_desc *pwdn, *reset;
	struct regulator_bulk_data supplies[ARRAY_SIZE(gc030a_supplies)];
	struct v4l2_ctrl *exposure, *gain;
};

static inline struct gc030a *to_gc030a(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc030a, sd);
}

struct reg8 { u8 reg, val; };

/* vendor sensor_init(), MIRROR 0x54 / STARTX 1 / STARTY 1 (no mirror) */
static const struct reg8 gc030a_init[] = {
	{ 0xfe, 0x80 }, { 0xfe, 0x80 }, { 0xfe, 0x80 },
	{ 0xf7, 0x01 }, { 0xf8, 0x0a }, { 0xf9, 0x0f }, { 0xfa, 0x00 },
	{ 0xfc, 0x0f }, { 0xfe, 0x00 },
	/* analog & CISCTL */
	{ 0x03, 0x01 }, { 0x04, 0xc8 }, { 0x05, 0x03 }, { 0x06, 0x7a },
	{ 0x07, 0x00 }, { 0x08, 0x06 }, { 0x0a, 0x00 }, { 0x0c, 0x08 },
	{ 0x0d, 0x01 }, { 0x0e, 0xe8 }, { 0x0f, 0x02 }, { 0x10, 0x88 },
	{ 0x12, 0x28 }, { 0x17, 0x54 }, { 0x18, 0x12 }, { 0x19, 0x07 },
	{ 0x1a, 0x1b }, { 0x1d, 0x48 }, { 0x1e, 0x50 }, { 0x1f, 0x80 },
	{ 0x23, 0x01 }, { 0x24, 0xc8 }, { 0x27, 0xaf }, { 0x28, 0x24 },
	{ 0x29, 0x1a }, { 0x2f, 0x14 }, { 0x30, 0x00 }, { 0x31, 0x04 },
	{ 0x32, 0x08 }, { 0x33, 0x0c }, { 0x34, 0x0d }, { 0x35, 0x0e },
	{ 0x36, 0x0f }, { 0x72, 0x98 }, { 0x73, 0x9a }, { 0x74, 0x47 },
	{ 0x76, 0x82 }, { 0x7a, 0xcb }, { 0xc2, 0x0c }, { 0xce, 0x03 },
	{ 0xcf, 0x48 }, { 0xd0, 0x10 }, { 0xdc, 0x75 }, { 0xeb, 0x78 },
	/* ISP: window 640x480 at (1,1) */
	{ 0x90, 0x01 }, { 0x92, 0x01 }, { 0x94, 0x01 }, { 0x95, 0x01 },
	{ 0x96, 0xe0 }, { 0x97, 0x02 }, { 0x98, 0x80 },
	/* gain */
	{ 0xb0, 0x46 }, { 0xb1, 0x01 }, { 0xb2, 0x00 }, { 0xb3, 0x40 },
	{ 0xb4, 0x40 }, { 0xb5, 0x40 }, { 0xb6, 0x00 },
	/* black level */
	{ 0x40, 0x26 }, { 0x4e, 0x00 }, { 0x4f, 0x3c },
	/* dark sun */
	{ 0xe0, 0x9f }, { 0xe1, 0x90 }, { 0xe4, 0x0f }, { 0xe5, 0xff },
	/* MIPI (page 3), output still off */
	{ 0xfe, 0x03 }, { 0x10, 0x00 }, { 0x01, 0x03 }, { 0x02, 0x33 },
	{ 0x03, 0x96 }, { 0x04, 0x01 }, { 0x05, 0x00 }, { 0x06, 0x80 },
	{ 0x11, 0x2b }, { 0x12, 0x20 }, { 0x13, 0x03 }, { 0x15, 0x00 },
	{ 0x21, 0x10 }, { 0x22, 0x00 }, { 0x23, 0x30 }, { 0x24, 0x02 },
	{ 0x25, 0x12 }, { 0x26, 0x02 }, { 0x29, 0x01 }, { 0x2a, 0x0a },
	{ 0x2b, 0x03 }, { 0xfe, 0x00 }, { 0xf9, 0x0e }, { 0xfc, 0x0e },
	{ 0xfe, 0x00 }, { 0x25, 0xa2 }, { 0x3f, 0x1a },
};

static int gc030a_write_array(struct gc030a *s, const struct reg8 *r, int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		ret = regmap_write(s->regmap, r[i].reg, r[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc030a_power_on(struct device *dev)
{
	struct gc030a *s = to_gc030a(dev_get_drvdata(dev));
	int ret;

	/* vendor order: PDN and RST asserted, IO, (D), A, MCLK, release */
	gpiod_set_value_cansleep(s->pwdn, 1);
	gpiod_set_value_cansleep(s->reset, 1);
	ret = regulator_bulk_enable(ARRAY_SIZE(s->supplies), s->supplies);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = clk_prepare_enable(s->xvclk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(s->supplies), s->supplies);
		return ret;
	}
	usleep_range(3000, 4000);
	gpiod_set_value_cansleep(s->pwdn, 0);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(s->reset, 0);
	usleep_range(2000, 3000);
	return 0;
}

static int gc030a_power_off(struct device *dev)
{
	struct gc030a *s = to_gc030a(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->reset, 1);
	gpiod_set_value_cansleep(s->pwdn, 1);
	clk_disable_unprepare(s->xvclk);
	regulator_bulk_disable(ARRAY_SIZE(s->supplies), s->supplies);
	return 0;
}

static int gc030a_set_exposure(struct gc030a *s, u32 exp)
{
	int ret;

	ret = regmap_write(s->regmap, GC030A_REG_PAGE, 0x00);
	ret = ret ?: regmap_write(s->regmap, GC030A_REG_EXP_H, (exp >> 8) & 0x0f);
	return ret ?: regmap_write(s->regmap, GC030A_REG_EXP_L, exp & 0xff);
}

/* vendor set_gain(): total gain in 1/64 split into 0xb1 (int) and 0xb2 (frac) */
static int gc030a_set_gain(struct gc030a *s, u32 gain)
{
	int ret;

	ret = regmap_write(s->regmap, GC030A_REG_PAGE, 0x00);
	ret = ret ?: regmap_write(s->regmap, 0xb1, gain >> 6);
	return ret ?: regmap_write(s->regmap, 0xb2, (gain << 2) & 0xfc);
}

static int gc030a_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc030a *s = container_of(ctrl->handler, struct gc030a, ctrls);
	struct device *dev = s->sd.dev;
	int ret = 0;

	/* only touch the hardware while it is powered */
	if (!pm_runtime_get_if_in_use(dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = gc030a_set_exposure(s, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc030a_set_gain(s, ctrl->val);
		break;
	}
	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops gc030a_ctrl_ops = {
	.s_ctrl = gc030a_s_ctrl,
};

static int gc030a_s_stream(struct v4l2_subdev *sd, int on)
{
	struct gc030a *s = to_gc030a(sd);
	struct device *dev = sd->dev;
	int ret;

	if (!on) {
		regmap_write(s->regmap, GC030A_REG_PAGE, 0x03);
		regmap_write(s->regmap, GC030A_REG_MIPI_EN, 0x00);
		regmap_write(s->regmap, GC030A_REG_PAGE, 0x00);
		pm_runtime_put(dev);
		return 0;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;
	ret = gc030a_write_array(s, gc030a_init, ARRAY_SIZE(gc030a_init));
	if (ret)
		goto err;
	msleep(100);			/* vendor: Sleep(100) before 0x25 = 0xe2 */
	ret = regmap_write(s->regmap, 0x25, 0xe2);
	ret = ret ?: __v4l2_ctrl_handler_setup(&s->ctrls);
	ret = ret ?: regmap_write(s->regmap, GC030A_REG_PAGE, 0x03);
	ret = ret ?: regmap_write(s->regmap, GC030A_REG_MIPI_EN, 0x90);
	ret = ret ?: regmap_write(s->regmap, GC030A_REG_PAGE, 0x00);
	if (ret)
		goto err;
	return 0;
err:
	pm_runtime_put(dev);
	return ret;
}

static void gc030a_fill_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = GC030A_WIDTH;
	fmt->height = GC030A_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int gc030a_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int gc030a_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = GC030A_WIDTH;
	fse->min_height = fse->max_height = GC030A_HEIGHT;
	return 0;
}

/* one fixed mode: get and set both report it */
static int gc030a_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
		      struct v4l2_subdev_format *f)
{
	gc030a_fill_fmt(&f->format);
	*v4l2_subdev_state_get_format(state, 0) = f->format;
	return 0;
}

static int gc030a_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	gc030a_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops gc030a_video_ops = {
	.s_stream = gc030a_s_stream,
};

static const struct v4l2_subdev_pad_ops gc030a_pad_ops = {
	.enum_mbus_code = gc030a_enum_mbus_code,
	.enum_frame_size = gc030a_enum_frame_size,
	.get_fmt = gc030a_fmt,
	.set_fmt = gc030a_fmt,
};

static const struct v4l2_subdev_ops gc030a_subdev_ops = {
	.video = &gc030a_video_ops,
	.pad = &gc030a_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc030a_internal_ops = {
	.init_state = gc030a_init_state,
};

static const struct regmap_config gc030a_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int gc030a_check_id(struct gc030a *s, struct device *dev)
{
	unsigned int hi, lo;
	int ret;

	ret = regmap_write(s->regmap, GC030A_REG_PAGE, 0x00);
	ret = ret ?: regmap_read(s->regmap, GC030A_REG_ID_H, &hi);
	ret = ret ?: regmap_read(s->regmap, GC030A_REG_ID_L, &lo);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read chip id\n");
	if ((hi << 8 | lo) != GC030A_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "chip id 0x%04x, expected 0x%04x\n",
				     hi << 8 | lo, GC030A_CHIP_ID);
	return 0;
}

static int gc030a_init_controls(struct gc030a *s)
{
	static const s64 link_freq[] = { GC030A_LINK_FREQ };
	struct v4l2_ctrl_handler *h = &s->ctrls;
	struct v4l2_ctrl *c;

	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(h, 8);
	s->exposure = v4l2_ctrl_new_std(h, &gc030a_ctrl_ops, V4L2_CID_EXPOSURE,
					1, GC030A_EXP_MAX, 1, GC030A_EXP_DEF);
	/* rotation and orientation from the DT node, libcamera reads them as controls */
	ret = v4l2_fwnode_device_parse(s->sd.dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(h, &gc030a_ctrl_ops, &props);
	dev_dbg(s->sd.dev, "fwnode props: ret=%d rotation=%u orientation=%u ctrl err=%d\n",
		 ret, props.rotation, props.orientation, h->error);
	/* fixed mode: blanking is read-only, libcamera needs it for frame timing */
	c = v4l2_ctrl_new_std(h, NULL, V4L2_CID_HBLANK, GC030A_HBLANK, GC030A_HBLANK, 1,
			      GC030A_HBLANK);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	c = v4l2_ctrl_new_std(h, NULL, V4L2_CID_VBLANK, GC030A_VBLANK, GC030A_VBLANK, 1,
			      GC030A_VBLANK);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->gain = v4l2_ctrl_new_std(h, &gc030a_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				    GC030A_GAIN_MIN, GC030A_GAIN_MAX, 1, GC030A_GAIN_DEF);
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE, GC030A_PIXEL_RATE,
			  GC030A_PIXEL_RATE, 1, GC030A_PIXEL_RATE);
	c = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0, link_freq);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	if (h->error)
		return h->error;
	s->sd.ctrl_handler = h;
	return 0;
}

static int gc030a_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc030a *s;
	int i, ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->regmap = devm_regmap_init_i2c(client, &gc030a_regmap);
	if (IS_ERR(s->regmap))
		return PTR_ERR(s->regmap);

	s->xvclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->xvclk))
		return dev_err_probe(dev, PTR_ERR(s->xvclk), "xvclk\n");
	ret = clk_set_rate(s->xvclk, GC030A_XVCLK);
	if (ret)
		dev_warn(dev, "xvclk not set to 13 MHz (%d), using %lu\n", ret,
			 clk_get_rate(s->xvclk));

	s->pwdn = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(s->pwdn))
		return dev_err_probe(dev, PTR_ERR(s->pwdn), "powerdown gpio\n");
	/* optional: on the Tab E7 the reset pad is shared with the rear sensor and hogged high */
	s->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return dev_err_probe(dev, PTR_ERR(s->reset), "reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(gc030a_supplies); i++)
		s->supplies[i].supply = gc030a_supplies[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(s->supplies), s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	v4l2_i2c_subdev_init(&s->sd, client, &gc030a_subdev_ops);
	s->sd.internal_ops = &gc030a_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = gc030a_power_on(dev);
	if (ret)
		return ret;
	ret = gc030a_check_id(s, dev);
	if (ret)
		goto power_off;
	dev_info(dev, "GC030A found\n");

	ret = gc030a_init_controls(s);
	if (ret)
		goto free_ctrls;

	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto free_ctrls;
	s->sd.state_lock = s->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret)
		goto cleanup_entity;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto disable_pm;
	pm_runtime_put_autosuspend(dev);
	return 0;

disable_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&s->sd);
cleanup_entity:
	media_entity_cleanup(&s->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&s->ctrls);
power_off:
	gc030a_power_off(dev);
	return ret;
}

static void gc030a_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc030a *s = to_gc030a(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc030a_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(gc030a_pm_ops, gc030a_power_off, gc030a_power_on, NULL);

static const struct of_device_id gc030a_of_match[] = {
	{ .compatible = "galaxycore,gc030a" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc030a_of_match);

static struct i2c_driver gc030a_driver = {
	.driver = {
		.name = "gc030a",
		.of_match_table = gc030a_of_match,
		.pm = pm_ptr(&gc030a_pm_ops),
	},
	.probe = gc030a_probe,
	.remove = gc030a_remove,
};
module_i2c_driver(gc030a_driver);

MODULE_DESCRIPTION("GalaxyCore GC030A VGA camera sensor driver");
MODULE_LICENSE("GPL");
