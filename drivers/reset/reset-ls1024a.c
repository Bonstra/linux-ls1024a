// SPDX-License-Identifier: GPL-2.0
/*
 * Mindspeed Comcerto (aka. QorIQ LS1024A) reset driver
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 *
 * Copied from reset-berlin.c
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <dt-bindings/reset/ls1024a.h>

/* Let the reset be effective (datasheet specifies a minimum of 100us for a
 * global external reset. Double the duration just to be safe.
 * Here, we assume a block reset is faster than a global external reset, but
 * that could be wrong.
 */
#define LS1024A_RESET_DELAY_US 200

#define to_ls1024a_reset_priv(p)		\
	container_of((p), struct ls1024a_reset_priv, rcdev)

struct ls1024a_reset_priv {
	struct regmap			*regmap;
	struct reset_controller_dev	rcdev;
};

static bool is_self_deasserting(unsigned long id)
{
	switch(id) {
	case LS1024A_DEVICE_PWR_ON_SOFT_RST:
	case LS1024A_DEVICE_GLB_SCLR_RST:
	case LS1024A_DEVICE_FUNC_SCLR_RST:
	case LS1024A_DEVICE_CLKRST_SCLR_RST:
		return true;
	}
	return false;
}

static int ls1024a_reset_reset(struct reset_controller_dev *rcdev,
                                unsigned long id)
{
	struct ls1024a_reset_priv *priv = to_ls1024a_reset_priv(rcdev);
	int offset = (id & ~0x1f) >> 3;
	int mask = BIT(id & 0x1f);
	if (id > LS1024A_MAX_RST)
		return -EINVAL;

	regmap_write_bits(priv->regmap, offset, mask, mask);
	udelay(LS1024A_RESET_DELAY_US);

	if (!is_self_deasserting(id))
		regmap_write_bits(priv->regmap, offset, mask, 0);

	udelay(LS1024A_RESET_DELAY_US);

	return 0;
}

static int ls1024a_reset_assert(struct reset_controller_dev *rcdev,
                                 unsigned long id)
{
	struct ls1024a_reset_priv *priv = to_ls1024a_reset_priv(rcdev);
	int offset = (id & ~0x1f) >> 3;
	int mask = BIT(id & 0x1f);
	if (id > LS1024A_MAX_RST)
		return -EINVAL;

	if (is_self_deasserting(id))
		return -EINVAL;

	regmap_write_bits(priv->regmap, offset, mask, mask);
	udelay(LS1024A_RESET_DELAY_US);
	return 0;
}

static int ls1024a_reset_deassert(struct reset_controller_dev *rcdev,
                                   unsigned long id)
{
	struct ls1024a_reset_priv *priv = to_ls1024a_reset_priv(rcdev);
	int offset = (id & ~0x1f) >> 3;
	int mask = BIT(id & 0x1f);
	if (id > LS1024A_MAX_RST)
		return -EINVAL;

	if (is_self_deasserting(id))
		return -EINVAL;

	regmap_write_bits(priv->regmap, offset, mask, 0);
	udelay(LS1024A_RESET_DELAY_US);
	return 0;
}

static int ls1024a_reset_status(struct reset_controller_dev *rcdev,
                                 unsigned long id)
{
	struct ls1024a_reset_priv *priv = to_ls1024a_reset_priv(rcdev);
	unsigned int offset = id >> 5;
	unsigned int mask = BIT(id & 0x1f);
	unsigned int val = 0;
	if (id > LS1024A_MAX_RST)
		return -EINVAL;

	regmap_read(priv->regmap, offset, &val);
	return !!(val & mask);
}

static const struct reset_control_ops ls1024a_reset_ops = {
	.reset		= ls1024a_reset_reset,
	.assert		= ls1024a_reset_assert,
	.deassert	= ls1024a_reset_deassert,
	.status		= ls1024a_reset_status
};

static int ls1024a_reset_probe(struct platform_device *pdev)
{
	struct ls1024a_reset_priv *priv;
	struct device_node *parent_np;

	if (!pdev->dev.of_node)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	parent_np = of_get_parent(pdev->dev.of_node);
	priv->regmap = syscon_node_to_regmap(parent_np);
	of_node_put(parent_np);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->rcdev.of_node = pdev->dev.of_node;
	priv->rcdev.nr_resets = LS1024A_MAX_RST + 1;
	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.ops = &ls1024a_reset_ops;

	return devm_reset_controller_register(&pdev->dev, &priv->rcdev);
}

static const struct of_device_id ls1024a_reset_dt_match[] = {
	{ .compatible = "fsl,ls1024a-reset" },
	{ },
};

static struct platform_driver ls1024a_reset_driver = {
	.probe	= ls1024a_reset_probe,
	.driver	= {
		.name = "ls1024a-reset",
		.of_match_table = ls1024a_reset_dt_match,
	},
};
builtin_platform_driver(ls1024a_reset_driver);
