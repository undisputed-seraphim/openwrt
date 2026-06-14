/*
 * Driver for the MCU (I2C bus)
 *
 * Copyright (C) 2018 Wistron Corporation.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>

#define MCU_APROM_ADDR		0x15
#define MCU_LDROM_ADDR		0x36

#define MCU_REG_LED		0x00
#define MCU_REG_LED_SPEED	0x01
#define MCU_REG_FAN		0x02
#define MCU_REG_READ_FAN	0x03
#define MCU_REG_READ_TEMPER1	0x04
#define MCU_REG_READ_TEMPER2	0x05
#define MCU_REG_PWM_LED		0x06
#define MCU_REG_READ_FW_VER	0x09
#define MCU_REG_LED_RED		0x0A
#define MCU_REG_LED_GREEN	0x0B
#define MCU_REG_LED_BLUE	0x0C
#define MCU_REG_PWM_LED_DUTY	0x0D
#define MCU_REG_BOOTLOADER_MODE	0x0F
#define MCU_REG_LED_F_RED	0x10
#define MCU_REG_LED_F_GREEN	0x11
#define MCU_REG_LED_F_BLUE	0x12

#define MCU_DATA_BOOTLOADER_MODE 0x1E

#define CMD_UPDATE_APROM	0xA0
#define CMD_READ_CONFIG		0xA2
#define CMD_SYNC_PACKNO		0xA4
#define CMD_GET_FWVER		0xA6
#define CMD_GET_DEVICEID	0xB1

#define PACKET_SIZE		64
#define BOOT_UPDATE_FIRMWARE_NAME "mcu_025.bin"

struct mcu_data {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *boot_gpio;
	struct mutex lock;
	/* firmware update state */
	unsigned int g_packno;
	u8 sendbuf[PACKET_SIZE];
	u8 rcvbuf[PACKET_SIZE];
	u8 fw_buf[512];
};

static int mcu_i2c_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2];
	int ret;
	int retries = 0;

	msgs[0].flags = 0;
	msgs[0].addr = MCU_APROM_ADDR;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr = MCU_APROM_ADDR;
	msgs[1].len = 1;
	msgs[1].buf = val;

	while (retries < 5) {
		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret == 2)
			return 0;
		retries++;
	}
	return ret;
}

static int mcu_i2c_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	struct i2c_msg msg;
	u8 buf[2] = { reg, val };
	int ret;
	int retries = 0;

	msg.flags = 0;
	msg.addr = MCU_APROM_ADDR;
	msg.len = 2;
	msg.buf = buf;

	while (retries < 5) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		retries++;
	}
	return ret;
}

static int mcu_ldrom_write(struct i2c_client *client, const u8 *data, u8 len)
{
	struct i2c_msg msg;
	int ret;
	int retries = 0;

	msg.flags = 0;
	msg.addr = MCU_LDROM_ADDR;
	msg.len = len;
	msg.buf = (u8 *)data;

	while (retries < 5) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		retries++;
	}
	return ret;
}

static int mcu_ldrom_read(struct i2c_client *client, u8 *buf, u8 len)
{
	struct i2c_msg msg;
	int ret;
	int retries = 0;

	msg.flags = I2C_M_RD;
	msg.addr = MCU_LDROM_ADDR;
	msg.len = len;
	msg.buf = buf;

	while (retries < 5) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		retries++;
	}
	return ret;
}

static u16 mcu_checksum(const u8 *buf, int len)
{
	int i;
	u16 c;

	for (c = 0, i = 0; i < len; i++)
		c += buf[i];
	return c;
}

static void mcu_enter_bootloader(struct mcu_data *mcu)
{
	gpiod_set_value(mcu->boot_gpio, 1);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 1);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 0);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 1);
	msleep(100);
}

static void mcu_leave_bootloader(struct mcu_data *mcu)
{
	gpiod_set_value(mcu->boot_gpio, 0);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 1);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 0);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 1);
	msleep(100);
}

static int mcu_ldrom_send(struct mcu_data *mcu)
{
	return mcu_ldrom_write(mcu->client, mcu->sendbuf, PACKET_SIZE);
}

