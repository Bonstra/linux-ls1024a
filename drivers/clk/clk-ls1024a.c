// SPDX-License-Identifier: GPL-2.0
/*
 * Freescale LS1024A clock and reset controller driver
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <dt-bindings/clock/ls1024a.h>

/* GPIO registers */

#define GPIO_SYSTEM_CONFIG		0x1c
#define USB_OSC_FREQ			BIT(5)
#define SERDES_OSC_FREQ			BIT(7)
#define SYS_PLL_REF_CLK_MASK		(BIT(8) | BIT(9))
#define SYS_PLL_REF_CLK_SHIFT		8;

#define SYS_PLL_REF_USB_XTAL		0
#define SYS_PLL_REF_SERDES0_CLKOUT	1
#define SYS_PLL_REF_SERDER2_CLKOUT	2
#define SYS_PLL_REF_SERDES_XTAL		3

/* CLKCORE registers */

#define GNRL_CLK_CNTRL_1	0x34
#define GLOBAL_BYPASS		BIT(0)

#define PLLS_GLOBAL_CNTRL	0x38

#define AXI_CLK_CNTRL0		0x40
#define AXI_CLK_CNTRL1		0x44
#define AXI_CLK_CNTRL2		0x48

#define A9DP_CPU_CLK_CNTRL	0x74

/* XXX_CLK_CNTRL register */
#define CNTRL_CLOCK_DOMAIN_ENABLE	BIT(0)
#define CNTRL_CLOCK_SOURCE_MASK		0x07
#define CNTRL_CLOCK_SOURCE_SHIFT	1

/* XXX_CLK_DIV_CNTRL register */
#define DIV_CNTRL_BYPASS	BIT(7)
#define DIV_CNTRL_DIVIDER_MASK	0x1f
#define DIV_CNTRL_DIVIDER_MAX	0x1f

#define PLL0_BASE		0x1c0
#define PLL1_BASE		0x1e0
#define PLL2_BASE		0x200
#define PLL3_BASE		0x220

#define PLLx_M_LSB		0x00
#define PLLx_M_MSB		0x04
#define PLLx_P			0x08
#define PLLx_S			0x0c
#define PLLx_CNTRL		0x10
#define PLLx_TEST		0x14
#define PLLx_STATUS		0x18
#define PLLx_DIV_CNTRL		0x1c

#define PLL3_M_LSB		0x220
#define PLL3_M_MSB		0x224
#define PLL3_P			0x228
#define PLL3_S			0x22c
#define PLL3_CNTRL		0x230
#define PLL3_TEST		0x234
#define PLL3_STATUS		0x238
#define PLL3_DITHER_CNTRL	0x23c
#define PLL3_K_LSB		0x240
#define PLL3_K_MSB		0x244
#define PLL3_MFR		0x248
#define PLL3_MRR		0x24c

#define PLLx_M_LSB_MASK		0xff
#define PLLx_M_MSB_MASK		0x03
#define PLL3_M_LSB_MASK		0xff
#define PLL3_M_MSB_MASK		0x01
#define PLLx_P_MASK		0x3f
#define PLLx_S_MASK		0x07
#define PLL3_K_LSB_MASK		0xff
#define PLL3_K_MSB_MASK		0x0f

#define PLLx_CNTRL_BYPASS	BIT(4)
#define PLLx_CNTRL_RESET	BIT(0)

#define PLLx_DIV_VALUE_MASK	0x1f
#define PLLx_DIV_BYPASS		BIT(7)

struct clk_ls1024a_pll {
	struct clk_hw hw;
	struct regmap	*map;
	unsigned int base;
	bool has_divider;
};
#define to_clk_ls1024a_pll(_hw) \
	container_of(_hw, struct clk_ls1024a_pll, hw)

struct clk_ls1024a_pll3 {
	struct clk_hw hw;
	struct regmap	*map;
};
#define to_clk_ls1024a_pll3(_hw) \
	container_of(_hw, struct clk_ls1024a_pll3, hw)

struct clk_ls1024a_clkgen {
	struct clk_hw	hw;
	struct regmap	*map;
	unsigned int	base;
	unsigned int	div_reg_offset;
	/* Save the value of the divider bypass bit, which cannot be read back
	 * from its register due to a hardware bug. */
	bool		bypass;
	unsigned long	max_rate;
};
#define to_clk_ls1024a_clkgen(_hw) \
	container_of(_hw, struct clk_ls1024a_clkgen, hw)

