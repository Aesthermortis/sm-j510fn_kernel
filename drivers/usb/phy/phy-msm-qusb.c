/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk/msm-clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/phy.h>

#define QUSB2PHY_PORT_POWERDOWN		0xB4
#define CLAMP_N_EN			BIT(5)
#define FREEZIO_N			BIT(1)
#define POWER_DOWN			BIT(0)
#define QUSB2PHY_PORT_UTMI_CTRL2	0xC4
#define QUSB2PHY_PORT_TUNE1             0x80
#define QUSB2PHY_PORT_TUNE2             0x84
#define QUSB2PHY_PORT_TUNE3             0x88
#define QUSB2PHY_PORT_TUNE4             0x8C

#define UTMI_OTG_VBUS_VALID             BIT(20)
#define SW_SESSVLD_SEL                  BIT(28)

#define QRBTC_USB2_PLL		0x404
#define QRBTC_USB2_PLLCTRL2	0x414
#define QRBTC_USB2_PLLCTRL1	0x410
#define QRBTC_USB2_PLLCTRL3	0x418
#define QRBTC_USB2_PLLTEST1	0x408
#define RUMI_RESET_ADDRESS	0x6500
#define RUMI_RESET_VALUE_1	0x80000000
#define RUMI_RESET_VALUE_2	0x000201e0

#define PORT_OFFSET(i) ((i == 0) ? 0x0 : ((i == 1) ? 0x6c : 0x88))
#define HS_PHY_CTRL_REG(i)              (0x10 + PORT_OFFSET(i))

struct qusb_phy {
	struct usb_phy		phy;
	void __iomem		*base;
	void __iomem		*qscratch_base;

	struct clk		*ref_clk;
	struct clk		*cfg_ahb_clk;
	struct clk		*phy_reset;

	struct regulator	*vdd;
	struct regulator	*vdda33;
	struct regulator	*vdda18;
	int			vdd_levels[3]; /* none, low, high */

	bool			power_enabled;
	bool			clocks_enabled;
	bool			cable_connected;
	bool			suspended;
	bool			emulation;
	bool			ulpi_mode;
};

static int qusb_phy_reset(struct usb_phy *phy)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	dev_dbg(phy->dev, "%s\n", __func__);

	clk_reset(qphy->phy_reset, CLK_RESET_ASSERT);
	usleep(100);
	clk_reset(qphy->phy_reset, CLK_RESET_DEASSERT);

	return 0;
}

static int qusb_phy_enable_power(struct qusb_phy *qphy, bool on)
{
	int ret = 0;

	dev_dbg(qphy->phy.dev, "%s turn %s regulators\n", __func__,
			on ? "on" : "off");

	if (qphy->power_enabled == on)
		return 0;

	if (!on)
		goto disable_vdda33;

	ret = regulator_enable(qphy->vdd);
	if (ret) {
		dev_err(qphy->phy.dev, "unable to enable vdd\n");
		return ret;
	}

	ret = regulator_enable(qphy->vdda18);
	if (ret) {
		dev_err(qphy->phy.dev, "unable to enable vdda18\n");
		goto disable_vdd;
	}

	ret = regulator_enable(qphy->vdda33);
	if (ret) {
		dev_err(qphy->phy.dev, "unable to enable vdda33\n");
		goto disable_vdda18;
	}

	qphy->power_enabled = true;

	return 0;

disable_vdda33:
	regulator_disable(qphy->vdda33);
disable_vdda18:
	regulator_disable(qphy->vdda18);
disable_vdd:
	regulator_disable(qphy->vdd);

	qphy->power_enabled = false;
	return ret;
}

