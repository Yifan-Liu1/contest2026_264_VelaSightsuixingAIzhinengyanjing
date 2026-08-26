#ifndef __APP_VELASIGHT_INCLUDE_VS_HISTORY_H
#define __APP_VELASIGHT_INCLUDE_VS_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vs_types.h"

/* Record keys are 8.3-safe file stems: "R" plus seven decimal digits.
 * SOCIAL and CHAT have independent key spaces, so kind must always travel
 * with the key.
 */

#define VS_HISTORY_KEY_MAX 16

enum vs_history_kind_e
{
  VS_HISTORY_KIND_SOCIAL = 0,
  VS_HISTORY_KIND_CHAT,
  VS_HISTORY_KIND_COUNT
};

/* One compact index entry.  The full JSON body is stored in a separate file
 * and is opened only by worker/Web threads; UI snapshots only copy this
 * structure and never touch SD-NAND.
 */

struct vs_history_index_s
{
  enum vs_history_kind_e kind;
  char     record_key[VS_HISTORY_KEY_MAX];
  char     date[VS_TEXT_SHORT];
  char     title[VS_TEXT_SHORT];
  char     summary[VS_TEXT_LONG];
  uint8_t  calm;
  uint8_t  happy;
  uint8_t  tense;
  bool     incomplete;
};

/* Wait for SD-NAND, create the two stores and load their indexes.  A missing
 * SOCIAL index is initialized with protocol-shaped sample sessions; a
 * missing CHAT index is initialized as an empty array.  Corrupt indexes are
 * not overwritten.  Returns 0 only when both stores are usable.
 */

int vs_history_open(void);

/* True only when this kind has a mounted, loaded and writable backing store.
 * A startup failure may still leave in-memory SOCIAL samples visible to the
 * display, but append/open_full then correctly return -ENODEV.
 */

bool vs_history_is_ready(enum vs_history_kind_e kind);

/* In-memory readers.  Index 0 is newest. */

unsigned int vs_history_count(enum vs_history_kind_e kind);
int vs_history_get_index(enum vs_history_kind_e kind, unsigned int index,
                         struct vs_history_index_s *out);

/* Copy one consistent page of index entries while holding the internal lock.
 * offset is newest-first; total is the full count and copied is the number
 * written to out.  out may be NULL only when capacity is zero.
 */

int vs_history_snapshot(enum vs_history_kind_e kind, unsigned int offset,
                        struct vs_history_index_s *out, size_t capacity,
                        unsigned int *total, unsigned int *copied);

/* Open a complete record after strict key validation and membership checking.
 * The returned descriptor is positioned at byte zero and belongs to the
 * caller.  Opening under the history lock gives Web downloads a stable file
 * even if a later append evicts its index entry.
 */

int vs_history_open_full(enum vs_history_kind_e kind, const char *record_key,
                         int *fd, size_t *size);

/* Read a complete JSON body and append a NUL.  Unlike the old API, this never
 * reports a truncated JSON document as success: -E2BIG means len is too
 * small for the complete body plus terminator.
 */

int vs_history_read_full(enum vs_history_kind_e kind, const char *record_key,
                         char *buf, size_t len);

/* Atomically append one full JSON object.  The module generates record_key,
 * writes the body, commits a replacement index, then removes the evicted
 * oldest body (if capacity was full).  A failed index commit leaves the old
 * index and old body intact and returns an error.  Capacity is per kind.
 */

int vs_history_append(enum vs_history_kind_e kind,
                      struct vs_history_index_s *index,
                      const char *full_json);

void vs_history_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_HISTORY_H */
