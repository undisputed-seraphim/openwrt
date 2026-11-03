#
# Kbuild Makefile — Broadcom BCM963xx PCIe host controller (impl1 lineage)
#
# Source: TP-Link Archer BE220 GPL (bcmdrivers/opensource/bus/pci/host/impl1)
# Target: OpenWrt bcmbca / BCM6750 (bcm63178) / Linux 6.18
#

ccflags-y := \
	-I$(src) \
	-I$(src)/include \
	-DLINUX \
	-DCONFIG_BCM963178 \
	-DCONFIG_BCM_PCIE_HCD_IMPL=1 \
	-DCONFIG_BCM_PCI_MSI \
	-DCONFIG_BCM_PMC

# Vendor code: not warning-clean. Do not treat as errors.
ccflags-y += -w

obj-m := bcm-pcie-hcd.o
bcm-pcie-hcd-objs := pcie_hcd.o pcie-bcm963xx.o pcie_wlep_stub.o bcm_platform.o