static int qusb_phy_init(struct usb_phy *phy)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	dev_dbg(phy->dev, "%s\n", __func__);

	if (!qphy->clocks_enabled) {
		clk_prepare_enable(qphy->ref_clk);
		clk_prepare_enable(qphy->cfg_ahb_clk);
		qphy->clocks_enabled = true;
	}

	if (qphy->emulation) {
		/* Configure QUSB2 PLLs for RUMI */
		writel_relaxed(0x19, qphy->base + QRBTC_USB2_PLL);
		writel_relaxed(0x20, qphy->base + QRBTC_USB2_PLLCTRL2);
		writel_relaxed(0x79, qphy->base + QRBTC_USB2_PLLCTRL1);
		writel_relaxed(0x00, qphy->base + QRBTC_USB2_PLLCTRL3);
		writel_relaxed(0x99, qphy->base + QRBTC_USB2_PLL);
		writel_relaxed(0x04, qphy->base + QRBTC_USB2_PLLTEST1);
		writel_relaxed(0xD9, qphy->base + QRBTC_USB2_PLL);

		/* Wait for 5ms as per QUSB2 RUMI sequence from VI */
		usleep(5000);

		/* Perform the RUMI PLL Reset */
		writel_relaxed((int)RUMI_RESET_VALUE_1,
					qphy->base + RUMI_RESET_ADDRESS);
		/* Wait for 10ms as per QUSB2 RUMI sequence from VI */
		usleep(10000);
		writel_relaxed(0x0, qphy->base + RUMI_RESET_ADDRESS);
		/* Wait for 10ms as per QUSB2 RUMI sequence from VI */
		usleep(10000);
		writel_relaxed((int)RUMI_RESET_VALUE_2,
					qphy->base + RUMI_RESET_ADDRESS);
		/* Wait for 10ms as per QUSB2 RUMI sequence from VI */
		usleep(10000);
		writel_relaxed(0x0, qphy->base + RUMI_RESET_ADDRESS);
	} else {
		/* Disable the PHY */
		writel_relaxed(CLAMP_N_EN | FREEZIO_N | POWER_DOWN,
				qphy->base + QUSB2PHY_PORT_POWERDOWN);

		/* configure for ULPI mode if requested */
		if (qphy->ulpi_mode)
			writel_relaxed(0x0,
					qphy->base + QUSB2PHY_PORT_UTMI_CTRL2);

		/* Program tuning parameters for PHY */
		writel_relaxed(0xA0, qphy->base + QUSB2PHY_PORT_TUNE1);
		writel_relaxed(0xA5, qphy->base + QUSB2PHY_PORT_TUNE2);
		writel_relaxed(0x81, qphy->base + QUSB2PHY_PORT_TUNE3);
		writel_relaxed(0x85, qphy->base + QUSB2PHY_PORT_TUNE4);

		/* ensure above writes are completed before re-enabling PHY */
		wmb();

		/* Enable the PHY */
		writel_relaxed(CLAMP_N_EN | FREEZIO_N,
				qphy->base + QUSB2PHY_PORT_POWERDOWN);
	}

	return 0;
}

static void qusb_phy_shutdown(struct usb_phy *phy)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	dev_dbg(phy->dev, "%s\n", __func__);

	/* clocks need to be on to access register */
	if (!qphy->clocks_enabled) {
		clk_prepare_enable(qphy->ref_clk);
		clk_prepare_enable(qphy->cfg_ahb_clk);
		qphy->clocks_enabled = true;
	}

	/* Disable the PHY */
	writel_relaxed(CLAMP_N_EN | FREEZIO_N | POWER_DOWN,
			qphy->base + QUSB2PHY_PORT_POWERDOWN);
	wmb();

	clk_disable_unprepare(qphy->cfg_ahb_clk);
	clk_disable_unprepare(qphy->ref_clk);
	qphy->clocks_enabled = false;
}

static void qusb_write_readback(void *base, u32 offset,
					const u32 mask, u32 val)
{
	u32 write_val, tmp = readl_relaxed(base + offset);
	tmp &= ~mask; /* retain other bits */
	write_val = tmp | val;

	writel_relaxed(write_val, base + offset);

	/* Read back to see if val was written */
	tmp = readl_relaxed(base + offset);
	tmp &= mask; /* clear other bits */

	if (tmp != val)
		pr_err("%s: write: %x to QSCRATCH: %x FAILED\n",
			__func__, val, offset);
}

