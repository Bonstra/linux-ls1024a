// SPDX-License-Identifier: GPL-2.0

/* Some of the code was borrowed from the Linux driver in the LS1024A SDK.
 *
 * Copyright (C) 2008 Mindspeed Technologies Inc.
 * Copyright (C) 2019 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define DRV_NAME "i2c-ls1024a"

#define I2C_STANDARD_FREQ	100000UL
#define I2C_FAST_FREQ		400000UL
#define I2C_HIGH_SPEED_FREQ	3400000UL

#define LS1024A_I2C_ADDR		(0x00*4)
#define LS1024A_I2C_DATA		(0x01*4)
#define LS1024A_I2C_CNTR		(0x02*4)
#define LS1024A_I2C_STAT		(0x03*4)
#define LS1024A_I2C_CCRFS		(0x03*4)
#define LS1024A_I2C_XADDR		(0x04*4)
#define LS1024A_I2C_CCRH		(0x05*4)
#define LS1024A_I2C_RESET		(0x07*4)

/* CNTR - Control register bits */
#define CNTR_IEN			(1<<7)
#define CNTR_ENAB			(1<<6)
#define CNTR_STA			(1<<5)
#define CNTR_STP			(1<<4)
#define CNTR_IFLG			(1<<3)
#define CNTR_AAK			(1<<2)

/* STAT - Status codes */
#define STAT_BUS_ERROR			0x00	/* Bus error in master mode only */
#define STAT_START			0x08	/* Start condition transmitted */
#define STAT_START_REPEATED		0x10	/* Repeated Start condition transmited */
#define STAT_ADDR_WR_ACK		0x18	/* Address + Write bit transmitted, ACK received */
#define STAT_ADDR_WR_NACK		0x20	/* Address + Write bit transmitted, NACK received */
#define STAT_DATA_WR_ACK		0x28	/* Data byte transmitted in master mode , ACK received */
#define STAT_DATA_WR_NACK		0x30	/* Data byte transmitted in master mode , NACK received */
#define STAT_ARBIT_LOST			0x38	/* Arbitration lost in address or data byte */
#define STAT_ADDR_RD_ACK		0x40	/* Address + Read bit transmitted, ACK received  */
#define STAT_ADDR_RD_NACK		0x48	/* Address + Read bit transmitted, NACK received  */
#define STAT_DATA_RD_ACK		0x50	/* Data byte received in master mode, ACK transmitted  */
#define STAT_DATA_RD_NACK		0x58	/* Data byte received in master mode, NACK transmitted*/
#define STAT_NO_RELEVANT_INFO		0xF8	/* No relevant status information, IFLF=0 */


#define REG_ADDR(i2c, offset)		((i2c)->base + (offset))
#define RD_REG(i2c, offset)		readb(REG_ADDR(i2c, offset))
#define WR_REG(i2c, offset, byte)	writeb(byte, REG_ADDR(i2c, offset))
#define RD_DATA(i2c)			RD_REG(i2c, LS1024A_I2C_DATA)
#define WR_DATA(i2c, byte)		WR_REG(i2c, LS1024A_I2C_DATA, byte)
#define RD_CNTR(i2c)			RD_REG(i2c, LS1024A_I2C_CNTR)
#define WR_CNTR(i2c, byte)		WR_REG(i2c, LS1024A_I2C_CNTR, byte)
#define RD_STAT(i2c)			RD_REG(i2c, LS1024A_I2C_STAT)
#define WR_CCRFS(i2c, byte)		WR_REG(i2c, LS1024A_I2C_CCRFS, byte)
#define WR_CCRH(i2c, byte)		WR_REG(i2c, LS1024A_I2C_CCRH, byte)
#define WR_RESET(i2c, byte)		WR_REG(i2c, LS1024A_I2C_RESET, byte)

#define to_ls1024a_i2c(d) container_of(d, struct ls1024a_i2c, adap)

struct ls1024a_i2c {
	struct device *dev;
	struct i2c_adapter adap;
	struct clk *clk;
	struct reset_control *reset;
	void __iomem *base;
	int irq;
	wait_queue_head_t wait;
	unsigned long busfreq;
};

