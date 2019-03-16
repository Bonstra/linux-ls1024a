// SPDX-License-Identifier: GPL-2.0

#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <dt-bindings/phy/phy-ls1024a-serdes.h>

#include "phy-ls1024a-serdes.h"

/* Registers in GPIO region */
#define GPIO_SYSTEM_CONFIG	0x1c
#define USB_OSC_FREQ		BIT(5)
#define SERDES_OSC_FREQ		BIT(7)
#define SERDES1_CNF		BIT(11)
#define SERDES2_CNF		BIT(12)

#define GPIO_DEVICE_ID		0x50
#define C2K_REVISION_SHIFT	24

/* Registers in PHY stat region */
#define SDx_PHY_STS(n)		(0x2c + 0x10 * (n))
#define SD_STS_CMU_OK		BIT(14)
#define SD_STS_LANE_OK		BIT(12)

#define SDx_PHY_CTRL2(n)	(0x34 + 0x10 * (n))
#define SD_CTRL_CMU_RST		BIT(16)
#define SD_CTRL_CMU_PD		BIT(7)
#define SD_CTRL_LANE_RST	BIT(6)
#define SD_CTRL_POWER_MODE	(BIT(3) | BIT(2))
#define SDx_PHY_CTRL3(n)	(0x38 + 0x10 * (n))

#define CK_SOC_DIV_I_DEFAULT	0x33f
#define CK_SOC_DIV_I_PCIE	0xff3c

#define CMU_OK_TIMEOUT_US	2000000
#define LANE_OK_TIMEOUT_US	200000

/* Registers in the SerDes config region */
#define SDx_CFG_REG(n, o)	(((n) * 0x4000) + (o))
#define SDx_CFG_CMU(n, o)	SDx_CFG_REG(n, o)
#define SDx_CFG_LANE0(n, o)	SDx_CFG_REG(n, (o) + 0x800)
#define SDx_CFG_LANES(n, o)	SDx_CFG_REG(n, (o) + 0x2800)

#define SD_CMU_CTL		0x000
#define CMU_CTL_EN		BIT(0)

#define SD_CMU_CLKDIV		0x184
#define CMU_CLKDIV_REV0_VAL	0x2e
#define CMU_CLKDIV_REV1_VAL	0x06

#define SD_LANE0_TX_CFG		0x01c
#define LANE0_TX_LEV_SHIFT	2
#define LANE0_TX_LEV_MASK	0x3c

#define SD_LANES_CTL		0x000
#define LANES_CTL_EN		(BIT(1) | BIT(0))
#define LANES_CTL_PCIE_VAL	0xc3
#define LANES_CTL_OTHER_VAL	0x03

struct ls1024a_serdes_cfg {
	/* DT-defined config */
	bool				sata_extclk;
	bool				pcie_extclk;
	unsigned int			pcie_polarity;
	int				sata_txlev;
	/* Defined by GPIO_SYSTEM_CONFIG */
	enum phy_mode			mode;
};

struct ls1024a_serdes_instance {
	unsigned int			idx;
	struct ls1024a_serdes_cfg	cfg;
	struct reset_control		*reset;
	struct phy			*phy;
	struct device			*dev;
	bool				initialized;
	bool				running;
};

struct ls1024a_serdes_phys {
	struct regmap			*regs;
	struct regmap			*statregs;
	struct regmap			*gpioregs;
	bool				intclk_24mhz;
	bool				soc_is_rev1;
	struct ls1024a_serdes_instance	inst[3];
};

static int ls1024a_serdes_phy_init(struct phy *phy)
{
	int ret;
	struct ls1024a_serdes_instance *inst = phy_get_drvdata(phy);
	ret = reset_control_assert(inst->reset);
	if (ret) {
		dev_err(&phy->dev, "reset failed (%d)\n", ret);
		return ret;
	}
	udelay(200);
	return 0;
}

static int ls1024a_serdes_phy_exit(struct phy *phy)
{
	int ret;
	struct ls1024a_serdes_instance *inst = phy_get_drvdata(phy);
	ret = reset_control_assert(inst->reset);
	if (ret) {
		dev_err(&phy->dev, "reset failed (%d)\n", ret);
		return ret;
	}
	udelay(200);
	return 0;
}

