/****************************************************************************
 * app/velasight/include/vs_history.h
 *
 * The on-device record store: kinds, key format, index entry and API.
 *
 * SOCIAL and CHAT have independent key spaces, so a kind always travels with a
 * key.  The index entry is deliberately compact and self-contained: UI
 * snapshots copy it whole and never touch SD-NAND, while the full JSON body
 * lives in a separate file that only worker and Web threads open.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
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

/* What a social record is called.  Every one of them carries the same string,
 * because the title says which kind of session it was and not which session:
 * it is a label for the kind, not information about the record.
 *
 * Here rather than spelled out at each end, because both ends need it and they
 * are in different translation units -- vs_social.c writes it into the record,
 * and vs_app.c puts it on the screen without reading the record's copy back.
 * Reading it back is what went wrong: the title used to be five glyphs, the box
 * that holds it is 64 px, this font advances 16.0 px per CJK glyph, and every
 * record already on a board kept the old five whatever the running firmware
 * said.  Shortening the string alone could only ever fix records not yet
 * written.
 *
 * Three glyphs is 48 px, centred at x=56..104 in that box, and the screen's
 * chord at the title row's highest scanline runs x=45..115.
 */

#define VS_HISTORY_SOCIAL_TITLE "面对面"

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
 * oldest body and its audio sidecar (if capacity was full).  A failed index
 * commit leaves the old index and old body intact and returns an error.
 * Capacity is per kind.
 *
 * On success *index carries the generated record_key back, which is what a
 * caller needs to name anything else it wants to file alongside the record --
 * see vs_history_audio_path().
 */

int vs_history_append(enum vs_history_kind_e kind,
                      struct vs_history_index_s *index,
                      const char *full_json);

/****************************************************************************
 * Name: vs_history_audio_path
 *
 * Description:
 *   Where this record's spoken minutes belong: the record's own stem with a
 *   .WAV extension, in the same per-kind directory as its body.
 *
 *   The store owns the naming rather than exposing its directory, because the
 *   two things that must stay true are its business: the name has to remain
 *   8.3-safe on this VFAT volume ("R" plus seven digits is exactly eight
 *   characters), and the file has to be removed when the record it belongs to
 *   is evicted -- which vs_history_append() does, and which no caller could do
 *   because eviction is not reported to anyone.
 *
 *   Formatting a path says nothing about whether the file exists.  A record
 *   whose session never produced audio, or whose download failed, simply has
 *   no file here; the seed records are all in that state.
 *
 * Returned Value:
 *   0 with path filled, -EINVAL for a bad kind or a malformed key, or
 *   -ENAMETOOLONG when len is too small.
 *
 ****************************************************************************/

int vs_history_audio_path(enum vs_history_kind_e kind,
                          const char *record_key, char *path, size_t len);

void vs_history_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_HISTORY_H */
