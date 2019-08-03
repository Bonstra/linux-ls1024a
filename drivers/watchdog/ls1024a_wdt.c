#include <linux/clk.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/watchdog.h>
#include <linux/mfd/syscon.h>

#define CLKCORE_DEVICE_RST_CNTRL		0x00
#define CDRC_WD_STATUS_CLEAR			BIT(6)
#define CDRC_AXI_WD_RST_EN			BIT(5)

#define CLKCORE_GNRL_DEVICE_STATUS		0x18
#define CGDS_AXI_WD_RST_ACTIVATED		BIT(0)

#define WDT_HIGH_BOUND				0xd0
#define WDT_CONTROL				0xd4
#define CONTROL_ENABLE				BIT(0)
#define WDT_CURRENT_COUNT			0xd8

struct ls1024a_wdt {
	struct device *dev;
	struct watchdog_device wdt;
	struct clk *clk;
	unsigned long clk_rate; /* Cached value */
	struct reset_control *rst;
	struct regmap *regs;
	struct regmap *clkcore;
};

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int heartbeat = 0;
module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat, "Initial watchdog heartbeat in seconds");


static unsigned int ls1024a_wdt_get_max_timeout(struct ls1024a_wdt *priv)
{
	return (unsigned int)(0xffffffffUL / priv->clk_rate);
}

static bool ls1024a_wdt_hw_is_running(struct ls1024a_wdt *priv)
{
	bool wdt_en, clkcore_rst_en;
	unsigned int val;

	regmap_read(priv->regs, WDT_CONTROL, &val);
	wdt_en = (val & CONTROL_ENABLE) != 0;

	regmap_read(priv->clkcore, CLKCORE_DEVICE_RST_CNTRL, &val);
	clkcore_rst_en = (val & CDRC_AXI_WD_RST_EN) != 0;

	return wdt_en && clkcore_rst_en;
}

static void ls1024a_wdt_hw_enable(struct ls1024a_wdt *priv)
{
	regmap_update_bits(priv->clkcore, CLKCORE_DEVICE_RST_CNTRL,
			CDRC_AXI_WD_RST_EN, CDRC_AXI_WD_RST_EN);
	regmap_update_bits(priv->regs, WDT_CONTROL,
			CONTROL_ENABLE, CONTROL_ENABLE);
}

static void ls1024a_wdt_hw_disable(struct ls1024a_wdt *priv)
{
	regmap_update_bits(priv->regs, WDT_CONTROL,
			CONTROL_ENABLE, 0);
	regmap_update_bits(priv->clkcore, CLKCORE_DEVICE_RST_CNTRL,
			CDRC_AXI_WD_RST_EN, 0);
}

static int ls1024a_wdt_hw_set_timeout(struct ls1024a_wdt *priv,
		unsigned int timeout)
{
	unsigned long high_bound = priv->clk_rate * timeout;
	if (high_bound & ~0xffffffffUL) {
		dev_err(priv->dev, "Attempted to set too large timeout %u\n",
				timeout);
		return -EINVAL;
	}
	/* Set maximum possible timeout first, to prevent accidental reset
	 * if watchdog is still active.
	 */
	regmap_write(priv->regs, WDT_HIGH_BOUND, 0xffffffff);

	regmap_write(priv->regs, WDT_HIGH_BOUND, high_bound);
	return 0;
}

static int ls1024a_wdt_start(struct watchdog_device *wdt)
{
	struct ls1024a_wdt *priv = watchdog_get_drvdata(wdt);
	int res;

	res = ls1024a_wdt_hw_set_timeout(priv, wdt->timeout);
	if (res)
		return res;

	set_bit(WDOG_HW_RUNNING, &wdt->status);
	ls1024a_wdt_hw_enable(priv);

	return 0;
}

static int ls1024a_wdt_stop(struct watchdog_device *wdt)
{
	struct ls1024a_wdt *priv = watchdog_get_drvdata(wdt);

	ls1024a_wdt_hw_disable(priv);
	clear_bit(WDOG_HW_RUNNING, &wdt->status);

	return 0;
}

static int ls1024a_wdt_ping(struct watchdog_device *wdt)
{
	struct ls1024a_wdt *priv = watchdog_get_drvdata(wdt);
	int res;

	if (unlikely(wdt->timeout == 0))
		return -EINVAL;

	res = ls1024a_wdt_hw_set_timeout(priv, wdt->timeout);
	if (res)
		return res;

	return 0;
}

static unsigned int ls1024a_wdt_status(struct watchdog_device *wdt)
{
	return 0;
}

static unsigned int ls1024a_wdt_get_timeleft(struct watchdog_device *wdt)
{
	struct ls1024a_wdt *priv = watchdog_get_drvdata(wdt);
	unsigned int high_bound, current_count;

	regmap_read(priv->regs, WDT_HIGH_BOUND, &high_bound);
	regmap_read(priv->regs, WDT_CURRENT_COUNT, &current_count);

	if (unlikely(current_count >= high_bound))
		return 0;

	return (high_bound - current_count) / priv->clk_rate;
}