/* Keeps track of all clocks */
static struct clk_hw_onecell_data *ls1024a_clk_data;

enum div_bypass_src {
	DIV_BYPASS_0,
	DIV_BYPASS_1,
	DIV_BYPASS_IRAM
};

struct ls1024a_clkgen_info {
	unsigned int idx;
	const char *name;
	const char **parents;
	u8 num_parents;
	unsigned int base;
	/* Offset of the divider control register relative to base */
	unsigned int div_reg_offset;
	enum div_bypass_src bypass_src;
	unsigned long max_rate;
	unsigned long flags;
};

static const char *parents_a9dp_only[] = { "a9dp" };

static const char *parents_all_plls[] = {
	"pll0_mux", "pll1_mux", "pll2_mux", "pll3_mux", "int_refclk"
};

static const struct ls1024a_clkgen_info clkgens_info[] = {
	{
		LS1024A_CLK_AXI, "axi",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x040, 12, DIV_BYPASS_IRAM,
		250000000, CLK_IS_CRITICAL
	},
	{
		LS1024A_CLK_A9DP, "a9dp",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x080, 4, DIV_BYPASS_IRAM,
		1200000000, CLK_IS_CRITICAL
	},
	{
		LS1024A_CLK_L2CC, "l2cc",
		parents_a9dp_only, ARRAY_SIZE(parents_a9dp_only),
		0x090, 4, DIV_BYPASS_IRAM,
		600000000, CLK_IS_CRITICAL
	},
	{
		LS1024A_CLK_TPI, "tpi",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x0a0, 4, DIV_BYPASS_IRAM,
		250000000, 0
	},
	{
		LS1024A_CLK_CSYS, "csys",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x0b0, 4, DIV_BYPASS_IRAM,
		166000000, 0
	},
	{
		LS1024A_CLK_EXTPHY0, "extphy0",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x0c0, 4, DIV_BYPASS_IRAM,
		125000000, 0
	},
	{
		LS1024A_CLK_EXTPHY1, "extphy1",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x0d0, 4, DIV_BYPASS_IRAM,
		125000000, 0
	},
	{
		LS1024A_CLK_EXTPHY2, "extphy2",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x0e0, 4, DIV_BYPASS_IRAM,
		125000000, 0
	},
	{
		LS1024A_CLK_DDR, "ddr",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x0f0, 4, DIV_BYPASS_IRAM,
		533000000, CLK_IS_CRITICAL
	},
	{
		LS1024A_CLK_PFE, "pfe",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x100, 4, DIV_BYPASS_IRAM,
		500000000, 0
	},
	{
		LS1024A_CLK_CRYPTO, "ipsec",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x110, 4, DIV_BYPASS_IRAM,
		300000000, 0
	},
	{
		LS1024A_CLK_DECT, "dect",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x120, 4, DIV_BYPASS_IRAM,
		250000000, 0
	},
	{
		LS1024A_CLK_GEM_TX, "gemtx",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x130, 4, DIV_BYPASS_IRAM,
		125000000, 0
	},
	{
		LS1024A_CLK_TDM_NTG, "tdm_ntg",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x140, 4, DIV_BYPASS_0,
		125000000, 0 /* FIXME: find the actual max rate */
	},
	{
		LS1024A_CLK_TSU_NTG, "tsu_ntg",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x150, 4, DIV_BYPASS_0,
		125000000, 0 /* FIXME: find the actual max rate */
	},
	{
		LS1024A_CLK_SATA_PMU, "sata_pmu",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x160, 4, DIV_BYPASS_IRAM,
		150000000, 0
		/* XXX: Nominal frequency is supposed to be 30MHz, according to
		 * the bootloader and AHCI driver code of the stock firmware;
		 * but even with max divider value (31), we cannot reach it.
		 * Stock firmware ends up clocking sata_pmu at 150MHz, and
		 * that does not appear to be a problem, so use that value as
		 * the maximum safe value.
		 */
	},
	{
		LS1024A_CLK_SATA_OOB, "sata_oob",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x170, 4, DIV_BYPASS_IRAM,
		125000000, 0
	},
	{
		LS1024A_CLK_SATA_OCC, "sata_occ",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x180, 4, DIV_BYPASS_0,
		125000000, 0 /* FIXME: find the actual max rate */
	},
	{
		LS1024A_CLK_PCIE_OCC, "pcie_occ",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x190, 4, DIV_BYPASS_0,
		125000000, 0 /* FIXME: find the actual max rate */
	},
	{
		LS1024A_CLK_SGMII_OCC, "sgmii_occ",
		parents_all_plls, ARRAY_SIZE(parents_all_plls),
		0x1a0, 4, DIV_BYPASS_0,
		125000000, 0 /* FIXME: find the actual max rate */
	},
};

