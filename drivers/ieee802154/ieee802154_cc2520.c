/* ieee802154_cc2520.c - IEEE 802.15.4 driver for TI CC2520 */

/*
 * Copyright (c) 2016 Intel Corporation.
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

#include <nanokernel.h>
#include <arch/cpu.h>

#include <board.h>
#include <device.h>
#include <init.h>

#include <misc/byteorder.h>
#include <string.h>
#include <rand32.h>

#include <gpio.h>

#include <net/l2_buf.h>
#include <packetbuf.h>

#define CONFIG_NETWORKING_LEGACY_RADIO_DRIVER

#ifdef CONFIG_NETWORKING_LEGACY_RADIO_DRIVER
#include <dev/radio.h>
#include <net_driver_15_4.h>
static struct device *cc2520_sglt;
#endif /* CONFIG_NETWORKING_LEGACY_RADIO_DRIVER */

#include "ieee802154_cc2520.h"

/**
 * Content is split as follows:
 * 1 - Debug related functions
 * 2 - Generic helper functions (for any parts)
 * 3 - GPIO related functions
 * 4 - TX related helper functions
 * 5 - RX related helper functions
 * 6 - Radio device API functions
 * 7 - Legacy radio device API functions
 * 8 - Initialization
 */


#if defined(CONFIG_TI_CC2520_AUTO_CRC) &&  defined(CONFIG_TI_CC2520_AUTO_ACK)
#define CC2520_AUTOMATISM		(FRMCTRL0_AUTOCRC | FRMCTRL0_AUTOACK)
#elif defined(CONFIG_TI_CC2520_AUTO_CRC)
#define CC2520_AUTOMATISM		(FRMCTRL0_AUTOCRC)
#else
#define CC2520_AUTOMATISM		(0)
#endif

#if defined(CONFIG_TI_CC2520_AUTO_ACK)
#define CC2520_FRAME_FILTERING		(FRMFILT0_FRAME_FILTER_EN)
#else
#define CC2520_FRAME_FILTERING		(0)
#endif

#define CC2520_TX_THRESHOLD		(0x7F)
#define CC2520_FCS_LENGTH		(2)

/*********
 * DEBUG *
 ********/
#ifndef CONFIG_TI_CC2520_DEBUG
#define _cc2520_print_gpio_config(...)
#define _cc2520_print_exceptions(...)
#define _cc2520_print_errors(...)
#else
static inline void _cc2520_print_gpio_config(struct device *dev)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	DBG("%s: GPIOCTRL0/1/2/3/4/5 = 0x%x/0x%x/0x%x/0x%x/0x%x/0x%x\n",
	    __func__,
	    read_reg_gpioctrl0(&cc2520->spi),
	    read_reg_gpioctrl1(&cc2520->spi),
	    read_reg_gpioctrl2(&cc2520->spi),
	    read_reg_gpioctrl3(&cc2520->spi),
	    read_reg_gpioctrl4(&cc2520->spi),
	    read_reg_gpioctrl5(&cc2520->spi));
	DBG("%s: GPIOPOLARITY: 0x%x\n",
	    __func__, read_reg_gpiopolarity(&cc2520->spi));
	DBG("%s: GPIOCTRL: 0x%x\n",
	    __func__, read_reg_gpioctrl(&cc2520->spi));
}

static inline void _cc2520_print_exceptions(struct cc2520_context *cc2520)
{
	uint8_t flag = read_reg_excflag0(&cc2520->spi);

	DBG("%s: EXCFLAG0: ", __func__);
	if (flag & EXCFLAG0_RF_IDLE) {
		DBG("RF_IDLE ");
	}
	if (flag & EXCFLAG0_TX_FRM_DONE) {
		DBG("TX_FRM_DONE ");
	}
	if (flag & EXCFLAG0_TX_ACK_DONE) {
		DBG("TX_ACK_DONE ");
	}
	if (flag & EXCFLAG0_TX_UNDERFLOW) {
		DBG("TX_UNDERFLOW ");
	}
	if (flag & EXCFLAG0_TX_OVERFLOW) {
		DBG("TX_OVERFLOW ");
	}
	if (flag & EXCFLAG0_RX_UNDERFLOW) {
		DBG("RX_UNDERFLOW ");
	}
	if (flag & EXCFLAG0_RX_OVERFLOW) {
		DBG("RX_OVERFLOW ");
	}
	if (flag & EXCFLAG0_RXENABLE_ZERO) {
		DBG("RXENABLE_ZERO");
	}
	DBG("\n");

	flag = read_reg_excflag1(&cc2520->spi);

	DBG("%s: EXCFLAG1: ", __func__);
	if (flag & EXCFLAG1_RX_FRM_DONE) {
		DBG("RX_FRM_DONE ");
	}
	if (flag & EXCFLAG1_RX_FRM_ACCEPTED) {
		DBG("RX_FRM_ACCEPTED ");
	}
	if (flag & EXCFLAG1_SRC_MATCH_DONE) {
		DBG("SRC_MATCH_DONE ");
	}
	if (flag & EXCFLAG1_SRC_MATCH_FOUND) {
		DBG("SRC_MATCH_FOUND ");
	}
	if (flag & EXCFLAG1_FIFOP) {
		DBG("FIFOP ");
	}
	if (flag & EXCFLAG1_SFD) {
		DBG("SFD ");
	}
	if (flag & EXCFLAG1_DPU_DONE_L) {
		DBG("DPU_DONE_L ");
	}
	if (flag & EXCFLAG1_DPU_DONE_H) {
		DBG("DPU_DONE_H");
	}
	DBG("\n");
}