static const struct ls1024a_serdes_phy_regval *ls1024a_get_regvals(
		struct ls1024a_serdes_instance *inst, size_t *num_regvals)
{
	struct ls1024a_serdes_phys *phys = dev_get_drvdata(inst->dev);
	const struct ls1024a_serdes_phy_regval *regvals = NULL;
	if (!num_regvals)
		return ERR_PTR(-EINVAL);

	switch(inst->cfg.mode) {
	case PHY_MODE_PCIE:
		if (inst->cfg.pcie_extclk) {
			regvals = ls1024a_serdes_pcie_100_cfg;
			*num_regvals = ARRAY_SIZE(ls1024a_serdes_pcie_100_cfg);
		} else {
			if (phys->intclk_24mhz) {
				regvals = ls1024a_serdes_pcie_24_cfg;
				*num_regvals =
					ARRAY_SIZE(ls1024a_serdes_pcie_24_cfg);
			} else {
				regvals = ls1024a_serdes_pcie_48_cfg;
				*num_regvals =
					ARRAY_SIZE(ls1024a_serdes_pcie_48_cfg);
			}
		}
		break;
	case PHY_MODE_SATA:
		if (inst->cfg.pcie_extclk) {
			regvals = ls1024a_serdes_sata_60_cfg;
			*num_regvals = ARRAY_SIZE(ls1024a_serdes_sata_60_cfg);
		} else {
			if (phys->intclk_24mhz) {
				regvals = ls1024a_serdes_sata_24_cfg;
				*num_regvals =
					ARRAY_SIZE(ls1024a_serdes_sata_24_cfg);
			} else {
				regvals = ls1024a_serdes_sata_48_cfg;
				*num_regvals =
					ARRAY_SIZE(ls1024a_serdes_sata_48_cfg);
			}
		}
		break;
	case PHY_MODE_ETHERNET:
		dev_err(&inst->phy->dev, "SGMII mode is not implemented\n");
		return ERR_PTR(-ENOSYS);
	default:
		dev_err(&inst->phy->dev, "invalid phy mode\n");
		return ERR_PTR(-EINVAL);
	};
	return regvals;
}

static int ls1024a_wait_cmu_ready(struct ls1024a_serdes_instance *inst)
{
	struct ls1024a_serdes_phys *phys = dev_get_drvdata(inst->dev);
	unsigned int val;
	int res = regmap_read_poll_timeout(phys->statregs,
			SDx_PHY_STS(inst->idx), val, (val & SD_STS_CMU_OK),
			10000, CMU_OK_TIMEOUT_US);
	if (res == -ETIMEDOUT) {
		dev_err(&inst->phy->dev, "CMU didn't come up\n");
	}
	return res;
}

static int ls1024a_wait_lane_ready(struct ls1024a_serdes_instance *inst)
{
	struct ls1024a_serdes_phys *phys = dev_get_drvdata(inst->dev);
	unsigned int val;
	int res = regmap_read_poll_timeout(phys->statregs,
			SDx_PHY_STS(inst->idx), val, (val & SD_STS_LANE_OK),
			10000, LANE_OK_TIMEOUT_US);
	if (res == -ETIMEDOUT) {
		dev_err(&inst->phy->dev, "LANE didn't come up\n");
	}
	return res;
}

static void ls1024a_apply_polarity(struct ls1024a_serdes_instance *inst,
		unsigned int polarity)
{
	struct ls1024a_serdes_phys *phys = dev_get_drvdata(inst->dev);
	unsigned int pol0;
	unsigned int pol1;

	if (WARN(polarity > 3, "Invalid polarity value: %u\n", polarity))
		return;
	switch(polarity) {
	case 0:
		pol0 = 0x0;
		pol1 = 0x0;
		break;
	case 1:
		pol0 = 0x8;
		pol1 = 0x2;
		break;
	case 2:
		pol0 = 0x0;
		pol1 = 0x2;
		break;
	case 3:
		pol0 = 0x8;
		pol1 = 0x0;
		break;
	}
	dev_dbg(inst->dev, "Applying polarity %u\n", polarity);
	regmap_write(phys->regs, SDx_CFG_LANE0(inst->idx, 0x0), pol0);
	regmap_write(phys->regs, SDx_CFG_LANE0(inst->idx, 0x8), pol1);
}

