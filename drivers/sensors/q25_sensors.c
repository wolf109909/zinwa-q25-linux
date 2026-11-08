// SPDX-License-Identifier: GPL-2.0
/*
 * Zinwa Q25 Onboard Sensor Suite IIO Driver for Mainline Linux 7.3+ (MT6789)
 *
 * Exposes three IIO devices on the Q25 sensor buses (SCP I2C0 GPIO132/133 and
 * SCP I2C2 GPIO148/149):
 *   1. Silan SC7A20 3-axis accelerometer (0x18 on GPIO132=SCL, GPIO133=SDA)
 *   2. MEMSIC MMC5603NJ 3-axis magnetometer (0x30 on GPIO132=SCL, GPIO133=SDA)
 *   3. TYHX HX32062SE ambient light & proximity sensor (0x2b on GPIO148=SCL, GPIO149=SDA)
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define MT6789_GPIO_PHYS	0x10005000ULL
#define MT6789_GPIO_SIZE	0x1000
#define MT6789_IOCFG_BL_PHYS	0x11e60000ULL
#define MT6789_IOCFG_BL_SIZE	0x1000
#define MT6789_IOCFG_RT_PHYS	0x11ea0000ULL
#define MT6789_IOCFG_RT_SIZE	0x1000

#define GPIO_DIR4		0x0040
#define GPIO_DIR_SET4		0x0044
#define GPIO_DIR_CLR4		0x0048
#define GPIO_DOUT4		0x0140
#define GPIO_DOUT_SET4		0x0144
#define GPIO_DOUT_CLR4		0x0148
#define GPIO_DIN4		0x0240
#define GPIO_MODE16		0x0400
#define GPIO_MODE18		0x0420

struct q25_i2c_bb {
	void __iomem *gpio_base;
	u32 scl_bit;
	u32 sda_bit;
};

struct q25_sensors_ctx {
	void __iomem *gpio_base;
	void __iomem *iocfg_bl_base;
	void __iomem *iocfg_rt_base;
	struct mutex lock;
	struct q25_i2c_bb bus_imu; /* GPIO132 (SCL), GPIO133 (SDA) */
	struct q25_i2c_bb bus_als; /* GPIO148 (SCL), GPIO149 (SDA) */
	struct platform_device *pdev;
};

static struct q25_sensors_ctx *g_q25_sensors;

static inline void bb_scl_hi(struct q25_i2c_bb *b)
{
	writel(b->scl_bit, b->gpio_base + GPIO_DIR_CLR4);
}

static inline void bb_scl_lo(struct q25_i2c_bb *b)
{
	writel(b->scl_bit, b->gpio_base + GPIO_DOUT_CLR4);
	writel(b->scl_bit, b->gpio_base + GPIO_DIR_SET4);
}

static inline void bb_sda_hi(struct q25_i2c_bb *b)
{
	writel(b->sda_bit, b->gpio_base + GPIO_DIR_CLR4);
}

static inline void bb_sda_lo(struct q25_i2c_bb *b)
{
	writel(b->sda_bit, b->gpio_base + GPIO_DOUT_CLR4);
	writel(b->sda_bit, b->gpio_base + GPIO_DIR_SET4);
}

static inline int bb_sda_in(struct q25_i2c_bb *b)
{
	return (readl(b->gpio_base + GPIO_DIN4) & b->sda_bit) ? 1 : 0;
}

static void bb_start(struct q25_i2c_bb *b)
{
	bb_sda_hi(b);
	bb_scl_hi(b);
	udelay(4);
	bb_sda_lo(b);
	udelay(4);
	bb_scl_lo(b);
	udelay(4);
}

static void bb_stop(struct q25_i2c_bb *b)
{
	bb_sda_lo(b);
	udelay(4);
	bb_scl_hi(b);
	udelay(4);
	bb_sda_hi(b);
	udelay(4);
}