static inline void _cc2520_print_errors(struct cc2520_context *cc2520)
{
	uint8_t flag = read_reg_excflag2(&cc2520->spi);

	DBG("EXCFLAG2: ");
	if (flag & EXCFLAG2_MEMADDR_ERROR) {
		DBG("MEMADDR_ERROR ");
	}
	if (flag & EXCFLAG2_USAGE_ERROR) {
		DBG("USAGE_ERROR ");
	}
	if (flag & EXCFLAG2_OPERAND_ERROR) {
		DBG("OPERAND_ERROR ");
	}
	if (flag & EXCFLAG2_SPI_ERROR) {
		DBG("SPI_ERROR ");
	}
	if (flag & EXCFLAG2_RF_NO_LOCK) {
		DBG("RF_NO_LOCK ");
	}
	if (flag & EXCFLAG2_RX_FRM_ABORTED) {
		DBG("RX_FRM_ABORTED ");
	}
	if (flag & EXCFLAG2_RFBUFMOV_TIMEOUT) {
		DBG("RFBUFMOV_TIMEOUT");
	}
	DBG("\n");
}
#endif


/*********************
 * Generic functions *
 ********************/
static void _usleep(uint32_t usec)
{
	static void (*func[3])(int32_t timeout_in_ticks) = {
		NULL,
		fiber_sleep,
		task_sleep,
	};

	if (sys_execution_context_type_get() == 0) {
		sys_thread_busy_wait(usec);
		return;
	}

	/* Timeout in ticks: */
	usec = USEC(usec);
	/** Most likely usec will generate 0 ticks,
	 * so setting at least to 1
	 */
	if (!usec) {
		usec = 1;
	}

	func[sys_execution_context_type_get()](usec);
}

uint8_t _cc2520_read_reg(struct cc2520_spi *spi,
			 bool freg, uint8_t addr)
{
	spi->cmd_buf[0] = freg ? CC2520_INS_MEMRD : CC2520_INS_REGRD;
	spi->cmd_buf[1] = addr;
	spi->cmd_buf[2] = 0;

	spi_slave_select(spi->dev, spi->slave);

	if (spi_transceive(spi->dev, spi->cmd_buf, 3,
			   spi->cmd_buf, 3) == DEV_OK) {
		return spi->cmd_buf[2];
	}

	return 0;
}

bool _cc2520_write_reg(struct cc2520_spi *spi, bool freg,
		       uint8_t addr, uint8_t value)
{
	spi->cmd_buf[0] = freg ? CC2520_INS_MEMWR : CC2520_INS_REGWR;
	spi->cmd_buf[1] = addr;
	spi->cmd_buf[2] = value;

	spi_slave_select(spi->dev, spi->slave);

	return (spi_write(spi->dev, spi->cmd_buf, 3) == DEV_OK);
}

bool _cc2520_write_ram(struct cc2520_spi *spi, uint16_t addr,
		       uint8_t *data_buf, uint8_t len)
{
	spi->cmd_buf[0] = CC2520_INS_MEMWR | (addr >> 8);
	spi->cmd_buf[1] = addr;

	memcpy(&spi->cmd_buf[2], data_buf, len);

	spi_slave_select(spi->dev, spi->slave);

	return (spi_write(spi->dev, spi->cmd_buf, len + 2) == DEV_OK);
}

static uint8_t _cc2520_status(struct cc2520_spi *spi)
{
	spi->cmd_buf[0] = CC2520_INS_SNOP;

	spi_slave_select(spi->dev, spi->slave);

	if (spi_transceive(spi->dev, spi->cmd_buf, 1,
			   spi->cmd_buf, 1) == DEV_OK) {
		return spi->cmd_buf[0];
	}

	return 0;
}

