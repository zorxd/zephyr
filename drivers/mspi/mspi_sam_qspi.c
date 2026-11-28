/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * QSPI controller of the SAM E70/S70/V70/V71 and PIC32CZ CA70/MC70 in serial
 * memory mode (QSPI_MR.SMM = 1), as an MSPI controller with one device on QCS.
 *
 * Each packet is one instruction frame: QSPI_IAR, QSPI_ICR and QSPI_IFR
 * describe the command, address, option and dummy phases, and the data phase
 * goes through the serial memory window. The window must be device memory
 * (no cache, no speculative reads), see the binding.
 */

#define DT_DRV_COMPAT atmel_sam_qspi_controller

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control/atmel_sam_pmc.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <soc.h>

LOG_MODULE_REGISTER(mspi_sam_qspi, CONFIG_MSPI_LOG_LEVEL);

/*
 * The Atmel headers name the QSPI register block Qspi; the Microchip headers
 * (PIC32CZ CA70) use qspi_registers_t. Both define the _Msk names used here.
 */
#ifdef QSPI_REGS
typedef qspi_registers_t sam_qspi_regs_t;
#else
typedef Qspi sam_qspi_regs_t;
#endif

/* QSPI_SCR.SCBR divides the peripheral clock by 1 to 256 */
#define SAM_QSPI_DIV_MAX 256U

/* QSPI_IFR.NBDUM is 5 bits wide */
#define SAM_QSPI_DUMMY_MAX 31U

/*
 * Minimum time QCS stays high between two frames, covering tSHSL of common
 * serial NOR flashes (50 ns at most).
 */
#define SAM_QSPI_CS_HIGH_NS 50U

/*
 * Mode byte sent in place of the first dummy cycles of reads with a multi-line
 * address phase (such as EBh in 1-4-4). 0xFF keeps the device out of its
 * continuous read (XIP) mode, whatever the vendor.
 */
#define SAM_QSPI_MODE_BYTE 0xFFU

struct mspi_sam_qspi_config {
	sam_qspi_regs_t *regs;
	mem_addr_t mem_base;
	uint32_t mem_size;
	const struct atmel_sam_pmc_config clock_cfg;
	const struct pinctrl_dev_config *pcfg;
	struct mspi_cfg mspicfg;
};

struct mspi_sam_qspi_data {
	const struct mspi_dev_id *dev_id;
	struct k_mutex lock;
	struct mspi_dev_cfg dev_cfg;
	/* QSPI_IFR.WIDTH for dev_cfg.io_mode */
	uint32_t width;
	uint32_t mck_hz;
};

