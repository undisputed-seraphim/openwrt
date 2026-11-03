#
# Copyright (C) 2025 OpenWrt.org
#
# Broadcom BCM963xx PCIe host controller for BCM6750 (bcmbca target).
# Source: impl1 from TP-Link Archer BE220 GPL, ported to 6.18.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bcm-pcie
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bcm-pcie
  SUBMENU:=Other modules
  TITLE:=Broadcom BCM963xx PCIe host controller
  DEPENDS:=@TARGET_bcmbca
  KCONFIG:=CONFIG_PCI_MSI
  FILES:=$(PKG_BUILD_DIR)/bcm-pcie-hcd.ko
  AUTOLOAD:=$(call AutoProbe,bcm-pcie-hcd)
endef

define KernelPackage/bcm-pcie/description
  PCIe host controller driver for the BCM963xx/BCM63178 PCIe cores
  (brcm,bcm963xx-pcie). Required to enumerate the BCM43684 and BCM6715
  WiFi radios on the WG660242-ST.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,bcm-pcie))
