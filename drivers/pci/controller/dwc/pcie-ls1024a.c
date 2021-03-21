// SPDX-License-Identifier: GPL-2.0

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/resource.h>

#include "pcie-designware.h"

struct ls1024a_pcie {
	struct dw_pcie *pci;
	struct clk *clk;
	struct phy *phy;
	struct reset_control *axi_reset;
	struct reset_control *power_reset;
	struct reset_control *regs_reset;
	struct regmap *app_regs;
	struct irq_domain *irq_domain;
	unsigned int port_idx;
};

#define PCIEx_CFGx(port, reg)	(((port) * 0x20) + ((reg) * 0x4))
#define PCIEx_STSx(port, reg)	(0x40 + ((port) * 0xc) + ((reg) * 0x4))
#define PCIEx_STS3(port)	(0x58 + ((port) * 0x4))
#define PCIEx_INTR_STS(port)	(0x100 + ((port) * 0x10))
#define PCIEx_INTR_EN(port)	(0x104 + ((port) * 0x10))

#define CFG0_DEV_TYPE_MASK	0xF
#define CFG0_DEV_TYPE_RC	0x4 /* Root complex */

#define CFG5_LINK_DOWN_RST	BIT(9)
#define CFG5_APP_RDY_L23	BIT(2)
#define CFG5_LTSSM_EN		BIT(1)
#define CFG5_APP_INIT_RST	BIT(0)

#define STS0_RDLH_LINK_UP	BIT(16)
#define STS0_XMLH_LINK_UP	BIT(15)
#define STS0_LINK_REQ_RST_NOT	BIT(0)

#define PCIE_INTR_MSI		BIT(12)
#define PCIE_INTR_LINK_AUTO_BW	BIT(11)
#define PCIE_INTR_HP		BIT(10)
#define PCIE_INTR_PME		BIT(9)
#define PCIE_INTR_AER		BIT(8)
#define PCIE_INTR_INTD_DEASSERT	BIT(7)
#define PCIE_INTR_INTD_ASSERT	BIT(6)
#define PCIE_INTR_INTC_DEASSERT	BIT(5)
#define PCIE_INTR_INTC_ASSERT	BIT(4)
#define PCIE_INTR_INTB_DEASSERT	BIT(3)
#define PCIE_INTR_INTB_ASSERT	BIT(2)
#define PCIE_INTR_INTA_DEASSERT	BIT(1)
#define PCIE_INTR_INTA_ASSERT	BIT(0)

/* IRQ numbers in the LS1024A PCIe MUX IRQ domain.
 * They are numbered after their order in the hardware so that currently unused
 * IRQs may be used in the future without renumbering and breaking existing
 * device trees.
 */
#define LS1024A_PCIE_INTC_INTA	0
#define LS1024A_PCIE_INTC_INTB	2
#define LS1024A_PCIE_INTC_INTC	4
#define LS1024A_PCIE_INTC_INTD	6
#define LS1024A_PCIE_INTC_MSI	12

#define LS1024A_PCIE_INTC_NUM_INTS	13

#define to_ls1024a_pcie(x)	dev_get_drvdata((x)->dev)

static int ls1024a_reset_assert(struct ls1024a_pcie *pcie)
{
	int res;

	res = reset_control_assert(pcie->regs_reset);
	if (res)
		goto fail;
	res = reset_control_assert(pcie->power_reset);
	if (res)
		goto fail;
	res = reset_control_assert(pcie->axi_reset);
	if (res)
		goto fail;
	return 0;
fail:
	dev_err(pcie->pci->dev, "Failed to assert resets: %d\n", res);
	return res;
}

static int ls1024a_reset_deassert(struct ls1024a_pcie *pcie)
{
	int res;

	res = reset_control_deassert(pcie->axi_reset);
	if (res)
		goto fail;
	res = reset_control_deassert(pcie->power_reset);
	if (res)
		goto fail;
	res = reset_control_deassert(pcie->regs_reset);
	if (res)
		goto fail;
	return 0;
fail:
	dev_err(pcie->pci->dev, "Failed to deassert resets: %d\n", res);
	ls1024a_reset_assert(pcie);
	return res;
}

