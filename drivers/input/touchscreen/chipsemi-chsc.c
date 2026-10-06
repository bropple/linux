// SPDX-License-Identifier: GPL-2.0-only
/*
 * Chipsemi CHSC capacitive touchscreen controllers ("semi_touch" protocol).
 *
 * The controller runs its own firmware from flash; this driver never
 * flashes it. Registers sit in a 32-bit address space, sent big-endian
 * before each transfer. After reset the host clears the boot status word
 * and waits for the firmware header to carry the "CHSC" signature; from
 * then on every falling edge of the interrupt line means a 56-byte report
 * at REPORT: byte 1 holds the number of contacts, and from byte 2 come ten
 * five-byte records (x low, y low, size, y/x high nibbles, event/id).
 *
 * The protocol follows the GPL "chsc5448" driver by murdoa, itself derived
 * from Chipsemi's GPL vendor driver; checked on the Cat S22 Flip's
 * controller (VID/PID 0x06007306).
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#define CHSC_HEADER		0x20000014	/* firmware header, 12 bytes */
#define CHSC_BOOT_STATUS	0x20000018	/* cleared by the host at reset */
#define CHSC_REPORT		0x2000002c

#define CHSC_SIG_OK		0x43534843	/* "CHSC", little-endian */
#define CHSC_SIG_ERROR		0x4f525245	/* "ERRO": firmware self-check failed */

#define CHSC_MAX_CONTACTS	10
#define CHSC_RECORD_SIZE	5
#define CHSC_REPORT_SIZE	(2 + CHSC_MAX_CONTACTS * CHSC_RECORD_SIZE + 4)

/* Event nibble of a record */
#define CHSC_EV_DOWN		0x0
#define CHSC_EV_UP		0x4
#define CHSC_EV_CONTACT		0x8
#define CHSC_ID_NONE		0xf

struct chsc_ts {
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[2];
	struct input_dev *input;
	struct touchscreen_properties prop;
};

static const struct regmap_config chsc_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static int chsc_start(struct chsc_ts *ts)
{
	struct device *dev = &ts->client->dev;
	u8 header[12], zero[4] = { };
	u32 sig = 0;
	int error = 0, i;

	gpiod_set_value_cansleep(ts->reset, 1);
	msleep(30);
	gpiod_set_value_cansleep(ts->reset, 0);
	usleep_range(5000, 6000);

	error = regmap_bulk_write(ts->regmap, CHSC_BOOT_STATUS, zero, sizeof(zero));
	if (error) {
		dev_err(dev, "no answer after reset: %d\n", error);
		return error;
	}

	for (i = 0; i < 30; i++) {
		usleep_range(10000, 12000);
		error = regmap_bulk_read(ts->regmap, CHSC_HEADER, header,
					 sizeof(header));
		if (error)
			continue;
		sig = get_unaligned_le32(header + 4);
		if (sig == CHSC_SIG_OK) {
			dev_info(dev, "firmware %04x, VID/PID %08x, up after %d ms\n",
				 get_unaligned_le16(header),
				 get_unaligned_le32(header + 8), (i + 1) * 10);
			return 0;
		}
		if (sig == CHSC_SIG_ERROR) {
			dev_err(dev, "firmware self-check failed\n");
			return -EIO;
		}
	}
	dev_err(dev, "firmware did not start (last header %*ph, error %d)\n",
		(int)sizeof(header), header, error);
	return error ?: -ETIMEDOUT;
}

