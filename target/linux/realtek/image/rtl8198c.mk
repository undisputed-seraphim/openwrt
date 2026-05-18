# SPDX-License-Identifier: GPL-2.0-only

define Device/f-secure_sense
  LOADADDR := 0x80000000
  LOADER_PLATFORM := rtl8198c
  LOADER_TYPE := bin
  LZMA_TEXT_START := 0x84000000
  SOC := rtl8198c
  DEVICE_VENDOR := F-Secure
  DEVICE_MODEL := Sense
  IMAGE_SIZE := 32768k
  KERNEL_INITRAMFS := kernel-bin | append-dtb | lzma | loader-kernel
endef

# TARGET_DEVICES += f-secure_sense


define Device/askey_ap5100w
  LOADADDR := 0x80000000
  SOC := rtl8198c
  DEVICE_VENDOR := Askey
  DEVICE_MODEL := AP5100W
  IMAGE_SIZE := 30720k
  RTK_LOADADDR := 0x80c00000
  RTK_BURNADDR := 0x00800000
  KERNEL := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c | rtk-nand-header
  IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | pad-rootfs | check-size | append-metadata
  KERNEL_INITRAMFS := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c
endef

TARGET_DEVICES += askey_ap5100w
