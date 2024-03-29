/* Bosch BMI160 inertial measurement unit driver
 *
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Datasheet:
 * http://ae-bst.resource.bosch.com/media/_tech/media/datasheets/BST-BMI160-DS000-07.pdf
 */

#include <init.h>
#include <sensor.h>
#include <spi.h>
#include <gpio.h>
#include <misc/byteorder.h>
#include <nanokernel.h>

#include "sensor_bmi160.h"

#ifndef CONFIG_SENSOR_DEBUG
#define DBG(...) { ; }
#else
#include <misc/printk.h>
#define DBG(...) printk(CONFIG_BMI160_NAME ": " __VA_ARGS__)
#endif /* CONFIG_SENSOR_DEBUG */

struct bmi160_device_data bmi160_data;

static int bmi160_transceive(struct device *dev, uint8_t *tx_buf,
			     uint8_t tx_buf_len, uint8_t *rx_buf,
			     uint8_t rx_buf_len)
{
	struct bmi160_device_config *dev_cfg = dev->config->config_info;
	struct bmi160_device_data *bmi160 = dev->driver_data;
	struct spi_config spi_cfg;

	spi_cfg.config = SPI_WORD(8);
	spi_cfg.max_sys_freq = dev_cfg->spi_freq;

	if (spi_configure(bmi160->spi, &spi_cfg) < 0) {
		DBG("Cannot configure SPI bus.\n");
		return -EIO;
	}

	if (spi_slave_select(bmi160->spi, dev_cfg->spi_slave) < 0) {
		DBG("Cannot select slave.\n");
		return -EIO;
	}

	return spi_transceive(bmi160->spi, tx_buf, tx_buf_len,
			      rx_buf, rx_buf_len);
}

static int bmi160_read(struct device *dev, uint8_t reg_addr,
		       uint8_t *data, uint8_t len)
{
	uint8_t tx = reg_addr | (1 << 7);

	return bmi160_transceive(dev, &tx, 1, data, len);
}

static int bmi160_byte_read(struct device *dev, uint8_t reg_addr,
			    uint8_t *byte)
{
	uint8_t rx_buf[2];

	if (bmi160_read(dev, reg_addr, rx_buf, 2) < 0) {
		return -EIO;
	}

	*byte = rx_buf[1];

	return 0;
}

static int bmi160_word_read(struct device *dev, uint8_t reg_addr,
			    uint16_t *word)
{
	union {
		uint8_t raw[3];
		struct {
			uint8_t dummy;
			uint16_t word;
		} __packed;
	} buf;

	if (bmi160_read(dev, reg_addr, buf.raw, 3) < 0) {
		return -EIO;
	}

	*word = sys_le16_to_cpu(buf.word);

	return 0;
}

static int bmi160_byte_write(struct device *dev, uint8_t reg_addr, uint8_t byte)
{
	uint8_t tx_buf[2] = {reg_addr & 0x7F, byte};

	return bmi160_transceive(dev, tx_buf, 2, NULL, 0);
}

static int bmi160_reg_field_update(struct device *dev, uint8_t reg_addr,
				   uint8_t pos, uint8_t mask, uint8_t val)
{
	uint8_t old_val;

	if (bmi160_byte_read(dev, reg_addr, &old_val) < 0) {
		return -EIO;
	}

	return  bmi160_byte_write(dev, reg_addr,
				  (old_val & ~mask) | (val << pos));
}

static int bmi160_pmu_set(struct device *dev, union bmi160_pmu_status *pmu_sts)
{
	struct {
		uint8_t cmd;
		uint16_t delay_us; /* values taken from page 82 */
	} cmds[] = {
		{BMI160_CMD_PMU_MAG | pmu_sts->mag, 350},
		{BMI160_CMD_PMU_ACC | pmu_sts->acc, 3200},
		{BMI160_CMD_PMU_GYR | pmu_sts->gyr, 55000}
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		union bmi160_pmu_status sts;
		bool pmu_set = false;

		if (bmi160_byte_write(dev, BMI160_REG_CMD, cmds[i].cmd) < 0) {
			return -EIO;
		}

		/*
		 * Cannot use a nano timer here since this is called from the
		 * init function and the timeouts were not initialized yet.
		 */
		sys_thread_busy_wait(cmds[i].delay_us);

		/* make sure the PMU_STATUS was set, though */
		do {
			if (bmi160_byte_read(dev, BMI160_REG_PMU_STATUS,
					       &sts.raw) < 0) {
				return -EIO;
			}

			if (i == 0) {
				pmu_set = (pmu_sts->mag == sts.mag);
			} else if (i == 1) {
				pmu_set = (pmu_sts->acc == sts.acc);
			} else {
				pmu_set = (pmu_sts->gyr == sts.gyr);
			}

		} while (!pmu_set);
	}

	/* set the undersampling flag for accelerometer */
	return bmi160_reg_field_update(dev, BMI160_REG_ACC_CONF,
				       BMI160_ACC_CONF_US, BMI160_ACC_CONF_US,
				       pmu_sts->acc != BMI160_PMU_NORMAL);
}

