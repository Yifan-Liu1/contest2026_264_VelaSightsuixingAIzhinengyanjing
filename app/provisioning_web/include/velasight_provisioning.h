/****************************************************************************
 * app/provisioning_web/include/velasight_provisioning.h
 *
 * AP-mode provisioning web entry.  The application switches the radio into
 * SoftAP itself, calls velasight_provisioning_start(), and gets told when a
 * phone has submitted credentials.  This service never touches the Wi-Fi
 * role: it does not associate, does not run wapi, does not start DHCPD and
 * does not switch back to STA.  Owning both the HTTP page and the radio would
 * make it impossible for the application to decide when the network may drop.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_PROVISIONING_WEB_INCLUDE_VELASIGHT_PROVISIONING_H
#define __APP_PROVISIONING_WEB_INCLUDE_VELASIGHT_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* IEEE 802.11 SSID length, and the WPA2 passphrase range.  An empty password
 * means an open network, which is the only reason 0 is legal.
 */

#define VELASIGHT_PROV_SSID_MAX 32
#define VELASIGHT_PROV_PSK_MIN  8
#define VELASIGHT_PROV_PSK_MAX  63
#define VELASIGHT_PROV_API_KEY_MAX 512

/* Where the record lives when the caller passes no path.  On SD-NAND, so it
 * survives a reset; /mnt itself is pseudo-filesystem and /mnt/ram is a PSRAM
 * ramdisk, neither of which would.
 *
 * Every component fits 8.3 because that SD-NAND is VFAT without long-name
 * support (CONFIG_FAT_LFN is not set). This single file contains Wi-Fi
 * credentials and the MiMo API key; deprecated KVDB is not used.
 */

#ifndef CONFIG_VELASIGHT_PROVISION_STORE
#  define CONFIG_VELASIGHT_PROVISION_STORE "/mnt/sdnand/prov/vela.cfg"
#endif

#ifndef CONFIG_VELASIGHT_PROVISION_PORT
#  define CONFIG_VELASIGHT_PROVISION_PORT 80
#endif

struct velasight_prov_credentials_s
{
  char     ssid[VELASIGHT_PROV_SSID_MAX + 1];
  char     password[VELASIGHT_PROV_PSK_MAX + 1];
  char     api_key[VELASIGHT_PROV_API_KEY_MAX + 1];
  uint32_t generation;   /* Monotonic save counter, first save is 1 */
  bool     open_network; /* Password is empty */
};

/* Called after the success page has been written and the connection closed,
 * never before.  The application may leave SoftAP from here, but keeping the
 * listener alive is also valid and lets the user submit another record. Doing
 * radio work mid-response is what makes a phone show "submit failed" for a
 * save that actually happened.
 *
 * status is 0 on success or a negative errno.  Credentials are deliberately
 * absent -- read them with velasight_provisioning_load() so a passphrase does
 * not travel through every callback that only wanted the notification.
 */

typedef void (*velasight_prov_saved_cb_t)(int status, uint32_t generation,
                                           void *arg);

struct velasight_prov_config_s
{
  uint16_t                  port;       /* 0 selects the built-in default */
  bool                      one_shot;   /* Stop after one accepted submit */
  const char               *store_path; /* NULL selects the built-in path */
  velasight_prov_saved_cb_t on_saved;   /* May be NULL */
  void                     *cb_arg;
};

/****************************************************************************
 * Name: velasight_provisioning_start
 *
 * Description:
 *   Bind the HTTP port and start the listener thread.  Returns 0, -EALREADY
 *   if it is already running, or a negative errno.  A NULL config means all
 *   defaults.  The config is copied, so the caller may free it.
 *
 ****************************************************************************/

int velasight_provisioning_start(
    const struct velasight_prov_config_s *config);

/****************************************************************************
 * Name: velasight_provisioning_stop
 *
 * Description:
 *   Close the listener and join the thread.  Returns 0, or -EALREADY when
 *   nothing was running.  Safe to call from the saved callback.
 *
 ****************************************************************************/

int velasight_provisioning_stop(void);

bool velasight_provisioning_is_running(void);

/* Saves observed in this process; 0 before the first one.  Persisted saves
 * from earlier boots are counted by the generation inside the record.
 */

uint32_t velasight_provisioning_generation(void);

/****************************************************************************
 * Name: velasight_provisioning_load
 *
 * Description:
 *   Read the stored credentials.  Returns 0, -ENOENT when nothing has been
 *   provisioned, -EBADMSG when the record is corrupt, or a negative errno.
 *
 ****************************************************************************/

int velasight_provisioning_load(
    struct velasight_prov_credentials_s *out);

int velasight_provisioning_load_from(
    const char *path, struct velasight_prov_credentials_s *out);

#ifdef __cplusplus
}
#endif

#endif /* __APP_PROVISIONING_WEB_INCLUDE_VELASIGHT_PROVISIONING_H */
