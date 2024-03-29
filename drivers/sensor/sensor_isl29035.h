/* sensor_isl29035.h - header file for ISL29035 light sensor driver */

/*
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
 */

#ifndef _SENSOR_ISL29035_H_
#define _SENSOR_ISL29035_H_

#include <device.h>
#include <nanokernel.h>
#include <sensor.h>

#ifndef CONFIG_SENSOR_DEBUG
#define DBG(...) { ; }
#else
#include <misc/printk.h>
#define DBG printk
#endif /* CONFIG_SENSOR_DEBUG */

#define ISL29035_I2C_ADDRESS		0x44

#define ISL29035_COMMAND_I_REG		0x00
#define ISL29035_OPMODE_SHIFT		5
#define ISL29035_OPMODE_MASK		(7 << ISL29035_OPMODE_SHIFT)
#define ISL29035_INT_BIT_SHIFT		2
#define ISL29035_INT_BIT_MASK		(1 << ISL29035_INT_BIT_SHIFT)
#define ISL29035_INT_PRST_SHIFT		0
#define ISL29035_INT_PRST_MASK		(3 << ISL29035_INT_BIT_SHIFT)

#define ISL29035_OPMODE_OFF		0
#define ISL29035_OPMODE_ALS_ONCE	1
#define ISL29035_OPMODE_IR_ONCE		2
#define ISL29035_OPMODE_ALS_CONT	5
#define ISL29035_OPMODE_IR_CONT		6

#define ISL29035_COMMAND_II_REG		0x01
#define ISL29035_LUX_RANGE_SHIFT	0
#define ISL29035_LUX_RANGE_MASK		(3 << ISL29035_LUX_RANGE_SHIFT)
#define ISL29035_ADC_RES_SHIFT		2
#define ISL29035_ADC_RES_MASK		(3 << ISL29035_ADC_RES_SHIFT)

#define ISL29035_DATA_LSB_REG		0x02
#define ISL29035_DATA_MSB_REG		0x03
#define ISL29035_INT_LT_LSB_REG		0x04
#define ISL29035_INT_LT_MSB_REG		0x05
#define ISL29035_INT_HT_LSB_REG		0x06
#define ISL29035_INT_HT_MSB_REG		0x07

#define ISL29035_ID_REG			0x0F
#define ISL29035_BOUT_SHIFT		7
#define ISL29035_BOUT_MASK		(1 << ISL29035_BOUT_SHIFT)
#define ISL29035_ID_SHIFT		3
#define ISL29035_ID_MASK		(3 << ISL29035_ID_SHIFT)

#if CONFIG_ISL29035_MODE_ALS
	#define ISL29035_ACTIVE_OPMODE		ISL29035_OPMODE_ALS_CONT
	#define ISL29035_ACTIVE_CHAN		SENSOR_CHAN_LIGHT
#elif CONFIG_ISL29035_MODE_IR
	#define ISL29035_ACTIVE_OPMODE		ISL29035_OPMODE_IR_CONT
	#define ISL29035_ACTIVE_CHAN		SENSOR_CHAN_IR
#endif

#define ISL29035_ACTIVE_OPMODE_BITS		\
	(ISL29035_ACTIVE_OPMODE << ISL29035_OPMODE_SHIFT)

#if CONFIG_ISL29035_LUX_RANGE_1K
	#define ISL29035_LUX_RANGE_IDX		0
	#define ISL29035_LUX_RANGE		1000
#elif CONFIG_ISL29035_LUX_RANGE_4
	#define ISL29035_LUX_RANGE_IDX		1
	#define ISL29035_LUX_RANGE		4000
#elif CONFIG_ISL29035_LUX_RANGE_16K
	#define ISL29035_LUX_RANGE_IDX		2
	#define ISL29035_LUX_RANGE		16000
#elif CONFIG_ISL29035_LUX_RANGE_64K
	#define ISL29035_LUX_RANGE_IDX		3
	#define ISL29035_LUX_RANGE		64000
#endif

#define ISL29035_LUX_RANGE_BITS			\
	(ISL29035_LUX_RANGE_IDX << ISL29035_LUX_RANGE_SHIFT)

#if CONFIG_ISL29035_INTEGRATION_TIME_26
	#define ISL29035_ADC_RES_IDX		3
#elif CONFIG_ISL29035_INTEGRATION_TIME_410
	#define ISL29035_ADC_RES_IDX		2
#elif CONFIG_ISL29035_INTEGRATION_TIME_6500
	#define ISL29035_ADC_RES_IDX		1
#elif CONFIG_ISL29035_INTEGRATION_TIME_105K
	#define ISL29035_ADC_RES_IDX		0
#endif

#define ISL29035_ADC_RES_BITS			\
	(ISL29035_ADC_RES_IDX << ISL29035_ADC_RES_SHIFT)

#define ISL29035_ADC_DATA_BITS	(16 - 4 * ISL29035_ADC_RES_IDX)
#define ISL29035_ADC_DATA_MASK	(0xFFFF >> (16 - ISL29035_ADC_DATA_BITS))

#if CONFIG_ISL29035_INT_PERSIST_1
	#define ISL29035_INT_PRST_IDX		0
	#define ISL29035_INT_PRST_CYCLES	1
#elif CONFIG_ISL29035_INT_PERSIST_4
	#define ISL29035_INT_PRST_IDX		1
	#define ISL29035_INT_PRST_CYCLES	4
#elif CONFIG_ISL29035_INT_PERSIST_8
	#define ISL29035_INT_PRST_IDX		2
	#define ISL29035_INT_PRST_CYCLES	8
#elif CONFIG_ISL29035_INT_PERSIST_16
	#define ISL29035_INT_PRST_IDX		3
	#define ISL29035_INT_PRST_CYCLES	16
#endif

#define ISL29035_INT_PRST_BITS			\
	(ISL29035_INT_PRST_IDX << ISL29035_INT_PRST_SHIFT)

struct isl29035_driver_data {
	struct device *i2c;
	uint16_t data_sample;

#if CONFIG_ISL29035_TRIGGER
	struct device *gpio;

	struct sensor_trigger th_trigger;
	sensor_trigger_handler_t th_handler;

#if defined(CONFIG_ISL29035_TRIGGER_OWN_FIBER)
	char __stack fiber_stack[CONFIG_ISL29035_FIBER_STACK_SIZE];
	struct nano_sem gpio_sem;
#elif defined(CONFIG_ISL29035_TRIGGER_GLOBAL_FIBER)
	struct sensor_work work;
#endif

#endif /* CONFIG_ISL29035_TRIGGER */
};

#ifdef CONFIG_ISL29035_TRIGGER
int isl29035_write_reg(struct isl29035_driver_data *drv_data,
		       uint8_t reg, uint8_t val);

int isl29035_read_reg(struct isl29035_driver_data *drv_data,
		      uint8_t reg, uint8_t *val);

int isl29035_update_reg(struct isl29035_driver_data *drv_data,
			uint8_t reg, uint8_t mask, uint8_t val);

int isl29035_attr_set(struct device *dev,
		      enum sensor_channel chan,
		      enum sensor_attribute attr,
		      const struct sensor_value *val);

int isl29035_trigger_set(struct device *dev,
			 const struct sensor_trigger *trig,
			 sensor_trigger_handler_t handler);

int isl29035_init_interrupt(struct device *dev);
#endif

#endif /* _SENSOR_ISL29035_H_ */
