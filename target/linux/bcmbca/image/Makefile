# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

DEVICE_VARS += SOC

define Device/Default
  KERNEL := kernel-bin | append-dtb
  KERNEL_DEPENDS = $$(wildcard $(DTS_DIR)/$$(DEVICE_DTS).dts)
  KERNEL_INITRAMFS_SUFFIX := .bin
  KERNEL_INITRAMFS := kernel-bin | append-dtb
  FILESYSTEMS := squashfs
  KERNEL_NAME := zImage
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  IMAGES := ubi
  IMAGE/ubi := append-ubi
endef

define Device/arcadyan_wg660242-st
  DEVICE_VENDOR := Arcadyan
  DEVICE_MODEL := WG660242-ST
  DEVICE_DTS := broadcom/bcm6750-arcadyan-wg660242-st
  DEVICE_PACKAGES := kmod-usb-ohci kmod-usb2 kmod-usb3
endef
TARGET_DEVICES += arcadyan_wg660242-st

$(eval $(call BuildImage))
