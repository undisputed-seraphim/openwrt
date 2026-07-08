/*
 * bcmsromio.c — SROM/NVRAM file loading using request_firmware()
 *
 * Ported from Broadcom GPL source for Linux 6.18 compatibility.
 * Replaces filp_open/vfs_read/set_fs pattern with standard
 * Linux firmware loader API.
 *
 * Original copyright: (c) 2017 Broadcom, GPLv2
 */

#if defined(WLC_LOW) || defined(DSLCPE_DONGLEHOST_WOMBO)

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <osl.h>
#include <typedefs.h>
#include <bcmdevs.h>
#include "boardparms.h"
#include "bcmsrom_fmt.h"
#include "siutils.h"
#include "bcmutils.h"

#define MAX_SROM_FILE_SIZE SROM_MAX

#ifdef DSLCPE_DONGLEHOST_WOMBO
int BCMATTACHFN(sprom_update_params)(si_t *sih, uint16 *buf, bool *idx);
#else
int BCMATTACHFN(sprom_update_params)(si_t *sih, uint16 *buf);
#endif
extern int BpGetBoardId(char *pszBoardId);

#define NVRAM_FILE_NAME_SIZE 64

/* Track which files have been loaded to avoid duplicates (in-memory) */
static char nvramloaded_last[NVRAM_FILE_NAME_SIZE];
static char nvram_loaded_list[5][NVRAM_FILE_NAME_SIZE];
static int nvram_loaded_count;

/* Forward: firmware path prefix for brcm WiFi files */
#define FIRMWARE_BRCM_PATH "/brcm/"

/*
 * Load a file via request_firmware().
 * Returns 1 on success, 0 on failure.
 * The caller gets a copy of the data in *buf up to *buf_len bytes.
 */
static int bcm_request_firmware_file(const char *file_path, char *buf,
				     int *buf_len)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, file_path, NULL);
	if (ret) {
		printk("wl: firmware %s not found (ret=%d)\n", file_path, ret);
		*buf_len = 0;
		return 0;
	}

	*buf_len = min_t(int, *buf_len, fw->size);
	memcpy(buf, fw->data, *buf_len);
	release_firmware(fw);
	return 1;
}

/*
 * Build a firmware path: firmware_path gets "brcm/<name>".
 * Returns pointer to a static buffer (not thread-safe, but single-threaded init).
 */
static const char *bcm_fw_path(const char *name)
{
	static char path[128];
	snprintf(path, sizeof(path), FIRMWARE_BRCM_PATH "%s", name);
	return path;
}

static int findMatching(char *buf, char *words, int size)
{
	int i = 0;
	char *cur = buf;
	while (i < size / NVRAM_FILE_NAME_SIZE) {
		if (!strcmp(cur, words))
			return 1;
		else
			cur += NVRAM_FILE_NAME_SIZE;
		i++;
	}
	return 0;
}

static void append_to_loaded_list(const char *fname)
{
	if (nvram_loaded_count < 5) {
		strncpy(nvram_loaded_list[nvram_loaded_count], fname,
			NVRAM_FILE_NAME_SIZE - 1);
		nvram_loaded_list[nvram_loaded_count][NVRAM_FILE_NAME_SIZE - 1] = '\0';
		nvram_loaded_count++;
	}
}

static int is_already_loaded(const char *fname)
{
	int i;
	for (i = 0; i < nvram_loaded_count; i++) {
		if (!strcmp(nvram_loaded_list[i], fname))
			return 1;
	}
	return 0;
}