static void ls1024a_pcie_disable_phy(struct ls1024a_pcie *pcie)
{
	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
}

static int ls1024a_pcie_enable_phy(struct ls1024a_pcie *pcie)
{
	int ret;

	ret = phy_init(pcie->phy);
	if (ret)
		return ret;

	ret = phy_set_mode(pcie->phy, PHY_MODE_PCIE);
	if (ret) {
		phy_exit(pcie->phy);
		return ret;
	}

	ret = phy_power_on(pcie->phy);
	if (ret) {
		phy_exit(pcie->phy);
		return ret;
	}

	return 0;
}

static int ls1024a_pcie_setup_phy(struct ls1024a_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret = 0;

	pcie->phy = devm_phy_get(dev, "bus");
	if (IS_ERR(pcie->phy) &&
	    (PTR_ERR(pcie->phy) == -EPROBE_DEFER))
		return PTR_ERR(pcie->phy);

	if (IS_ERR(pcie->phy)) {
		pcie->phy = NULL;
	}

	if (!pcie->phy) {
		dev_err(dev, "No available PHY\n");
		return -EINVAL;
	}

	ret = ls1024a_pcie_enable_phy(pcie);
	if (ret)
		dev_err(dev, "Failed to initialize PHY(s) (%d)\n", ret);

	return ret;
}

static int ls1024a_pcie_link_up(struct dw_pcie *pci)
{
	struct ls1024a_pcie *pcie = to_ls1024a_pcie(pci);
	unsigned int port = pcie->port_idx;
	unsigned int reg = PCIEx_STSx(port, 0);
	unsigned int val;

	regmap_read(pcie->app_regs, reg, &val);
	if ((val & STS0_RDLH_LINK_UP) == STS0_RDLH_LINK_UP)
		return 1;

	dev_dbg(pci->dev, "No link detected (PCIE%u_STS0: 0x%x).\n", port, val);
	return 0;
}

static void ls1024a_pcie_establish_link(struct ls1024a_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	unsigned int reg;
	unsigned int val = 0;

	if (!dw_pcie_link_up(pci)) {
		/* Disable LTSSM state machine to enable configuration */
		reg = PCIEx_CFGx(pcie->port_idx, 5);
		regmap_read(pcie->app_regs, reg, &val);
		val &= ~(CFG5_LTSSM_EN);
		regmap_write(pcie->app_regs, reg, val);
	}

	/* Set the device to root complex mode */
	reg = PCIEx_CFGx(pcie->port_idx, 0);
	regmap_read(pcie->app_regs, reg, &val);
	val &= ~CFG0_DEV_TYPE_MASK;
	val |= CFG0_DEV_TYPE_RC;
	regmap_write(pcie->app_regs, reg, val);

	if (!dw_pcie_link_up(pci)) {
		/* Configuration done. Start LTSSM */
		reg = PCIEx_CFGx(pcie->port_idx, 5);
		regmap_read(pcie->app_regs, reg, &val);
		val |= CFG5_LTSSM_EN | CFG5_APP_INIT_RST;
		regmap_write(pcie->app_regs, reg, val);
	}

	/* Wait until the link becomes active again */
	if (dw_pcie_wait_for_link(pci))
		dev_err(pci->dev, "Link not up after reconfiguration\n");
}

static int ls1024a_pcie_start_link(struct dw_pcie *pci)
{
	struct ls1024a_pcie *pcie = to_ls1024a_pcie(pci);
	ls1024a_pcie_establish_link(pcie);
	return 0;
}

static void ls1024a_pcie_mask_irq(struct irq_data *data)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(data);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ls1024a_pcie *pcie = to_ls1024a_pcie(pci);
	u32 irq = BIT(data->hwirq);

	regmap_write_bits(pcie->app_regs,
			PCIEx_INTR_EN(pcie->port_idx),
			irq, 0);
}

static void ls1024a_pcie_unmask_irq(struct irq_data *data)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(data);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ls1024a_pcie *pcie = to_ls1024a_pcie(pci);
	u32 irq = BIT(data->hwirq);

	regmap_write_bits(pcie->app_regs,
			PCIEx_INTR_EN(pcie->port_idx),
			irq, irq);
}

