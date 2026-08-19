/****************************************************************************
 * app/conv/conv_store.c
 *
 * Conversation records on the SD-NAND.  See conv_store.h for why the date
 * lives in an index file instead of in filenames or mtimes.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "conv_store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Days per month, non-leap.  Used by both conversions. */

static const int g_mdays[12] =
{
  31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

#define CONV_IS_LEAP(y) \
  (((y) % 4 == 0 && (y) % 100 != 0) || (y) % 400 == 0)

/* Bytes of transcript examined per read while searching for a keyword.
 *
 * A conversation can be longer than any buffer worth putting on this stack,
 * so the search streams.  The overlap below is what makes a keyword that
 * straddles two reads still match.
 */

#define CONV_SCAN_CHUNK   256

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: conv_parse_line
 *
 * Description:
 *   Parse one index line into an entry.
 *
 *   The format is pipe-separated rather than JSON:
 *
 *     seq|epoch|duration_ms|cue|confidence|unable|text_bytes|summary
 *
 *   A JSON index would be nicer to hand to the web console, but the board is
 *   the side that has to *read* it on every query, and reading JSON in C
 *   costs a parser and the RAM to run it.  Writing JSON is cheap -- printf
 *   does it -- so the conversion happens on the way out instead, where it is
 *   free.  The summary is last so that it may contain anything except a
 *   newline without needing to be escaped.
 *
 * Returned Value:
 *   True if the line parsed.
 *
 ****************************************************************************/

static bool conv_parse_line(char *line, struct conv_entry_s *entry)
{
  char *field[8];
  char *p = line;
  int n = 0;

  memset(entry, 0, sizeof(*entry));

  while (n < 8)
    {
      field[n++] = p;

      /* The last field keeps any pipes it contains: stop splitting once the
       * summary is reached.
       */

      if (n == 8)
        {
          break;
        }

      p = strchr(p, '|');
      if (p == NULL)
        {
          return false;
        }

      *p++ = '\0';
    }

  if (n < 8)
    {
      return false;
    }

  entry->seq             = (unsigned int)strtoul(field[0], NULL, 10);
  entry->epoch           = (uint32_t)strtoul(field[1], NULL, 10);
  entry->duration_ms     = (unsigned int)strtoul(field[2], NULL, 10);
  entry->confidence      = strtof(field[4], NULL);
  entry->unable_to_judge = strtoul(field[5], NULL, 10) != 0;
  entry->text_bytes      = (size_t)strtoul(field[6], NULL, 10);

  strncpy(entry->cue, field[3], sizeof(entry->cue) - 1);
  strncpy(entry->summary, field[7], sizeof(entry->summary) - 1);

  return entry->seq != 0;
}

/****************************************************************************
 * Name: conv_transcript_has
 *
 * Description:
 *   Whether a record's transcript contains needle.
 *
 *   Streamed in overlapping windows so a match that spans two reads is not
 *   missed -- the bug that produces is a keyword search that works for short
 *   conversations and quietly fails for long ones, which is worse than not
 *   having the feature.
 *
 ****************************************************************************/

static bool conv_transcript_has(unsigned int seq, const char *needle)
{
  char path[64];
  char window[CONV_SCAN_CHUNK * 2 + 1];
  size_t needle_len = strlen(needle);
  size_t carry = 0;
  bool found = false;
  int fd;

  if (needle_len == 0)
    {
      return true;
    }

  if (needle_len > CONV_SCAN_CHUNK)
    {
      /* Longer than the window: the streaming logic below cannot guarantee a
       * match, so say so rather than answer wrongly.
       */

      printf("conv: keyword longer than %d bytes is not searchable\n",
             CONV_SCAN_CHUNK);
      return false;
    }

  snprintf(path, sizeof(path), CONV_FMT_TEXT, seq);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return false;
    }

  for (; ; )
    {
      ssize_t got = read(fd, window + carry, CONV_SCAN_CHUNK);

      if (got <= 0)
        {
          break;
        }

      window[carry + (size_t)got] = '\0';

      if (strstr(window, needle) != NULL)
        {
          found = true;
          break;
        }

      /* Keep the tail so the next read can complete a straddling match.
       *
       * The amount to keep is needle_len - 1: any shorter and a match
       * beginning in the last bytes of this window could not be completed by
       * the next read.  It is taken from the end of the *valid* data, which
       * is the previous carry plus what was just read -- not from the end of
       * what was just read.
       */

      {
        size_t total = carry + (size_t)got;
        size_t keep = needle_len - 1;

        if (keep > total)
          {
            keep = total;
          }

        memmove(window, window + total - keep, keep);
        carry = keep;
      }
    }

  close(fd);
  return found;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int conv_store_ready(void)
{
  struct stat st;

  /* The mount is deferred during bring-up, so a command can legitimately run
   * before it exists.  Name that case instead of failing as "no records".
   */

  if (stat("/mnt/sdnand", &st) < 0)
    {
      printf("conv: /mnt/sdnand is not mounted yet -- SD-NAND init is "
             "deferred at boot, check for \"SD-NAND persistent VFAT "
             "mounted\" in the log\n");
      return -ENODEV;
    }

  mkdir(CONV_DIR, 0755);
  if (mkdir(CONV_SUBDIR, 0755) < 0 && errno != EEXIST)
    {
      printf("conv: cannot create %s: %d\n", CONV_SUBDIR, errno);
      return -errno;
    }

  return OK;
}

unsigned int conv_store_next_seq(void)
{
  char line[CONV_LINE_MAX];
  unsigned int last = 0;
  FILE *f;

  f = fopen(CONV_INDEX, "r");
  if (f == NULL)
    {
      return 1;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      unsigned int seq = (unsigned int)strtoul(line, NULL, 10);

      if (seq > last)
        {
          last = seq;
        }
    }

  fclose(f);
  return last + 1;
}

int conv_store_append(struct conv_entry_s *entry, const char *transcript,
                      const char *cue_json)
{
  char path[64];
  FILE *f;

  if (entry == NULL || transcript == NULL || cue_json == NULL)
    {
      return -EINVAL;
    }

  entry->seq = conv_store_next_seq();
  entry->text_bytes = strlen(transcript);

  snprintf(path, sizeof(path), CONV_FMT_TEXT, entry->seq);
  f = fopen(path, "w");
  if (f == NULL)
    {
      printf("conv: cannot write %s: %d\n", path, errno);
      return -errno;
    }

  fputs(transcript, f);
  fclose(f);

  snprintf(path, sizeof(path), CONV_FMT_CUE, entry->seq);
  f = fopen(path, "w");
  if (f == NULL)
    {
      return -errno;
    }

  fputs(cue_json, f);
  fclose(f);

  /* Index last: see the header.  A record without an index line is
   * invisible; an index line without a complete record is a broken query.
   */

  f = fopen(CONV_INDEX, "a");
  if (f == NULL)
    {
      printf("conv: cannot append to %s: %d\n", CONV_INDEX, errno);
      return -errno;
    }

  fprintf(f, "%u|%lu|%u|%s|%.2f|%d|%zu|%s\n",
          entry->seq, (unsigned long)entry->epoch, entry->duration_ms,
          entry->cue, (double)entry->confidence,
          entry->unable_to_judge ? 1 : 0, entry->text_bytes,
          entry->summary);
  fclose(f);

  return OK;
}

int conv_store_query(const struct conv_filter_s *filter, conv_visit_t visit,
                     void *arg)
{
  char line[CONV_LINE_MAX];
  struct conv_entry_s entry;
  int matches = 0;
  FILE *f;

  if (filter == NULL || visit == NULL)
    {
      return -EINVAL;
    }

  f = fopen(CONV_INDEX, "r");
  if (f == NULL)
    {
      return errno == ENOENT ? 0 : -errno;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      size_t len = strlen(line);
      int ret;

      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
          line[--len] = '\0';
        }

      if (!conv_parse_line(line, &entry))
        {
          continue;
        }

      /* Stage one: everything decidable from the index. */

      if (filter->from_epoch != 0 && entry.epoch < filter->from_epoch)
        {
          continue;
        }

      if (filter->to_epoch != 0 && entry.epoch > filter->to_epoch)
        {
          continue;
        }

      if (filter->cue != NULL && strcmp(filter->cue, entry.cue) != 0)
        {
          continue;
        }

      if (entry.confidence < filter->min_confidence)
        {
          continue;
        }

      if (entry.unable_to_judge && !filter->include_unjudged)
        {
          continue;
        }

      /* Stage two: the summary first, because it is already in hand.
       *
       * Searching the transcript alone was the first version and it was
       * wrong in a way that looked right: a search for a topic missed the
       * conversation whose summary was about exactly that topic, because the
       * participants never said the word -- "关于爬山路线的不同意见" has no
       * 爬山 in its transcript, only "走西边那条路线".  A summary is written
       * to be searched; excluding it makes the feature miss the records a
       * person is most likely to be looking for.
       *
       * Checking it first is also free: it is a field of the line just
       * parsed, so a match here avoids opening the transcript at all.
       */

      if (filter->keyword != NULL &&
          strstr(entry.summary, filter->keyword) == NULL &&
          !conv_transcript_has(entry.seq, filter->keyword))
        {
          continue;
        }

      matches++;

      ret = visit(&entry, arg);
      if (ret != 0)
        {
          fclose(f);
          return ret;
        }
    }

  fclose(f);
  return matches;
}

