// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL819X watchdog driver
 *
 * Uses the on-chip timer-block watchdog (WDTCNR at offset 0x1C
 * within the timer-block register window at 0x18003100).
 *
 * The SoC has no dedicated soft-reset register; reboot is
 * triggered by writing 0 to WDTCNR for an immediate watchdog
 * reset, matching the BSP's bsp_machine_restart() in
 * arch/rlx/soc-rtl819xd/setup.c.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define RTL819X_WDT_CDBR    0x18  /* clock divider */
#define RTL819X_WDT_WDTCNR  0x1C  /* timer counter / reset */

#define RTL819X_WDT_DISABLE 0xA5600000
#define RTL819X_WDT_ENABLE  0x00600000

struct rtl819x_wdt {
	struct watchdog_device wdev;
	void __iomem *base;
};

static int rtl819x_wdt_restart(struct watchdog_device *wdev,
			       unsigned long action, void *data)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);

	writel(0, wdt->base + RTL819X_WDT_WDTCNR);
	mdelay(2000);
	return 0;
}

static int rtl819x_wdt_start(struct watchdog_device *wdev)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);

	writel(RTL819X_WDT_ENABLE, wdt->base + RTL819X_WDT_WDTCNR);
	return 0;
}

static int rtl819x_wdt_stop(struct watchdog_device *wdev)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);

	writel(RTL819X_WDT_DISABLE, wdt->base + RTL819X_WDT_WDTCNR);
	return 0;
}

static int rtl819x_wdt_ping(struct watchdog_device *wdev)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);

	writel(RTL819X_WDT_DISABLE, wdt->base + RTL819X_WDT_WDTCNR);
	writel(RTL819X_WDT_ENABLE, wdt->base + RTL819X_WDT_WDTCNR);
	return 0;
}

static const struct watchdog_info rtl819x_wdt_ident = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "RTL819X Watchdog",
};

static const struct watchdog_ops rtl819x_wdt_ops = {
	.owner   = THIS_MODULE,
	.start   = rtl819x_wdt_start,
	.stop    = rtl819x_wdt_stop,
	.ping    = rtl819x_wdt_ping,
	.restart = rtl819x_wdt_restart,
};

static int rtl819x_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl819x_wdt *wdt;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	wdt->wdev.info       = &rtl819x_wdt_ident;
	wdt->wdev.ops        = &rtl819x_wdt_ops;
	wdt->wdev.min_timeout = 1;
	wdt->wdev.max_timeout = 60;
	wdt->wdev.timeout    = 30;
	wdt->wdev.parent     = dev;

	watchdog_set_drvdata(&wdt->wdev, wdt);
	watchdog_set_restart_priority(&wdt->wdev, 128);

	return devm_watchdog_register_device(dev, &wdt->wdev);
}

static const struct of_device_id rtl819x_wdt_of_match[] = {
	{ .compatible = "realtek,rtl819x-wdt" },
	{}
};
MODULE_DEVICE_TABLE(of, rtl819x_wdt_of_match);

static struct platform_driver rtl819x_wdt_driver = {
	.probe  = rtl819x_wdt_probe,
	.driver = {
		.name = "rtl819x-wdt",
		.of_match_table = rtl819x_wdt_of_match,
	},
};
module_platform_driver(rtl819x_wdt_driver);

MODULE_DESCRIPTION("Realtek RTL819X watchdog driver");
MODULE_LICENSE("GPL");
