/*
 * PCIe WLAN-endpoint (WLEP) stubs.
 *
 * The vendor driver uses WLEP to reach the WiFi endpoint early, for two
 * errata workarounds (Gen3 TCA fine tuning and PHY RX filter setup) that
 * need MDIO access through the endpoint's BAR. Those helpers live in
 * pcie_wlep.c + bcm_pcie.c, which depend on the BCA UBUS window API.
 *
 * We do not need them for basic host bringup: both workarounds are gated
 * on chip/board configuration (phyrxfilter / g3tcaerrata), and if they do
 * trigger, the caller only logs an error and continues. Provide stubs so
 * the host controller can link; port bcm_pcie.c if a board needs them.
 */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <pcie_wlep.h>

struct wlep_dev {
	int unused;
};

struct wlep_dev *wlep_probe_dev(struct pci_bus *root_bus, u32 owin, int *prc)
{
	(void)root_bus;
	(void)owin;
	if (prc)
		*prc = -ENODEV;
	return NULL;
}

int wlep_config_dev(struct wlep_dev *dev)
{
	(void)dev;
	return -ENODEV;
}

int wlep_set_power_state(struct wlep_dev *dev, pci_power_t state)
{
	(void)dev;
	(void)state;
	return -ENODEV;
}

int wlep_get_mlw(struct wlep_dev *dev)
{
	(void)dev;
	return -ENODEV;
}

int wlep_mdio_write(struct wlep_dev *dev, u16 phyad, u16 regad, u16 wrdata)
{
	(void)dev;
	(void)phyad;
	(void)regad;
	(void)wrdata;
	return -ENODEV;
}

u16 wlep_mdio_read(struct wlep_dev *dev, u16 phyad, u16 regad)
{
	(void)dev;
	(void)phyad;
	(void)regad;
	return 0xffff;
}

void wlep_free_dev(struct wlep_dev *dev)
{
	(void)dev;
}
