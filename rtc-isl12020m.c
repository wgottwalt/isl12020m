// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtc-isl12020m.c - Renesas ISL12020M RTC I2C driver
 * Copyright (C) 2023 Wilken Gottwalt <wilken.gottwalt@posteo.net>
 */

#include <linux/bcd.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#define INTERNAL_NAME		"isl12020m"
#define DRIVER_NAME		"rtc-"INTERNAL_NAME

#define MASK3BITS		GENMASK(2, 0)
#define MASK5BITS		GENMASK(4, 0)
#define MASK6BITS		GENMASK(5, 0)
#define MASK7BITS		GENMASK(6, 0)

#define CENTURY_LEN		100
#define MONTH_OFFSET		1

#define MILLI_DEGREE_CELCIUS	1000
#define CELCIUS0		(273 * MILLI_DEGREE_CELCIUS)
#define TEMP_MIN		(-40 * MILLI_DEGREE_CELCIUS)
#define TEMP_LCRIT		(-50 * MILLI_DEGREE_CELCIUS)
#define TEMP_MAX		(85 * MILLI_DEGREE_CELCIUS)
#define TEMP_CRIT		(90 * MILLI_DEGREE_CELCIUS)

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
#define ISL_REG_CSR_BETA	0x0D

/* ISL12020M bits  */
#define ISL_BIT_RTC_HR_MIL	(1 << 7)

#define ISL_BIT_CSR_SR_OSCF	BIT(7)
#define ISL_BIT_CSR_INT_WRTC	(1 << 6)
#define ISL_REG_CSR_BETA_TSE	BIT(7)

#define ISL_REG_TEMP_TKOL	0x28 /* bit 0-7 = lower part of 10bit temperature */
#define ISL_REG_TEMP_TKOM	0x29 /* bit 0-1 = upper part of 10bit temperature */

struct isl12020m_data {
	struct i2c_client *client;
	struct rtc_device *rtc;
	struct regmap *regmap;
	struct device *hwmon_dev;
	struct kobject *sysfs_kobj;
	bool tse;
};

static int isl12020m_tse(struct isl12020m_data *priv, bool enable)
{
	int val;
	int err;

	err = regmap_read(priv->regmap, ISL_REG_CSR_BETA, &val);
	if (err == 0) {
		val = enable ? (val | ISL_REG_CSR_BETA_TSE) : (val & ~ISL_REG_CSR_BETA_TSE);

		err = regmap_write(priv->regmap, ISL_REG_CSR_BETA, val);
		if (!err) {
			priv->tse = enable;
		} else {
			dev_warn(&priv->client->dev, "TSE register writing failed (%d)\n", err);
		}
	} else {
		dev_warn(&priv->client->dev, "TSE register reading failed (%d)\n", err);
	}

	return err;
}

static int isl12020m_read_temp(struct isl12020m_data *priv, long *val)
{
	int err = 0;
	__le16 buf;

	if (priv->tse) {
		err = regmap_bulk_read(priv->regmap, ISL_REG_TEMP_TKOL, &buf, sizeof(buf));
		if (err == 0) {
			*val = le16_to_cpu(buf);
			*val *= MILLI_DEGREE_CELCIUS / 2;
			*val -= CELCIUS0;
		}
	} else {
		err = -EOPNOTSUPP;
	}

	return err;
}

static umode_t isl12020m_hwmon_temp_is_visible(const struct isl12020m_data *priv, u32 attr,
					       int channel)
{
	umode_t err = 0444;

	switch (attr) {
	case hwmon_temp_input:
	case hwmon_temp_lcrit:
	case hwmon_temp_min:
	case hwmon_temp_max:
	case hwmon_temp_crit:
		if (channel > 0)
			err = 0;
		break;
	default:
		break;
	}

	return err;
}

static int isl12020m_hwmon_temp_read(struct isl12020m_data *priv, u32 attr, int channel, long *val)
{
	int err = 0;

	switch (attr) {
	case hwmon_temp_input:
		err = isl12020m_read_temp(priv, val);
		break;
	case hwmon_temp_lcrit:
		*val = TEMP_LCRIT;
		break;
	case hwmon_temp_min:
		*val = TEMP_MIN;
		break;
	case hwmon_temp_max:
		*val = TEMP_MAX;
		break;
	case hwmon_temp_crit:
		*val = TEMP_CRIT;
		break;
	default:
		err = -EOPNOTSUPP;
	}

	return err;
}

static umode_t isl12020m_hwmon_ops_is_visible(const void *data, enum hwmon_sensor_types type,
					      u32 attr, int channel)
{
	const struct isl12020m_data *priv = data;

	if (type == hwmon_temp)
		return isl12020m_hwmon_temp_is_visible(priv, attr, channel);

	return 0;
}

