// SPDX-License-Identifier: GPL-2.0
/*
 * Freescale LS1024A pinctrl driver
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/regmap.h>
#include "../core.h"
#include "../pinctrl-utils.h"

#define DRIVER_NAME "ls1024a-pinctrl"

/* Regmap offsets */
#define GPIO_PIN_SELECT_REG	0x58
#define GPIO_PIN_SELECT_REG1	0x5c
#define GPIO_MISC_PIN_SELECT	0x60
#define GPIO_63_32_PIN_SELECT	0xdc

enum ls1024a_mux_method {
	LS1024A_MUX_METHOD_NONE = 0,
	LS1024A_MUX_METHOD_GPIO0,
	LS1024A_MUX_METHOD_GPIO1,
	LS1024A_MUX_METHOD_MISC,
};

struct ls1024a_desc_function {
	const char *name;
	unsigned int mux_value;
};

struct ls1024a_function {
	const char *name;
	const char * const *groups;
	unsigned int ngroups;
};

/**
 * @mux_method: method used to set the mux value for the pin group.
 * @mux_index: shift value for pin group in register specified by the mux
 *             method.
 *             NONE: No muxing
 *             GPIO0: GPIO number
 *             GPIO1: GPIO number - 32
 *             MISC: shift / 2
 */
struct ls1024a_group {
	const char *name;
	const unsigned int *pins;
	unsigned int npins;
	enum ls1024a_mux_method mux_method;
	unsigned int mux_index;
	struct ls1024a_desc_function *functions;
};

struct ls1024a_pinctrl {
	struct regmap *regmap;
	struct device *dev;
	struct pinctrl_dev *pctldev;
	const struct ls1024a_function *functions;
	unsigned int nfunctions;
	const struct ls1024a_group *groups;
	unsigned int ngroups;
};

#define LS1024A_PINS_SINGLE(_name, _pin) \
	static const unsigned int ls1024a_##_name##_pins[] = { _pin }

