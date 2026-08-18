/****************************************************************************
 * app/web_tool/host/tests/test_wt_queue.c
 *
 * The drop policy is the part of this design most likely to be wrong in a way
 * nobody notices: a queue that quietly discards a response looks like a slow
 * board, and a log stream with a silent gap looks like a bug that has gone
 * away.  So the policy is tested here, off-target, where the queue can be
 * driven into every corner in milliseconds.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_protocol.h"
#include "wt_queue.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_checks;
static int g_failures;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#define CHECK(cond, ...)                                    \
  do                                                        \
    {                                                       \
      g_checks++;                                           \
      if (!(cond))                                          \
        {                                                   \
          g_failures++;                                     \
          printf("  FAIL %s:%d: ", __func__, __LINE__);     \
          printf(__VA_ARGS__);                              \
          printf("\n");                                     \
        }                                                   \
    }                                                       \
  while (0)

static uint8_t *dup_bytes(const char *s, size_t *len)
{
  size_t n = strlen(s);
  uint8_t *p = malloc(n + 1);

  memcpy(p, s, n + 1);
  *len = n;
  return p;
}

static uint8_t *blob(size_t n, uint8_t fill)
{
  uint8_t *p = malloc(n);

  memset(p, fill, n);
  return p;
}

/* ---- 1. FIFO order and ownership -------------------------------------- */

static void test_fifo(void)
{
  struct wt_queue_s q;
  struct wt_item_s it;
  size_t len;

  CHECK(wt_queue_init(&q, 4) == 0, "init");

  CHECK(wt_queue_put(&q, WT_TYPE_RSP, 1, dup_bytes("a", &len), len, 100)
        == WT_PUT_OK, "put a");
  CHECK(wt_queue_put(&q, WT_TYPE_RSP, 2, dup_bytes("b", &len), len, 100)
        == WT_PUT_OK, "put b");

  CHECK(wt_queue_get(&q, &it, 100) == 0, "get 1");
  CHECK(it.req_id == 1 && it.payload[0] == 'a', "first out is first in");
  free(it.payload);

  CHECK(wt_queue_get(&q, &it, 100) == 0, "get 2");
  CHECK(it.req_id == 2 && it.payload[0] == 'b', "second");
  free(it.payload);

  CHECK(wt_queue_get(&q, &it, 10) == -ETIMEDOUT, "empty queue times out");

  wt_queue_destroy(&q);
}

/* ---- 2. EVT_FRAME keeps at most one, newest wins ---------------------- */

static void test_frame_replaces_oldest(void)
{
  struct wt_queue_s q;
  struct wt_item_s it;
  uint32_t sent;
  uint32_t dropped;
  int i;

  wt_queue_init(&q, 8);

  for (i = 0; i < 5; i++)
    {
      CHECK(wt_queue_put(&q, WT_TYPE_EVT_FRAME, 0, blob(16, (uint8_t)i), 16,
                         0) == WT_PUT_OK, "frame %d accepted", i);
    }

  CHECK(q.count == 1, "only one frame is ever queued, count=%d", q.count);
  CHECK(q.frames == 1, "frame accounting, frames=%d", q.frames);

  CHECK(wt_queue_get(&q, &it, 100) == 0, "get frame");
  CHECK(it.payload[0] == 4, "the surviving frame is the newest (%u)",
        it.payload[0]);
  free(it.payload);

  wt_queue_frame_stats(&q, &sent, &dropped);
  CHECK(dropped == 4, "four frames were superseded, dropped=%u", dropped);
  CHECK(sent == 0, "nothing marked sent yet");

  CHECK(q.frames == 0, "frame count back to zero");

  wt_queue_destroy(&q);
}

/* ---- 3. A frame never evicts a response ------------------------------- */

