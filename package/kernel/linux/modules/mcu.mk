define KernelPackage/mcu
  SUBMENU:=$(OTHER_MENU)
  TITLE:=Nuvoton MINI54FDE MCU driver
  DEPENDS:=@TARGET_ipq40xx
  KCONFIG:=CONFIG_MCU_I2C
  FILES:=$(LINUX_DIR)/drivers/misc/mcu-i2c.ko
  AUTOLOAD:=$(call AutoProbe,mcu)
endef

$(eval $(call KernelPackage,mcu))
