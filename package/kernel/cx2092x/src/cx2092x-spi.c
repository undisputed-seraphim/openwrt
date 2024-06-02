/*
 * cx2092x-spi.c -- CX20921 and CX20924 SPI Audio driver
 *
 * Copyright:   (C) 2017 Conexant Systems, Inc.
 *
 * This is based on Alexander Sverdlin's CS4271 driver code.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include "cx2092x.h"

static int cx2092x_spi_probe(struct spi_device *spi)
{
	struct regmap_config config;

	config = cx2092x_regmap_config;
	config.write_flag_mask = 0x81;

	return cx2092x_dev_probe(&spi->dev,
			devm_regmap_init_spi(spi, &cx2092x_regmap_config));
}

static int cx2092x_spi_remove(struct spi_device *spi)
{
	snd_soc_unregister_codec(&spi->dev);
	return 0;
}

static const struct spi_device_id cx2092x_spi_id[] = {
	{"cx20921", 0},
	{"cx20924", 0},
	{}
};

static struct spi_driver cx2092x_spi_driver = {
	.driver = {
		.name = "cx2092x",
		.of_match_table = of_match_ptr(cx2092x_dt_ids),
	},
	.id_table = cx2092x_spi_id,
	.probe = cx2092x_spi_probe,
	.remove = cx2092x_spi_remove,
};

module_spi_driver(cx2092x_spi_driver);

MODULE_DESCRIPTION("ASoC CX2092X SPI Driver");
MODULE_AUTHOR("Simon Ho <simon.ho@conexant.com>");
MODULE_LICENSE("GPL");
