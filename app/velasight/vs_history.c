/****************************************************************************
 * app/velasight/vs_history.c
 *
 * One SD-NAND history engine serves two independent record kinds:
 *
 *   /mnt/sdnand/ai_agent/history/social/INDEX.JSN + R0000000.JSN
 *   /mnt/sdnand/ai_agent/history/chat/INDEX.JSN   + R0000000.JSN
 *
 * SOCIAL bodies use the cloud protocol's ttsMinutes/txtMinutes,
 * audioTimeline and emotionTimeline fields.  CHAT bodies are written by
 * vs_voice.c when a multi-turn idle-assistant conversation ends.  The two
 * kinds share all indexing, locking, atomic-write and bounded-retention
 * code, but have separate directories, sequence spaces and capacities.
 *
 * A record append is a small transaction: write the new body, atomically
 * replace the index with a candidate containing the new entry, update RAM,
 * then unlink the evicted oldest body.  Power loss can therefore leave an
 * unreferenced body, but never an index that points at a body deleted before
 * the new index became durable.  A failed index write is reported and the
 * previous state remains usable.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
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

#ifndef CONFIG_VS_HISTORY_MAX_RECORDS
#  define CONFIG_VS_HISTORY_MAX_RECORDS 64
#endif

#define VS_HISTORY_ROOT       "/mnt/sdnand/ai_agent/history"
#define VS_HISTORY_INDEX_JSN  "INDEX.JSN"

/* Bump to redeploy g_social_seed to devices that already booted the
 * previous set.  A stored SEEDVER below this triggers a one-time
 * re-seed of SOCIAL on the next open.
 */

#define VS_HISTORY_SOCIAL_SEED_VERSION 3
#define VS_HISTORY_INDEX_TMP  "INDEX.TMP"
#define VS_HISTORY_PATH_MAX   96
#define VS_HISTORY_SEQ_MAX    9999999u
#define VS_HISTORY_KEY_FMT    "R%07u"
#define VS_HISTORY_BODY_FMT   "%s/R%07u.JSN"
#define VS_HISTORY_BODY_TMP   "%s/R%07u.TMP"
#define VS_HISTORY_INDEX_MAX  \
  ((size_t)CONFIG_VS_HISTORY_MAX_RECORDS * 1024u + 2u)
#define VS_HISTORY_SEQ_PROBES \
  ((unsigned int)CONFIG_VS_HISTORY_MAX_RECORDS + 1024u)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *const g_history_dir[VS_HISTORY_KIND_COUNT] =
{
  VS_HISTORY_ROOT "/social",
  VS_HISTORY_ROOT "/chat",
};

static struct vs_history_index_s *g_index[VS_HISTORY_KIND_COUNT];
static unsigned int g_count[VS_HISTORY_KIND_COUNT];
static unsigned int g_next_seq[VS_HISTORY_KIND_COUNT];
static bool g_kind_ready[VS_HISTORY_KIND_COUNT];
static bool g_opened;
static pthread_mutex_t g_history_lock = PTHREAD_MUTEX_INITIALIZER;

/* Fresh-store SOCIAL examples.  They are persisted through the same atomic
 * body/index path as future real sessions.  CHAT is intentionally never
 * seeded: fabricated user conversations must not appear as real history.
 */

static const struct vs_history_index_s g_social_seed[] =
{
  {
    VS_HISTORY_KIND_SOCIAL, "R0000000", "08/20 10:00", "播报测试",
    "数字播报测试\n说你好触发", 70, 25, 5, false
  },
  {
    VS_HISTORY_KIND_SOCIAL, "R0000001", "08/19 12:30", "午间闲聊",
    "同事午餐闲聊\n气氛轻松", 60, 35, 5, false
  },
  {
    VS_HISTORY_KIND_SOCIAL, "R0000002", "08/18 20:15", "争执片段",
    "对话中出现愤怒\n夹杂低落情绪", 10, 5, 85, true
  },
};

/* Each entry is exactly the interface doc's msgEvent-2 result payload: a
 * single `response` object holding ttsMinutes, txtMinutes, audioTimeline
 * and emotionTimeline, with the documented string types and the fixed
 * emotionColor/emotionDetail sets.  No local envelope is added -- the
 * social/ vs chat/ directory already carries the kind -- so a downloaded
 * record is byte-for-byte the shape the cloud returns.  Record 0 is a TTS
 * soak test whose text documents that "你好" is answered by counting one
 * to thirty; record 1 is an ordinary pleasant talk; record 2 carries
 * anger turning to sadness.
 */