static int sam_qspi_width(enum mspi_io_mode io_mode, uint32_t *width)
{
	switch (io_mode) {
	case MSPI_IO_MODE_SINGLE:
		*width = QSPI_IFR_WIDTH_SINGLE_BIT_SPI_Val;
		break;
	case MSPI_IO_MODE_DUAL_1_1_2:
		*width = QSPI_IFR_WIDTH_DUAL_OUTPUT_Val;
		break;
	case MSPI_IO_MODE_QUAD_1_1_4:
		*width = QSPI_IFR_WIDTH_QUAD_OUTPUT_Val;
		break;
	case MSPI_IO_MODE_DUAL_1_2_2:
		*width = QSPI_IFR_WIDTH_DUAL_IO_Val;
		break;
	case MSPI_IO_MODE_QUAD_1_4_4:
		*width = QSPI_IFR_WIDTH_QUAD_IO_Val;
		break;
	case MSPI_IO_MODE_DUAL:
		*width = QSPI_IFR_WIDTH_DUAL_CMD_Val;
		break;
	case MSPI_IO_MODE_QUAD:
		*width = QSPI_IFR_WIDTH_QUAD_CMD_Val;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

/* Number of data lines of the address phase, which also carries the mode byte */
static uint32_t sam_qspi_addr_lines(uint32_t width)
{
	switch (width) {
	case QSPI_IFR_WIDTH_DUAL_IO_Val:
	case QSPI_IFR_WIDTH_DUAL_CMD_Val:
		return 2U;
	case QSPI_IFR_WIDTH_QUAD_IO_Val:
	case QSPI_IFR_WIDTH_QUAD_CMD_Val:
		return 4U;
	default:
		return 1U;
	}
}

static void sam_qspi_enable(sam_qspi_regs_t *regs, bool enable)
{
	if (enable) {
		regs->QSPI_CR = QSPI_CR_QSPIEN_Msk;
		while ((regs->QSPI_SR & QSPI_SR_QSPIENS_Msk) == 0U) {
		}
	} else {
		regs->QSPI_CR = QSPI_CR_QSPIDIS_Msk;
		while ((regs->QSPI_SR & QSPI_SR_QSPIENS_Msk) != 0U) {
		}
	}
}

/* Program the serial clock for dev_cfg.freq and dev_cfg.cpp */
static int sam_qspi_set_clock(const struct device *dev)
{
	const struct mspi_sam_qspi_config *cfg = dev->config;
	struct mspi_sam_qspi_data *data = dev->data;
	uint32_t divisor;
	uint32_t scr;

	if (data->dev_cfg.freq == 0U) {
		return -EINVAL;
	}

	/* Use the requested or the next lower possible frequency */
	divisor = DIV_ROUND_UP(data->mck_hz, data->dev_cfg.freq);
	divisor = CLAMP(divisor, 1U, SAM_QSPI_DIV_MAX);
	scr = QSPI_SCR_SCBR(divisor - 1U);

	switch (data->dev_cfg.cpp) {
	case MSPI_CPP_MODE_0:
		break;
	case MSPI_CPP_MODE_1:
		scr |= QSPI_SCR_CPHA_Msk;
		break;
	case MSPI_CPP_MODE_2:
		scr |= QSPI_SCR_CPOL_Msk;
		break;
	case MSPI_CPP_MODE_3:
		scr |= QSPI_SCR_CPOL_Msk | QSPI_SCR_CPHA_Msk;
		break;
	default:
		return -ENOTSUP;
	}

	if (cfg->regs->QSPI_SCR != scr) {
		sam_qspi_enable(cfg->regs, false);
		cfg->regs->QSPI_SCR = scr;
		sam_qspi_enable(cfg->regs, true);
	}

	LOG_DBG("QSCK %u Hz, mode %u", data->mck_hz / divisor, data->dev_cfg.cpp);

	return 0;
}

static int mspi_sam_qspi_config(const struct mspi_dt_spec *spec)
{
	const struct device *dev = spec->bus;
	const struct mspi_sam_qspi_config *cfg = dev->config;
	struct mspi_sam_qspi_data *data = dev->data;
	const struct mspi_cfg *config = &spec->config;
	sam_qspi_regs_t *regs = cfg->regs;
	uint32_t dlycs;
	int ret;

	if (config->op_mode != MSPI_OP_MODE_CONTROLLER) {
		LOG_ERR("Only controller mode is supported");
		return -ENOTSUP;
	}

	if (config->duplex != MSPI_HALF_DUPLEX) {
		LOG_ERR("Only half-duplex transfers are supported");
		return -ENOTSUP;
	}

	if (config->dqs_support) {
		LOG_ERR("DQS is not supported");
		return -ENOTSUP;
	}

	if ((config->num_periph > 1U) || (config->num_ce_gpios != 0U)) {
		LOG_ERR("Only one device, on QCS, is supported");
		return -ENOTSUP;
	}

	ret = clock_control_on(SAM_DT_PMC_CONTROLLER, (clock_control_subsys_t)&cfg->clock_cfg);
	if (ret < 0) {
		return ret;
	}

#ifdef SOC_ATMEL_SAM_MCK_FREQ_HZ
	data->mck_hz = SOC_ATMEL_SAM_MCK_FREQ_HZ;
#else
	/* SoCs without a fixed MCK rate (PIC32CZ CA70): ask the PMC. */
	ret = clock_control_get_rate(SAM_DT_PMC_CONTROLLER,
				     (clock_control_subsys_t)&cfg->clock_cfg, &data->mck_hz);
	if (ret < 0) {
		return ret;
	}
#endif

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/* The reset leaves the controller disabled, in SPI mode */
	regs->QSPI_CR = QSPI_CR_SWRST_Msk;

	/* QSPI_MR.DLYCS counts peripheral clock cycles */
	dlycs = DIV_ROUND_UP((uint64_t)data->mck_hz * SAM_QSPI_CS_HIGH_NS, NSEC_PER_SEC);
	regs->QSPI_MR = QSPI_MR_SMM_Msk | QSPI_MR_DLYCS(MIN(dlycs, 0xFFU));

	/* Until a device is configured: mode 0 at the slowest clock */
	regs->QSPI_SCR = QSPI_SCR_SCBR(SAM_QSPI_DIV_MAX - 1U);

	sam_qspi_enable(regs, true);

	return 0;
}

static int sam_qspi_check_dev_cfg(const enum mspi_dev_cfg_mask mask,
				  const struct mspi_dev_cfg *dev_cfg)
{
	uint32_t width;

	if (((mask & MSPI_DEVICE_CONFIG_CE_NUM) != 0U) && (dev_cfg->ce_num != 0U)) {
		LOG_ERR("Only chip select 0 (QCS) is available");
		return -ENOTSUP;
	}

	if (((mask & MSPI_DEVICE_CONFIG_IO_MODE) != 0U) &&
	    (sam_qspi_width(dev_cfg->io_mode, &width) < 0)) {
		LOG_ERR("Unsupported I/O mode %d", dev_cfg->io_mode);
		return -ENOTSUP;
	}

	if (((mask & MSPI_DEVICE_CONFIG_DATA_RATE) != 0U) &&
	    (dev_cfg->data_rate != MSPI_DATA_RATE_SINGLE)) {
		LOG_ERR("Only single data rate is supported");
		return -ENOTSUP;
	}

	if (((mask & MSPI_DEVICE_CONFIG_CPP) != 0U) && (dev_cfg->cpp > MSPI_CPP_MODE_3)) {
		return -EINVAL;
	}

	if (((mask & MSPI_DEVICE_CONFIG_CE_POL) != 0U) &&
	    (dev_cfg->ce_polarity != MSPI_CE_ACTIVE_LOW)) {
		LOG_ERR("Only an active low chip select is supported");
		return -ENOTSUP;
	}

	if (((mask & MSPI_DEVICE_CONFIG_DQS) != 0U) && dev_cfg->dqs_enable) {
		LOG_ERR("DQS is not supported");
		return -ENOTSUP;
	}

	if ((((mask & MSPI_DEVICE_CONFIG_RX_DUMMY) != 0U) &&
	     (dev_cfg->rx_dummy > SAM_QSPI_DUMMY_MAX)) ||
	    (((mask & MSPI_DEVICE_CONFIG_TX_DUMMY) != 0U) &&
	     (dev_cfg->tx_dummy > SAM_QSPI_DUMMY_MAX))) {
		LOG_ERR("At most %u dummy cycles are supported", SAM_QSPI_DUMMY_MAX);
		return -ENOTSUP;
	}

	if (((mask & MSPI_DEVICE_CONFIG_CMD_LEN) != 0U) && (dev_cfg->cmd_length > 1U)) {
		LOG_ERR("Only 8-bit commands are supported");
		return -ENOTSUP;
	}

	if (((mask & MSPI_DEVICE_CONFIG_ADDR_LEN) != 0U) && (dev_cfg->addr_length != 0U) &&
	    (dev_cfg->addr_length != 3U) && (dev_cfg->addr_length != 4U)) {
		LOG_ERR("Only 3- and 4-byte addresses are supported");
		return -ENOTSUP;
	}

	return 0;
}

static int mspi_sam_qspi_dev_config(const struct device *dev, const struct mspi_dev_id *dev_id,
				    const enum mspi_dev_cfg_mask param_mask,
				    const struct mspi_dev_cfg *dev_cfg)
{
	const struct mspi_sam_qspi_config *cfg = dev->config;
	struct mspi_sam_qspi_data *data = dev->data;
	struct mspi_dev_cfg *cur = &data->dev_cfg;
	bool locked = false;
	int ret;

	ret = sam_qspi_check_dev_cfg(param_mask, dev_cfg);
	if (ret < 0) {
		return ret;
	}

	if (data->dev_id != dev_id) {
		if (k_mutex_lock(&data->lock, K_MSEC(CONFIG_MSPI_COMPLETION_TIMEOUT_TOLERANCE)) !=
		    0) {
			return -EBUSY;
		}
		data->dev_id = dev_id;
		locked = true;
	}

	/* Acquire the controller without reconfiguring it */
	if ((param_mask == MSPI_DEVICE_CONFIG_NONE) && !cfg->mspicfg.sw_multi_periph) {
		return 0;
	}

	if ((param_mask & MSPI_DEVICE_CONFIG_CE_NUM) != 0U) {
		cur->ce_num = dev_cfg->ce_num;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_FREQUENCY) != 0U) {
		cur->freq = dev_cfg->freq;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_IO_MODE) != 0U) {
		cur->io_mode = dev_cfg->io_mode;
		(void)sam_qspi_width(cur->io_mode, &data->width);
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_DATA_RATE) != 0U) {
		cur->data_rate = dev_cfg->data_rate;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_CPP) != 0U) {
		cur->cpp = dev_cfg->cpp;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_ENDIAN) != 0U) {
		cur->endian = dev_cfg->endian;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_CE_POL) != 0U) {
		cur->ce_polarity = dev_cfg->ce_polarity;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_DQS) != 0U) {
		cur->dqs_enable = dev_cfg->dqs_enable;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_RX_DUMMY) != 0U) {
		cur->rx_dummy = dev_cfg->rx_dummy;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_TX_DUMMY) != 0U) {
		cur->tx_dummy = dev_cfg->tx_dummy;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_READ_CMD) != 0U) {
		cur->read_cmd = dev_cfg->read_cmd;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_WRITE_CMD) != 0U) {
		cur->write_cmd = dev_cfg->write_cmd;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_CMD_LEN) != 0U) {
		cur->cmd_length = dev_cfg->cmd_length;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_ADDR_LEN) != 0U) {
		cur->addr_length = dev_cfg->addr_length;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_MEM_BOUND) != 0U) {
		cur->mem_boundary = dev_cfg->mem_boundary;
	}
	if ((param_mask & MSPI_DEVICE_CONFIG_BREAK_TIME) != 0U) {
		cur->time_to_break = dev_cfg->time_to_break;
	}

	if ((param_mask & (MSPI_DEVICE_CONFIG_FREQUENCY | MSPI_DEVICE_CONFIG_CPP)) != 0U) {
		ret = sam_qspi_set_clock(dev);
		if (ret < 0) {
			if (locked) {
				data->dev_id = NULL;
				(void)k_mutex_unlock(&data->lock);
			}
			return ret;
		}
	}

	return 0;
}

