/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/include/kvdb.h
 *
 * Application-facing view of the board's key-value store.
 *
 * The implementation and its documentation live in
 * board/beken/chips/bk7258/bk7258_kvdb.c; the chip's own include directory is
 * not on an application's include path, while this one is exported as
 * <arch/board/...> (the same route hello_paint.h takes), so the declarations
 * are repeated here rather than reached across layers.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_INCLUDE_KVDB_H
#define __BOARDS_BEKEN_BK7258_AP_INCLUDE_KVDB_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>

#include <nuttx/compiler.h>

/* Keys the rest of the system reads.  Keep in step with
 * chips/bk7258/include/bk7258_kvdb.h.
 */

#define BK7258_KVDB_KEY_LLM_KEY    "llm.key"
#define BK7258_KVDB_KEY_LLM_HOST   "llm.host"
#define BK7258_KVDB_KEY_LLM_MODEL  "llm.model"
#define BK7258_KVDB_KEY_WIFI_SSID  "wifi.ssid"
#define BK7258_KVDB_KEY_WIFI_PSK   "wifi.psk"

typedef CODE void (*bk7258_kvdb_cb_t)(FAR const char *key,
                                      FAR const char *value,
                                      FAR void *arg);

int bk7258_kvdb_init(void);

/* Whether writes outlive a reset.  False means the store works for this boot
 * only -- see CONFIG_BK7258_KVDB_FLASH.
 */

bool bk7258_kvdb_persistent(void);
int bk7258_kvdb_get(FAR const char *key, FAR char *value, size_t size);
int bk7258_kvdb_set(FAR const char *key, FAR const char *value);
int bk7258_kvdb_del(FAR const char *key);
int bk7258_kvdb_foreach(bk7258_kvdb_cb_t callback, FAR void *arg);

#endif /* __BOARDS_BEKEN_BK7258_AP_INCLUDE_KVDB_H */