int readSromFile(si_t *sih, uint chipId, void *buf, uint nbytes, char *pBoardId)
{
	char fname[64] = {0};
	char BoardId[32] = {0};
	char *base = NULL;
	int size = 0;
	int patch_status = BP_BOARD_ID_NOT_FOUND;
	int ret = -1;
	char devpath[SI_DEVPATH_BUFSZ];

	si_devpath(sih, devpath, sizeof(devpath));
	printk("wl: ID=%s\n", devpath);

	if (pBoardId)
		sprintf(BoardId, "_%s", pBoardId);

	if ((chipId & 0xff00) == 0x4300 || (chipId & 0xff00) == 0x6300) {
		int i = 1;
		sprintf(fname, "bcm%04x%s_map.bin", chipId, BoardId);
		while (is_already_loaded(fname))
			sprintf(fname, "bcm%04x%s_wl%d_map.bin", chipId, BoardId, i++);
	} else if ((chipId / 1000) == 43) {
		int i = 1;
		sprintf(fname, "bcm%d%s_map.bin", chipId, BoardId);
		while (is_already_loaded(fname))
			sprintf(fname, "bcm%d%s_wl%d_map.bin", chipId, BoardId, i++);
	} else {
		return ret;
	}

	base = kmalloc(MAX_SROM_FILE_SIZE, GFP_KERNEL);
	if (!base) {
		printk("%s: failed to malloc.\n", __FUNCTION__);
		return ret;
	}

	size = MAX_SROM_FILE_SIZE;
	if (bcm_request_firmware_file(bcm_fw_path(fname), base, &size)) {
		printk("wl: loading %s\n", bcm_fw_path(fname));
		patch_status = BpUpdateWirelessSromMap(chipId, (uint16 *)base,
						       size / sizeof(uint16));
#ifndef DSLCPE_DONGLEHOST_WOMBO
		sprom_update_params(sih, (uint16 *)base);
#endif
		memcpy(buf, base, min_t(uint, size, nbytes));
		ret = 0;
		strncpy(nvramloaded_last, fname, sizeof(nvramloaded_last) - 1);
		append_to_loaded_list(fname);
	}

	kfree(base);
	return ret;
}

int BCMATTACHFN(init_srom_sw_map)(si_t *sih, uint chipId, void *buf, uint nbytes)
{
	int ret = -1;
	char pszBoardId[32];

	nvram_loaded_count = 0;
	memset(nvram_loaded_list, 0, sizeof(nvram_loaded_list));
	memset(nvramloaded_last, 0, sizeof(nvramloaded_last));

	ASSERT(nbytes <= MAX_SROM_FILE_SIZE);
	BpGetBoardId(pszBoardId);

	if ((ret = readSromFile(sih, chipId, buf, nbytes, pszBoardId)) != 0)
		ret = readSromFile(sih, chipId, buf, nbytes, NULL);

	return ret;
}

int read_sromfile(void *swmap, void *buf, uint offset, uint nbytes)
{
	memcpy((char *)buf, (char *)swmap + offset, nbytes);
	return 0;
}

extern int kerSysGetWlanSromParamsLen(void);
extern int kerSysGetWlanSromParams(unsigned char *wlanCal, unsigned short len);
#define SROM_PARAMS_LEN 256

typedef struct entry_struct {
	unsigned short offset;
	unsigned short value;
} Entry_struct;

typedef struct adapter_struct {
	unsigned char id[SI_DEVPATH_BUFSZ];
	unsigned short entry_num;
	struct entry_struct entry[1];
} Adapter_struct;

#ifdef DSLCPE_DONGLEHOST_WOMBO
int BCMATTACHFN(sprom_update_params)(si_t *sih, uint16 *buf, bool *idx)
#else
int BCMATTACHFN(sprom_update_params)(si_t *sih, uint16 *buf)
#endif
{
	uint16 adapter_num = 0, entry_num = 0, pos = 0;
	struct adapter_struct *adapter_ptr = NULL;
	struct entry_struct *entry_ptr = NULL;
	char id[SI_DEVPATH_BUFSZ];
	int i = 0, j = 0;
	int ret = 0;
	char devpath[SI_DEVPATH_BUFSZ];
	char *params = NULL;
	int param_len = kerSysGetWlanSromParamsLen();

	if (param_len <= 0)
		return -1;
	params = kmalloc(param_len, GFP_KERNEL);
	if (!params)
		return -1;

	si_devpath(sih, devpath, sizeof(devpath));
	kerSysGetWlanSromParams(params, param_len);

	adapter_num = *(uint16 *)(params);
	pos = 2;

	for (i = 0; (i < adapter_num) && (pos < param_len); i++) {
		adapter_ptr = (struct adapter_struct *)(params + pos);
		strncpy(id, adapter_ptr->id, SI_DEVPATH_BUFSZ);
		entry_num = adapter_ptr->entry_num;
		if (!strncmp(id, devpath, strlen(devpath))) {
			entry_ptr = (struct entry_struct *)&(adapter_ptr->entry);
			printk("wl: updating srom from flash...\n");
			for (j = 0; j < entry_num; j++, entry_ptr++) {
				buf[entry_ptr->offset] = entry_ptr->value;
#ifdef DSLCPE_DONGLEHOST_WOMBO
				idx[entry_ptr->offset] = 1;
#endif
			}
			ret = 1;
			break;
		}
		pos += SI_DEVPATH_BUFSZ + sizeof(uint16) +
		       entry_num * sizeof(uint16) * 2;
	}
	kfree(params);
	return ret;
}