/**
 * Performs QUSB2 PHY suspend/resume functionality.
 *
 * @uphy - usb phy pointer.
 * @suspend - to enable suspend or not. 1 - suspend, 0 - resume
 *
 */
static int qusb_phy_set_suspend(struct usb_phy *phy, int suspend)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	if (!qphy->clocks_enabled) {
		dev_dbg(phy->dev, "clocks not enabled yet\n");
		return -EAGAIN;
	}

	if (qphy->suspended && suspend) {
		dev_dbg(phy->dev, "%s: USB PHY is already suspended\n",
			__func__);
		return 0;
	}

	if (suspend) {
		/* Suspend case */
		if (qphy->cable_connected) {
			return -EAGAIN;
		} else { /* Disconnect case */
			/* Disable the PHY */
			writel_relaxed(CLAMP_N_EN | FREEZIO_N | POWER_DOWN,
				qphy->base + QUSB2PHY_PORT_POWERDOWN);
			clk_disable_unprepare(qphy->cfg_ahb_clk);
			clk_disable_unprepare(qphy->ref_clk);
			qusb_phy_enable_power(qphy, false);
		}
		qphy->suspended = true;
	} else {
		qusb_phy_enable_power(qphy, true);
		clk_prepare_enable(qphy->ref_clk);
		clk_prepare_enable(qphy->cfg_ahb_clk);
		/* Enable the PHY */
		writel_relaxed(CLAMP_N_EN | FREEZIO_N,
			qphy->base + QUSB2PHY_PORT_POWERDOWN);
		/* Caller needs to call qusb_phy_init to apply tuning params */
		qphy->suspended = false;
	}

	return 0;
}

static int qusb_phy_notify_connect(struct usb_phy *phy,
					enum usb_device_speed speed)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	qphy->cable_connected = true;

	dev_dbg(phy->dev, " cable_connected=%d\n", qphy->cable_connected);

	/* Set OTG VBUS Valid from HSPHY to controller */
	qusb_write_readback(qphy->qscratch_base, HS_PHY_CTRL_REG(0),
				UTMI_OTG_VBUS_VALID,
				UTMI_OTG_VBUS_VALID);

	/* Indicate value is driven by UTMI_OTG_VBUS_VALID bit */
	qusb_write_readback(qphy->qscratch_base, HS_PHY_CTRL_REG(0),
				SW_SESSVLD_SEL, SW_SESSVLD_SEL);

	dev_dbg(phy->dev, "QUSB2 phy connect notification\n");
	return 0;
}

static int qusb_phy_notify_disconnect(struct usb_phy *phy,
					enum usb_device_speed speed)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	qphy->cable_connected = false;

	dev_dbg(phy->dev, " cable_connected=%d\n", qphy->cable_connected);

	/* Set OTG VBUS Valid from HSPHY to controller */
	qusb_write_readback(qphy->qscratch_base, HS_PHY_CTRL_REG(0),
				UTMI_OTG_VBUS_VALID, 0);

	/* Indicate value is driven by UTMI_OTG_VBUS_VALID bit */
	qusb_write_readback(qphy->qscratch_base, HS_PHY_CTRL_REG(0),
				SW_SESSVLD_SEL, 0);

	dev_dbg(phy->dev, "QUSB2 phy disconnect notification\n");
	return 0;
}