int conv_store_clear(void)
{
  char line[CONV_LINE_MAX];
  char path[64];
  struct conv_entry_s entry;
  int removed = 0;
  FILE *f;

  f = fopen(CONV_INDEX, "r");
  if (f == NULL)
    {
      return errno == ENOENT ? 0 : -errno;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      size_t len = strlen(line);

      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
          line[--len] = '\0';
        }

      if (!conv_parse_line(line, &entry))
        {
          continue;
        }

      snprintf(path, sizeof(path), CONV_FMT_TEXT, entry.seq);
      unlink(path);
      snprintf(path, sizeof(path), CONV_FMT_CUE, entry.seq);
      unlink(path);

      /* Older records may still have an .OGG from when recordings were kept
       * on the card.  Removed too, so a clear leaves nothing behind.
       */

      snprintf(path, sizeof(path), CONV_SUBDIR "/C%04u.OGG", entry.seq);
      unlink(path);

      removed++;
    }

  fclose(f);

  /* The index goes last, mirroring the write order: while it exists the
   * records it names are still enumerable, so an interrupted clear can be
   * repeated rather than leaving orphans nothing knows about.
   */

  if (unlink(CONV_INDEX) < 0 && errno != ENOENT)
    {
      return -errno;
    }

  return removed;
}

