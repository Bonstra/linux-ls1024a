// SPDX-License-Identifier: GPL-2.0

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>


struct ls1024a_priv {
	struct regmap *reg;
	struct device *dev;
	struct reset_control *phy_rst;
	struct reset_control *utmi_rst;
	struct clk *intclk;
	bool extclk;
};

static int ls1024a_assert_reset(struct ls1024a_priv *priv)
{
	int res;

	/* Assert PHY and UTMI+ lines */
	res = reset_control_assert(priv->utmi_rst);
	if (res) {
		dev_err(priv->dev, "Failed to assert UTMI reset: %d\n", res);
		return res;
	}
	res = reset_control_assert(priv->phy_rst);
	if (res) {
		dev_err(priv->dev, "Failed to assert PHY reset: %d\n", res);
		return res;
	}
	return 0;
}

static int ls1024a_phy_power_on(struct phy *phy)
{
	struct ls1024a_priv *priv = phy_get_drvdata(phy);
	unsigned int clkcfg_val;
	int res;

	if (priv->extclk) {
		clkcfg_val = 0x4209927a;
	} else {
		unsigned long rate;
		WARN_ON(clk_enable(priv->intclk));
		rate = clk_get_rate(priv->intclk);
		if (rate == 24000000) {
			clkcfg_val = 0x420E82A8;
		} else if (rate == 48000000) {
			clkcfg_val = 0x420E82A9;
		} else {
			dev_err(&phy->dev, "unsupported intclk rate %lu\n",
			        rate);
			clk_disable(priv->intclk);
			return -EINVAL;
		}
	}
	regmap_write(priv->reg, 0x10, 0x00e00080);
	regmap_write(priv->reg, 0x20, clkcfg_val);
	regmap_write(priv->reg, 0x24, 0x69c34f53);
	regmap_write(priv->reg, 0x28, 0x0005d815);
	regmap_write(priv->reg, 0x2c, 0x00000801);

	/* Bring UTMI+ and PHY out of reset */
	res = reset_control_deassert(priv->utmi_rst);
	if (res) {
		dev_err(priv->dev, "Failed to deassert UTMI reset: %d\n", res);
		return res;
	}
	res = reset_control_deassert(priv->phy_rst);
	if (res) {
		dev_err(priv->dev, "Failed to deassert PHY reset: %d\n", res);
		return res;
	}
	return 0;
}

static int ls1024a_phy_power_off(struct phy *phy)
{
	struct ls1024a_priv *priv = phy_get_drvdata(phy);
	if (priv->intclk)
		clk_disable(priv->intclk);
	return ls1024a_assert_reset(priv);
}

static int ls1024a_phy_init(struct phy *phy)
{
	struct ls1024a_priv *priv = phy_get_drvdata(phy);
	return ls1024a_assert_reset(priv);
}

static const struct phy_ops ls1024a_phy_ops = {
	.init		= ls1024a_phy_init,
	.power_on	= ls1024a_phy_power_on,
	.power_off	= ls1024a_phy_power_off,
	.owner		= THIS_MODULE,
};

static int ls1024a_phy_probe(struct platform_device *pdev)
{
	const struct regmap_config base_regmap_cfg = {
		.reg_bits = 16,
		.reg_stride = 4,
		.val_bits = 32,
		.fast_io = true
	};
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct phy *phy;
	struct ls1024a_priv *priv;
	struct resource *res;
	void __iomem *base;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	if (of_find_property(dev->of_node, "fsl,use-external-clock", NULL)) {
		priv->extclk = true;
		priv->intclk = NULL;
	} else {
		priv->extclk = false;
		priv->intclk = devm_clk_get(dev, NULL);
		if (IS_ERR(priv->intclk)) {
			dev_err(dev, "Failed to get internal clock: %ld\n",
			        PTR_ERR(priv->intclk));
			return PTR_ERR(priv->intclk);
		}
	}

	priv->phy_rst = devm_reset_control_get_exclusive(dev, "phy");
	if (IS_ERR(priv->phy_rst)) {
		dev_err(dev, "cannot get \"phy\" reset control: %ld\n",
		        PTR_ERR(priv->phy_rst));
		return PTR_ERR(priv->phy_rst);
	}

	priv->utmi_rst = devm_reset_control_get_exclusive(dev, "utmi");
	if (IS_ERR(priv->utmi_rst)) {
		dev_err(dev, "cannot get \"utmi\" reset control: %ld\n",
		        PTR_ERR(priv->utmi_rst));
		return PTR_ERR(priv->utmi_rst);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base)) {
		dev_err(dev, "failed to remap PHY registers: %ld\n",
		        PTR_ERR(base));
		return PTR_ERR(base);
	}
	priv->reg = devm_regmap_init_mmio(dev, base, &base_regmap_cfg);
	if (IS_ERR(priv->reg)) {
		dev_err(dev, "failed to create PHY regmap: %ld\n",
		        PTR_ERR(priv->reg));
		return PTR_ERR(priv->reg);
	}

	phy = devm_phy_create(dev, NULL, &ls1024a_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, priv);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id ls1024a_phy_of_match[] = {
	{.compatible = "fsl,ls1024a-usb3-phy",},
	{ },
};
MODULE_DEVICE_TABLE(of, ls1024a_phy_of_match);

static struct platform_driver ls1024a_phy_driver = {
	.probe	= ls1024a_phy_probe,
	.driver = {
		.name	= "ls1024a-usb3-phy",
		.of_match_table	= ls1024a_phy_of_match,
	}
};
module_platform_driver(ls1024a_phy_driver);

MODULE_DESCRIPTION("LS1024A USB3 PHY driver");
MODULE_AUTHOR("Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>");
MODULE_LICENSE("GPL v2");

