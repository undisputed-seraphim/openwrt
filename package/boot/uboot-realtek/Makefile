include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_VERSION:=2026.07-rc4
PKG_RELEASE:=1
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/undisputed-seraphim/u-boot.git
PKG_SOURCE_VERSION:=c9935564915764112edb9b16d8292d4fb9e6513c
PKG_SOURCE_DATE:=2026-06-18
PKG_MIRROR_HASH:=c42bc96a6780159d596dba10a0d0d7bafcc84d54b5a71bd9e271f5489f58f51a

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk

define U-Boot/Default
  BUILD_TARGET:=realtek
  HIDDEN:=1
endef

define U-Boot/ap5100w_nand
  NAME:=Askey AP5100W
  BUILD_SUBTARGET:=rtl8198c
  BUILD_DEVICES:=askey_ap5100w askey_ap5100w_migration
  UBOOT_CONFIG:=ap5100w_nand
  UBOOT_IMAGE:=u-boot.bin
endef

UBOOT_TARGETS := ap5100w_nand

define Package/u-boot/install
	$(INSTALL_DIR) $(1)/lib/firmware
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/spl/u-boot-spl.bin $(1)/lib/firmware/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/u-boot.bin $(1)/lib/firmware/
endef

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/spl/u-boot-spl.bin $(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-u-boot-spl.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/$(UBOOT_IMAGE) $(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-u-boot.bin
endef

$(eval $(call BuildPackage/U-Boot))