#if defined(CONFIG_BMI160_GYRO_ODR_RUNTIME) ||\
	defined(CONFIG_BMI160_ACCEL_ODR_RUNTIME)
/*
 * Output data rate map with allowed frequencies:
 * freq = freq_int + freq_milli / 1000
 *
 * Since we don't need a finer frequency resolution than milliHz, use uint16_t
 * to save some flash.
 */
struct {
	uint16_t freq_int;
	uint16_t freq_milli; /* User should convert to uHz before setting the
			      * SENSOR_ATTR_SAMPLING_FREQUENCY attribute.
			      */
} bmi160_odr_map[] = {
	{0,    0  }, {0,     780}, {1,     562}, {3,    120}, {6,   250},
	{12,   500}, {25,    0  }, {50,    0  }, {100,  0  }, {200, 0  },
	{400,  0  }, {800,   0  }, {1600,  0  }, {3200, 0  },
};

static int bmi160_freq_to_odr_val(uint16_t freq_int, uint16_t freq_milli)
{
	int i;

	/* An ODR of 0 Hz is not allowed */
	if (freq_int == 0 && freq_milli == 0) {
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(bmi160_odr_map); i++) {
		if (freq_int == bmi160_odr_map[i].freq_int &&
		    freq_milli == bmi160_odr_map[i].freq_milli) {
			return i;
		}
	}

	return -EINVAL;
}
#endif

#if defined(CONFIG_BMI160_ACCEL_ODR_RUNTIME)
static int bmi160_acc_odr_set(struct device *dev, uint16_t freq_int,
			      uint16_t freq_milli)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;
	uint8_t odr = bmi160_freq_to_odr_val(freq_int, freq_milli);

	if (odr < 0) {
		return odr;
	}

	/* some odr values cannot be set in certain power modes */
	if ((bmi160->pmu_sts.acc == BMI160_PMU_NORMAL &&
	     odr < BMI160_ODR_25_2) ||
	    (bmi160->pmu_sts.acc == BMI160_PMU_LOW_POWER &&
	    odr < BMI160_ODR_25_32) || odr > BMI160_ODR_1600) {
		return -ENOTSUP;
	}

	return bmi160_reg_field_update(dev, BMI160_REG_ACC_CONF,
				       BMI160_ACC_CONF_ODR_POS,
				       BMI160_ACC_CONF_ODR_MASK,
				       odr);
}
#endif

static const struct bmi160_range bmi160_acc_range_map[] = {
	{2,	BMI160_ACC_RANGE_2G},
	{4,	BMI160_ACC_RANGE_4G},
	{8,	BMI160_ACC_RANGE_8G},
	{16,	BMI160_ACC_RANGE_16G},
};
#define BMI160_ACC_RANGE_MAP_SIZE	ARRAY_SIZE(bmi160_acc_range_map)

static const struct bmi160_range bmi160_gyr_range_map[] = {
	{2000,	BMI160_GYR_RANGE_2000DPS},
	{1000,	BMI160_GYR_RANGE_1000DPS},
	{500,	BMI160_GYR_RANGE_500DPS},
	{250,	BMI160_GYR_RANGE_250DPS},
	{125,	BMI160_GYR_RANGE_125DPS},
};
#define BMI160_GYR_RANGE_MAP_SIZE	ARRAY_SIZE(bmi160_gyr_range_map)

#if defined(CONFIG_BMI160_ACCEL_RANGE_RUNTIME) ||\
	defined(CONFIG_BMI160_GYRO_RANGE_RUNTIME)
