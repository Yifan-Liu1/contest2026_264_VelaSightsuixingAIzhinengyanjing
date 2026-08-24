/****************************************************************************
 * app/velasight/vs_history.c
 *
 * SD-NAND backed history for the idle voice assistant.  An in-memory index
 * (bounded, CONFIG_VS_HISTORY_MAX_RECORDS entries) backs vs_snapshot()'s
 * hot-path reads; disk I/O only happens in vs_history_open() (startup) and
 * vs_history_append()/vs_history_read_full() (called from vs_voice.c's
 * worker thread, never from the UI loop).
 *
 * Filenames are 8.3-safe on purpose even though this board's defconfig now
 * enables CONFIG_FAT_LFN: a long-name volume can still be re-formatted or
 * swapped for a short-name one (see app/sdnand_init), and nothing here
 * needs more than eight characters, so there is no reason to depend on the
 * option.  Layout, one directory:
 *
 *   /mnt/sdnand/ai_agent/history/INDEX.JSN   the whole index, one JSON array
 *   /mnt/sdnand/ai_agent/history/R%08u.JSN   one record's full body
 *
 * INDEX.JSN holds the full index rather than one line per record (contrast
 * app/conv/conv_store.c's pipe-delimited INDEX.TXT): the index here is
 * small (bounded record count, short fields) and callers want structured
 * fields including a leading record_key, so a single cJSON round trip on
 * open/append is simpler than a line-oriented format and does not need a
 * second parser.
 *
 * Write ordering mirrors app/provisioning_web/vp_store.c: temp file, fwrite,
 * fflush+fsync, close, rename over the destination, then fsync the
 * directory.  The record body is written and renamed into place *before*
 * the index is updated, so a crash between the two leaves an index that
 * does not yet mention the new record (invisible, harmless) rather than an
 * index entry whose record file does not exist (a read that fails).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <netutils/cJSON.h>

#include "include/vs_history.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VS_HISTORY_DIR       "/mnt/sdnand/ai_agent/history"
#define VS_HISTORY_INDEX     VS_HISTORY_DIR "/INDEX.JSN"
#define VS_HISTORY_INDEX_TMP VS_HISTORY_DIR "/INDEX.TMP"
#define VS_HISTORY_FMT_JSN   VS_HISTORY_DIR "/R%08u.JSN"
#define VS_HISTORY_FMT_TMP   VS_HISTORY_DIR "/R%08u.TMP"

#ifndef CONFIG_VS_HISTORY_MAX_RECORDS
#  define CONFIG_VS_HISTORY_MAX_RECORDS 64
#endif

/* Full-record reads go through a caller buffer sized by CONFIG_VS_VOICE_*;
 * this is only the cap on what vs_history.c itself will ever read off disk
 * into a scratch buffer while building the trimmed copy, independent of
 * whatever vs_voice.c asks for. */

#define VS_HISTORY_RAW_MAX (16 * 1024)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Newest first, index 0 is the newest -- matching the demo data's ordering
 * and vs_app.c's existing runtime->index semantics (KEY_NEXT/KEY_BACK walk
 * this array by position, unaffected by how it got populated). */

static struct vs_history_index_s g_index[CONFIG_VS_HISTORY_MAX_RECORDS];
static unsigned int g_count;
static unsigned int g_next_seq;
static bool g_ready;

/* Seed data shown until the first real record is appended.  Kept as the
 * fallback rather than deleted so a fresh SD-NAND (or one that failed the
 * 8.3 gate and was reformatted) still demonstrates the history UI. */