static int mcu_ldrom_recv(struct mcu_data *mcu)
{
	u16 cksum, rcv_cksum;
	u32 rcv_packno;
	int ret;

	ret = mcu_ldrom_read(mcu->client, mcu->rcvbuf, PACKET_SIZE);
	if (ret)
		return ret;

	memcpy(&rcv_cksum, mcu->rcvbuf, 2);
	memcpy(&rcv_packno, mcu->rcvbuf + 4, 4);

	if (rcv_packno != mcu->g_packno) {
		dev_info(&mcu->client->dev,
			 "g_packno=%d rcv=%d\n", mcu->g_packno, rcv_packno);
		return -EIO;
	}

	cksum = mcu_checksum(mcu->sendbuf, PACKET_SIZE);
	if (rcv_cksum != cksum) {
		dev_info(&mcu->client->dev,
			 "cksum mismatch: sent=0x%x recv=0x%x\n", cksum, rcv_cksum);
		return -EIO;
	}

	mcu->g_packno++;
	return 0;
}

static int mcu_ldrom_cmd(struct mcu_data *mcu, u32 cmd,
			 const u32 *args, int nargs,
			 u32 *resp, int nresp)
{
	int ret, i;

	memset(mcu->sendbuf, 0, PACKET_SIZE);
	memcpy(mcu->sendbuf, &cmd, 4);
	memcpy(mcu->sendbuf + 4, &mcu->g_packno, 4);
	mcu->g_packno++;
	for (i = 0; i < nargs; i++)
		memcpy(mcu->sendbuf + 8 + i * 4, &args[i], 4);

	ret = mcu_ldrom_send(mcu);
	if (ret)
		return ret;
	msleep(100);

	ret = mcu_ldrom_recv(mcu);
	if (ret)
		return ret;

	for (i = 0; i < nresp; i++)
		memcpy(&resp[i], mcu->rcvbuf + 8 + i * 4, 4);

	return 0;
}