struct ls1024a_gate_info {
	unsigned int idx;
	const char *name;
	const char *parent;
	unsigned int reg;
	unsigned int shift;
	unsigned long flags;
};

static const struct ls1024a_gate_info gates_info[] = {
	{
		LS1024A_CLK_DPI_CIE, "dpi_cie", "axi",
		AXI_CLK_CNTRL0, 5, 0
	},
	{
		LS1024A_CLK_DPI_DECOMP, "dpi_decomp", "axi",
		AXI_CLK_CNTRL0, 6, 0
	},
	{
		LS1024A_CLK_DUS, "dus", "axi",
		AXI_CLK_CNTRL1, 0, 0
	},
	{
		LS1024A_CLK_IPSEC_EAPE, "ipsec_eape", "axi",
		AXI_CLK_CNTRL1, 1, 0
	},
	{
		LS1024A_CLK_IPSEC_SPACC, "ipsec_spacc", "axi",
		AXI_CLK_CNTRL1, 2, 0
	},
	{
		LS1024A_CLK_PFE_SYS, "pfe_sys", "axi",
		AXI_CLK_CNTRL1, 3, 0
	},
	{
		LS1024A_CLK_TDM, "tdm", "axi",
		AXI_CLK_CNTRL1, 4, 0
	},
	{
		LS1024A_CLK_I2CSPI, "i2cspi", "axi",
		AXI_CLK_CNTRL1, 5, 0
	},
	{
		LS1024A_CLK_UART, "uart", "axi",
		AXI_CLK_CNTRL1, 6, 0
	},
	{
		LS1024A_CLK_RTC_TIM, "rtc_tim", "axi",
		AXI_CLK_CNTRL1, 7, 0
	},
	{
		LS1024A_CLK_PCIE0, "pcie0", "axi",
		AXI_CLK_CNTRL2, 0, 0
	},
	{
		LS1024A_CLK_PCIE1, "pcie1", "axi",
		AXI_CLK_CNTRL2, 1, 0
	},
	{
		LS1024A_CLK_SATA, "sata", "axi",
		AXI_CLK_CNTRL2, 2, 0
	},
	{
		LS1024A_CLK_USB0, "usb0", "axi",
		AXI_CLK_CNTRL2, 3, 0
	},
	{
		LS1024A_CLK_USB1, "usb1", "axi",
		AXI_CLK_CNTRL2, 4, 0
	},
};

/* === Regular PLL === */

static unsigned long ls1024a_pll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_ls1024a_pll *pll = to_clk_ls1024a_pll(hw);
	unsigned int val;
	unsigned long rate;
	unsigned long divider = 1;
	unsigned long m;
	unsigned long p;
	unsigned long s;
	bool pll_bypass;
	bool pll_reset;

	regmap_read(pll->map, pll->base + PLLx_CNTRL, &val);
	pll_bypass = !!(val & PLLx_CNTRL_BYPASS);
	pll_reset = !!(val & PLLx_CNTRL_RESET);

	/* PLL reset and no bypass -> no output */
	if (!pll_bypass && pll_reset)
		return 0UL;

	if (pll->has_divider) {
		regmap_read(pll->map, pll->base + PLLx_DIV_CNTRL, &val);
		if (!(val & PLLx_DIV_BYPASS)) {
			divider = val & PLLx_DIV_VALUE_MASK;
			/* Divider values below 2 disable clock output */
			if (divider < 2)
				return 0UL;
		}
	}

	if (pll_bypass)
		return parent_rate / divider;

	regmap_read(pll->map, pll->base + PLLx_M_LSB, &val);
	m = val & PLLx_M_LSB_MASK;
	regmap_read(pll->map, pll->base + PLLx_M_MSB, &val);
	m |= ((val & PLLx_M_MSB_MASK) << 8);
	regmap_read(pll->map, pll->base + PLLx_P, &val);
	p = val & PLLx_P_MASK;
	regmap_read(pll->map, pll->base + PLLx_S, &val);
	s = val & PLLx_S_MASK;

	divider *= p;
	divider <<= s;
	rate = m * (parent_rate / divider);
	return rate;
}