static int32_t bmi160_range_to_reg_val(uint16_t range,
				       const struct bmi160_range *range_map,
				       uint16_t range_map_size)
{
	int i;

	for (i = 0; i < range_map_size; i++) {
		if (range == range_map[i].range) {
			return range_map[i].reg_val;
		}
	}

	return -EINVAL;
}
#endif

static int32_t bmi160_reg_val_to_range(uint8_t reg_val,
				       const struct bmi160_range *range_map,
				       uint16_t range_map_size)
{
	int i;

	for (i = 0; i < range_map_size; i++) {
		if (reg_val == range_map[i].reg_val) {
			return range_map[i].range;
		}
	}

	return -EINVAL;
}

static int bmi160_do_calibration(struct device *dev, uint8_t foc_conf)
{
	if (bmi160_byte_write(dev, BMI160_REG_FOC_CONF, foc_conf) < 0) {
		return -EIO;
	}

	if (bmi160_byte_write(dev, BMI160_REG_CMD, BMI160_CMD_START_FOC) < 0) {
		return -EIO;
	}

	sys_thread_busy_wait(250000); /* calibration takes a maximum of 250ms */

	return 0;
}

#if defined(CONFIG_BMI160_ACCEL_RANGE_RUNTIME)
static int bmi160_acc_range_set(struct device *dev, int32_t range)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;
	int32_t reg_val = bmi160_range_to_reg_val(range,
						  bmi160_acc_range_map,
						  BMI160_ACC_RANGE_MAP_SIZE);

	if (reg_val < 0) {
		return reg_val;
	}

	if (bmi160_byte_write(dev, BMI160_REG_ACC_RANGE, reg_val & 0xff) < 0) {
		return -EIO;
	}

	bmi160->scale.acc = BMI160_ACC_SCALE(range);

	return 0;
}
#endif

#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
/*
 * Accelerometer offset scale, taken from pg. 79, converted to micro m/s^2:
 *	3.9 * 9.80665 * 1000
 */
#define BMI160_ACC_OFS_LSB		38246
static int bmi160_acc_ofs_set(struct device *dev, enum sensor_channel chan,
			      const struct sensor_value *ofs)
{
	uint8_t reg_addr[] = {
		BMI160_REG_OFFSET_ACC_X,
		BMI160_REG_OFFSET_ACC_Y,
		BMI160_REG_OFFSET_ACC_Z
	};
	int i;
	int32_t ofs_u;
	int8_t reg_val;

	/* we need the offsets for all axis */
	if (chan != SENSOR_CHAN_ACCEL_ANY) {
		return -ENOTSUP;
	}

	for (i = 0; i < 3; i++, ofs++) {
		if (ofs->type != SENSOR_TYPE_INT_PLUS_MICRO) {
			return -EINVAL;
		}

		/* convert ofset to micro m/s^2 */
		ofs_u = ofs->val1 * 1000000ULL + ofs->val2;
		reg_val = ofs_u / BMI160_ACC_OFS_LSB;

		if (bmi160_byte_write(dev, reg_addr[i], reg_val) < 0) {
			return -EIO;
		}
	}

	/* activate accel HW compensation */
	return bmi160_reg_field_update(dev, BMI160_REG_OFFSET_EN,
				       BMI160_ACC_OFS_EN_POS,
				       BIT(BMI160_ACC_OFS_EN_POS), 1);
}

static int  bmi160_acc_calibrate(struct device *dev, enum sensor_channel chan,
				 const struct sensor_value *xyz_calib_value)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;
	uint8_t foc_pos[] = {
		BMI160_FOC_ACC_X_POS,
		BMI160_FOC_ACC_Y_POS,
		BMI160_FOC_ACC_Z_POS,
	};
	int i;
	uint8_t reg_val = 0;

	/* Calibration has to be done in normal mode. */
	if (bmi160->pmu_sts.acc != BMI160_PMU_NORMAL) {
		return -ENOTSUP;
	}

	/*
	 * Hardware calibration is done knowing the expected values on all axis.
	 */
	if (chan != SENSOR_CHAN_ACCEL_ANY) {
		return -ENOTSUP;
	}

	for (i = 0; i < 3; i++, xyz_calib_value++) {
		int32_t accel_g;
		uint8_t accel_val;

		accel_g = sensor_ms2_to_g(xyz_calib_value);
		if (accel_g == 0) {
			accel_val = 3;
		} else if (accel_g == 1) {
			accel_val = 1;
		} else if (accel_g == -1) {
			accel_val = 2;
		} else {
			accel_val = 0;
		}
		reg_val |= (accel_val << foc_pos[i]);
	}

	if (bmi160_do_calibration(dev, reg_val) < 0) {
		return -EIO;
	}

	/* activate accel HW compensation */
	return bmi160_reg_field_update(dev, BMI160_REG_OFFSET_EN,
				       BMI160_ACC_OFS_EN_POS,
				       BIT(BMI160_ACC_OFS_EN_POS), 1);
}

