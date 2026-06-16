// SPDX-License-Identifier: GPL-2.0-only
/*
 * Setup for the Realtek RTL838X SoC:
 *	Memory, Timer and Serial
 *
 * Copyright (C) 2020 B. Koblitz
 * based on the original BSP by
 * Copyright (C) 2006-2012 Tony Wu (tonywu@realtek.com)
 *
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/of_fdt.h>
#include <linux/irqchip.h>

#include <asm/addrspace.h>
#include <asm/io.h>
#include <asm/bootinfo.h>
#include <asm/time.h>
#include <asm/prom.h>
#include <asm/reboot.h>
#include <asm/smp-ops.h>

#include "mach-rtl819x.h"

extern struct rtl83xx_soc_info soc_info;

#define RTL8198C_GIMR	0xB8003000
#define RTL8198C_WDTCNR	0xB800311C

/*
 * Fallback machine restart for when the rtl819x watchdog driver is not
 * bound.  The primary reboot path goes through the watchdog restart
 * notifier; this handler triggers the same hardware mechanism (write 0
 * to the watchdog timer counter) directly, matching the BSP's
 * bsp_machine_restart() in arch/rlx/soc-rtl819xd/setup.c.
 *
 * Return rather than spin so machine_restart() still reaches
 * do_kernel_restart() and reports "Reboot failed" if the write did
 * not reset the hardware.
 */
static void rtl8198c_restart(char *command)
{
	__raw_writel(0, (volatile void *)RTL8198C_GIMR);
	local_irq_disable();
	__raw_writel(0, (volatile void *)RTL8198C_WDTCNR);
}

void __init plat_mem_setup(void)
{
	void *dtb;

	set_io_port_base(KSEG1);

	dtb = get_fdt();
	if (!dtb)
		panic("no dtb found");

	/*
	 * Load the devicetree. This causes the chosen node to be
	 * parsed resulting in our memory appearing
	 */
	__dt_setup_arch(dtb);

	_machine_restart = rtl8198c_restart;
}

static void plat_time_init_fallback(void)
{
	struct device_node *np;
	u32 freq = 500000000;

	np = of_find_node_by_name(NULL, "cpus");
	if (!np) {
		pr_err("Missing 'cpus' DT node, using default frequency.");
	} else {
		if (of_property_read_u32(np, "frequency", &freq) < 0)
			pr_err("No 'frequency' property in DT, using default.");
		else
			pr_info("CPU frequency from device tree: %dMHz", freq / 1000000);
		of_node_put(np);
	}
	mips_hpt_frequency = freq / 2;
}

void __init plat_time_init(void)
{
/*
 * Initialization routine resembles generic MIPS plat_time_init() with
 * lazy error handling. The final fallback is only needed until we have
 * converted all device trees to new clock syntax.
 */
	struct device_node *np;
	struct clk *clk;

	of_clk_init(NULL);

	mips_hpt_frequency = 0;
	np = of_get_cpu_node(0, NULL);
	if (!np) {
		pr_err("Failed to get CPU node\n");
	} else {
		clk = of_clk_get(np, 0);
		if (IS_ERR(clk)) {
			pr_err("Failed to get CPU clock: %ld\n", PTR_ERR(clk));
		} else {
			mips_hpt_frequency = clk_get_rate(clk) / 2;
			clk_put(clk);
		}
	}

	if (!mips_hpt_frequency)
		plat_time_init_fallback();

	timer_probe();
}

void __init arch_init_irq(void)
{
	irqchip_init();
}