static int ls1024a_serdes_phy_power_on(struct phy *phy)
{
	int ret;
	struct ls1024a_serdes_instance *inst = phy_get_drvdata(phy);
	const struct ls1024a_serdes_phy_regval *regvals;
	struct ls1024a_serdes_phys *phys = dev_get_drvdata(inst->dev);
	size_t num_regvals;
	size_t i;

	dev_dbg(&phy->dev, "PHY %u power on\n", inst->idx);
	ret = reset_control_deassert(inst->reset);
	if (ret) {
		dev_err(&phy->dev, "reset deassert failed (%d)\n", ret);
		return ret;
	}
	/* Init registers */
	regvals = ls1024a_get_regvals(inst, &num_regvals);
	if (IS_ERR(regvals)) {
		return PTR_ERR(regvals);
	}
	for (i = 0; i < num_regvals; i++) {
		struct ls1024a_serdes_phy_regval rv = regvals[i];
		regmap_write(phys->regs, SDx_CFG_REG(inst->idx, rv.offset),
				rv.value);
	}

	switch(inst->cfg.mode) {
	case PHY_MODE_SATA:
		/* Override default lane 0 Tx level for SATA if requested */
		if (inst->cfg.sata_txlev >= 0) {
			unsigned int reg =
				SDx_CFG_LANE0(inst->idx, SD_LANE0_TX_CFG);
			unsigned int val =
				inst->cfg.sata_txlev << LANE0_TX_LEV_SHIFT;
			regmap_write_bits(phys->regs, reg,
					LANE0_TX_LEV_MASK, val);
		}
		break;
	case PHY_MODE_PCIE:
		if (phys->soc_is_rev1) {
			/* Adjust divider value for silicon revision 1 */
			regmap_write(phys->regs,
					SDx_CFG_CMU(inst->idx, SD_CMU_CLKDIV),
					CMU_CLKDIV_REV1_VAL);
		}
		ls1024a_apply_polarity(inst, inst->cfg.pcie_polarity);
		break;
	default:
		break;
	}
	/* Setup divider */
	if (inst->cfg.mode == PHY_MODE_PCIE) {
		regmap_write(phys->statregs, SDx_PHY_CTRL3(inst->idx),
				CK_SOC_DIV_I_PCIE);
	} else {
		regmap_write(phys->statregs, SDx_PHY_CTRL3(inst->idx),
				CK_SOC_DIV_I_DEFAULT);
	}
	regmap_write_bits(phys->statregs, SDx_PHY_CTRL2(inst->idx), 0x3, 0x0);

	/* Enable CMU */
	regmap_write_bits(phys->regs, SDx_CFG_CMU(inst->idx, SD_CMU_CTL),
			CMU_CTL_EN, CMU_CTL_EN);
	/* Release CMU reset and wait for it to come up. */
	regmap_write_bits(phys->statregs, SDx_PHY_CTRL2(inst->idx),
			SD_CTRL_CMU_RST, SD_CTRL_CMU_RST);
	ret = ls1024a_wait_cmu_ready(inst);
	if (ret)
		return ret;
	/* Enable all lanes */
	regmap_write_bits(phys->regs, SDx_CFG_LANES(inst->idx, SD_LANES_CTL),
			LANES_CTL_EN, LANES_CTL_EN);
	/* Release LANE reset */
	regmap_write_bits(phys->statregs, SDx_PHY_CTRL2(inst->idx),
			SD_CTRL_LANE_RST, SD_CTRL_LANE_RST);
	/* For PCIe, we do not need the lane to be ready right now. */
	if (inst->cfg.mode == PHY_MODE_PCIE)
		return 0;
	/* Wait for LANE OK to come up. */
	return ls1024a_wait_lane_ready(inst);
}