static u8 ls1024a_i2c_calculate_dividers(
		struct ls1024a_i2c *i2c,
		unsigned long freq)
{
	unsigned long m, n, hz;
	unsigned long saved_n, saved_m, saved_hz;
	u8 dividers;
	unsigned long refclk;

	/* Get the i2c clock rate */
	refclk = clk_get_rate(i2c->clk);

	saved_hz = saved_n = saved_m = 0;

	for (m = 0; m < 16; m++) {
		for (n = 0; n < 8; n++) {
			hz = refclk / ((1 << n) * (m + 1) * 10);
			if (!saved_hz || abs(freq - hz) < abs(freq - saved_hz)) {
				saved_n = n;
				saved_m = m;
				saved_hz = hz;
			}
		}
	}

	dividers = (saved_m << 3) | saved_n;
	dev_dbg(i2c->dev, "target=%luHz actual=%luHz M=%lu N=%lu dividers=0x%02x\n",
			freq, saved_hz, saved_m, saved_n, dividers);

	return dividers;
}

/* Set divider values for fast and high-speed modes */
static void ls1024a_i2c_set_dividers(struct ls1024a_i2c *i2c)
{
	u8 fast_divs;
	u8 high_divs;

	if (i2c->busfreq > I2C_FAST_FREQ) {
		fast_divs = ls1024a_i2c_calculate_dividers(i2c, I2C_FAST_FREQ);
		high_divs = ls1024a_i2c_calculate_dividers(i2c, i2c->busfreq);
	} else {
		fast_divs = ls1024a_i2c_calculate_dividers(i2c, i2c->busfreq);
		high_divs = fast_divs;
	}
	WR_CCRFS(i2c, fast_divs);
	WR_CCRH(i2c, high_divs);
}

static void ls1024a_i2c_soft_reset(struct ls1024a_i2c *i2c)
{
	u8 status;
	/* Disable as many things as possible. Do not attempt to clear IFLG
	 * as it would cause the state machine to advance.
	 */
	WR_CNTR(i2c, CNTR_IFLG);
	WR_DATA(i2c, 0);
	WR_RESET(i2c, 1);

	udelay(10);
	status = RD_STAT(i2c);
	if (status != STAT_NO_RELEVANT_INFO) {
		dev_warn(i2c->dev, "controller not idle after reset! STAT=%02x\n",
				status);
	}
	ls1024a_i2c_set_dividers(i2c);
}

static irqreturn_t ls1024a_i2c_irq_handler(int irq, void *userdata)
{
	struct ls1024a_i2c *i2c = userdata;

	if (!(RD_CNTR(i2c) & CNTR_IFLG))
		return IRQ_NONE;

	/* Disable interrupts */
	WR_CNTR(i2c, RD_CNTR(i2c) & ~CNTR_IEN);

	/* Notify waiter */
	wake_up(&i2c->wait);

	return IRQ_HANDLED;
}

/*
 * Returns the timeout (in jiffies) for the given message.
 */
static int ls1024a_i2c_calculate_timeout(struct ls1024a_i2c *i2c,
		struct i2c_msg *msg)
{
	int timeout;

	/* if no timeout was specified, calculate it */
	if (i2c->adap.timeout <= 0) {
		/* for the interrupt mode calculate timeout for 'full' message */
		timeout = ((int)msg->len) * 10;	/* convert approx. to bits */
		timeout *= 1000;		/* prepare convert to KHz */
		timeout /= i2c->busfreq;	/* convert to bits per ms */
		timeout += timeout >> 1;	/* add 50% */
		timeout = (timeout * HZ) * 1000;/* convert to jiffies */
		if (timeout < HZ / 5)		/* at least 200ms */
			timeout = HZ / 5;
	} else {
		timeout = i2c->adap.timeout;
	}
	return timeout;
}

static bool ls1024a_i2c_test_iflg(struct ls1024a_i2c *i2c)
{
	return (RD_CNTR(i2c) & CNTR_IFLG) != 0;
}

static bool ls1024a_i2c_is_idle(struct ls1024a_i2c *i2c)
{
	u8 cntr = RD_CNTR(i2c);
	u8 status = RD_STAT(i2c);
	if (cntr & (CNTR_STP | CNTR_STA | CNTR_IFLG))
		return false;
	if (status != STAT_NO_RELEVANT_INFO)
		return false;
	return true;
}

static void ls1024a_i2c_disable_interrupts(struct ls1024a_i2c *i2c)
{
	/* Reset the IEN flag. We keep IFLG set so we do not cause the
	 * controller's state machine to advance.
	 */
	WR_CNTR(i2c, (RD_CNTR(i2c) & ~CNTR_IEN) | CNTR_IFLG);
}