static const char *const g_social_seed_body[] =
{
  "{\"response\":{\"ttsMinutes\":\"\",\"txtMinutes\":\"这是一条语音播报（TTS）测试记录。约定：当用户说“你好”时，助手用中文从一数到三十（一、二、三、……、三十）连续朗读，用于验证长文本语音合成与播放是否完整、连贯、无中断。\",\"audioTimeline\":[{\"sentence\":\"你好\",\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.96\",\"timestampBegin\":\"0.0\",\"timestampEnd\":\"0.8\"},{\"sentence\":\"一二三四五六七八九十\",\"emotionColor\":\"green\",\"emotionDetail\":\"中立\",\"confidence\":\"0.90\",\"timestampBegin\":\"1.0\",\"timestampEnd\":\"6.5\"}],\"emotionTimeline\":[{\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.95\",\"timestamp\":\"500\"},{\"emotionColor\":\"green\",\"emotionDetail\":\"中立\",\"confidence\":\"0.90\",\"timestamp\":\"3500\"}]}}",

  "{\"response\":{\"ttsMinutes\":\"\",\"txtMinutes\":\"午间与同事一起吃饭，聊到最近的项目进度、周末计划和一部刚上映的电影，整体气氛轻松愉快。中途对下周的排期安排有一点疑惑，简单确认后达成一致。全程情绪平稳，以愉悦和中立为主，没有需要特别关注的片段。\",\"audioTimeline\":[{\"sentence\":\"这家店的午餐还挺不错的\",\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.93\",\"timestampBegin\":\"0.0\",\"timestampEnd\":\"2.4\"},{\"sentence\":\"你周末有什么安排吗\",\"emotionColor\":\"green\",\"emotionDetail\":\"中立\",\"confidence\":\"0.90\",\"timestampBegin\":\"5.1\",\"timestampEnd\":\"7.0\"},{\"sentence\":\"我打算去看那部新上映的电影\",\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.92\",\"timestampBegin\":\"7.5\",\"timestampEnd\":\"10.2\"},{\"sentence\":\"下周的排期我还有点不确定\",\"emotionColor\":\"blue\",\"emotionDetail\":\"疑惑\",\"confidence\":\"0.84\",\"timestampBegin\":\"20.3\",\"timestampEnd\":\"23.0\"},{\"sentence\":\"那我们等确认了再定吧\",\"emotionColor\":\"green\",\"emotionDetail\":\"中立\",\"confidence\":\"0.88\",\"timestampBegin\":\"24.0\",\"timestampEnd\":\"26.1\"},{\"sentence\":\"好的没问题\",\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.94\",\"timestampBegin\":\"26.5\",\"timestampEnd\":\"27.8\"}],\"emotionTimeline\":[{\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.93\",\"timestamp\":\"1200\"},{\"emotionColor\":\"green\",\"emotionDetail\":\"中立\",\"confidence\":\"0.89\",\"timestamp\":\"6000\"},{\"emotionColor\":\"blue\",\"emotionDetail\":\"疑惑\",\"confidence\":\"0.83\",\"timestamp\":\"21500\"},{\"emotionColor\":\"green\",\"emotionDetail\":\"愉悦\",\"confidence\":\"0.92\",\"timestamp\":\"27000\"}]}}",

  "{\"response\":{\"ttsMinutes\":\"\",\"txtMinutes\":\"傍晚的一次对话中出现明显冲突。对方语气升高、多次表达不满与愤怒，随后情绪转为低落和伤心，交流未能达成一致，气氛紧张。建议先暂停争论，给彼此一些冷静的时间，待情绪平复后再心平气和地沟通。\",\"audioTimeline\":[{\"sentence\":\"你怎么又把这件事搞砸了\",\"emotionColor\":\"red\",\"emotionDetail\":\"生气\",\"confidence\":\"0.95\",\"timestampBegin\":\"2.0\",\"timestampEnd\":\"4.6\"},{\"sentence\":\"我真的受够了这样\",\"emotionColor\":\"red\",\"emotionDetail\":\"反感\",\"confidence\":\"0.90\",\"timestampBegin\":\"5.0\",\"timestampEnd\":\"7.3\"},{\"sentence\":\"我也不想这样其实我很难过\",\"emotionColor\":\"blue\",\"emotionDetail\":\"伤心\",\"confidence\":\"0.87\",\"timestampBegin\":\"30.2\",\"timestampEnd\":\"33.5\"}],\"emotionTimeline\":[{\"emotionColor\":\"red\",\"emotionDetail\":\"生气\",\"confidence\":\"0.94\",\"timestamp\":\"3000\"},{\"emotionColor\":\"red\",\"emotionDetail\":\"反感\",\"confidence\":\"0.89\",\"timestamp\":\"6000\"},{\"emotionColor\":\"blue\",\"emotionDetail\":\"伤心\",\"confidence\":\"0.86\",\"timestamp\":\"31500\"}]}}",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Seed-version sidecar.  See the file that bumps
 * VS_HISTORY_SOCIAL_SEED_VERSION for why this exists: it forces already-
 * provisioned NAND to adopt new seed content.  Best-effort -- a failed
 * read or write just re-seeds again next boot, which is harmless.
 */

static int vs_history_seed_version_get_locked(enum vs_history_kind_e kind)
{
  char path[VS_HISTORY_PATH_MAX];
  FILE *f;
  int ver = 0;

  snprintf(path, sizeof(path), "%s/SEEDVER", g_history_dir[kind]);
  f = fopen(path, "r");
  if (f == NULL)
    {
      return 0;
    }

  if (fscanf(f, "%d", &ver) != 1)
    {
      ver = 0;
    }

  fclose(f);
  return ver;
}

static void vs_history_seed_version_put_locked(enum vs_history_kind_e kind,
                                               int ver)
{
  char path[VS_HISTORY_PATH_MAX];
  FILE *f;

  snprintf(path, sizeof(path), "%s/SEEDVER", g_history_dir[kind]);
  f = fopen(path, "w");
  if (f == NULL)
    {
      return;
    }

  fprintf(f, "%d\n", ver);
  fclose(f);
}

static bool vs_history_kind_valid(enum vs_history_kind_e kind)
{
  return (unsigned int)kind < (unsigned int)VS_HISTORY_KIND_COUNT;
}

static int vs_history_alloc_tables_locked(void)
{
  unsigned int kind;

  for (kind = 0; kind < VS_HISTORY_KIND_COUNT; kind++)
    {
      if (g_index[kind] == NULL)
        {
          g_index[kind] = calloc(CONFIG_VS_HISTORY_MAX_RECORDS,
                                 sizeof(g_index[kind][0]));
          if (g_index[kind] == NULL)
            {
              unsigned int cleanup;

              for (cleanup = 0; cleanup < VS_HISTORY_KIND_COUNT; cleanup++)
                {
                  free(g_index[cleanup]);
                  g_index[cleanup] = NULL;
                }

              return -ENOMEM;
            }
        }
    }

  return 0;
}

static int vs_history_mkdir(const char *path)
{
  if (mkdir(path, 0700) == 0 || errno == EEXIST)
    {
      return 0;
    }

  return -errno;
}

static int vs_history_wait_for_store(void)
{
#ifdef CONFIG_BK7258_SDIO
  char source[32];
  bool mounted;
  unsigned int attempt;

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

static int vs_history_key_parse(const char *key, unsigned int *seq)
{
  unsigned int value = 0;
  size_t i;

  if (key == NULL || strlen(key) != 8 || key[0] != 'R')
    {
      return -EINVAL;
    }

  for (i = 1; i < 8; i++)
    {
      if (key[i] < '0' || key[i] > '9')
        {
          return -EINVAL;
        }

      value = value * 10u + (unsigned int)(key[i] - '0');
    }

  if (value > VS_HISTORY_SEQ_MAX)
    {
      return -EINVAL;
    }

  if (seq != NULL)
    {
      *seq = value;
    }

  return 0;
}

static void vs_history_body_path(enum vs_history_kind_e kind,
                                 unsigned int seq, bool temporary,
                                 char *path, size_t path_len)
{
  snprintf(path, path_len,
           temporary ? VS_HISTORY_BODY_TMP : VS_HISTORY_BODY_FMT,
           g_history_dir[kind], seq);
}

static void vs_history_index_path(enum vs_history_kind_e kind,
                                  bool temporary, char *path,
                                  size_t path_len)
{
  snprintf(path, path_len, "%s/%s", g_history_dir[kind],
           temporary ? VS_HISTORY_INDEX_TMP : VS_HISTORY_INDEX_JSN);
}

static void vs_history_sync_dir(const char *dir)
{
  int fd = open(dir, O_RDONLY);

  if (fd >= 0)
    {
      (void)fsync(fd);
      close(fd);
    }
}

static int vs_history_write_all(int fd, const char *text, size_t len)
{
  size_t done = 0;

  while (done < len)
    {
      ssize_t n = write(fd, text + done, len - done);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          return -EIO;
        }

      done += (size_t)n;
    }

  return 0;
}

static int vs_history_read_all(int fd, char *buf, size_t len)
{
  size_t done = 0;

  while (done < len)
    {
      ssize_t n = read(fd, buf + done, len - done);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          return -EIO;
        }

      done += (size_t)n;
    }

  return 0;
}

