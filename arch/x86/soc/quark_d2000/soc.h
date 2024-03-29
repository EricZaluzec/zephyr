/*
 * Copyright (c) 2015 Wind River Systems, Inc.
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

/**
 * @brief board configuration macros for the Quark D2000
 * This header file is used to specify and describe board-level aspects for
 * the Quark D2000 Platform.
 */

#ifndef __SOC_H_
#define __SOC_H_

#include <stdint.h>
#include <misc/util.h>
#include <uart.h>
#include <drivers/ioapic.h>

#define INT_VEC_IRQ0  0x20 /* Vector number for IRQ0 */
#define FIXED_HARDWARE_IRQ_TO_VEC_MAPPING(x) (INT_VEC_IRQ0 + x)
#define IOAPIC_LO32_RTE_SUPPORTED_MASK (IOAPIC_INT_MASK | IOAPIC_TRIGGER_MASK)

/* Base Register */
#define SCSS_REGISTER_BASE              0xB0800000

/* Clock */
#define CLOCK_PERIPHERAL_BASE_ADDR	(SCSS_REGISTER_BASE + 0x18)
#define CLOCK_EXTERNAL_BASE_ADDR	(SCSS_REGISTER_BASE + 0x24)
#define CLOCK_SENSOR_BASE_ADDR		(SCSS_REGISTER_BASE + 0x28)
#define CLOCK_SYSTEM_CLOCK_CONTROL      (SCSS_REGISTER_BASE + 0x38)

struct scss_peripheral {
	volatile uint32_t usb_phy_cfg0; /**< USB Configuration */
	volatile uint32_t periph_cfg0;  /**< Peripheral Configuration */
	volatile uint32_t reserved[2];
	volatile uint32_t cfg_lock; /**< Configuration Lock */
};

struct int_ss_i2c {
	volatile uint32_t err_mask;
	volatile uint32_t rx_avail_mask;
	volatile uint32_t tx_req_mask;
	volatile uint32_t stop_det_mask;
};

struct int_ss_spi {
	volatile uint32_t err_int_mask;
	volatile uint32_t rx_avail_mask;
	volatile uint32_t tx_req_mask;
};


struct scss_interrupt {
	volatile uint32_t int_ss_adc_err_mask;
	volatile uint32_t int_ss_adc_irq_mask;
	volatile uint32_t int_ss_gpio_intr_mask[2];
	struct int_ss_i2c int_ss_i2c[2];
	struct int_ss_spi int_ss_spi[2];
	volatile uint32_t int_i2c_mst_mask[2];
	volatile uint32_t reserved;
	volatile uint32_t int_spi_mst_mask[2];
	volatile uint32_t int_spi_slv_mask[1];
	volatile uint32_t int_uart_mask[2];
	volatile uint32_t int_i2s_mask;
	volatile uint32_t int_gpio_mask;
	volatile uint32_t int_pwm_timer_mask;
	volatile uint32_t int_usb_mask;
	volatile uint32_t int_rtc_mask;
	volatile uint32_t int_watchdog_mask;
	volatile uint32_t int_dma_channel_mask[8];
	volatile uint32_t int_mailbox_mask;
	volatile uint32_t int_comparators_ss_halt_mask;
	volatile uint32_t int_comparators_host_halt_mask;
	volatile uint32_t int_comparators_ss_mask;
	volatile uint32_t int_comparators_host_mask;
	volatile uint32_t int_host_bus_err_mask;
	volatile uint32_t int_dma_error_mask;
	volatile uint32_t int_sram_controller_mask;
	volatile uint32_t int_flash_controller_mask[2];
	volatile uint32_t int_aon_timer_mask;
	volatile uint32_t int_adc_pwr_mask;
	volatile uint32_t int_adc_calib_mask;
	volatile uint32_t int_aon_gpio_mask;
	volatile uint32_t lock_int_mask_reg;
};

#define SCSS_PERIPHERAL_BASE (0xB0800800)
#define SCSS_PERIPHERAL ((struct scss_peripheral *)SCSS_PERIPHERAL_BASE)

#define SCSS_INT_BASE (0xB0800400)
#define SCSS_INTERRUPT ((struct scss_interrupt *)SCSS_INT_BASE)

/* Peripheral Clock Gate Control */
#define SCSS_CCU_PERIPH_CLK_GATE_CTL	0x18
#define CCU_PERIPH_CLK_EN		(1 << 1)
#define CCU_PERIPH_CLK_DIV_CTL0		0x1C
#define INT_UNMASK_IA			(~0x00000001)
/*
 * Local APIC (LOAPIC) device information (Intel loapic)
 */

#define LOAPIC_IRQ_BASE			CONFIG_LOAPIC_TIMER_IRQ
#define LOAPIC_IRQ_COUNT		1
#define LOAPIC_LVT_REG_SPACING  0x10

/* Watchdog */
#define WDT_DW_INT_MASK			(SCSS_INT_BASE + 0x7C)
#define SCSS_PERIPH_CFG0		0x4

/* RTC */
#define RTC_DW_INT_MASK			(SCSS_INT_BASE + 0x78)
#define CCU_RTC_CLK_DIV_OFFSET          0x3

/*
 * I2C
 */
#define I2C_MST_0_INT_MASK		(SCSS_INT_BASE + 0x48)

#define I2C_DW_0_BASE_ADDR		0xB0002800
#define I2C_DW_0_IRQ			4

#if defined(CONFIG_MVIC)
#define I2C_DW_IRQ_FLAGS		(IOAPIC_EDGE | IOAPIC_HIGH)
#endif

/*
 * GPIO
 */
#define GPIO_DW_PORT_0_INT_MASK		(SCSS_INT_BASE + 0x6C)

#define GPIO_DW_0_BASE_ADDR		0xB0000C00
#define GPIO_DW_0_IRQ			15
#define GPIO_DW_0_BITS			26

#if defined(CONFIG_MVIC)
#define GPIO_DW_0_IRQ_FLAGS		(IOAPIC_EDGE | IOAPIC_HIGH)
#endif

/* Comparator */
#define INT_AIO_CMP_IRQ			(0x0E)

/*
 * PINMUX configuration settings
 */
#if defined(CONFIG_PINMUX)

#define PINMUX_BASE_ADDR		0xb0800900
#define PINMUX_NUM_PINS			25

#endif /* CONFIG_PINMUX */

/*
 * RTC (Real Time Clock)
 */
#define RTC_DW_BASE_ADDR		0xB0000400
#define RTC_DW_IRQ			2

/*
 * UART
 */
#define UART_NS16550_PORT_0_BASE_ADDR	0xB0002000
#define UART_NS16550_PORT_0_IRQ		8
#define UART_NS16550_PORT_0_CLK_FREQ	MHZ(32)

#define UART_NS16550_PORT_1_BASE_ADDR	0xB0002400
#define UART_NS16550_PORT_1_IRQ		6
#define UART_NS16550_PORT_1_CLK_FREQ	MHZ(32)

#ifdef CONFIG_MVIC
#include <drivers/ioapic.h>
#define UART_IRQ_FLAGS			(IOAPIC_EDGE | IOAPIC_HIGH)
#endif /* CONFIG_MVIC */

/*
 * WDT/Watchdog
 */
#define WDT_DW_BASE_ADDR		0xB0000000
#define WDT_DW_IRQ			16

#endif /* __SOC_H_ */