static bool verify_osc_stabilization(struct cc2520_context *cc2520)
{
	uint8_t timeout = 100;
	uint8_t status;

	do {
		status = _cc2520_status(&cc2520->spi);
		_usleep(1);
		timeout--;
	} while (!(status & CC2520_STATUS_XOSC_STABLE_N_RUNNING) && timeout);

	return !!(status & CC2520_STATUS_XOSC_STABLE_N_RUNNING);
}


/******************
 * GPIO functions *
 *****************/
static inline void set_reset(struct device *dev, uint32_t value)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	gpio_pin_write(cc2520->gpios[CC2520_GPIO_IDX_RESET],
		       CONFIG_CC2520_GPIO_RESET, value);
}

static inline void set_vreg_en(struct device *dev, uint32_t value)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	gpio_pin_write(cc2520->gpios[CC2520_GPIO_IDX_VREG_EN],
		       CONFIG_CC2520_GPIO_VREG_EN, value);
}

static inline uint32_t get_fifo(struct cc2520_context *cc2520)
{
	uint32_t pin_value;

	gpio_pin_read(cc2520->gpios[CC2520_GPIO_IDX_FIFO],
		      CONFIG_CC2520_GPIO_FIFO, &pin_value);

	return pin_value;
}

static inline uint32_t get_fifop(struct cc2520_context *cc2520)
{
	uint32_t pin_value;

	gpio_pin_read(cc2520->gpios[CC2520_GPIO_IDX_FIFOP],
		      CONFIG_CC2520_GPIO_FIFOP, &pin_value);

	return pin_value;
}

static inline uint32_t get_cca(struct cc2520_context *cc2520)
{
	uint32_t pin_value;

	gpio_pin_read(cc2520->gpios[CC2520_GPIO_IDX_CCA],
		      CONFIG_CC2520_GPIO_CCA, &pin_value);

	return pin_value;
}

static inline void sfd_int_handler(struct device *port, uint32_t pin)
{
	struct cc2520_context *cc2520 = cc2520_sglt->driver_data;

	if (atomic_get(&cc2520->tx) == 1) {
		atomic_set(&cc2520->tx, 0);
		device_sync_call_complete(&cc2520->tx_sync);
	}
}

static inline void fifop_int_handler(struct device *port, uint32_t pin)
{
	struct cc2520_context *cc2520 = cc2520_sglt->driver_data;

	/* Note: Errata document - 1.2 */
	if (!get_fifop(cc2520) && !get_fifop(cc2520)) {
		return;
	}

	if (!get_fifo(cc2520)) {
		cc2520->overflow = true;
	}

	nano_isr_sem_give(&cc2520->rx_lock);
}

static void gpio_int_handler(struct device *port, uint32_t pin)
{
	if (pin == CONFIG_CC2520_GPIO_SFD) {
		sfd_int_handler(port, pin);
	} else if (pin == CONFIG_CC2520_GPIO_FIFOP) {
		fifop_int_handler(port, pin);
	}
}

static void enable_fifop_interrupt(struct cc2520_context *cc2520,
				   bool enable)
{
	if (enable) {
		gpio_pin_enable_callback(cc2520->gpios[CC2520_GPIO_IDX_FIFOP],
					 CONFIG_CC2520_GPIO_FIFOP);
	} else {
		gpio_pin_disable_callback(cc2520->gpios[CC2520_GPIO_IDX_FIFOP],
					  CONFIG_CC2520_GPIO_FIFOP);
	}
}

static void enable_sfd_interrupt(struct cc2520_context *cc2520,
				 bool enable)
{
	if (enable) {
		gpio_pin_enable_callback(cc2520->gpios[CC2520_GPIO_IDX_SFD],
					 CONFIG_CC2520_GPIO_SFD);
	} else {
		gpio_pin_disable_callback(cc2520->gpios[CC2520_GPIO_IDX_SFD],
					  CONFIG_CC2520_GPIO_SFD);
	}
}

static inline void setup_gpio_callbacks(struct device *dev)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	gpio_set_callback(cc2520->gpios[CC2520_GPIO_IDX_FIFOP],
			  gpio_int_handler);
	gpio_set_callback(cc2520->gpios[CC2520_GPIO_IDX_SFD],
			  gpio_int_handler);
}


/****************
 * TX functions *
 ***************/
static inline bool write_txfifo_length(struct cc2520_spi *spi,
				       struct net_buf *buf)
{
	spi->cmd_buf[0] = CC2520_INS_TXBUF;
	spi->cmd_buf[1] = packetbuf_totlen(buf) + CC2520_FCS_LENGTH;

