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
#include <linux/of.h>
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
	struct iio_mount_matrix accel_mount_matrix;
	u32 last_als1;
	s32 last_ir3;
	u16 last_dark2;
	u16 last_ps;
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

	/*
	 * 2. MEMSIC MMC5603NJ (0x30), single-shot mode. Register map: 0x1A ODR,
	 * 0x1B Internal Control 0, 0x1C Internal Control 1, 0x1D Internal
	 * Control 2. The previous code wrote "ODR = 50" to 0x1C, which set
	 * Y/Z inhibit + the self-test current and pinned Y/Z at full scale.
	 */
	q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1d, 0x00); /* CTRL2: continuous mode off */
	q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1c, 0x00); /* CTRL1: BW=6.6ms, no inhibit/self-test */
	q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1b, 0x08); /* CTRL0: Do_Set (clear residual magnetization) */
	usleep_range(1000, 2000);

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

static const struct iio_mount_matrix *sc7a20_get_mount_matrix(
	struct iio_dev *indio_dev, const struct iio_chan_spec *chan)
{
	struct q25_sensors_ctx *ctx = iio_device_get_drvdata(indio_dev);

	return &ctx->accel_mount_matrix;
}

static const struct iio_chan_spec_ext_info sc7a20_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, sc7a20_get_mount_matrix),
	{ }
};

#define SC7A20_ACCEL_CHANNEL(axis, idx) { \
	.type = IIO_ACCEL, \
	.modified = 1, \
	.channel2 = IIO_MOD_##axis, \
	.address = (idx), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.ext_info = sc7a20_ext_info, \
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
	int ret, i;

	if (!ctx)
		return -ENODEV;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&ctx->lock);
		q25_sensors_configure_pins(ctx);
		q25_i2c_write_reg(&ctx->bus_imu, 0x30, 0x1b, 0x21); /* Take_meas_M | Auto_SR_en */
		/* BW=00: ~6.6 ms per measurement (+SET/RESET); wait for Meas_m_done (Status1 bit 6) */
		ret = -ETIMEDOUT;
		for (i = 0; i < 10; i++) {
			usleep_range(5000, 6000);
			if (q25_i2c_read_regs(&ctx->bus_imu, 0x30, 0x18, buf, 1) == 0 && (buf[0] & BIT(6))) {
				ret = 0;
				break;
			}
		}
		if (!ret)
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

/*
 * Stock orientation: the stock SCP sensor-hub firmware (scp_a partition,
 * tinysys-scp-RV55_A) carries a built-in sensor dts with
 * { name = "mmc5603", slave_addr = 0x30, direction = 7 }. MTK direction 7 in
 * its hwmsen/coordinate table (identical copy in that firmware) is
 * sign {-1,-1,-1}, map {1,0,2}, i.e. Android X = -chip Y, Y = -chip X,
 * Z = -chip Z. Expose that as the IIO mount matrix (rows = device axes).
 */
static const struct iio_mount_matrix mmc5603_mount_matrix = {
	.rotation = {
		"0", "-1", "0",
		"-1", "0", "0",
		"0", "0", "-1",
	},
};

static const struct iio_mount_matrix *mmc5603_get_mount_matrix(
	struct iio_dev *indio_dev, const struct iio_chan_spec *chan)
{
	return &mmc5603_mount_matrix;
}

static const struct iio_chan_spec_ext_info mmc5603_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, mmc5603_get_mount_matrix),
	{ }
};

#define MMC5603_MAGN_CHANNEL(axis, idx) { \
	.type = IIO_MAGN, \
	.modified = 1, \
	.channel2 = IIO_MOD_##axis, \
	.address = (idx), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.ext_info = mmc5603_ext_info, \
}

static const struct iio_chan_spec mmc5603_channels[] = {
	MMC5603_MAGN_CHANNEL(X, 0),
	MMC5603_MAGN_CHANNEL(Y, 1),
	MMC5603_MAGN_CHANNEL(Z, 2),
};

/* -------------------------------------------------------------------------
 * 3. TYHX HX32062SE Ambient Light (ALS) & Proximity (PS) IIO Device
 *
 * Data paths follow the stock SCP sensor-hub driver (hx32062se in the
 * scp_a tinysys image; no source is available):
 *   - reg 0x83 |= 0x02 latches the data registers, &= ~0x02 releases them;
 *   - ALS: 9 bytes from 0x01: als1 = 24-bit LE (0x01..0x03), ir3 = signed
 *     24-bit LE (0x04..0x06) >> 3, dark2 = 16-bit LE (0x07..0x08)
 *     (logged by the vendor as "als1_lp_data / als2_lp_data / als3_lp_data");
 *   - PS: the value the vendor compares against ps_threshold_high/low
 *     (3800/3300 in the stock SCP dts) is the 16-bit LE word at 0x69..0x6a.
 *
 * No lux is reported: the vendor lux path depends on run-time AGC gain,
 * light-type coefficient selection and per-part dark offsets read from the
 * sensor's own efuse, implemented only in that SCP firmware binary. Only the
 * raw channels are exposed; userspace must not treat them as lux.
 * ------------------------------------------------------------------------- */