static int ls1024a_pll_is_enabled(struct clk_hw *hw)
{
	struct clk_ls1024a_pll *pll = to_clk_ls1024a_pll(hw);
	unsigned int val;
	bool pll_bypass;
	bool pll_reset;

	regmap_read(pll->map, pll->base + PLLx_CNTRL, &val);
	pll_bypass = !!(val & PLLx_CNTRL_BYPASS);
	pll_reset = !!(val & PLLx_CNTRL_RESET);

	if (pll_bypass)
		return 1;

	if (pll_reset)
		return 0;

	if (pll->has_divider) {
		regmap_read(pll->map, pll->base + PLLx_DIV_CNTRL, &val);
		if (!(val & PLLx_DIV_BYPASS) &&
		    ((val & PLLx_DIV_VALUE_MASK) < 2)) {
			return 0;
		}
	}
	return 1;
}

static const struct clk_ops ls1024a_pll_clk_ops = {
	.recalc_rate = ls1024a_pll_recalc_rate,
	.is_enabled = ls1024a_pll_is_enabled
};

static struct clk_hw *ls1024a_pll_register(const char *name,
		const char *parent_name, struct regmap *map,
		unsigned int base_offset, bool has_divider)
{
	struct clk_ls1024a_pll *pll;
	struct clk_init_data initdata;
	int res;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	initdata.name = name;
	initdata.ops = &ls1024a_pll_clk_ops;
	initdata.flags = 0;
	initdata.parent_names = &parent_name;
	initdata.num_parents = 1;
	pll->map = map;
	pll->base = base_offset;
	pll->has_divider = has_divider;
	pll->hw.init = &initdata;

	res = clk_hw_register(NULL, &pll->hw);
	if (res) {
		kfree(pll);
		return ERR_PTR(res);
	};

	return &pll->hw;
}

/* === PLL 3 === */

static unsigned long ls1024a_pll3_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_ls1024a_pll3 *pll = to_clk_ls1024a_pll3(hw);
	unsigned int val;
	unsigned long rate;
	unsigned long m;
	unsigned long p;
	unsigned long s;
	unsigned long k;
	unsigned long divider;
	bool pll_bypass;
	bool pll_reset;

	regmap_read(pll->map, PLL3_CNTRL, &val);
	pll_bypass = !!(val & PLLx_CNTRL_BYPASS);
	pll_reset = !!(val & PLLx_CNTRL_RESET);

	/* PLL reset and no bypass -> no output */
	if (!pll_bypass && pll_reset)
		return 0UL;

	if (pll_bypass)
		return parent_rate;

	regmap_read(pll->map, PLL3_M_LSB, &val);
	m = val & PLL3_M_LSB_MASK;
	regmap_read(pll->map, PLL3_M_MSB, &val);
	m |= (val & PLL3_M_MSB_MASK) << 8;
	regmap_read(pll->map, PLL3_P, &val);
	p = val & PLLx_P_MASK;
	regmap_read(pll->map, PLL3_S, &val);
	s = val & PLLx_S_MASK;
	regmap_read(pll->map, PLL3_K_LSB, &val);
	k = val & PLL3_K_LSB_MASK;
	regmap_read(pll->map, PLL3_K_MSB, &val);
	k |= (val & PLL3_K_MSB_MASK) << 8;

	divider = (p << s);
	rate = (parent_rate / 1000000) * ((m << 10) + k);
	rate += 511; /* round to the closest MHz */
	rate /= (divider << 10);
	rate *= 1000000;
	return rate;
}

static int ls1024a_pll3_is_enabled(struct clk_hw *hw)
{
	struct clk_ls1024a_pll3 *pll = to_clk_ls1024a_pll3(hw);
	unsigned int val;
	bool pll_bypass;
	bool pll_reset;

	regmap_read(pll->map, PLL3_CNTRL, &val);
	pll_bypass = !!(val & PLLx_CNTRL_BYPASS);
	pll_reset = !!(val & PLLx_CNTRL_RESET);

	if (pll_bypass)
		return 1;

	if (pll_reset)
		return 0;

	return 1;
}

static const struct clk_ops ls1024a_pll3_clk_ops = {
	.recalc_rate = ls1024a_pll3_recalc_rate,
	.is_enabled = ls1024a_pll3_is_enabled
};

