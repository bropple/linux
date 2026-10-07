// SPDX-License-Identifier: GPL-2.0
/*
 * GalaxyCore GC02M2: 2 MP (1600x1200) raw Bayer sensor, one MIPI CSI-2 lane.
 *
 * The register setup and the analogue gain table come from Rockchip's GPL
 * driver (drivers/media/i2c/gc02m2.c in rockchip-linux/kernel, Copyright
 * (C) 2020 Rockchip Electronics Co., Ltd.); the rest follows the mainline
 * conventions of our gc5035 driver: analogue gain is linear for userspace
 * (16 = 1x), the pixel array is reported through get_selection, and the
 * firmware's orientation and rotation are controls.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define GC02M2_MCLK_RATE	24000000
#define GC02M2_LINK_FREQ	336000000
#define GC02M2_PIXEL_RATE	(GC02M2_LINK_FREQ * 2LL / 10)	/* one lane, 10 bits */

#define GC02M2_WIDTH		1600
#define GC02M2_HEIGHT		1200
#define GC02M2_HTS		(0x448 * 2)
#define GC02M2_VTS_DEF		0x4f4
#define GC02M2_VTS_MAX		0x3fff
#define GC02M2_EXP_DEF		0x475
#define GC02M2_EXP_MIN		4
#define GC02M2_EXP_MARGIN	16

#define GC02M2_REG_PAGE		0xfe
#define GC02M2_REG_ID_H		0xf0
#define GC02M2_REG_ID_L		0xf1
#define GC02M2_ID		0x02f0
#define GC02M2_REG_EXP_H	0x03
#define GC02M2_REG_EXP_L	0x04
#define GC02M2_REG_VTS_H	0x41
#define GC02M2_REG_VTS_L	0x42
#define GC02M2_REG_AGAIN	0xb6
#define GC02M2_REG_PREGAIN_H	0xb1
#define GC02M2_REG_PREGAIN_L	0xb2
#define GC02M2_REG_FLIP		0x17	/* bit 0 mirror, bit 1 flip */
#define GC02M2_REG_MODE		0x3e
#define GC02M2_MODE_STREAM	0x90
#define GC02M2_MODE_STANDBY	0x00

/* Gain for userspace: linear, 16 = 1x */
#define GC02M2_GAIN_ONE		16
#define GC02M2_GAIN_MAX		160

struct gc02m2_reg {
	u8 reg, val;
};