/*
 * Copy from the serial memory window. Device memory does not allow unaligned
 * accesses: the window is read with byte accesses up to a word boundary, then
 * with word accesses. The window is little-endian, as the AHB.
 */
static void sam_qspi_read_window(mem_addr_t win, uint8_t *buf, uint32_t len)
{
	uint32_t i = 0U;

	while ((i < len) && (((win + i) & 3U) != 0U)) {
		buf[i] = sys_read8(win + i);
		i++;
	}

	while ((len - i) >= 4U) {
		sys_put_le32(sys_read32(win + i), &buf[i]);
		i += 4U;
	}

	while (i < len) {
		buf[i] = sys_read8(win + i);
		i++;
	}
}

static void sam_qspi_write_window(mem_addr_t win, const uint8_t *buf, uint32_t len)
{
	uint32_t i = 0U;

	while ((i < len) && (((win + i) & 3U) != 0U)) {
		sys_write8(buf[i], win + i);
		i++;
	}

	while ((len - i) >= 4U) {
		sys_write32(sys_get_le32(&buf[i]), win + i);
		i += 4U;
	}

	while (i < len) {
		sys_write8(buf[i], win + i);
		i++;
	}
}

/* Wait for the end of the frame: instruction end and chip select rise */
static int sam_qspi_wait_frame_end(sam_qspi_regs_t *regs, k_timepoint_t end)
{
	const uint32_t done = QSPI_SR_INSTRE_Msk | QSPI_SR_CSR_Msk;
	uint32_t sr = 0U;

	/* INSTRE and CSR are cleared on read: accumulate them */
	while (true) {
		sr |= regs->QSPI_SR;
		if ((sr & done) == done) {
			return 0;
		}

		if (sys_timepoint_expired(end)) {
			/* Check again: the thread may have been preempted */
			sr |= regs->QSPI_SR;
			return ((sr & done) == done) ? 0 : -ETIMEDOUT;
		}
	}
}