int conv_store_read_raw(const char *path, char *buf, size_t len)
{
  ssize_t got;
  int fd;

  if (path == NULL || buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  got = read(fd, buf, len - 1);
  close(fd);

  if (got < 0)
    {
      return -errno;
    }

  buf[got] = '\0';
  return (int)got;
}

int conv_store_read_file(const char *fmt, unsigned int seq, char *buf,
                         size_t len)
{
  char path[64];

  if (fmt == NULL)
    {
      return -EINVAL;
    }

  snprintf(path, sizeof(path), fmt, seq);
  return conv_store_read_raw(path, buf, len);
}

unsigned int conv_epoch_to_date(uint32_t epoch)
{
  uint32_t days = epoch / 86400u;
  int year = 1970;
  int month = 0;

  for (; ; )
    {
      uint32_t ydays = CONV_IS_LEAP(year) ? 366u : 365u;

      if (days < ydays)
        {
          break;
        }

      days -= ydays;
      year++;
    }

  for (month = 0; month < 12; month++)
    {
      uint32_t mdays = (uint32_t)g_mdays[month];

      if (month == 1 && CONV_IS_LEAP(year))
        {
          mdays++;
        }

      if (days < mdays)
        {
          break;
        }

      days -= mdays;
    }

  return (unsigned int)year * 10000u + (unsigned int)(month + 1) * 100u +
         (unsigned int)days + 1u;
}

uint32_t conv_date_to_epoch(unsigned int yyyymmdd, bool end_of_day)
{
  int year = (int)(yyyymmdd / 10000u);
  int month = (int)((yyyymmdd / 100u) % 100u);
  int day = (int)(yyyymmdd % 100u);
  uint32_t days = 0;
  int y;
  int m;

  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
    {
      return 0;
    }

  for (y = 1970; y < year; y++)
    {
      days += CONV_IS_LEAP(y) ? 366u : 365u;
    }

  for (m = 0; m < month - 1; m++)
    {
      days += (uint32_t)g_mdays[m];

      if (m == 1 && CONV_IS_LEAP(year))
        {
          days++;
        }
    }

  days += (uint32_t)(day - 1);

  /* An inclusive upper bound has to reach the end of the named day, or
   * "-e today" would exclude everything recorded today.
   */

  return days * 86400u + (end_of_day ? 86399u : 0u);
}