static int vs_history_atomic_write(const char *dir, const char *temporary,
                                   const char *destination,
                                   const char *text)
{
  size_t len = strlen(text);
  int fd;
  int ret;

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = vs_history_write_all(fd, text, len);
  if (ret == 0 && fsync(fd) < 0)
    {
      ret = -errno;
    }

  if (close(fd) < 0 && ret == 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  if (rename(temporary, destination) < 0)
    {
      ret = -errno;
      unlink(temporary);
      return ret;
    }

  vs_history_sync_dir(dir);
  return 0;
}

static bool vs_history_percent_valid(cJSON *item, uint8_t *value)
{
  if (item == NULL)
    {
      *value = 0;
      return true;
    }

  if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 100)
    {
      return false;
    }

  *value = (uint8_t)item->valueint;
  return true;
}

static bool vs_history_copy_json_string(cJSON *obj, const char *name,
                                        char *out, size_t out_len,
                                        bool required)
{
  cJSON *item = cJSON_GetObjectItem(obj, name);

  if (item == NULL)
    {
      out[0] = '\0';
      return !required;
    }

  if (!cJSON_IsString(item) || strlen(item->valuestring) >= out_len)
    {
      return false;
    }

  snprintf(out, out_len, "%s", item->valuestring);
  return true;
}

