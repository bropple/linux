// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_SOFT_RB_SPARE		0x8f
/*
 * Spare byte that survives a reset but that the stock Qualcomm bootloader
 * neither reads nor scrubs (it scrubs SOFT_RB_SPARE's reason bits). Our
 * lk2nd checks it for LK2ND_FASTBOOT_MAGIC, clears it and stays in its
 * fastboot: a one-shot "next boot: lk2nd fastboot" switch.
 */
#define PON_DVDD_RB_SPARE		0x8d
#define LK2ND_FASTBOOT_MAGIC		0x6c	/* 'l' */

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

struct qcom_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
};

static int qcom_pon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_pon *pon = container_of
			(reboot, struct qcom_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static ssize_t lk2nd_fastboot_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct qcom_pon *pon = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(pon->regmap, pon->baseaddr + PON_DVDD_RB_SPARE, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", val == LK2ND_FASTBOOT_MAGIC);
}

static ssize_t lk2nd_fastboot_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct qcom_pon *pon = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	ret = regmap_write(pon->regmap, pon->baseaddr + PON_DVDD_RB_SPARE,
			   on ? LK2ND_FASTBOOT_MAGIC : 0);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(lk2nd_fastboot);

static int qcom_pon_probe(struct platform_device *pdev)
{
	struct qcom_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = qcom_pon_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	platform_set_drvdata(pdev, pon);

	if (reason_shift == GEN1_REASON_SHIFT) {
		unsigned int val;

		/* One-shot: never let the lk2nd switch outlive a boot */
		if (!regmap_read(pon->regmap,
				 pon->baseaddr + PON_DVDD_RB_SPARE, &val) &&
		    val == LK2ND_FASTBOOT_MAGIC) {
			dev_info(&pdev->dev, "lk2nd fastboot switch was set; cleared\n");
			regmap_write(pon->regmap,
				     pon->baseaddr + PON_DVDD_RB_SPARE, 0);
		}

		error = device_create_file(&pdev->dev, &dev_attr_lk2nd_fastboot);
		if (error)
			dev_warn(&pdev->dev, "no lk2nd_fastboot attribute: %d\n",
				 error);
	}

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id qcom_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pon_id_table);

static struct platform_driver qcom_pon_driver = {
	.probe = qcom_pon_probe,
	.driver = {
		.name = "qcom-pon",
		.of_match_table = qcom_pon_id_table,
	},
};
module_platform_driver(qcom_pon_driver);

MODULE_DESCRIPTION("Qualcomm Power On driver");
MODULE_LICENSE("GPL v2");