static int bb_write_byte(struct q25_i2c_bb *b, u8 val)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		if (val & BIT(i))
			bb_sda_hi(b);
		else
			bb_sda_lo(b);
		udelay(3);
		bb_scl_hi(b);
		udelay(4);
		bb_scl_lo(b);
		udelay(2);
	}
	bb_sda_hi(b);
	udelay(3);
	bb_scl_hi(b);
	udelay(4);
	ack = !bb_sda_in(b);
	bb_scl_lo(b);
	udelay(2);
	return ack ? 0 : -EIO;
}

static u8 bb_read_byte(struct q25_i2c_bb *b, bool send_ack)
{
	int i;
	u8 val = 0;

	bb_sda_hi(b);
	for (i = 7; i >= 0; i--) {
		udelay(3);
		bb_scl_hi(b);
		udelay(4);
		if (bb_sda_in(b))
			val |= BIT(i);
		bb_scl_lo(b);
	}
	if (send_ack)
		bb_sda_lo(b);
	else
		bb_sda_hi(b);
	udelay(3);
	bb_scl_hi(b);
	udelay(4);
	bb_scl_lo(b);
	bb_sda_hi(b);
	udelay(2);
	return val;
}

static int q25_i2c_write_reg(struct q25_i2c_bb *b, u8 addr, u8 reg, u8 val)
{
	int ret;

	bb_start(b);
	ret = bb_write_byte(b, addr << 1);
	if (ret < 0)
		goto out;
	ret = bb_write_byte(b, reg);
	if (ret < 0)
		goto out;
	ret = bb_write_byte(b, val);
out:
	bb_stop(b);
	return ret;
}

static int q25_i2c_read_regs(struct q25_i2c_bb *b, u8 addr, u8 reg, u8 *buf, int len)
{
	int i, ret;

	bb_start(b);
	ret = bb_write_byte(b, addr << 1);
	if (ret < 0) {
		bb_stop(b);
		return ret;
	}
	ret = bb_write_byte(b, reg);
	if (ret < 0) {
		bb_stop(b);
		return ret;
	}
	bb_start(b);
	ret = bb_write_byte(b, (addr << 1) | 1);
	if (ret < 0) {
		bb_stop(b);
		return ret;
	}
	for (i = 0; i < len; i++)
		buf[i] = bb_read_byte(b, i < (len - 1));
	bb_stop(b);
	return 0;
}

static void q25_sensors_configure_pins(struct q25_sensors_ctx *ctx)
{
	u32 m16, m18;

	/* Configure GPIO132 (bit 4) and GPIO133 (bit 5) as GPIO mode 0 with internal pull-up */
	m16 = readl(ctx->gpio_base + GPIO_MODE16);
	m16 &= ~((0x7U << 16) | (0x7U << 20));
	writel(m16, ctx->gpio_base + GPIO_MODE16);

	/* Configure GPIO148 (bit 20) and GPIO149 (bit 21) as GPIO mode 0 with internal pull-up */
	m18 = readl(ctx->gpio_base + GPIO_MODE18);
	m18 &= ~((0x7U << 16) | (0x7U << 20));
	writel(m18, ctx->gpio_base + GPIO_MODE18);

	/* Enable IES, SMT, PU, and RSEL on GPIO132/133 (IOCFG_BL) and GPIO148/149 (IOCFG_RT) */
	if (ctx->iocfg_bl_base) {
		writel(BIT(24) | BIT(25), ctx->iocfg_bl_base + 0x054); /* IES_SET */
		writel(BIT(7) | BIT(8), ctx->iocfg_bl_base + 0x0d4);   /* SMT_SET */
		writel(BIT(20) | BIT(21), ctx->iocfg_bl_base + 0x068); /* PD_CLR */
		writel(BIT(20) | BIT(21), ctx->iocfg_bl_base + 0x084); /* PU_SET */
	}
	if (ctx->iocfg_rt_base) {
		writel(BIT(6) | BIT(10), ctx->iocfg_rt_base + 0x074);  /* IES_SET */
		writel(BIT(6) | BIT(10), ctx->iocfg_rt_base + 0x144);  /* SMT_SET */
		writel(BIT(6) | BIT(10), ctx->iocfg_rt_base + 0x098);  /* PD_CLR */
		writel(BIT(6) | BIT(10), ctx->iocfg_rt_base + 0x0c4);  /* PU_SET */
	}

	bb_scl_hi(&ctx->bus_imu);
	bb_sda_hi(&ctx->bus_imu);
	bb_scl_hi(&ctx->bus_als);
	bb_sda_hi(&ctx->bus_als);
	udelay(20);
}

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