static int isl12020m_hwmon_ops_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				    int channel, long *val)
{
	struct isl12020m_data *priv = dev_get_drvdata(dev);

	if (type == hwmon_temp)
		return isl12020m_hwmon_temp_read(priv, attr, channel, val);

	return -EOPNOTSUPP;
}

static const struct hwmon_ops isl12020m_hwmon_ops = {
	.is_visible = isl12020m_hwmon_ops_is_visible,
	.read = isl12020m_hwmon_ops_read,
};

static const struct hwmon_channel_info *isl12020m_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LCRIT | HWMON_T_MIN | HWMON_T_MAX | HWMON_T_CRIT),
	NULL,
};

static const struct hwmon_chip_info isl12020m_chip_info = {
	.ops = &isl12020m_hwmon_ops,
	.info = isl12020m_info,
};

static ssize_t isl12020m_tse_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct isl12020m_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%c\n", priv->tse ? '1' : '0');
}

static ssize_t isl12020m_tse_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct isl12020m_data *priv = dev_get_drvdata(dev);
	u8 val;

	sscanf(buf, "%hhu", &val);
	if (val)
		isl12020m_tse(priv, true);
	else
		isl12020m_tse(priv, false);

	return count;
}

static struct device_attribute isl12020m_tse_dev_attr = {
        .attr = {
                .name = "temperatur_sensing_enable",
                .mode = S_IWUSR | S_IRUGO,
        },
        .show = isl12020m_tse_show,
	.store = isl12020m_tse_store,
};

static const struct attribute *isl12020m_attrs[] = {
	&isl12020m_tse_dev_attr.attr,
	NULL,
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

	tm->tm_sec = bcd2bin(regmap_buf[ISL_REG_RTC_SC] & MASK7BITS);
	tm->tm_min = bcd2bin(regmap_buf[ISL_REG_RTC_MN] & MASK7BITS);
	tm->tm_hour = bcd2bin(regmap_buf[ISL_REG_RTC_HR] & MASK6BITS);
	tm->tm_mday = bcd2bin(regmap_buf[ISL_REG_RTC_DT] & MASK6BITS);
	tm->tm_mon = bcd2bin(regmap_buf[ISL_REG_RTC_MO] & MASK5BITS) - MONTH_OFFSET;
	tm->tm_year = bcd2bin(regmap_buf[ISL_REG_RTC_YR]) + CENTURY_LEN;
	tm->tm_wday = regmap_buf[ISL_REG_RTC_DW] & MASK3BITS;

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
	regmap_buf[ISL_REG_RTC_DW] = tm->tm_wday & MASK3BITS;

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
	int initial_state;
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
		dev_err(&client->dev, "allocating rtc device failed (%d)\n", err);
		return err;
	}

	priv->rtc->ops = &isl12020m_rtc_ops;
	priv->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	priv->rtc->range_max = RTC_TIMESTAMP_END_2099;

	/* sysfs is required and should not fail */
	priv->sysfs_kobj = kobject_create_and_add(INTERNAL_NAME, &client->dev.kobj);
	if (IS_ERR(priv->sysfs_kobj)) {
		dev_err(&client->dev, "setup of sysfs failed (%ld)\n", PTR_ERR(priv->sysfs_kobj));
		goto sysfs_fail;
	}
	err = sysfs_create_files(priv->sysfs_kobj, isl12020m_attrs);
	if (err) {
		pr_err("failed to create sysfs entries (%d)\n", err);
		goto entries_fail;
	}

	/* get initial state of the rtc and check for failures, this is critical */
	err = regmap_read(priv->regmap, ISL_REG_CSR_SR, &initial_state);
	if (err) {
		dev_err(&client->dev, "failed to acquire initial status (%d)\n", err);
		goto state_fail;
	}
	if (initial_state & ISL_BIT_CSR_SR_OSCF)
		dev_warn(&client->dev, "oscillator failure detected\n");

	/* setup of hwmon failing is not critical */
	priv->hwmon_dev = hwmon_device_register_with_info(&client->dev, INTERNAL_NAME, priv,
							  &isl12020m_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		dev_warn(&client->dev, "registering hwmon device failed (%ld)\n",
			 PTR_ERR(priv->hwmon_dev));
	}

	if (device_property_present(&client->dev, "temp-sensing-enable"))
		isl12020m_tse(priv, true);

	return devm_rtc_register_device(priv->rtc);

state_fail:
	sysfs_remove_files(priv->sysfs_kobj, isl12020m_attrs);
entries_fail:
	kobject_put(priv->sysfs_kobj);
sysfs_fail:
	return err;
}

static void isl12020m_remove(struct i2c_client *client)
{
	struct isl12020m_data *priv = i2c_get_clientdata(client);

	sysfs_remove_files(priv->sysfs_kobj, isl12020m_attrs);
	kobject_put(priv->sysfs_kobj);
	hwmon_device_unregister(priv->hwmon_dev);
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