static int mcu_fw_update(struct mcu_data *mcu, const char *filename)
{
	const struct firmware *fw = NULL;
	unsigned int i, j, k, ct, l, bin_cksum;
	u32 devid, config[2], fwver, startaddr;
	u16 get_cksum;
	int ret;

	ret = request_firmware(&fw, filename, &mcu->client->dev);
	if (ret) {
		dev_warn(&mcu->client->dev,
			 "firmware \"%s\" not available for update\n", filename);
		return ret;
	}

	dev_info(&mcu->client->dev, "firmware \"%s\" loaded, size=%zu\n",
		 filename, fw->size);

	mutex_lock(&mcu->lock);
	mcu->g_packno = 1;

	mcu_enter_bootloader(mcu);

	mcu->sendbuf[0] = MCU_REG_BOOTLOADER_MODE;
	mcu->sendbuf[1] = MCU_DATA_BOOTLOADER_MODE;
	ret = mcu_i2c_write_reg(mcu->client, MCU_REG_BOOTLOADER_MODE,
				MCU_DATA_BOOTLOADER_MODE);
	if (ret < 0) {
		dev_info(&mcu->client->dev,
			 "APROM crashed; resetting MCU to LDROM\n");
		mcu_enter_bootloader(mcu);
	}
	msleep(200);

	ret = mcu_ldrom_cmd(mcu, CMD_SYNC_PACKNO, NULL, 0, NULL, 0);
	if (ret) {
		dev_err(&mcu->client->dev, "Sync Packno cmd failed\n");
		goto out;
	}

	ret = mcu_ldrom_cmd(mcu, CMD_GET_DEVICEID, NULL, 0, &devid, 1);
	if (ret)
		goto out;
	dev_info(&mcu->client->dev, "DeviceID: 0x%x\n", devid);

	ret = mcu_ldrom_cmd(mcu, CMD_READ_CONFIG, NULL, 0, config, 2);
	if (ret)
		goto out;
	dev_info(&mcu->client->dev, "config0: 0x%x  config1: 0x%x\n",
		 config[0], config[1]);

	ret = mcu_ldrom_cmd(mcu, CMD_GET_FWVER, NULL, 0, &fwver, 1);
	if (ret)
		goto out;
	dev_info(&mcu->client->dev, "FW version: 0x%x\n", fwver & 0xff);

	/* send update command with initial 48 bytes */
	memset(mcu->sendbuf, 0, PACKET_SIZE);
	memcpy(mcu->sendbuf, &(u32){CMD_UPDATE_APROM}, 4);
	memcpy(mcu->sendbuf + 4, &mcu->g_packno, 4);
	mcu->g_packno++;
	startaddr = 0;
	memcpy(mcu->sendbuf + 8, &startaddr, 4);
	memcpy(mcu->sendbuf + 12, &fw->size, 4);

	for (i = 0; i < 48 && i < fw->size; i++)
		mcu->fw_buf[i] = fw->data[i];
	memcpy(mcu->sendbuf + 16, mcu->fw_buf, 48);

	ret = mcu_ldrom_send(mcu);
	if (ret)
		goto out;

	for (i = 0; i < (fw->size / 512 + 1); i++)
		msleep(200);

	ret = mcu_ldrom_recv(mcu);
	if (ret)
		goto out;

	ct = 48;
	for (i = 48; i < fw->size; i += 56) {
		if (((i - 48) % 448) == 0) {
			l = min_t(unsigned int, fw->size - i, 448);
			for (k = ct; k < ct + l; k++)
				mcu->fw_buf[k - ct] = fw->data[k];
			ct += 448;
		}

		dev_info(&mcu->client->dev, "Programming: %d %%",
			 (int)((unsigned long)i * 100 / fw->size));

		memset(mcu->sendbuf, 0, PACKET_SIZE);
		memcpy(mcu->sendbuf + 4, &mcu->g_packno, 4);
		mcu->g_packno++;

		if ((fw->size - i) > 56) {
			memcpy(mcu->sendbuf + 8,
			       &mcu->fw_buf[((i - 48) % 448)], 56);
			ret = mcu_ldrom_send(mcu);
			if (ret)
				goto out;
			msleep(100);
			ret = mcu_ldrom_recv(mcu);
			if (ret)
				goto out;
		} else {
			memcpy(mcu->sendbuf + 8,
			       &mcu->fw_buf[((i - 48) % 448)],
			       fw->size - i);
			ret = mcu_ldrom_send(mcu);
			if (ret)
				goto out;
			msleep(100);
			ret = mcu_ldrom_recv(mcu);
			if (ret)
				goto out;

			memcpy(&get_cksum, mcu->rcvbuf + 8, 2);
			dev_info(&mcu->client->dev,
				 "device checksum=0x%x\n", get_cksum);

			for (bin_cksum = 0, j = 0; j < fw->size; j++)
				bin_cksum += fw->data[j];

			if ((bin_cksum & 0xffff) != get_cksum) {
				dev_err(&mcu->client->dev,
					"Checksum mismatch -- update failed!\n");
				ret = -EIO;
				goto out;
			}
		}
	}

	dev_info(&mcu->client->dev, "Programming: 100 %%\n");
	dev_info(&mcu->client->dev, "Firmware update success!\n");

out:
	if (ret)
		dev_err(&mcu->client->dev, "Firmware update failed!\n");

	dev_info(&mcu->client->dev, "Resetting MCU to APROM...\n");
	mcu_leave_bootloader(mcu);
	mutex_unlock(&mcu->lock);
	release_firmware(fw);
	return ret;
}

static struct mcu_data *mcu_dev_get(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev->parent);

	return i2c_get_clientdata(client);
}