static void q25_sensors_hw_init(struct q25_sensors_ctx *ctx)
{
	int i;

	mutex_lock(&ctx->lock);
	q25_sensors_configure_pins(ctx);

	/* 1. Initialize Silan SC7A20 (0x18): 100Hz XYZ enabled, BDU + HR */
	q25_i2c_write_reg(&ctx->bus_imu, 0x18, 0x20, 0x57); /* CTRL_REG1: 100Hz, XYZ en */
	q25_i2c_write_reg(&ctx->bus_imu, 0x18, 0x23, 0x88); /* CTRL_REG4: BDU=1, HR=1 (+/-2g) */

	/* 2. Initialize MEMSIC MMC5603NJ (0x30): Auto_SR_en + Continuous 50Hz */
	q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1c, 50);   /* ODR = 50Hz */
	q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1b, 0xa0); /* CTRL0: Cmm_freq_en | Auto_SR_en */
	q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1d, 0x10); /* CTRL2: Cmm_en = 1 */

	/* 3. Initialize TYHX HX32062SE (0x2b): ALS + PS active */
	q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x4c, 0x0f);
	for (i = 0; i < ARRAY_SIZE(hx32062se_init_table); i++)
		q25_i2c_write_reg(&ctx->bus_als, 0x2b, hx32062se_init_table[i].reg, hx32062se_init_table[i].val);
	q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x1a, 0x01);
	q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x1a, 0x05);
	q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x4c, 0x00);
	mutex_unlock(&ctx->lock);
}

/* -------------------------------------------------------------------------
 * 1. Silan SC7A20 3-Axis Accelerometer IIO Device
 *    12-bit signed (raw >> 4), +/-2g range => 1 LSB = 9.80665 / 1024 m/s^2
 *    scale = 0.009576806 m/s^2
 * ------------------------------------------------------------------------- */
static int sc7a20_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct q25_sensors_ctx *ctx = g_q25_sensors;
	u8 buf[6];
	s16 raw;
	int ret;

	if (!ctx)
		return -ENODEV;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&ctx->lock);
		q25_sensors_configure_pins(ctx);
		q25_i2c_write_reg(&ctx->bus_imu, 0x18, 0x20, 0x57);
		q25_i2c_write_reg(&ctx->bus_imu, 0x18, 0x23, 0x88);
		ret = q25_i2c_read_regs(&ctx->bus_imu, 0x18, 0x28 | 0x80, buf, 6);
		mutex_unlock(&ctx->lock);
		if (ret < 0)
			return ret;
		raw = (s16)((buf[chan->address * 2 + 1] << 8) | buf[chan->address * 2]);
		*val = raw >> 4;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 9576806; /* 9.80665 / 1024 m/s^2 in NANO */
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info sc7a20_iio_info = {
	.read_raw = sc7a20_read_raw,
};

#define SC7A20_ACCEL_CHANNEL(axis, idx) { \
	.type = IIO_ACCEL, \
	.modified = 1, \
	.channel2 = IIO_MOD_##axis, \
	.address = (idx), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
}

static const struct iio_chan_spec sc7a20_channels[] = {
	SC7A20_ACCEL_CHANNEL(X, 0),
	SC7A20_ACCEL_CHANNEL(Y, 1),
	SC7A20_ACCEL_CHANNEL(Z, 2),
};

/* -------------------------------------------------------------------------
 * 2. MEMSIC MMC5603NJ 3-Axis Magnetometer IIO Device
 *    16-bit unsigned centered at 32768 (1024 counts/Gauss = 10.24 counts/uT)
 *    In IIO Gauss units: scale = 1 / 1024 = 0.000976562 Gauss/LSB
 * ------------------------------------------------------------------------- */