static bool vs_history_parse_entry(enum vs_history_kind_e kind, cJSON *obj,
                                   struct vs_history_index_s *entry)
{
  cJSON *item;

  if (!cJSON_IsObject(obj))
    {
      return false;
    }

  memset(entry, 0, sizeof(*entry));
  entry->kind = kind;

  if (!vs_history_copy_json_string(obj, "record_key", entry->record_key,
                                   sizeof(entry->record_key), true) ||
      vs_history_key_parse(entry->record_key, NULL) < 0 ||
      !vs_history_copy_json_string(obj, "date", entry->date,
                                   sizeof(entry->date), false) ||
      !vs_history_copy_json_string(obj, "title", entry->title,
                                   sizeof(entry->title), false) ||
      !vs_history_copy_json_string(obj, "summary", entry->summary,
                                   sizeof(entry->summary), false) ||
      !vs_history_percent_valid(cJSON_GetObjectItem(obj, "calm"),
                                &entry->calm) ||
      !vs_history_percent_valid(cJSON_GetObjectItem(obj, "happy"),
                                &entry->happy) ||
      !vs_history_percent_valid(cJSON_GetObjectItem(obj, "tense"),
                                &entry->tense))
    {
      return false;
    }

  item = cJSON_GetObjectItem(obj, "incomplete");
  if (item != NULL && !cJSON_IsBool(item))
    {
      return false;
    }

  entry->incomplete = item != NULL && cJSON_IsTrue(item);
  return true;
}

static cJSON *vs_history_entry_json(const struct vs_history_index_s *entry)
{
  cJSON *obj = cJSON_CreateObject();

  if (obj == NULL)
    {
      return NULL;
    }

  cJSON_AddStringToObject(obj, "record_key", entry->record_key);
  cJSON_AddStringToObject(obj, "date", entry->date);
  cJSON_AddStringToObject(obj, "title", entry->title);
  cJSON_AddStringToObject(obj, "summary", entry->summary);
  cJSON_AddNumberToObject(obj, "calm", entry->calm);
  cJSON_AddNumberToObject(obj, "happy", entry->happy);
  cJSON_AddNumberToObject(obj, "tense", entry->tense);
  cJSON_AddBoolToObject(obj, "incomplete", entry->incomplete);
  return obj;
}

static int vs_history_write_index_locked(
    enum vs_history_kind_e kind,
    const struct vs_history_index_s *entries, unsigned int count)
{
  char temporary[VS_HISTORY_PATH_MAX];
  char destination[VS_HISTORY_PATH_MAX];
  cJSON *root;
  char *text;
  unsigned int i;
  int ret;

  root = cJSON_CreateArray();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < count; i++)
    {
      cJSON *obj = vs_history_entry_json(&entries[i]);

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

  if (strlen(text) > VS_HISTORY_INDEX_MAX)
    {
      free(text);
      return -E2BIG;
    }

  vs_history_index_path(kind, true, temporary, sizeof(temporary));
  vs_history_index_path(kind, false, destination, sizeof(destination));
  ret = vs_history_atomic_write(g_history_dir[kind], temporary,
                                destination, text);
  free(text);
  return ret;
}