static int sam_qspi_xfer_packet(const struct device *dev, const struct mspi_xfer *xfer,
				const struct mspi_xfer_packet *pkt, k_timepoint_t end)
{
	const struct mspi_sam_qspi_config *cfg = dev->config;
	struct mspi_sam_qspi_data *data = dev->data;
	sam_qspi_regs_t *regs = cfg->regs;
	uint32_t ifr = QSPI_IFR_WIDTH(data->width);
	uint32_t icr = 0U;
	uint32_t offset = 0U;
	uint32_t dummy = (pkt->dir == MSPI_RX) ? xfer->rx_dummy : xfer->tx_dummy;
	bool has_data = (pkt->num_bytes != 0U);

	if (xfer->cmd_length == 1U) {
		ifr |= QSPI_IFR_INSTEN_Msk;
		icr |= QSPI_ICR_INST(pkt->cmd);
	}

	if (xfer->addr_length != 0U) {
		ifr |= QSPI_IFR_ADDREN_Msk;
		if (xfer->addr_length == 4U) {
			ifr |= QSPI_IFR_ADDRL_Msk;
		}
	}

	/*
	 * Reads with a multi-line address phase: send the mode byte in place
	 * of the first dummy cycles rather than leaving the lines floating.
	 */
	if ((pkt->dir == MSPI_RX) && has_data && (xfer->addr_length != 0U)) {
		uint32_t mode_cycles = 8U / sam_qspi_addr_lines(data->width);

		if ((mode_cycles < 8U) && (dummy >= mode_cycles)) {
			ifr |= QSPI_IFR_OPTEN_Msk | QSPI_IFR_OPTL_OPTION_8BIT;
			icr |= QSPI_ICR_OPT(SAM_QSPI_MODE_BYTE);
			dummy -= mode_cycles;
		}
	}

	if (dummy > SAM_QSPI_DUMMY_MAX) {
		return -ENOTSUP;
	}
	ifr |= QSPI_IFR_NBDUM(dummy);

	if (has_data) {
		ifr |= QSPI_IFR_DATAEN_Msk;

		/*
		 * With an address, the frame is a memory transfer: the
		 * controller sends the window offset as the address.
		 * Without, the window offset does not matter.
		 */
		if (xfer->addr_length != 0U) {
			offset = pkt->address;
			ifr |= QSPI_IFR_TFRTYP((pkt->dir == MSPI_RX)
						       ? QSPI_IFR_TFRTYP_TRSFR_READ_MEMORY_Val
						       : QSPI_IFR_TFRTYP_TRSFR_WRITE_MEMORY_Val);
		} else {
			ifr |= QSPI_IFR_TFRTYP((pkt->dir == MSPI_RX)
						       ? QSPI_IFR_TFRTYP_TRSFR_READ_Val
						       : QSPI_IFR_TFRTYP_TRSFR_WRITE_Val);
		}

		if ((offset >= cfg->mem_size) || (pkt->num_bytes > (cfg->mem_size - offset))) {
			LOG_ERR("Transfer at 0x%x (%u bytes) is outside the window", offset,
				pkt->num_bytes);
			return -EINVAL;
		}
	}

	/* Clear the status flags of the previous frame */
	(void)regs->QSPI_SR;

	if (!has_data && (xfer->addr_length != 0U)) {
		regs->QSPI_IAR = pkt->address;
	}
	regs->QSPI_ICR = icr;
	/* Without a data phase, this write starts the frame */
	regs->QSPI_IFR = ifr;

	if (has_data) {
		/* Synchronize the APB and AHB accesses */
		(void)regs->QSPI_IFR;

		if (pkt->dir == MSPI_RX) {
			sam_qspi_read_window(cfg->mem_base + offset, pkt->data_buf,
					     pkt->num_bytes);
		} else {
			sam_qspi_write_window(cfg->mem_base + offset, pkt->data_buf,
					      pkt->num_bytes);
		}

		barrier_dsync_fence_full();
		regs->QSPI_CR = QSPI_CR_LASTXFER_Msk;
	}

	return sam_qspi_wait_frame_end(regs, end);
}