static struct clk_hw *ls1024a_pll3_register(const char *name,
		const char *parent_name, struct regmap *map)
{
	struct clk_ls1024a_pll3 *pll;
	struct clk_init_data initdata;
	int res;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	initdata.name = name;
	initdata.ops = &ls1024a_pll3_clk_ops;
	initdata.flags = 0;
	initdata.parent_names = &parent_name;
	initdata.num_parents = 1;
	pll->map = map;
	pll->hw.init = &initdata;

	res = clk_hw_register(NULL, &pll->hw);
	if (res) {
		kfree(pll);
		return ERR_PTR(res);
	};

	return &pll->hw;
}

/* === Device clock generator === */

static int ls1024a_clkgen_enable(struct clk_hw *hw)
{
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);
	unsigned int val;
	unsigned long rate;

	rate = clk_hw_get_rate(hw);
	if (rate > clkgen->max_rate) {
		pr_err("Can't enable clk %s: rate=%luHz, max_rate=%luHz\n",
				clk_hw_get_name(hw), rate, clkgen->max_rate);
		return -EINVAL;
	}

	pr_debug("Enabling clk %s\n", clk_hw_get_name(hw));
	regmap_read(clkgen->map, clkgen->base, &val);
	val |= CNTRL_CLOCK_DOMAIN_ENABLE;
	regmap_write(clkgen->map, clkgen->base, val);
	return 0;
}

static void ls1024a_clkgen_disable(struct clk_hw *hw)
{
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);
	unsigned int val;
	pr_debug("Disabling clk %s\n", clk_hw_get_name(hw));
	regmap_read(clkgen->map, clkgen->base, &val);
	val &= ~CNTRL_CLOCK_DOMAIN_ENABLE;
	regmap_write(clkgen->map, clkgen->base, val);
}

static unsigned long ls1024a_clkgen_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);
	unsigned int val;
	unsigned int div_cntrl_offset;
	unsigned int divider;
	bool enabled;

	regmap_read(clkgen->map, clkgen->base, &val);
	enabled = !!(val & CNTRL_CLOCK_DOMAIN_ENABLE);
	if (!enabled)
		return 0UL;

	if (clkgen->bypass)
		return parent_rate;

	div_cntrl_offset = clkgen->base + clkgen->div_reg_offset;
	regmap_read(clkgen->map, div_cntrl_offset, &val);
	divider = val & DIV_CNTRL_DIVIDER_MASK;
	if (divider < 2)
		return 0UL;

	return parent_rate / divider;
}

static long ls1024a_clkgen_round_rate(struct clk_hw *hw,
		unsigned long rate, unsigned long *parent_rate)
{
	unsigned long div;
	unsigned long newrate;
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);

	if (rate > clkgen->max_rate) {
		return -EINVAL;
	}
	if (rate > *parent_rate)
		return *parent_rate;

	div = DIV_ROUND_CLOSEST(*parent_rate, rate);
	if (div == 0)
		div = 1;
	if (div > DIV_CNTRL_DIVIDER_MAX)
		div = DIV_CNTRL_DIVIDER_MAX;
	newrate = *parent_rate / div;
	/* Ensure the rate do not become higher than the maximum acceptable
	 * rate after rounding to the closest divider.
	 */
	if (newrate > clkgen->max_rate) {
		div++;
		if (div > DIV_CNTRL_DIVIDER_MAX) {
			pr_err("Clock %s cannot be slow enough: max=%lu best=%lu\n",
					clk_hw_get_name(hw), clkgen->max_rate,
					newrate);
			return -EINVAL; /* Clock cannot be slow enough */
		}
		newrate = *parent_rate / div;
	}
	return newrate;
}

static int ls1024a_clkgen_set_rate(struct clk_hw *hw,
		unsigned long rate, unsigned long parent_rate)
{
	unsigned long div;
	unsigned int val;
	bool bypass = false;
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);

	if (rate > clkgen->max_rate) {
		pr_err("Can't set clk %s rate: rate=%luHz, max_rate=%luHz\n",
				clk_hw_get_name(hw), rate, clkgen->max_rate);
		return -EINVAL;
	}
	if (rate > parent_rate)
		return -EINVAL;

	div = DIV_ROUND_CLOSEST(parent_rate, rate);
	if (div == 0 || div > DIV_CNTRL_DIVIDER_MAX) {
		pr_err("Invalid div: %lu\n", div);
		return -EINVAL;
	}

	if (div == 1) {
		div = 2;
		bypass = true;
	}

	pr_debug("Setting clk %s rate to %lu\n",clk_hw_get_name(hw), rate);
	regmap_read(clkgen->map, clkgen->base + clkgen->div_reg_offset, &val);
	if (bypass) {
		val |= DIV_CNTRL_BYPASS;
	} else {
		val &= ~(DIV_CNTRL_BYPASS | DIV_CNTRL_DIVIDER_MASK);
		val |= div;
	}
	regmap_write(clkgen->map, clkgen->base + clkgen->div_reg_offset, val);
	/* Save the bypass bit since it cannot be read back from HW */
	clkgen->bypass = bypass;
	return 0;
}