static const struct gc02m2_reg gc02m2_init[] = {
	{ 0xfc, 0x01 }, { 0xf4, 0x41 }, { 0xf5, 0xc0 }, { 0xf6, 0x44 }, { 0xf8, 0x38 }, { 0xf9, 0x82 },
	{ 0xfa, 0x00 }, { 0xfd, 0x80 }, { 0xfc, 0x81 }, { 0xfe, 0x03 }, { 0x01, 0x0b }, { 0xf7, 0x01 },
	{ 0xfc, 0x80 }, { 0xfc, 0x80 }, { 0xfc, 0x80 }, { 0xfc, 0x8e }, { 0xfe, 0x00 }, { 0x87, 0x09 },
	{ 0xee, 0x72 }, { 0xfe, 0x01 }, { 0x8c, 0x90 }, { 0xfe, 0x00 }, { 0x90, 0x00 }, { 0x03, 0x04 },
	{ 0x04, 0x7d }, { 0x41, 0x04 }, { 0x42, 0xf4 }, { 0x05, 0x04 }, { 0x06, 0x48 }, { 0x07, 0x00 },
	{ 0x08, 0x18 }, { 0x9d, 0x18 }, { 0x09, 0x00 }, { 0x0a, 0x02 }, { 0x0d, 0x04 }, { 0x0e, 0xbc },
	{ 0x17, 0x80 }, { 0x19, 0x04 }, { 0x24, 0x00 }, { 0x56, 0x20 }, { 0x5b, 0x00 }, { 0x5e, 0x01 },
	{ 0x21, 0x3c }, { 0x44, 0x20 }, { 0xcc, 0x01 }, { 0x1a, 0x04 }, { 0x1f, 0x11 }, { 0x27, 0x30 },
	{ 0x2b, 0x00 }, { 0x33, 0x00 }, { 0x53, 0x90 }, { 0xe6, 0x50 }, { 0x39, 0x07 }, { 0x43, 0x04 },
	{ 0x46, 0x2a }, { 0x7c, 0xa0 }, { 0xd0, 0xbe }, { 0xd1, 0x40 }, { 0xd2, 0x40 }, { 0xd3, 0xb3 },
	{ 0xde, 0x1c }, { 0xcd, 0x06 }, { 0xce, 0x6f }, { 0xfc, 0x88 }, { 0xfe, 0x10 }, { 0xfe, 0x00 },
	{ 0xfc, 0x8e }, { 0xfe, 0x00 }, { 0xfe, 0x00 }, { 0xfe, 0x00 }, { 0xfe, 0x00 }, { 0xfc, 0x88 },
	{ 0xfe, 0x10 }, { 0xfe, 0x00 }, { 0xfc, 0x8e }, { 0xfe, 0x04 }, { 0xe0, 0x01 }, { 0xfe, 0x00 },
	{ 0xfe, 0x01 }, { 0x53, 0x54 }, { 0x87, 0x53 }, { 0x89, 0x03 }, { 0xfe, 0x00 }, { 0xb0, 0x74 },
	{ 0xb1, 0x04 }, { 0xb2, 0x00 }, { 0xb6, 0x00 }, { 0xfe, 0x04 }, { 0xd8, 0x00 }, { 0xc0, 0x40 },
	{ 0xc0, 0x00 }, { 0xc0, 0x00 }, { 0xc0, 0x00 }, { 0xc0, 0x60 }, { 0xc0, 0x00 }, { 0xc0, 0xc0 },
	{ 0xc0, 0x2a }, { 0xc0, 0x80 }, { 0xc0, 0x00 }, { 0xc0, 0x00 }, { 0xc0, 0x40 }, { 0xc0, 0xa0 },
	{ 0xc0, 0x00 }, { 0xc0, 0x90 }, { 0xc0, 0x19 }, { 0xc0, 0xc0 }, { 0xc0, 0x00 }, { 0xc0, 0xd0 },
	{ 0xc0, 0x2f }, { 0xc0, 0xe0 }, { 0xc0, 0x00 }, { 0xc0, 0x90 }, { 0xc0, 0x39 }, { 0xc0, 0x00 },
	{ 0xc0, 0x01 }, { 0xc0, 0x20 }, { 0xc0, 0x04 }, { 0xc0, 0x20 }, { 0xc0, 0x01 }, { 0xc0, 0xe0 },
	{ 0xc0, 0x0f }, { 0xc0, 0x40 }, { 0xc0, 0x01 }, { 0xc0, 0xe0 }, { 0xc0, 0x1a }, { 0xc0, 0x60 },
	{ 0xc0, 0x01 }, { 0xc0, 0x20 }, { 0xc0, 0x25 }, { 0xc0, 0x80 }, { 0xc0, 0x01 }, { 0xc0, 0xa0 },
	{ 0xc0, 0x2c }, { 0xc0, 0xa0 }, { 0xc0, 0x01 }, { 0xc0, 0xe0 }, { 0xc0, 0x32 }, { 0xc0, 0xc0 },
	{ 0xc0, 0x01 }, { 0xc0, 0x20 }, { 0xc0, 0x38 }, { 0xc0, 0xe0 }, { 0xc0, 0x01 }, { 0xc0, 0x60 },
	{ 0xc0, 0x3c }, { 0xc0, 0x00 }, { 0xc0, 0x02 }, { 0xc0, 0xa0 }, { 0xc0, 0x40 }, { 0xc0, 0x80 },
	{ 0xc0, 0x02 }, { 0xc0, 0x18 }, { 0xc0, 0x5c }, { 0xfe, 0x00 }, { 0x9f, 0x10 }, { 0xfe, 0x00 },
	{ 0x26, 0x20 }, { 0xfe, 0x01 }, { 0x40, 0x22 }, { 0x46, 0x7f }, { 0x49, 0x0f }, { 0x4a, 0xf0 },
	{ 0xfe, 0x04 }, { 0x14, 0x80 }, { 0x15, 0x80 }, { 0x16, 0x80 }, { 0x17, 0x80 }, { 0xfe, 0x01 },
	{ 0x41, 0x20 }, { 0x4c, 0x00 }, { 0x4d, 0x0c }, { 0x44, 0x08 }, { 0x48, 0x03 }, { 0xfe, 0x01 },
	{ 0x90, 0x01 }, { 0x91, 0x00 }, { 0x92, 0x06 }, { 0x93, 0x00 }, { 0x94, 0x06 }, { 0x95, 0x04 },
	{ 0x96, 0xb0 }, { 0x97, 0x06 }, { 0x98, 0x40 }, { 0xfe, 0x03 }, { 0x01, 0x23 }, { 0x03, 0xce },
	{ 0x04, 0x48 }, { 0x15, 0x01 }, { 0x21, 0x10 }, { 0x22, 0x05 }, { 0x23, 0x20 }, { 0x25, 0x20 },
	{ 0x26, 0x08 }, { 0x29, 0x06 }, { 0x2a, 0x0a }, { 0x2b, 0x08 }, { 0xfe, 0x01 }, { 0x8c, 0x10 },
};

