/*
 * Platform / vendor symbol stubs for the Broadcom DHD driver port.
 *
 * These symbols are provided by BCA platform code or proprietary modules
 * (wlcsm, board param library) that are not part of the open GPL sources.
 * They are stubbed so dhd.ko links; the corresponding functionality is
 * either not needed on the WG660242-ST or provided elsewhere (firmware,
 * device tree, NVRAM blobs).
 */
#include <typedefs.h>
#include <osl.h>
#include <siutils.h>

/* bcmsrom.c defines this only for !BCMDONGLEHOST; dhd_pcie.c calls it
 * unconditionally. Failure is non-fatal (logs a warning and continues). */
int
dbushost_initvars_flash(si_t *sih, osl_t *osh, char **base, uint len)
{
	(void)sih; (void)osh; (void)base; (void)len;
	return -1;
}

/* BCA board-parameter MAC accessor (platform library). */
int
kerSysGetMacAddress(unsigned char *pucaAddr, unsigned long ulId)
{
	(void)pucaAddr; (void)ulId;
	return -1;
}

/* Dongle-side devpath var accessor. */
char *
si_getdevpathvar(si_t *sih, const char *name)
{
	(void)sih; (void)name;
	return NULL;
}

/* WiFi Coexistence Management (wlcsm) NVRAM accessors. */
char *
wlcsm_nvram_k_get(char *name)
{
	(void)name;
	return NULL;
}

int
wlcsm_nvram_k_set(char *name, char *value)
{
	(void)name; (void)value;
	return -1;
}

int
wlcsm_nvram_getall(char *buf, int count)
{
	(void)buf; (void)count;
	return -1;
}
