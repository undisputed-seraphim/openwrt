# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=mips
BOARD:=realtek
BOARDNAME:=Realtek MIPS
DEVICE_TYPE:=basic
FEATURES:=ramdisk squashfs
SUBTARGETS:=rtl8198c rtl838x rtl839x rtl930x rtl930x_nand rtl931x rtl931x_nand rtl960x

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Realtek RTL8198C/RTL83xx/RTL93xx/RTL960x based boards.
endef

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += \
	ethtool \
	firewall4 \
	kmod-gpio-button-hotplug \
	odhcp6c \
	uboot-envtools \

# The RTL8198C buttons use the in-kernel gpio-keys-polled driver; the
# out-of-tree module registers the same platform driver name and fails
# to load with "Driver 'gpio-keys-polled' is already registered".
ifeq ($(SUBTARGET),rtl8198c)
DEFAULT_PACKAGES := $(filter-out kmod-gpio-button-hotplug,$(DEFAULT_PACKAGES))
endif

$(eval $(call BuildTarget))
