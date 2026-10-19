// SPDX-License-Identifier: GPL-2.0
/*
 * GalaxyCore GC2375H 2 MP MIPI CSI-2 RAW sensor.
 *
 * Rear camera of the Lenovo Tab E7 (TB-7104F). One mode, 1600x1200 RAW10
 * RGGB on one data lane at 30 fps. Register sequence and gain table from the
 * vendor driver (imgsensor/src/mt8167/gc2375hmipi_raw, kernel-4.4 GPL).
 *
 * ponytail: single fixed mode, exposure and analogue gain only. Reset is
 * not driven here: the pad is shared with the front sensor and held high
 * by a gpio-hog, power-down alone gates the sensor.
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

#define GC2375H_REG_PAGE	0xfe
#define GC2375H_REG_ID_H	0xf0
#define GC2375H_REG_ID_L	0xf1
#define GC2375H_CHIP_ID		0x2375
#define GC2375H_REG_EXP_H	0x03	/* page 0, [13:8] */
#define GC2375H_REG_EXP_L	0x04
#define GC2375H_REG_MIPI_EN	0xef	/* page 0: 0x90 streams */

#define GC2375H_WIDTH		1600
#define GC2375H_HEIGHT		1200
#define GC2375H_PIXEL_RATE	45500000
#define GC2375H_LINK_FREQ	227500000	/* RAW10, 1 lane: 45.5 Mpx * 10 / 2 */
#define GC2375H_LINE_LENGTH	1040
#define GC2375H_FRAME_LENGTH	1456
#define GC2375H_EXP_MAX		(GC2375H_FRAME_LENGTH - 16)
#define GC2375H_EXP_DEF		0x465
#define GC2375H_GAIN_MIN	64	/* 1.0x, units of 1/64 */
#define GC2375H_GAIN_MAX	1038	/* 16.2x, last vendor table step */
#define GC2375H_GAIN_DEF	64
#define GC2375H_XVCLK		13000000	/* vendor .mclk = 13 */

static const char * const gc2375h_supplies[] = { "dovdd", "avdd", "dvdd" };

struct gc2375h {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct regmap *regmap;
	struct clk *xvclk;
	struct gpio_desc *pwdn;
	struct regulator_bulk_data supplies[ARRAY_SIZE(gc2375h_supplies)];
};

static inline struct gc2375h *to_gc2375h(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc2375h, sd);
}

struct reg8 { u8 reg, val; };

/* vendor sensor_init(), MIRROR 0xd4 (normal), BLK select 0x00/0x3c/0x00/0x03; ends with MIPI on */
static const struct reg8 gc2375h_init[] = {
	{ 0xfe, 0x00 }, { 0xf7, 0x01 }, { 0xf8, 0x1b }, { 0xf9, 0x42 },
	{ 0xfa, 0x88 }, { 0xfc, 0x8e }, { 0xfe, 0x00 }, { 0x88, 0x03 },
	{ 0x03, 0x04 }, { 0x04, 0x65 }, { 0x05, 0x02 }, { 0x06, 0x5a },
	{ 0x07, 0x00 }, { 0x08, 0xe8 }, { 0x09, 0x00 }, { 0x0a, 0x04 },
	{ 0x0b, 0x00 }, { 0x0c, 0x14 }, { 0x0d, 0x04 }, { 0x0e, 0xb8 },
	{ 0x0f, 0x06 }, { 0x10, 0x48 }, { 0x17, 0xd4 }, { 0x1c, 0x10 },
	{ 0x1d, 0x13 }, { 0x20, 0x0b }, { 0x21, 0x6d }, { 0x22, 0x0c },
	{ 0x25, 0xc1 }, { 0x26, 0x0e }, { 0x27, 0x22 }, { 0x29, 0x5f },
	{ 0x2b, 0x88 }, { 0x2f, 0x12 }, { 0x38, 0x86 }, { 0x3d, 0x00 },
	{ 0xcd, 0xa3 }, { 0xce, 0x57 }, { 0xd0, 0x09 }, { 0xd1, 0xca },
	{ 0xd2, 0x34 }, { 0xd3, 0xbb }, { 0xd8, 0x60 }, { 0xe0, 0x08 },
	{ 0xe1, 0x1f }, { 0xe4, 0xf8 }, { 0xe5, 0x0c }, { 0xe6, 0x10 },
	{ 0xe7, 0xcc }, { 0xe8, 0x02 }, { 0xe9, 0x01 }, { 0xea, 0x02 },
	{ 0xeb, 0x01 }, { 0x90, 0x01 }, { 0x92, 0x04 }, { 0x94, 0x04 },
	{ 0x95, 0x04 }, { 0x96, 0xb0 }, { 0x97, 0x06 }, { 0x98, 0x40 },
	{ 0x18, 0x02 }, { 0x1a, 0x18 }, { 0x28, 0x00 }, { 0x3f, 0x40 },
	{ 0x40, 0x26 }, { 0x41, 0x00 }, { 0x43, 0x03 }, { 0x4a, 0x00 },
	{ 0x4e, 0x00 }, { 0x4f, 0x3c }, { 0x66, 0x00 }, { 0x67, 0x03 },
	{ 0x68, 0x00 }, { 0xb0, 0x58 }, { 0xb1, 0x01 }, { 0xb2, 0x00 },
	{ 0xb6, 0x00 }, { 0xef, 0x90 }, { 0xfe, 0x03 }, { 0x01, 0x03 },
	{ 0x02, 0x33 }, { 0x03, 0x90 }, { 0x04, 0x04 }, { 0x05, 0x00 },
	{ 0x06, 0x80 }, { 0x11, 0x2b }, { 0x12, 0xd0 }, { 0x13, 0x07 },
	{ 0x15, 0x00 }, { 0x21, 0x08 }, { 0x22, 0x05 }, { 0x23, 0x13 },
	{ 0x24, 0x02 }, { 0x25, 0x13 }, { 0x26, 0x08 }, { 0x29, 0x06 },
	{ 0x2a, 0x08 }, { 0x2b, 0x08 }, { 0xfe, 0x00 },
};

