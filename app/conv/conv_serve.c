/****************************************************************************
 * app/conv/conv_serve.c
 *
 * Staying connected to the web console and answering the queries it forwards.
 *
 * Until this existed the board could only be asked from its own shell: the
 * query ran where the recordings are, which is right, but the only way to
 * start one was to type it on the serial console.  The browser had no way to
 * reach it.  This closes that: the console forwards a query, the board runs
 * it against its index, and the result goes back over the same connection.
 *
 * Recordings do not go back that way.  The console's frame limit is 16 KB and
 * a recording is tens of kilobytes, so audio is pushed over HTTP on demand --
 * one record at a time, only when the browser opens one, because most rows in
 * a result are never opened.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netutils/netlib.h>

#include "conv_net.h"
#include "conv_store.h"
#include "conv_ws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Room for one outgoing message.  Bounded by what the console accepts, and
 * checked while filling rather than after: a result set that would overflow
 * is truncated with a flag, not silently cut in the middle of a JSON object.
 */

#define SERVE_OUT_MAX     CONV_WS_MAX_MSG

/* Longest string value read out of an incoming message.
 *
 * Sized for the longest of them, which is the API key: a rejected key that
 * was merely truncated on the way in is a failure nobody would think to look
 * for here.
 */

#define SERVE_FIELD_MAX   CONV_KEY_MAX

/* Ceiling on one transcript.
 *
 * Escaped it can grow, and the frame it goes in is capped at 16 KB, so this
 * leaves room for both. A conversation longer than this is truncated on the
 * way out of the file, which conv_store_read_raw does silently -- acceptable
 * because 4 KB is far more dialogue than a single exchange produces, and the
 * alternative is a frame the console rejects whole.
 */

#define SERVE_TEXT_MAX    4096

/* The interface the address is renewed on.  Matches what bring-up creates and
 * what the `renew wlan0` in every set of instructions here refers to.
 */

#define SERVE_IFNAME      "wlan0"

/* Retries reported in full before the log goes quiet, and how often it speaks
 * afterwards.  At the two-to-ten second backoff below, every 30th attempt is
 * roughly one line a minute while a console is down.
 */

#define SERVE_LOUD_RETRIES  3
#define SERVE_QUIET_EVERY   30

/* Consecutive unreachable-network failures before the address is renewed.
 *
 * Three rather than one: a single ENETUNREACH happens while the interface is
 * still coming up after a reconnect, and renewing then would fight the DHCP
 * client that is already working.
 */

