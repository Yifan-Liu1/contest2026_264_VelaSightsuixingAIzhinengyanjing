/****************************************************************************
 * Secret-key classification shared by the kvdb CLI and host tests.
 ****************************************************************************/

#ifndef __APP_KVDB_TOOL_KVDB_SECRET_H
#define __APP_KVDB_TOOL_KVDB_SECRET_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool kvdb_key_has_suffix(const char *key, const char *suffix)
{
  size_t key_len = strlen(key);
  size_t suffix_len = strlen(suffix);

  return key_len >= suffix_len &&
         strcmp(key + key_len - suffix_len, suffix) == 0;
}

static inline bool kvdb_key_is_secret(const char *key)
{
  return kvdb_key_has_suffix(key, ".key") ||
         kvdb_key_has_suffix(key, ".psk") ||
         kvdb_key_has_suffix(key, ".token");
}

#endif
