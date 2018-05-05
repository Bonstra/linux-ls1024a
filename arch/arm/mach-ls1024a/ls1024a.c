// SPDX-License-Identifier: GPL-2.0
/*
 * LS1024A DT board support code
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/clocksource.h>
#include <linux/irqchip.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

static void __init ls1024a_map_io(void)
{
	debug_ll_io_init();
}

static const char *const ls1024a_compat[] __initconst = {
	"fsl,ls1024a",
	NULL,
};

DT_MACHINE_START(LS1024A, "Freescale LS1024A")
	.map_io		= ls1024a_map_io,
	.dt_compat	= ls1024a_compat,
MACHINE_END

