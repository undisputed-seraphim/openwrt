// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 B. Koblitz
 * based on the original BSP by
 * Copyright (C) 2006-2012 Tony Wu (tonywu@realtek.com)
 */

#include <asm/bootinfo.h>
#include <asm/prom.h>
#include <asm/reboot.h>
#include <asm/time.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/of_clk.h>

#define RTL8198C_GIMR    0xB8003000
#define RTL8198C_WDTCNR  0xB800311C

static void rtl8198c_restart(char *command)
{
	__raw_writel(0, (volatile void *)RTL8198C_GIMR);
	local_irq_disable();
	__raw_writel(0, (volatile void *)RTL8198C_WDTCNR);
	while (1);
}

static void rtl8198c_halt(void)
{
	local_irq_disable();
	while (1);
}

void __init plat_mem_setup(void)
{
	void *dtb;

	set_io_port_base(KSEG1);

	dtb = get_fdt();
	if (!dtb)
		panic("no dtb found");

	/* Load the devicetree to let the memory appear. */
	__dt_setup_arch(dtb);

	_machine_restart = rtl8198c_restart;
	_machine_halt = rtl8198c_halt;
	pm_power_off = rtl8198c_halt;
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
	 * Initialization routine resembles generic MIPS plat_time_init() with lazy error
	 * handling. The final fallback is needed until all device trees use new clock syntax.
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
