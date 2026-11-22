// SPDX-License-Identifier: GPL-2.0
/*
 * Zinwa Q25 sensors without a mainline driver (MT6789):
 *   - MEMSIC MMC5603NJ 3-axis magnetometer (I2C1, 0x30)
 *   - TYHX HX32062SE ambient light & proximity sensor (I2C9, 0x2b)
 *
 * The SC7A20 accelerometer on I2C1 (0x18) uses the mainline st_accel driver.
 * The stock firmware reads all three from the SCP sensor hub; here they are
 * ordinary I2C clients of the AP I2C controllers on the same pins (see the
 * board DT). Register-level facts come from the stock SCP sensor-hub firmware.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

/* ------------------------------------------------------------------------
 * MEMSIC MMC5603NJ: 16-bit unsigned output centred at 32768,
 * 1024 counts/Gauss => scale 1/1024 Gauss per LSB.
 * ------------------------------------------------------------------------ */
#define MMC5603_REG_XOUT0	0x00
#define MMC5603_REG_STATUS1	0x18
#define MMC5603_STATUS1_MEAS_M_DONE	BIT(6)
#define MMC5603_REG_CTRL0	0x1b
#define MMC5603_CTRL0_TM_M		BIT(0)
#define MMC5603_CTRL0_DO_SET		BIT(3)
#define MMC5603_CTRL0_AUTO_SR		BIT(5)
#define MMC5603_REG_CTRL1	0x1c
#define MMC5603_REG_CTRL2	0x1d
#define MMC5603_REG_PRODUCT_ID	0x39
#define MMC5603_PRODUCT_ID	0x10

struct mmc5603 {
	struct i2c_client *client;
	struct mutex lock;	/* one measurement at a time */
};

static int mmc5603_measure(struct mmc5603 *mmc, u8 *buf)
{
	struct i2c_client *c = mmc->client;
	int ret, i;

	ret = i2c_smbus_write_byte_data(c, MMC5603_REG_CTRL0,
					MMC5603_CTRL0_TM_M | MMC5603_CTRL0_AUTO_SR);
	if (ret)
		return ret;

	/* BW=00: ~6.6 ms per measurement plus SET/RESET */
	for (i = 0; i < 10; i++) {
		usleep_range(5000, 6000);
		ret = i2c_smbus_read_byte_data(c, MMC5603_REG_STATUS1);
		if (ret < 0)
			return ret;
		if (ret & MMC5603_STATUS1_MEAS_M_DONE)
			break;
	}
	if (i == 10)
		return -ETIMEDOUT;

	ret = i2c_smbus_read_i2c_block_data(c, MMC5603_REG_XOUT0, 6, buf);
	if (ret < 0)
		return ret;
	return ret == 6 ? 0 : -EIO;
}

static int mmc5603_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct mmc5603 *mmc = iio_priv(indio_dev);
	u8 buf[6];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&mmc->lock);
		ret = mmc5603_measure(mmc, buf);
		mutex_unlock(&mmc->lock);
		if (ret)
			return ret;
		*val = (int)((buf[chan->address * 2] << 8) |
			     buf[chan->address * 2 + 1]) - 32768;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 976562; /* 1 / 1024 Gauss per LSB */
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info mmc5603_iio_info = {
	.read_raw = mmc5603_read_raw,
};

/*
 * Stock orientation: the SCP sensor-hub firmware (scp_a, tinysys-scp-RV55_A)
 * carries { name = "mmc5603", slave_addr = 0x30, direction = 7 }. MTK
 * direction 7 is sign {-1,-1,-1}, map {1,0,2}: Android X = -chip Y,
 * Y = -chip X, Z = -chip Z. Exposed as the IIO mount matrix.
 */
static const struct iio_mount_matrix mmc5603_mount_matrix = {
	.rotation = {
		"0", "-1", "0",
		"-1", "0", "0",
		"0", "0", "-1",
	},
};

static const struct iio_mount_matrix *
mmc5603_get_mount_matrix(struct iio_dev *indio_dev,
			 const struct iio_chan_spec *chan)
{
	return &mmc5603_mount_matrix;
}

static const struct iio_chan_spec_ext_info mmc5603_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, mmc5603_get_mount_matrix),
	{ }
};

