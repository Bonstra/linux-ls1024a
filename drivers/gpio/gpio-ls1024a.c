// SPDX-License-Identifier: GPL-2.0
/*
 * Freescale LS1024A GPIO driver
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/gpio.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "gpiolib.h"

/* GPIO 0~31 registers */
#define GPIO_OUTPUT_REG			0x0000
#define GPIO_OE_REG			0x0004
#define GPIO_INT_CFG_REG		0x0008
#define GPIO_INPUT_REG			0x0010

/* GPIO 32~63 registers */
#define GPIO_63_32_PIN_OUTPUT		0x00d0
#define GPIO_63_32_PIN_OUTPUT_EN	0x00d4
#define GPIO_63_32_PIN_INPUT		0x00d8

struct ls1024a_gpiochip {
	struct gpio_chip chip;
	struct regmap *regmap;
};

static const struct of_device_id ls1024a_gpio_of_match[] = {
	{
		.compatible = "fsl,ls1024a-gpio",
	},
	{}
};


static int ls1024a_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
	struct ls1024a_gpiochip *lschip = gpiochip_get_data(chip);
	u32 val;
	int retval;

	if (offset < 32) {
		regmap_read(lschip->regmap, GPIO_OE_REG, &val);
		retval = 1;
		if (val & BIT(offset))
			retval = 0;
	} else {
		regmap_read(lschip->regmap, GPIO_63_32_PIN_OUTPUT_EN, &val);
		retval = 0;
		if (val & BIT(offset - 32))
			retval = 1;
	}
	return retval;
}

static int ls1024a_gpio_direction_input(struct gpio_chip *chip,
                                        unsigned offset)
{
	struct ls1024a_gpiochip *lschip = gpiochip_get_data(chip);
	int res;

	res = pinctrl_gpio_direction_input(chip->base + offset);
	if (res)
		return res;

	if (offset < 32) {
		regmap_update_bits(lschip->regmap, GPIO_OE_REG,
		                   BIT(offset), 0);
	} else {
		regmap_update_bits(lschip->regmap, GPIO_63_32_PIN_OUTPUT_EN,
		                   BIT(offset - 32), BIT(offset - 32));
	}
	return 0;
}

static int ls1024a_gpio_direction_output(struct gpio_chip *chip,
                                         unsigned offset, int value)
{
	struct ls1024a_gpiochip *lschip = gpiochip_get_data(chip);
	int res;

	res = pinctrl_gpio_direction_output(chip->base + offset);
	if (res)
		return res;

	if (offset < 32) {
		u32 mask = BIT(offset);
		regmap_update_bits(lschip->regmap, GPIO_OUTPUT_REG,
		                   mask, value ? mask : 0);
		regmap_update_bits(lschip->regmap, GPIO_OE_REG,
		                   mask, mask);
	} else {
		u32 mask = BIT(offset - 32);
		regmap_update_bits(lschip->regmap, GPIO_63_32_PIN_OUTPUT,
		                   mask, value ? mask : 0);
		regmap_update_bits(lschip->regmap, GPIO_63_32_PIN_OUTPUT_EN,
		                   mask, 0);
	}
	return 0;
}

static int ls1024a_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct ls1024a_gpiochip *lschip = gpiochip_get_data(chip);
	u32 val;
	unsigned reg;
	u32 mask;

	if (offset < 32) {
		reg = GPIO_INPUT_REG;
		mask = BIT(offset);
	} else {
		reg = GPIO_63_32_PIN_INPUT;
		mask = BIT(offset -32);
	}
	regmap_read(lschip->regmap, reg, &val);
	return (val & mask) ? 1 : 0;
}

static void ls1024a_gpio_set(struct gpio_chip *chip,
                             unsigned offset,
                             int value)
{
	struct ls1024a_gpiochip *lschip = gpiochip_get_data(chip);
	unsigned reg;
	u32 mask;

	if (offset < 32) {
		reg = GPIO_OUTPUT_REG;
		mask = BIT(offset);
	} else {
		reg = GPIO_63_32_PIN_OUTPUT;
		mask = BIT(offset - 32);
	}
	regmap_update_bits(lschip->regmap, reg, mask, value ? mask : 0);
}

static int ls1024a_gpio_probe(struct platform_device *pdev)
{
	struct ls1024a_gpiochip *chip;
	int res;

	if (!of_match_device(ls1024a_gpio_of_match, &pdev->dev))
		return -ENODEV;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	platform_set_drvdata(pdev, chip);

	chip->regmap = syscon_node_to_regmap(pdev->dev.of_node);
	if (IS_ERR(chip->regmap))
		return PTR_ERR(chip->regmap);

	chip->chip.label = dev_name(&pdev->dev);
	chip->chip.parent = &pdev->dev;
	chip->chip.request = gpiochip_generic_request;
	chip->chip.free = gpiochip_generic_free;
	chip->chip.get_direction = ls1024a_gpio_get_direction;
	chip->chip.direction_input = ls1024a_gpio_direction_input;
	chip->chip.direction_output = ls1024a_gpio_direction_output;
	chip->chip.get = ls1024a_gpio_get;
	chip->chip.set = ls1024a_gpio_set;
	chip->chip.base = 0;
	chip->chip.ngpio = 64;
	chip->chip.can_sleep = false;
	chip->chip.of_node = pdev->dev.of_node;

	res = devm_gpiochip_add_data(&pdev->dev, &chip->chip, chip);
	if (res)
		return res;

	return 0;
}

static struct platform_driver ls1024a_gpio_driver = {
	.driver = {
		.name = "ls1024a-gpio",
		.of_match_table = ls1024a_gpio_of_match,
	},
	.probe = ls1024a_gpio_probe,
};
builtin_platform_driver(ls1024a_gpio_driver);