static int ls1024a_wdt_restart(struct watchdog_device *wdt,
		unsigned long action, void *data)
{
	struct ls1024a_wdt *priv = watchdog_get_drvdata(wdt);
	int res;

	/* Since resetting the system is the effect we're looking for, we can
	 * carelessly enable the watchdog without resetting the counter.
	 */
	ls1024a_wdt_hw_enable(priv);

	/* Using 0 as timeout will cause the counter to be reset and
	 * immediately match the timeout value, triggering a reset.
	 */
	res = ls1024a_wdt_hw_set_timeout(priv, 0);
	if (unlikely(res))
		return res;

	return 0;
}

static const struct watchdog_ops ls1024a_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ls1024a_wdt_start,
	.stop = ls1024a_wdt_stop,
	.ping = ls1024a_wdt_ping,
	.status = ls1024a_wdt_status,
	.get_timeleft = ls1024a_wdt_get_timeleft,
	.restart = ls1024a_wdt_restart,
};

static const struct watchdog_info ls1024a_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE |
		WDIOF_CARDRESET,
	.firmware_version = 0,
	.identity = "LS1024A watchdog"
};

unsigned int ls1024a_wdt_get_and_clear_bootstatus(struct ls1024a_wdt *priv)
{
	unsigned int status = 0;
	regmap_read(priv->clkcore, CLKCORE_GNRL_DEVICE_STATUS, &status);
	regmap_update_bits(priv->clkcore, CLKCORE_DEVICE_RST_CNTRL,
			CDRC_WD_STATUS_CLEAR, CDRC_WD_STATUS_CLEAR);
	return (status & CGDS_AXI_WD_RST_ACTIVATED) ? WDIOF_CARDRESET : 0;
}

static int ls1024a_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ls1024a_wdt *priv = NULL;
	unsigned int max_timeout;
	int res;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);
	watchdog_set_drvdata(&priv->wdt, priv);

	priv->wdt.info = &ls1024a_wdt_info;
	priv->wdt.ops = &ls1024a_wdt_ops;

	priv->regs = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "Failed to get timer syscon\n");
		return PTR_ERR(priv->regs);
	}

	priv->clkcore = syscon_regmap_lookup_by_phandle(
			dev->of_node, "fsl,clkcore");
	if (IS_ERR(priv->clkcore)) {
		dev_err(dev, "Failed to get clkcore syscon\n");
		return PTR_ERR(priv->clkcore);
	}

	priv->rst = devm_reset_control_get_shared(dev->parent, NULL);
	if (IS_ERR(priv->rst)) {
		dev_err(dev, "Failed to get watchdog reset control\n");
		return PTR_ERR(priv->rst);
	}

	priv->clk = devm_clk_get(dev->parent, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "Failed to get watchdog clock\n");
		return PTR_ERR(priv->clk);
	}

	res = reset_control_deassert(priv->rst);
	if (res) {
		dev_err(dev, "Failed to put watchdog out of reset\n");
		return res;
	}

	res = clk_prepare_enable(priv->clk);
	if (res) {
		dev_err(dev, "Failed to enable watchdog clock\n");
		return res;
	}
	priv->clk_rate = clk_get_rate(priv->clk);

	priv->wdt.bootstatus = ls1024a_wdt_get_and_clear_bootstatus(priv);

	/* Get maximum possible timeout value (in ms) and set it as the
	 * default timeout.
	 */
	max_timeout = ls1024a_wdt_get_max_timeout(priv);
	if (max_timeout == 0) {
		dev_err(dev, "Clock frequency too high for watchdog\n");
		return -EINVAL;
	}
	priv->wdt.max_timeout = max_timeout;
	priv->wdt.timeout = max_timeout;

	/* Apply module or DT paramters */
	watchdog_init_timeout(&priv->wdt, heartbeat, dev);
	watchdog_set_nowayout(&priv->wdt, nowayout);

	/* Use watchdog to restart as last resort */
	watchdog_set_restart_priority(&priv->wdt, 0);

	watchdog_stop_on_unregister(&priv->wdt);

	/* Tell the framework if watchdog is already running */
	if (ls1024a_wdt_hw_is_running(priv))
		set_bit(WDOG_HW_RUNNING, &priv->wdt.status);

	res = devm_watchdog_register_device(dev, &priv->wdt);
	if (res) {
		dev_err(dev, "Failed to register watchdog device\n");
		return res;
	}
	return 0;
}

static int ls1024a_wdt_remove(struct platform_device *pdev)
{
	struct ls1024a_wdt *priv = platform_get_drvdata(pdev);
	clk_disable_unprepare(priv->clk);
	reset_control_assert(priv->rst);
	return 0;
}

static const struct of_device_id ls1024a_wdt_of_match[] = {
	{.compatible = "fsl,ls1024a-wdt",},
	{ },
};
MODULE_DEVICE_TABLE(of, ls1024a_wdt_of_match);

static struct platform_driver ls1024a_wdt_driver = {
	.probe	= ls1024a_wdt_probe,
	.remove	= ls1024a_wdt_remove,
	.driver = {
		.name	= "ls1024a-wdt",
		.of_match_table	= ls1024a_wdt_of_match,
	}
};

module_platform_driver(ls1024a_wdt_driver);

MODULE_DESCRIPTION("LS1024A watchdog timer driver");
MODULE_AUTHOR("Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>");
MODULE_LICENSE("GPL v2");
