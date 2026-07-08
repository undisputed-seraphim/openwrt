#
# Copyright (C) 2025 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=broadcom-wl
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/broadcom-wl
  SUBMENU:=Wireless Drivers
  TITLE:=Broadcom DHD WiFi driver for BCM6750
  DEPENDS:=@TARGET_bcmbca +kmod-mii
  FILES:=$(PKG_BUILD_DIR)/dhd.ko
  AUTOLOAD:=$(call AutoProbe,dhd)
endef

define KernelPackage/broadcom-wl/description
  Broadcom DHD (dongle host driver) for BCM43684 and BCM6715 WiFi radios
  found on the Broadcom BCM6750 SoC (Arcadyan WG660242-ST).

  This package contains the open-source HAL and adaptation layer for the
  proprietary DHD driver binary. The firmware blobs (rtecdc.bin) must be
  placed in /lib/firmware/brcm/ alongside NVRAM configuration files.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(PKG_EXTRA_CFLAGS)" \
		modules
endef

$(eval $(call KernelPackage,broadcom-wl))
