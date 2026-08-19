/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_kvdb.h
 *
 * Key-value configuration that survives a reset, kept in the AP's own flash
 * partition.  See bk7258_kvdb.c for the layout and for why nothing here is
 * compiled into the image any more.
 *
 * Keys in use, so the next person does not invent a second spelling:
 *
 *   llm.key     API key, e.g. tp-... or sk-...
 *   llm.host    endpoint host, e.g. token-plan-cn.xiaomimimo.com
 *   llm.model   model name, e.g. mimo-v2.5
 *   wifi.ssid   2.4GHz SSID (this part has no 5GHz radio)
 *   wifi.psk    WPA2 passphrase, empty for an open network
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_KVDB_H
#define __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_KVDB_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>

#include <nuttx/compiler.h>

#define BK7258_KVDB_KEY_LLM_KEY    "llm.key"
#define BK7258_KVDB_KEY_LLM_HOST   "llm.host"
#define BK7258_KVDB_KEY_LLM_MODEL  "llm.model"
#define BK7258_KVDB_KEY_WIFI_SSID  "wifi.ssid"
#define BK7258_KVDB_KEY_WIFI_PSK   "wifi.psk"

typedef CODE void (*bk7258_kvdb_cb_t)(FAR const char *key,
                                      FAR const char *value,
                                      FAR void *arg);

/****************************************************************************
 * Name: bk7258_kvdb_init
 *
 * Description:
 *   Bring the store up empty and usable.  Safe from board bring-up: it
 *   touches nothing outside this core.  Idempotent.
 *
 ****************************************************************************/

int bk7258_kvdb_init(void);

/* Kept as the place a backend would be attached from a task.  There is no
 * backend today -- the AP has no flash controller of its own -- so this only
 * reports that the store is memory-only and returns -ENOTSUP.
 */

int bk7258_kvdb_load(void);

/* Whether writes outlive a reset.  Always false: the store is memory-only,
 * so it works for this boot and no longer.
 */

bool bk7258_kvdb_persistent(void);

/****************************************************************************
 * Name: bk7258_kvdb_get
 *
 * Description:
 *   Copy the value of key into value, NUL terminated.  Returns its length,
 *   -ENOENT if the key is not set, or -E2BIG if the buffer is too small.
 *
 ****************************************************************************/

int bk7258_kvdb_get(FAR const char *key, FAR char *value, size_t size);

/****************************************************************************
 * Name: bk7258_kvdb_set / bk7258_kvdb_del
 *
 * Description:
 *   Write or remove a key and persist the whole store.  Each call erases and
 *   rewrites one 4KB sector, which is why these are meant for configuration a
 *   human types, not for anything periodic.  Returns OK or a negated errno.
 *
 ****************************************************************************/

int bk7258_kvdb_set(FAR const char *key, FAR const char *value);
int bk7258_kvdb_del(FAR const char *key);

/****************************************************************************
 * Name: bk7258_kvdb_foreach
 *
 * Description:
 *   Call callback for every key.  Returns the number visited.  Values are
 *   handed over in full; masking secrets is the caller's business, and the
 *   'kvdb' command does exactly that.
 *
 ****************************************************************************/

int bk7258_kvdb_foreach(bk7258_kvdb_cb_t callback, FAR void *arg);

/****************************************************************************
 * Name: bk7258_kvdb_seed_agent_config
 *
 * Description:
 *   Write the stored key and endpoint into the configuration file ai_agent
 *   reads at start-up, so it comes up configured without the key ever being
 *   compiled in.  Silent when nothing is stored or when the agent's data
 *   directory is not on a filesystem.  Implemented on the board side
 *   (bk7258_agent_config.c) because the paths are the agent's, not the
 *   chip's.
 *
 ****************************************************************************/

void bk7258_kvdb_seed_agent_config(void);

/****************************************************************************
 * Name: bk7258_kvdb_apply_wifi
 *
 * Description:
 *   Join the stored network: brings wlan0 up, hands the driver the stored
 *   passphrase and asks it to associate with the stored SSID -- the two `wapi`
 *   commands the manual procedure used to need, through the same ioctls.
 *   Returns OK, -ENOENT when no SSID is stored, or a negated errno.
 *
 *   An address is not part of this.  DHCP lives in apps and cannot be reached
 *   from here, so `renew wlan0` is still what asks for one.
 *
 *   Call from a task, for the same reason as bk7258_kvdb_load(): associating
 *   is a request to the CP and it blocks.
 *
 ****************************************************************************/

int bk7258_kvdb_apply_wifi(void);

#endif /* __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_KVDB_H */