static int mmc5603_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct q25_sensors_ctx *ctx = g_q25_sensors;
	u8 buf[6];
	int ret;

	if (!ctx)
		return -ENODEV;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&ctx->lock);
		q25_sensors_configure_pins(ctx);
		q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1b, 0x21); /* Take_meas_M | Auto_SR_en */
		usleep_range(6000, 8000);
		ret = q25_i2c_read_regs(&ctx->bus_imu, 0x30, 0x00, buf, 6);
		mutex_unlock(&ctx->lock);
		if (ret < 0)
			return ret;
		*val = (int)((buf[chan->address * 2] << 8) | buf[chan->address * 2 + 1]) - 32768;
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

#define MMC5603_MAGN_CHANNEL(axis, idx) { \
	.type = IIO_MAGN, \
	.modified = 1, \
	.channel2 = IIO_MOD_##axis, \
	.address = (idx), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
}

static const struct iio_chan_spec mmc5603_channels[] = {
	MMC5603_MAGN_CHANNEL(X, 0),
	MMC5603_MAGN_CHANNEL(Y, 1),
	MMC5603_MAGN_CHANNEL(Z, 2),
};

/* -------------------------------------------------------------------------
 * 3. TYHX HX32062SE Ambient Light (ALS) & Proximity (PS) IIO Device
 * ------------------------------------------------------------------------- */
static int hx32062se_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct q25_sensors_ctx *ctx = g_q25_sensors;
	u8 buf[10];
	u32 als0, als1;
	u16 ps0;
	int ret;

	if (!ctx)
		return -ENODEV;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		mutex_lock(&ctx->lock);
		q25_sensors_configure_pins(ctx);
		q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x83, 0x02);
		ret = q25_i2c_read_regs(&ctx->bus_als, 0x2b, 0x01, buf, 6);
		if (ret == 0)
			ret = q25_i2c_read_regs(&ctx->bus_als, 0x2b, 0x0d, buf + 6, 3);
		q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x83, 0x00);
		mutex_unlock(&ctx->lock);
		if (ret < 0)
			return ret;

		als0 = (buf[0] | (buf[1] << 8) | (buf[2] << 16)) >> 6;
		als1 = (buf[3] | (buf[4] << 8) | (buf[5] << 16)) >> 6;
		ps0 = buf[6] | (buf[7] << 8);

		if (chan->type == IIO_LIGHT) {
			/* Convert 18-bit ALS counts (baseline ~46800) to lux estimate */
			*val = (als0 > 46800U) ? (int)((als0 - 46800U) / 5U) : (int)(als0 / 200U);
			return IIO_VAL_INT;
		} else if (chan->type == IIO_INTENSITY) {
			*val = (int)(chan->address == 0 ? als0 : als1);
			return IIO_VAL_INT;
		} else if (chan->type == IIO_PROXIMITY) {
			*val = (int)ps0;
			return IIO_VAL_INT;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static const struct iio_info hx32062se_iio_info = {
	.read_raw = hx32062se_read_raw,
};

static const struct iio_chan_spec hx32062se_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) | BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_INTENSITY,
		.indexed = 1,
		.channel = 0,
		.address = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_INTENSITY,
		.indexed = 1,
		.channel = 1,
		.address = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static int q25_sensors_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_accel, *indio_magn, *indio_als;
	int ret;

	indio_accel = devm_iio_device_alloc(dev, 0);
	if (!indio_accel)
		return -ENOMEM;
	indio_accel->name = "sc7a20";
	indio_accel->modes = INDIO_DIRECT_MODE;
	indio_accel->info = &sc7a20_iio_info;
	indio_accel->channels = sc7a20_channels;
	indio_accel->num_channels = ARRAY_SIZE(sc7a20_channels);
	ret = devm_iio_device_register(dev, indio_accel);
	if (ret)
		return ret;

	indio_magn = devm_iio_device_alloc(dev, 0);
	if (!indio_magn)
		return -ENOMEM;
	indio_magn->name = "mmc5603nj";
	indio_magn->modes = INDIO_DIRECT_MODE;
	indio_magn->info = &mmc5603_iio_info;
	indio_magn->channels = mmc5603_channels;
	indio_magn->num_channels = ARRAY_SIZE(mmc5603_channels);
	ret = devm_iio_device_register(dev, indio_magn);
	if (ret)
		return ret;

	indio_als = devm_iio_device_alloc(dev, 0);
	if (!indio_als)
		return -ENOMEM;
	indio_als->name = "hx32062se";
	indio_als->modes = INDIO_DIRECT_MODE;
	indio_als->info = &hx32062se_iio_info;
	indio_als->channels = hx32062se_channels;
	indio_als->num_channels = ARRAY_SIZE(hx32062se_channels);
	ret = devm_iio_device_register(dev, indio_als);
	if (ret)
		return ret;

	dev_info(dev, "Registered Zinwa Q25 IIO sensors: sc7a20 (accel), mmc5603nj (magn), hx32062se (als/ps)\n");
	return 0;
}