#define SERVE_NOROUTE_LIMIT 3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct serve_ctx_s
{
  char *out;
  size_t cap;
  size_t len;
  int emitted;
  bool truncated;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: serve_json_int / serve_json_str
 *
 * Description:
 *   Pull one field out of a JSON message by name.
 *
 *   Substring matching, not parsing.  The peer is one known server sending
 *   messages this project defines, and a parser plus its working memory is a
 *   poor trade for reading four fields.  The cost is that a *value* which
 *   contains the text of another key could mislead it -- a keyword of
 *   `"cue":"x"` for instance.  That is accepted here because the fields are
 *   read left to right from the key that was asked for, so only a value
 *   deliberately crafted to look like a key can do it, and this connection
 *   carries no untrusted input.  If it ever does, this is the first thing to
 *   replace.
 *
 ****************************************************************************/

static bool serve_json_int(const char *msg, const char *key, long *value)
{
  char pattern[SERVE_FIELD_MAX];
  const char *p;

  snprintf(pattern, sizeof(pattern), "\"%s\"", key);

  p = strstr(msg, pattern);
  if (p == NULL)
    {
      return false;
    }

  p = strchr(p, ':');
  if (p == NULL)
    {
      return false;
    }

  *value = strtol(p + 1, NULL, 10);
  return true;
}

static bool serve_json_str(const char *msg, const char *key, char *out,
                           size_t len)
{
  char pattern[SERVE_FIELD_MAX];
  const char *p;
  size_t o = 0;

  snprintf(pattern, sizeof(pattern), "\"%s\"", key);

  p = strstr(msg, pattern);
  if (p == NULL)
    {
      return false;
    }

  p = strchr(p, ':');
  if (p == NULL)
    {
      return false;
    }

  p++;
  while (*p == ' ')
    {
      p++;
    }

  if (*p != '"')
    {
      return false;
    }

  p++;
  while (*p != '\0' && *p != '"' && o + 1 < len)
    {
      /* Escapes are passed through as-is except for the quote, which is the
       * only one that would end the value early.  The console does not send
       * escaped keywords today; anything else arriving is left for the
       * comparison to fail on rather than being silently altered.
       */

      if (*p == '\\' && p[1] == '"')
        {
          out[o++] = '"';
          p += 2;
          continue;
        }

      out[o++] = *p++;
    }

  out[o] = '\0';
  return o > 0;
}

/****************************************************************************
 * Name: serve_append
 *
 * Description:
 *   Append to the outgoing message, refusing to overflow.
 *
 * Returned Value:
 *   True if it fitted.
 *
 ****************************************************************************/

static bool serve_append(struct serve_ctx_s *ctx, const char *fmt, ...)
{
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(ctx->out + ctx->len, ctx->cap - ctx->len, fmt, ap);
  va_end(ap);

  if (n < 0 || (size_t)n >= ctx->cap - ctx->len)
    {
      /* Roll back the partial write: half a JSON object is worse than a
       * short list with a flag saying it was cut.
       */

      ctx->out[ctx->len] = '\0';
      ctx->truncated = true;
      return false;
    }

  ctx->len += (size_t)n;
  return true;
}

/****************************************************************************
 * Name: serve_visit
 ****************************************************************************/

static int serve_visit(const struct conv_entry_s *entry, void *arg)
{
  struct serve_ctx_s *ctx = arg;

  if (ctx->truncated)
    {
      return 0;
    }

  serve_append(ctx,
               "%s{\"seq\":%u,\"date\":%u,\"epoch\":%lu,"
               "\"durationMs\":%u,\"cue\":\"%s\",\"confidence\":%.2f,"
               "\"unableToJudge\":%s,\"textBytes\":%zu,"
               "\"summary\":\"%s\"}",
               ctx->emitted > 0 ? "," : "",
               entry->seq, conv_epoch_to_date(entry->epoch),
               (unsigned long)entry->epoch, entry->duration_ms,
               entry->cue, (double)entry->confidence,
               entry->unable_to_judge ? "true" : "false",
               entry->text_bytes, entry->summary);

  if (!ctx->truncated)
    {
      ctx->emitted++;
    }

  return 0;
}

/****************************************************************************
 * Name: serve_handle_query
 ****************************************************************************/

static int serve_handle_query(struct conv_ws_s *ws, const char *msg,
                              char *out, size_t outcap)
{
  struct conv_filter_s filter;
  struct serve_ctx_s ctx;
  char request_id[SERVE_FIELD_MAX];
  char cue[CONV_CUE_MAX];
  char keyword[SERVE_FIELD_MAX];
  long value;
  int ret;

  memset(&filter, 0, sizeof(filter));

  if (!serve_json_str(msg, "requestId", request_id, sizeof(request_id)))
    {
      strncpy(request_id, "unknown", sizeof(request_id) - 1);
      request_id[sizeof(request_id) - 1] = '\0';
    }

  /* Dates arrive as YYYYMMDD, the same form the command line takes, so the
   * console and the shell cannot disagree about what a date means.
   */

  if (serve_json_int(msg, "from", &value) && value > 0)
    {
      filter.from_epoch = conv_date_to_epoch((unsigned int)value, false);
    }

  if (serve_json_int(msg, "to", &value) && value > 0)
    {
      filter.to_epoch = conv_date_to_epoch((unsigned int)value, true);
    }

  if (serve_json_str(msg, "cue", cue, sizeof(cue)))
    {
      filter.cue = cue;
    }

  if (serve_json_str(msg, "keyword", keyword, sizeof(keyword)))
    {
      filter.keyword = keyword;
    }

  if (serve_json_int(msg, "minConfidence", &value))
    {
      /* Sent as a percentage so the field stays an integer: JSON floats
       * would work but this avoids a locale-sensitive strtof on a value that
       * only ever has two digits of meaning.
       */

      filter.min_confidence = (float)value / 100.0f;
    }

  filter.include_unjudged = strstr(msg, "\"includeUnjudged\":true") != NULL;

  ctx.out = out;
  ctx.cap = outcap;
  ctx.len = 0;
  ctx.emitted = 0;
  ctx.truncated = false;

  serve_append(&ctx, "{\"type\":\"query_result\",\"requestId\":\"%s\","
                     "\"records\":[", request_id);

  ret = conv_store_query(&filter, serve_visit, &ctx);
  if (ret < 0)
    {
      snprintf(out, outcap,
               "{\"type\":\"query_result\",\"requestId\":\"%s\","
               "\"error\":\"query failed: %d\"}", request_id, ret);
      return conv_ws_send_text(ws, out);
    }

  serve_append(&ctx, "],\"matched\":%d,\"returned\":%d,\"truncated\":%s}",
               ret, ctx.emitted, ctx.truncated ? "true" : "false");

  if (ctx.truncated)
    {
      printf("conv: %d match(es) did not fit one frame, sent %d\n", ret,
             ctx.emitted);
    }
  else
    {
      printf("conv: query %s -> %d match(es)\n", request_id, ret);
    }

  return conv_ws_send_text(ws, out);
}

/****************************************************************************
 * Name: serve_json_escape
 *
 * Description:
 *   Copy text into a JSON string body, escaping what has to be escaped.
 *
 *   A transcript contains newlines -- it is a dialogue, one line per turn --
 *   and a raw newline inside a JSON string is invalid.  Emitting one produced
 *   a frame the console rejected as malformed, which looked like a transport
 *   fault rather than a quoting one.
 *
 * Returned Value:
 *   Characters written, not counting the terminator.  Stops early rather than
 *   overflowing.
 *
 ****************************************************************************/

static size_t serve_json_escape(const char *src, char *dst, size_t dstlen)
{
  size_t o = 0;

  while (*src != '\0' && o + 7 < dstlen)
    {
      unsigned char c = (unsigned char)*src++;

      switch (c)
        {
          case '"':
            dst[o++] = '\\';
            dst[o++] = '"';
            break;

          case '\\':
            dst[o++] = '\\';
            dst[o++] = '\\';
            break;

          case '\n':
            dst[o++] = '\\';
            dst[o++] = 'n';
            break;

          case '\r':
            dst[o++] = '\\';
            dst[o++] = 'r';
            break;

          case '\t':
            dst[o++] = '\\';
            dst[o++] = 't';
            break;

          default:

            /* Other control characters have to be escaped numerically; UTF-8
             * bytes above 0x7f are already valid inside a JSON string and are
             * passed through untouched, which is what keeps Chinese text
             * intact.
             */

            if (c < 0x20)
              {
                o += (size_t)snprintf(dst + o, dstlen - o, "\\u%04x", c);
              }
            else
              {
                dst[o++] = (char)c;
              }

            break;
        }
    }

  dst[o] = '\0';
  return o;
}

/****************************************************************************
 * Name: serve_handle_fetch
 *
 * Description:
 *   Return one record's full transcript.
 *
 *   Over the same WebSocket, not over HTTP: without recordings to move, a
 *   record is text, and text fits a frame.  That removed the upload endpoint,
 *   the file store beside the console and the second transport along with it.
 *
 *   On demand rather than with the query: a result carries summaries, and most
 *   rows are never opened.
 *
 ****************************************************************************/

static int serve_handle_fetch(struct conv_ws_s *ws, const char *msg,
                              char *out, size_t outcap)
{
  char request_id[SERVE_FIELD_MAX];
  char *text;
  char *escaped;
  long seq = 0;
  int got;
  int ret;

  if (!serve_json_str(msg, "requestId", request_id, sizeof(request_id)))
    {
      strncpy(request_id, "unknown", sizeof(request_id) - 1);
      request_id[sizeof(request_id) - 1] = '\0';
    }

  if (!serve_json_int(msg, "seq", &seq) || seq <= 0)
    {
      snprintf(out, outcap,
               "{\"type\":\"fetch_done\",\"requestId\":\"%s\","
               "\"error\":\"fetch needs a seq\"}", request_id);
      return conv_ws_send_text(ws, out);
    }

  /* From the heap: a transcript plus its escaped form is several kilobytes,
   * and this runs on an 8 KiB stack.
   */

  text = malloc(SERVE_TEXT_MAX);
  escaped = malloc(SERVE_TEXT_MAX * 2);
  if (text == NULL || escaped == NULL)
    {
      free(text);
      free(escaped);
      snprintf(out, outcap,
               "{\"type\":\"fetch_done\",\"requestId\":\"%s\",\"seq\":%ld,"
               "\"error\":\"out of memory\"}", request_id, seq);
      return conv_ws_send_text(ws, out);
    }

  got = conv_store_read_file(CONV_FMT_TEXT, (unsigned int)seq, text,
                             SERVE_TEXT_MAX);
  if (got < 0)
    {
      snprintf(out, outcap,
               "{\"type\":\"fetch_done\",\"requestId\":\"%s\",\"seq\":%ld,"
               "\"error\":\"no transcript for %ld\"}", request_id, seq, seq);
    }
  else
    {
      serve_json_escape(text, escaped, SERVE_TEXT_MAX * 2);
      snprintf(out, outcap,
               "{\"type\":\"fetch_done\",\"requestId\":\"%s\",\"seq\":%ld,"
               "\"transcript\":\"%s\"}", request_id, seq, escaped);
      printf("conv: sent transcript %ld (%d bytes)\n", seq, got);
    }

  free(text);
  free(escaped);

  ret = conv_ws_send_text(ws, out);
  return ret;
}

/****************************************************************************
 * Name: serve_handle_set_llm
 *
 * Description:
 *   Store the model host, name and API key the console sent.
 *
 *   The key never comes back out: the reply carries a prefix and a length,
 *   the same shape the agent's config_show uses.  A console that could read
 *   the key back would put it in every screenshot of the page.
 *
 ****************************************************************************/

static int serve_handle_set_llm(struct conv_ws_s *ws, const char *msg,
                                char *out, size_t outcap)
{
  char request_id[SERVE_FIELD_MAX];
  char host[CONV_HOST_MAX];
  char model[CONV_MODEL_MAX];
  char key[CONV_KEY_MAX];
  char status[256];
  int ret;

  if (!serve_json_str(msg, "requestId", request_id, sizeof(request_id)))
    {
      strncpy(request_id, "unknown", sizeof(request_id) - 1);
      request_id[sizeof(request_id) - 1] = '\0';
    }

  if (!serve_json_str(msg, "host", host, sizeof(host)) ||
      !serve_json_str(msg, "model", model, sizeof(model)) ||
      !serve_json_str(msg, "key", key, sizeof(key)))
    {
      snprintf(out, outcap,
               "{\"type\":\"llm_status\",\"requestId\":\"%s\","
               "\"error\":\"host, model and key are all required\"}",
               request_id);
      return conv_ws_send_text(ws, out);
    }

  ret = conv_llm_set(host, model, key);

  /* Overwritten before anything else can happen with it.  The buffer is on
   * this stack and the stack is reused; a key left there is a key that can
   * turn up in a later dump.
   */

  memset(key, 0, sizeof(key));

  if (ret < 0)
    {
      snprintf(out, outcap,
               "{\"type\":\"llm_status\",\"requestId\":\"%s\","
               "\"error\":\"could not store: %d\"}", request_id, ret);
      return conv_ws_send_text(ws, out);
    }

  conv_llm_report(status, sizeof(status));
  snprintf(out, outcap,
           "{\"type\":\"llm_status\",\"requestId\":\"%s\",\"saved\":true,"
           "\"config\":%s}", request_id, status);

  return conv_ws_send_text(ws, out);
}

/****************************************************************************
 * Name: serve_handle_get_llm
 ****************************************************************************/

static int serve_handle_get_llm(struct conv_ws_s *ws, const char *msg,
                                char *out, size_t outcap)
{
  char request_id[SERVE_FIELD_MAX];
  char status[256];

  if (!serve_json_str(msg, "requestId", request_id, sizeof(request_id)))
    {
      strncpy(request_id, "unknown", sizeof(request_id) - 1);
      request_id[sizeof(request_id) - 1] = '\0';
    }

  conv_llm_report(status, sizeof(status));
  snprintf(out, outcap,
           "{\"type\":\"llm_status\",\"requestId\":\"%s\",\"config\":%s}",
           request_id, status);

  return conv_ws_send_text(ws, out);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: serve_session
 *
 * Description:
 *   One connection's worth of work: register, take the clock, answer queries
 *   until it drops.
 *
 * Returned Value:
 *   Zero if the peer closed, a negated errno if the connection could not be
 *   established or failed.
 *
 ****************************************************************************/

static int serve_session(const char *host, int port, char *in, char *out)
{
  struct conv_ws_s ws;
  char hello[256];
  long epoch;
  int ret;

  ret = conv_ws_connect(&ws, host, port);
  if (ret < 0)
    {
      return ret;
    }

  snprintf(hello, sizeof(hello),
           "{\"type\":\"hello\",\"role\":\"board\",\"deviceId\":\"%s\","
           "\"name\":\"%s\"}", "bk7258-001", "VelaSight BK7258");

  ret = conv_ws_send_text(&ws, hello);
  if (ret < 0)
    {
      conv_ws_close(&ws);
      return ret;
    }

  printf("conv: registered with %s:%d, waiting for queries\n", host, port);

  for (; ; )
    {
      ret = conv_ws_recv_text(&ws, in, CONV_WS_MAX_MSG, 1000);

      if (ret == 0)
        {
          continue;
        }

      if (ret < 0)
        {
          if (ret == -ENOTCONN)
            {
              printf("conv: the console closed the connection\n");
              ret = OK;
            }

          break;
        }

      if (strstr(in, "\"hello_ack\"") != NULL)
        {
          /* The console sends the time with the acknowledgement, which is the
           * earliest it is available.  Setting the clock here means anything
           * recorded afterwards is dated correctly without a separate step.
           */

          if (serve_json_int(in, "epoch", &epoch) && epoch > 0)
            {
              conv_clock_set((uint32_t)epoch);
            }
          else
            {
              printf("conv: registered, but the console sent no time; dates "
                     "will be wrong until 'conv time' is used\n");
            }

          continue;
        }

      if (strstr(in, "\"query\"") != NULL)
        {
          serve_handle_query(&ws, in, out, SERVE_OUT_MAX);
          continue;
        }

      if (strstr(in, "\"fetch\"") != NULL)
        {
          serve_handle_fetch(&ws, in, out, SERVE_OUT_MAX);
          continue;
        }

      if (strstr(in, "\"set_llm\"") != NULL)
        {
          serve_handle_set_llm(&ws, in, out, SERVE_OUT_MAX);
          continue;
        }

      if (strstr(in, "\"get_llm\"") != NULL)
        {
          serve_handle_get_llm(&ws, in, out, SERVE_OUT_MAX);
          continue;
        }

      if (strstr(in, "\"error\"") != NULL)
        {
          printf("conv: console reported: %.120s\n", in);
          continue;
        }

      /* Anything else is the console's own traffic -- set_volume and the
       * like -- which this command has no business answering.
       */
    }

  conv_ws_close(&ws);
  return ret;
}

/****************************************************************************
 * Name: serve_report_failure
 *
 * Description:
 *   Report a failed attempt, loudly at first and then rarely.
 *
 *   Printing every attempt was a mistake: a console that was down overnight
 *   produced a line every two seconds, and by the time anyone looked the
 *   scrollback held nothing but those lines -- the boot messages and the
 *   original error had been pushed out, so the log was least useful exactly
 *   when it was needed.  The first few attempts are what diagnoses the
 *   problem; after that the only new information is that it is still
 *   happening, which is worth one line a minute, not thirty.
 *
 ****************************************************************************/

static void serve_report_failure(const char *host, int port, int ret,
                                 unsigned int failures)
{
  const char *why;

  /* Name the two cases that matter, because they call for different actions
   * and the bare number does not say which is which: 111 means bring the
   * console up, 101 means fix this board's network.
   */

  switch (-ret)
    {
      case ECONNREFUSED:
        why = "没人监听，控制台没起来";
        break;

      case ENETUNREACH:
      case EHOSTUNREACH:
      case ENETDOWN:
        why = "路由不到，本机可能没有地址";
        break;

      default:
        why = "连接失败";
        break;
    }

  if (failures <= SERVE_LOUD_RETRIES)
    {
      printf("conv: %s:%d %s (%d), retry %u\n", host, port, why, ret,
             failures);
    }
  else if (failures % SERVE_QUIET_EVERY == 0)
    {
      printf("conv: still retrying %s:%d, %s (%d), attempt %u\n", host, port,
             why, ret, failures);
    }
}

int conv_serve(const char *host, int port)
{
  char *in;
  char *out;
  unsigned int failures = 0;
  unsigned int noroute = 0;
  int ret;

  /* Two 16 KB buffers from the heap rather than the stack: this runs from an
   * NSH command whose stack is 8 KB.  Allocated once around the reconnect
   * loop, so a reconnect does not depend on the heap being able to hand back
   * 32 KB at the moment the link drops.
   */

  in = malloc(CONV_WS_MAX_MSG);
  out = malloc(SERVE_OUT_MAX);
  if (in == NULL || out == NULL)
    {
      free(in);
      free(out);
      printf("conv: no memory for the message buffers\n");
      return -ENOMEM;
    }

  printf("conv: serving history to %s:%d, Ctrl-C to stop\n", host, port);

  /* Reconnects rather than returning.
   *
   * One session was observed to drop right after a successful upload and it
   * did not reproduce on the next attempt, so the cause is not established.
   * That is precisely the case for reconnecting: a board that gives up
   * permanently on a link that flickers once needs someone at the serial
   * console to notice and retype the command, which during a demonstration
   * means the feature is simply gone.  Whatever the cause turns out to be,
   * surviving it is correct.
   */

  for (; ; )
    {
      ret = serve_session(host, port, in, out);

      if (ret == OK)
        {
          failures = 0;
          noroute = 0;
          printf("conv: connection closed, reconnecting\n");
        }
      else
        {
          failures++;

          /* Distinguish "nobody listening" from "no way to get there".
           *
           * ECONNREFUSED means the console is down and retrying is the whole
           * answer.  ENETUNREACH means this board has no route -- it lost its
           * address -- and retrying the connect can never fix that.  Observed
           * in practice: the console was brought back up and the board went on
           * failing 170 times, because the retry loop was solving the wrong
           * problem.  The address has to be re-acquired.
           */

          if (ret == -ENETUNREACH || ret == -EHOSTUNREACH ||
              ret == -ENETDOWN)
            {
              noroute++;
            }
          else
            {
              noroute = 0;
            }

          serve_report_failure(host, port, ret, failures);

          if (noroute >= SERVE_NOROUTE_LIMIT)
            {
              /* The same call the `renew` command is: apps/system/dhcpc's
               * renew_main.c is a wrapper around this one line.
               */

              printf("conv: no route for %u attempts, renewing the DHCP "
                     "lease on %s\n", noroute, SERVE_IFNAME);

              if (netlib_obtain_ipv4addr(SERVE_IFNAME) < 0)
                {
                  printf("conv: renew failed; is the AP still in range?\n");
                }
              else
                {
                  printf("conv: got an address again\n");
                }

              noroute = 0;
            }
        }

      /* Back off a little after repeated failures so an unreachable console
       * is not hammered, but stay responsive to one that is merely
       * restarting -- which during development it does constantly.
       */

      sleep(failures > 3 ? 10 : 2);
    }

  /* Not reached: the loop ends only by signal, which is how NSH's Ctrl-C
   * stops it.  The buffers are reclaimed by task teardown.
   */
}