static int qusb_phy_probe(struct platform_device *pdev)
{
	struct qusb_phy *qphy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret = 0;
	const char *phy_type;

	qphy = devm_kzalloc(dev, sizeof(*qphy), GFP_KERNEL);
	if (!qphy)
		return -ENOMEM;

	qphy->phy.dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qusb_phy_base");
	qphy->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(qphy->base))
		return PTR_ERR(qphy->base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qscratch_base");
	qphy->qscratch_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(qphy->qscratch_base))
		qphy->qscratch_base = NULL;

	qphy->ref_clk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(qphy->ref_clk))
		return PTR_ERR(qphy->ref_clk);
	clk_set_rate(qphy->ref_clk, 19200000);

	qphy->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb_clk");
	if (IS_ERR(qphy->cfg_ahb_clk))
		return PTR_ERR(qphy->cfg_ahb_clk);

	qphy->phy_reset = devm_clk_get(dev, "phy_reset");
	if (IS_ERR(qphy->phy_reset))
		return PTR_ERR(qphy->phy_reset);

	qphy->emulation = of_property_read_bool(dev->of_node,
						"qcom,emulation");

	qphy->ulpi_mode = false;
	ret = of_property_read_string(dev->of_node, "phy_type", &phy_type);

	if (!ret) {
		if (!strcasecmp(phy_type, "ulpi"))
			qphy->ulpi_mode = true;
	} else {
		dev_err(dev, "error reading phy_type property\n");
		return ret;
	}

	ret = of_property_read_u32_array(dev->of_node, "qcom,vdd-voltage-level",
					 (u32 *) qphy->vdd_levels,
					 ARRAY_SIZE(qphy->vdd_levels));
	if (ret) {
		dev_err(dev, "error reading qcom,vdd-voltage-level property\n");
		return ret;
	}

	qphy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(qphy->vdd)) {
		dev_err(dev, "unable to get vdd supply\n");
		return PTR_ERR(qphy->vdd);
	}

	qphy->vdda33 = devm_regulator_get(dev, "vdda33");
	if (IS_ERR(qphy->vdda33)) {
		dev_err(dev, "unable to get vdda33 supply\n");
		return PTR_ERR(qphy->vdda33);
	}

	qphy->vdda18 = devm_regulator_get(dev, "vdda18");
	if (IS_ERR(qphy->vdda18)) {
		dev_err(dev, "unable to get vdda18 supply\n");
		return PTR_ERR(qphy->vdda18);
	}

	ret = qusb_phy_enable_power(qphy, true);
	if (ret)
		return ret;

	clk_prepare_enable(qphy->ref_clk);
	clk_prepare_enable(qphy->cfg_ahb_clk);
	qphy->clocks_enabled = true;

	platform_set_drvdata(pdev, qphy);

	qphy->phy.label			= "msm-qusb-phy";
	qphy->phy.init			= qusb_phy_init;
	qphy->phy.set_suspend           = qusb_phy_set_suspend;
	qphy->phy.shutdown		= qusb_phy_shutdown;
	qphy->phy.reset			= qusb_phy_reset;
	qphy->phy.type			= USB_PHY_TYPE_USB2;

	if (qphy->qscratch_base) {
		qphy->phy.notify_connect        = qusb_phy_notify_connect;
		qphy->phy.notify_disconnect     = qusb_phy_notify_disconnect;
	}

	qusb_phy_reset(&qphy->phy);
	ret = usb_add_phy_dev(&qphy->phy);

	return ret;
}

static int qusb_phy_remove(struct platform_device *pdev)
{
	struct qusb_phy *qphy = platform_get_drvdata(pdev);

	usb_remove_phy(&qphy->phy);

	if (qphy->clocks_enabled) {
		clk_disable_unprepare(qphy->cfg_ahb_clk);
		clk_disable_unprepare(qphy->ref_clk);
		qphy->clocks_enabled = false;
	}

	qusb_phy_enable_power(qphy, false);

	return 0;
}

static const struct of_device_id qusb_phy_id_table[] = {
	{ .compatible = "qcom,qusb2phy", },
	{ },
};
MODULE_DEVICE_TABLE(of, qusb_phy_id_table);

static struct platform_driver qusb_phy_driver = {
	.probe		= qusb_phy_probe,
	.remove		= qusb_phy_remove,
	.driver = {
		.name	= "msm-qusb-phy",
		.of_match_table = of_match_ptr(qusb_phy_id_table),
	},
};

module_platform_driver(qusb_phy_driver);

MODULE_DESCRIPTION("MSM QUSB2 PHY driver");
MODULE_LICENSE("GPL v2");
