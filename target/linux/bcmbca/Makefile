# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=bcmbca
BOARDNAME:=Broadcom BCA (ARMv7 Cortex-A7/A9/B15)
FEATURES:=squashfs nand usb gpio
CPU_TYPE:=cortex-a7
SUBTARGETS:=generic

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Broadcom BCA ARMv7 SoC family routers
	(BCM6750, BCM6756, BCM6846, BCM6878, BCM63138, BCM63148, etc.)
endef

include $(INCLUDE_DIR)/target.mk

KERNELNAME:=zImage dtbs

DEFAULT_PACKAGES += \
	fdt-utils uboot-envtools \
	kmod-gpio-button-hotplug

$(eval $(call BuildTarget))