#define MCU_ATTR_RW(_name, _reg)					    \
	static ssize_t mcu_show_##_name(struct device *dev,		    \
					 struct device_attribute *attr,  \
					 char *buf)			    \
	{								    \
		struct mcu_data *mcu = mcu_dev_get(dev);		    \
		u8 val = 0;						    \
		int ret;						    \
									    \
		ret = mcu_i2c_read_reg(mcu->client, _reg, &val);	    \
		if (ret)						    \
			return ret;					    \
		return sprintf(buf, "%d\n", val);			    \
	}								    \
	static ssize_t mcu_store_##_name(struct device *dev,		    \
					  struct device_attribute *attr, \
					  const char *buf, size_t count)  \
	{								    \
		struct mcu_data *mcu = mcu_dev_get(dev);		    \
		unsigned long val;					    \
		int ret;						    \
									    \
		ret = kstrtoul(buf, 0, &val);				    \
		if (ret)						    \
			return ret;					    \
		ret = mcu_i2c_write_reg(mcu->client, _reg, (u8)val);	    \
		if (ret)						    \
			return ret;					    \
		return count;						    \
	}								    \
	static DEVICE_ATTR(_name, 0644, mcu_show_##_name, mcu_store_##_name)

#define MCU_ATTR_RO(_name, _reg)					    \
	static ssize_t mcu_show_##_name(struct device *dev,		    \
					 struct device_attribute *attr,  \
					 char *buf)			    \
	{								    \
		struct mcu_data *mcu = mcu_dev_get(dev);		    \
		u8 val = 0;						    \
		int ret;						    \
									    \
		ret = mcu_i2c_read_reg(mcu->client, _reg, &val);	    \
		if (ret)						    \
			return ret;					    \
		return sprintf(buf, "%d\n", val);			    \
	}								    \
	static DEVICE_ATTR(_name, 0444, mcu_show_##_name, NULL)

MCU_ATTR_RO(temper1, MCU_REG_READ_TEMPER1);
MCU_ATTR_RO(temper2, MCU_REG_READ_TEMPER2);
MCU_ATTR_RO(fan, MCU_REG_READ_FAN);
MCU_ATTR_RO(fw_ver, MCU_REG_READ_FW_VER);

MCU_ATTR_RW(fan_speed, MCU_REG_FAN);
MCU_ATTR_RW(pwm_led, MCU_REG_PWM_LED);
MCU_ATTR_RW(pwm_led_duty, MCU_REG_PWM_LED_DUTY);
MCU_ATTR_RW(led_speed, MCU_REG_LED_SPEED);
MCU_ATTR_RW(led_red, MCU_REG_LED_RED);
MCU_ATTR_RW(led_green, MCU_REG_LED_GREEN);
MCU_ATTR_RW(led_blue, MCU_REG_LED_BLUE);
MCU_ATTR_RW(led_f_red, MCU_REG_LED_F_RED);
MCU_ATTR_RW(led_f_green, MCU_REG_LED_F_GREEN);
MCU_ATTR_RW(led_f_blue, MCU_REG_LED_F_BLUE);

static ssize_t mcu_show_led(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct mcu_data *mcu = mcu_dev_get(dev);
	u8 val = 0;
	int ret;

	ret = mcu_i2c_read_reg(mcu->client, MCU_REG_LED, &val);
	if (ret)
		return ret;
	return sprintf(buf, "0x%x\n", val);
}

static ssize_t mcu_store_led(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mcu_data *mcu = mcu_dev_get(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	ret = mcu_i2c_write_reg(mcu->client, MCU_REG_LED, (u8)val);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR(led, 0644, mcu_show_led, mcu_store_led);

static ssize_t mcu_show_bootloader_fw_ver(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct mcu_data *mcu = mcu_dev_get(dev);
	u32 fwver;
	int ret;

	mutex_lock(&mcu->lock);
	mcu->g_packno = 1;
	mcu_enter_bootloader(mcu);
	msleep(200);

	ret = mcu_ldrom_cmd(mcu, CMD_SYNC_PACKNO, NULL, 0, NULL, 0);
	if (ret)
		goto out_ver;

	ret = mcu_ldrom_cmd(mcu, CMD_GET_FWVER, NULL, 0, &fwver, 1);
	if (ret)
		goto out_ver;

out_ver:
	mcu_leave_bootloader(mcu);
	mutex_unlock(&mcu->lock);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", fwver & 0xff);
}
static DEVICE_ATTR(bootloader_fw_ver, 0444, mcu_show_bootloader_fw_ver, NULL);

static ssize_t mcu_store_bootloader_mode(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct mcu_data *mcu = mcu_dev_get(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	mutex_lock(&mcu->lock);
	mcu_enter_bootloader(mcu);
	mutex_unlock(&mcu->lock);
	return count;
}
static DEVICE_ATTR(bootloader_mode, 0200, NULL, mcu_store_bootloader_mode);

static ssize_t mcu_store_fw_update(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mcu_data *mcu = mcu_dev_get(dev);
	u8 ver_buf = 0;
	unsigned long val;
	int ret, ver;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	if (val != 1)
		return count;

	ret = mcu_i2c_read_reg(mcu->client, MCU_REG_READ_FW_VER, &ver_buf);
	if (ret)
		return ret;

	dev_info(&mcu->client->dev, "Current FW version: %d\n", ver_buf);

	ret = sscanf(BOOT_UPDATE_FIRMWARE_NAME, "mcu_%3d", &ver);
	if (ret != 1)
		return -EINVAL;

	dev_info(&mcu->client->dev, "Firmware file version: %d\n", ver);

	if (ver > ver_buf) {
		dev_info(&mcu->client->dev, "Requesting firmware update\n");
		ret = mcu_fw_update(mcu, BOOT_UPDATE_FIRMWARE_NAME);
		if (ret)
			return ret;

		mcu_i2c_write_reg(mcu->client, MCU_REG_LED, 0xB0);
	}

	return count;
}
static DEVICE_ATTR(fw_update, 0200, NULL, mcu_store_fw_update);

static struct attribute *mcu_attrs[] = {
	&dev_attr_temper1.attr,
	&dev_attr_temper2.attr,
	&dev_attr_fan.attr,
	&dev_attr_fw_ver.attr,
	&dev_attr_bootloader_fw_ver.attr,
	&dev_attr_fan_speed.attr,
	&dev_attr_led.attr,
	&dev_attr_pwm_led.attr,
	&dev_attr_pwm_led_duty.attr,
	&dev_attr_led_speed.attr,
	&dev_attr_led_red.attr,
	&dev_attr_led_green.attr,
	&dev_attr_led_blue.attr,
	&dev_attr_led_f_red.attr,
	&dev_attr_led_f_green.attr,
	&dev_attr_led_f_blue.attr,
	&dev_attr_bootloader_mode.attr,
	&dev_attr_fw_update.attr,
	NULL
};
ATTRIBUTE_GROUPS(mcu);

static void mcu_reset(struct mcu_data *mcu)
{
	gpiod_set_value(mcu->boot_gpio, 0);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 1);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 0);
	msleep(10);
	gpiod_set_value(mcu->reset_gpio, 1);
	msleep(100);
}

static int mcu_i2c_probe(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	struct mcu_data *mcu;
	int err;

	dev_info(&client->dev, "MCU driver probe\n");

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -EIO;

	mcu = devm_kzalloc(&client->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->client = client;
	mutex_init(&mcu->lock);
	i2c_set_clientdata(client, mcu);

	mcu->boot_gpio = devm_gpiod_get(&client->dev, "boot", GPIOD_OUT_HIGH);
	if (IS_ERR(mcu->boot_gpio)) {
		err = PTR_ERR(mcu->boot_gpio);
		dev_err(&client->dev, "failed to get boot gpio: %d\n", err);
		return err;
	}

	mcu->reset_gpio = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mcu->reset_gpio)) {
		err = PTR_ERR(mcu->reset_gpio);
		dev_err(&client->dev, "failed to get reset gpio: %d\n", err);
		return err;
	}

	mcu_reset(mcu);

	err = mcu_i2c_write_reg(client, MCU_REG_LED, 0xC0);
	if (err)
		dev_warn(&client->dev, "Init LED pattern failed: %d\n", err);

	dev_info(&client->dev, "MCU driver probed\n");
	return 0;
}

static void mcu_i2c_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "MCU driver removed\n");
}

static const struct i2c_device_id mcu_id[] = {
	{ "mcu", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcu_id);

static const struct of_device_id mcu_of_match[] = {
	{ .compatible = "nuvoton,mini54fde" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcu_of_match);

static struct i2c_driver mcu_i2c_driver = {
	.driver = {
		.name = "mcu",
		.of_match_table = mcu_of_match,
		.dev_groups = mcu_groups,
	},
	.probe = mcu_i2c_probe,
	.remove = mcu_i2c_remove,
	.id_table = mcu_id,
};
module_i2c_driver(mcu_i2c_driver);

MODULE_DESCRIPTION("Nuvoton MINI54FDE MCU driver");
MODULE_LICENSE("GPL");
