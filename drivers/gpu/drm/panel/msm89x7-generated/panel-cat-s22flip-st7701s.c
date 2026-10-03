// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 bropple
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved.
// ST7701S 480x640 video mode panel of the Cat S22 Flip
// (stock node qcom,mdss_dsi_st7701s_vga_video).

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct st7701s_vga {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

static const struct regulator_bulk_data st7701s_vga_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "vddio" },
};

static inline struct st7701s_vga *to_st7701s_vga(struct drm_panel *panel)
{
	return container_of_const(panel, struct st7701s_vga, panel);
}

static void st7701s_vga_reset(struct st7701s_vga *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(120);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);
}

static int st7701s_vga_on(struct st7701s_vga *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x13);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xef, 0x08);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x10);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc0, 0x4f, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc1, 0x10, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc2, 0x07, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xcc, 0x10);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0,
					 0x00, 0x10, 0x17, 0x0d, 0x11, 0x06,
					 0x05, 0x08, 0x07, 0x1f, 0x04, 0x11,
					 0x0e, 0x29, 0x30, 0x1f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1,
					 0x00, 0x0d, 0x14, 0x0e, 0x11, 0x06,
					 0x04, 0x08, 0x08, 0x20, 0x05, 0x13,
					 0x13, 0x26, 0x30, 0x1f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x11);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x65);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1, 0x82);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2, 0x82);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb3, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb5, 0x42);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb7, 0x85);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb8, 0x20);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc1, 0x78);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc2, 0x78);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd0, 0x88);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xee, 0x42);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe0, 0x00, 0x00, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe1,
					 0x04, 0xa0, 0x06, 0xa0, 0x05, 0xa0,
					 0x07, 0xa0, 0x00, 0x44, 0x44);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe2,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe3,
					 0x00, 0x00, 0x22, 0x22);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe4, 0x44, 0x44);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe5,
					 0x0c, 0x90, 0xa0, 0xa0, 0x0e, 0x92,
					 0xa0, 0xa0, 0x08, 0x8c, 0xa0, 0xa0,
					 0x0a, 0x8e, 0xa0, 0xa0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe6,
					 0x00, 0x00, 0x22, 0x22);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7, 0x44, 0x44);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe8,
					 0x0d, 0x91, 0xa0, 0xa0, 0x0f, 0x93,
					 0xa0, 0xa0, 0x09, 0x8d, 0xa0, 0xa0,
					 0x0b, 0x8f, 0xa0, 0xa0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xeb,
					 0x00, 0x00, 0xe4, 0xe4, 0x44, 0x00,
					 0x40);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xed,
					 0xff, 0xf5, 0x47, 0x6f, 0x0b, 0xa1,
					 0xab, 0xff, 0xff, 0xba, 0x1a, 0xb0,
					 0xf6, 0x74, 0x5f, 0xff);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xef,
					 0x08, 0x08, 0x08, 0x40, 0x3f, 0x64);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x13);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe6, 0x16, 0x7c);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe8, 0x00, 0x0e);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x13);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe8, 0x00, 0x0c);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe8, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff,
					 0x77, 0x01, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x35, 0x00);

	return dsi_ctx.accum_err;
}

static int st7701s_vga_off(struct st7701s_vga *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int st7701s_vga_prepare(struct drm_panel *panel)
{
	struct st7701s_vga *ctx = to_st7701s_vga(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(st7701s_vga_supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	st7701s_vga_reset(ctx);

	ret = st7701s_vga_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(st7701s_vga_supplies), ctx->supplies);
		return ret;
	}

	return 0;
}

static int st7701s_vga_unprepare(struct drm_panel *panel)
{
	struct st7701s_vga *ctx = to_st7701s_vga(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = st7701s_vga_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(st7701s_vga_supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode st7701s_vga_mode = {
	.clock = (480 + 160 + 20 + 60) * (640 + 44 + 4 + 82) * 60 / 1000,
	.hdisplay = 480,
	.hsync_start = 480 + 160,
	.hsync_end = 480 + 160 + 20,
	.htotal = 480 + 160 + 20 + 60,
	.vdisplay = 640,
	.vsync_start = 640 + 44,
	.vsync_end = 640 + 44 + 4,
	.vtotal = 640 + 44 + 4 + 82,
	.width_mm = 43,
	.height_mm = 58,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int st7701s_vga_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &st7701s_vga_mode);
}

static const struct drm_panel_funcs st7701s_vga_panel_funcs = {
	.prepare = st7701s_vga_prepare,
	.unprepare = st7701s_vga_unprepare,
	.get_modes = st7701s_vga_get_modes,
};

static int st7701s_vga_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct st7701s_vga *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct st7701s_vga, panel,
				   &st7701s_vga_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(st7701s_vga_supplies),
					    st7701s_vga_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_VIDEO_NO_HFP;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void st7701s_vga_remove(struct mipi_dsi_device *dsi)
{
	struct st7701s_vga *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id st7701s_vga_of_match[] = {
	{ .compatible = "cat,s22flip-st7701s" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, st7701s_vga_of_match);

static struct mipi_dsi_driver st7701s_vga_driver = {
	.probe = st7701s_vga_probe,
	.remove = st7701s_vga_remove,
	.driver = {
		.name = "panel-st7701s-vga",
		.of_match_table = st7701s_vga_of_match,
	},
};
module_mipi_dsi_driver(st7701s_vga_driver);

MODULE_AUTHOR("bropple <36946060+bropple@users.noreply.github.com>");
MODULE_DESCRIPTION("DRM driver for the Cat S22 Flip ST7701S panel");
MODULE_LICENSE("GPL");