static const struct vs_history_index_s g_seed[] =
{
  {
    "R00000000", "08/18 09:20", "上午交流", "整体较平稳\n后段略有疑惑",
    55, 30, 15, false
  },
  {
    "R00000001", "08/17 16:40", "项目讨论", "对方需要进一步确认",
    30, 20, 50, true
  },
  {
    "R00000002", "08/16 11:05", "日常记录", "交流进展顺利",
    65, 25, 10, false
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int vs_history_wait_for_store(void)
{
#ifdef CONFIG_BK7258_SDIO
  char source[32];
  bool mounted;
  unsigned int attempt;

  /* Same bound as vs_config.c's wait: automount can be delayed up to 60 s,
   * plus card-probe timeout.  Only called from vs_history_open(), which
   * runs at startup, never from the UI hot path. */

  for (attempt = 0; attempt < 700; attempt++)
    {
      if (bk7258_mmcsd_status(&mounted, source, sizeof(source)) == 0 &&
          mounted)
        {
          return 0;
        }

      usleep(100000);
    }

  return -ETIMEDOUT;
#else
  return -ENODEV;
#endif
}

static void vs_history_load_seed(void)
{
  unsigned int i;

  for (i = 0; i < sizeof(g_seed) / sizeof(g_seed[0]); i++)
    {
      g_index[i] = g_seed[i];
    }

  g_count    = (unsigned int)(sizeof(g_seed) / sizeof(g_seed[0]));
  g_next_seq = g_count;
}

/* Parse one index entry out of a cJSON object, tolerating missing or
 * mistyped fields so a partially corrupt INDEX.JSN loses only the entries
 * that are actually bad rather than failing the whole load. */

static bool vs_history_parse_entry(cJSON *obj, struct vs_history_index_s *out)
{
  cJSON *item;

  memset(out, 0, sizeof(*out));

  item = cJSON_GetObjectItem(obj, "record_key");
  if (item == NULL || !cJSON_IsString(item) || item->valuestring[0] == '\0')
    {
      return false;
    }

  snprintf(out->record_key, sizeof(out->record_key), "%s",
           item->valuestring);

  item = cJSON_GetObjectItem(obj, "date");
  if (item != NULL && cJSON_IsString(item))
    {
      snprintf(out->date, sizeof(out->date), "%s", item->valuestring);
    }

  item = cJSON_GetObjectItem(obj, "title");
  if (item != NULL && cJSON_IsString(item))
    {
      snprintf(out->title, sizeof(out->title), "%s", item->valuestring);
    }

  item = cJSON_GetObjectItem(obj, "summary");
  if (item != NULL && cJSON_IsString(item))
    {
      snprintf(out->summary, sizeof(out->summary), "%s", item->valuestring);
    }

  item = cJSON_GetObjectItem(obj, "calm");
  out->calm = (item != NULL && cJSON_IsNumber(item)) ?
              (uint8_t)item->valueint : 0;

  item = cJSON_GetObjectItem(obj, "happy");
  out->happy = (item != NULL && cJSON_IsNumber(item)) ?
               (uint8_t)item->valueint : 0;

  item = cJSON_GetObjectItem(obj, "tense");
  out->tense = (item != NULL && cJSON_IsNumber(item)) ?
               (uint8_t)item->valueint : 0;

  item = cJSON_GetObjectItem(obj, "incomplete");
  out->incomplete = item != NULL && cJSON_IsBool(item) && cJSON_IsTrue(item);
  return true;
}

static int vs_history_load_index(void)
{
  unsigned char *raw;
  FILE *f;
  long sz;
  cJSON *root;
  int i;
  int n;

  f = fopen(VS_HISTORY_INDEX, "r");
  if (f == NULL)
    {
      return -ENOENT;
    }

  if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0 || sz == 0 || sz > VS_HISTORY_RAW_MAX)
    {
      fclose(f);
      return -EBADMSG;
    }

  raw = malloc((size_t)sz + 1);
  if (raw == NULL)
    {
      fclose(f);
      return -ENOMEM;
    }

  n = (int)fread(raw, 1, (size_t)sz, f);
  fclose(f);
  raw[n] = '\0';

  root = cJSON_Parse((const char *)raw);
  free(raw);

  if (root == NULL || !cJSON_IsArray(root))
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }

  g_count    = 0;
  g_next_seq = 0;

  for (i = 0; i < cJSON_GetArraySize(root) &&
       g_count < CONFIG_VS_HISTORY_MAX_RECORDS; i++)
    {
      cJSON *obj = cJSON_GetArrayItem(root, i);
      unsigned int seq;

      if (!cJSON_IsObject(obj))
        {
          continue;
        }

      /* A corrupt individual entry is skipped, not fatal to the load. */

      if (!vs_history_parse_entry(obj, &g_index[g_count]))
        {
          continue;
        }

      if (sscanf(g_index[g_count].record_key, "R%08u", &seq) == 1 &&
          seq + 1 > g_next_seq)
        {
          g_next_seq = seq + 1;
        }

      g_count++;
    }

  cJSON_Delete(root);
  return 0;
}

static cJSON *vs_history_entry_to_json(const struct vs_history_index_s *e)
{
  cJSON *obj = cJSON_CreateObject();

  if (obj == NULL)
    {
      return NULL;
    }

  cJSON_AddStringToObject(obj, "record_key", e->record_key);
  cJSON_AddStringToObject(obj, "date", e->date);
  cJSON_AddStringToObject(obj, "title", e->title);
  cJSON_AddStringToObject(obj, "summary", e->summary);
  cJSON_AddNumberToObject(obj, "calm", e->calm);
  cJSON_AddNumberToObject(obj, "happy", e->happy);
  cJSON_AddNumberToObject(obj, "tense", e->tense);
  cJSON_AddBoolToObject(obj, "incomplete", e->incomplete);
  return obj;
}

/* Write `text` to `tmp`, fsync, close, rename onto `dest`, fsync the
 * directory.  Shared by the index save and the per-record body save. */

