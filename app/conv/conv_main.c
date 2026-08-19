/****************************************************************************
 * app/conv/conv_main.c
 *
 * Conversation history on the board: seed it, query it locally, ship the
 * matches to the web console.
 *
 * The point of running the query here rather than on the PC is that the
 * recordings are here.  Shipping every conversation to the web so it can
 * filter them would move megabytes to answer a question whose answer is a
 * few hundred bytes, over a link that also has to carry live audio.  The
 * board reads its own index -- one sequential pass over a few kilobytes --
 * and sends only what matched.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "conv_net.h"
#include "conv_store.h"
#include "conv_ws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Buffer for one transcript or one analysis when printing a record. */

#define CONV_TEXT_MAX     1024

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What a seeded record is made of.  Held as data rather than built by code
 * so the set is easy to read and extend.
 *
 * day_offset is days before today, resolved against the date the caller
 * supplies -- the board has no clock, so "today" has to come from outside.
 */

struct conv_seed_s
{
  int day_offset;
  unsigned int hour;
  unsigned int minute;
  unsigned int duration_ms;
  const char *cue;
  const char *meaning;
  float confidence;
  bool unable_to_judge;
  const char *reason;
  const char *suggestion;
  const char *summary;
  const char *transcript;
};

/* Six conversations across a fortnight, so a date range has something to
 * include and something to exclude.
 *
 * The cue vocabulary and the hedged `meaning` wording follow social-cue/v1
 * as social_cue_skill.h defines it: "possible confusion", never "confused".
 * One record carries unable_to_judge, because a history that never contains
 * an inconclusive analysis would misrepresent what the model actually
 * returns, and the query has to cope with it.
 */

static const struct conv_seed_s g_seeds[] =
{
  {
    -13, 9, 42, 8000, "brow_furrow", "possible confusion", 0.72f, false,
    "", "slow down and check whether the last point landed",
    "周末爬山的安排",
    "对方: 周末那个爬山还去吗\n"
    "我: 去啊，几点集合\n"
    "对方: 早上六点，太早了点\n"
    "我: 六点可以，我定个闹钟\n"
    "对方: 那行，我到时候联系你\n"
  },
  {
    -9, 14, 5, 12000, "gaze_aversion", "possible discomfort", 0.64f, false,
    "", "give them room and avoid pressing the topic",
    "项目排期的分歧",
    "对方: 这个排期我觉得有点紧\n"
    "我: 哪一块最紧\n"
    "对方: 测试那块，只留了两天\n"
    "我: 那我们把测试挪到下周\n"
    "对方: 嗯，那样好一些\n"
    "我: 我今天改一下计划发你\n"
  },
  {
    -6, 10, 20, 6000, "smile", "possible agreement", 0.81f, false,
    "", "a good moment to confirm the next step",
    "确认了下周的评审时间",
    "对方: 下周三下午可以吗\n"
    "我: 可以，三点行不行\n"
    "对方: 三点没问题\n"
    "我: 那我发个日历邀请\n"
  },
  {
    -4, 16, 48, 15000, "head_shake", "possible disagreement", 0.69f, false,
    "", "ask what specifically does not work before restating",
    "关于爬山路线的不同意见",
    "对方: 我还是觉得走西边那条路线不合适\n"
    "我: 为什么呢\n"
    "对方: 那边石头多，下雨天滑\n"
    "我: 那我们走东边，虽然远一点\n"
    "对方: 远点没关系，安全重要\n"
  },
  {
    -2, 11, 15, 4000, "", "", 0.31f, true,
    "face partially out of frame", "reposition before judging",
    "画面不完整，未能判断",
    "对方: 那个东西你带了吗\n"
    "我: 带了，在包里\n"
  },
  {
    0, 15, 30, 9000, "lean_forward", "possible interest", 0.77f, false,
    "", "expand on the point that drew them in",
    "聊到爬山装备",
    "对方: 你那双鞋在哪买的\n"
    "我: 网上买的，牌子我回去发你\n"
    "对方: 好，我也想换一双\n"
    "我: 爬山鞋还是别省，脚舒服很重要\n"
    "对方: 有道理\n"
  },
};

#define CONV_NSEEDS (sizeof(g_seeds) / sizeof(g_seeds[0]))

/* Visitor state for the listing and sending paths. */