static int ls1024a_serdes_phy_power_off(struct phy *phy)
{
	struct ls1024a_serdes_instance *inst = phy_get_drvdata(phy);
	struct ls1024a_serdes_phys *phys = dev_get_drvdata(inst->dev);
	unsigned int resets = SD_CTRL_CMU_RST | SD_CTRL_LANE_RST;
	if (!phy->init_count)
		return -EBADF;
	/* Assert CMU and LANE resets. */
	dev_dbg(&phy->dev, "PHY %u power off\n", inst->idx);
	regmap_write_bits(phys->statregs, SDx_PHY_CTRL2(inst->idx),
			resets, 0);
	return 0;
}

static int ls1024a_serdes_phy_set_mode(struct phy *phy,
		enum phy_mode mode, int submode)
{
	struct ls1024a_serdes_instance *inst = phy_get_drvdata(phy);
	if (inst->cfg.mode != mode)
		return -EINVAL;
	return 0;
}

static struct phy_ops ls1024a_serdes_phy_ops = {
	.init		= ls1024a_serdes_phy_init,
	.exit		= ls1024a_serdes_phy_exit,
	.power_on	= ls1024a_serdes_phy_power_on,
	.power_off	= ls1024a_serdes_phy_power_off,
	.set_mode	= ls1024a_serdes_phy_set_mode,
	.owner		= THIS_MODULE,
};

static const struct of_device_id ls1024a_serdes_phy_of_match[] = {
	{
		.compatible = "fsl,ls1024a-serdes-phy",
		.data = NULL
	},
	/* Sentinel */
	{ },
};
MODULE_DEVICE_TABLE(of, ls1024a_serdes_phy_of_match);

static enum phy_mode ls1024a_binding_to_phy_mode(unsigned int type)
{
	switch(type) {
	case LS1024A_SERDES_TYPE_SATA:
		return PHY_MODE_SATA;
	case LS1024A_SERDES_TYPE_PCIE:
		return PHY_MODE_PCIE;
	case LS1024A_SERDES_TYPE_SGMII:
		return PHY_MODE_ETHERNET;
	}
	return PHY_MODE_INVALID;
}

static struct phy *ls1024a_serdes_phy_of_xlate(
		struct device *dev, struct of_phandle_args *args)
{
	uint32_t addr;
	int res;
	struct ls1024a_serdes_phys *phys;
	enum phy_mode expected_mode;

	phys = dev_get_drvdata(dev);

	/* Expecting exactly one argument */
	if (args->args_count != 1) {
		return ERR_PTR(-EINVAL);
	}
	expected_mode = ls1024a_binding_to_phy_mode(args->args[0]);
	if (expected_mode == PHY_MODE_INVALID)
		return ERR_PTR(-EINVAL);

	/* Must be a direct subnode of the PHY provider node */
	if (args->np->parent != dev->of_node)
		return ERR_PTR(-EINVAL);

	res = of_property_read_u32(args->np, "reg", &addr);
	if (res) {
		dev_err(dev, "invalid/missing reg property\n");
		return ERR_PTR(-EINVAL);
	}

	if (addr > 2) {
		dev_err(dev, "SerDes PHY address out of range (0-2): %u\n",
				(unsigned int)addr);
		return ERR_PTR(-EINVAL);
	}

	if (!phys->inst[addr].initialized) {
		dev_err(dev, "SerDes%u PHY is disabled\n", (unsigned int)addr);
		return ERR_PTR(-ENODEV);
	}
	if (phys->inst[addr].cfg.mode != expected_mode) {
		dev_warn(dev, "SerDes%u PHY is not the expected type (%u)\n",
				(unsigned int)addr, args->args[0]);
		return ERR_PTR(-ENODEV);
	}
	return phys->inst[addr].phy;
}

static const struct device_node *ls1024a_serdes_find_cfg_node(
		struct device *dev, unsigned int idx)
{
	struct device_node *child;
	for_each_available_child_of_node(dev->of_node, child) {
		uint32_t addr;
		if (of_property_read_u32(child, "reg", &addr))
			continue;
		if (addr == idx) {
			of_node_put(child);
			return child;
		}
	};
	return NULL;
}

