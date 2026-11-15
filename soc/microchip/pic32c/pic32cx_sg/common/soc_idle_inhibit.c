/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/arm/misc.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/atomic.h>

#include "soc_idle_inhibit.h"

static atomic_t idle_inhibit_count;

void soc_cpu_idle_inhibit_get(void)
{
	(void)atomic_inc(&idle_inhibit_count);
}

void soc_cpu_idle_inhibit_put(void)
{
	atomic_val_t old = atomic_dec(&idle_inhibit_count);

	__ASSERT(old > 0, "unbalanced CPU idle inhibit release");
	ARG_UNUSED(old);
}

/* Called by the idle thread before WFI. Returning false skips the WFI. */
bool z_arm_on_enter_cpu_idle(void)
{
	return atomic_get(&idle_inhibit_count) == 0;
}