static int ls1024a_clkgen_is_enabled(struct clk_hw *hw)
{
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);
	unsigned int val;
	unsigned int div_cntrl_offset;
	unsigned int divider;
	bool enabled;

	regmap_read(clkgen->map, clkgen->base, &val);
	enabled = !!(val & CNTRL_CLOCK_DOMAIN_ENABLE);
	if (!enabled)
		return 0;

	if (clkgen->bypass)
		return 1;

	div_cntrl_offset = clkgen->base + clkgen->div_reg_offset;
	regmap_read(clkgen->map, div_cntrl_offset, &val);
	divider = val & DIV_CNTRL_DIVIDER_MASK;
	BUG_ON(divider < 2);
	return 1;
}

static u8 ls1024a_clkgen_get_parent(struct clk_hw *hw)
{
	struct clk_ls1024a_clkgen *clkgen = to_clk_ls1024a_clkgen(hw);
	unsigned int val;

	regmap_read(clkgen->map, clkgen->base, &val);
	return (val & CNTRL_CLOCK_SOURCE_MASK) >> CNTRL_CLOCK_SOURCE_SHIFT;
}

static const struct clk_ops ls1024a_clkgen_ops = {
	.enable = ls1024a_clkgen_enable,
	.disable = ls1024a_clkgen_disable,
	.recalc_rate = ls1024a_clkgen_recalc_rate,
	.round_rate = ls1024a_clkgen_round_rate,
	.is_enabled = ls1024a_clkgen_is_enabled,
	.get_parent = ls1024a_clkgen_get_parent,
	.set_rate = ls1024a_clkgen_set_rate,
};

static bool clkgen_is_sane(struct clk_ls1024a_clkgen *clkgen)
{
	unsigned int val;
	unsigned int source;
	unsigned int div_reg_offset = clkgen->base + clkgen->div_reg_offset;
	unsigned int divider;
	unsigned int max_sources = clkgen->hw.init->num_parents;
	BUG_ON(regmap_read(clkgen->map, clkgen->base, &val));
	source = (val & CNTRL_CLOCK_SOURCE_MASK) >> CNTRL_CLOCK_SOURCE_SHIFT;
	if (source >= max_sources) {
		pr_err("invalid clock source %u selected (max %u)",
				source, max_sources);
		return false;
	}
	BUG_ON(regmap_read(clkgen->map, div_reg_offset, &val));
	divider = val & DIV_CNTRL_DIVIDER_MASK;
	if (!clkgen->bypass && divider < 2) {
		pr_err("invalid divider value: %u < 2", divider);
		return false;
	}
	return true;
}

static struct clk_hw *ls1024a_clkgen_register(
		const struct ls1024a_clkgen_info *info,
		struct regmap *map, bool bypass)
{
	struct clk_ls1024a_clkgen *clkgen;
	struct clk_init_data initdata;
	int res;

	if (!info || !map)
		return ERR_PTR(-EINVAL);

	clkgen = kzalloc(sizeof(*clkgen), GFP_KERNEL);
	if (!clkgen)
		return ERR_PTR(-ENOMEM);

	initdata.name = info->name;
	initdata.ops = &ls1024a_clkgen_ops;
	initdata.flags = info->flags;
	initdata.parent_names = info->parents;
	initdata.num_parents = info->num_parents;
	clkgen->map = map;
	clkgen->base = info->base;
	clkgen->div_reg_offset = info->div_reg_offset;
	clkgen->bypass = bypass;
	clkgen->max_rate = info->max_rate;
	clkgen->hw.init = &initdata;

	if (WARN_ON(!clkgen_is_sane(clkgen))) {
		kfree(clkgen);
		return ERR_PTR(-EINVAL);
	};

	res = clk_hw_register(NULL, &clkgen->hw);
	if (res) {
		kfree(clkgen);
		return ERR_PTR(res);
	};
	return &clkgen->hw;
}

/* === Rest of the driver === */

