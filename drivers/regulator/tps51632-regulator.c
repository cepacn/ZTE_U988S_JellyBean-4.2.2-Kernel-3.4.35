/*
 * tps51632-regulator.c -- TI tps51632
 *
 * Regulator driver for TPS51632 3-2-1 Phase D-Cap Step Down Driverless
 * Controller with serial VID control and DVFS.
 *
 * Copyright (c) 2012, NVIDIA Corporation.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/tps51632-regulator.h>
#include <linux/slab.h>

/* Register definitions */
#define TPS51632_VOLTAGE_SELECT_REG		0x0
#define TPS51632_VOLTAGE_BASE_REG		0x1
#define TPS51632_OFFSET_REG			0x2
#define TPS51632_IMON_REG			0x3
#define TPS51632_VMAX_REG			0x4
#define TPS51632_DVFS_CONTROL_REG		0x5
#define TPS51632_POWER_STATE_REG		0x6
#define TPS51632_SLEW_REGS			0x7
#define TPS51632_FAULT_REG			0x14

#define TPS51632_MAX_REG			0x15

#define TPS51632_VOUT_MASK		0x7F
#define TPS51632_VOUT_OFFSET_MASK	0x1F
#define TPS51632_VMAX_MASK		0x7F
#define TPS51632_VMAX_LOCK		0x80

/* TPS51632_DVFS_CONTROL_REG */
#define TPS51632_DVFS_PWMEN		0x1
#define TPS51632_DVFS_STEP_20		0x2
#define TPS51632_DVFS_VMAX_PG		0x4
#define TPS51632_DVFS_PWMRST		0x8
#define TPS51632_DVFS_OCA_EN		0x10
#define TPS51632_DVFS_FCCM		0x20

/* TPS51632_POWER_STATE_REG */
#define TPS51632_POWER_STATE_MASK		0x03
#define TPS51632_POWER_STATE_MULTI_PHASE_CCM	0x0
#define TPS51632_POWER_STATE_SINGLE_PHASE_CCM	0x1
#define TPS51632_POWER_STATE_SINGLE_PHASE_DCM	0x2

#define TPS51632_MIN_VOLATGE	500000
#define TPS51632_MAX_VOLATGE	1520000
#define TPS51632_VOLATGE_STEP	10000
#define TPS51632_MAX_SEL	0x7F

/* TPS51632 chip information */
struct tps51632_chip {
	struct device *dev;
	struct regulator_desc desc;
	struct regulator_dev *rdev;
	struct regmap *regmap;

	bool pwm_enabled;
	unsigned int change_uv_per_us;
	int shutdown_complete;
	struct mutex mutex;
};

static int tps51632_dcdc_get_voltage_sel(struct regulator_dev *rdev)
{
	struct tps51632_chip *tps = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret;
	unsigned int reg = TPS51632_VOLTAGE_SELECT_REG;

	mutex_lock(&tps->mutex);
	if (tps && tps->shutdown_complete) {
		mutex_unlock(&tps->mutex);
		return -ENODEV;
	}
	if (tps->pwm_enabled)
		reg = TPS51632_VOLTAGE_BASE_REG;
	ret = regmap_read(tps->regmap, reg, &data);
	if (ret < 0) {
		dev_err(tps->dev, "reg read failed, err %d\n", ret);
		mutex_unlock(&tps->mutex);
		return ret;
	}
	mutex_unlock(&tps->mutex);
	return data & TPS51632_VOUT_MASK;
}

static int tps51632_dcdc_set_voltage(struct regulator_dev *rdev,
	     int min_uV, int max_uV, unsigned *selector)
{
	struct tps51632_chip *tps = rdev_get_drvdata(rdev);
	unsigned int reg = TPS51632_VOLTAGE_SELECT_REG;
	int vsel;
	int ret;

	mutex_lock(&tps->mutex);
	if (tps && tps->shutdown_complete) {
		mutex_unlock(&tps->mutex);
		return -ENODEV;
	}

	if ((max_uV < min_uV) || (max_uV < TPS51632_MIN_VOLATGE) ||
			(min_uV > TPS51632_MAX_VOLATGE)) {
		mutex_unlock(&tps->mutex);
		return -EINVAL;
	}

	vsel = DIV_ROUND_UP(min_uV - TPS51632_MIN_VOLATGE,
			TPS51632_VOLATGE_STEP) + 0x19;
	if (selector)
		*selector = (vsel & TPS51632_VOUT_MASK);

	if (tps->pwm_enabled)
		reg = TPS51632_VOLTAGE_BASE_REG;

	ret = regmap_write(tps->regmap, reg, vsel);
	if (ret < 0)
		dev_err(tps->dev, "reg write failed, err %d\n", ret);
	mutex_unlock(&tps->mutex);
	return ret;
}