static int ls1024a_serdes_parse_config(struct device *dev, unsigned int idx,
		struct ls1024a_serdes_cfg *cfg)
{
	const struct device_node *node;
	uint32_t value;

	node = ls1024a_serdes_find_cfg_node(dev, idx);
	if (!node)
		return -ENOENT;
	if (of_property_read_bool(node, "fsl,pcie-external-clk"))
		cfg->pcie_extclk = true;
	if (of_property_read_bool(node, "fsl,sata-external-clk"))
		cfg->sata_extclk = true;
	if (!of_property_read_u32(node, "fsl,sata-txlev", &value)) {
		if (value > 15) {
			dev_err(dev, "invalid value %u for fsl,sata-txlev\n",
					(unsigned int)value);
			return -EINVAL;
		}
		cfg->sata_txlev = (int)value;
	} else {
		cfg->sata_txlev = -1;
	}
	if (!of_property_read_u32(node, "fsl,pcie-polarity", &value)) {
		if (value > 3) {
			dev_err(dev, "invalid value %u for fsl,pcie-polarity\n",
					(unsigned int)value);
			return -EINVAL;
		}
		cfg->pcie_polarity = (unsigned int)value;
	} else {
		cfg->pcie_polarity = 0;
	}
	return 0;
}

/* Get valid PHY mode depending on system config straps. */
static int ls1024a_serdes_get_valid_mode(struct device *dev,
		unsigned int idx, enum phy_mode *mode)
{
	struct ls1024a_serdes_phys *phys;
	unsigned int val;
	int res;

	if (idx == 0) {
		*mode = PHY_MODE_PCIE;
		return 0;
	}

	phys = dev_get_drvdata(dev);
	res = regmap_read(phys->gpioregs, GPIO_SYSTEM_CONFIG, &val);
	if (res)
		return res;

	if (idx == 1) {
		if (val & SERDES1_CNF) {
			*mode = PHY_MODE_SATA;
		} else {
			*mode = PHY_MODE_PCIE;
		}
		return 0;
	}

	if (idx == 2) {
		if (val & SERDES2_CNF) {
			*mode = PHY_MODE_ETHERNET;
		} else {
			*mode = PHY_MODE_SATA;
		}
		return 0;
	}

	return -EINVAL;
}

static int ls1024a_serdes_instantiate_phy(struct device *dev, unsigned int idx)
{
	struct ls1024a_serdes_phys *phys;
	struct ls1024a_serdes_instance *inst;
	struct reset_control *reset;
	struct phy *phy;
	int res;

	phys = dev_get_drvdata(dev);
	inst = &phys->inst[idx];

	res = ls1024a_serdes_get_valid_mode(dev, idx, &inst->cfg.mode);
	if (res) {
		dev_err(dev, "failed to get SerDes%u system config\n", idx);
		return res;
	}

	res = ls1024a_serdes_parse_config(dev, idx, &inst->cfg);
	if (res == -ENOENT)
		return 0; /* No config => disabled */
	if (res) {
		dev_err(dev, "failed to parse SerDes%u DT config\n", idx);
		return res;
	}

	reset = devm_reset_control_get_exclusive_by_index(dev, idx);
	if (IS_ERR(reset)) {
		if (PTR_ERR(reset) == -EPROBE_DEFER) {
			dev_dbg(dev, "Serdes%u reset not ready\n", idx);
		} else {
			dev_err(dev, "SerDes%u reset error: %ld\n",
			        idx, PTR_ERR(reset));
		}
		return PTR_ERR(reset);
	}

	phy = devm_phy_create(dev, NULL, &ls1024a_serdes_phy_ops);
	if (IS_ERR(phy)) {
		dev_err(dev, "failed to create SerDes%u PHY\n", idx);
		return PTR_ERR(phy);
	}
	phy_set_drvdata(phy, inst);

	inst->idx = idx;
	inst->reset = reset;
	inst->phy = phy;
	inst->dev = dev;
	inst->initialized = true;
	inst->running = false;
	return 0;
}