static bool is_clkcore_sane(struct regmap *map)
{
	int res;
	unsigned int val;

	/* The dividers global bypass should NEVER be active once the PLLs are
	 * running. The bootloader should have done its job and cleared the
	 * GLOBAL_BYPASS bit.
	 */
	res = regmap_read(map, GNRL_CLK_CNTRL_1, &val);
	if (res)
		goto regmap_fail;
	if (val & GLOBAL_BYPASS) {
		pr_crit("clock dividers global bypass is active! "
				"This configuration isn't supported");
		return false;
	}

	return true;

regmap_fail:
	pr_crit("failed to read from regmap");
	return false;
}

static int __init get_refclk(struct device_node *np, unsigned long *rate)
{
	struct regmap *gpio_regs;
	unsigned int sysconfig;
	unsigned int sys_pll_ref;
	int res;
	if (!np || !rate)
		return -EINVAL;

	gpio_regs = syscon_regmap_lookup_by_compatible(
			"fsl,ls1024a-gpio");
	if (IS_ERR(gpio_regs)) {
		pr_err("failed to get gpio syscon regmap");
		return PTR_ERR(gpio_regs);
	}

	res = regmap_read(gpio_regs, GPIO_SYSTEM_CONFIG, &sysconfig);
	if (res) {
		pr_err("failed to read system config using regmap");
		return res;
	}

	sys_pll_ref = (sysconfig & SYS_PLL_REF_CLK_MASK) >> SYS_PLL_REF_CLK_SHIFT;
	switch(sys_pll_ref) {
	case SYS_PLL_REF_USB_XTAL:
		*rate = (sysconfig & USB_OSC_FREQ) ? 24000000UL : 48000000UL;
		break;
	case SYS_PLL_REF_SERDES_XTAL:
		*rate = (sysconfig & SERDES_OSC_FREQ) ? 24000000UL : 48000000UL;
		break;
	default:
		pr_err("SerDes clkout is not supported as PLLs ref");
		return -EINVAL;
	}
	return 0;
}

static void __iomem *map_bypass_bug_iram(struct device_node *np)
{
	struct device_node *bug_node;
	void __iomem *base;

	bug_node = of_parse_phandle(np,
			"fsl,ls1024a-bypass-workaround", 0);
	if (!bug_node)
		return NULL;

	base = of_iomap(bug_node, 0);
	of_node_put(bug_node);
	return base;
}

static inline bool bypass_bug_get_bypass(void __iomem *base,
		const struct ls1024a_clkgen_info *info)
{
	unsigned int offset = info->base + info->div_reg_offset;
	uint32_t val = readl(base + (offset - 0x4c));
	return !!(val & DIV_CNTRL_BYPASS);
}

static DEFINE_SPINLOCK(ls1024a_clk_lock);

#ifdef DEBUG
static inline void debug_clk(struct clk_hw *hw)
{
	pr_debug("clk: %s, parent: %s, rate: %lu, enabled: %i",
			clk_hw_get_name(hw),
			clk_hw_get_name(clk_hw_get_parent(hw)),
			clk_hw_get_rate(hw), clk_hw_is_enabled(hw) ? 1 : 0);
}
#else
static inline void debug_clk(struct clk_hw *hw)
{
}
#endif

static void register_plls(struct regmap *map)
{
	struct clk_hw *hw;

	hw = ls1024a_pll_register("pll0", "refclk", map, PLL0_BASE, true);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL0] = hw;

	hw = ls1024a_pll_register("pll1", "refclk", map, PLL1_BASE, true);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL1] = hw;

	hw = ls1024a_pll_register("pll2", "refclk", map, PLL2_BASE, false);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL2] = hw;

	hw = ls1024a_pll3_register("pll3", "refclk", map);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL3] = hw;
}

static void register_pll_muxes(void __iomem *base)
{
	const char* mux0_parents[] = { "pll0", "int_refclk" };
	const char* mux1_parents[] = { "pll1", "int_refclk" };
	const char* mux2_parents[] = { "pll2", "int_refclk" };
	const char* mux3_parents[] = { "pll3", "int_refclk" };
	struct clk_hw *hw;

	hw = clk_hw_register_mux(NULL, "pll0_mux",
			mux0_parents, 2, ARRAY_SIZE(mux0_parents),
			base + PLLS_GLOBAL_CNTRL, 0, 1, 0, &ls1024a_clk_lock);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL0_EXT_BYPASS_MUX] = hw;

	hw = clk_hw_register_mux(NULL, "pll1_mux",
			mux1_parents, 2, ARRAY_SIZE(mux1_parents),
			base + PLLS_GLOBAL_CNTRL, 1, 1, 0, &ls1024a_clk_lock);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL1_EXT_BYPASS_MUX] = hw;

	hw = clk_hw_register_mux(NULL, "pll2_mux",
			mux2_parents, 2, ARRAY_SIZE(mux2_parents),
			base + PLLS_GLOBAL_CNTRL, 2, 1, 0, &ls1024a_clk_lock);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL2_EXT_BYPASS_MUX] = hw;

	hw = clk_hw_register_mux(NULL, "pll3_mux",
			mux3_parents, 2, ARRAY_SIZE(mux3_parents),
			base + PLLS_GLOBAL_CNTRL, 3, 1, 0, &ls1024a_clk_lock);
	debug_clk(hw);
	ls1024a_clk_data->hws[LS1024A_CLK_PLL3_EXT_BYPASS_MUX] = hw;
}