#define MMC5603_MAGN_CHANNEL(axis, idx) {				\
	.type = IIO_MAGN,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.address = (idx),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.ext_info = mmc5603_ext_info,					\
}

static const struct iio_chan_spec mmc5603_channels[] = {
	MMC5603_MAGN_CHANNEL(X, 0),
	MMC5603_MAGN_CHANNEL(Y, 1),
	MMC5603_MAGN_CHANNEL(Z, 2),
};

static int mmc5603_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct mmc5603 *mmc;
	int ret;

	ret = i2c_smbus_read_byte_data(client, MMC5603_REG_PRODUCT_ID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no response\n");
	if (ret != MMC5603_PRODUCT_ID)
		return dev_err_probe(dev, -ENODEV, "unexpected product id 0x%02x\n", ret);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*mmc));
	if (!indio_dev)
		return -ENOMEM;
	mmc = iio_priv(indio_dev);
	mmc->client = client;
	mutex_init(&mmc->lock);

	/*
	 * Single-shot mode. CTRL1 = 0: BW 6.6 ms, no X/Y/Z inhibit, no
	 * self-test current (a non-zero CTRL1 pins Y/Z at full scale).
	 */
	ret = i2c_smbus_write_byte_data(client, MMC5603_REG_CTRL2, 0x00);
	if (!ret)
		ret = i2c_smbus_write_byte_data(client, MMC5603_REG_CTRL1, 0x00);
	if (!ret)
		ret = i2c_smbus_write_byte_data(client, MMC5603_REG_CTRL0,
						MMC5603_CTRL0_DO_SET);
	if (ret)
		return dev_err_probe(dev, ret, "init failed\n");
	usleep_range(1000, 2000);

	indio_dev->name = "mmc5603nj";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &mmc5603_iio_info;
	indio_dev->channels = mmc5603_channels;
	indio_dev->num_channels = ARRAY_SIZE(mmc5603_channels);
	return devm_iio_device_register(dev, indio_dev);
}

/* ------------------------------------------------------------------------
 * TYHX HX32062SE ambient light & proximity.
 *
 * Data paths follow the stock SCP driver (no source available):
 *   - reg 0x83 |= 0x02 latches the data registers, &= ~0x02 releases them;
 *   - ALS: 9 bytes from 0x01: als1 = 24-bit LE, ir3 = signed 24-bit LE >> 3,
 *     dark2 = 16-bit LE;
 *   - PS: 16-bit LE at 0x69..0x6a (stock thresholds 3800 near / 3300 far).
 *
 * No lux: the vendor lux path depends on run-time AGC gain, light-type
 * coefficients and per-part efuse offsets inside the SCP firmware. Only raw
 * channels are exposed; userspace must not treat them as lux.
 * ------------------------------------------------------------------------ */
#define HX32062SE_REG_CHIP_ID	0x00
#define HX32062SE_CHIP_ID	0x37
#define HX32062SE_REG_ALS	0x01
#define HX32062SE_REG_MODE	0x1a
#define HX32062SE_REG_STANDBY	0x4c
#define HX32062SE_REG_PS	0x69
#define HX32062SE_REG_LATCH	0x83
#define HX32062SE_LATCH		BIT(1)

enum {
	HX32062SE_ALS1,
	HX32062SE_IR3,
	HX32062SE_DARK2,
};

struct hx32062se {
	struct i2c_client *client;
	struct mutex lock;	/* latch/read/re-trigger sequence + cache */
	u32 als1;
	s32 ir3;
	u16 dark2;
	u16 ps;
};