static int ls1024a_serdes_read_soc_info(struct device *dev)
{
	struct ls1024a_serdes_phys *phys;
	unsigned int val;
	unsigned int rev;
	int res;

	phys = dev_get_drvdata(dev);

	/* Read XTAL frequency straps */
	res = regmap_read(phys->gpioregs, GPIO_SYSTEM_CONFIG, &val);
	if (res)
		return res;
	if (val & (SERDES_OSC_FREQ | USB_OSC_FREQ)) {
		phys->intclk_24mhz = true;
	} else {
		phys->intclk_24mhz = false;
	}

	/* Read SoC revision */
	res = regmap_read(phys->gpioregs, GPIO_DEVICE_ID, &val);
	if (res)
		return res;
	rev = val >> C2K_REVISION_SHIFT;
	if (rev == 0) {
		phys->soc_is_rev1 = false;
	} else if (rev == 1) {
		phys->soc_is_rev1 = true;
	} else {
		dev_err(dev, "unsupported SoC revision %u\n", rev);
		return -ENOTSUPP;
	}

	return 0;
}

static int ls1024a_serdes_phy_probe(struct platform_device *pdev)
{
	const struct regmap_config base_regmap_cfg = {
		.reg_bits = 16,
		.reg_stride = 4,
		.val_bits = 32,
		.fast_io = true
	};
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *base;
	struct phy_provider *phy_provider;
	const struct of_device_id *match;
	struct ls1024a_serdes_phys *phys;
	unsigned int i;
	unsigned int instantiated;
	int ret;

	match = of_match_device(ls1024a_serdes_phy_of_match, &pdev->dev);
	if (!match)
		return -ENODEV;

	phys = devm_kzalloc(dev, sizeof(*phys), GFP_KERNEL);
	if (!phys)
		return -ENOMEM;
	dev_set_drvdata(dev, phys);

	phys->statregs =
		syscon_regmap_lookup_by_compatible("fsl,ls1024a-phystat");
	if (IS_ERR(phys->statregs)) {
		dev_err(dev, "failed to get phystat registers: %ld\n",
		        PTR_ERR(phys->statregs));
		return PTR_ERR(phys->statregs);
	}

	phys->gpioregs =
		syscon_regmap_lookup_by_compatible("fsl,ls1024a-gpio");
	if (IS_ERR(phys->gpioregs)) {
		dev_err(dev, "failed to get GPIO registers: %ld\n",
		        PTR_ERR(phys->gpioregs));
		return PTR_ERR(phys->gpioregs);
	}

	ret = ls1024a_serdes_read_soc_info(dev);
	if (ret) {
		dev_err(dev, "failed to read SoC info: %d\n", ret);
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		dev_err(dev, "failed to remap SerDes registers: %ld\n",
		        PTR_ERR(base));
		return PTR_ERR(base);
	}
	phys->regs = devm_regmap_init_mmio(dev, base, &base_regmap_cfg);
	if (IS_ERR(phys->regs)) {
		dev_err(dev, "failed to create SerDes regmap: %ld\n",
		        PTR_ERR(phys->regs));
		return PTR_ERR(phys->regs);
	}

	instantiated = 0;
	for (i = 0; i < 3; i++) {
		ret = ls1024a_serdes_instantiate_phy(dev, i);
		if (ret == -EPROBE_DEFER) {
			dev_dbg(dev, "probe deferred\n");
			return ret;
		} else if (ret) {
			dev_err(dev, "SerDes%u PHY instantiation failed: %d\n",
			        i, ret);
		} else {
			instantiated++;
		}
	}
	if (instantiated == 0)
		return -ENOENT;

	phy_provider = devm_of_phy_provider_register(
			dev, ls1024a_serdes_phy_of_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static struct platform_driver ls1024a_serdes_phy_driver = {
	.probe	= ls1024a_serdes_phy_probe,
	.driver = {
		.of_match_table	= ls1024a_serdes_phy_of_match,
		.name  = "ls1024a-serdes-phy",
	}
};
module_platform_driver(ls1024a_serdes_phy_driver);

MODULE_DESCRIPTION("LS1024A SerDes phy driver");
MODULE_AUTHOR("Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>");
MODULE_LICENSE("GPL v2");