static struct platform_driver q25_sensors_pdrv = {
	.driver = {
		.name = "q25-sensors",
	},
	.probe = q25_sensors_probe,
};

static int __init q25_sensors_init(void)
{
	struct q25_sensors_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	ctx->gpio_base = ioremap(MT6789_GPIO_PHYS, MT6789_GPIO_SIZE);
	ctx->iocfg_bl_base = ioremap(MT6789_IOCFG_BL_PHYS, MT6789_IOCFG_BL_SIZE);
	ctx->iocfg_rt_base = ioremap(MT6789_IOCFG_RT_PHYS, MT6789_IOCFG_RT_SIZE);
	if (!ctx->gpio_base) {
		kfree(ctx);
		return -ENOMEM;
	}

	ctx->bus_imu.gpio_base = ctx->gpio_base;
	ctx->bus_imu.scl_bit = BIT(4);  /* GPIO 132 */
	ctx->bus_imu.sda_bit = BIT(5);  /* GPIO 133 */

	ctx->bus_als.gpio_base = ctx->gpio_base;
	ctx->bus_als.scl_bit = BIT(20); /* GPIO 148 */
	ctx->bus_als.sda_bit = BIT(21); /* GPIO 149 */

	g_q25_sensors = ctx;
	q25_sensors_hw_init(ctx);

	ret = platform_driver_register(&q25_sensors_pdrv);
	if (ret)
		goto err_unmap;

	ctx->pdev = platform_device_register_simple("q25-sensors", -1, NULL, 0);
	if (IS_ERR(ctx->pdev)) {
		ret = PTR_ERR(ctx->pdev);
		platform_driver_unregister(&q25_sensors_pdrv);
		goto err_unmap;
	}

	return 0;

err_unmap:
	g_q25_sensors = NULL;
	if (ctx->iocfg_rt_base)
		iounmap(ctx->iocfg_rt_base);
	if (ctx->iocfg_bl_base)
		iounmap(ctx->iocfg_bl_base);
	iounmap(ctx->gpio_base);
	kfree(ctx);
	return ret;
}

static void __exit q25_sensors_exit(void)
{
	struct q25_sensors_ctx *ctx = g_q25_sensors;

	if (!ctx)
		return;
	platform_device_unregister(ctx->pdev);
	platform_driver_unregister(&q25_sensors_pdrv);
	g_q25_sensors = NULL;
	if (ctx->iocfg_rt_base)
		iounmap(ctx->iocfg_rt_base);
	if (ctx->iocfg_bl_base)
		iounmap(ctx->iocfg_bl_base);
	if (ctx->gpio_base)
		iounmap(ctx->gpio_base);
	kfree(ctx);
}

module_init(q25_sensors_init);
module_exit(q25_sensors_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zinwa Q25 Mainline Port");
MODULE_DESCRIPTION("Zinwa Q25 Onboard IIO Sensor Driver (SC7A20 Accel, MMC5603NJ Magn, HX32062SE ALS/PS)");