/* Analogue gain levels: the gain at each level, 64 = 1x (Rockchip's table);
 * what a level cannot reach, the pre-gain (1024 = 1x) makes up */
static const u16 gc02m2_again[] = {
	64, 96, 127, 157, 198, 227, 259, 287, 318, 356, 392, 420, 451, 480,
	513, 646,
};

static const char * const gc02m2_supplies[] = { "dovdd", "avdd", "dvdd" };

static const s64 gc02m2_link_freqs[] = { GC02M2_LINK_FREQ };

struct gc02m2 {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *mclk;
	struct gpio_desc *reset;
	struct gpio_desc *powerdown;
	struct regulator_bulk_data supplies[ARRAY_SIZE(gc02m2_supplies)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip, *vflip;
};

static inline struct gc02m2 *to_gc02m2(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc02m2, sd);
}

static int gc02m2_write(struct gc02m2 *s, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(s->client, reg, val);

	if (ret)
		dev_err_ratelimited(&s->client->dev, "write %#x: %d\n", reg, ret);
	return ret;
}

static int gc02m2_read(struct gc02m2 *s, u8 reg)
{
	return i2c_smbus_read_byte_data(s->client, reg);
}

static int gc02m2_set_gain(struct gc02m2 *s, int val)
{
	unsigned int total = val * 64 / GC02M2_GAIN_ONE, i, pre;
	int ret;

	if (total < 64)
		total = 64;
	for (i = ARRAY_SIZE(gc02m2_again) - 1; i > 0; i--)
		if (total >= gc02m2_again[i])
			break;
	pre = total * 1024 / gc02m2_again[i];
	ret = gc02m2_write(s, GC02M2_REG_PAGE, 0);
	ret = ret ?: gc02m2_write(s, GC02M2_REG_AGAIN, i);
	ret = ret ?: gc02m2_write(s, GC02M2_REG_PREGAIN_H, pre >> 8);
	return ret ?: gc02m2_write(s, GC02M2_REG_PREGAIN_L, pre & 0xff);
}

static int gc02m2_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc02m2 *s = container_of(ctrl->handler, struct gc02m2, ctrls);
	int ret = 0, v;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* The exposure stays inside the frame */
		int max = GC02M2_HEIGHT + ctrl->val - GC02M2_EXP_MARGIN;
		__v4l2_ctrl_modify_range(s->exposure, s->exposure->minimum, max,
					 1, min(s->exposure->default_value, max));
	}
	if (!pm_runtime_get_if_in_use(&s->client->dev))
		return 0;

	ret = gc02m2_write(s, GC02M2_REG_PAGE, 0);
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ret ?: gc02m2_write(s, GC02M2_REG_EXP_H, ctrl->val >> 8 & 0x3f);
		ret = ret ?: gc02m2_write(s, GC02M2_REG_EXP_L, ctrl->val & 0xff);
		break;
	case V4L2_CID_VBLANK:
		v = GC02M2_HEIGHT + ctrl->val;
		ret = ret ?: gc02m2_write(s, GC02M2_REG_VTS_H, v >> 8 & 0x3f);
		ret = ret ?: gc02m2_write(s, GC02M2_REG_VTS_L, v & 0xff);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ret ?: gc02m2_set_gain(s, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		v = gc02m2_read(s, GC02M2_REG_FLIP);
		if (v < 0) {
			ret = v;
			break;
		}
		v &= ~0x03;
		v |= s->hflip->val ? 0x01 : 0;
		v |= s->vflip->val ? 0x02 : 0;
		ret = ret ?: gc02m2_write(s, GC02M2_REG_FLIP, v);
		break;
	default:
		ret = -EINVAL;
	}
	pm_runtime_put(&s->client->dev);
	return ret;
}