	spi_slave_select(spi->dev, spi->slave);

	return (spi_write(spi->dev, spi->cmd_buf, 2) == DEV_OK);
}

static inline bool write_txfifo_content(struct cc2520_spi *spi,
					struct net_buf *buf)
{
	uint8_t cmd[128 + 1];

	cmd[0] = CC2520_INS_TXBUF;
	memcpy(&cmd[1], packetbuf_hdrptr(buf), packetbuf_totlen(buf));

	spi_slave_select(spi->dev, spi->slave);

	return (spi_write(spi->dev, cmd, packetbuf_totlen(buf) + 1) == DEV_OK);
}

static inline bool verify_txfifo_status(struct cc2520_context *cc2520,
					struct net_buf *buf)
{
	if (read_reg_txfifocnt(&cc2520->spi) < (packetbuf_totlen(buf) + 1) ||
	    (read_reg_excflag0(&cc2520->spi) & EXCFLAG0_TX_UNDERFLOW)) {
		return false;
	}

	return true;
}

static inline bool verify_tx_done(struct cc2520_context *cc2520)
{
	uint8_t timeout = 10;
	uint8_t status;

	do {
		_usleep(1);
		timeout--;
		status = read_reg_excflag0(&cc2520->spi);
	} while (!(status & EXCFLAG0_TX_FRM_DONE) && timeout);

	return !!(status & EXCFLAG0_TX_FRM_DONE);
}

static inline void enable_reception(struct cc2520_context *cc2520)
{
	/* Note: Errata document - 1.1 */
	enable_fifop_interrupt(cc2520, false);

	instruct_srxon(&cc2520->spi);
	instruct_sflushrx(&cc2520->spi);
	instruct_sflushrx(&cc2520->spi);

	enable_fifop_interrupt(cc2520, true);

	write_reg_excflag0(&cc2520->spi, EXCFLAG0_RESET_RX_FLAGS);
}

/****************
 * RX functions *
 ***************/
static inline void flush_rxfifo(struct cc2520_context *cc2520)
{
	/* Note: Errata document - 1.1 */
	enable_fifop_interrupt(cc2520, false);

	instruct_sflushrx(&cc2520->spi);
	instruct_sflushrx(&cc2520->spi);

	enable_fifop_interrupt(cc2520, true);

	write_reg_excflag0(&cc2520->spi, EXCFLAG0_RESET_RX_FLAGS);
}

static inline uint8_t read_rxfifo_length(struct cc2520_spi *spi)
{
	spi->cmd_buf[0] = CC2520_INS_RXBUF;

	spi_slave_select(spi->dev, spi->slave);

	if (spi_transceive(spi->dev, spi->cmd_buf, 1,
			   spi->cmd_buf, 2) == DEV_OK) {
		return spi->cmd_buf[1];
	}

	return 0;
}

static inline bool verify_rxfifo_validity(struct cc2520_spi *spi,
					  uint8_t pkt_len)
{
	if (pkt_len < 2 || read_reg_rxfifocnt(spi) != pkt_len) {
		return false;
	}

	return true;
}

static inline bool read_rxfifo_content(struct cc2520_spi *spi,
				       struct net_buf *buf, uint8_t len)
{
	uint8_t data[128+1];

	spi->cmd_buf[0] = CC2520_INS_RXBUF;

	spi_slave_select(spi->dev, spi->slave);

	if (spi_transceive(spi->dev, spi->cmd_buf, 1, data, len+1) != DEV_OK) {
		return false;
	}

	if (read_reg_excflag0(spi) & EXCFLAG0_RX_UNDERFLOW) {
		return false;
	}

	memcpy(packetbuf_dataptr(buf), &data[1], len);
	packetbuf_set_datalen(buf, len);

	return true;
}

static inline bool read_rxfifo_footer(struct cc2520_spi *spi,
				      uint8_t *buf, uint8_t len)
{
	spi->cmd_buf[0] = CC2520_INS_RXBUF;

	spi_slave_select(spi->dev, spi->slave);

	if (spi_transceive(spi->dev, spi->cmd_buf, 1,
			   spi->cmd_buf, len+1) != DEV_OK) {
		return false;
	}

	memcpy(buf, &spi->cmd_buf[1], CC2520_FCS_LENGTH);

	return true;
}