struct conv_list_ctx_s
{
  bool as_json;
  bool first;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void conv_usage(void)
{
  printf(
    "Usage: conv <time|llm|seed|list|find|get|serve|clear> [options]\n"
    "\n"
    "  time                     show what the clock believes\n"
    "  time <epoch>             set it by hand\n"
    "  time -s <ip> <port>      set it from the web console's /api/time\n"
    "        This board has no RTC, so CLOCK_REALTIME starts at zero every\n"
    "        boot and has to be set before anything is recorded.  Setting the\n"
    "        system clock rather than keeping a private offset means\n"
    "        time(NULL) is right everywhere afterwards.\n"
    "\n"
    "  seed [-d <YYYYMMDD>]\n"
    "        write %u sample conversations, dated relative to the day given.\n"
    "        -d is only needed while the clock is unset; after 'conv time'\n"
    "        today's date is taken from the clock.\n"
    "        -a attaches an existing recording to every record; make one\n"
    "        with 'audio_test opus -t 3 -o /mnt/sdnand/SEED.OGG'\n"
    "\n"
    "  list  print the whole index\n"
    "\n"
    "  find [-s <YYYYMMDD>] [-e <YYYYMMDD>] [-c <cue>] [-k <keyword>]\n"
    "       [-m <confidence>] [-u]\n"
    "        -s/-e  inclusive date range\n"
    "        -c     social-cue/v1 cue, e.g. brow_furrow\n"
    "        -k     substring of the transcript\n"
    "        -m     minimum overall_confidence, e.g. 0.7\n"
    "        -u     include records the model could not judge\n"
    "\n"
    "  get <seq>\n"
    "        print one conversation's transcript and analysis\n"
    "\n"
    "  llm                      show the configured model, key masked\n"
    "  llm <host> <model> <key> set them, e.g.\n"
    "        conv llm api.xiaomimimo.com mimo-v2.5 sk-xxxx\n"
    "\n"
    "  serve <ip> <port>\n"
    "        stay connected to the web console over WebSocket, set the clock\n"
    "        from its hello_ack, and answer the queries it forwards.  This is\n"
    "        what lets the browser search history; the commands above only\n"
    "        work from this shell.\n"
    "\n"
    "  clear --confirm\n"
    "        delete every record and recording\n"
    "\n"
    "  -j    print results as JSON instead of a table\n",
    (unsigned int)CONV_NSEEDS);
}

/****************************************************************************
 * Name: conv_print_entry
 ****************************************************************************/

static int conv_print_entry(const struct conv_entry_s *entry, void *arg)
{
  struct conv_list_ctx_s *ctx = arg;

  if (ctx->as_json)
    {
      /* Built with printf rather than a serialiser: the board only ever
       * writes JSON, never reads it, and writing it is this cheap.
       */

      printf("%s{\"seq\":%u,\"date\":%u,\"epoch\":%lu,\"duration_ms\":%u,"
             "\"cue\":\"%s\",\"confidence\":%.2f,\"unable_to_judge\":%s,"
             "\"text_bytes\":%zu,\"summary\":\"%s\"}",
             ctx->first ? "" : ",\n",
             entry->seq, conv_epoch_to_date(entry->epoch),
             (unsigned long)entry->epoch, entry->duration_ms,
             entry->cue, (double)entry->confidence,
             entry->unable_to_judge ? "true" : "false",
             entry->text_bytes, entry->summary);
    }
  else
    {
      printf("  %3u  %u  %2u.%03us  %-14s %.2f%s  %5zuB  %s\n",
             entry->seq, conv_epoch_to_date(entry->epoch),
             entry->duration_ms / 1000, entry->duration_ms % 1000,
             entry->cue[0] != '\0' ? entry->cue : "(none)",
             (double)entry->confidence,
             entry->unable_to_judge ? "?" : " ",
             entry->text_bytes, entry->summary);
    }

  ctx->first = false;
  return 0;
}

/****************************************************************************
 * Name: conv_seed
 *
 * Description:
 *   Write the sample set, dated relative to a day the caller supplies.
 *
 *   Each record gets a real Ogg Opus recording rather than a placeholder, so
 *   that what the web console receives is a file it can actually play and
 *   the transfer path is exercised on real data.  A tone is not a
 *   conversation, but it is a valid recording of the right shape and size.
 *
 ****************************************************************************/

static int conv_seed(unsigned int today)
{
  uint32_t base = conv_date_to_epoch(today, false);
  unsigned int i;
  int ret;

  if (base == 0)
    {
      printf("conv: -d wants a date like 20260818\n");
      return -EINVAL;
    }

  for (i = 0; i < CONV_NSEEDS; i++)
    {
      const struct conv_seed_s *s = &g_seeds[i];
      struct conv_entry_s entry;
      char cue_json[512];

      memset(&entry, 0, sizeof(entry));

      /* day_offset is negative, and base is unsigned: add the seconds rather
       * than subtracting into a wrap.
       */

      entry.epoch = base + (uint32_t)(s->day_offset * 86400) +
                    s->hour * 3600u + s->minute * 60u;
      entry.duration_ms = s->duration_ms;
      entry.confidence = s->confidence;
      entry.unable_to_judge = s->unable_to_judge;
      strncpy(entry.cue, s->cue, sizeof(entry.cue) - 1);
      strncpy(entry.summary, s->summary, sizeof(entry.summary) - 1);

      snprintf(cue_json, sizeof(cue_json),
               "{\n"
               "  \"schema\": \"social-cue/v1\",\n"
               "  \"cues\": [\n"
               "    { \"cue\": \"%s\", \"meaning\": \"%s\","
               " \"confidence\": %.2f }\n"
               "  ],\n"
               "  \"overall_confidence\": %.2f,\n"
               "  \"unable_to_judge\": %s,\n"
               "  \"reason\": \"%s\",\n"
               "  \"suggestion\": \"%s\"\n"
               "}\n",
               s->cue, s->meaning, (double)s->confidence,
               (double)s->confidence,
               s->unable_to_judge ? "true" : "false",
               s->reason, s->suggestion);

      ret = conv_store_append(&entry, s->transcript, cue_json);
      if (ret < 0)
        {
          printf("conv: writing record %u failed: %d\n", i + 1, ret);
          return ret;
        }

      printf("conv: wrote %u  %u  %s  %s\n", entry.seq,
             conv_epoch_to_date(entry.epoch),
             entry.cue[0] != '\0' ? entry.cue : "(unjudged)", entry.summary);
    }

  return OK;
}

/****************************************************************************
 * Name: conv_get
 ****************************************************************************/

static int conv_get(unsigned int seq)
{
  char buf[CONV_TEXT_MAX];
  int ret;

  ret = conv_store_read_file(CONV_FMT_TEXT, seq, buf, sizeof(buf));
  if (ret < 0)
    {
      printf("conv: record %u has no transcript: %d\n", seq, ret);
      return ret;
    }

  printf("--- transcript %u ---\n%s", seq, buf);

  ret = conv_store_read_file(CONV_FMT_CUE, seq, buf, sizeof(buf));
  if (ret >= 0)
    {
      printf("--- analysis %u ---\n%s", seq, buf);
    }

  return OK;
}

/****************************************************************************
 * Name: conv_parse_filter
 *
 * Description:
 *   Fill a filter from argv, starting at argv[i].
 *
 * Returned Value:
 *   Zero on success, -EINVAL on a bad option.
 *
 ****************************************************************************/

static int conv_parse_filter(int argc, char *argv[], int start,
                             struct conv_filter_s *filter, bool *as_json)
{
  int i;