static int vs_history_load_index_locked(enum vs_history_kind_e kind)
{
  struct vs_history_index_s *entries;
  char path[VS_HISTORY_PATH_MAX];
  struct stat st;
  cJSON *root = NULL;
  char *raw = NULL;
  unsigned int count;
  unsigned int next_seq = 0;
  int fd = -1;
  int ret = 0;
  int i;

  vs_history_index_path(kind, false, path, sizeof(path));
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &st) < 0)
    {
      ret = -errno;
      goto out;
    }

  if (st.st_size < 2 || (uint64_t)st.st_size > VS_HISTORY_INDEX_MAX)
    {
      ret = -EBADMSG;
      goto out;
    }

  raw = malloc((size_t)st.st_size + 1u);
  entries = calloc(CONFIG_VS_HISTORY_MAX_RECORDS, sizeof(*entries));
  if (raw == NULL || entries == NULL)
    {
      free(entries);
      ret = -ENOMEM;
      goto out;
    }

  ret = vs_history_read_all(fd, raw, (size_t)st.st_size);
  if (ret < 0)
    {
      free(entries);
      goto out;
    }

  raw[st.st_size] = '\0';
  root = cJSON_Parse(raw);
  if (root == NULL || !cJSON_IsArray(root) ||
      cJSON_GetArraySize(root) > CONFIG_VS_HISTORY_MAX_RECORDS)
    {
      free(entries);
      ret = -EBADMSG;
      goto out;
    }

  count = (unsigned int)cJSON_GetArraySize(root);
  for (i = 0; i < (int)count; i++)
    {
      unsigned int seq;
      int j;

      /* No stat() of the body here.  On SD-NAND that was one VFAT lookup per
       * record -- thirteen for the chat index -- on the boot path, and a
       * missing or empty body is caught anyway when the record is opened or
       * downloaded.  A single bad body should not fail the whole index load.
       */

      if (!vs_history_parse_entry(kind, cJSON_GetArrayItem(root, i),
                                  &entries[i]) ||
          vs_history_key_parse(entries[i].record_key, &seq) < 0)
        {
          free(entries);
          ret = -EBADMSG;
          goto out;
        }

      for (j = 0; j < i; j++)
        {
          if (strcmp(entries[j].record_key, entries[i].record_key) == 0)
            {
              free(entries);
              ret = -EBADMSG;
              goto out;
            }
        }

      if (seq >= next_seq)
        {
          next_seq = seq == VS_HISTORY_SEQ_MAX ? 0 : seq + 1u;
        }
    }

  memcpy(g_index[kind], entries, count * sizeof(entries[0]));
  g_count[kind] = count;
  g_next_seq[kind] = next_seq;
  free(entries);

out:
  cJSON_Delete(root);
  free(raw);
  close(fd);
  return ret;
}

static void vs_history_load_seed_memory_locked(void)
{
  unsigned int count = (unsigned int)(sizeof(g_social_seed) /
                                      sizeof(g_social_seed[0]));

  memcpy(g_index[VS_HISTORY_KIND_SOCIAL], g_social_seed,
         count * sizeof(g_social_seed[0]));
  g_count[VS_HISTORY_KIND_SOCIAL] = count;
  g_next_seq[VS_HISTORY_KIND_SOCIAL] = count;
}

