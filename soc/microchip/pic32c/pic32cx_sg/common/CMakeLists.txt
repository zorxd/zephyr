# Copyright (c) 2025 Microchip Technology Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")

if(CONFIG_CAN_MCHP_MCAN_G1 AND (CONFIG_SRAM_SIZE GREATER 64))
  zephyr_linker_sources(RAM_SECTIONS can_message_ram.ld)
endif()