static int vs_history_atomic_write(const char *tmp, const char *dest,
                                   const char *text)
{
  size_t len = strlen(text);
  int fd;
  ssize_t written;

  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  written = write(fd, text, len);
  if (written < 0 || (size_t)written != len || fsync(fd) < 0)
    {
      int err = errno != 0 ? -errno : -EIO;

      close(fd);
      unlink(tmp);
      return err;
    }

  if (close(fd) < 0)
    {
      int err = -errno;

      unlink(tmp);
      return err;
    }

  if (rename(tmp, dest) < 0)
    {
      int err = -errno;

      unlink(tmp);
      return err;
    }

  /* Best-effort directory fsync so the rename itself survives a power
   * cut.  Not fatal if the platform's fs does not support it. */

  fd = open(VS_HISTORY_DIR, O_RDONLY);
  if (fd >= 0)
    {
      fsync(fd);
      close(fd);
    }

  return 0;
}

static int vs_history_save_index(void)
{
  cJSON *root;
  char *text;
  unsigned int i;
  int ret;

  root = cJSON_CreateArray();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < g_count; i++)
    {
      cJSON *obj = vs_history_entry_to_json(&g_index[i]);

      if (obj == NULL)
        {
          cJSON_Delete(root);
          return -ENOMEM;
        }

      cJSON_AddItemToArray(root, obj);
    }

  text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (text == NULL)
    {
      return -ENOMEM;
    }

  ret = vs_history_atomic_write(VS_HISTORY_INDEX_TMP, VS_HISTORY_INDEX, text);
  free(text);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_history_open(void)
{
  int ret;

  if (g_ready)
    {
      return 0;
    }

  ret = vs_history_wait_for_store();
  if (ret < 0)
    {
      printf("vs_history: store unavailable (%d), using seed data\n", ret);
      vs_history_load_seed();
      g_ready = true;
      return ret;
    }

  mkdir("/mnt/sdnand/ai_agent", 0700);
  mkdir(VS_HISTORY_DIR, 0700);

  ret = vs_history_load_index();
  if (ret < 0)
    {
      printf("vs_history: no usable index (%d), seeding\n", ret);
      vs_history_load_seed();
    }
  else
    {
      printf("vs_history: loaded %u record(s) from %s\n", g_count,
             VS_HISTORY_INDEX);
    }

  g_ready = true;
  return 0;
}

unsigned int vs_history_count(void)
{
  return g_count;
}

int vs_history_get_index(unsigned int i, struct vs_history_index_s *out)
{
  if (out == NULL || i >= g_count)
    {
      return -EINVAL;
    }

  *out = g_index[i];
  return 0;
}

int vs_history_read_full(const char *record_key, char *buf, size_t len)
{
  char path[64];
  FILE *f;
  long sz;
  int n;

  if (record_key == NULL || buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  snprintf(path, sizeof(path), "%s/%s.JSN", VS_HISTORY_DIR, record_key);

  f = fopen(path, "r");
  if (f == NULL)
    {
      return -ENOENT;
    }

  if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      return -EIO;
    }

  if (sz >= (long)len)
    {
      sz = (long)len - 1;
    }

  n = (int)fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = '\0';
  return n;
}

int vs_history_append(struct vs_history_index_s *index,
                      const char *full_json)
{
  char path[64];
  char tmp[64];
  unsigned int seq;
  int ret;

  if (index == NULL)
    {
      return -EINVAL;
    }

  if (!g_ready)
    {
      return -ENODEV;
    }

  if (g_count >= CONFIG_VS_HISTORY_MAX_RECORDS)
    {
      return -ENOSPC;
    }

  seq = g_next_seq;
  snprintf(index->record_key, sizeof(index->record_key), "R%08u", seq);

  if (full_json != NULL)
    {
      snprintf(path, sizeof(path), VS_HISTORY_FMT_JSN, seq);
      snprintf(tmp, sizeof(tmp), VS_HISTORY_FMT_TMP, seq);

      ret = vs_history_atomic_write(tmp, path, full_json);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Insert at the front: index 0 is always the newest, matching how
   * vs_app.c's KEY_NEXT/KEY_BACK walk this array and how the seed data is
   * ordered. */

  memmove(&g_index[1], &g_index[0],
          g_count * sizeof(g_index[0]));
  g_index[0] = *index;
  g_count++;
  g_next_seq = seq + 1;

  ret = vs_history_save_index();
  if (ret < 0)
    {
      /* The record body (if any) is already durable and simply will not be
       * listed until the next successful index save; do not lose the
       * in-memory entry over a transient write failure. */

      printf("vs_history: index save failed (%d), record %s kept in "
             "memory only\n", ret, index->record_key);
    }

  return 0;
}

void vs_history_close(void)
{
  g_ready = false;
  g_count = 0;
  g_next_seq = 0;
}
