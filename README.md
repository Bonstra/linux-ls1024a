LS1024A SoC support in Linux 5.x
================================

This repository contains a v5.x Linux kernel with additional drivers and
support for Freescale's LS1024A SoC and the QNAP TS-x31 family of NAS machines.

My goal is to get a QNAP TS-x31 NAS running with most features on a v5.x Linux
kernel. When the code is stable enough (i.e. I don't need to break older stuff
when I write a new driver), I intend to submit patches for integration in
Linux mainline.

**Be advised that this is a work in progress and that this branch WILL
be updated using force push.**

Drivers specific to the LS1024A have been re-written from scratch to make use
of the various frameworks which were introduced since the outdated 3.2.26
kernel Freescale is providing (common clock framework, device tree, pinctrl and
PHY frameworks).


What's working:
---------------

- Dual Cortex A9 SMP
- UART0
- UART1
- Clock controller
- Pin muxing
- GPIOs
- PCIe
- USB 3.0 in host mode
- SATA
- I2C
- SPI
- Simple CPU frequency scaling
- Watchdog timer
- Powering off using PIC on UART0

What's NOT working yet (but I'll be working on it soon):
--------------------------------------------------------

- Blob-less PFE (Gigabit ethernet)
- NAND controller + expansion bus
- DMA controller
- Timers
- Crypto engine (SPACC)

What's NOT working yet and is low on the priority list:
-------------------------------------------------------

- Suspend to RAM
- PMU (when I realize the system doesn't wake up :-)  )
- Other power-saving features (USB, PCIe, SATA, DVFS, Wake on LAN)
- OTP memory (read-only)
- TrustZone (maybe, it could be fun)

What's NOT working (and I won't try):
-------------------------------------

- RTC (documented, but the QNAP TS-x31 lacks a 32 KHz oscillator)
- I2S (not routed on my hardware)
- DPI (no documentation or source code available)
- IPSEC (no documentation or source code available)
- DECT (not routed on my hardware)
- TDM (not routed on my hardware)
- PFE with firmware binary blobs (too many security concerns: firmware is very
  complex, ISA (eSi-RISC) is not publicly documented, PEs have access to the
  DDR and peripherals)

Known issues:
-------------

- CPU1 cannot be brought back up when put offline
- System may hang without a clear reason (watchdog reset, no trace on serial
  console). Happens rarely, which make it even the more difficult to debug.

Changelog:
----------

2021-03-21:

- Added PCIe driver
- UART0 and SPI support
- Enabled SPI NOR flash
- Added simple CPU frequency scaling
- Rebased on Linux v5.11
- Various SerDes PHY changes
- Minor watchdog changes
- Disabled Cortex-A9 global timer as it prevents booting when SMP is enabled
- CPU nodes fixes in DT