static int ls1024a_i2c_stop(struct ls1024a_i2c *i2c, unsigned long timeout_ms)
{
	unsigned long mark = jiffies + msecs_to_jiffies(timeout_ms);

	WR_CNTR(i2c, CNTR_STP);
	while (time_is_after_jiffies(mark)) {
		if (ls1024a_i2c_is_idle(i2c))
			return 0; /* Okay, controller is idle */
		if (need_resched())
			schedule();
	}
	/* Check one last time */
	if (ls1024a_i2c_is_idle(i2c))
		return 0;
	return -ETIMEDOUT;
}

static void ls1024a_i2c_recover(struct ls1024a_i2c *i2c)
{
	const unsigned long timeout_ms = 1000;
	int res;
	u8 cntr;
	u8 status;

	res = ls1024a_i2c_stop(i2c, timeout_ms);
	if (!res)
		return;

	/* Attempt to diagnose issue */
	cntr = RD_CNTR(i2c);
	status = RD_STAT(i2c);

	if ((status == STAT_NO_RELEVANT_INFO) &&
	    (cntr & (CNTR_STA)) &&
	    !(cntr & CNTR_IFLG)) {
		dev_warn(i2c->dev, "I2C bus is still busy after %lu ms!\n",
				timeout_ms);
	} else {
		dev_warn(i2c->dev, "Controller busy after %lu ms! CNTR=%02x STAT=%02x\n",
				timeout_ms, cntr, status);
	}
	dev_info(i2c->dev, "Resetting controller\n");
	ls1024a_i2c_soft_reset(i2c);
}

static int ls1024a_i2c_repeated_start(struct ls1024a_i2c *i2c)
{
	const unsigned long timeout_ms = 250;
	int res;
	u8 status;

	WR_CNTR(i2c, CNTR_STA | CNTR_IEN);
	res = wait_event_interruptible_timeout(i2c->wait,
			ls1024a_i2c_test_iflg(i2c),
			msecs_to_jiffies(timeout_ms));
	if (!res) {
		dev_warn(i2c->dev, "I2C bus busy after %lu ms\n", timeout_ms);
		ls1024a_i2c_disable_interrupts(i2c);
		return -EBUSY;
	} else if (res == -ERESTARTSYS) {
		ls1024a_i2c_disable_interrupts(i2c);
		return res;
	}

	status = RD_STAT(i2c);
	switch(status) {
	case STAT_START:
	case STAT_START_REPEATED:
		break;
	case STAT_ARBIT_LOST:
		return -EAGAIN;
	default:
		dev_err(i2c->dev, "Unexpected status %02x", status);
		return -EIO;
	}
	return 0;
}

static int ls1024a_i2c_send_address(struct ls1024a_i2c *i2c,
		struct i2c_msg *msg)
{
	const unsigned long timeout_ms = 250;
	u8 data = i2c_8bit_addr_from_msg(msg);
	u8 status;
	int res;

	WR_DATA(i2c, data);
	WR_CNTR(i2c, CNTR_IEN);
	res = wait_event_interruptible_timeout(i2c->wait,
			ls1024a_i2c_test_iflg(i2c),
			msecs_to_jiffies(timeout_ms));
	if (!res) {
		dev_warn(i2c->dev, "I2C bus busy after %lu ms\n", timeout_ms);
		ls1024a_i2c_disable_interrupts(i2c);
		return -EBUSY;
	} else if (res == -ERESTARTSYS) {
		ls1024a_i2c_disable_interrupts(i2c);
		return res;
	}

	status = RD_STAT(i2c);
	switch(status) {
	case STAT_ADDR_RD_ACK:
	case STAT_ADDR_WR_ACK:
		break;
	case STAT_ADDR_RD_NACK:
	case STAT_ADDR_WR_NACK:
		return -ENXIO;
	case STAT_ARBIT_LOST:
		return -EAGAIN;
	default:
		dev_err(i2c->dev, "Unexpected status %02x", status);
		return -EIO;
	}
	return 0;
}

static int ls1024a_i2c_data_tx(struct ls1024a_i2c *i2c,
		struct i2c_msg *msg)
{
	unsigned int i;
	int res;
	unsigned long timeout;
	unsigned long timeout_ms;

