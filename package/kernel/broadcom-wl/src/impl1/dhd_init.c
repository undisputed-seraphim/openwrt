/*
 * Minimal module init for the Broadcom DHD HAL library.
 * The actual DHD driver attaches via the proprietary dhd.ko binary.
 */
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Broadcom DHD HAL for BCM6750");

static int __init bcm_dhd_hal_init(void)
{
	pr_info("bcm-dhd: HAL library loaded\n");
	return 0;
}

static void __exit bcm_dhd_hal_exit(void)
{
	pr_info("bcm-dhd: HAL library unloaded\n");
}

module_init(bcm_dhd_hal_init);
module_exit(bcm_dhd_hal_exit);