static int tps51632_dcdc_list_voltage(struct regulator_dev *rdev,
					unsigned selector)
{
	if (selector > TPS51632_MAX_SEL)
		return -EINVAL;

	return TPS51632_MIN_VOLATGE + (selector - 0x19) * TPS51632_VOLATGE_STEP;
}

static int tps51632_dcdc_set_voltage_time_sel(struct regulator_dev *rdev,
		unsigned int old_selector, unsigned int new_selector)
{
	struct tps51632_chip *tps = rdev_get_drvdata(rdev);
	int old_uV, new_uV;

	old_uV = tps51632_dcdc_list_voltage(rdev, old_selector);
	if (old_uV < 0)
		return old_uV;

	new_uV = tps51632_dcdc_list_voltage(rdev, new_selector);
	if (new_uV < 0)
		return new_uV;

	return DIV_ROUND_UP(abs(old_uV - new_uV), tps->change_uv_per_us);
}

static int tps51632_dcdc_set_control_mode(struct regulator_dev *rdev,
		unsigned int mode)
{
	struct tps51632_chip *tps = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&tps->mutex);
	if (tps && tps->shutdown_complete) {
		mutex_unlock(&tps->mutex);
		return -ENODEV;
	}
	if (mode == REGULATOR_CONTROL_MODE_I2C)
	     ret = regmap_update_bits(tps->regmap,
			TPS51632_DVFS_CONTROL_REG, TPS51632_DVFS_PWMEN, 0);
	else if (mode == REGULATOR_CONTROL_MODE_PWM)
	      ret = regmap_update_bits(tps->regmap,
			TPS51632_DVFS_CONTROL_REG, TPS51632_DVFS_PWMEN,
			TPS51632_DVFS_PWMEN);
	else {
		mutex_unlock(&tps->mutex);
		return -EINVAL;
	}
	if (ret < 0) {
		dev_err(tps->dev, "DVFS reg write failed, err %d\n", ret);
		mutex_unlock(&tps->mutex);
		return ret;
	}
	tps->pwm_enabled = (mode == REGULATOR_CONTROL_MODE_PWM);
	mutex_unlock(&tps->mutex);
	return ret;
}

static unsigned int tps51632_dcdc_get_control_mode(struct regulator_dev *rdev)
{
	struct tps51632_chip *tps = rdev_get_drvdata(rdev);

	if (tps->pwm_enabled)
		return REGULATOR_CONTROL_MODE_PWM;
	return REGULATOR_CONTROL_MODE_I2C;
}


static struct regulator_ops tps51632_dcdc_ops = {
	.get_voltage_sel	= tps51632_dcdc_get_voltage_sel,
	.set_voltage		= tps51632_dcdc_set_voltage,
	.list_voltage		= tps51632_dcdc_list_voltage,
	.set_voltage_time_sel	= tps51632_dcdc_set_voltage_time_sel,
	.set_control_mode	= tps51632_dcdc_set_control_mode,
	.get_control_mode	= tps51632_dcdc_get_control_mode,
};

static int __devinit tps51632_init_dcdc(struct tps51632_chip *tps,
		struct tps51632_regulator_platform_data *pdata)
{
	int ret;
	uint8_t	control = 0;
	int vsel;
	unsigned int vmax;

	mutex_lock(&tps->mutex);
	if (tps && tps->shutdown_complete) {
		mutex_unlock(&tps->mutex);
		return -ENODEV;
	}
	if (pdata->enable_pwm) {
		control |= TPS51632_DVFS_PWMEN;
		tps->pwm_enabled = pdata->enable_pwm;
	}
	vsel = DIV_ROUND_UP(pdata->base_voltage_uV -
		TPS51632_MIN_VOLATGE, TPS51632_VOLATGE_STEP) + 0x19;
	ret = regmap_write(tps->regmap, TPS51632_VOLTAGE_BASE_REG, vsel);
	if (ret < 0) {
		dev_err(tps->dev, "BASE reg write failed, err %d\n", ret);
		mutex_unlock(&tps->mutex);
		return ret;
	}
	if (pdata->dvfs_step_20mV)
		control |= TPS51632_DVFS_STEP_20;
	if (pdata->enable_vmax_alarm)
		control |= TPS51632_DVFS_VMAX_PG;
	if (pdata->enable_overcurrent_alram)
		control |= TPS51632_DVFS_OCA_EN;
	if (pdata->max_voltage_uV) {
		/**
		 * TPS51632 hw behavior: VMAX register can be write only
		 * once as it get locked after first write. The lock get
		 * reset only when device is power-reset.
		 * Write register only when lock bit is not enabled.
		 */
		ret = regmap_read(tps->regmap, TPS51632_VMAX_REG, &vmax);
		if (ret < 0) {
			dev_err(tps->dev, "VMAX read failed, err %d\n", ret);
			mutex_unlock(&tps->mutex);
			return ret;
		}
		if (vmax & TPS51632_VMAX_LOCK)
			goto skip_vmax_config;

		vsel = DIV_ROUND_UP(pdata->max_voltage_uV -
			TPS51632_MIN_VOLATGE, TPS51632_VOLATGE_STEP) + 0x19;
		ret = regmap_write(tps->regmap, TPS51632_VMAX_REG, vsel);
		if (ret < 0) {
			dev_err(tps->dev, "VMAX write failed, err %d\n", ret);
			mutex_unlock(&tps->mutex);
			return ret;
		}
	}

skip_vmax_config:
	ret = regmap_write(tps->regmap, TPS51632_DVFS_CONTROL_REG, control);
	if (ret < 0) {
		dev_err(tps->dev, "DVFS reg write failed, err %d\n", ret);
		mutex_unlock(&tps->mutex);
		return ret;
	}

