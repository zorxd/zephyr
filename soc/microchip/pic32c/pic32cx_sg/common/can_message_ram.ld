/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Place the MCAN Message RAM buffers in a dedicated section to ensure they are
 * located in the first 64 KB of SRAM. The M_CAN Message RAM element addresses
 * are 16-bit offsets from the start of SRAM (0x20000000).
 */

SECTION_PROLOGUE(.can_message_ram,(NOLOAD),)
{
    . = ALIGN(4);
    *(.can_message_ram)
    . = ALIGN(4);
} GROUP_LINK_IN(RAMABLE_REGION)

ASSERT((. - RAM_ADDR) <= KB(64), "Error: PIC32CX SG MCAN MRAM buffers exceed 64 KB SRAM boundary!")