static void cc2520_rx(int arg, int unused2)
{
	struct device *dev = INT_TO_POINTER(arg);
	struct cc2520_context *cc2520 = dev->driver_data;
	struct net_buf *pkt_buf = NULL;
	uint8_t pkt_len;
#ifdef CONFIG_TI_CC2520_AUTO_CRC
	uint8_t buf[CC2520_FCS_LENGTH];
#endif

	ARG_UNUSED(unused2);

	while (1) {
		nano_fiber_sem_take(&cc2520->rx_lock, TICKS_UNLIMITED);

		if (cc2520->overflow) {
			DBG("RX overflow!\n");
			cc2520->overflow = false;
			goto flush;
		}

		pkt_len = read_rxfifo_length(&cc2520->spi) & 0x7f;
		if (!verify_rxfifo_validity(&cc2520->spi, pkt_len)) {
			DBG("Invalid content\n");
			goto flush;
		}

		pkt_buf = l2_buf_get_reserve(0);
		if (!pkt_buf) {
			DBG("No pkt buf available\n");
			goto flush;
		}

		if (!read_rxfifo_content(&cc2520->spi, pkt_buf,
					 pkt_len - CC2520_FCS_LENGTH)) {
			DBG("No content read\n");
			goto error;
		}
#ifdef CONFIG_TI_CC2520_AUTO_CRC
		if (!read_rxfifo_footer(&cc2520->spi, buf, CC2520_FCS_LENGTH)) {
			DBG("No footer read\n");
			goto error;
		}

		if (!(buf[1] & CC2520_FCS_CRC_OK)) {
			DBG("Bad packet CRC\n");
			goto error;
		}
#ifdef CONFIG_TI_CC2520_LINK_DETAILS
		packetbuf_set_attr(pkt_buf, PACKETBUF_ATTR_RSSI,
				   buf[0]);
		packetbuf_set_attr(pkt_buf, PACKETBUF_ATTR_LINK_QUALITY,
				   buf[1] & CC2520_FCS_CORRELATION);
#endif /* CONFIG_TI_CC2520_LINK_DETAILS */
#endif /* CONFIG_TI_CC2520_AUTO_CRC */

		DBG("Caught a packet (%u)\n", pkt_len - CC2520_FCS_LENGTH);

		if (net_driver_15_4_recv_from_hw(pkt_buf) < 0) {
			DBG("Packet dropped by NET stack\n");
			goto error;
		}

		net_analyze_stack("CC2520 Rx Fiber stack",
				  cc2520->cc2520_rx_stack,
				  CONFIG_CC2520_RX_STACK_SIZE);
		goto flush;
error:
		l2_buf_unref(pkt_buf);
flush:
		flush_rxfifo(cc2520);
	}
}


/********************
 * Radio device API *
 *******************/
static int cc2520_set_channel(struct device *dev, uint16_t channel)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	DBG("%s: %u\n", __func__, channel);

	if (channel < 11 || channel > 26) {
		return DEV_FAIL;
	}

	/* See chapter 16 */
	channel = 11 + 5 * (channel - 11);

	if (!write_reg_freqctrl(&cc2520->spi, FREQCTRL_FREQ(channel))) {
		DBG("%s: FAILED\n", __func__);
		return DEV_FAIL;
	}

	return DEV_OK;
}

static int cc2520_set_pan_id(struct device *dev, uint16_t pan_id)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	DBG("%s: 0x%x\n", __func__, pan_id);

	pan_id = sys_le16_to_cpu(pan_id);

	if (!write_mem_pan_id(&cc2520->spi, (uint8_t *) &pan_id)) {
		DBG("%s: FAILED\n", __func__);
		return DEV_FAIL;
	}

	return DEV_OK;
}

static int cc2520_set_short_addr(struct device *dev, uint16_t short_addr)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	DBG("%s: 0x%x\n", __func__, short_addr);

	short_addr = sys_le16_to_cpu(short_addr);

	if (!write_mem_short_addr(&cc2520->spi, (uint8_t *) &short_addr)) {
		DBG("%s: FAILED\n", __func__);
		return DEV_FAIL;
	}

	return DEV_OK;
}

static int cc2520_set_ieee_addr(struct device *dev, const uint8_t *ieee_addr)
{
	struct cc2520_context *cc2520 = dev->driver_data;
	uint8_t ext_addr[8];
	int idx;

	DBG("%s: %p\n", __func__, ieee_addr);

	for (idx = 0; idx < 8; idx++) {
		ext_addr[idx] = ieee_addr[7 - idx];
	}

	if (!write_mem_ext_addr(&cc2520->spi, ext_addr)) {
		DBG("%s: FAILED\n", __func__);
		return DEV_FAIL;
	}

	return DEV_OK;
}

