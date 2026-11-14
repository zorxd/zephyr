/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief I2C controller driver for the Microchip TWIHS (group g1).
 */

#define DT_DRV_COMPAT microchip_twihs_g1_i2c

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control/mchp_sam_pmc.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <soc.h>

LOG_MODULE_REGISTER(i2c_mchp_twihs_g1, CONFIG_I2C_LOG_LEVEL);

#include "i2c-priv.h"

#define TWIHS_CKDIV_MAX 7U
#define TWIHS_CLDIV_MAX 255U

/* Transfer time budget: a fixed margin plus one millisecond per byte. */
#define TWIHS_TIMEOUT_BASE_MS 10U

#define TWIHS_IRQ_ALL                                                                              \
	(TWIHS_IER_TXRDY_Msk | TWIHS_IER_RXRDY_Msk | TWIHS_IER_TXCOMP_Msk | TWIHS_IER_NACK_Msk |   \
	 TWIHS_IER_ARBLST_Msk)

struct i2c_mchp_twihs_config {
	twihs_registers_t *regs;
	const struct device *clock_dev;
	struct sam_clk_cfg clock_cfg;
	const struct pinctrl_dev_config *pcfg;
	void (*config_irq)(void);
	uint32_t bitrate;
};

struct i2c_mchp_twihs_msg {
	uint8_t *buf;
	uint32_t len;
	uint32_t idx;
	/* TWIHS_SR error flags seen during the message, 0 on success. */
	uint32_t error;
	uint8_t flags;
};

struct i2c_mchp_twihs_data {
	struct k_mutex lock;
	struct k_sem done;
	struct i2c_mchp_twihs_msg msg;
	uint32_t dev_config;
};

static int i2c_mchp_twihs_set_clock(const struct device *dev, uint32_t bitrate)
{
	const struct i2c_mchp_twihs_config *cfg = dev->config;
	uint32_t periph_hz;
	uint32_t ckdiv = 0U;
	uint32_t cldiv;
	int ret;

	ret = clock_control_get_rate(cfg->clock_dev, (clock_control_subsys_t)&cfg->clock_cfg,
				     &periph_hz);
	if (ret < 0) {
		return ret;
	}

	if (periph_hz < (bitrate * 2U * 4U)) {
		return -EINVAL;
	}

	/* Each SCL half period is (CLDIV * 2^CKDIV + 3) peripheral clock cycles. */
	do {
		cldiv = ((periph_hz / (bitrate * 2U)) - 3U) >> ckdiv;
		if (cldiv <= TWIHS_CLDIV_MAX) {
			break;
		}
		ckdiv++;
	} while (ckdiv <= TWIHS_CKDIV_MAX);

	if (ckdiv > TWIHS_CKDIV_MAX) {
		LOG_ERR("bitrate %u too low for a %u Hz peripheral clock", bitrate, periph_hz);
		return -EINVAL;
	}

	cfg->regs->TWIHS_CWGR =
		TWIHS_CWGR_CLDIV(cldiv) | TWIHS_CWGR_CHDIV(cldiv) | TWIHS_CWGR_CKDIV(ckdiv);

	return 0;
}

static int i2c_mchp_twihs_apply_config(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_mchp_twihs_config *cfg = dev->config;
	twihs_registers_t *twihs = cfg->regs;
	uint32_t bitrate;
	int ret;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U) {
		return -ENOTSUP;
	}

	if ((dev_config & I2C_ADDR_10_BITS) != 0U) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		bitrate = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		bitrate = KHZ(400);
		break;
	default:
		return -ENOTSUP;
	}

	twihs->TWIHS_CR = TWIHS_CR_SWRST_Msk;
	twihs->TWIHS_IDR = TWIHS_IRQ_ALL;
	(void)twihs->TWIHS_SR;

	ret = i2c_mchp_twihs_set_clock(dev, bitrate);
	if (ret < 0) {
		return ret;
	}

	twihs->TWIHS_CR = TWIHS_CR_SVDIS_Msk;
	twihs->TWIHS_CR = TWIHS_CR_MSEN_Msk;

	return 0;
}

