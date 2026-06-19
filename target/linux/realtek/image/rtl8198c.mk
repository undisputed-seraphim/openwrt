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


define Device/askey_ap5100w_rescue
  DEFAULT := y
  LOADADDR := 0x80000000
  SOC := rtl8198c
  DEVICE_DTS := rtl8198c_askey_ap5100w
  DEVICE_VENDOR := Askey
  DEVICE_MODEL := AP5100W
  DEVICE_VARIANT := Rescue
  IMAGE_SIZE := 30720k
  DEVICE_PACKAGES += \
	hostapd-common hostapd-utils \
	kmod-rtw88-8814ae \
	luci luci-ssl \
	dropbear \
	sysupgrade-helper \
	mtd
  KERNEL := kernel-bin | append-dtb | lzma
  IMAGES := rescue.bin
  IMAGE/rescue.bin := append-kernel | pad-to 128k | append-rootfs | pad-rootfs | rt-compress | check-size
  SUPPORTED_DEVICES += askey,ap5100w
endef

define Device/askey_ap5100w
  LOADADDR := 0x80000000
  SOC := rtl8198c
  DEVICE_VENDOR := Askey
  DEVICE_MODEL := AP5100W
  IMAGE_SIZE := 49152k
  DEVICE_PACKAGES += kmod-rtw88-8814ae
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma
  KERNEL_INITRAMFS := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c
  UBOOT_PATH := $(STAGING_DIR_IMAGE)/ap5100w_nand-u-boot.bin
  UBOOT_SPL_PATH := $(STAGING_DIR_IMAGE)/ap5100w_nand-u-boot-spl.bin
  ARTIFACTS := u-boot.bin spl.bin
  ARTIFACT/u-boot.bin := append-uboot
  ARTIFACT/spl.bin := append-uboot-spl
  SUPPORTED_DEVICES += askey,ap5100w
endef

define Device/askey_ap5100w_migration
  DEFAULT := n
  LOADADDR := 0x80000000
  SOC := rtl8198c
  DEVICE_DTS := rtl8198c_askey_ap5100w_migration
  DEVICE_VENDOR := Askey
  DEVICE_MODEL := AP5100W
  DEVICE_VARIANT := Migration
  DEVICE_PACKAGES += nand-utils dropbear
  KERNEL_INITRAMFS := kernel-bin | append-dtb | rt-compress | rt-loader-rtl8198c
endef

TARGET_DEVICES += askey_ap5100w askey_ap5100w_rescue askey_ap5100w_migration