static void test_frame_never_evicts_rsp(void)
{
  struct wt_queue_s q;
  size_t len;
  int i;
  uint32_t dropped;

  wt_queue_init(&q, 2);

  /* Fill with responses, which must not be droppable. */

  for (i = 0; i < 2; i++)
    {
      CHECK(wt_queue_put(&q, WT_TYPE_RSP, (uint16_t)i,
                         dup_bytes("rsp", &len), len, 100) == WT_PUT_OK,
            "rsp %d", i);
    }

  /* The frame is the one that goes, and it says so rather than blocking the
   * camera thread behind a full queue.
   */

  CHECK(wt_queue_put(&q, WT_TYPE_EVT_FRAME, 0, blob(16, 9), 16, 0)
        == WT_PUT_DROPPED, "frame dropped when queue is full of responses");

  wt_queue_frame_stats(&q, NULL, &dropped);
  CHECK(dropped == 1, "and it is counted, dropped=%u", dropped);
  CHECK(q.count == 2, "the responses are still there, count=%d", q.count);

  wt_queue_destroy(&q);
}

/* ---- 4. EVT_LOG drops are counted and reported once ------------------- */

static void test_log_drop_counted(void)
{
  struct wt_queue_s q;
  size_t len;
  int i;

  wt_queue_init(&q, 2);

  CHECK(wt_queue_put(&q, WT_TYPE_EVT_LOG, 0, dup_bytes("l0", &len), len, 0)
        == WT_PUT_OK, "log 0");
  CHECK(wt_queue_put(&q, WT_TYPE_EVT_LOG, 0, dup_bytes("l1", &len), len, 0)
        == WT_PUT_OK, "log 1");

  for (i = 0; i < 3; i++)
    {
      CHECK(wt_queue_put(&q, WT_TYPE_EVT_LOG, 0, dup_bytes("x", &len), len, 0)
            == WT_PUT_DROPPED, "log %d dropped", i + 2);
    }

  CHECK(wt_queue_take_log_dropped(&q) == 3, "three drops reported");
  CHECK(wt_queue_take_log_dropped(&q) == 0,
        "the count clears, so the notice is sent once and not repeated");

  wt_queue_destroy(&q);
}

/* ---- 5. RSP on a full queue times out rather than being dropped ------- */

static void test_rsp_times_out(void)
{
  struct wt_queue_s q;
  size_t len;
  int ret;

  wt_queue_init(&q, 2);

  wt_queue_put(&q, WT_TYPE_EVT_LOG, 0, dup_bytes("a", &len), len, 0);
  wt_queue_put(&q, WT_TYPE_EVT_LOG, 0, dup_bytes("b", &len), len, 0);

  ret = wt_queue_put(&q, WT_TYPE_RSP, 5, dup_bytes("late", &len), len, 30);
  CHECK(ret == -ETIMEDOUT, "expected -ETIMEDOUT, got %d", ret);
  CHECK(wt_queue_take_log_dropped(&q) == 0,
        "a timed-out response must not be filed as a dropped log");

  wt_queue_destroy(&q);
}

/* ---- 6. close() wakes waiters and refuses new work ------------------- */

static void test_close(void)
{
  struct wt_queue_s q;
  struct wt_item_s it;
  size_t len;

  wt_queue_init(&q, 2);
  wt_queue_close(&q);

  CHECK(wt_queue_put(&q, WT_TYPE_RSP, 1, dup_bytes("x", &len), len, 100)
        == -ESHUTDOWN, "put after close");
  CHECK(wt_queue_get(&q, &it, 100) == -ESHUTDOWN, "get after close");

  wt_queue_close(&q);           /* idempotent */
  wt_queue_destroy(&q);
}

/* ---- 7. destroy() frees what is still queued ------------------------- */

static void test_destroy_frees(void)
{
  struct wt_queue_s q;
  size_t len;

  /* Under ASan a leak here fails the run, which is the check. */

  wt_queue_init(&q, 4);
  wt_queue_put(&q, WT_TYPE_RSP, 1, dup_bytes("one", &len), len, 0);
  wt_queue_put(&q, WT_TYPE_EVT_LOG, 0, dup_bytes("two", &len), len, 0);
  wt_queue_put(&q, WT_TYPE_EVT_FRAME, 0, blob(1024, 7), 1024, 0);
  wt_queue_destroy(&q);
  CHECK(1, "no leak");
}

