// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 InvenSense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/property.h>

#include "inv_icm42600.h"

static int inv_icm42600_spi_bus_setup(struct inv_icm42600_state *st)
{
	unsigned int mask, val;
	int ret;

	/* setup interface registers */
	val = INV_ICM42600_INTF_CONFIG6_I3C_EN |
	      INV_ICM42600_INTF_CONFIG6_I3C_SDR_EN |
	      INV_ICM42600_INTF_CONFIG6_I3C_DDR_EN;
	ret = regmap_update_bits(st->map, INV_ICM42600_REG_INTF_CONFIG6,
				 INV_ICM42600_INTF_CONFIG6_MASK, val);
	if (ret)
		return ret;

	ret = regmap_clear_bits(st->map, INV_ICM42600_REG_INTF_CONFIG4,
				INV_ICM42600_INTF_CONFIG4_I3C_BUS_ONLY);
	if (ret)
		return ret;

	/* set slew rates for I2C and SPI */
	mask = INV_ICM42600_DRIVE_CONFIG_I2C_MASK |
	       INV_ICM42600_DRIVE_CONFIG_SPI_MASK;
	val = INV_ICM42600_DRIVE_CONFIG_I2C(INV_ICM42600_SLEW_RATE_20_60NS) |
	      INV_ICM42600_DRIVE_CONFIG_SPI(INV_ICM42600_SLEW_RATE_INF_2NS);
	ret = regmap_update_bits(st->map, INV_ICM42600_REG_DRIVE_CONFIG,
				 mask, val);
	if (ret)
		return ret;

	/* disable i2c bus */
	return regmap_update_bits(st->map, INV_ICM42600_REG_INTF_CONFIG0,
				  INV_ICM42600_INTF_CONFIG0_UI_SIFS_CFG_MASK,
				  INV_ICM42600_INTF_CONFIG0_UI_SIFS_CFG_I2C_DIS);
}

static int inv_icm42600_probe(struct spi_device *spi)
{
	const void *match;
	enum inv_icm42600_chip chip;
	struct regmap *regmap;

	match = device_get_match_data(&spi->dev);
	if (!match)
		return -EINVAL;
	chip = (uintptr_t)match;

	/* use SPI specific regmap */
	regmap = devm_regmap_init_spi(spi, &inv_icm42600_spi_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return inv_icm42600_core_probe(regmap, chip, inv_icm42600_spi_bus_setup);
}

/*
 * device id table is used to identify what device can be
 * supported by this driver
 */
static const struct spi_device_id inv_icm42600_id[] = {
	{ "icm42600", INV_CHIP_ICM42600 },
	{ "icm42602", INV_CHIP_ICM42602 },
	{ "icm42605", INV_CHIP_ICM42605 },
	{ "icm42686", INV_CHIP_ICM42686 },
	{ "icm42622", INV_CHIP_ICM42622 },
	{ "icm42688", INV_CHIP_ICM42688 },
	{ "icm42631", INV_CHIP_ICM42631 },
	{ }
};
MODULE_DEVICE_TABLE(spi, inv_icm42600_id);

static const struct of_device_id inv_icm42600_of_matches[] = {
	{
		.compatible = "invensense,icm42600",
		.data = (void *)INV_CHIP_ICM42600,
	}, {
		.compatible = "invensense,icm42602",
		.data = (void *)INV_CHIP_ICM42602,
	}, {
		.compatible = "invensense,icm42605",
		.data = (void *)INV_CHIP_ICM42605,
	}, {
		.compatible = "invensense,icm42686",
		.data = (void *)INV_CHIP_ICM42686,
	}, {
		.compatible = "invensense,icm42622",
		.data = (void *)INV_CHIP_ICM42622,
	}, {
		.compatible = "invensense,icm42688",
		.data = (void *)INV_CHIP_ICM42688,
	}, {
		.compatible = "invensense,icm42631",
		.data = (void *)INV_CHIP_ICM42631,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, inv_icm42600_of_matches);

static struct spi_driver inv_icm42600_driver = {
	.driver = {
		.name = "inv-icm42600-spi",
		.of_match_table = inv_icm42600_of_matches,
		.pm = pm_ptr(&inv_icm42600_pm_ops),
	},
	.id_table = inv_icm42600_id,
	.probe = inv_icm42600_probe,
};
module_spi_driver(inv_icm42600_driver);

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-426xx SPI driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_ICM42600");