static int cc2520_set_txpower(struct device *dev, short dbm)
{
	struct cc2520_context *cc2520 = dev->driver_data;
	uint8_t pwr;

	DBG("%s: %d\n", dbm);

	/* See chapter 19 part 8 */
	switch (dbm) {
	case 5:
		pwr = 0xF7;
		break;
	case 3:
		pwr = 0xF2;
		break;
	case 2:
		pwr = 0xAB;
		break;
	case 1:
		pwr = 0x13;
		break;
	case 0:
		pwr = 0x32;
		break;
	case -2:
		pwr = 0x81;
		break;
	case -4:
		pwr = 0x88;
		break;
	case -7:
		pwr = 0x2C;
		break;
	case -18:
		pwr = 0x03;
		break;
	default:
		goto error;
	}

	if (!write_reg_txpower(&cc2520->spi, pwr)) {
		goto error;
	}

	return DEV_OK;
error:
	DBG("%s: FAILED\n");
	return DEV_FAIL;
}

static int cc2520_tx(struct device *dev, struct net_buf *buf)
{
	struct cc2520_context *cc2520 = dev->driver_data;
	uint8_t retry = 2;
	bool status;

	DBG("%s: %p (%u)\n", __func__, buf, packetbuf_totlen(buf));

	if (!write_reg_excflag0(&cc2520->spi, EXCFLAG0_RESET_TX_FLAGS) ||
	    !write_txfifo_length(&cc2520->spi, buf) ||
	    !write_txfifo_content(&cc2520->spi, buf)) {
		DBG("%s: Cannot feed in TX fifo\n", __func__);
		goto error;
	}

	if (!verify_txfifo_status(cc2520, buf)) {
		DBG("%s: Did not write properly into TX FIFO\n", __func__);
		goto error;
	}

	/* 1 retry is allowed here */
	do {
		atomic_set(&cc2520->tx, 1);

		if (!instruct_stxoncca(&cc2520->spi)) {
			DBG("%s: Cannot start transmission\n", __func__);
			goto error;
		}

		/* _cc2520_print_exceptions(cc2520); */

		device_sync_call_wait(&cc2520->tx_sync);

		retry--;
		status = verify_tx_done(cc2520);
	} while (!status && retry);

	if (!status) {
		DBG("%s: No TX_FRM_DONE\n", __func__);
		goto error;
	}

	enable_reception(cc2520);

	return DEV_OK;
error:
	atomic_set(&cc2520->tx, 0);
	instruct_sflushtx(&cc2520->spi);
	enable_reception(cc2520);

	return DEV_FAIL;
}

static const uint8_t *cc2520_get_mac(struct device *dev)
{
	struct cc2520_context *cc2520 = cc2520_sglt->driver_data;

	if (cc2520->mac_addr[1] == 0x00) {
		/* TI OUI */
		cc2520->mac_addr[0] = 0x00;
		cc2520->mac_addr[1] = 0x12;
		cc2520->mac_addr[2] = 0x4b;

		cc2520->mac_addr[3] = 0x00;
		UNALIGNED_PUT(sys_rand32_get(),
			      (uint32_t *) ((void *)cc2520->mac_addr+4));

		cc2520->mac_addr[7] = (cc2520->mac_addr[7] & ~0x01) | 0x02;
	}

	return cc2520->mac_addr;
}

static int cc2520_start(struct device *dev)
{
	struct cc2520_context *cc2520 = cc2520_sglt->driver_data;

	DBG("%s\n", __func__);

	if (!instruct_srxon(&cc2520->spi) ||
	    !verify_osc_stabilization(cc2520)) {
		return DEV_FAIL;
	}

	flush_rxfifo(cc2520);

	enable_fifop_interrupt(cc2520, true);
	enable_sfd_interrupt(cc2520, true);

	return DEV_OK;
}

static int cc2520_stop(struct device *dev)
{
	struct cc2520_context *cc2520 = cc2520_sglt->driver_data;

	DBG("%s\n", __func__);

	enable_fifop_interrupt(cc2520, false);
	enable_sfd_interrupt(cc2520, false);

	if (!instruct_sroff(&cc2520->spi)) {
		return DEV_FAIL;
	}

	flush_rxfifo(cc2520);

	return DEV_OK;
}


/***************************
 * Legacy Radio device API *
 **************************/
#ifdef CONFIG_NETWORKING_LEGACY_RADIO_DRIVER
/**
 * NOTE: This legacy API DOES NOT FIT within Zephyr device driver model
 *       and, as such, will be made obsolete soon (well, hopefully...)
 */

static int cc2520_initialize(void)
{
	net_set_mac((uint8_t *) cc2520_get_mac(cc2520_sglt), 8);

	return 1;
}

static int cc2520_prepare(const void *payload, unsigned short payload_len)
{
	return 0;
}

