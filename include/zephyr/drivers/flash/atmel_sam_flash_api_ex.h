/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Extended operations of the Atmel SAM EEFC flash driver.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_FLASH_ATMEL_SAM_FLASH_API_EX_H_
#define ZEPHYR_INCLUDE_DRIVERS_FLASH_ATMEL_SAM_FLASH_API_EX_H_

#include <zephyr/drivers/flash.h>

/**
 * @brief Extended operations of the atmel,sam-flash-controller driver.
 *
 * The general-purpose NVM (GPNVM) bits select, for example, the boot source
 * (bit 1: 0 = ROM, 1 = flash) and the TCM configuration on SAM E70/S70/V70/V71
 * and PIC32CZ CA70/MC70. See the device data sheet for the bit assignments.
 */
enum atmel_sam_flash_ex_ops {
	/**
	 * @brief Read the GPNVM bits.
	 *
	 * @p in is unused. @p out points to a @c uint32_t that receives the
	 * GPNVM bits, bit n holding GPNVM bit n.
	 */
	FLASH_SAM_EX_OP_GPNVM_GET = FLASH_EX_OP_VENDOR_BASE,

	/**
	 * @brief Set one GPNVM bit.
	 *
	 * @p in is the bit number, @p out is unused. Setting bit 0, the
	 * security bit, is refused with -EPERM: it can only be cleared by a
	 * full chip erase.
	 */
	FLASH_SAM_EX_OP_GPNVM_SET,

	/**
	 * @brief Clear one GPNVM bit.
	 *
	 * @p in is the bit number, @p out is unused.
	 */
	FLASH_SAM_EX_OP_GPNVM_CLEAR,
};

#endif /* ZEPHYR_INCLUDE_DRIVERS_FLASH_ATMEL_SAM_FLASH_API_EX_H_ */