#define LS1024A_FUNCTION(_name)					\
	{							\
		.name = #_name,					\
		.groups = ls1024a_##_name##_groups,		\
		.ngroups = ARRAY_SIZE(ls1024a_##_name##_groups),\
	}

#define LS1024A_GROUP(_name, _method, _muxidx, ...)		\
	{								\
		.name = #_name "_grp",					\
		.pins = ls1024a_##_name##_pins,				\
		.npins = ARRAY_SIZE(ls1024a_##_name##_pins),		\
		.mux_method = LS1024A_MUX_METHOD_##_method,	\
		.mux_index = _muxidx,					\
		.functions = (struct ls1024a_desc_function[]){		\
			__VA_ARGS__, { } },				\
	}

#define LS1024A_FUNCTION_DESC(_name, _muxval)	\
	{					\
		.name = #_name,			\
		.mux_value = _muxval,		\
	}

static const struct pinctrl_pin_desc ls1024a_pins[] = {
	/* Muxed GPIO + function */
	PINCTRL_PIN(0, "gpio00"),
	PINCTRL_PIN(1, "gpio01"),
	PINCTRL_PIN(2, "gpio02"),
	PINCTRL_PIN(3, "gpio03"),
	PINCTRL_PIN(4, "gpio04"),
	PINCTRL_PIN(5, "gpio05"),
	PINCTRL_PIN(6, "gpio06"),
	PINCTRL_PIN(7, "gpio07"),
	PINCTRL_PIN(8, "gpio08"),
	PINCTRL_PIN(9, "gpio09"),
	PINCTRL_PIN(10, "gpio10"),
	PINCTRL_PIN(11, "gpio11"),
	PINCTRL_PIN(12, "gpio12"),
	PINCTRL_PIN(13, "gpio13"),
	PINCTRL_PIN(14, "gpio14"),
	PINCTRL_PIN(15, "gpio15"),
	PINCTRL_PIN(16, "i2c_scl"),
	PINCTRL_PIN(17, "i2c_sda"),
	PINCTRL_PIN(18, "spi_ss0_n"),
	PINCTRL_PIN(19, "spi_ss1_n"),
	PINCTRL_PIN(20, "spi_2_ss1_n"),
	PINCTRL_PIN(21, "spi_ss2_n"),
	PINCTRL_PIN(22, "spi_ss3_n"),
	PINCTRL_PIN(23, "exp_cs2_n"),
	PINCTRL_PIN(24, "exp_cs3_n"),
	PINCTRL_PIN(25, "exp_ale"),
	PINCTRL_PIN(26, "exp_rdy"),
	PINCTRL_PIN(27, "tm_ext_reset"),
	PINCTRL_PIN(28, "exp_nand_cs"),
	PINCTRL_PIN(29, "exp_nand_rdy"),
	PINCTRL_PIN(30, "spi_txd"),
	PINCTRL_PIN(31, "spi_sclk"),
	PINCTRL_PIN(32, "spi_rxd"),
	PINCTRL_PIN(33, "spi_2_rxd"),
	PINCTRL_PIN(34, "spi_2_ss0_n"),
	PINCTRL_PIN(35, "exp_dq[8]"),
	PINCTRL_PIN(36, "exp_dq[9]"),
	PINCTRL_PIN(37, "exp_dq[10]"),
	PINCTRL_PIN(38, "exp_dq[11]"),
	PINCTRL_PIN(39, "exp_dq[12]"),
	PINCTRL_PIN(40, "exp_dq[13]"),
	PINCTRL_PIN(41, "exp_dq[14]"),
	PINCTRL_PIN(42, "exp_dq[15]"),
	PINCTRL_PIN(43, "exp_dm[1]"),
	PINCTRL_PIN(44, "coresight_d0"),
	PINCTRL_PIN(45, "coresight_d1"),
	PINCTRL_PIN(46, "coresight_d2"),
	PINCTRL_PIN(47, "coresight_d3"),
	PINCTRL_PIN(48, "coresight_d4"),
	PINCTRL_PIN(49, "coresight_d5"),
	PINCTRL_PIN(50, "coresight_d6"),
	PINCTRL_PIN(51, "coresight_d7"),
	PINCTRL_PIN(52, "coresight_d8"),
	PINCTRL_PIN(53, "coresight_d9"),
	PINCTRL_PIN(54, "coresight_d10"),
	PINCTRL_PIN(55, "coresight_d11"),
	PINCTRL_PIN(56, "coresight_d12"),
	PINCTRL_PIN(57, "coresight_d13"),
	PINCTRL_PIN(58, "coresight_d14"),
	PINCTRL_PIN(59, "coresight_d15"),
	PINCTRL_PIN(60, "tdm_dx"),
	PINCTRL_PIN(61, "tdm_dr"),
	PINCTRL_PIN(62, "tdm_fs"),
	PINCTRL_PIN(63, "tdm_ck"),
	/* UART selection */
	PINCTRL_PIN(64, "uart1_rx"),
	PINCTRL_PIN(65, "uart1_tx"),
	/* GEM */
	PINCTRL_PIN(66, "gem0_rxd0"),
	PINCTRL_PIN(67, "gem0_rxd1"),
	PINCTRL_PIN(68, "gem0_rxd2"),
	PINCTRL_PIN(69, "gem0_rxd3"),
	PINCTRL_PIN(70, "gem0_rx_ctl"),
	PINCTRL_PIN(71, "gem0_rxc"),
	PINCTRL_PIN(72, "gem0_txd0"),
	PINCTRL_PIN(73, "gem0_txd1"),
	PINCTRL_PIN(74, "gem0_txd2"),
	PINCTRL_PIN(75, "gem0_txd3"),
	PINCTRL_PIN(76, "gem0_txd_ctl"),
	PINCTRL_PIN(77, "gem0_txc"),
	PINCTRL_PIN(78, "gem1_rxd0"),
	PINCTRL_PIN(79, "gem1_rxd1"),
	PINCTRL_PIN(80, "gem1_rxd2"),
	PINCTRL_PIN(81, "gem1_rxd3"),
	PINCTRL_PIN(82, "gem1_rx_ctl"),
	PINCTRL_PIN(83, "gem1_rxc"),
	PINCTRL_PIN(84, "gem1_txd0"),
	PINCTRL_PIN(85, "gem1_txd1"),
	PINCTRL_PIN(86, "gem1_txd2"),
	PINCTRL_PIN(87, "gem1_txd3"),
	PINCTRL_PIN(88, "gem1_txd_ctl"),
	PINCTRL_PIN(89, "gem1_txc"),
	PINCTRL_PIN(90, "gem2_rxd0"),
	PINCTRL_PIN(91, "gem2_rxd1"),
	PINCTRL_PIN(92, "gem2_rxd2"),
	PINCTRL_PIN(93, "gem2_rxd3"),
	PINCTRL_PIN(94, "gem2_rx_ctl"),
	PINCTRL_PIN(95, "gem2_rxc"),
	PINCTRL_PIN(96, "gem2_txd0"),
	PINCTRL_PIN(97, "gem2_txd1"),
	PINCTRL_PIN(98, "gem2_txd2"),
	PINCTRL_PIN(99, "gem2_txd3"),
	PINCTRL_PIN(100, "gem2_txd_ctl"),
	PINCTRL_PIN(101, "gem2_txc"),
	PINCTRL_PIN(102, "gem2_refclk"),
};

/* List of pins in each pin group */
LS1024A_PINS_SINGLE(gpio00, 0);
LS1024A_PINS_SINGLE(gpio01, 1);
LS1024A_PINS_SINGLE(gpio02, 2);
LS1024A_PINS_SINGLE(gpio03, 3);
LS1024A_PINS_SINGLE(gpio04, 4);
LS1024A_PINS_SINGLE(gpio05, 5);
LS1024A_PINS_SINGLE(gpio06, 6);
LS1024A_PINS_SINGLE(gpio07, 7);
LS1024A_PINS_SINGLE(gpio08, 8);
LS1024A_PINS_SINGLE(gpio09, 9);
LS1024A_PINS_SINGLE(gpio10, 10);
LS1024A_PINS_SINGLE(gpio11, 11);
LS1024A_PINS_SINGLE(gpio12, 12);
LS1024A_PINS_SINGLE(gpio13, 13);
LS1024A_PINS_SINGLE(gpio14, 14);
LS1024A_PINS_SINGLE(gpio15, 15);
LS1024A_PINS_SINGLE(i2c_scl, 16);
LS1024A_PINS_SINGLE(i2c_sda, 17);
LS1024A_PINS_SINGLE(spi_ss0, 18);
LS1024A_PINS_SINGLE(spi_ss1, 19);
LS1024A_PINS_SINGLE(spi_2_ss1, 20);
LS1024A_PINS_SINGLE(spi_ss2, 21);
LS1024A_PINS_SINGLE(spi_ss3, 22);
LS1024A_PINS_SINGLE(exp_cs2_n, 23);
LS1024A_PINS_SINGLE(exp_cs3_n, 24);
LS1024A_PINS_SINGLE(exp_ale, 25);
LS1024A_PINS_SINGLE(exp_rdy, 26);
LS1024A_PINS_SINGLE(tm_ext_reset, 27);
LS1024A_PINS_SINGLE(exp_nand_cs, 28);
LS1024A_PINS_SINGLE(exp_nand_rdy, 29);
LS1024A_PINS_SINGLE(spi_txd, 30);
LS1024A_PINS_SINGLE(spi_sclk, 31);
LS1024A_PINS_SINGLE(spi_rxd, 32);
LS1024A_PINS_SINGLE(spi_2_rxd, 33);
LS1024A_PINS_SINGLE(spi_2_ss0, 34);
LS1024A_PINS_SINGLE(exp_dq8, 35);
LS1024A_PINS_SINGLE(exp_dq9, 36);
LS1024A_PINS_SINGLE(exp_dq10, 37);
LS1024A_PINS_SINGLE(exp_dq11, 38);
LS1024A_PINS_SINGLE(exp_dq12, 39);
LS1024A_PINS_SINGLE(exp_dq13, 40);
LS1024A_PINS_SINGLE(exp_dq14, 41);
LS1024A_PINS_SINGLE(exp_dq15, 42);
LS1024A_PINS_SINGLE(exp_dm1, 43);
LS1024A_PINS_SINGLE(coresight_d0, 44);
LS1024A_PINS_SINGLE(coresight_d1, 45);
LS1024A_PINS_SINGLE(coresight_d2, 46);
LS1024A_PINS_SINGLE(coresight_d3, 47);
LS1024A_PINS_SINGLE(coresight_d4, 48);
LS1024A_PINS_SINGLE(coresight_d5, 49);
LS1024A_PINS_SINGLE(coresight_d6, 50);
LS1024A_PINS_SINGLE(coresight_d7, 51);
LS1024A_PINS_SINGLE(coresight_d8, 52);
LS1024A_PINS_SINGLE(coresight_d9, 53);
LS1024A_PINS_SINGLE(coresight_d10, 54);
LS1024A_PINS_SINGLE(coresight_d11, 55);
LS1024A_PINS_SINGLE(coresight_d12, 56);
LS1024A_PINS_SINGLE(coresight_d13, 57);
LS1024A_PINS_SINGLE(coresight_d14, 58);
LS1024A_PINS_SINGLE(coresight_d15, 59);
static const unsigned int ls1024a_tdm_pins[] = { 60, 61, 62, 63 };
static const unsigned int ls1024a_uart1_pins[] = { 64, 65 };

/* Associations between pin groups and functions */
static const char * const ls1024a_gpio_groups[] = {
	"gpio00_grp", "gpio01_grp", "gpio02_grp", "gpio03_grp",
	"gpio04_grp", "gpio05_grp", "gpio06_grp", "gpio07_grp",
	"gpio08_grp", "gpio09_grp", "gpio10_grp", "gpio11_grp",
	"gpio12_grp", "gpio13_grp", "gpio14_grp", "gpio15_grp",
	"i2c_scl_grp", "i2c_sda_grp", "spi_ss0_n_grp", "spi_ss1_n_grp",
	"spi_2_ss1_n_grp", "spi_ss2_n_grp", "spi_ss3_n_grp", "exp_cs2_n_grp",
	"exp_cs3_n_grp", "exp_ale_grp", "exp_rdy_grp", "tm_ext_reset_grp",
	"exp_nand_cs_grp", "exp_nand_rdy_grp", "spi_txd_grp", "spi_sclk_grp",
	"spi_rxd_grp", "spi_2_rxd_grp", "spi_2_ss0_n_grp", "exp_dq8_grp",
	"exp_dq9_grp", "exp_dq10_grp", "exp_dq11_grp", "exp_dq12_grp",
	"exp_dq13_grp", "exp_dq14_grp", "exp_dq15_grp", "exp_dm1_grp",
	"coresight_d0_grp", "coresight_d1_grp",
	"coresight_d2_grp", "coresight_d3_grp",
	"coresight_d4_grp", "coresight_d5_grp",
	"coresight_d6_grp", "coresight_d7_grp",
	"coresight_d8_grp", "coresight_d9_grp",
	"coresight_d10_grp", "coresight_d11_grp",
	"coresight_d12_grp", "coresight_d13_grp",
	"coresight_d14_grp", "coresight_d15_grp",
	"tdm_grp"
};
static const char * const ls1024a_pwm_groups[] = {
	"gpio04_grp", "gpio05_grp", "gpio06_grp", "gpio07_grp",
	"gpio12_grp", "gpio13_grp"
};
static const char * const ls1024a_sata_groups[] = {
	"gpio06_grp", "gpio07_grp", "gpio14_grp", "gpio15_grp"
};
static const char * const ls1024a_i2c_groups[] = {
	"i2c_scl_grp", "i2c_sda_grp"
};
static const char * const ls1024a_spi_groups[] = {
	"spi_sclk_grp", "spi_rxd_grp", "spi_txd_grp",
	"spi_ss0_grp", "spi_ss1_grp", "spi_ss2_grp", "spi_ss3_grp",
	"spi_2_ss1_grp", "spi_2_rxd_grp", "spi_2_ss0_grp", "spi_2_ss1_grp"
};
static const char * const ls1024a_exp_groups[] = {
	"exp_cs2_n_grp", "exp_cs3_n_grp", "exp_ale_grp", "exp_rdy_grp",
	"exp_nand_cs_grp", "exp_nand_rdy_grp", "exp_dq8_grp", "exp_dq9_grp",
	"exp_dq10_grp", "exp_dq11_grp", "exp_dq12_grp", "exp_dq13_grp",
	"exp_dq14_grp", "exp_dq15_grp", "exp_dm1_grp"
};
static const char * const ls1024a_tm_ext_reset_groups[] = {
	"tm_ext_reset_grp"
};
static const char * const ls1024a_coresight_groups[] = {
	"coresight_d0_grp", "coresight_d1_grp",
	"coresight_d2_grp", "coresight_d3_grp",
	"coresight_d4_grp", "coresight_d5_grp",
	"coresight_d6_grp", "coresight_d7_grp",
	"coresight_d8_grp", "coresight_d9_grp",
	"coresight_d10_grp", "coresight_d11_grp",
	"coresight_d12_grp", "coresight_d13_grp",
	"coresight_d14_grp", "coresight_d15_grp",
};
static const char * const ls1024a_tdm_groups[] = { "tdm_grp" };
static const char * const ls1024a_zds_groups[] = { "tdm_grp" };
static const char * const ls1024a_msif_groups[] = { "tdm_grp" };
static const char * const ls1024a_uart0_groups[] = {
	"gpio08_grp", "gpio09_grp", "gpio10_grp", "gpio11_grp"
};
static const char * const ls1024a_uart1_groups[] = { "uart1_grp" };
static const char * const ls1024a_uart2_groups[] = { "uart1_grp" };
static const char * const ls1024a_uart_pfe_groups[] = {
	"gpio12_grp", "gpio13_grp"
};

/* List of groups and the associated mux value/method */
static const struct ls1024a_group ls1024a_groups[] = {
	LS1024A_GROUP(gpio00, GPIO0, 0,
			LS1024A_FUNCTION_DESC(gpio, 0)),
	LS1024A_GROUP(gpio01, GPIO0, 1,
			LS1024A_FUNCTION_DESC(gpio, 0)),
	LS1024A_GROUP(gpio02, GPIO0, 2,
			LS1024A_FUNCTION_DESC(gpio, 0)),
	LS1024A_GROUP(gpio03, GPIO0, 3,
			LS1024A_FUNCTION_DESC(gpio, 0)),
	LS1024A_GROUP(gpio04, GPIO0, 4,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(pwm, 1)),
	LS1024A_GROUP(gpio05, GPIO0, 5,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(pwm, 1)),
	LS1024A_GROUP(gpio06, GPIO0, 6,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(pwm, 1),
			LS1024A_FUNCTION_DESC(sata, 2)),
	LS1024A_GROUP(gpio07, GPIO0, 7,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(pwm, 1),
			LS1024A_FUNCTION_DESC(sata, 2)),
	LS1024A_GROUP(gpio08, GPIO0, 8,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(uart0, 2)),
	LS1024A_GROUP(gpio09, GPIO0, 9,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(uart0, 2)),
	LS1024A_GROUP(gpio10, GPIO0, 10,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(uart0, 2)),
	LS1024A_GROUP(gpio11, GPIO0, 11,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(uart0, 2)),
	LS1024A_GROUP(gpio12, GPIO0, 12,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(pwm, 1),
			LS1024A_FUNCTION_DESC(uart_pfe, 2)),
	LS1024A_GROUP(gpio13, GPIO0, 13,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(pwm, 1),
			LS1024A_FUNCTION_DESC(uart_pfe, 2)),
	LS1024A_GROUP(gpio14, GPIO0, 14,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(sata, 1)),
	LS1024A_GROUP(gpio15, GPIO0, 15,
			LS1024A_FUNCTION_DESC(gpio, 0),
			LS1024A_FUNCTION_DESC(sata, 1)),
	LS1024A_GROUP(i2c_scl, GPIO0, 16,
			LS1024A_FUNCTION_DESC(i2c, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(i2c_sda, GPIO0, 17,
			LS1024A_FUNCTION_DESC(i2c, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_ss0, GPIO0, 18,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_ss1, GPIO0, 19,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_2_ss1, GPIO0, 20,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_ss2, GPIO0, 21,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_ss3, GPIO0, 22,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_cs2_n, GPIO0, 23,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_cs3_n, GPIO0, 24,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_ale, GPIO0, 25,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_rdy, GPIO0, 26,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(tm_ext_reset, GPIO0, 27,
			LS1024A_FUNCTION_DESC(tm_ext_reset, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_nand_cs, GPIO0, 28,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_nand_rdy, GPIO0, 29,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_txd, GPIO0, 30,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_sclk, GPIO0, 31,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_rxd, GPIO1, 0,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_2_rxd, GPIO1, 1,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(spi_2_ss0, GPIO1, 2,
			LS1024A_FUNCTION_DESC(spi, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq8, GPIO1, 3,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq9, GPIO1, 4,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq10, GPIO1, 5,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq11, GPIO1, 6,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq12, GPIO1, 7,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq13, GPIO1, 8,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq14, GPIO1, 9,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dq15, GPIO1, 10,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(exp_dm1, GPIO1, 11,
			LS1024A_FUNCTION_DESC(exp, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d0, GPIO1, 12,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d1, GPIO1, 13,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d2, GPIO1, 14,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d3, GPIO1, 15,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d4, GPIO1, 16,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d5, GPIO1, 17,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d6, GPIO1, 18,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d7, GPIO1, 19,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d8, GPIO1, 20,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d9, GPIO1, 21,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d10, GPIO1, 22,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d11, GPIO1, 23,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d12, GPIO1, 24,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d13, GPIO1, 25,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d14, GPIO1, 26,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(coresight_d15, GPIO1, 27,
			LS1024A_FUNCTION_DESC(coresight, 0),
			LS1024A_FUNCTION_DESC(gpio, 1)),
	LS1024A_GROUP(tdm, MISC, 2,
			LS1024A_FUNCTION_DESC(tdm, 0),
			LS1024A_FUNCTION_DESC(zds, 1),
			LS1024A_FUNCTION_DESC(gpio, 2),
			LS1024A_FUNCTION_DESC(msif, 3)),
	LS1024A_GROUP(uart1, MISC, 0,
			LS1024A_FUNCTION_DESC(uart1, 0),
			LS1024A_FUNCTION_DESC(uart2, 1)),
};

struct ls1024a_function ls1024a_functions[] = {
	LS1024A_FUNCTION(gpio),
	LS1024A_FUNCTION(pwm),
	LS1024A_FUNCTION(sata),
	LS1024A_FUNCTION(i2c),
	LS1024A_FUNCTION(spi),
	LS1024A_FUNCTION(exp),
	LS1024A_FUNCTION(tm_ext_reset),
	LS1024A_FUNCTION(coresight),
	LS1024A_FUNCTION(tdm),
	LS1024A_FUNCTION(zds),
	LS1024A_FUNCTION(msif),
	LS1024A_FUNCTION(uart0),
	LS1024A_FUNCTION(uart1),
	LS1024A_FUNCTION(uart2),
	LS1024A_FUNCTION(uart_pfe),
};

/* Tell whether pin is part of group pg */
static bool ls1024a_pmx_group_has_pin(const struct ls1024a_group *pg,
                                      unsigned int pin)
{
	unsigned int i;
	for (i = 0; i < pg->npins; i++) {
		if (pg->pins[i] == pin)
			return true;
	}
	return false;
}

/* Get pin group and gpio function descriptor for the specifed pin. */
static int ls1024a_pmx_get_gpio_mux_by_pin(
		unsigned pin,
		const struct ls1024a_group **pg,
		const struct ls1024a_desc_function **func_desc)
{
	size_t ngroups = ARRAY_SIZE(ls1024a_groups);
	size_t grpidx;
	for (grpidx = 0; grpidx < ngroups; grpidx++) {
		const struct ls1024a_group *grp = &ls1024a_groups[grpidx];
		const struct ls1024a_desc_function *func = grp->functions;

		if (!ls1024a_pmx_group_has_pin(grp, pin))
			continue;

		while(func) {
			if (strcmp("gpio", func->name) == 0) {
				*pg = grp;
				*func_desc = func;
				return 0;
			}
			func++;
		}
	}
	return -ENOENT;
}

static int ls1024a_pmx_set_group_mux(struct ls1024a_pinctrl *pctl,
                                     const struct ls1024a_group *pg,
                                     const struct ls1024a_desc_function *func)
{
	unsigned int res;
	unsigned int reg;
	unsigned int val;
	unsigned int mask;
	unsigned int mux_idx;

	mux_idx = pg->mux_index;
	switch(pg->mux_method) {
	case LS1024A_MUX_METHOD_GPIO0:
		if (mux_idx < 16) {
			reg = GPIO_PIN_SELECT_REG;
			val = func->mux_value << (mux_idx * 2);
			mask = 0x3 << (mux_idx * 2);
		} else {
			reg = GPIO_PIN_SELECT_REG1;
			val = func->mux_value << ((mux_idx - 16) * 2);
			mask = 0x3 << ((mux_idx - 16) * 2);
		}
		break;
	case LS1024A_MUX_METHOD_GPIO1:
		reg = GPIO_63_32_PIN_SELECT;
		val = func->mux_value << mux_idx;
		mask = 0x1 << mux_idx;
		break;
	case LS1024A_MUX_METHOD_MISC:
		reg = GPIO_MISC_PIN_SELECT;
		val = func->mux_value << (2 * mux_idx);
		mask = 0x3 << (2 * mux_idx);
		break;
	default:
		dev_err(pctl->dev, "no mux method for group %s\n", pg->name);
		return -EINVAL;
	}
	res = regmap_update_bits(pctl->regmap, reg, mask, val);
	if (res) {
		dev_err(pctl->dev, "failed to update mux setting (%d) "
			"reg=0x%x val=0x%x mask=0x%x\n",
			res, reg, val, mask);
		return res;
	}
	dev_dbg(pctl->dev, "mux %s set for group %s\n", func->name, pg->name);
	return 0;
}

static int ls1024a_pmx_get_functions_count(struct pinctrl_dev *pctldev)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	return pctl->nfunctions;
}

static const char * ls1024a_pmx_get_function_name(struct pinctrl_dev *pctldev,
                                                  unsigned selector)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	return pctl->functions[selector].name;
}

static int ls1024a_pmx_get_function_groups(struct pinctrl_dev *pctldev,
                                           unsigned selector,
                                           const char * const **groups,
                                           unsigned *num_groups)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	*groups = pctl->functions[selector].groups;
	*num_groups = pctl->functions[selector].ngroups;

	return 0;
}

static int ls1024a_pmx_set_mux(struct pinctrl_dev *pctldev,
                               unsigned func_selector,
                               unsigned group_selector)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	const struct ls1024a_group *pg = &pctl->groups[group_selector];
	const struct ls1024a_function *pf = &pctl->functions[func_selector];
	const char *fname = pf->name;
	const struct ls1024a_desc_function *functions = pg->functions;

	while(functions->name) {
		if (strcmp(functions->name, fname)) {
			functions++;
			continue;
		}
		return ls1024a_pmx_set_group_mux(pctl, pg, functions);
	}
	return -EINVAL;
}

static int ls1024a_pmx_gpio_set_direction(struct pinctrl_dev *pctldev,
                                          struct pinctrl_gpio_range *range,
                                          unsigned offset,
                                          bool input)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	const struct ls1024a_group *pg = NULL;
	const struct ls1024a_desc_function *func = NULL;
	unsigned int pin = range->pin_base + offset;
	int res;

	res = ls1024a_pmx_get_gpio_mux_by_pin(pin, &pg, &func);
	if (WARN_ON(res))
		return res;

	return ls1024a_pmx_set_group_mux(pctl, pg, func);
}

static const struct pinmux_ops ls1024a_pmx_ops = {
	.get_functions_count = ls1024a_pmx_get_functions_count,
	.get_function_name = ls1024a_pmx_get_function_name,
	.get_function_groups = ls1024a_pmx_get_function_groups,
	.set_mux = ls1024a_pmx_set_mux,
	.gpio_set_direction = ls1024a_pmx_gpio_set_direction,
	.strict = true,
};

static int ls1024a_pctl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	return pctl->ngroups;
}

static const char * ls1024a_pctl_get_group_name(struct pinctrl_dev *pctldev,
                                                unsigned selector)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	return pctl->groups[selector].name;
}

static int ls1024a_pctl_get_group_pins(struct pinctrl_dev *pctldev,
                                       unsigned int group,
                                       const unsigned int **pins,
                                       unsigned int *num_pins)
{
	struct ls1024a_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	*pins = pctl->groups[group].pins;
	*num_pins = pctl->groups[group].npins;

	return 0;
}

static void ls1024a_pctl_pin_dbg_show(struct pinctrl_dev *pctldev,
                                      struct seq_file *s, unsigned offset)
{
	seq_puts(s, " " DRIVER_NAME);
}

static const struct pinctrl_ops ls1024a_pctl_ops = {
	.get_groups_count = ls1024a_pctl_get_groups_count,
	.get_group_name = ls1024a_pctl_get_group_name,
	.get_group_pins = ls1024a_pctl_get_group_pins,
	.pin_dbg_show = ls1024a_pctl_pin_dbg_show,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_group,
	.dt_free_map = pinctrl_utils_free_map,
};

static struct pinctrl_desc ls1024a_desc = {
	.name = DRIVER_NAME,
	.pins = ls1024a_pins,
	.npins = ARRAY_SIZE(ls1024a_pins),
	.pctlops = &ls1024a_pctl_ops,
	.pmxops = &ls1024a_pmx_ops,
	.owner = THIS_MODULE,
};

static int ls1024a_pinctrl_probe(struct platform_device *pdev)
{
	int res;
	struct ls1024a_pinctrl *pctl;
	struct device_node *parent_np;

	if (!pdev->dev.of_node)
		return -EINVAL;

	pctl = devm_kzalloc(&pdev->dev, sizeof(*pctl), GFP_KERNEL);
	if (!pctl)
		return -ENOMEM;
	pctl->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, pctl);

	parent_np = of_get_parent(pdev->dev.of_node);
	pctl->regmap = syscon_node_to_regmap(parent_np);
	of_node_put(parent_np);
	if (IS_ERR(pctl->regmap)) {
		dev_err(&pdev->dev, "failed to get gpio regmap\n");
		return -ENODEV;
	}

	pctl->functions = ls1024a_functions;
	pctl->nfunctions = ARRAY_SIZE(ls1024a_functions);
	pctl->groups = ls1024a_groups;
	pctl->ngroups = ARRAY_SIZE(ls1024a_groups);

	res = devm_pinctrl_register_and_init(&pdev->dev, &ls1024a_desc,
	                                pctl, &pctl->pctldev);
	if (res)
		return res;

	return pinctrl_enable(pctl->pctldev);
}

static const struct of_device_id ls1024a_pinctrl_match[] = {
	{ .compatible = "fsl,ls1024a-pinctrl" },
	{},
};

static struct platform_driver ls1024a_pinctrl_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ls1024a_pinctrl_match,
		.suppress_bind_attrs = true,
	},
	.probe = ls1024a_pinctrl_probe,
};

static int __init ls1024a_pinctrl_init(void)
{
	return platform_driver_register(&ls1024a_pinctrl_driver);
}
arch_initcall(ls1024a_pinctrl_init);