static int bmi160_acc_config(struct device *dev, enum sensor_channel chan,
			     enum sensor_attribute attr,
			     const struct sensor_value *val)
{
	switch (attr) {
#if defined(CONFIG_BMI160_ACCEL_RANGE_RUNTIME)
	case SENSOR_ATTR_FULL_SCALE:
		if (val->type != SENSOR_TYPE_INT_PLUS_MICRO) {
			return -EINVAL;
		}

		return bmi160_acc_range_set(dev, sensor_ms2_to_g(val));
#endif
#if defined(CONFIG_BMI160_ACCEL_ODR_RUNTIME)
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		if (val->type != SENSOR_TYPE_INT_PLUS_MICRO) {
			return -EINVAL;
		}

		return bmi160_acc_odr_set(dev, val->val1, val->val2 / 1000);
#endif
	case SENSOR_ATTR_OFFSET:
		return bmi160_acc_ofs_set(dev, chan, val);
	case SENSOR_ATTR_CALIB_TARGET:
		return bmi160_acc_calibrate(dev, chan, val);
	default:
		DBG("Accel attribute not supported.\n");
		return -ENOTSUP;
	}

	return 0;
}
#endif /* !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND) */

#if defined(CONFIG_BMI160_GYRO_ODR_RUNTIME)
static int bmi160_gyr_odr_set(struct device *dev, uint16_t freq_int,
			      uint16_t freq_milli)
{
	uint8_t odr = bmi160_freq_to_odr_val(freq_int, freq_milli);

	if (odr < 0) {
		return odr;
	}

	if (odr < BMI160_ODR_25 || odr > BMI160_ODR_3200) {
		return -ENOTSUP;
	}

	return bmi160_reg_field_update(dev, BMI160_REG_GYR_CONF,
				       BMI160_GYR_CONF_ODR_POS,
				       BMI160_GYR_CONF_ODR_MASK,
				       odr);
}
#endif

#if defined(CONFIG_BMI160_GYRO_RANGE_RUNTIME)
static int bmi160_gyr_range_set(struct device *dev, uint16_t range)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;
	int32_t reg_val = bmi160_range_to_reg_val(range,
						  bmi160_gyr_range_map,
						  BMI160_GYR_RANGE_MAP_SIZE);

	if (reg_val < 0) {
		return reg_val;
	}

	if (bmi160_byte_write(dev, BMI160_REG_GYR_RANGE, reg_val) < 0) {
		return -EIO;
	}

	bmi160->scale.gyr = BMI160_GYR_SCALE(range);

	return 0;
}
#endif

#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
/*
 * Gyro offset scale, taken from pg. 79, converted to micro rad/s:
 *		0.061 * (pi / 180) * 1000000, where pi = 3.141592
 */
