.. zephyr:board:: sam_e70_cult

Overview
********

This board runs Zephyr on the PIC32CZ CA70 Curiosity Ultra development board
with the SAM E70 rev B SoC support (``same70q21b``) instead of the PIC32CZ CA70
SoC support of :zephyr:board:`pic32cz_ca70_cult`. The PIC32CZ CA70 is SAM E70
rev B silicon: the SAM E70 clock setup, pin multiplexing and drivers apply to
it unchanged.

Hardware
********

- 144-pin PIC32CZ2051CA70144 microcontroller, Cortex-M7 at up to 300 MHz
- 2 MB of flash and 384 KB of SRAM (the SAM E70 default TCM configuration)
- 12 MHz and 32.768 kHz crystals
- On-board debugger (PKoB4) with virtual COM port
- Two user LEDs and two user push buttons
- Two CAN FD transceivers
- Ethernet RMII interface to a modular PHY header
- 32 Mbit QSPI flash
- AT24MAC402 EEPROM with EUI-48 MAC address
- Arduino Uno R3, mikroBUS and Xplained Pro extension headers

Supported Features
==================

.. zephyr:board-supported-hw::

Connections and IOs
===================

The pin assignments are those of :zephyr:board:`pic32cz_ca70_cult`:

- Console on UART1 (PA5 RX, PA6 TX), the on-board debugger virtual COM port,
  at 115200 baud.
- User LEDs LED0 on PD23 and LED1 on PD29, user buttons SW0 on PA1 and SW1 on
  PA9, all active low.
- AT24MAC402 EEPROM on TWIHS0 (PA3 SDA, PA4 SCL) at 0x56 (``eeprom-0``), with
  the factory EUI-48 MAC address at 0x5e.
- GMAC RMII on PD0 to PD9 to the PHY daughter board header J700/J701, PHY at
  MDIO address 0.
- MCAN0 (PB2 TX, PB3 RX) and MCAN1 (PC14 TX, PC12 RX) to the ATA6561 CAN FD
  transceivers. MCAN0 is the ``zephyr,canbus`` controller.
- SPI0 (PD20 MISO, PD21 MOSI, PD22 SCK) to the Arduino and mikroBUS headers.
- USART0 on the mikroBUS socket J803 and UART4 on J402, described but
  disabled.
- SST26VF032B QSPI flash, driven as a single-line SPI NOR flash through the
  QSPI controller in SPI mode, at 50 MHz, erased in 4 KiB sectors.

System Clock
============

The 12 MHz crystal drives PLLA at 300 MHz (the SAM E70 SoC defaults). The
processor runs at 300 MHz and the master clock (MCK) at 150 MHz. The
32.768 kHz crystal drives the slow clock
(:kconfig:option:`CONFIG_SOC_ATMEL_SAM_EXT_SLCK`).

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

The programming tools see a PIC32CZ2051CA70144. Flash as described for
:zephyr:board:`pic32cz_ca70_cult`, including setting GPNVM bit 1 to boot from
flash.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: sam_e70_cult
   :goals: build flash
   :flash-args: -r jlink
