# SPDX-License-Identifier: GPL-2.0-only

define Device/askey_ap5100w
  LOADADDR := 0x80100000
  SOC := rtl8198c
  DEVICE_VENDOR := Askey
  DEVICE_MODEL := AP5100W
  KERNEL_SIZE := 6m
  IMAGE_SIZE := 24576k
  DEVICE_PACKAGES := kmod-rtw88-8814ae wpad-basic-mbedtls
  KERNEL := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c | \
	    realtek-cvimg 0x80c00000 0x800000
  KERNEL_INITRAMFS := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c
  IMAGES := sysupgrade.tar
  IMAGE/sysupgrade.tar := append-rootfs | check-size | \
	sysupgrade-tar rootfs=$$$$@ | append-metadata
  SUPPORTED_DEVICES += askey,ap5100w
endef

TARGET_DEVICES += askey_ap5100w