static int cc2520_transmit(struct net_buf *buf, unsigned short transmit_len)
{
	if (cc2520_tx(cc2520_sglt, buf) != DEV_OK) {
		return RADIO_TX_ERR;
	}

	return RADIO_TX_OK;
}

static int cc2520_send(struct net_buf *buf,
		       const void *payload, unsigned short payload_len)
{
	return cc2520_transmit(buf, payload_len);
}

static int cc2520_read(void *buf, unsigned short buf_len)
{
	return 0;
}

static int cc2520_channel_clear(void)
{
	struct cc2520_context *cc2520 = cc2520_sglt->driver_data;

	return get_cca(cc2520);
}

static int cc2520_receiving_packet(void)
{
	return 0;
}

static int cc2520_pending_packet(void)
{
	return 0;
}

static int cc2520_on(void)
{
	return (cc2520_start(cc2520_sglt) == DEV_OK);
}

static int cc2520_off(void)
{
	return (cc2520_stop(cc2520_sglt) == DEV_OK);
}

static radio_result_t cc2520_get_value(radio_param_t param,
				       radio_value_t *value)
{
	switch (param) {
	case RADIO_PARAM_POWER_MODE:
		*value = RADIO_POWER_MODE_ON;
		break;
	case RADIO_PARAM_CHANNEL:
		*value = CONFIG_TI_CC2520_CHANNEL;
		break;
	case RADIO_CONST_CHANNEL_MIN:
		*value = 11;
		break;
	case RADIO_CONST_CHANNEL_MAX:
		*value = 26;
		break;
	default:
		return RADIO_RESULT_NOT_SUPPORTED;
	}

	return RADIO_RESULT_OK;
}

static radio_result_t cc2520_set_value(radio_param_t param,
				       radio_value_t value)
{
	switch (param) {
	case RADIO_PARAM_POWER_MODE:
		break;
	case RADIO_PARAM_CHANNEL:
		cc2520_set_channel(cc2520_sglt, value);
		break;
	case RADIO_PARAM_PAN_ID:
		cc2520_set_pan_id(cc2520_sglt, value);
		break;
	case RADIO_PARAM_RX_MODE:
	default:
		return RADIO_RESULT_NOT_SUPPORTED;
	}

	return RADIO_RESULT_OK;
}

static radio_result_t cc2520_get_object(radio_param_t param,
					void *dest, size_t size)
{
	return RADIO_RESULT_NOT_SUPPORTED;
}

static radio_result_t cc2520_set_object(radio_param_t param,
					const void *src, size_t size)
{
	return RADIO_RESULT_NOT_SUPPORTED;
}

struct radio_driver cc2520_15_4_radio_driver = {
	.init = cc2520_initialize,
	.prepare = cc2520_prepare,
	.transmit = cc2520_transmit,
	.send = cc2520_send,
	.read = cc2520_read,
	.channel_clear = cc2520_channel_clear,
	.receiving_packet = cc2520_receiving_packet,
	.pending_packet = cc2520_pending_packet,
	.on = cc2520_on,
	.off = cc2520_off,
	.get_value = cc2520_get_value,
	.set_value = cc2520_set_value,
	.get_object = cc2520_get_object,
	.set_object = cc2520_set_object,
};
#endif /* CONFIG_NETWORKING_LEGACY_RADIO_DRIVER */


/******************
 * Initialization *
 *****************/