static int i2c_mchp_twihs_configure(const struct device *dev, uint32_t dev_config)
{
	struct i2c_mchp_twihs_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = i2c_mchp_twihs_apply_config(dev, dev_config);
	if (ret == 0) {
		data->dev_config = dev_config;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2c_mchp_twihs_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_mchp_twihs_data *data = dev->data;

	*dev_config = data->dev_config;

	return 0;
}

static void i2c_mchp_twihs_start(twihs_registers_t *twihs, struct i2c_mchp_twihs_msg *msg,
				 uint16_t addr)
{
	if ((msg->flags & I2C_MSG_READ) != 0U) {
		twihs->TWIHS_MMR = TWIHS_MMR_MREAD_Msk | TWIHS_MMR_DADR(addr);
		/* A single byte read sets START and STOP together. */
		twihs->TWIHS_CR = TWIHS_CR_START_Msk | ((msg->len == 1U) ? TWIHS_CR_STOP_Msk : 0U);
		twihs->TWIHS_IER = TWIHS_IER_RXRDY_Msk | TWIHS_IER_TXCOMP_Msk | TWIHS_IER_NACK_Msk |
				   TWIHS_IER_ARBLST_Msk;
	} else if (msg->len == 0U) {
		/* Address-only write, for example to probe a target. */
		twihs->TWIHS_MMR = TWIHS_MMR_DADR(addr);
		twihs->TWIHS_CR = TWIHS_CR_QUICK_Msk;
		twihs->TWIHS_IER = TWIHS_IER_TXCOMP_Msk | TWIHS_IER_NACK_Msk | TWIHS_IER_ARBLST_Msk;
	} else {
		twihs->TWIHS_MMR = TWIHS_MMR_DADR(addr);
		twihs->TWIHS_THR = msg->buf[msg->idx++];
		twihs->TWIHS_IER = TWIHS_IER_TXRDY_Msk | TWIHS_IER_TXCOMP_Msk | TWIHS_IER_NACK_Msk |
				   TWIHS_IER_ARBLST_Msk;
	}
}

static int i2c_mchp_twihs_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
				   uint16_t addr)
{
	const struct i2c_mchp_twihs_config *cfg = dev->config;
	struct i2c_mchp_twihs_data *data = dev->data;
	twihs_registers_t *twihs = cfg->regs;
	int ret = 0;

	if (num_msgs == 0U) {
		return 0;
	}

	for (uint8_t i = 0U; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_ADDR_10_BITS) != 0U) {
			return -ENOTSUP;
		}
		if ((msgs[i].flags & I2C_MSG_READ) != 0U && msgs[i].len == 0U) {
			return -EINVAL;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Clear stale status such as a previous NACK. */
	(void)twihs->TWIHS_SR;
	twihs->TWIHS_IADR = 0U;

	for (uint8_t i = 0U; i < num_msgs; i++) {
		struct i2c_mchp_twihs_msg *msg = &data->msg;

		msg->buf = msgs[i].buf;
		msg->len = msgs[i].len;
		msg->idx = 0U;
		msg->error = 0U;
		msg->flags = msgs[i].flags;

		k_sem_reset(&data->done);
		i2c_mchp_twihs_start(twihs, msg, addr);

		if (k_sem_take(&data->done, K_MSEC(TWIHS_TIMEOUT_BASE_MS + msg->len)) != 0) {
			LOG_ERR("transfer timeout");
			/* Recover the controller; the bus state is unknown. */
			(void)i2c_mchp_twihs_apply_config(dev, data->dev_config);
			ret = -ETIMEDOUT;
			break;
		}

		if (msg->error != 0U) {
			ret = -EIO;
			break;
		}
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static void i2c_mchp_twihs_isr(const struct device *dev)
{
	const struct i2c_mchp_twihs_config *cfg = dev->config;
	struct i2c_mchp_twihs_data *data = dev->data;
	twihs_registers_t *twihs = cfg->regs;
	struct i2c_mchp_twihs_msg *msg = &data->msg;
	uint32_t status = twihs->TWIHS_SR & twihs->TWIHS_IMR;

	if ((status & (TWIHS_SR_NACK_Msk | TWIHS_SR_ARBLST_Msk)) != 0U) {
		msg->error = status & (TWIHS_SR_NACK_Msk | TWIHS_SR_ARBLST_Msk);
		goto done;
	}

	if ((status & TWIHS_SR_RXRDY_Msk) != 0U) {
		msg->buf[msg->idx++] = (uint8_t)twihs->TWIHS_RHR;

		/* STOP must be requested while the last byte is being received. */
		if (msg->idx == msg->len - 1U) {
			twihs->TWIHS_CR = TWIHS_CR_STOP_Msk;
		}
	}

	if ((status & TWIHS_SR_TXRDY_Msk) != 0U) {
		if (msg->idx < msg->len) {
			twihs->TWIHS_THR = msg->buf[msg->idx++];
		} else if ((msg->flags & I2C_MSG_STOP) != 0U) {
			twihs->TWIHS_CR = TWIHS_CR_STOP_Msk;
			twihs->TWIHS_IDR = TWIHS_IDR_TXRDY_Msk;
		} else {
			/* Leave the bus held for a repeated start by the next message. */
			goto done;
		}
	}

	if ((status & TWIHS_SR_TXCOMP_Msk) != 0U) {
		goto done;
	}

	return;

done:
	twihs->TWIHS_IDR = twihs->TWIHS_IMR;
	k_sem_give(&data->done);
}

static int i2c_mchp_twihs_init(const struct device *dev)
{
	const struct i2c_mchp_twihs_config *cfg = dev->config;
	struct i2c_mchp_twihs_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);
	k_sem_init(&data->done, 0, 1);

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(cfg->clock_dev, (clock_control_subsys_t)&cfg->clock_cfg);
	if (ret < 0) {
		return ret;
	}

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	cfg->config_irq();

	ret = i2c_mchp_twihs_configure(dev, I2C_MODE_CONTROLLER | i2c_map_dt_bitrate(cfg->bitrate));
	if (ret < 0) {
		LOG_ERR("configuration failed: %d", ret);
		return ret;
	}

	return 0;
}

static DEVICE_API(i2c, i2c_mchp_twihs_api) = {
	.configure = i2c_mchp_twihs_configure,
	.get_config = i2c_mchp_twihs_get_config,
	.transfer = i2c_mchp_twihs_transfer,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

#define I2C_MCHP_TWIHS_INIT(n)                                                                     \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static void i2c_mchp_twihs_config_irq_##n(void)                                            \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), i2c_mchp_twihs_isr,         \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct i2c_mchp_twihs_config i2c_mchp_twihs_config_##n = {                    \
		.regs = (twihs_registers_t *)DT_INST_REG_ADDR(n),                                  \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_cfg = SAM_DT_INST_CLOCK_PMC_CFG(n),                                         \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.config_irq = i2c_mchp_twihs_config_irq_##n,                                       \
		.bitrate = DT_INST_PROP(n, clock_frequency),                                       \
	};                                                                                         \
                                                                                                   \
	static struct i2c_mchp_twihs_data i2c_mchp_twihs_data_##n;                                 \
                                                                                                   \
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_mchp_twihs_init, NULL, &i2c_mchp_twihs_data_##n,          \
				  &i2c_mchp_twihs_config_##n, POST_KERNEL,                         \
				  CONFIG_I2C_INIT_PRIORITY, &i2c_mchp_twihs_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_MCHP_TWIHS_INIT)
