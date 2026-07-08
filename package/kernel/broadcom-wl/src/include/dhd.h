/*
 * Minimal dhd.h stub for compiling the Broadcom DHD adaptation layer
 * against mainline Linux 6.18.
 *
 * This provides opaque type declarations and function prototypes
 * that the adaptation layer (dhd_linux_dslcpe.c) needs.
 * Actual implementations are in the proprietary DHD binary blob.
 */
#ifndef _DHD_H_STUB_
#define _DHD_H_STUB_

#include <linux/netdevice.h>

struct dhd_pub;
typedef struct dhd_pub dhd_pub_t;

struct dhd_bus;
struct dhd_info;
struct wl_info;
struct wl_if;

typedef struct {
	int unit;
	struct net_device *dev;
} dhd_dummyif_dev_priv_t;

typedef enum {
	DHD_HELPER_STATUS_OK = 0,
	DHD_HELPER_STATUS_ERR
} dhd_helper_status_t;

dhd_pub_t *dhd_dev_get_dhdpub(struct net_device *dev);
int dhd_dev_get_ifidx(struct net_device *dev);
void dhd_clear_stats(dhd_pub_t *dhdp);

void dhd_pwlcs_unregister_dummyif(struct net_device *dev);
struct net_device *dhd_pwlcs_register_dummyif(char *ifname, bool need_rtnl_lock);

void dhd_reset_cnt(struct net_device *dev);
void dhd_if_clear_stats(struct net_device *dev);

int dhd_get_instance(void *osh);

int dhd_get_cfe_mac(unsigned int instance_id, char *mac);
int dhd_check_and_set_mutxmax(unsigned int instance_id, char *memblock, unsigned int *len);
int dhd_check_and_set_mac(unsigned int instance_id, char *memblock, unsigned int *len);
int dhd_get_nvram_mutxmax(char *var, unsigned int size);

void dhd_vars_adjust(void *dhd, char *memblock, unsigned int *len);

int dhd_dslcpe_iovar_op(unsigned long data, unsigned long len, unsigned long set);

int dhd_pwlcs_get_enable(int unit);
void dhd_pwlcs_init_bc(int unit, int bc, int cur_bc);
void dhd_pwlcs_reset_bc(int unit, int bc);
int dhd_pwlcs_get_bc(int unit, int *bc, int *max_fbc, int *max_dbc);
void dhd_pwlcs_set_bc(int unit, int bc);
int dhd_pwlcs_status2bc(int unit, int status);
int dhd_pwlcs_bc2status(int unit, int bc);
int dhd_pwlcs_get_maxfbc(int unit);
int dhd_pwlcs_get_maxdbc(int unit);
int dhd_pwlcs_test_bc(int unit, int bc);
int dhd_pwlcs_cb(int unit, int bc);

void dhd_wlfc_clear_counts(dhd_pub_t *dhdp, int mode);

void dhd_runner_do_iovar(void *dhd, unsigned long data, unsigned long len, unsigned long set);

struct bcmstrbuf;
void dhd_dbg(dhd_pub_t *dhdp, char *name, struct bcmstrbuf *b, unsigned long data);

int dhd_priv_ioctl(dhd_pub_t *dhdp, struct ifreq *ifr, int cmd);

#endif /* _DHD_H_STUB_ */