#define BMI160_GYR_OFS_LSB		1065
static int bmi160_gyr_ofs_set(struct device *dev, enum sensor_channel chan,
			      const struct sensor_value *ofs)
{
	struct {
		uint8_t lsb_addr;
		uint8_t msb_pos;
	} ofs_desc[] = {
		{BMI160_REG_OFFSET_GYR_X, BMI160_GYR_MSB_OFS_X_POS},
		{BMI160_REG_OFFSET_GYR_Y, BMI160_GYR_MSB_OFS_Y_POS},
		{BMI160_REG_OFFSET_GYR_Z, BMI160_GYR_MSB_OFS_Z_POS},
	};
	int i;
	int32_t ofs_u;
	int16_t val;

	/* we need the offsets for all axis */
	if (chan != SENSOR_CHAN_GYRO_ANY) {
		return -ENOTSUP;
	}

	for (i = 0; i < 3; i++, ofs++) {
		/* convert offset to micro rad/s */
		ofs_u = ofs->val1 * 1000000ULL + ofs->val2;

		val = ofs_u / BMI160_GYR_OFS_LSB;

		/*
		 * The gyro offset is a 10 bit two-complement value. Make sure
		 * the passed value is within limits.
		 */
		if (val < -512 || val > 512) {
			return -EINVAL;
		}

		/* write the LSB */
		if (bmi160_byte_write(dev, ofs_desc[i].lsb_addr,
				      val & 0xff) < 0) {
			return -EIO;
		}

		/* write the MSB */
		if (bmi160_reg_field_update(dev, BMI160_REG_OFFSET_EN,
					    ofs_desc[i].msb_pos,
					    0x3 << ofs_desc[i].msb_pos,
					    (val >> 8) & 0x3) < 0) {
			return -EIO;
		}
	}

	/* activate gyro HW compensation */
	return bmi160_reg_field_update(dev, BMI160_REG_OFFSET_EN,
				       BMI160_GYR_OFS_EN_POS,
				       BIT(BMI160_GYR_OFS_EN_POS), 1);
}

static int bmi160_gyr_calibrate(struct device *dev, enum sensor_channel chan)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;

	/* Calibration has to be done in normal mode. */
	if (bmi160->pmu_sts.gyr != BMI160_PMU_NORMAL) {
		return -ENOTSUP;
	}

	if (bmi160_do_calibration(dev, BIT(BMI160_FOC_GYR_EN_POS)) < 0) {
		return -EIO;
	}

	/* activate gyro HW compensation */
	return bmi160_reg_field_update(dev, BMI160_REG_OFFSET_EN,
				       BMI160_GYR_OFS_EN_POS,
				       BIT(BMI160_GYR_OFS_EN_POS), 1);
}

static int bmi160_gyr_config(struct device *dev, enum sensor_channel chan,
			     enum sensor_attribute attr,
			     const struct sensor_value *val)
{
	switch (attr) {
#if defined(CONFIG_BMI160_GYRO_RANGE_RUNTIME)
	case SENSOR_ATTR_FULL_SCALE:
		if (val->type != SENSOR_TYPE_INT_PLUS_MICRO) {
			return -EINVAL;
		}

		return bmi160_gyr_range_set(dev, sensor_rad_to_degrees(val));
#endif
#if defined(CONFIG_BMI160_GYRO_ODR_RUNTIME)
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		if (val->type != SENSOR_TYPE_INT_PLUS_MICRO) {
			return -EINVAL;
		}

		return bmi160_gyr_odr_set(dev, val->val1, val->val2 / 1000);
#endif
	case SENSOR_ATTR_OFFSET:
		return bmi160_gyr_ofs_set(dev, chan, val);

	case SENSOR_ATTR_CALIB_TARGET:
		return bmi160_gyr_calibrate(dev, chan);

	default:
		DBG("Gyro attribute not supported.\n");
		return -ENOTSUP;
	}

	return 0;
}
#endif /* !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND) */

static int bmi160_attr_set(struct device *dev, enum sensor_channel chan,
		    enum sensor_attribute attr, const struct sensor_value *val)
{
	switch (chan) {
#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
	case SENSOR_CHAN_GYRO_X:
	case SENSOR_CHAN_GYRO_Y:
	case SENSOR_CHAN_GYRO_Z:
	case SENSOR_CHAN_GYRO_ANY:
		return bmi160_gyr_config(dev, chan, attr, val);
#endif
#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_ANY:
		return bmi160_acc_config(dev, chan, attr, val);
#endif
	default:
		DBG("attr_set() not supported on this channel.\n");
		return -ENOTSUP;
	}

	return 0;
}

#if defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
#	define BMI160_SAMPLE_BURST_READ_ADDR	BMI160_REG_DATA_ACC_X
#else
#	define BMI160_SAMPLE_BURST_READ_ADDR	BMI160_REG_DATA_GYR_X
#endif
static int bmi160_sample_fetch(struct device *dev)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;
	uint8_t tx = BMI160_SAMPLE_BURST_READ_ADDR | (1 << 7);
	int i;

	if (bmi160_transceive(dev, &tx, 1, bmi160->sample.raw,
			      BMI160_BUF_SIZE) < 0) {
		return -EIO;
	}

	/* convert samples to cpu endianness */
	for (i = 0; i < BMI160_SAMPLE_SIZE; i += 2) {
		uint16_t *sample =
			(uint16_t *) &bmi160->sample.raw[BMI160_DATA_OFS + i];

		*sample = sys_le16_to_cpu(*sample);
	}

	return 0;
}

