# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_sources(soc.c)

if(CONFIG_CLOCK_CONTROL_SAMA7G5)
  set(SAM_PMC_DIR ${ZEPHYR_BASE}/soc/microchip/sam/common)

  zephyr_include_directories(${SAM_PMC_DIR})
  zephyr_sources(
    clk_pic32cz_ca70.c
    ${SAM_PMC_DIR}/clk-main.c
    ${SAM_PMC_DIR}/clk-master-mckr.c
    ${SAM_PMC_DIR}/clk-peripheral.c
    ${SAM_PMC_DIR}/clk-plla.c
    ${SAM_PMC_DIR}/pmc.c
  )
endif()
