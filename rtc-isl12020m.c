// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtc-isl12020m.c - Renesas ISL12020M RTC I2C driver
 * Copyright (C) 2023 Wilken Gottwalt <wilken.gottwalt@jenoptik.com>
 */

#include <linux/bcd.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/types.h>

#define INTERNAL_NAME	"isl12020m"
#define DRIVER_NAME	"rtc-"INTERNAL_NAME

#define CENTURY_LEN		100
#define MONTH_OFFSET		1
#define WEEK_LEN		7

/* ISL12020M register offsets */
#define ISL_REG_RTC_SC		0x00 /* bit 0-6 = seconds 0-59, default 0x00 */
#define ISL_REG_RTC_MN		0x01 /* bit 0-6 = minutes 0-59, default 0x00 */
#define ISL_REG_RTC_HR		0x02 /* bit 0-5 = hours 0-23, bit 7 = 24 hour time, default 0x00 */
#define ISL_REG_RTC_DT		0x03 /* bit 0-5 = days 1-31, default 0x01 */
#define ISL_REG_RTC_MO		0x04 /* bit 0-4 = months 1-12, default 0x01 */
#define ISL_REG_RTC_YR		0x05 /* bit 0-7 = years 0-99, default 0x00 */
#define ISL_REG_RTC_DW		0x06 /* bit 0-2 = day of week 0-6, default 0x00 */

#define ISL_REG_CSR_SR		0x07
#define ISL_REG_CSR_INT		0x08

/* ISL12020M bits  */
#define ISL_BIT_RTC_HR_MIL	(1 << 7)

#define ISL_BIT_CSR_INT_WRTC	(1 << 6)

struct isl12020m_data {
	struct i2c_client *client;
	struct rtc_device *rtc;
	struct regmap *regmap;
};

static int isl12020m_rtc_ops_read_time(struct device *dev, struct rtc_time *tm)
{
	struct isl12020m_data *priv = dev_get_drvdata(dev);
	struct regmap *regmap = priv->regmap;
	uint8_t regmap_buf[ISL_REG_CSR_INT + 1];
	int err;

	err = regmap_bulk_read(regmap, ISL_REG_RTC_SC, regmap_buf, sizeof(regmap_buf));
	if (err < 0)
		return err;

	tm->tm_sec = bcd2bin(regmap_buf[ISL_REG_RTC_SC] & 0x7F);
	tm->tm_min = bcd2bin(regmap_buf[ISL_REG_RTC_MN] & 0x7F);
	tm->tm_hour = bcd2bin(regmap_buf[ISL_REG_RTC_HR] & 0x3F);
	tm->tm_mday = bcd2bin(regmap_buf[ISL_REG_RTC_DT] & 0x3F);
	tm->tm_mon = bcd2bin(regmap_buf[ISL_REG_RTC_MO] & 0x1F) - MONTH_OFFSET;
	tm->tm_year = bcd2bin(regmap_buf[ISL_REG_RTC_YR]) + CENTURY_LEN;
	tm->tm_wday = regmap_buf[ISL_REG_RTC_DW] & WEEK_LEN;

	return 0;
}

static int isl12020m_rtc_ops_set_time(struct device *dev, struct rtc_time *tm)
{
	struct isl12020m_data *priv = dev_get_drvdata(dev);
	struct regmap *regmap = priv->regmap;
	uint8_t regmap_buf[ISL_REG_RTC_DW + 1];
	int err;

	err = regmap_update_bits(regmap, ISL_REG_CSR_INT, ISL_BIT_CSR_INT_WRTC,
				 ISL_BIT_CSR_INT_WRTC);
	if (err < 0)
		return err;

	regmap_buf[ISL_REG_RTC_SC] = bin2bcd(tm->tm_sec);
	regmap_buf[ISL_REG_RTC_MN] = bin2bcd(tm->tm_min);
	regmap_buf[ISL_REG_RTC_HR] = bin2bcd(tm->tm_hour) | ISL_BIT_RTC_HR_MIL;
	regmap_buf[ISL_REG_RTC_DT] = bin2bcd(tm->tm_mday);
	regmap_buf[ISL_REG_RTC_MO] = bin2bcd(tm->tm_mon + MONTH_OFFSET);
	regmap_buf[ISL_REG_RTC_YR] = bin2bcd(tm->tm_year % CENTURY_LEN);
	regmap_buf[ISL_REG_RTC_DW] = tm->tm_wday & WEEK_LEN;

	return regmap_bulk_write(priv->regmap, ISL_REG_RTC_SC, regmap_buf, sizeof(regmap_buf));
}

static const struct rtc_class_ops isl12020m_rtc_ops = {
	.read_time = isl12020m_rtc_ops_read_time,
	.set_time = isl12020m_rtc_ops_set_time,
};

static const struct regmap_config isl12020m_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.use_single_write = true,
};

static int isl12020m_probe(struct i2c_client *client)
{
	struct isl12020m_data *priv;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	priv = devm_kzalloc(&client->dev, sizeof(struct isl12020m_data), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	dev_set_drvdata(&client->dev, priv);

	priv->regmap = devm_regmap_init_i2c(client, &isl12020m_regmap_config);
	if (IS_ERR(priv->regmap)) {
		err = PTR_ERR(priv->regmap);
		dev_err(&client->dev, "allocating regmap failed (%d)\n", err);
		return err;
	}

	priv->rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(priv->rtc)) {
		err = PTR_ERR(priv->rtc);
		dev_err(&client->dev, "allocating rtc structure failed (%d)\n", err);
		return err;
	}

	priv->rtc->ops = &isl12020m_rtc_ops;
	priv->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	priv->rtc->range_max = RTC_TIMESTAMP_END_2099;

	return devm_rtc_register_device(priv->rtc);
}

static void isl12020m_remove(struct i2c_client *client)
{
	// TODO: hwmon, sysfs, etc
}

static const struct of_device_id isl12020m_of_match_table[] = {
	{ .compatible = "renesas,isl12020m" },
	{ .compatible = "isil,isl12020m" },
	{ },
};
MODULE_DEVICE_TABLE(of, isl12020m_of_match_table);

static const struct i2c_device_id isl12020m_id[] = {
	{ "isl12020mirz", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, isl12020m_id);

static struct i2c_driver isl12020m_driver = {
	.driver	= {
		.name = DRIVER_NAME,
		.of_match_table = isl12020m_of_match_table,
	},
	.probe = isl12020m_probe,
	.remove = isl12020m_remove,
	.id_table = isl12020m_id,
};
module_i2c_driver(isl12020m_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Wilken Gottwalt <wilken.gottwalt@posteo.net>");
MODULE_DESCRIPTION("Renesas ISL12020M RTC I2C driver");