/* ---- 8. Log ring: lines, partial lines, CR stripping ----------------- */

static void test_logring_lines(void)
{
  struct wt_logring_s r;
  char line[WT_LOG_LINE_MAX];

  CHECK(wt_logring_init(&r, 1024) == 0, "ring init");

  wt_logring_put(&r, "first\nsecond\n", 13);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 5, "len of 'first'");
  CHECK(strcmp(line, "first") == 0, "got '%s'", line);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 6, "len of 'second'");
  CHECK(strcmp(line, "second") == 0, "got '%s'", line);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 0, "ring drained");

  /* A line that arrives in pieces must not be emitted early -- half a log
   * line in the browser reads as a different message.
   */

  wt_logring_put(&r, "par", 3);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 0,
        "partial line withheld");
  wt_logring_put(&r, "tial\n", 5);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 7, "completed");
  CHECK(strcmp(line, "partial") == 0, "got '%s'", line);

  /* CRLF from the console path, and blank lines. */

  wt_logring_put(&r, "crlf\r\n\r\n\r\nafter\n", 16);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 4, "crlf line");
  CHECK(strcmp(line, "crlf") == 0, "got '%s'", line);
  CHECK(wt_logring_getline(&r, line, sizeof(line)) == 5,
        "blank lines are skipped, not reported as 'drained'");
  CHECK(strcmp(line, "after") == 0, "got '%s'", line);

  wt_logring_free(&r);
}

/* ---- 9. Log ring: overwrite oldest, count the loss ------------------- */

static void test_logring_overwrite(void)
{
  struct wt_logring_s r;
  char line[WT_LOG_LINE_MAX];
  int i;
  uint32_t lost;

  /* 512-byte ring, 100 lines of 10 bytes: the ring has to give up the
   * beginning.  That is the right trade for a log -- the end is what you are
   * looking at -- but it must be admitted, not hidden.
   */

  CHECK(wt_logring_init(&r, 512) == 0, "ring init");

  for (i = 0; i < 100; i++)
    {
      char buf[24];

      snprintf(buf, sizeof(buf), "line%04d\n", i);
      wt_logring_put(&r, buf, strlen(buf));
    }

  lost = wt_logring_take_lost(&r);
  CHECK(lost > 0, "loss is reported, lost=%u", lost);
  CHECK(wt_logring_take_lost(&r) == 0, "and cleared on read");

  /* Whatever survives must be the newest, and the last line must be intact.
   */

  {
    char last[WT_LOG_LINE_MAX] = "";
    int n = 0;

    while (wt_logring_getline(&r, line, sizeof(line)) > 0)
      {
        strcpy(last, line);
        n++;
      }

    CHECK(n > 0 && n < 100, "kept the tail, %d lines", n);
    CHECK(strcmp(last, "line0099") == 0, "newest line intact: '%s'", last);
  }

  wt_logring_free(&r);
}

/* ---- 10. Log ring: no newline at all must not stall ----------------- */

static void test_logring_no_newline(void)
{
  struct wt_logring_s r;
  char line[32];
  char big[256];
  int ret;

  wt_logring_init(&r, 1024);
  memset(big, 'z', sizeof(big));
  wt_logring_put(&r, big, sizeof(big));

  ret = wt_logring_getline(&r, line, sizeof(line));
  CHECK(ret == (int)sizeof(line) - 1,
        "emits a full line's worth rather than stalling, got %d", ret);
  CHECK(line[0] == 'z' && line[sizeof(line) - 2] == 'z', "contents");

  wt_logring_free(&r);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  printf("wt_queue unit tests\n");

  test_fifo();
  test_frame_replaces_oldest();
  test_frame_never_evicts_rsp();
  test_log_drop_counted();
  test_rsp_times_out();
  test_close();
  test_destroy_frees();
  test_logring_lines();
  test_logring_overwrite();
  test_logring_no_newline();

  printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