static int mspi_sam_qspi_transceive(const struct device *dev, const struct mspi_dev_id *dev_id,
				    const struct mspi_xfer *xfer)
{
	struct mspi_sam_qspi_data *data = dev->data;
	k_timepoint_t end;
	int ret = 0;

	if (dev_id != data->dev_id) {
		LOG_ERR("Controller not acquired by this device");
		return -ESTALE;
	}

	if (xfer->async || (xfer->xfer_mode != MSPI_PIO)) {
		LOG_DBG("Only synchronous PIO transfers are supported");
		return -ENOTSUP;
	}

	if ((xfer->num_packet == 0U) || (xfer->packets == NULL)) {
		return -EINVAL;
	}

	if ((xfer->cmd_length > 1U) ||
	    ((xfer->addr_length != 0U) && (xfer->addr_length != 3U) &&
	     (xfer->addr_length != 4U))) {
		return -ENOTSUP;
	}

	end = sys_timepoint_calc(K_MSEC(xfer->timeout));

	for (uint32_t i = 0U; i < xfer->num_packet; i++) {
		const struct mspi_xfer_packet *pkt = &xfer->packets[i];

		ret = sam_qspi_xfer_packet(dev, xfer, pkt, end);
		if (ret < 0) {
			LOG_ERR("Packet %u (cmd 0x%02x) failed: %d", i, pkt->cmd, ret);
			break;
		}
	}

	return ret;
}