enum {
	HX32062SE_ALS1,
	HX32062SE_IR3,
	HX32062SE_DARK2,
};

static int hx32062se_latch(struct q25_sensors_ctx *ctx, bool on)
{
	u8 v;
	int ret;

	ret = q25_i2c_read_regs(&ctx->bus_als, 0x2b, 0x83, &v, 1);
	if (ret < 0)
		return ret;
	v = on ? (v | 0x02) : (v & ~0x02);
	return q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x83, v);
}

static int hx32062se_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct q25_sensors_ctx *ctx = g_q25_sensors;
	u8 buf[9], ps[2];
	u32 als1, ir_raw;
	s32 ir3;
	u16 dark2, psv;
	int ret;

	if (!ctx)
		return -ENODEV;
	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	q25_sensors_configure_pins(ctx);
	ret = hx32062se_latch(ctx, true);
	if (ret == 0)
		ret = q25_i2c_read_regs(&ctx->bus_als, 0x2b, 0x01, buf, sizeof(buf));
	if (ret == 0)
		ret = q25_i2c_read_regs(&ctx->bus_als, 0x2b, 0x69, ps, sizeof(ps));
	hx32062se_latch(ctx, false);
	if (ret == 0) {
		als1 = buf[0] | (buf[1] << 8) | (buf[2] << 16);
		ir_raw = buf[3] | (buf[4] << 8) | (buf[5] << 16);
		ir3 = sign_extend32(ir_raw, 23) >> 3;
		dark2 = buf[6] | (buf[7] << 8);
		psv = ps[0] | (ps[1] << 8);
		if (als1 != 0 || psv != 0) {
			ctx->last_als1 = als1;
			ctx->last_ir3 = ir3;
			ctx->last_dark2 = dark2;
			ctx->last_ps = psv;
			/* Trigger next conversion cycle */
			q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x4c, 0x0f);
			q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x1a, 0x01);
			q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x1a, 0x05);
			q25_i2c_write_reg(&ctx->bus_als, 0x2b, 0x4c, 0x00);
		}
	}
	als1 = ctx->last_als1;
	ir3 = ctx->last_ir3;
	dark2 = ctx->last_dark2;
	psv = ctx->last_ps;
	mutex_unlock(&ctx->lock);
	if (ret < 0)
		return ret;

	switch (chan->type) {
	case IIO_INTENSITY:
		switch (chan->address) {
		case HX32062SE_ALS1:
			*val = als1;
			break;
		case HX32062SE_IR3:
			*val = ir3;
			break;
		default:
			*val = dark2;
			break;
		}
		return IIO_VAL_INT;
	case IIO_PROXIMITY:
		*val = psv;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
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

#define HX32062SE_INTENSITY_CHANNEL(idx) { \
	.type = IIO_INTENSITY, \
	.indexed = 1, \
	.channel = (idx), \
	.address = (idx), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
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

static int q25_sensors_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_accel, *indio_magn, *indio_als;
	struct q25_sensors_ctx *ctx = g_q25_sensors;
	int ret;

	ret = iio_read_mount_matrix(dev, &ctx->accel_mount_matrix);
	if (ret)
		return dev_err_probe(dev, ret, "Invalid accelerometer mount-matrix\n");

	indio_accel = devm_iio_device_alloc(dev, 0);
	if (!indio_accel)
		return -ENOMEM;
	iio_device_set_drvdata(indio_accel, ctx);
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

static const struct of_device_id q25_sensors_of_match[] = {
	{ .compatible = "zinwa,q25-sensors" },
	{ }
};
MODULE_DEVICE_TABLE(of, q25_sensors_of_match);

static struct platform_driver q25_sensors_pdrv = {
	.driver = {
		.name = "q25-sensors",
		.of_match_table = q25_sensors_of_match,
	},
	.probe = q25_sensors_probe,
};

static int __init q25_sensors_init(void)
{
	struct q25_sensors_ctx *ctx;
	struct device_node *np;
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

	/* Older boot images predate the sensor node; keep their IIO ABI usable. */
	np = of_find_compatible_node(NULL, NULL, "zinwa,q25-sensors");
	if (np) {
		of_node_put(np);
		return 0;
	}

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
	if (ctx->pdev)
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