static int power_on_and_setup(struct device *dev)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	/* Switching to LPM2 mode */
	set_reset(dev, 0);
	_usleep(150);

	set_vreg_en(dev, 0);
	_usleep(250);

	/* Then to ACTIVE mode */
	set_vreg_en(dev, 1);
	_usleep(250);

	set_reset(dev, 1);
	_usleep(150);

	if (!verify_osc_stabilization(cc2520)) {
		return DEV_FAIL;
	}

	/* Default settings to always write (see chapter 28 part 1) */
	if (!write_reg_txpower(&cc2520->spi, CC2520_TXPOWER_DEFAULT) ||
	    !write_reg_ccactrl0(&cc2520->spi, CC2520_CCACTRL0_DEFAULT) ||
	    !write_reg_mdmctrl0(&cc2520->spi, CC2520_MDMCTRL0_DEFAULT) ||
	    !write_reg_mdmctrl1(&cc2520->spi, CC2520_MDMCTRL1_DEFAULT) ||
	    !write_reg_rxctrl(&cc2520->spi, CC2520_RXCTRL_DEFAULT) ||
	    !write_reg_fsctrl(&cc2520->spi, CC2520_FSCTRL_DEFAULT) ||
	    !write_reg_fscal1(&cc2520->spi, CC2520_FSCAL1_DEFAULT) ||
	    !write_reg_agcctrl1(&cc2520->spi, CC2520_AGCCTRL1_DEFAULT) ||
	    !write_reg_adctest0(&cc2520->spi, CC2520_ADCTEST0_DEFAULT) ||
	    !write_reg_adctest1(&cc2520->spi, CC2520_ADCTEST1_DEFAULT) ||
	    !write_reg_adctest2(&cc2520->spi, CC2520_ADCTEST2_DEFAULT)) {
		return DEV_FAIL;
	}

	/* EXTCLOCK0: Disabling external clock
	 * FRMCTRL0: AUTOACK and AUTOCRC enabled
	 * FRMCTRL1: SET_RXENMASK_ON_TX and IGNORE_TX_UNDERF
	 * FRMFILT0: Frame filtering (setting CC2520_FRAME_FILTERING)
	 * FIFOPCTRL: Set TX threshold (setting CC2520_TX_THRESHOLD)
	 */
	if (!write_reg_extclock(&cc2520->spi, 0) ||
	    !write_reg_frmctrl0(&cc2520->spi, CC2520_AUTOMATISM) ||
	    !write_reg_frmctrl1(&cc2520->spi, FRMCTRL1_IGNORE_TX_UNDERF |
				FRMCTRL1_SET_RXENMASK_ON_TX) ||
	    !write_reg_frmfilt0(&cc2520->spi, CC2520_FRAME_FILTERING |
				FRMFILT0_MAX_FRAME_VERSION(3)) ||
	    !write_reg_fifopctrl(&cc2520->spi,
				 FIFOPCTRL_FIFOP_THR(CC2520_TX_THRESHOLD))) {
		return DEV_FAIL;
	}

	/* Cleaning up TX fifo */
	instruct_sflushtx(&cc2520->spi);

	setup_gpio_callbacks(dev);

	_cc2520_print_gpio_config(dev);

	return DEV_OK;
}

static inline int configure_spi(struct device *dev)
{
	struct cc2520_context *cc2520 = dev->driver_data;
	struct spi_config spi_conf = {
		.config = SPI_WORD(8),
		.max_sys_freq = CONFIG_TI_CC2520_SPI_FREQ,
	};

	cc2520->spi.dev = device_get_binding(CONFIG_TI_CC2520_SPI_DRV_NAME);
	if (cc2520->spi.dev) {
		cc2520->spi.slave = CONFIG_TI_CC2520_SPI_SLAVE;

		if (spi_configure(cc2520->spi.dev, &spi_conf) != DEV_OK ||
		    spi_slave_select(cc2520->spi.dev,
				     cc2520->spi.slave) != DEV_OK) {
			cc2520->spi.dev = NULL;
			return DEV_FAIL;
		}
	}

	return DEV_OK;
}

int cc2520_init(struct device *dev)
{
	struct cc2520_context *cc2520 = dev->driver_data;

	dev->driver_api = NULL;

	device_sync_call_init(&cc2520->tx_sync);
	atomic_set(&cc2520->tx, 0);
	nano_sem_init(&cc2520->rx_lock);

	cc2520->gpios = cc2520_configure_gpios();
	if (!cc2520->gpios) {
		DBG("Configuring GPIOS failed\n");
		return DEV_FAIL;
	}

	if (configure_spi(dev) != DEV_OK) {
		DBG("Configuring SPI failed\n");
		return DEV_FAIL;
	}

	DBG("GPIO and SPI configured\n");

	if (power_on_and_setup(dev) != DEV_OK) {
		DBG("Configuring CC2520 failed\n");
		return DEV_FAIL;
	}

	/* That should not be done here... */
	if (cc2520_set_pan_id(dev, 0xFFFF) != DEV_OK ||
	    cc2520_set_short_addr(dev, 0x0000) != DEV_OK ||
	    cc2520_set_channel(dev, CONFIG_TI_CC2520_CHANNEL) != DEV_OK) {
		DBG("Could not initialize properly cc2520\n");
		return DEV_FAIL;
	}

	task_fiber_start(cc2520->cc2520_rx_stack,
			 CONFIG_CC2520_RX_STACK_SIZE,
			 cc2520_rx, POINTER_TO_INT(dev),
			 0, 0, 0);

#ifdef CONFIG_NETWORKING_LEGACY_RADIO_DRIVER
	cc2520_sglt = dev;
#endif

	return DEV_OK;
}

struct cc2520_context cc2520_context_data;

DEVICE_INIT(cc2520, CONFIG_TI_CC2520_DRV_NAME,
	    cc2520_init, &cc2520_context_data, NULL,
	    APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
