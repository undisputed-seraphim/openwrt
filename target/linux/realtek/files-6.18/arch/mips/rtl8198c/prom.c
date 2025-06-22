// SPDX-License-Identifier: GPL-2.0-only
/*
 * prom.c
 * Early intialization code for the Realtek RTL838X SoC
 *
 * based on the original BSP by
 * Copyright (C) 2006-2012 Tony Wu (tonywu@realtek.com)
 * Copyright (C) 2020 B. Koblitz
 *
 */

#include <asm/fw/fw.h>
#include <asm/mips-cps.h>
#include <asm/prom.h>
#include <asm/smp-ops.h>

#include <mach-rtl819x.h>

struct rtl8198_soc_info soc_info;
const void *fdt;

#ifdef CONFIG_MIPS_MT_SMP

extern const struct plat_smp_ops vsmp_smp_ops;
static struct plat_smp_ops rtlops;

static void rtlsmp_init_secondary(void)
{
	/*
	 * Enable all CPU interrupts, as everything is managed by the external controller.
	 * TODO: Standard vsmp_init_secondary() has special treatment for Malta if external
	 * GIC is available. Maybe we need this too.
	 */
	if (mips_gic_present())
		pr_warn("%s: GIC present. Maybe interrupt enabling required.\n", __func__);
	else
		set_c0_status(ST0_IM);
}

static void rtlsmp_finish(void)
{
	/* These devices are low on resources. There might be the chance that CEVT_R4K is
	 * not enabled in kernel build. Nevertheless the timer and interrupt 7 might be
	 * active by default after startup of secondary VPE. With no registered handler
	 * that leads to continuous unhandeled interrupts. In this case disable counting
	 * (DC) in the core and confirm a pending interrupt.
	 */
	if (!IS_ENABLED(CONFIG_CEVT_R4K)) {
		write_c0_cause(read_c0_cause() | CAUSEF_DC);
		write_c0_compare(0);
	}

	local_irq_enable();
}

static int rtlsmp_register(void)
{
	if (!cpu_has_mipsmt)
		return 1;

	rtlops = vsmp_smp_ops;
	rtlops.init_secondary = rtlsmp_init_secondary;
	rtlops.smp_finish = rtlsmp_finish;
	register_smp_ops(&rtlops);

	return 0;
}

#else /* !CONFIG_MIPS_MT_SMP */

#define rtlsmp_register() (1)

#endif

void __init device_tree_init(void)
{
	if (!fdt_check_header(&__appended_dtb)) {
		fdt = &__appended_dtb;
		pr_info("Using appended Device Tree.\n");
	}
	initial_boot_params = (void *)fdt;
	unflatten_and_copy_device_tree();

	/* delay cpc & smp probing to allow devicetree access */
	mips_cpc_probe();

	if (!register_cps_smp_ops())
		return;

	if (!rtlsmp_register())
		return;

	register_up_smp_ops();
}

const char *get_system_type(void)
{
	return soc_info.name;
}

void __init prom_init(void)
{
	uint32_t model;

	model = ioread32(RTL8198C_MODEL_NAME_INFO);
	model = model >> 16 & 0xFFFF;
	if ((model & 0x8190) == 0x8190)
		pr_info("RTL8198C model is %x\n", model);
	else
		pr_info("Unknown CPU model: %x\n", model);

	soc_info.id = model;
	soc_info.name = "RTL8198C";
	soc_info.family = RTL8190_FAMILY_ID;

	pr_info("SoC Type: %s\n", get_system_type());

	/*
	 * fw_arg2 is be the pointer to the environment. Some devices (e.g. HP JG924A) hand
	 * over other than expected kernel boot arguments. Something like 0xfffdffff looks
	 * suspicous. Do extra cleanup for fw_init_cmdline() to avoid a hang during boot.
	 */
	if (fw_arg2 >= CKSEG2)
		fw_arg2 = 0;

	fw_init_cmdline();
}
