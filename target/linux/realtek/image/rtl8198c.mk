# SPDX-License-Identifier: GPL-2.0-only

define Device/askey_ap5100w
  LOADADDR := 0x80100000
  SOC := rtl8198c
  DEVICE_VENDOR := Askey
  DEVICE_MODEL := AP5100W
  IMAGE_SIZE := 24576k
  DEVICE_PACKAGES := kmod-rtw88-8814ae wpad-basic-mbedtls
  KERNEL_INITRAMFS := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c
  IMAGES :=
endef

TARGET_DEVICES += askey_ap5100w