static void bmi160_to_fixed_point(int16_t raw_val, uint16_t scale,
				  struct sensor_value *val)
{
	int32_t converted_val;

	val->type = SENSOR_TYPE_INT_PLUS_MICRO;

	/*
	 * maximum converted value we can get is: max(raw_val) * max(scale)
	 *	max(raw_val) = +/- 2^15
	 *	max(scale) = 4785
	 *	max(converted_val) = 156794880 which is less than 2^31
	 */
	converted_val = raw_val * scale;
	val->val1 = converted_val / 1000000;
	val->val2 = converted_val % 1000000;
}

static void bmi160_channel_convert(enum sensor_channel chan,
				   uint16_t scale,
				   uint16_t *raw_xyz,
				   struct sensor_value *val)
{
	int i;
	uint8_t ofs_start, ofs_stop;

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_GYRO_X:
		ofs_start = ofs_stop = 0;
		break;
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_GYRO_Y:
		ofs_start = ofs_stop = 1;
		break;
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_GYRO_Z:
		ofs_start = ofs_stop = 2;
		break;
	default:
		ofs_start = 0; ofs_stop = 2;
		break;
	}

	for (i = ofs_start; i <= ofs_stop ; i++, val++) {
		bmi160_to_fixed_point(raw_xyz[i], scale, val);
	}
}

#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
static inline void bmi160_gyr_channel_get(struct device *dev,
					  enum sensor_channel chan,
					  struct sensor_value *val)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;

	bmi160_channel_convert(chan, bmi160->scale.gyr,
			       bmi160->sample.gyr, val);
}
#endif

#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
static inline void bmi160_acc_channel_get(struct device *dev,
					  enum sensor_channel chan,
					  struct sensor_value *val)
{
	struct bmi160_device_data *bmi160 = dev->driver_data;

	bmi160_channel_convert(chan, bmi160->scale.acc,
			       bmi160->sample.acc, val);
}
#endif

static int bmi160_temp_channel_get(struct device *dev, struct sensor_value *val)
{
	int16_t temp_raw = 0;
	int32_t temp_micro = 0;
	struct bmi160_device_data *bmi160 = dev->driver_data;

	if (bmi160->pmu_sts.raw == 0) {
		return -EINVAL;
	}

	if (bmi160_word_read(dev, BMI160_REG_TEMPERATURE0, &temp_raw) < 0) {
		return -EIO;
	}

	/* the scale is 1/2^9/LSB = 1953 micro degrees */
	temp_micro = BMI160_TEMP_OFFSET * 1000000ULL + temp_raw * 1953ULL;

	val->type = SENSOR_TYPE_INT_PLUS_MICRO;
	val->val1 = temp_micro / 1000000ULL;
	val->val2 = temp_micro % 1000000ULL;

	return 0;
}

static int bmi160_channel_get(struct device *dev,
			      enum sensor_channel chan,
			      struct sensor_value *val)
{
	switch (chan) {
#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
	case SENSOR_CHAN_GYRO_X:
	case SENSOR_CHAN_GYRO_Y:
	case SENSOR_CHAN_GYRO_Z:
	case SENSOR_CHAN_GYRO_ANY:
		bmi160_gyr_channel_get(dev, chan, val);
		return 0;
#endif
#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_ANY:
		bmi160_acc_channel_get(dev, chan, val);
		return 0;
#endif
	case SENSOR_CHAN_TEMP:
		return bmi160_temp_channel_get(dev, val);
	default:
		DBG("Channel not supported.\n");
		return -ENOTSUP;
	}

	return 0;
}

struct sensor_driver_api bmi160_api = {
	.attr_set = bmi160_attr_set,
	.sample_fetch = bmi160_sample_fetch,
	.channel_get = bmi160_channel_get,
};

