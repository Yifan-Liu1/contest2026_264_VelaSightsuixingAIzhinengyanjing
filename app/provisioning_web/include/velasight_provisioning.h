/****************************************************************************
 * app/provisioning_web/include/velasight_provisioning.h
 *
 * HTTP provisioning and local-history entry.  The application owns the Wi-Fi
 * role and may keep this listener on either its temporary SoftAP or an
 * already-connected STA interface.  The service binds INADDR_ANY but never
 * associates, runs DHCP, or switches radio mode itself.
 *
 * The service is unauthenticated HTTP.  Applications exposing it on STA must
 * rely on the trusted LAN boundary or add a stronger access-control layer;
 * history and credential changes are available to every client that can
 * reach the configured port.
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

/* Volcengine (ByteDance speech platform) credentials for the idle voice
 * assistant's ASR and TTS.  Both fields are required together: the backend
 * refuses to open a session unless app_id and token are both non-empty (see
 * volc_asr_stream_open()/volc_tts_ws_synthesize_stream() in
 * packages/ai_agent).  Sized from what the platform actually issues, not
 * from the MiMo key's budget -- app_id is a short numeric string and token
 * is a bearer token, neither approaching 512 bytes.
 */

#define VELASIGHT_PROV_VOLC_APPID_MAX 64
#define VELASIGHT_PROV_VOLC_TOKEN_MAX 128

/* Endpoint of the team's own social-session cloud, the /contest/v1 interface
 * app/velasight/vs_cloud.c speaks.  Not the MiMo or Volcengine services
 * above: this one is the project's own server and the three have nothing in
 * common beyond all being reachable over the same radio.
 *
 * Three fields rather than one URL, because each is consumed separately and
 * a parser that split a URL back apart would be a second place for the
 * scheme, the port default and the trailing slash to disagree:
 *
 *   host  a DNS name or dotted quad, no scheme and no port
 *   port  TCP port; 0 means "use the built-in default"
 *   path  the prefix in front of /contest/v1, "" when the endpoints sit at
 *         the document root
 *
 * The path exists because the deployed server does not host the interface at
 * the root the interface document's own examples use: those are written
 * against 127.0.0.1:18080/contest/v1, while the staging deployment answers
 * at /hlthopen/public/contest/v1 and returns an empty 204 for the unprefixed
 * path.  Making the prefix configurable is what lets one image talk to both
 * without a rebuild.
 *
 * host is sized for a fully qualified name with room to spare; path is sized
 * for several segments.  Both are deliberately far smaller than the API key
 * fields -- they are addresses, not credentials.
 */

#define VELASIGHT_PROV_CLOUD_HOST_MAX 96
#define VELASIGHT_PROV_CLOUD_PATH_MAX 64

/* The endpoint a device with nothing stored talks to: the deployment the
 * cloud interface document names.  These are the single definition of that
 * default -- the setup page shows them as the placeholder text and
 * vs_cloud_init() falls back to them, so the address the page says is the
 * factory default is the address the device actually uses.
 *
 * Overridable from Kconfig for a build aimed at a different deployment, which
 * is also how a local mock is pointed at without editing this header.  Port
 * is a string as well as a number because the placeholder attribute needs it
 * pasted into a string literal, and deriving one from the other with the
 * preprocessor needs two levels of indirection for no benefit.
 */

#ifndef CONFIG_VELASIGHT_PROVISION_CLOUD_HOST
#  define CONFIG_VELASIGHT_PROVISION_CLOUD_HOST "staging-hlth.xiaomiwear.com"
#endif

#ifndef CONFIG_VELASIGHT_PROVISION_CLOUD_PATH
#  define CONFIG_VELASIGHT_PROVISION_CLOUD_PATH "/hlthopen/public"
#endif

#ifndef CONFIG_VELASIGHT_PROVISION_CLOUD_PORT
#  define CONFIG_VELASIGHT_PROVISION_CLOUD_PORT 80
#endif

#define VELASIGHT_PROV_CLOUD_HOST_DEFAULT \
  CONFIG_VELASIGHT_PROVISION_CLOUD_HOST
#define VELASIGHT_PROV_CLOUD_PATH_DEFAULT \
  CONFIG_VELASIGHT_PROVISION_CLOUD_PATH
#define VELASIGHT_PROV_CLOUD_PORT_DEFAULT \
  CONFIG_VELASIGHT_PROVISION_CLOUD_PORT

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
  char     volc_appid[VELASIGHT_PROV_VOLC_APPID_MAX + 1];
  char     volc_token[VELASIGHT_PROV_VOLC_TOKEN_MAX + 1];

  /* Social-session cloud endpoint.  Empty host or zero port means the
   * consumer should fall back to its own compiled-in default rather than
   * treat the endpoint as unconfigured -- see vs_cloud_init().  That is the
   * difference between these and the credential fields above: there is a
   * sensible default address, but there is no sensible default API key.
   */

  char     cloud_host[VELASIGHT_PROV_CLOUD_HOST_MAX + 1];
  char     cloud_path[VELASIGHT_PROV_CLOUD_PATH_MAX + 1];
  uint16_t cloud_port;

  uint32_t generation;   /* Monotonic save counter, first save is 1 */
  bool     open_network; /* Password is empty */
};

/* Optional read-only history provider.  The HTTP core deliberately owns no
 * VelaSight storage types: a product adapter supplies compact index entries
 * and opens one stable full-record descriptor for streaming. */

#define VELASIGHT_PROV_HISTORY_KEY_MAX      8
#define VELASIGHT_PROV_HISTORY_DATE_MAX     39
#define VELASIGHT_PROV_HISTORY_TITLE_MAX    39
#define VELASIGHT_PROV_HISTORY_SUMMARY_MAX 127
#define VELASIGHT_PROV_HISTORY_MAX_ENTRIES 256

struct velasight_prov_history_entry_s
{
  char    record_key[VELASIGHT_PROV_HISTORY_KEY_MAX + 1];
  char    date[VELASIGHT_PROV_HISTORY_DATE_MAX + 1];
  char    title[VELASIGHT_PROV_HISTORY_TITLE_MAX + 1];
  char    summary[VELASIGHT_PROV_HISTORY_SUMMARY_MAX + 1];
  uint8_t calm;
  uint8_t happy;
  uint8_t tense;
  bool    incomplete;
};

typedef int (*velasight_prov_history_snapshot_cb_t)(
    unsigned int offset, struct velasight_prov_history_entry_s *out,
    size_t capacity, unsigned int *total, unsigned int *copied, void *arg);

typedef int (*velasight_prov_history_open_cb_t)(
    const char *record_key, int *fd, size_t *size, void *arg);

struct velasight_prov_history_provider_s
{
  velasight_prov_history_snapshot_cb_t snapshot;
  velasight_prov_history_open_cb_t     open;
  void                                *arg;
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
  struct velasight_prov_history_provider_s history; /* Both callbacks or none */
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
