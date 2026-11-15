/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SOC_MICROCHIP_PIC32CX_SG_COMMON_SOC_IDLE_INHIBIT_H_
#define SOC_MICROCHIP_PIC32CX_SG_COMMON_SOC_IDLE_INHIBIT_H_

/**
 * @file
 * @brief Keep the CPU out of the WFI sleep while a bus master needs its clock.
 *
 * The only sleep Zephyr uses on this SoC is the WFI of the idle thread, and the reset value
 * of PM.SLEEPCFG makes it stop the CPU, AHB and APB clocks. A peripheral that is a bus master,
 * such as the CAN controller, cannot read or write RAM without the AHB clock: its transfers
 * stall until the CPU wakes up and frames are lost or corrupted.
 *
 * While at least one inhibit is held, the idle thread does not execute WFI. Calls can nest.
 */

/** Forbid the idle thread from executing WFI until a matching soc_cpu_idle_inhibit_put(). */
void soc_cpu_idle_inhibit_get(void);

/** Release one soc_cpu_idle_inhibit_get(). WFI is allowed again when the last one is released. */
void soc_cpu_idle_inhibit_put(void);

#endif /* SOC_MICROCHIP_PIC32CX_SG_COMMON_SOC_IDLE_INHIBIT_H_ */