static int mspi_sam_qspi_get_channel_status(const struct device *dev, uint8_t ch)
{
	const struct mspi_sam_qspi_config *cfg = dev->config;
	struct mspi_sam_qspi_data *data = dev->data;

	ARG_UNUSED(ch);

	/* QSPI_SR.CSS is 0 while QCS is asserted, during a frame */
	if ((cfg->regs->QSPI_SR & QSPI_SR_CSS_Msk) == 0U) {
		return -EBUSY;
	}

	if (data->dev_id != NULL) {
		data->dev_id = NULL;
		return k_mutex_unlock(&data->lock);
	}

	return 0;
}

static int mspi_sam_qspi_init(const struct device *dev)
{
	const struct mspi_sam_qspi_config *cfg = dev->config;
	struct mspi_sam_qspi_data *data = dev->data;
	const struct mspi_dt_spec spec = {
		.bus = dev,
		.config = cfg->mspicfg,
	};

	k_mutex_init(&data->lock);
	data->dev_cfg.io_mode = MSPI_IO_MODE_SINGLE;
	data->width = QSPI_IFR_WIDTH_SINGLE_BIT_SPI_Val;

	return mspi_sam_qspi_config(&spec);
}

static DEVICE_API(mspi, mspi_sam_qspi_api) = {
	.config = mspi_sam_qspi_config,
	.dev_config = mspi_sam_qspi_dev_config,
	.get_channel_status = mspi_sam_qspi_get_channel_status,
	.transceive = mspi_sam_qspi_transceive,
};

#define MSPI_SAM_QSPI_INIT(n)                                                                      \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static const struct mspi_sam_qspi_config mspi_sam_qspi_config_##n = {                      \
		.regs = (sam_qspi_regs_t *)DT_INST_REG_ADDR_BY_NAME(n, qspi),                      \
		.mem_base = DT_INST_REG_ADDR_BY_NAME(n, qspi_mm),                                  \
		.mem_size = DT_INST_REG_SIZE_BY_NAME(n, qspi_mm),                                  \
		.clock_cfg = SAM_DT_INST_CLOCK_PMC_CFG(n),                                         \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.mspicfg =                                                                         \
			{                                                                          \
				.op_mode = MSPI_OP_MODE_CONTROLLER,                                \
				.duplex = MSPI_HALF_DUPLEX,                                        \
				.max_freq = DT_INST_PROP_OR(n, clock_frequency, 0),                \
				.num_periph = DT_INST_CHILD_NUM(n),                                \
				.sw_multi_periph = DT_INST_PROP(n, software_multiperipheral),      \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static struct mspi_sam_qspi_data mspi_sam_qspi_data_##n;                                   \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, mspi_sam_qspi_init, NULL, &mspi_sam_qspi_data_##n,                \
			      &mspi_sam_qspi_config_##n, POST_KERNEL, CONFIG_MSPI_INIT_PRIORITY,   \
			      &mspi_sam_qspi_api);

DT_INST_FOREACH_STATUS_OKAY(MSPI_SAM_QSPI_INIT)