  memset(filter, 0, sizeof(*filter));

  for (i = start; i < argc; i++)
    {
      if (strcmp(argv[i], "-j") == 0)
        {
          *as_json = true;
          continue;
        }

      if (strcmp(argv[i], "-u") == 0)
        {
          filter->include_unjudged = true;
          continue;
        }

      if (i + 1 >= argc)
        {
          printf("conv: %s wants a value\n", argv[i]);
          return -EINVAL;
        }

      if (strcmp(argv[i], "-s") == 0)
        {
          filter->from_epoch =
            conv_date_to_epoch((unsigned int)strtoul(argv[i + 1], NULL, 10),
                               false);
          if (filter->from_epoch == 0)
            {
              printf("conv: -s wants a date like 20260801\n");
              return -EINVAL;
            }
        }
      else if (strcmp(argv[i], "-e") == 0)
        {
          /* Inclusive: the end of that day, not its midnight. */

          filter->to_epoch =
            conv_date_to_epoch((unsigned int)strtoul(argv[i + 1], NULL, 10),
                               true);
          if (filter->to_epoch == 0)
            {
              printf("conv: -e wants a date like 20260818\n");
              return -EINVAL;
            }
        }
      else if (strcmp(argv[i], "-c") == 0)
        {
          filter->cue = argv[i + 1];
        }
      else if (strcmp(argv[i], "-k") == 0)
        {
          filter->keyword = argv[i + 1];
        }
      else if (strcmp(argv[i], "-m") == 0)
        {
          filter->min_confidence = strtof(argv[i + 1], NULL);
        }
      else
        {
          printf("conv: unknown option %s\n", argv[i]);
          return -EINVAL;
        }

      i++;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct conv_filter_s filter;
  struct conv_list_ctx_s ctx;
  bool as_json = false;
  int ret;

  if (argc < 2)
    {
      conv_usage();
      return EXIT_FAILURE;
    }

  ret = conv_store_ready();
  if (ret < 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "seed") == 0)
    {
      unsigned int today = 0;
      int i;

      for (i = 2; i + 1 < argc; i += 2)
        {
          if (strcmp(argv[i], "-d") == 0)
            {
              today = (unsigned int)strtoul(argv[i + 1], NULL, 10);
            }
          else
            {
              conv_usage();
              return EXIT_FAILURE;
            }
        }

      /* Once the clock has been set there is no reason to make the caller
       * repeat today's date, and asking for it anyway would invite a typo
       * that silently dates the whole sample set wrongly.
       */

      if (today == 0 && conv_clock_synced())
        {
          today = conv_epoch_to_date((uint32_t)time(NULL));
          printf("conv: using today's date from the clock: %u\n", today);
        }

      if (today == 0)
        {
          printf("conv: the clock is not set, so seed needs -d <YYYYMMDD>. "
                 "Set the clock instead with 'conv time -s <ip> <port>' and "
                 "-d becomes optional.\n");
          return EXIT_FAILURE;
        }

      return conv_seed(today) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "clear") == 0)
    {
      /* Guarded by an explicit flag, the same way sdnand_init guards its
       * provision step.  This deletes recordings that cannot be recovered,
       * and a one-word command that does that is one typo away from losing
       * a session someone cared about.
       */

      if (argc < 3 || strcmp(argv[2], "--confirm") != 0)
        {
          printf("conv: this deletes every record and its recording.\n"
                 "      Run 'conv clear --confirm' if that is what you "
                 "want.\n");
          return EXIT_FAILURE;
        }

      ret = conv_store_clear();
      if (ret < 0)
        {
          printf("conv: clear failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("conv: removed %d record(s)\n", ret);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "time") == 0)
    {
      if (argc == 2)
        {
          conv_clock_report();
          return EXIT_SUCCESS;
        }

      if (strcmp(argv[2], "-s") == 0)
        {
          if (argc < 5)
            {
              printf("Usage: conv time -s <ip> <port>\n");
              return EXIT_FAILURE;
            }

          return conv_clock_fetch(argv[3], atoi(argv[4])) < 0 ?
                 EXIT_FAILURE : EXIT_SUCCESS;
        }

      return conv_clock_set((uint32_t)strtoul(argv[2], NULL, 10)) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "get") == 0)
    {
      if (argc < 3)
        {
          printf("Usage: conv get <seq>\n");
          return EXIT_FAILURE;
        }

      return conv_get((unsigned int)strtoul(argv[2], NULL, 10)) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "serve") == 0)
    {
      if (argc < 4)
        {
          printf("Usage: conv serve <ip> <port>\n");
          return EXIT_FAILURE;
        }

      return conv_serve(argv[2], atoi(argv[3])) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "llm") == 0)
    {
      char status[256];

      if (argc == 2)
        {
          conv_llm_report(status, sizeof(status));
          printf("conv: %s\n", status);
          return EXIT_SUCCESS;
        }

      if (argc < 5)
        {
          printf("Usage: conv llm <host> <model> <key>\n"
                 "   eg: conv llm api.xiaomimimo.com mimo-v2.5 sk-xxxx\n");
          return EXIT_FAILURE;
        }

      return conv_llm_set(argv[2], argv[3], argv[4]) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "find") == 0)
    {
      if (conv_parse_filter(argc, argv, 2, &filter, &as_json) < 0)
        {
          return EXIT_FAILURE;
        }

      /* `list` means everything, including what the model could not judge --
       * hiding those from a plain listing would make records disappear with
       * no way to notice.
       */

      if (strcmp(argv[1], "list") == 0)
        {
          filter.include_unjudged = true;
        }

      ctx.as_json = as_json;
      ctx.first = true;

      if (as_json)
        {
          printf("[\n");
        }
      else
        {
          printf("  seq  date      dur      cue            conf  audio   "
                 "summary\n");
        }

      ret = conv_store_query(&filter, conv_print_entry, &ctx);

      if (as_json)
        {
          printf("\n]\n");
        }

      if (ret < 0)
        {
          printf("conv: query failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      if (!as_json)
        {
          printf("conv: %d match(es)\n", ret);
        }

      return EXIT_SUCCESS;
    }

  conv_usage();
  return EXIT_FAILURE;
}
