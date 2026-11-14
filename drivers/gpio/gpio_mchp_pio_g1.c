/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief GPIO driver for the Microchip PIO (group g1).
 */

#define DT_DRV_COMPAT microchip_pio_g1_gpio

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control/mchp_sam_pmc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <soc.h>

struct gpio_mchp_pio_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	pio_registers_t *regs;
	const struct device *clock_dev;
	struct sam_clk_cfg clock_cfg;
	void (*config_irq)(void);
};

struct gpio_mchp_pio_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	sys_slist_t callbacks;
};

static int gpio_mchp_pio_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;
	pio_registers_t *pio = cfg->regs;
	uint32_t mask = BIT(pin);

	if ((flags & GPIO_SINGLE_ENDED) != 0U) {
		/* Open drain is the only supported single-ended mode. */
		if ((flags & GPIO_LINE_OPEN_DRAIN) == 0U) {
			return -ENOTSUP;
		}
		pio->PIO_MDER = mask;
	} else {
		pio->PIO_MDDR = mask;
	}

	/* Pull-up and pull-down are exclusive: disable both before enabling one. */
	pio->PIO_PUDR = mask;
	pio->PIO_PPDDR = mask;

	if ((flags & (GPIO_INPUT | GPIO_OUTPUT)) == 0U) {
		/* Disconnected: no interrupt, no output, PIO keeps the pin. */
		pio->PIO_IDR = mask;
		pio->PIO_ODR = mask;
		pio->PIO_PER = mask;
		return 0;
	}

	if ((flags & GPIO_OUTPUT) != 0U) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			pio->PIO_SODR = mask;
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			pio->PIO_CODR = mask;
		}
		pio->PIO_OER = mask;
		/* Allow writes through PIO_ODSR for set_masked_raw and toggle. */
		pio->PIO_OWER = mask;
	} else {
		pio->PIO_ODR = mask;
	}

	/* The input is always enabled. */
	if ((flags & GPIO_PULL_UP) != 0U) {
		pio->PIO_PUER = mask;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		pio->PIO_PPDER = mask;
	}

	pio->PIO_PER = mask;

	return 0;
}

static int gpio_mchp_pio_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;

	*value = cfg->regs->PIO_PDSR;

	return 0;
}

static int gpio_mchp_pio_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					     gpio_port_value_t value)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;
	pio_registers_t *pio = cfg->regs;
	unsigned int key = irq_lock();

	pio->PIO_ODSR = (pio->PIO_ODSR & ~mask) | (value & mask);

	irq_unlock(key);

	return 0;
}

static int gpio_mchp_pio_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;

	cfg->regs->PIO_SODR = pins;

	return 0;
}

static int gpio_mchp_pio_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;

	cfg->regs->PIO_CODR = pins;

	return 0;
}

static int gpio_mchp_pio_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;
	pio_registers_t *pio = cfg->regs;
	unsigned int key = irq_lock();

	pio->PIO_ODSR ^= pins;

	irq_unlock(key);

	return 0;
}

static int gpio_mchp_pio_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
						 enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;
	pio_registers_t *pio = cfg->regs;
	uint32_t mask = BIT(pin);

	pio->PIO_IDR = mask;
	pio->PIO_AIMDR = mask;

	if (mode == GPIO_INT_MODE_DISABLED) {
		return 0;
	}

	if (trig != GPIO_INT_TRIG_BOTH) {
		/* Additional interrupt modes select a single edge or a level. */
		pio->PIO_AIMER = mask;

		if (mode == GPIO_INT_MODE_EDGE) {
			pio->PIO_ESR = mask;
		} else {
			pio->PIO_LSR = mask;
		}

		if (trig == GPIO_INT_TRIG_HIGH) {
			pio->PIO_REHLSR = mask;
		} else {
			pio->PIO_FELLSR = mask;
		}
	} else if (mode == GPIO_INT_MODE_LEVEL) {
		/* Both levels at once cannot be expressed. */
		return -ENOTSUP;
	}

	/* Reading PIO_ISR clears pending changes before the interrupt is enabled. */
	(void)pio->PIO_ISR;
	pio->PIO_IER = mask;

	return 0;
}

static int gpio_mchp_pio_manage_callback(const struct device *dev, struct gpio_callback *callback,
					 bool set)
{
	struct gpio_mchp_pio_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static void gpio_mchp_pio_isr(const struct device *dev)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;
	struct gpio_mchp_pio_data *data = dev->data;
	pio_registers_t *pio = cfg->regs;

	/* PIO_ISR flags every input change; only report enabled pins. */
	uint32_t status = pio->PIO_ISR & pio->PIO_IMR;

	gpio_fire_callbacks(&data->callbacks, dev, status);
}

static DEVICE_API(gpio, gpio_mchp_pio_api) = {
	.pin_configure = gpio_mchp_pio_pin_configure,
	.port_get_raw = gpio_mchp_pio_port_get_raw,
	.port_set_masked_raw = gpio_mchp_pio_port_set_masked_raw,
	.port_set_bits_raw = gpio_mchp_pio_port_set_bits_raw,
	.port_clear_bits_raw = gpio_mchp_pio_port_clear_bits_raw,
	.port_toggle_bits = gpio_mchp_pio_port_toggle_bits,
	.pin_interrupt_configure = gpio_mchp_pio_pin_interrupt_configure,
	.manage_callback = gpio_mchp_pio_manage_callback,
};

static int gpio_mchp_pio_init(const struct device *dev)
{
	const struct gpio_mchp_pio_config *cfg = dev->config;
	int ret;

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	/* The PIO clock is needed to sample inputs and to detect input changes. */
	ret = clock_control_on(cfg->clock_dev, (clock_control_subsys_t)&cfg->clock_cfg);
	if (ret < 0) {
		return ret;
	}

	cfg->config_irq();

	return 0;
}

#define GPIO_MCHP_PIO_INIT(n)                                                                      \
	static void gpio_mchp_pio_config_irq_##n(void)                                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), gpio_mchp_pio_isr,          \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct gpio_mchp_pio_config gpio_mchp_pio_config_##n = {                      \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(n),                                      \
		.regs = (pio_registers_t *)DT_INST_REG_ADDR(n),                                    \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_cfg = SAM_DT_INST_CLOCK_PMC_CFG(n),                                         \
		.config_irq = gpio_mchp_pio_config_irq_##n,                                        \
	};                                                                                         \
                                                                                                   \
	static struct gpio_mchp_pio_data gpio_mchp_pio_data_##n;                                   \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, gpio_mchp_pio_init, NULL, &gpio_mchp_pio_data_##n,                \
			      &gpio_mchp_pio_config_##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,  \
			      &gpio_mchp_pio_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_MCHP_PIO_INIT)