/* PCIe IRQ MUX */
static struct irq_chip ls1024a_pcie_intc_irq_chip = {
	.name = "LS1024A PCIe IRQ MUX",
	.irq_mask = ls1024a_pcie_mask_irq,
	.irq_unmask = ls1024a_pcie_unmask_irq,
};

static int ls1024a_pcie_intc_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &ls1024a_pcie_intc_irq_chip,
			handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intc_domain_ops = {
	.map = ls1024a_pcie_intc_map,
	.xlate = irq_domain_xlate_onecell,
};

static irqreturn_t ls1024a_pcie_intc_handler(int irq, void *arg)
{
	const unsigned int intx_mask =
		PCIE_INTR_INTA_ASSERT | PCIE_INTR_INTB_ASSERT |
		PCIE_INTR_INTC_ASSERT | PCIE_INTR_INTD_ASSERT;
	struct ls1024a_pcie *pcie = arg;
	unsigned int reg;
	unsigned int status;
	unsigned int virq;

	reg = PCIEx_INTR_STS(pcie->port_idx);
	regmap_read(pcie->app_regs, reg, &status);
	/* Acknowledge interrupts */
	regmap_write(pcie->app_regs, reg, status);

	if (status & PCIE_INTR_MSI) {
		BUG_ON(!IS_ENABLED(CONFIG_PCI_MSI));
		virq = irq_find_mapping(pcie->irq_domain,
				LS1024A_PCIE_INTC_MSI);
		if (virq)
			generic_handle_irq(virq);
	}
	if (status & intx_mask) {
		if ((status & PCIE_INTR_INTA_ASSERT) != 0) {
			virq = irq_find_mapping(pcie->irq_domain,
					LS1024A_PCIE_INTC_INTA);
			if (virq)
				generic_handle_irq(virq);
		}
		if ((status & PCIE_INTR_INTB_ASSERT) != 0) {
			virq = irq_find_mapping(pcie->irq_domain,
					LS1024A_PCIE_INTC_INTB);
			if (virq)
				generic_handle_irq(virq);
		}
		if ((status & PCIE_INTR_INTC_ASSERT) != 0) {
			virq = irq_find_mapping(pcie->irq_domain,
					LS1024A_PCIE_INTC_INTC);
			if (virq)
				generic_handle_irq(virq);
		}
		if ((status & PCIE_INTR_INTD_ASSERT) != 0) {
			virq = irq_find_mapping(pcie->irq_domain,
					LS1024A_PCIE_INTC_INTD);
			if (virq)
				generic_handle_irq(virq);
		}
	}

	return IRQ_HANDLED;
}

static struct device_node *find_intc_node(unsigned int index)
{
	char intc_node_name[32];
	struct device_node *intc_node;
	struct device_node *node = of_find_compatible_node(NULL, NULL,
			"fsl,ls1024a-pci-usb-ctrl");

	if (!node)
		return NULL;

	snprintf(intc_node_name, sizeof(intc_node_name),
			"pcie%u-interrupt-controller", index);

	intc_node = of_get_child_by_name(node, intc_node_name);
	of_node_put(node);

	if (!intc_node)
		pr_err("PCIe interrupt node %s not found\n", intc_node_name);

	return intc_node;
}

static int ls1024a_pcie_init_irq(struct ls1024a_pcie *pcie,
		struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	struct device_node *intc_node;
	int ret;

	intc_node = find_intc_node(pcie->port_idx);
	if (!intc_node) {
		return -ENODEV;
	}

	pcie->irq_domain = irq_domain_add_linear(intc_node,
						LS1024A_PCIE_INTC_NUM_INTS,
						&intc_domain_ops, pp);
	if (!pcie->irq_domain) {
		of_node_put(intc_node);
		dev_err(dev, "Failed to get PCIe INTC IRQ domain\n");
		return -ENODEV;
	}

	pp->irq = of_irq_get(intc_node, 0);
	if (pp->irq < 0) {
		of_node_put(intc_node);
		dev_err(dev, "failed to get irq for port\n");
		return pp->irq;
	}
	of_node_put(intc_node);

	ret = devm_request_irq(dev, pp->irq, ls1024a_pcie_intc_handler,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       "ls1024a-pcie-intc", pcie);
	if (ret) {
		dev_err(dev, "failed to request irq %d\n", pp->irq);
		return ret;
	}

	return 0;
}