static int vs_history_seed_social_locked(void)
{
  unsigned int count = (unsigned int)(sizeof(g_social_seed) /
                                      sizeof(g_social_seed[0]));
  unsigned int i;
  int ret;

  for (i = 0; i < count; i++)
    {
      char temporary[VS_HISTORY_PATH_MAX];
      char destination[VS_HISTORY_PATH_MAX];
      unsigned int seq;

      if (vs_history_key_parse(g_social_seed[i].record_key, &seq) < 0)
        {
          return -EINVAL;
        }

      vs_history_body_path(VS_HISTORY_KIND_SOCIAL, seq, true, temporary,
                           sizeof(temporary));
      vs_history_body_path(VS_HISTORY_KIND_SOCIAL, seq, false, destination,
                           sizeof(destination));
      ret = vs_history_atomic_write(
          g_history_dir[VS_HISTORY_KIND_SOCIAL], temporary, destination,
          g_social_seed_body[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = vs_history_write_index_locked(VS_HISTORY_KIND_SOCIAL,
                                      g_social_seed, count);
  if (ret < 0)
    {
      return ret;
    }

  vs_history_seed_version_put_locked(VS_HISTORY_KIND_SOCIAL,
                                     VS_HISTORY_SOCIAL_SEED_VERSION);
  vs_history_load_seed_memory_locked();
  return 0;
}

static int vs_history_create_empty_locked(enum vs_history_kind_e kind)
{
  int ret = vs_history_write_index_locked(kind, NULL, 0);

  if (ret == 0)
    {
      g_count[kind] = 0;
      g_next_seq[kind] = 0;
    }

  return ret;
}

static int vs_history_find_key_locked(enum vs_history_kind_e kind,
                                      const char *key)
{
  unsigned int i;

  for (i = 0; i < g_count[kind]; i++)
    {
      if (strcmp(g_index[kind][i].record_key, key) == 0)
        {
          return (int)i;
        }
    }

  return -ENOENT;
}

static int vs_history_choose_seq_locked(enum vs_history_kind_e kind,
                                        unsigned int *result)
{
  unsigned int seq = g_next_seq[kind];
  unsigned int attempt;

  for (attempt = 0; attempt < VS_HISTORY_SEQ_PROBES; attempt++)
    {
      char key[VS_HISTORY_KEY_MAX];
      char path[VS_HISTORY_PATH_MAX];

      snprintf(key, sizeof(key), VS_HISTORY_KEY_FMT, seq);
      vs_history_body_path(kind, seq, false, path, sizeof(path));
      if (vs_history_find_key_locked(kind, key) == -ENOENT &&
          access(path, F_OK) < 0)
        {
          if (errno == ENOENT)
            {
              *result = seq;
              return 0;
            }

          return -errno;
        }

      seq = seq == VS_HISTORY_SEQ_MAX ? 0 : seq + 1u;
    }

  return -ENOSPC;
}

static void vs_history_normalize_entry(struct vs_history_index_s *entry,
                                       enum vs_history_kind_e kind,
                                       unsigned int seq)
{
  entry->kind = kind;
  snprintf(entry->record_key, sizeof(entry->record_key), VS_HISTORY_KEY_FMT,
           seq);
  entry->date[sizeof(entry->date) - 1] = '\0';
  entry->title[sizeof(entry->title) - 1] = '\0';
  entry->summary[sizeof(entry->summary) - 1] = '\0';
  if (entry->calm > 100)
    {
      entry->calm = 100;
    }

  if (entry->happy > 100)
    {
      entry->happy = 100;
    }

  if (entry->tense > 100)
    {
      entry->tense = 100;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_history_open(void)
{
  int first_error = 0;
  int store_ret;
  int ret;
  unsigned int kind;

  pthread_mutex_lock(&g_history_lock);
  if (g_opened)
    {
      for (kind = 0; kind < VS_HISTORY_KIND_COUNT; kind++)
        {
          if (!g_kind_ready[kind] && first_error == 0)
            {
              first_error = -ENODEV;
            }
        }

      pthread_mutex_unlock(&g_history_lock);
      return first_error;
    }
  pthread_mutex_unlock(&g_history_lock);

  store_ret = vs_history_wait_for_store();
  pthread_mutex_lock(&g_history_lock);
  if (g_opened)
    {
      pthread_mutex_unlock(&g_history_lock);
      return 0;
    }

  memset(g_kind_ready, 0, sizeof(g_kind_ready));
  memset(g_count, 0, sizeof(g_count));
  memset(g_next_seq, 0, sizeof(g_next_seq));

  ret = vs_history_alloc_tables_locked();
  if (ret < 0)
    {
      pthread_mutex_unlock(&g_history_lock);
      return ret;
    }

  if (store_ret < 0)
    {
      printf("vs_history: store unavailable (%d); samples are RAM-only\n",
             store_ret);
      vs_history_load_seed_memory_locked();
      g_opened = true;
      pthread_mutex_unlock(&g_history_lock);
      return store_ret;
    }

  ret = vs_history_mkdir("/mnt/sdnand/ai_agent");
  if (ret == 0)
    {
      ret = vs_history_mkdir(VS_HISTORY_ROOT);
    }

  for (kind = 0; ret == 0 && kind < VS_HISTORY_KIND_COUNT; kind++)
    {
      ret = vs_history_mkdir(g_history_dir[kind]);
    }

  if (ret < 0)
    {
      vs_history_load_seed_memory_locked();
      g_opened = true;
      pthread_mutex_unlock(&g_history_lock);
      return ret;
    }

  for (kind = 0; kind < VS_HISTORY_KIND_COUNT; kind++)
    {
      ret = vs_history_load_index_locked((enum vs_history_kind_e)kind);
      if (ret == -ENOENT)
        {
          ret = kind == VS_HISTORY_KIND_SOCIAL ?
                vs_history_seed_social_locked() :
                vs_history_create_empty_locked((enum vs_history_kind_e)kind);
        }
      else if (ret == 0 && kind == VS_HISTORY_KIND_SOCIAL &&
               vs_history_seed_version_get_locked(VS_HISTORY_KIND_SOCIAL) !=
               VS_HISTORY_SOCIAL_SEED_VERSION)
        {
          /* Seed content changed since this card was written; adopt it
           * once.  SOCIAL has no real producer, so nothing real is lost.
           */

          printf("vs_history: social seed v%d -> v%d, reseeding\n",
                 vs_history_seed_version_get_locked(VS_HISTORY_KIND_SOCIAL),
                 VS_HISTORY_SOCIAL_SEED_VERSION);
          ret = vs_history_seed_social_locked();
        }

      if (ret == 0)
        {
          g_kind_ready[kind] = true;
          printf("vs_history: loaded %u %s record(s)\n", g_count[kind],
                 kind == VS_HISTORY_KIND_SOCIAL ? "social" : "chat");
        }
      else
        {
          /* Never replace a corrupt index with samples/empty data. */

          printf("vs_history: %s store unavailable (%d)\n",
                 kind == VS_HISTORY_KIND_SOCIAL ? "social" : "chat", ret);
          g_count[kind] = 0;
          g_next_seq[kind] = 0;
          if (first_error == 0)
            {
              first_error = ret;
            }
        }
    }

  g_opened = true;
  pthread_mutex_unlock(&g_history_lock);
  return first_error;
}

bool vs_history_is_ready(enum vs_history_kind_e kind)
{
  bool ready = false;

  if (!vs_history_kind_valid(kind))
    {
      return false;
    }

  pthread_mutex_lock(&g_history_lock);
  ready = g_kind_ready[kind];
  pthread_mutex_unlock(&g_history_lock);
  return ready;
}

unsigned int vs_history_count(enum vs_history_kind_e kind)
{
  unsigned int count = 0;

  if (!vs_history_kind_valid(kind))
    {
      return 0;
    }

  pthread_mutex_lock(&g_history_lock);
  count = g_count[kind];
  pthread_mutex_unlock(&g_history_lock);
  return count;
}

int vs_history_get_index(enum vs_history_kind_e kind, unsigned int index,
                         struct vs_history_index_s *out)
{
  if (!vs_history_kind_valid(kind) || out == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_history_lock);
  if (index >= g_count[kind])
    {
      pthread_mutex_unlock(&g_history_lock);
      return -ENOENT;
    }

  *out = g_index[kind][index];
  pthread_mutex_unlock(&g_history_lock);
  return 0;
}

int vs_history_snapshot(enum vs_history_kind_e kind, unsigned int offset,
                        struct vs_history_index_s *out, size_t capacity,
                        unsigned int *total, unsigned int *copied)
{
  unsigned int available;
  unsigned int take;

  if (!vs_history_kind_valid(kind) || total == NULL || copied == NULL ||
      (capacity > 0 && out == NULL))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_history_lock);
  *total = g_count[kind];
  if (offset >= g_count[kind])
    {
      *copied = 0;
      pthread_mutex_unlock(&g_history_lock);
      return 0;
    }

  available = g_count[kind] - offset;
  take = available < capacity ? available : (unsigned int)capacity;
  if (take > 0)
    {
      memcpy(out, &g_index[kind][offset], take * sizeof(out[0]));
    }

  *copied = take;
  pthread_mutex_unlock(&g_history_lock);
  return 0;
}

int vs_history_open_full(enum vs_history_kind_e kind, const char *record_key,
                         int *fd_out, size_t *size_out)
{
  char path[VS_HISTORY_PATH_MAX];
  struct stat st;
  unsigned int seq;
  int fd;
  int ret;

  if (!vs_history_kind_valid(kind) || fd_out == NULL || size_out == NULL ||
      vs_history_key_parse(record_key, &seq) < 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_history_lock);
  if (!g_kind_ready[kind])
    {
      pthread_mutex_unlock(&g_history_lock);
      return -ENODEV;
    }

  ret = vs_history_find_key_locked(kind, record_key);
  if (ret < 0)
    {
      pthread_mutex_unlock(&g_history_lock);
      return ret;
    }

  vs_history_body_path(kind, seq, false, path, sizeof(path));
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      ret = -errno;
      pthread_mutex_unlock(&g_history_lock);
      return ret;
    }

  if (fstat(fd, &st) < 0 || st.st_size <= 0 ||
      (uint64_t)st.st_size > (uint64_t)SIZE_MAX)
    {
      ret = errno != 0 ? -errno : -EIO;
      close(fd);
      pthread_mutex_unlock(&g_history_lock);
      return ret;
    }

  *fd_out = fd;
  *size_out = (size_t)st.st_size;
  pthread_mutex_unlock(&g_history_lock);
  return 0;
}

int vs_history_read_full(enum vs_history_kind_e kind, const char *record_key,
                         char *buf, size_t len)
{
  size_t size;
  int fd;
  int ret;

  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  ret = vs_history_open_full(kind, record_key, &fd, &size);
  if (ret < 0)
    {
      return ret;
    }

  if (size >= len)
    {
      close(fd);
      return -E2BIG;
    }

  if (size > INT_MAX)
    {
      close(fd);
      return -EFBIG;
    }

  ret = vs_history_read_all(fd, buf, size);
  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  buf[size] = '\0';
  return (int)size;
}

int vs_history_append(enum vs_history_kind_e kind,
                      struct vs_history_index_s *index,
                      const char *full_json)
{
  struct vs_history_index_s *candidate;
  struct vs_history_index_s entry;
  char temporary[VS_HISTORY_PATH_MAX];
  char destination[VS_HISTORY_PATH_MAX];
  char evicted_path[VS_HISTORY_PATH_MAX];
  cJSON *body;
  unsigned int old_count;
  unsigned int new_count;
  unsigned int copy_count;
  unsigned int seq = 0;
  bool evict;
  int ret;

  if (!vs_history_kind_valid(kind) || index == NULL || full_json == NULL ||
      full_json[0] == '\0')
    {
      return -EINVAL;
    }

  body = cJSON_Parse(full_json);
  if (body == NULL || !cJSON_IsObject(body))
    {
      cJSON_Delete(body);
      return -EBADMSG;
    }
  cJSON_Delete(body);

  candidate = malloc(CONFIG_VS_HISTORY_MAX_RECORDS * sizeof(*candidate));
  if (candidate == NULL)
    {
      return -ENOMEM;
    }

  pthread_mutex_lock(&g_history_lock);
  if (!g_kind_ready[kind])
    {
      ret = -ENODEV;
      goto out_unlock;
    }

  ret = vs_history_choose_seq_locked(kind, &seq);
  if (ret < 0)
    {
      goto out_unlock;
    }

  entry = *index;
  vs_history_normalize_entry(&entry, kind, seq);

  old_count = g_count[kind];
  evict = old_count >= CONFIG_VS_HISTORY_MAX_RECORDS;
  new_count = evict ? CONFIG_VS_HISTORY_MAX_RECORDS : old_count + 1u;
  copy_count = new_count - 1u;
  candidate[0] = entry;
  if (copy_count > 0)
    {
      memcpy(&candidate[1], g_index[kind],
             copy_count * sizeof(candidate[0]));
    }

  evicted_path[0] = '\0';
  if (evict)
    {
      unsigned int evicted_seq;

      if (vs_history_key_parse(g_index[kind][old_count - 1u].record_key,
                               &evicted_seq) == 0)
        {
          vs_history_body_path(kind, evicted_seq, false, evicted_path,
                               sizeof(evicted_path));
        }
    }

  vs_history_body_path(kind, seq, true, temporary, sizeof(temporary));
  vs_history_body_path(kind, seq, false, destination, sizeof(destination));
  ret = vs_history_atomic_write(g_history_dir[kind], temporary, destination,
                                full_json);
  if (ret < 0)
    {
      goto out_unlock;
    }

  ret = vs_history_write_index_locked(kind, candidate, new_count);
  if (ret < 0)
    {
      unlink(destination);
      vs_history_sync_dir(g_history_dir[kind]);
      goto out_unlock;
    }

  memcpy(g_index[kind], candidate, new_count * sizeof(candidate[0]));
  g_count[kind] = new_count;
  g_next_seq[kind] = seq == VS_HISTORY_SEQ_MAX ? 0 : seq + 1u;
  *index = entry;

  if (evicted_path[0] != '\0' && unlink(evicted_path) < 0 && errno != ENOENT)
    {
      /* The committed index is authoritative; a failed cleanup is only an
       * orphan and must not turn a durable append into a reported failure.
       */

      printf("vs_history: could not remove evicted body %s: %d\n",
             evicted_path, -errno);
    }
  else if (evicted_path[0] != '\0')
    {
      vs_history_sync_dir(g_history_dir[kind]);
    }

  ret = 0;

out_unlock:
  pthread_mutex_unlock(&g_history_lock);
  free(candidate);
  return ret;
}

void vs_history_close(void)
{
  unsigned int kind;

  pthread_mutex_lock(&g_history_lock);
  g_opened = false;
  memset(g_kind_ready, 0, sizeof(g_kind_ready));
  memset(g_count, 0, sizeof(g_count));
  memset(g_next_seq, 0, sizeof(g_next_seq));
  for (kind = 0; kind < VS_HISTORY_KIND_COUNT; kind++)
    {
      free(g_index[kind]);
      g_index[kind] = NULL;
    }

  pthread_mutex_unlock(&g_history_lock);
}