int BCMATTACHFN(init_sromvars_map)(si_t *sih, uint chipId, void *buf, uint nbytes)
{
	char fname[64] = {0};
	int ret = -1;
	int size;

	if ((chipId & 0xff00) == 0x4300 || (chipId & 0xff00) == 0x6300)
		sprintf(fname, "bcm%04x_vars.bin", chipId);
	else if ((chipId / 1000) == 43)
		sprintf(fname, "bcm%d_vars.bin", chipId);
	else
		return ret;

	ASSERT(nbytes <= VARS_MAX);

	size = VARS_MAX;
	if (bcm_request_firmware_file(bcm_fw_path(fname), buf, &size)) {
		printk("wl: reading %s\n", bcm_fw_path(fname));
		ret = 0;
	}

	return ret;
}

int read_nvramfile(char *fname, void *buf)
{
	int ret = -1;
	int size = VARS_MAX;

	if (bcm_request_firmware_file(bcm_fw_path(fname), buf, &size)) {
		printk("wl: reading %s\n", bcm_fw_path(fname));
		ret = 0;
	}

	return ret;
}

int BCMATTACHFN(init_nvramvars_cmn)(si_t *sih, void *buf)
{
	char fname[] = "bcmcmn_nvramvars.bin";
	return read_nvramfile(fname, buf);
}

int BCMATTACHFN(init_nvramvars_chip)(si_t *sih, uint chipId, void *buf)
{
	char fname[64] = "";
	char chip_name[32] = "";
	char BoardId[32] = "";
	int ret = -1, i = 0, found = 0;

	if ((chipId & 0xff00) == 0x4300 || (chipId & 0xff00) == 0x6300)
		sprintf(chip_name, "bcm%04x", chipId);
	else if ((chipId / 1000) == 43)
		sprintf(chip_name, "bcm%d", chipId);
	else if ((chipId / 1000) == 53)
		sprintf(chip_name, "bcm%d", 47189);
	else
		return ret;

	if (BpGetBoardId(BoardId) == 0)
		i = 0;
	else
		i = 2;

	while (i < 4) {
		switch (i) {
		case 0:
			sprintf(fname, "%s_%s_nvramvars.bin", chip_name, BoardId);
			break;
		case 1:
			sprintf(fname, "%s_%s_wl%d_nvramvars.bin", chip_name, BoardId, i);
			break;
		case 2:
			sprintf(fname, "%s_nvramvars.bin", chip_name);
			break;
		default:
			i++;
			continue;
		}

		if (!is_already_loaded(fname)) {
			ret = read_nvramfile(fname, buf);
			if (ret == 0) {
				printk("Apply NVRAMVARS:%s\n", fname);
				append_to_loaded_list(fname);
				found = 1;
				break;
			}
		}
		i++;
	}

	if (!found)
		return -1;

	return ret;
}

void BCMATTACHFN(reinit_loaded_srommap)(void)
{
	/* No-op for firmware-loader approach.
	 * State tracking is in-memory and reset on module reload.
	 */
	nvram_loaded_count = 0;
	memset(nvramloaded_last, 0, sizeof(nvramloaded_last));
}

#endif /* WLC_LOW */