static const struct dw_pcie_host_ops ls1024a_pcie_host_ops;

static int ls1024a_add_pcie_port(struct ls1024a_pcie *pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->ops = &ls1024a_pcie_host_ops;

	ret = ls1024a_pcie_init_irq(pcie, pp);
	if (ret < 0)
		return ret;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.link_up = ls1024a_pcie_link_up,
	.start_link = ls1024a_pcie_start_link,
};

static int ls1024a_reset_setup(struct ls1024a_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	pcie->axi_reset = devm_reset_control_get_exclusive(pci->dev, "axi");
	if (IS_ERR(pcie->axi_reset)) {
		int ret = PTR_ERR(pcie->axi_reset);
		dev_err(pci->dev, "Failed to get AXI reset: %d\n", ret);
		return ret;
	}
	pcie->power_reset =
		devm_reset_control_get_exclusive(pci->dev, "power");
	if (IS_ERR(pcie->power_reset)) {
		int ret = PTR_ERR(pcie->power_reset);
		dev_err(pci->dev, "Failed to get power reset: %d\n", ret);
		return ret;
	}
	pcie->regs_reset = devm_reset_control_get_exclusive(pci->dev, "regs");
	if (IS_ERR(pcie->regs_reset)) {
		int ret = PTR_ERR(pcie->regs_reset);
		dev_err(pci->dev, "Failed to get regs reset: %d\n", ret);
		return ret;
	}
	return ls1024a_reset_assert(pcie);
}

static int ls1024a_pcie_probe(struct platform_device *pdev)
{
	struct dw_pcie *pci;
	struct ls1024a_pcie *pcie;
	struct device *dev = &pdev->dev;
	struct resource *base;
	uint32_t port = 0;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	pcie->pci = pci;

	ret = of_property_read_u32(dev->of_node, "fsl,port-index", &port);
	if (ret || port > 1) {
		dev_err(dev, "Missing or invalid fsl,port-index property.\n");
		return -EINVAL;
	}
	pcie->port_idx = port;
	dev_dbg(dev, "port_idx: %u\n", pcie->port_idx);

	pcie->app_regs = syscon_regmap_lookup_by_compatible(
			"fsl,ls1024a-pci-usb-ctrl");
	if (IS_ERR(pcie->app_regs)) {
		ret = PTR_ERR(pcie->app_regs);
		dev_err(dev, "Failed to get PCI ctrl syscon: %d\n", ret);
		return ret;
	}

	pcie->clk = devm_clk_get(dev, "axi");
	if (IS_ERR(pcie->clk))
		return PTR_ERR(pcie->clk);

	ret = ls1024a_reset_setup(pcie);
	if (ret)
		return ret;

	ret = clk_prepare_enable(pcie->clk);
	if (ret)
		goto fail_reset;

	ret = ls1024a_pcie_setup_phy(pcie);
	if (ret)
		goto fail_clk;

	ret = ls1024a_reset_deassert(pcie);
	if (ret)
		goto disable_phy;

	/* Get the dw-pcie unit DBI registers base. */
	base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, base);
	if (IS_ERR(pci->dbi_base)) {
		dev_err(dev, "couldn't remap regs base %p\n", base);
		ret = PTR_ERR(pci->dbi_base);
		goto reset;
	}

	platform_set_drvdata(pdev, pcie);

	ret = ls1024a_add_pcie_port(pcie, pdev);
	if (ret)
		goto reset;

	return 0;

reset:
	ls1024a_reset_assert(pcie);
disable_phy:
	ls1024a_pcie_disable_phy(pcie);
fail_clk:
	clk_disable_unprepare(pcie->clk);
fail_reset:
	return ret;
}

static const struct of_device_id ls1024a_pcie_of_match[] = {
	{ .compatible = "fsl,ls1024a-pcie", },
	{},
};

static struct platform_driver ls1024a_pcie_driver = {
	.probe		= ls1024a_pcie_probe,
	.driver = {
		.name	= "ls1024a-pcie",
		.of_match_table = of_match_ptr(ls1024a_pcie_of_match),
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ls1024a_pcie_driver);

