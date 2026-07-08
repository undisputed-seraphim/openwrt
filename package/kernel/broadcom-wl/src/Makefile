#
# Kbuild Makefile for Broadcom DHD WiFi driver (GPL open-source portion)
#

ccflags-y := \
	-I$(src)/include \
	-I$(src)/shared \
	-I$(src)/shared/bcmwifi/include \
	-I$(src)/impl1 \
	-I$(src)/common/include \
	-I$(src)/include/proto \
	-DBCMDRIVER \
	-DWLC_LOW \
	-DDSLCPE \
	-DBCMPCIE \
	-DBCM_DHD_LOCK \
	-DLINUX_PORT \
	-D__ARM_ARCH_7A__ \
	-Wno-date-time \
	-Wno-declaration-after-statement \
	-Wno-implicit-fallthrough \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-missing-prototypes \
	-Wno-empty-body \
	-Wno-unused-but-set-variable \
	-Wno-macro-redefined \
	-Wno-pointer-sign \
	-w

ccflags-y += \
	-DBCM_NORUNNER \
	-DBCM_NOPKTC \
	-DBCM_NOAWL \
	-DBCM_NOPKTFWD \
	-DBCM_NOWFD \
	-DBCM_NOBLOG \
	-Dvirt_to_dma\(dev,va\)=virt_to_phys\(va\) \
	-DOSH_NULL=NULL \
	-DBCMATTACHFN\(_fn\)=_fn

# Minimal core HAL sources for basic WiFi bringup
bcm-hal-objs := \
	shared/linux_osl.o \
	shared/linux_pkt.o \
	shared/aiutils.o \
	shared/siutils.o \
	shared/hnddma.o \
	shared/hnddma_rx.o \
	shared/hnddma_tx.o \
	shared/hndpmu.o \
	shared/pcie_core.o \
	shared/nicpci.o \
	shared/hndpci.o \
	shared/hndchipc.o \
	shared/hndsoc.o \
	shared/hndlhl.o \
	shared/bcmsrom.o \
	shared/bcmotp.o \
	shared/bcmevent.o \
	shared/bcmutils.o \
	shared/sbutils.o \
	shared/bcm_math.o \
	shared/nvram.o \
	shared/nvramstubs.o \
	shared/hnd_pktpool.o \

# Adaptation layer (disabled — needs proprietary DHD headers not in GPL dump)
# bcm-hal-objs += impl1/dhd_linux_dslcpe.o
bcm-hal-objs += \
	impl1/bcmsromio.o \
	impl1/dhd_init.o

obj-m := dhd.o
dhd-objs := $(bcm-hal-objs)
