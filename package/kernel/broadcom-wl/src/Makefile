#
# Kbuild Makefile — Broadcom DHD driver (impl99 lineage) for BCM6750 / kernel 6.18
#
# Source: TP-Link Archer BE220 GPL (HND 5.04.04p3, impl99, kernel 4.19.294)
# Target: OpenWrt bcmbca / BCM6750 (bcm63178) / Linux 6.18
#
# Phase 0: minimal build, NO cfg80211 (WL_CFG80211 undefined).
# See /home/vivy/.opencode/plan/wg660242-wifi-port.md
#

ccflags-y := \
	-I$(src)/include \
	-I$(src)/include/proto \
	-I$(src)/dhd/sys \
	-I$(src)/shared \
	-I$(src)/shared/bcmwifi/include \
	-I$(src)/shared/impl1 \
	-I$(src)/wl/sys \
	-I$(src)/dongle/include \
	-DLINUX \
	-DBCMDRIVER \
	-DBCMDONGLEHOST \
	-DHEALTH_CHECK \
	-DDHDAP \
	-DBCMDMA64OSL \
	-DPCIE_FULL_DONGLE \
	-DBCMPCIE \
	-DBCMPCIE_IPC_ACWI \
	-DBCM_ROUTER \
	-DBCM_ROUTER_DHD \
	-DDHD_SBF \
	-DBCM_GMAC3 \
	-DDHD_USE_PERIM \
	-DBCM_IOCV_MEM_LOG \
	-DWL_SAE \
	-DBCMCRYPTO_COMPONENT \
	-DCUSTOM_DPC_PRIO_SETTING=-1 \
	-DEXTFDIMGPATH=\"/lib/firmware/brcm\"

# Vendor code: not warning-clean. Do not treat as errors.
ccflags-y += -w

# ---- DHD core (dhd/sys) — core PCIe fullmac; optional features omitted ----
dhd-objs := \
	dhd/sys/dhd_common.o \
	dhd/sys/dhd_custom_gpio.o \
	dhd/sys/dhd_ip.o \
	dhd/sys/dhd_linux.o \
	dhd/sys/dhd_linux_sched.o \
	dhd/sys/dhd_linux_wq.o \
	dhd/sys/dhd_linux_mon.o \
	dhd/sys/dhd_msgbuf.o \
	dhd/sys/dhd_flowring.o \
	dhd/sys/dhd_pcie.o \
	dhd/sys/dhd_pcie_linux.o \
	dhd/sys/dhd_macdbg.o \
	dhd/sys/dhd_sbf.o

# ---- WL core (wl/sys) ----
dhd-objs += \
	wl/sys/wl_core.o \
	wl/sys/wldev_common.o

# ---- HAL (shared) ----
dhd-objs += \
	shared/aiutils.o \
	shared/bcmevent.o \
	shared/bcm_l2_filter.o \
	shared/bcmotp.o \
	shared/bcm_psta.o \
	shared/bcmsrom.o \
	shared/bcmstdlib_s.o \
	shared/bcmutils.o \
	shared/bcmxtlv.o \
	shared/siutils.o \
	shared/hndpmu.o \
	shared/hnd_pktpool.o \
	shared/hnd_pktq.o \
	shared/pcie_core.o \
	shared/nvram_ext.o \
	shared/dld_linux.o \
	shared/hnd_linux.o \
	shared/linux_osl.o \
	shared/linux_pkt.o \
	shared/dhd_platform_stubs.o

obj-m := dhd.o