static const struct { u8 reg; u8 val; } hx32062se_init_table[] = {
	{ 0x64, 0x00 }, { 0x65, 0x00 }, { 0x66, 0x00 }, { 0x72, 0xff },
	{ 0x53, 0xf4 }, { 0x52, 0x52 }, { 0x55, 0xc5 }, { 0x50, 0x74 },
	{ 0x5c, 0x21 }, { 0xe0, 0x00 }, { 0xe1, 0xd7 }, { 0xe2, 0xc8 },
	{ 0xe3, 0xf8 }, { 0xe4, 0x80 }, { 0xe7, 0x00 }, { 0xe9, 0x3f },
	{ 0xea, 0x57 }, { 0xeb, 0x04 }, { 0xec, 0x2c }, { 0xed, 0x01 },
	{ 0xef, 0x9f }, { 0xf0, 0x0f }, { 0x5a, 0x00 }, { 0x5b, 0x01 },
	{ 0x20, 0x30 }, { 0x21, 0x10 }, { 0x30, 0x30 }, { 0x31, 0x10 },
	{ 0x54, 0x22 }, { 0x1d, 0x10 }, { 0x80, 0x12 }, { 0x83, 0x00 },
	{ 0x1f, 0x80 }, { 0x19, 0x00 }, { 0x1d, 0xd0 }, { 0x1e, 0xc0 },
	{ 0x1c, 0x00 }, { 0x1b, 0x00 }, { 0x8a, 0xd0 }, { 0x8b, 0x00 },
	{ 0x8c, 0xd0 }, { 0x8d, 0x00 }, { 0x2a, 0x01 }, { 0x2b, 0xfc },
	{ 0x2c, 0x01 }, { 0x3a, 0x00 }, { 0x3b, 0x00 }, { 0x3c, 0x00 },
	{ 0x3d, 0x00 }, { 0x4d, 0x00 }, { 0x4e, 0x00 }, { 0x4f, 0x00 },
	{ 0x29, 0x06 }, { 0x39, 0x06 }, { 0x4a, 0x06 }, { 0x46, 0x00 },
	{ 0x47, 0x00 }, { 0x48, 0x18 }, { 0x67, 0x18 }, { 0x14, 0x02 },
	{ 0x77, 0x20 }, { 0x58, 0x80 }, { 0x6e, 0x0f }, { 0x70, 0x00 },
	{ 0x45, 0x10 }, { 0x1d, 0x10 },
};

/* Start the next ALS/PS conversion cycle: standby, mode 1 -> 5, active. */
static int hx32062se_trigger(struct i2c_client *c)
{
	int ret;

	ret = i2c_smbus_write_byte_data(c, HX32062SE_REG_STANDBY, 0x0f);
	if (!ret)
		ret = i2c_smbus_write_byte_data(c, HX32062SE_REG_MODE, 0x01);
	if (!ret)
		ret = i2c_smbus_write_byte_data(c, HX32062SE_REG_MODE, 0x05);
	if (!ret)
		ret = i2c_smbus_write_byte_data(c, HX32062SE_REG_STANDBY, 0x00);
	return ret;
}

static int hx32062se_latch(struct i2c_client *c, bool on)
{
	int v = i2c_smbus_read_byte_data(c, HX32062SE_REG_LATCH);

	if (v < 0)
		return v;
	v = on ? (v | HX32062SE_LATCH) : (v & ~HX32062SE_LATCH);
	return i2c_smbus_write_byte_data(c, HX32062SE_REG_LATCH, v);
}

/* Refresh the cached sample; called with hx->lock held. */
static int hx32062se_update(struct hx32062se *hx)
{
	struct i2c_client *c = hx->client;
	u8 buf[9], ps[2];
	u32 als1;
	int ret, ret2;

	ret = hx32062se_latch(c, true);
	if (!ret) {
		ret = i2c_smbus_read_i2c_block_data(c, HX32062SE_REG_ALS,
						    sizeof(buf), buf);
		if (ret >= 0)
			ret = ret == sizeof(buf) ? 0 : -EIO;
	}
	if (!ret) {
		ret = i2c_smbus_read_i2c_block_data(c, HX32062SE_REG_PS,
						    sizeof(ps), ps);
		if (ret >= 0)
			ret = ret == sizeof(ps) ? 0 : -EIO;
	}
	ret2 = hx32062se_latch(c, false);
	if (ret)
		return ret;
	if (ret2)
		return ret2;

	als1 = buf[0] | (buf[1] << 8) | (buf[2] << 16);
	/* all-zero data: conversion not finished yet, keep the last sample */
	if (als1 == 0 && ps[0] == 0 && ps[1] == 0)
		return 0;

	hx->als1 = als1;
	hx->ir3 = sign_extend32(buf[3] | (buf[4] << 8) | (buf[5] << 16), 23) >> 3;
	hx->dark2 = buf[6] | (buf[7] << 8);
	hx->ps = ps[0] | (ps[1] << 8);
	return hx32062se_trigger(c);
}

