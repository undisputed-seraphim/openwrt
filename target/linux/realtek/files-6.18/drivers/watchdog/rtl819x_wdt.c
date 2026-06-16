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

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define RTL819X_WDT_CDBR	0x18	/* clock divider (DIVF: bits 31:16) */
#define RTL819X_WDT_WDTCNR	0x1C	/* control / counter */

#define RTL819X_WDT_WDTCLR	BIT(23)		/* clear the counter (kick) */
#define RTL819X_WDT_OVSEL_18	0x00600000	/* overflow 2^18 ticks */
#define RTL819X_WDT_DISABLE	0xA5600000	/* 0xA5 stop pattern */
#define RTL819X_WDT_LX_CLOCK	200000000	/* LX bus clock */
#define RTL819X_WDT_TICKS	(1 << 18)

struct rtl819x_wdt {
	struct watchdog_device wdev;
	void __iomem *base;
	u32 divider;
};

static int rtl819x_wdt_restart(struct watchdog_device *wdev,
			       unsigned long action, void *data)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);

	/* Fastest possible timeout (2^15 ticks at LX clock) for an
	 * immediate reset.
	 */
	writel(1 << 16, wdt->base + RTL819X_WDT_CDBR);
	writel(0, wdt->base + RTL819X_WDT_WDTCNR);
	mdelay(2000);
	return 0;
}

/* Program the divider and (re)arm the watchdog, clearing the counter. */
static void rtl819x_wdt_arm(struct rtl819x_wdt *wdt)
{
	writel(wdt->divider << 16, wdt->base + RTL819X_WDT_CDBR);
	writel(RTL819X_WDT_OVSEL_18 | RTL819X_WDT_WDTCLR,
	       wdt->base + RTL819X_WDT_WDTCNR);
}

static int rtl819x_wdt_start(struct watchdog_device *wdev)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);

	rtl819x_wdt_arm(wdt);
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

	writel(readl(wdt->base + RTL819X_WDT_WDTCNR) | RTL819X_WDT_WDTCLR,
	       wdt->base + RTL819X_WDT_WDTCNR);
	return 0;
}

static int rtl819x_wdt_set_timeout(struct watchdog_device *wdev,
				   unsigned int timeout)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdev);
	u64 div = (u64)timeout * RTL819X_WDT_LX_CLOCK;

	do_div(div, RTL819X_WDT_TICKS);
	wdt->divider = clamp_t(u32, (u32)div, 1, 0xffff);
	wdev->timeout = timeout;

	if (watchdog_active(wdev))
		rtl819x_wdt_arm(wdt);

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
	.set_timeout = rtl819x_wdt_set_timeout,
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
	wdt->wdev.max_timeout = 85;
	wdt->wdev.timeout    = 30;
	wdt->wdev.parent     = dev;

	watchdog_set_drvdata(&wdt->wdev, wdt);
	rtl819x_wdt_set_timeout(&wdt->wdev, wdt->wdev.timeout);
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
