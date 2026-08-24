#ifndef __APP_VELASIGHT_INCLUDE_VS_HISTORY_H
#define __APP_VELASIGHT_INCLUDE_VS_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vs_types.h"

/* record_key is a short, stable identifier ("R00000001") used both as the
 * on-disk record file's stem and as the handle vs_voice.c freezes at the
 * moment a question is asked, so a later page change cannot make the
 * question answer a different record than the one shown on screen.
 */

#define VS_HISTORY_KEY_MAX 16

struct vs_history_index_s
{
  char     record_key[VS_HISTORY_KEY_MAX];
  char     date[VS_TEXT_SHORT];
  char     title[VS_TEXT_SHORT];
  char     summary[VS_TEXT_LONG];
  uint8_t  calm;
  uint8_t  happy;
  uint8_t  tense;
  bool     incomplete;
};

/****************************************************************************
 * Name: vs_history_open
 *
 * Description:
 *   Wait for the SD-NAND store, load the index into memory, and seed it
 *   with the built-in demo records the very first time no store exists yet
 *   so the UI keeps showing something meaningful before any real record has
 *   been written.  Blocking: SD-NAND automount can take tens of seconds.
 *   Call once at startup, never from the UI hot path.
 *
 * Returned Value:
 *   0 on success (including "seeded fresh"), or a negative errno.  A
 *   failure here still leaves the in-memory index at zero records rather
 *   than leaving the caller without a usable vs_history_count().
 *
 ****************************************************************************/

int vs_history_open(void);

/* Number of records currently known.  In-memory only, never blocks, safe to
 * call from vs_snapshot(). */

unsigned int vs_history_count(void);

/* Copy index fields for record i (0-based, 0 is the newest).  In-memory
 * only, never blocks.  Returns 0, or -EINVAL if i is out of range. */

int vs_history_get_index(unsigned int i, struct vs_history_index_s *out);

/* Read the stored full record for record_key as a NUL-terminated JSON
 * object string (title/summary/date and, when a producer supplies them,
 * body/full_transcript/timeline/emotion_distribution).  Opens a file: call
 * only from a worker thread, never from the UI hot path.
 *
 * Returns the number of bytes written (excluding the NUL), or a negative
 * errno.  -ENOENT means record_key is not on disk (stale key, or the store
 * was cleared). */

int vs_history_read_full(const char *record_key, char *buf, size_t len);

/****************************************************************************
 * Name: vs_history_append
 *
 * Description:
 *   Add one record.  index->record_key is generated and filled in on
 *   return; any value the caller put there is ignored.  full_json, when
 *   non-NULL, is stored verbatim as the full record body alongside the
 *   index fields (it must already be a valid, NUL-terminated JSON object
 *   fragment -- e.g. {"body":"...","full_transcript":"..."} -- with no
 *   trailing content); NULL stores only the index fields.
 *
 *   Writes go through a temp-file-then-rename sequence with fsync so a
 *   power loss mid-write leaves either the old state or the new one, never
 *   a half-written file.  Call only from a worker thread.
 *
 * Returned Value:
 *   0 on success, or a negative errno.  -ENOSPC means the in-memory index
 *   is already at CONFIG_VS_HISTORY_MAX_RECORDS; the caller keeps its
 *   existing history rather than losing an old record to make room.
 *
 ****************************************************************************/

int vs_history_append(struct vs_history_index_s *index,
                      const char *full_json);

void vs_history_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_HISTORY_H */