	tps->change_uv_per_us = max(6000u, pdata->slew_rate_uv_per_us);

	vsel = BIT(tps->change_uv_per_us/6000 - 1);

	ret = regmap_write(tps->regmap, TPS51632_SLEW_REGS, vsel);
	if (ret < 0)
		dev_err(tps->dev, "SLEW reg write failed, err %d\n", ret);
	mutex_unlock(&tps->mutex);
	return ret;
}

static const struct regmap_config tps51632_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.max_register		= TPS51632_MAX_REG - 1,
	.cache_type		= REGCACHE_RBTREE,
};

static void tps51632_shutdown(struct i2c_client *client)
{
	struct tps51632_chip *tps = i2c_get_clientdata(client);

	mutex_lock(&tps->mutex);
	tps->shutdown_complete = 1;
	mutex_unlock(&tps->mutex);

}
static int __devinit tps51632_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct tps51632_regulator_platform_data *pdata;
	struct regulator_dev *rdev;
	struct tps51632_chip *tps;
	int ret;

	pdata = client->dev.platform_data;
	if (!pdata) {
		dev_err(&client->dev, "No Platform data\n");
		return -EINVAL;
	}

	tps = devm_kzalloc(&client->dev, sizeof(*tps), GFP_KERNEL);
	if (!tps) {
		dev_err(&client->dev, "Memory allocation failed\n");
		return -ENOMEM;
	}

	tps->dev = &client->dev;
	tps->desc.name = id->name;
	tps->desc.id = 0;
	tps->desc.ops = &tps51632_dcdc_ops;
	tps->desc.type = REGULATOR_VOLTAGE;
	tps->desc.owner = THIS_MODULE;
	tps->regmap = devm_regmap_init_i2c(client, &tps51632_regmap_config);
	if (IS_ERR(tps->regmap)) {
		ret = PTR_ERR(tps->regmap);
		dev_err(&client->dev, "regmap init failed, err %d\n", ret);
		return ret;
	}
	i2c_set_clientdata(client, tps);

	mutex_init(&tps->mutex);
	ret = tps51632_init_dcdc(tps, pdata);
	if (ret < 0) {
		dev_err(tps->dev, "Init failed, err = %d\n", ret);
		return ret;
	}

	/* Register the regulators */
	rdev = regulator_register(&tps->desc, &client->dev,
			pdata->reg_init_data, tps, NULL);
	if (IS_ERR(rdev)) {
		dev_err(tps->dev, "regulator register failed\n");
		return PTR_ERR(rdev);
	}

	tps->shutdown_complete = 0;
	tps->rdev = rdev;
	return 0;
}

static int __devexit tps51632_remove(struct i2c_client *client)
{
	struct tps51632_chip *tps = i2c_get_clientdata(client);

	mutex_destroy(&tps->mutex);
	regulator_unregister(tps->rdev);
	return 0;
}

static const struct i2c_device_id tps51632_id[] = {
	{.name = "tps51632",},
	{},
};

MODULE_DEVICE_TABLE(i2c, tps51632_id);

static struct i2c_driver tps51632_i2c_driver = {
	.driver = {
		.name = "tps51632",
		.owner = THIS_MODULE,
	},
	.probe = tps51632_probe,
	.shutdown = tps51632_shutdown,
	.remove = __devexit_p(tps51632_remove),
	.id_table = tps51632_id,
};

static int __init tps51632_init(void)
{
	return i2c_add_driver(&tps51632_i2c_driver);
}
subsys_initcall(tps51632_init);

static void __exit tps51632_cleanup(void)
{
	i2c_del_driver(&tps51632_i2c_driver);
}
module_exit(tps51632_cleanup);

MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_DESCRIPTION("TPS51632 voltage regulator driver");
MODULE_LICENSE("GPL v2");