/* vendor set_gain(): analogue steps (0x20/0x22/0x26/0xb6) and a 1/64 digital remainder in 0xb1/0xb2 */
static const struct { u16 gain; u8 r20, r22, b6; } gc2375h_gain_steps[] = {
	{ 64, 0x0b, 0x0c, 0 }, { 92, 0x0c, 0x0e, 1 }, { 128, 0x0c, 0x0e, 2 },
	{ 182, 0x0c, 0x0e, 3 }, { 254, 0x0c, 0x0e, 4 }, { 363, 0x0e, 0x0e, 5 },
	{ 521, 0x0e, 0x0e, 6 }, { 725, 0x0e, 0x0e, 7 }, { 1038, 0x0e, 0x0e, 8 },
};

static int gc2375h_write_array(struct gc2375h *s, const struct reg8 *r, int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		ret = regmap_write(s->regmap, r[i].reg, r[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc2375h_power_on(struct device *dev)
{
	struct gc2375h *s = to_gc2375h(dev_get_drvdata(dev));
	int ret;

	gpiod_set_value_cansleep(s->pwdn, 1);
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
	usleep_range(5000, 6000);
	return 0;
}

static int gc2375h_power_off(struct device *dev)
{
	struct gc2375h *s = to_gc2375h(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->pwdn, 1);
	clk_disable_unprepare(s->xvclk);
	regulator_bulk_disable(ARRAY_SIZE(s->supplies), s->supplies);
	return 0;
}

static int gc2375h_set_exposure(struct gc2375h *s, u32 exp)
{
	int ret;

	ret = regmap_write(s->regmap, GC2375H_REG_PAGE, 0x00);
	ret = ret ?: regmap_write(s->regmap, GC2375H_REG_EXP_H, (exp >> 8) & 0x3f);
	return ret ?: regmap_write(s->regmap, GC2375H_REG_EXP_L, exp & 0xff);
}

static int gc2375h_set_gain(struct gc2375h *s, u32 gain)
{
	int i = ARRAY_SIZE(gc2375h_gain_steps) - 1, ret;
	u32 rem;

	while (i > 0 && gain < gc2375h_gain_steps[i].gain)
		i--;
	rem = 64 * gain / gc2375h_gain_steps[i].gain;
	ret = regmap_write(s->regmap, GC2375H_REG_PAGE, 0x00);
	ret = ret ?: regmap_write(s->regmap, 0x20, gc2375h_gain_steps[i].r20);
	ret = ret ?: regmap_write(s->regmap, 0x22, gc2375h_gain_steps[i].r22);
	ret = ret ?: regmap_write(s->regmap, 0x26, 0x0e);
	ret = ret ?: regmap_write(s->regmap, 0xb6, gc2375h_gain_steps[i].b6);
	ret = ret ?: regmap_write(s->regmap, 0xb1, rem >> 6);
	return ret ?: regmap_write(s->regmap, 0xb2, (rem << 2) & 0xfc);
}

static int gc2375h_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc2375h *s = container_of(ctrl->handler, struct gc2375h, ctrls);
	struct device *dev = s->sd.dev;
	int ret = 0;

	if (!pm_runtime_get_if_in_use(dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = gc2375h_set_exposure(s, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc2375h_set_gain(s, ctrl->val);
		break;
	}
	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops gc2375h_ctrl_ops = {
	.s_ctrl = gc2375h_s_ctrl,
};

static int gc2375h_s_stream(struct v4l2_subdev *sd, int on)
{
	struct gc2375h *s = to_gc2375h(sd);
	struct device *dev = sd->dev;
	int ret;

	if (!on) {
		regmap_write(s->regmap, GC2375H_REG_PAGE, 0x00);
		regmap_write(s->regmap, GC2375H_REG_MIPI_EN, 0x00);
		pm_runtime_put(dev);
		return 0;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;
	ret = gc2375h_write_array(s, gc2375h_init, ARRAY_SIZE(gc2375h_init));
	ret = ret ?: __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret) {
		pm_runtime_put(dev);
		return ret;
	}
	return 0;
}

static void gc2375h_fill_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = GC2375H_WIDTH;
	fmt->height = GC2375H_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int gc2375h_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int gc2375h_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = GC2375H_WIDTH;
	fse->min_height = fse->max_height = GC2375H_HEIGHT;
	return 0;
}

static int gc2375h_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
		       struct v4l2_subdev_format *f)
{
	gc2375h_fill_fmt(&f->format);
	*v4l2_subdev_state_get_format(state, 0) = f->format;
	return 0;
}

static int gc2375h_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	gc2375h_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops gc2375h_video_ops = {
	.s_stream = gc2375h_s_stream,
};

static const struct v4l2_subdev_pad_ops gc2375h_pad_ops = {
	.enum_mbus_code = gc2375h_enum_mbus_code,
	.enum_frame_size = gc2375h_enum_frame_size,
	.get_fmt = gc2375h_fmt,
	.set_fmt = gc2375h_fmt,
};

static const struct v4l2_subdev_ops gc2375h_subdev_ops = {
	.video = &gc2375h_video_ops,
	.pad = &gc2375h_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc2375h_internal_ops = {
	.init_state = gc2375h_init_state,
};

static const struct regmap_config gc2375h_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int gc2375h_check_id(struct gc2375h *s, struct device *dev)
{
	unsigned int hi, lo;
	int ret;

	ret = regmap_write(s->regmap, GC2375H_REG_PAGE, 0x00);
	ret = ret ?: regmap_read(s->regmap, GC2375H_REG_ID_H, &hi);
	ret = ret ?: regmap_read(s->regmap, GC2375H_REG_ID_L, &lo);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read chip id\n");
	if ((hi << 8 | lo) != GC2375H_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "chip id 0x%04x, expected 0x%04x\n",
				     hi << 8 | lo, GC2375H_CHIP_ID);
	return 0;
}

static int gc2375h_init_controls(struct gc2375h *s)
{
	static const s64 link_freq[] = { GC2375H_LINK_FREQ };
	struct v4l2_ctrl_handler *h = &s->ctrls;
	struct v4l2_ctrl *c;

	v4l2_ctrl_handler_init(h, 4);
	v4l2_ctrl_new_std(h, &gc2375h_ctrl_ops, V4L2_CID_EXPOSURE, 1, GC2375H_EXP_MAX, 1,
			  GC2375H_EXP_DEF);
	v4l2_ctrl_new_std(h, &gc2375h_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, GC2375H_GAIN_MIN,
			  GC2375H_GAIN_MAX, 1, GC2375H_GAIN_DEF);
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE, GC2375H_PIXEL_RATE,
			  GC2375H_PIXEL_RATE, 1, GC2375H_PIXEL_RATE);
	c = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0, link_freq);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	if (h->error)
		return h->error;
	s->sd.ctrl_handler = h;
	return 0;
}

static int gc2375h_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc2375h *s;
	int i, ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->regmap = devm_regmap_init_i2c(client, &gc2375h_regmap);
	if (IS_ERR(s->regmap))
		return PTR_ERR(s->regmap);

	s->xvclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->xvclk))
		return dev_err_probe(dev, PTR_ERR(s->xvclk), "xvclk\n");
	ret = clk_set_rate(s->xvclk, GC2375H_XVCLK);
	if (ret)
		dev_warn(dev, "xvclk not set to 13 MHz (%d), using %lu\n", ret,
			 clk_get_rate(s->xvclk));

	s->pwdn = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(s->pwdn))
		return dev_err_probe(dev, PTR_ERR(s->pwdn), "powerdown gpio\n");

	for (i = 0; i < ARRAY_SIZE(gc2375h_supplies); i++)
		s->supplies[i].supply = gc2375h_supplies[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(s->supplies), s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	v4l2_i2c_subdev_init(&s->sd, client, &gc2375h_subdev_ops);
	s->sd.internal_ops = &gc2375h_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = gc2375h_power_on(dev);
	if (ret)
		return ret;
	ret = gc2375h_check_id(s, dev);
	if (ret)
		goto power_off;
	dev_info(dev, "GC2375H found\n");

	ret = gc2375h_init_controls(s);
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
	gc2375h_power_off(dev);
	return ret;
}

static void gc2375h_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2375h *s = to_gc2375h(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc2375h_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(gc2375h_pm_ops, gc2375h_power_off, gc2375h_power_on, NULL);

static const struct of_device_id gc2375h_of_match[] = {
	{ .compatible = "galaxycore,gc2375h" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc2375h_of_match);

static struct i2c_driver gc2375h_driver = {
	.driver = {
		.name = "gc2375h",
		.of_match_table = gc2375h_of_match,
		.pm = pm_ptr(&gc2375h_pm_ops),
	},
	.probe = gc2375h_probe,
	.remove = gc2375h_remove,
};
module_i2c_driver(gc2375h_driver);

MODULE_DESCRIPTION("GalaxyCore GC2375H 2 MP camera sensor driver");
MODULE_LICENSE("GPL");
