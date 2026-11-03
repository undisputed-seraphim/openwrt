#
# Copyright (C) 2025 OpenWrt.org
#
# Broadcom DHD WiFi driver for BCM6750 (bcmbca target).
# Source: impl99 (HND 5.04.04p3) from TP-Link Archer BE220 GPL, ported to 6.18.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=broadcom-wl
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/broadcom-wl
  SUBMENU:=Wireless Drivers
  TITLE:=Broadcom DHD WiFi driver (BCM43684/BCM6715)
  DEPENDS:=@TARGET_bcmbca
  FILES:=$(PKG_BUILD_DIR)/dhd.ko
  AUTOLOAD:=$(call AutoProbe,dhd)
endef

define KernelPackage/broadcom-wl/description
  Broadcom DHD (dongle host driver) for the BCM43684 and BCM6715 PCIe
  WiFi radios on the Broadcom BCM6750 SoC. Firmware (rtecdc.bin) is installed
  to /lib/firmware/brcm/.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

define KernelPackage/broadcom-wl/install
	$(INSTALL_DIR) $(1)/lib/firmware/brcm
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/firmware/43684c0.bin \
		$(1)/lib/firmware/brcm/brcmfmac43684c0.bin 2>/dev/null || true
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/firmware/6715b0.bin \
		$(1)/lib/firmware/brcm/brcmfmac6715b0.bin 2>/dev/null || true
endef

$(eval $(call KernelPackage,broadcom-wl))
