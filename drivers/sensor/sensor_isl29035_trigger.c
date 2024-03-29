/* sensor_isl29035.c - trigger support for ISL29035 light sensor */

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

#include <gpio.h>
#include <misc/util.h>
#include <nanokernel.h>

#include "sensor_isl29035.h"

extern struct isl29035_driver_data isl29035_data;

static uint16_t isl29035_lux_processed_to_raw(struct sensor_value const *val)
{
	uint64_t raw_val, ival, uval;

	ival = val->val1;
	if (val->type == SENSOR_TYPE_INT) {
		uval = 0;
	} else {
		uval = val->val2;
	}

	/* raw_val = val * (2 ^ adc_data_bits) / lux_range */
	raw_val = (ival << ISL29035_ADC_DATA_BITS) +
		  (uval << ISL29035_ADC_DATA_BITS) / 1000000;

	return raw_val / ISL29035_LUX_RANGE;
}

int isl29035_attr_set(struct device *dev,
		      enum sensor_channel chan,
		      enum sensor_attribute attr,
		      const struct sensor_value *val)
{
	struct isl29035_driver_data *drv_data = dev->driver_data;
	uint8_t lsb_reg, msb_reg;
	uint16_t raw_val;

	if (attr == SENSOR_ATTR_UPPER_THRESH) {
		lsb_reg = ISL29035_INT_HT_LSB_REG;
		msb_reg = ISL29035_INT_HT_MSB_REG;
	} else if (attr == SENSOR_ATTR_LOWER_THRESH) {
		lsb_reg = ISL29035_INT_LT_LSB_REG;
		msb_reg = ISL29035_INT_LT_MSB_REG;
	} else {
		return -ENOTSUP;
	}

	raw_val = isl29035_lux_processed_to_raw(val);

	if (isl29035_write_reg(drv_data, lsb_reg, raw_val & 0xFF) != 0 ||
	    isl29035_write_reg(drv_data, msb_reg, raw_val >> 8) != 0) {
		DBG("Failed to set attribute.\n");
		return -EIO;
	}

	return 0;
}

static void isl29035_gpio_callback(struct device *dev, uint32_t pin)
{
	gpio_pin_disable_callback(dev, pin);

#if defined(CONFIG_ISL29035_TRIGGER_OWN_FIBER)
	nano_sem_give(&isl29035_data.gpio_sem);
#elif defined(CONFIG_ISL29035_TRIGGER_GLOBAL_FIBER)
	nano_isr_fifo_put(sensor_get_work_fifo(), &isl29035_data.work);
#endif
}

static void isl29035_fiber_cb(void *arg)
{
	struct device *dev = arg;
	struct isl29035_driver_data *drv_data = dev->driver_data;
	uint8_t val;

	/* clear interrupt */
	isl29035_read_reg(drv_data, ISL29035_COMMAND_I_REG, &val);

	if (drv_data->th_handler != NULL) {
		drv_data->th_handler(dev, &drv_data->th_trigger);
	}

	gpio_pin_enable_callback(drv_data->gpio, CONFIG_ISL29035_GPIO_PIN_NUM);
}

#ifdef CONFIG_ISL29035_TRIGGER_OWN_FIBER
static void isl29035_fiber(int ptr, int unused)
{
	struct device *dev = INT_TO_POINTER(ptr);
	struct isl29035_driver_data *drv_data = dev->driver_data;

	ARG_UNUSED(unused);

	while (1) {
		nano_fiber_sem_take(&drv_data->gpio_sem, TICKS_UNLIMITED);
		isl29035_fiber_cb(dev);
	}
}
#endif

int isl29035_trigger_set(struct device *dev,
			 const struct sensor_trigger *trig,
			 sensor_trigger_handler_t handler)
{
	struct isl29035_driver_data *drv_data = dev->driver_data;

	/* disable interrupt callback while changing parameters */
	gpio_pin_disable_callback(drv_data->gpio, CONFIG_ISL29035_GPIO_PIN_NUM);

	drv_data->th_handler = handler;
	drv_data->th_trigger = *trig;

	/* enable interrupt callback */
	gpio_pin_enable_callback(drv_data->gpio, CONFIG_ISL29035_GPIO_PIN_NUM);

	return 0;
}

int isl29035_init_interrupt(struct device *dev)
{
	struct isl29035_driver_data *drv_data = dev->driver_data;
	int ret;

	/* set interrupt persistence */
	ret = isl29035_update_reg(drv_data, ISL29035_COMMAND_I_REG,
				  ISL29035_INT_PRST_MASK,
				  ISL29035_INT_PRST_BITS);
	if (ret != 0) {
		DBG("Failed to set interrupt persistence cycles.\n");
		return -EIO;
	}

	/* setup gpio interrupt */
	drv_data->gpio = device_get_binding(CONFIG_ISL29035_GPIO_DEV_NAME);
	if (drv_data->gpio == NULL) {
		DBG("Failed to get GPIO device.\n");
		return -EINVAL;
	}

	gpio_pin_configure(drv_data->gpio, CONFIG_ISL29035_GPIO_PIN_NUM,
			   GPIO_DIR_IN | GPIO_INT | GPIO_INT_LEVEL |
			   GPIO_INT_ACTIVE_LOW | GPIO_INT_DEBOUNCE);

	ret = gpio_set_callback(drv_data->gpio, isl29035_gpio_callback);
	if (ret != 0) {
		DBG("Failed to set gpio callback.\n");
		return -EIO;
	}

#if defined(CONFIG_ISL29035_TRIGGER_OWN_FIBER)
	nano_sem_init(&drv_data->gpio_sem);

	fiber_start(drv_data->fiber_stack, CONFIG_ISL29035_FIBER_STACK_SIZE,
		    (nano_fiber_entry_t)isl29035_fiber, POINTER_TO_INT(dev),
		    0, CONFIG_ISL29035_FIBER_PRIORITY, 0);
#elif defined(CONFIG_ISL29035_TRIGGER_GLOBAL_FIBER)
	drv_data->work.handler = isl29035_fiber_cb;
	drv_data->work.arg = dev;
#endif

	return 0;
}
