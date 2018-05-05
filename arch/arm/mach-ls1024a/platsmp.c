// SPDX-License-Identifier: GPL-2.0
/*
 * SMP operations for LS1024A
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/memory.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#define A9DP_PWR_CNTRL		0x2c
#define CLAMP_CORE0		BIT(4)
#define CORE_PWRDWN0		BIT(5)
#define CLAMP_CORE1		BIT(6)
#define CORE_PWRDWN1		BIT(7)

#define A9DP_CPU_CLK_CNTRL	0x74
#define CPU0_CLK_ENABLE		BIT(0)
#define NEON0_CLK_ENABLE	BIT(1)
#define CPU1_CLK_ENABLE		BIT(2)
#define NEON1_CLK_ENABLE	BIT(3)

#define A9DP_CPU_RESET		0x78
#define CPU0_RST		BIT(0)
#define NEON0_RST		BIT(1)
#define CPU1_RST		BIT(2)
#define NEON1_RST		BIT(3)

#define CPU_VECTORS_PHYS	0x00000000
#define ARM_JUMP_TO_KERNEL	0xe59ff018 	/* ldr pc, [pc, #0x1c] */
#define SW_RESET_ADDR		0x20

static int ls1024a_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned int cpuid;
	struct regmap *regs = syscon_regmap_lookup_by_compatible(
			"fsl,ls1024a-clkcore");
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	cpuid = cpu_logical_map(cpu) & 0x3;
	if (cpuid != 1)
		return -EINVAL;

	/* Get CPU 1 out of reset */
	regmap_update_bits(regs, A9DP_CPU_RESET, CPU1_RST, 0);
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CLAMP_CORE1, 0);
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL, CPU1_CLK_ENABLE,
			CPU1_CLK_ENABLE);
#ifdef CONFIG_NEON
	/* Get NEON 1 out of reset */
	regmap_update_bits(regs, A9DP_CPU_RESET, NEON1_RST, 0);
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL, NEON1_CLK_ENABLE,
			NEON1_CLK_ENABLE);
#endif
	return 0;
}

static void __init ls1024a_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	void __iomem *scu_base;
	void __iomem *vectors_base;

	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	scu_base = of_iomap(np, 0);
	of_node_put(np);
	if (!scu_base)
		return;

	vectors_base = phys_to_virt(CPU_VECTORS_PHYS);
	if (!vectors_base)
		goto unmap_scu;

	scu_enable(scu_base);
	flush_cache_all();

	/*
	 * Write the first instruction the CPU will execute after being reset
	 * in the reset exception vector.
	 */
	writel(ARM_JUMP_TO_KERNEL, vectors_base);

	/*
	 * Write the secondary startup address into the SW reset address
	 * vector.
	 */
	writel(__pa_symbol(secondary_startup), vectors_base + SW_RESET_ADDR);
	smp_wmb();
	__sync_cache_range_w(vectors_base, 0x24);

unmap_scu:
	iounmap(scu_base);
}

#ifdef CONFIG_HOTPLUG_CPU
static void ls1024a_cpu_die(unsigned int cpu)
{
	v7_exit_coherency_flush(louis);
	while (1)
		cpu_do_idle();
}

static int ls1024a_cpu_kill(unsigned int cpu)
{
	unsigned int cpuid;
	struct regmap *regs = syscon_regmap_lookup_by_compatible(
			"fsl,ls1024a-clkcore");
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	cpuid = cpu_logical_map(cpu) & 0x3;
	if (cpuid != 1)
		return -EINVAL;

#ifdef CONFIG_NEON
	/* Put NEON 1 in reset */
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL, NEON1_CLK_ENABLE, 0);
	regmap_update_bits(regs, A9DP_CPU_RESET, NEON1_RST, NEON1_RST);
#endif
	/* Put CPU 1 in reset */
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL, CPU1_CLK_ENABLE, 0);
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CLAMP_CORE1, CLAMP_CORE1);
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CORE_PWRDWN1, CORE_PWRDWN1);
	regmap_update_bits(regs, A9DP_CPU_RESET, CPU1_RST, CPU1_RST);
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CORE_PWRDWN1, 0);
	return 1;
}
#endif

static const struct smp_operations ls1024a_smp_ops __initconst = {
	.smp_prepare_cpus	= ls1024a_smp_prepare_cpus,
	.smp_boot_secondary	= ls1024a_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= ls1024a_cpu_die,
	.cpu_kill		= ls1024a_cpu_kill,
#endif
};
CPU_METHOD_OF_DECLARE(ls1024a_smp, "fsl,ls1024a-smp", &ls1024a_smp_ops);