int bmi160_init(struct device *dev)
{
	struct bmi160_device_config *cfg = dev->config->config_info;
	struct bmi160_device_data *bmi160 = dev->driver_data;
	uint8_t val = 0;
	int32_t acc_range, gyr_range;

	bmi160->spi = device_get_binding((char *)cfg->spi_port);
	if (!bmi160->spi) {
		DBG("SPI master controller not found: %d.\n", bmi160->spi);
		return -EINVAL;
	}

	dev->driver_api = &bmi160_api;

	/* reboot the chip */
	if (bmi160_byte_write(dev, BMI160_REG_CMD, BMI160_CMD_SOFT_RESET) < 0) {
		DBG("Cannot reboot chip.\n");
		return -EIO;
	}

	sys_thread_busy_wait(1000);

	/* do a dummy read from 0x7F to activate SPI */
	if (bmi160_byte_read(dev, 0x7F, &val) < 0) {
		DBG("Cannot read from 0x7F..\n");
		return -EIO;
	}

	sys_thread_busy_wait(100);

	if (bmi160_byte_read(dev, BMI160_REG_CHIPID, &val) < 0) {
		DBG("Failed to read chip id.\n");
		return -EIO;
	}

	if (val != BMI160_CHIP_ID) {
		DBG("Unsupported chip detected (0x%x)!\n", val);
		return -ENODEV;
	}

	/* set default PMU for gyro, accelerometer */
	bmi160->pmu_sts.gyr = BMI160_DEFAULT_PMU_GYR;
	bmi160->pmu_sts.acc = BMI160_DEFAULT_PMU_ACC;
	/* compass not supported, yet */
	bmi160->pmu_sts.mag = BMI160_PMU_SUSPEND;

	/*
	 * The next command will take around 100ms (contains some necessary busy
	 * waits), but we cannot do it in a separate fiber since we need to
	 * guarantee the BMI is up and running, befoare the app's main() is
	 * called.
	 */
	if (bmi160_pmu_set(dev, &bmi160->pmu_sts) < 0) {
		DBG("Failed to set power mode.\n");
		return -EIO;
	}

	/* set accelerometer default range */
	if (bmi160_byte_write(dev, BMI160_REG_ACC_RANGE,
				BMI160_DEFAULT_RANGE_ACC) < 0) {
		DBG("Cannot set default range for accelerometer.\n");
		return -EIO;
	}

	acc_range = bmi160_reg_val_to_range(BMI160_DEFAULT_RANGE_ACC,
					    bmi160_acc_range_map,
					    BMI160_ACC_RANGE_MAP_SIZE);

	bmi160->scale.acc = BMI160_ACC_SCALE(acc_range);

	/* set gyro default range */
	if (bmi160_byte_write(dev, BMI160_REG_GYR_RANGE,
			      BMI160_DEFAULT_RANGE_GYR) < 0) {
		DBG("Cannot set default range for gyroscope.\n");
		return -EIO;
	}

	gyr_range = bmi160_reg_val_to_range(BMI160_DEFAULT_RANGE_GYR,
					    bmi160_gyr_range_map,
					    BMI160_GYR_RANGE_MAP_SIZE);

	bmi160->scale.gyr = BMI160_GYR_SCALE(gyr_range);

	if (bmi160_reg_field_update(dev, BMI160_REG_ACC_CONF,
				    BMI160_ACC_CONF_ODR_POS,
				    BMI160_ACC_CONF_ODR_MASK,
				    BMI160_DEFAULT_ODR_ACC) < 0) {
		DBG("Failed to set accel's default ODR.\n");
		return -EIO;
	}

	if (bmi160_reg_field_update(dev, BMI160_REG_GYR_CONF,
				    BMI160_GYR_CONF_ODR_POS,
				    BMI160_GYR_CONF_ODR_MASK,
				    BMI160_DEFAULT_ODR_GYR) < 0) {
		DBG("Failed to set gyro's default ODR.\n");
		return -EIO;
	}

	return 0;
}

struct bmi160_device_config bmi160_config = {
	.spi_port = CONFIG_BMI160_SPI_PORT_NAME,
	.spi_freq = CONFIG_BMI160_SPI_BUS_FREQ,
	.spi_slave = CONFIG_BMI160_SLAVE,
};

DEVICE_INIT(bmi160, CONFIG_BMI160_NAME, bmi160_init, &bmi160_data,
	    &bmi160_config, NANOKERNEL, CONFIG_BMI160_INIT_PRIORITY);