static void register_clkgens(struct device_node *np, struct regmap *map)
{
	unsigned int i;
	void __iomem *bug_base;

	bug_base = map_bypass_bug_iram(np);
	if (!bug_base) {
		pr_crit("failed to map bypass bug workaround region");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(clkgens_info); i++) {
		struct clk_hw *hw;
		const struct ls1024a_clkgen_info *info = &clkgens_info[i];
		bool bypass;

		if (info->bypass_src == DIV_BYPASS_IRAM) {
			bypass = bypass_bug_get_bypass(bug_base, info);
		} else {
			bypass = info->bypass_src == DIV_BYPASS_1;
		}
		hw = ls1024a_clkgen_register(info, map, bypass);
		if (IS_ERR(hw)) {
			pr_err("failed to register clkgen %s (%li)",
					info->name, PTR_ERR(hw));
		}
		ls1024a_clk_data->hws[info->idx] = hw;
		debug_clk(hw);
	}
	iounmap(bug_base);
}

static void register_gates(void __iomem *base)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gates_info); i ++) {
		struct clk_hw *hw;
		const struct ls1024a_gate_info *info = &gates_info[i];
		hw = clk_hw_register_gate(NULL, info->name, info->parent,
				info->flags, base + info->reg, info->shift,
				0, &ls1024a_clk_lock);
		if (IS_ERR(hw))
			pr_crit("failed to register gate %s", info->name);

		ls1024a_clk_data->hws[info->idx] = hw;
		debug_clk(hw);
	}
}

static void __init ls1024a_cc_init(struct device_node *np)
{
	int res;
	unsigned long refclk_rate = 0;
	struct regmap *map;
	struct clk_hw *hw;
	void __iomem *clkcore_base;

	ls1024a_clk_data = kzalloc(sizeof(*ls1024a_clk_data) +
			sizeof(*ls1024a_clk_data->hws) * LS1024A_NUM_CLKS,
			GFP_KERNEL);
	if (!ls1024a_clk_data)
		return;

	map = syscon_node_to_regmap(np);
	if (IS_ERR(map)) {
		pr_err("no syscon regmap\n");
		return;
	}

	clkcore_base = of_io_request_and_map(np, 0, "clkcore");
	if (!clkcore_base) {
		pr_err("failed to map clkcore registers");
		return;
	}

	if (!is_clkcore_sane(map))
		return;

	res = get_refclk(np, &refclk_rate);
	if (res) {
		pr_crit("could not get refclk rate: %i", res);
		return;
	}
	pr_debug("ref clock: %lu", refclk_rate / 1000000);

	hw = clk_hw_register_fixed_rate(NULL, "refclk", NULL, 0, refclk_rate);
	ls1024a_clk_data->hws[LS1024A_CLK_REF] = hw;

	hw = clk_hw_register_fixed_factor(NULL, "int_refclk", "refclk", 0, 1, 1);
	ls1024a_clk_data->hws[LS1024A_CLK_INT_REF] = hw;

	register_plls(map);
	register_pll_muxes(clkcore_base);
	register_clkgens(np, map);

	hw = clk_hw_register_fixed_factor(NULL, "a9dp_mpu", "a9dp", 0, 1, 4);
	ls1024a_clk_data->hws[LS1024A_CLK_A9DP_MPU] = hw;
	debug_clk(hw);

	register_gates(clkcore_base);

	/* Register provider to use clocks in device tree */
	ls1024a_clk_data->num = LS1024A_NUM_CLKS;
	of_clk_add_hw_provider(np, of_clk_hw_onecell_get, ls1024a_clk_data);
}
CLK_OF_DECLARE_DRIVER(ls1024a_cc, "fsl,ls1024a-clkcore", ls1024a_cc_init);