	timeout = ls1024a_i2c_calculate_timeout(i2c, msg);
	timeout_ms = jiffies_to_msecs(timeout);
	for (i = 0; i < msg->len; i ++) {
		u8 status;
		u8 data = msg->buf[i];

		WR_DATA(i2c, data);
		WR_CNTR(i2c, CNTR_IEN);
		res = wait_event_interruptible_timeout(i2c->wait,
				ls1024a_i2c_test_iflg(i2c), timeout);
		if (!res) {
			dev_warn(i2c->dev, "Write timed out after %lu ms\n", timeout_ms);
			ls1024a_i2c_disable_interrupts(i2c);
			return -EBUSY;
		} else if (res == -ERESTARTSYS) {
			ls1024a_i2c_disable_interrupts(i2c);
			return res;
		}

		status = RD_STAT(i2c);
		switch(status) {
		case STAT_DATA_WR_ACK:
			break;
		case STAT_DATA_WR_NACK:
			return -EIO;
		case STAT_ARBIT_LOST:
			return -EAGAIN;
		default:
			dev_err(i2c->dev, "Unexpected status %02x", status);
			return -EIO;
		}
	}
	return 0;
}

static int ls1024a_i2c_data_rx(struct ls1024a_i2c *i2c,
		struct i2c_msg *msg)
{
	unsigned int i;
	int res;
	unsigned long timeout;
	unsigned long timeout_ms;

	timeout = ls1024a_i2c_calculate_timeout(i2c, msg);
	timeout_ms = jiffies_to_msecs(timeout);
	for (i = 0; i < msg->len; i ++) {
		u8 status;
		u8 data = msg->buf[i];
		u8 cntr = CNTR_IEN;

		/* ACK the byte, unless it is the last one */
		if (i != (msg->len - 1))
			cntr |= CNTR_AAK;

		WR_CNTR(i2c, cntr);
		res = wait_event_interruptible_timeout(i2c->wait,
				ls1024a_i2c_test_iflg(i2c), timeout);
		if (!res) {
			dev_warn(i2c->dev, "Read timed out after %lu ms\n", timeout_ms);
			ls1024a_i2c_disable_interrupts(i2c);
			return -EBUSY;
		} else if (res == -ERESTARTSYS) {
			ls1024a_i2c_disable_interrupts(i2c);
			return res;
		}

		status = RD_STAT(i2c);
		data = RD_DATA(i2c);
		switch(status) {
		case STAT_DATA_RD_ACK:
		case STAT_DATA_RD_NACK:
			msg->buf[i] = data;
			break;
		case STAT_ARBIT_LOST:
			return -EAGAIN;
		default:
			dev_err(i2c->dev, "Unexpected status %02x", status);
			return -EIO;
		}
	}
	return 0;
}

static int ls1024a_i2c_perform_xfer(struct ls1024a_i2c *i2c,
		struct i2c_msg *msg)
{
	if (msg->flags & I2C_M_RD) {
		return ls1024a_i2c_data_rx(i2c, msg);
	} else {
		return ls1024a_i2c_data_tx(i2c, msg);
	}
}

static int ls1024a_i2c_process_msg(struct ls1024a_i2c *i2c,
		struct i2c_msg *msg, bool last)
{
	int res;

	res = ls1024a_i2c_repeated_start(i2c);
	if (res)
		goto fail;
	res = ls1024a_i2c_send_address(i2c, msg);
	if (res)
		goto fail;
	res = ls1024a_i2c_perform_xfer(i2c, msg);
	if (res)
		goto fail;
	if (last) {
		res = ls1024a_i2c_stop(i2c, 250);
		if (res)
			goto fail;
	}
	return 0;
fail:
	ls1024a_i2c_recover(i2c);
	return res;
}

static int ls1024a_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
		int num)
{
	struct ls1024a_i2c *i2c = to_ls1024a_i2c(adap);
	int i;
	int res;

	for (i = 0; i < num; i++) {
		res = ls1024a_i2c_process_msg(i2c, &msgs[i], i == (num - 1));
		if (res) {
			dev_dbg(i2c->dev, "transfer failed on message #%d (addr=%#x, flags=%#x, len=%u)\n",
				i, msgs[i].addr, msgs[i].flags, msgs[i].len);
			return res;
		}
	}
	return i;
}

static int ls1024a_i2c_hw_init(struct ls1024a_i2c *i2c)
{
	int res;

	res = reset_control_deassert(i2c->reset);
	if (res) {
		dev_err(i2c->dev, "Failed to deassert reset\n");
		goto fail_reset;
	}

	res = clk_enable(i2c->clk);
	if (res) {
		dev_err(i2c->dev, "Failed to enable clock\n");
		goto fail_clk;
	}

	udelay(10);
	ls1024a_i2c_set_dividers(i2c);

	return 0;

fail_clk:
fail_reset:
	return res;
}

