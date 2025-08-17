/*
 * cx2092x-i2c.c -- CX20921 and CX20924 I2C Audio driver
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
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include "cx2092x.h"

static int cx2092x_i2c_probe(struct i2c_client *i2c)
{
	return cx2092x_dev_probe(&i2c->dev,
			devm_regmap_init_i2c(i2c, &cx2092x_regmap_config));
}
static void cx2092x_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_component(&client->dev);
}

static const struct i2c_device_id cx2092x_i2c_id[] = {
	{"cx20921", 0},
	{"cx20924", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, cx2092x_i2c_id);

static struct i2c_driver cx2092x_i2c_driver = {
	.driver = {
		.name = "cx2092x",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(cx2092x_dt_ids),
	},
	.id_table = cx2092x_i2c_id,
	.probe = cx2092x_i2c_probe,
	.remove = cx2092x_i2c_remove,
};
module_i2c_driver(cx2092x_i2c_driver);

MODULE_DESCRIPTION("ASoC CX2092X I2C Driver");
MODULE_AUTHOR("Simon Ho <simon.ho@conexant.com>");
MODULE_LICENSE("GPL");