static const struct v4l2_ctrl_ops gc02m2_ctrl_ops = {
	.s_ctrl = gc02m2_set_ctrl,
};

static int gc02m2_s_stream(struct v4l2_subdev *sd, int on)
{
	struct gc02m2 *s = to_gc02m2(sd);
	struct device *dev = &s->client->dev;
	int ret = 0;
	unsigned int i;

	if (!on) {
		gc02m2_write(s, GC02M2_REG_PAGE, 0);
		gc02m2_write(s, GC02M2_REG_MODE, GC02M2_MODE_STANDBY);
		pm_runtime_put(dev);
		return 0;
	}
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(gc02m2_init) && !ret; i++)
		ret = gc02m2_write(s, gc02m2_init[i].reg, gc02m2_init[i].val);
	ret = ret ?: __v4l2_ctrl_handler_setup(&s->ctrls);
	ret = ret ?: gc02m2_write(s, GC02M2_REG_PAGE, 0);
	ret = ret ?: gc02m2_write(s, GC02M2_REG_MODE, GC02M2_MODE_STREAM);
	if (ret)
		pm_runtime_put(dev);
	return ret;
}

static void gc02m2_fill_fmt(struct v4l2_mbus_framefmt *f)
{
	f->width = GC02M2_WIDTH;
	f->height = GC02M2_HEIGHT;
	f->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
}

static int gc02m2_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int gc02m2_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = GC02M2_WIDTH;
	fse->min_height = fse->max_height = GC02M2_HEIGHT;
	return 0;
}

static int gc02m2_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	/* One mode: whatever is asked, this is what there is */
	gc02m2_fill_fmt(&fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int gc02m2_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect){ 0, 0, GC02M2_WIDTH, GC02M2_HEIGHT };
		return 0;
	default:
		return -EINVAL;
	}
}

static int gc02m2_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	gc02m2_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops gc02m2_video_ops = {
	.s_stream = gc02m2_s_stream,
};

static const struct v4l2_subdev_pad_ops gc02m2_pad_ops = {
	.enum_mbus_code = gc02m2_enum_mbus_code,
	.enum_frame_size = gc02m2_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = gc02m2_set_fmt,
	.get_selection = gc02m2_get_selection,
};

static const struct v4l2_subdev_ops gc02m2_ops = {
	.video = &gc02m2_video_ops,
	.pad = &gc02m2_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc02m2_internal_ops = {
	.init_state = gc02m2_init_state,
};

static int gc02m2_power_on(struct device *dev)
{
	struct gc02m2 *s = to_gc02m2(dev_get_drvdata(dev));
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(s->supplies), s->supplies);
	if (ret)
		return ret;
	gpiod_set_value_cansleep(s->reset, 1);
	gpiod_set_value_cansleep(s->powerdown, 0);
	ret = clk_prepare_enable(s->mclk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(s->supplies), s->supplies);
		return ret;
	}
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(s->reset, 0);
	/* 8192 clock cycles before the first I2C transfer */
	usleep_range(1000, 1500);
	return 0;
}

static int gc02m2_power_off(struct device *dev)
{
	struct gc02m2 *s = to_gc02m2(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->powerdown, 1);
	gpiod_set_value_cansleep(s->reset, 1);
	clk_disable_unprepare(s->mclk);
	regulator_bulk_disable(ARRAY_SIZE(s->supplies), s->supplies);
	return 0;
}