static int hx32062se_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct hx32062se *hx = iio_priv(indio_dev);
	int ret;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	mutex_lock(&hx->lock);
	ret = hx32062se_update(hx);
	if (!ret) {
		switch (chan->type) {
		case IIO_INTENSITY:
			if (chan->address == HX32062SE_ALS1)
				*val = hx->als1;
			else if (chan->address == HX32062SE_IR3)
				*val = hx->ir3;
			else
				*val = hx->dark2;
			break;
		case IIO_PROXIMITY:
			*val = hx->ps;
			break;
		default:
			ret = -EINVAL;
		}
	}
	mutex_unlock(&hx->lock);

	return ret ? ret : IIO_VAL_INT;
}

static int hx32062se_read_label(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan, char *label)
{
	static const char * const names[] = { "als1", "ir3", "dark2" };

	if (chan->type == IIO_INTENSITY && chan->address < ARRAY_SIZE(names))
		return sysfs_emit(label, "%s\n", names[chan->address]);
	if (chan->type == IIO_PROXIMITY)
		return sysfs_emit(label, "ps\n");
	return -EINVAL;
}

static const struct iio_info hx32062se_iio_info = {
	.read_raw = hx32062se_read_raw,
	.read_label = hx32062se_read_label,
};

#define HX32062SE_INTENSITY_CHANNEL(idx) {				\
	.type = IIO_INTENSITY,						\
	.indexed = 1,							\
	.channel = (idx),						\
	.address = (idx),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
}

static const struct iio_chan_spec hx32062se_channels[] = {
	HX32062SE_INTENSITY_CHANNEL(HX32062SE_ALS1),
	HX32062SE_INTENSITY_CHANNEL(HX32062SE_IR3),
	HX32062SE_INTENSITY_CHANNEL(HX32062SE_DARK2),
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static int hx32062se_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct hx32062se *hx;
	int ret, i;

	ret = i2c_smbus_read_byte_data(client, HX32062SE_REG_CHIP_ID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no response\n");
	if (ret != HX32062SE_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "unexpected chip id 0x%02x\n", ret);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*hx));
	if (!indio_dev)
		return -ENOMEM;
	hx = iio_priv(indio_dev);
	hx->client = client;
	mutex_init(&hx->lock);

	ret = i2c_smbus_write_byte_data(client, HX32062SE_REG_STANDBY, 0x0f);
	for (i = 0; !ret && i < ARRAY_SIZE(hx32062se_init_table); i++)
		ret = i2c_smbus_write_byte_data(client, hx32062se_init_table[i].reg,
						hx32062se_init_table[i].val);
	if (!ret)
		ret = hx32062se_trigger(client);
	if (ret)
		return dev_err_probe(dev, ret, "init failed\n");

	indio_dev->name = "hx32062se";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &hx32062se_iio_info;
	indio_dev->channels = hx32062se_channels;
	indio_dev->num_channels = ARRAY_SIZE(hx32062se_channels);
	return devm_iio_device_register(dev, indio_dev);
}

/* ------------------------------------------------------------------------ */

static int q25_sensors_probe(struct i2c_client *client)
{
	int (*probe)(struct i2c_client *) = i2c_get_match_data(client);

	if (!probe)
		return -ENODEV;
	return probe(client);
}

static const struct of_device_id q25_sensors_of_match[] = {
	{ .compatible = "memsic,mmc5603", .data = mmc5603_probe },
	{ .compatible = "tyhx,hx32062se", .data = hx32062se_probe },
	{ }
};
MODULE_DEVICE_TABLE(of, q25_sensors_of_match);

static const struct i2c_device_id q25_sensors_id[] = {
	{ "mmc5603", (kernel_ulong_t)mmc5603_probe },
	{ "hx32062se", (kernel_ulong_t)hx32062se_probe },
	{ }
};
MODULE_DEVICE_TABLE(i2c, q25_sensors_id);

static struct i2c_driver q25_sensors_driver = {
	.driver = {
		.name = "q25-sensors",
		.of_match_table = q25_sensors_of_match,
	},
	.probe = q25_sensors_probe,
	.id_table = q25_sensors_id,
};
module_i2c_driver(q25_sensors_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zinwa Q25 Mainline Port");
MODULE_DESCRIPTION("Zinwa Q25 MMC5603NJ magnetometer and HX32062SE light/proximity IIO driver");
