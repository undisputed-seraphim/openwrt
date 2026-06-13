#
# Copyright (C) 2006-2020 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=firmware-utils
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL=https://github.com/undisputed-seraphim/firmware-utils.git
PKG_SOURCE_DATE:=2026-10-25
PKG_SOURCE_VERSION:=8468cd2ab9e531ad4bdf87bed6de38e706be8422
PKG_MIRROR_HASH:=skip

include $(INCLUDE_DIR)/host-build.mk
include $(INCLUDE_DIR)/cmake.mk

$(eval $(call HostBuild))