static void ls1024a_i2c_hw_deinit(struct ls1024a_i2c *i2c)
{
	ls1024a_i2c_soft_reset(i2c);
	clk_disable(i2c->clk);
	reset_control_assert(i2c->reset);
}

static u32 ls1024a_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ls1024a_i2c_algo = {
	.master_xfer = ls1024a_i2c_xfer,
	.functionality = ls1024a_i2c_functionality,
};

static int ls1024a_i2c_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct ls1024a_i2c *i2c;
	struct i2c_adapter *adap;
	u32 freq = I2C_STANDARD_FREQ;
	int res;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	init_waitqueue_head(&i2c->wait);
	platform_set_drvdata(pdev, i2c);
	i2c->dev = &pdev->dev;
	adap = &i2c->adap;

	of_property_read_u32(node, "clock-frequency", &freq);
	if (freq > I2C_HIGH_SPEED_FREQ) {
		dev_err(i2c->dev, "Bus frequency %lu too high. Maximum is %lu Hz.\n",
				(unsigned long)freq, I2C_HIGH_SPEED_FREQ);
		return -EINVAL;
	} else if (freq == 0) {
		dev_err(i2c->dev, "Bus frequency cannot be 0.\n");
		return -EINVAL;
	}
	if (freq > I2C_FAST_FREQ) {
		dev_err(i2c->dev, "High-speed mode is not implemented yet\n");
		return -EINVAL;
	}
	i2c->busfreq = freq;

	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base)) {
		dev_err(i2c->dev, "Failed to map IO registers\n");
		return PTR_ERR(i2c->base);
	}

	i2c->irq = platform_get_irq(pdev, 0);
	if (i2c->irq < 0) {
		dev_err(i2c->dev, "Failed to get IRQ\n");
		return i2c->irq;
	}
	res = devm_request_irq(i2c->dev, i2c->irq, ls1024a_i2c_irq_handler, 0,
			DRV_NAME, i2c);
	if (res) {
		dev_err(i2c->dev, "Failed to request IRQ\n");
		return res;
	}

	i2c->clk = devm_clk_get(i2c->dev, NULL);
	if (IS_ERR(i2c->clk)) {
		dev_err(i2c->dev, "Failed to get reference clock\n");
		return PTR_ERR(i2c->clk);
	}
	res = clk_prepare(i2c->clk);
	if (res) {
		dev_err(i2c->dev, "Failed to prepare reference clock\n");
		return res;
	}

	i2c->reset = devm_reset_control_get_shared(i2c->dev, NULL);
	if (IS_ERR(i2c->reset)) {
		dev_err(i2c->dev, "Failed to get reset control\n");
		return PTR_ERR(i2c->reset);
	}

	res = ls1024a_i2c_hw_init(i2c);
	if (res)
		return res;

	snprintf(adap->name, sizeof(adap->name), DRV_NAME);
	adap->owner = THIS_MODULE;
	adap->dev.parent = i2c->dev;
	adap->dev.of_node = node;
	adap->timeout = 0; /* Compute timeouts at run time */
	adap->retries = 0; /* No retries by default */
	adap->algo = &ls1024a_i2c_algo;
	i2c_set_adapdata(adap, i2c);

	res = i2c_add_adapter(adap);
	if (res) {
		dev_err(i2c->dev, "Failed to add I2C adapter\n");
		goto fail_adap;
	}
	return 0;
fail_adap:
	ls1024a_i2c_hw_deinit(i2c);
	return res;
}

static int ls1024a_i2c_remove(struct platform_device *pdev)
{
	struct ls1024a_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
	ls1024a_i2c_hw_deinit(i2c);
	return 0;
}

static const struct of_device_id ls1024a_i2c_match[] = {
	{ .compatible = "fsl,ls1024a-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, ls1024a_i2c_match);

static struct platform_driver ls1024a_i2c_driver = {
	.probe		= ls1024a_i2c_probe,
	.remove		= ls1024a_i2c_remove,
	.driver		= {
		.name	= DRV_NAME,
		.of_match_table = ls1024a_i2c_match,
	},
};

module_platform_driver(ls1024a_i2c_driver);

MODULE_AUTHOR("Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>");
MODULE_DESCRIPTION("I2C-Bus adapter for the Freescale LS1024A platform");
MODULE_LICENSE("GPL v2");