static int gc02m2_init_controls(struct gc02m2 *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *c;
	int ret, vblank = GC02M2_VTS_DEF - GC02M2_HEIGHT;

	v4l2_ctrl_handler_init(h, 12);
	c = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				   gc02m2_link_freqs);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE, GC02M2_PIXEL_RATE,
			  GC02M2_PIXEL_RATE, 1, GC02M2_PIXEL_RATE);
	c = v4l2_ctrl_new_std(h, NULL, V4L2_CID_HBLANK,
			      GC02M2_HTS - GC02M2_WIDTH, GC02M2_HTS - GC02M2_WIDTH,
			      1, GC02M2_HTS - GC02M2_WIDTH);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, &gc02m2_ctrl_ops, V4L2_CID_VBLANK, vblank,
			  GC02M2_VTS_MAX - GC02M2_HEIGHT, 1, vblank);
	s->exposure = v4l2_ctrl_new_std(h, &gc02m2_ctrl_ops, V4L2_CID_EXPOSURE,
					GC02M2_EXP_MIN,
					GC02M2_VTS_DEF - GC02M2_EXP_MARGIN, 1,
					GC02M2_EXP_DEF);
	v4l2_ctrl_new_std(h, &gc02m2_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  GC02M2_GAIN_ONE, GC02M2_GAIN_MAX, 1, GC02M2_GAIN_ONE);
	s->hflip = v4l2_ctrl_new_std(h, &gc02m2_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	s->vflip = v4l2_ctrl_new_std(h, &gc02m2_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	ret = v4l2_fwnode_device_parse(&s->client->dev, &props);
	if (ret)
		return ret;
	v4l2_ctrl_new_fwnode_properties(h, &gc02m2_ctrl_ops, &props);
	if (h->error)
		return h->error;
	s->sd.ctrl_handler = h;
	return 0;
}

static int gc02m2_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc02m2 *s;
	unsigned int i;
	int ret, id;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;

	s->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(dev, PTR_ERR(s->mclk), "mclk\n");
	ret = clk_set_rate(s->mclk, GC02M2_MCLK_RATE);
	if (ret)
		return dev_err_probe(dev, ret, "mclk rate\n");
	s->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return dev_err_probe(dev, PTR_ERR(s->reset), "reset GPIO\n");
	s->powerdown = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(s->powerdown))
		return dev_err_probe(dev, PTR_ERR(s->powerdown), "powerdown GPIO\n");
	for (i = 0; i < ARRAY_SIZE(gc02m2_supplies); i++)
		s->supplies[i].supply = gc02m2_supplies[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(s->supplies), s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	v4l2_i2c_subdev_init(&s->sd, client, &gc02m2_ops);
	s->sd.internal_ops = &gc02m2_internal_ops;

	ret = gc02m2_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "power on\n");
	gc02m2_write(s, GC02M2_REG_PAGE, 0);
	id = gc02m2_read(s, GC02M2_REG_ID_H) << 8 | gc02m2_read(s, GC02M2_REG_ID_L);
	if (id != GC02M2_ID) {
		ret = dev_err_probe(dev, -ENODEV, "sensor ID %#x, not %#x\n",
				    id, GC02M2_ID);
		goto err_power;
	}

	ret = gc02m2_init_controls(s);
	if (ret)
		goto err_ctrls;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto err_ctrls;
	s->sd.state_lock = s->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto err_pm;
	pm_runtime_idle(dev);
	dev_info(dev, "GC02M2 found\n");
	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&s->sd);
err_entity:
	media_entity_cleanup(&s->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&s->ctrls);
err_power:
	gc02m2_power_off(dev);
	return ret;
}

static void gc02m2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc02m2 *s = to_gc02m2(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc02m2_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(gc02m2_pm_ops, gc02m2_power_off,
				 gc02m2_power_on, NULL);

static const struct of_device_id gc02m2_of_match[] = {
	{ .compatible = "galaxycore,gc02m2" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc02m2_of_match);

static struct i2c_driver gc02m2_driver = {
	.driver = {
		.name = "gc02m2",
		.of_match_table = gc02m2_of_match,
		.pm = pm_ptr(&gc02m2_pm_ops),
	},
	.probe = gc02m2_probe,
	.remove = gc02m2_remove,
};
module_i2c_driver(gc02m2_driver);

MODULE_DESCRIPTION("GalaxyCore GC02M2 camera sensor driver");
MODULE_LICENSE("GPL");