static void chsc_release_all(struct chsc_ts *ts)
{
	int i;

	for (i = 0; i < CHSC_MAX_CONTACTS; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_inactive(ts->input);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t chsc_irq(int irq, void *data)
{
	struct chsc_ts *ts = data;
	struct device *dev = &ts->client->dev;
	u8 buf[CHSC_REPORT_SIZE];
	unsigned long ids = 0;
	int error, i;

	error = regmap_bulk_read(ts->regmap, CHSC_REPORT, buf, sizeof(buf));
	if (error) {
		dev_err_ratelimited(dev, "report read failed: %d\n", error);
		return IRQ_HANDLED;
	}
	dev_dbg(dev, "report %*ph\n", 2 + 2 * CHSC_RECORD_SIZE, buf);

	/* Proximity, heartbeat and gesture packets: not touches */
	if (buf[0] >= 0xfc && buf[0] <= 0xfe)
		return IRQ_HANDLED;
	if ((buf[1] & 0x0f) == 0) {
		chsc_release_all(ts);
		return IRQ_HANDLED;
	}

	/* Check the whole report before acting on any of it */
	for (i = 0; i < CHSC_MAX_CONTACTS; i++) {
		const u8 *r = &buf[2 + i * CHSC_RECORD_SIZE];
		unsigned int id = r[4] & 0x0f, ev = r[4] >> 4;

		if (id == CHSC_ID_NONE)
			continue;
		if (id >= CHSC_MAX_CONTACTS || test_bit(id, &ids) ||
		    (ev != CHSC_EV_DOWN && ev != CHSC_EV_UP && ev != CHSC_EV_CONTACT)) {
			dev_warn_ratelimited(dev, "bad report %*ph\n",
					     (int)sizeof(buf), buf);
			return IRQ_HANDLED;
		}
		__set_bit(id, &ids);
	}

	for (i = 0; i < CHSC_MAX_CONTACTS; i++) {
		const u8 *r = &buf[2 + i * CHSC_RECORD_SIZE];
		unsigned int id = r[4] & 0x0f;
		bool down = (r[4] >> 4) != CHSC_EV_UP;

		if (id == CHSC_ID_NONE)
			continue;
		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);
		if (!down)
			continue;
		touchscreen_report_pos(ts->input, &ts->prop,
				       r[0] | (r[3] & 0x0f) << 8,
				       r[1] | (r[3] >> 4) << 8, true);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, r[2]);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static void chsc_power_off(void *data)
{
	struct chsc_ts *ts = data;

	gpiod_set_value_cansleep(ts->reset, 1);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static int chsc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct chsc_ts *ts;
	int error;

	if (!client->irq)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->regmap = devm_regmap_init_i2c(client, &chsc_regmap_config);
	if (IS_ERR(ts->regmap))
		return PTR_ERR(ts->regmap);

	/* Held in reset until the supplies are up */
	ts->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return dev_err_probe(dev, PTR_ERR(ts->reset), "reset GPIO\n");

	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "vio";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies),
					ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "supplies\n");

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "Chipsemi CHSC touchscreen";
	ts->input->id.bustype = BUS_I2C;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);
	if (!ts->prop.max_x || !ts->prop.max_y)
		return dev_err_probe(dev, -EINVAL,
				     "touchscreen-size-x and -y are needed\n");
	error = input_mt_init_slots(ts->input, CHSC_MAX_CONTACTS,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "power on\n");
	error = devm_add_action_or_reset(dev, chsc_power_off, ts);
	if (error)
		return error;

	error = chsc_start(ts);
	if (error)
		return error;

	error = input_register_device(ts->input);
	if (error)
		return error;

	return devm_request_threaded_irq(dev, client->irq, NULL, chsc_irq,
					 IRQF_ONESHOT, dev_name(dev), ts);
}

static int chsc_suspend(struct device *dev)
{
	struct chsc_ts *ts = dev_get_drvdata(dev);

	disable_irq(ts->client->irq);
	chsc_release_all(ts);
	gpiod_set_value_cansleep(ts->reset, 1);
	return 0;
}

static int chsc_resume(struct device *dev)
{
	struct chsc_ts *ts = dev_get_drvdata(dev);
	int error = chsc_start(ts);

	enable_irq(ts->client->irq);
	return error;
}

static DEFINE_SIMPLE_DEV_PM_OPS(chsc_pm_ops, chsc_suspend, chsc_resume);

static const struct of_device_id chsc_of_match[] = {
	{ .compatible = "chipsemi,chsc-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, chsc_of_match);

static const struct i2c_device_id chsc_id[] = {
	{ "chsc-ts" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, chsc_id);

static struct i2c_driver chsc_driver = {
	.driver = {
		.name = "chipsemi-chsc",
		.of_match_table = chsc_of_match,
		.pm = pm_sleep_ptr(&chsc_pm_ops),
	},
	.probe = chsc_probe,
	.id_table = chsc_id,
};
module_i2c_driver(chsc_driver);

MODULE_DESCRIPTION("Chipsemi CHSC touchscreen driver");
MODULE_LICENSE("GPL");
