/* SPDX-License-Identifier: GPL-2.0 */
/* Device Tree binding constants for Freescale LS2014A clock controller. */

#ifndef _DT_BINDINGS_CLOCK_LS1024A_H
#define _DT_BINDINGS_CLOCK_LS1024A_H

/* Total number of clocks. */
#define LS1024A_NUM_CLKS 46

/* PLL reference clock. 24/48 MHz clock/crystal */
#define LS1024A_CLK_REF 0
/* Same as LS1024A_CLK_REF, but divided to be 48 MHz at most */
#define LS1024A_CLK_INT_REF 1
/* PLL outputs */
#define LS1024A_CLK_PLL0 2
#define LS1024A_CLK_PLL1 3
#define LS1024A_CLK_PLL2 4
#define LS1024A_CLK_PLL3 5
/* PLL ext bypass */
#define LS1024A_CLK_PLL0_EXT_BYPASS_MUX 6
#define LS1024A_CLK_PLL1_EXT_BYPASS_MUX 7
#define LS1024A_CLK_PLL2_EXT_BYPASS_MUX 8
#define LS1024A_CLK_PLL3_EXT_BYPASS_MUX 9
/* AXI fabric clock */
#define LS1024A_CLK_AXI 10
/* ARM A9 CPU clock */
#define LS1024A_CLK_A9DP 11
/* ARM A9 peripherals clock. Sourced from LS1024A_CLK_A9DP */
#define LS1024A_CLK_A9DP_MPU 12
/* L2 cache controller clock. Sourced from LS1024A_CLK_A9DP */
#define LS1024A_CLK_L2CC 13
/* CoreSight clock */
#define LS1024A_CLK_CSYS 14
/* Trace Port Interface clock */
#define LS1024A_CLK_TPI 15
/* Packet Forwarding Engine core clock */
#define LS1024A_CLK_PFE 16
/* Gigabit Ethernet MAC Tx clock */
#define LS1024A_CLK_GEM_TX 17
/* DDR PHY clock */
#define LS1024A_CLK_DDR 18
/* CSS (DECT) subsystem clock */
#define LS1024A_CLK_DECT 19
/* Security accelerator "IPSec" clock */
#define LS1024A_CLK_CRYPTO 20
/* SATA clocks */
#define LS1024A_CLK_SATA_PMU 21
#define LS1024A_CLK_SATA_OOB 22
/* Reference output clocks */
#define LS1024A_CLK_SATA_OCC 23
#define LS1024A_CLK_PCIE_OCC 24
#define LS1024A_CLK_SGMII_OCC 25
/* TDM & TSU clocks */
#define LS1024A_CLK_TSU_NTG 26
#define LS1024A_CLK_TDM_NTG 27
/* External bus interface clocks */
#define LS1024A_CLK_EXTPHY0 28
#define LS1024A_CLK_EXTPHY1 29
#define LS1024A_CLK_EXTPHY2 30
/* AXI slaves gates */
#define LS1024A_CLK_DPI_CIE 31
#define LS1024A_CLK_DPI_DECOMP 32
#define LS1024A_CLK_DUS 33
#define LS1024A_CLK_IPSEC_EAPE 34
#define LS1024A_CLK_IPSEC_SPACC 35
#define LS1024A_CLK_PFE_SYS 36
#define LS1024A_CLK_TDM 37
#define LS1024A_CLK_I2CSPI 38
#define LS1024A_CLK_UART 39
#define LS1024A_CLK_RTC_TIM 40
#define LS1024A_CLK_PCIE0 41
#define LS1024A_CLK_PCIE1 42
#define LS1024A_CLK_SATA 43
#define LS1024A_CLK_USB0 44
#define LS1024A_CLK_USB1 45

#endif /* _DT_BINDINGS_CLOCK_LS1024A_H */

