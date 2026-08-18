/****************************************************************************
 * app/web_tool/wt_selftest.c
 *
 * `web_tool selftest` -- drive the running service from the board itself.
 *
 * Why this exists.  The development machine here is on a wired subnet and the
 * only wireless network available to the board is an open guest SSID which
 * blocks traffic between the two in both directions (measured 2026-08-17:
 * inbound TCP to the board times out, and the board's own connect() to the
 * host returns ETIMEDOUT).  Without credentials for an internal SSID the whole
 * board-side stack would therefore go to acceptance never having run on the
 * hardware it was written for -- and "it passed against a mock" is not the
 * same claim.
 *
 * So the host side is replaced, for the duration of this test, by a client on
 * the board talking to 127.0.0.1.  What that covers is everything on the
 * board: accept, the whitelist, framing in both directions, every structured
 * command, the syslog channel and the log ring, the camera thread and its
 * queue, the shell gate.  What it cannot cover is the parts that are not on
 * the board -- the page, the WebSocket bridge, the capture files -- and those
 * are covered by host/tests/test_e2e.py against mock_board.py.
 *
 * It prints the numbers it measured, not verdicts alone: a frame rate of
 * "PASS" cannot be compared with next month's.
 *
 * Usage: web_tool selftest [host [port]]
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "wt_protocol.h"
#include "wt_selftest.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ST_RXCAP      (68 * 1024)
#define ST_CHUNK      1460

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct st_s
{
  int                 fd;
  struct wt_parser_s  parser;
  uint8_t            *rxbuf;
  uint16_t            next_id;

  int                 checks;
  int                 failures;

  /* Frame accounting, filled while waiting for a response. */

  unsigned int        frames;
  unsigned int        frame_bad;
  unsigned int        log_lines;
  unsigned int        dropped;
  uint32_t            first_frame_ms;
  uint32_t            last_frame_ms;
  size_t              frame_min;
  size_t              frame_max;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t st_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void st_check(struct st_s *st, bool ok, const char *what,
                     const char *fmt, ...)
{
  va_list ap;

  st->checks++;
  if (!ok)
    {
      st->failures++;
    }

  printf("[%s] %s\n       ", ok ? "PASS" : "FAIL", what);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  fflush(stdout);
}

static int st_send_req(struct st_s *st, const char *json, uint16_t *out_id)
{
  uint8_t hdr[WT_HDR_LEN];
  size_t len = strlen(json);
  uint16_t id = st->next_id++;

  if (st->next_id == 0)
    {
      st->next_id = 1;
    }

  if (wt_hdr_encode(hdr, sizeof(hdr), WT_TYPE_REQ, id, (uint32_t)len) < 0)
    {
      return -EINVAL;
    }

  if (write(st->fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
      write(st->fd, json, len) != (ssize_t)len)
    {
      return -errno;
    }

  if (out_id != NULL)
    {
      *out_id = id;
    }

  return 0;
}

/* Read frames until the response with req_id arrives or the deadline passes.
 * Events that show up on the way are counted rather than discarded -- they are
 * what the log and camera checks look at.
 */

static int st_wait_rsp(struct st_s *st, uint16_t want_id, int timeout_ms,
                       char *out, size_t outcap)
{
  uint32_t deadline = st_now_ms() + (uint32_t)timeout_ms;
  uint8_t chunk[ST_CHUNK];

  for (; ; )
    {
      ssize_t got;
      size_t off = 0;

      if ((int32_t)(st_now_ms() - deadline) >= 0)
        {
          return -ETIMEDOUT;
        }

      got = recv(st->fd, chunk, sizeof(chunk), 0);
      if (got == 0)
        {
          return -ENOTCONN;
        }

      if (got < 0)
        {
          if (errno == EINTR || errno == EAGAIN)
            {
              continue;
            }

          return -errno;
        }

      while (off < (size_t)got)
        {
          size_t consumed = 0;
          int ret = wt_parser_push(&st->parser, chunk + off,
                                   (size_t)got - off, &consumed);

          off += consumed;

          if (ret < 0)
            {
              printf("       parser rejected a header (%d): "
                     "%02x %02x %02x %02x %02x %02x %02x %02x\n", ret,
                     st->parser.hdr[0], st->parser.hdr[1], st->parser.hdr[2],
                     st->parser.hdr[3], st->parser.hdr[4], st->parser.hdr[5],
                     st->parser.hdr[6], st->parser.hdr[7]);
              return ret;
            }

          if (ret != WT_PARSE_FRAME)
            {
              if (consumed == 0)
                {
                  return -EIO;
                }

              continue;
            }

          if (st->parser.type == WT_TYPE_EVT_FRAME)
            {
              const uint8_t *jpeg = st->parser.payload + WT_FRAME_META_LEN;
              size_t jlen = st->parser.paylen - WT_FRAME_META_LEN;
              uint32_t claimed = wt_rd32(st->parser.payload + 4);
              bool good = wt_fnv1a(jpeg, jlen) == claimed &&
                          jpeg[0] == 0xff && jpeg[1] == 0xd8 &&
                          jpeg[jlen - 2] == 0xff && jpeg[jlen - 1] == 0xd9;

              st->frames++;
              if (!good)
                {
                  st->frame_bad++;
                }

              if (st->frame_min == 0 || jlen < st->frame_min)
                {
                  st->frame_min = jlen;
                }

              if (jlen > st->frame_max)
                {
                  st->frame_max = jlen;
                }

              st->last_frame_ms = st_now_ms();
              if (st->first_frame_ms == 0)
                {
                  st->first_frame_ms = st->last_frame_ms;
                }

              continue;
            }

          if (st->parser.type == WT_TYPE_EVT_LOG)
            {
              st->log_lines++;
              if (strstr((const char *)st->parser.payload, "dropped") != NULL)
                {
                  st->dropped++;
                }

              continue;
            }

          if (st->parser.type == WT_TYPE_RSP)
            {
              if (strstr((const char *)st->parser.payload, "dropped") != NULL
                  && st->parser.req_id == 0)
                {
                  st->dropped++;
                  continue;
                }

              if (st->parser.req_id != want_id)
                {
                  continue;
                }

              if (out != NULL)
                {
                  size_t n = st->parser.paylen < outcap - 1
                             ? st->parser.paylen : outcap - 1;

                  memcpy(out, st->parser.payload, n);
                  out[n] = '\0';
                }

              return 0;
            }

          /* PONG and anything else: not what we are waiting for. */
        }
    }
}

/* Read and account for a fixed period, which is what the real host does all
 * the time.  The first version of this test slept instead, so the socket
 * filled up and the board -- correctly -- gave up on frames nobody was
 * reading; a harness whose client behaves unlike the real one measures the
 * harness.
 */

static int st_drain(struct st_s *st, int ms)
{
  uint32_t deadline = st_now_ms() + (uint32_t)ms;

  while ((int32_t)(st_now_ms() - deadline) < 0)
    {
      /* Waiting for a response id that never arrives is exactly "read and
       * count events until the deadline".
       */

      int ret = st_wait_rsp(st, 0xffff, 250, NULL, 0);

      if (ret < 0 && ret != -ETIMEDOUT)
        {
          return ret;
        }
    }

  return 0;
}

static int st_cmd(struct st_s *st, const char *json, int timeout_ms,
                  char *out, size_t outcap)
{
  uint16_t id = 0;
  int ret;

  /* Clear first.  On the first board run a timed-out camera.start left the
   * previous response in this buffer, and the failure line printed that
   * stale JSON -- which sent the reader looking for a mismatched-response bug
   * that did not exist.  A harness that can lie about what it saw is worse
   * than no harness.
   */

  if (out != NULL && outcap > 0)
    {
      out[0] = '\0';
    }

  ret = st_send_req(st, json, &id);
  if (ret < 0)
    {
      return ret;
    }

  return st_wait_rsp(st, id, timeout_ms, out, outcap);
}

static int st_connect(const char *host, int port)
{
  struct sockaddr_in addr;
  struct timeval tv;
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0)
    {
      return -errno;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      close(fd);
      return -EINVAL;
    }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      int err = -errno;

      close(fd);
      return err;
    }

  /* Long enough for a 5 fps camera to deliver, short enough that a wedged
   * service fails the test instead of hanging it.
   */

  tv.tv_sec = 6;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wt_selftest_run(const char *host, int port)
{
  struct st_s *st;
  char rsp[1024];
  int ret;
  int failures;

  st = calloc(1, sizeof(*st));
  if (st == NULL)
    {
      printf("selftest: out of memory\n");
      return 1;
    }

  st->rxbuf = malloc(ST_RXCAP);
  if (st->rxbuf == NULL)
    {
      free(st);
      printf("selftest: out of memory for the receive buffer\n");
      return 1;
    }

  st->next_id = 1;
  wt_parser_init(&st->parser, st->rxbuf, ST_RXCAP);

  printf("web_tool selftest -> %s:%d\n"
         "(the host side is replaced by this client; the page and the "
         "capture files are covered by host/tests/test_e2e.py)\n\n",
         host, port);

  ret = st_connect(host, port);
  if (ret < 0)
    {
      printf("[FAIL] connect to %s:%d\n       errno %d -- if this is "
             "127.0.0.1 make sure web.allow includes it\n", host, port, -ret);
      free(st->rxbuf);
      free(st);
      return 1;
    }

  st->fd = ret;
  st_check(st, true, "connected", "%s:%d accepted this address", host, port);

  /* ---- sys.status ---------------------------------------------------- */

  ret = st_cmd(st, "{\"cmd\":\"sys.status\"}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"ok\":true") != NULL,
           "sys.status", "ret=%d %.360s", ret, ret == 0 ? rsp : "");

  /* ---- kvdb round trip ----------------------------------------------- */

  ret = st_cmd(st, "{\"cmd\":\"kvdb.set\",\"args\":"
                   "{\"key\":\"selftest.k\",\"value\":\"v1\"}}",
               5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"ok\":true") != NULL,
           "kvdb.set", "ret=%d %.200s", ret, rsp);

  ret = st_cmd(st, "{\"cmd\":\"kvdb.get\",\"args\":{\"key\":\"selftest.k\"}}",
               5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"value\":\"v1\"") != NULL,
           "kvdb.get returns what was set", "ret=%d %.200s", ret, rsp);

  ret = st_cmd(st, "{\"cmd\":\"kvdb.get\",\"args\":{\"key\":\"no.such.key\"}}",
               5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"errname\":\"ENOENT\"") != NULL
           && strstr(rsp, "\"errno\":-2") != NULL,
           "a missing key answers ENOENT with number and name",
           "%.200s", rsp);

  ret = st_cmd(st, "{\"cmd\":\"kvdb.del\",\"args\":{\"key\":\"selftest.k\"}}",
               5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"ok\":true") != NULL,
           "kvdb.del", "%.200s", rsp);

  /* Masking: set a value under a .key name and check it comes back short. */

  (void)st_cmd(st, "{\"cmd\":\"kvdb.set\",\"args\":"
                   "{\"key\":\"selftest.key\","
                   "\"value\":\"sk-0123456789abcdef0123\"}}",
               5000, rsp, sizeof(rsp));

  ret = st_cmd(st, "{\"cmd\":\"kvdb.get\",\"args\":"
                   "{\"key\":\"selftest.key\"}}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"masked\":true") != NULL
           && strstr(rsp, "bytes)") != NULL
           && strstr(rsp, "abcdef0123\"") == NULL,
           "a .key value is masked but still recognisable", "%.220s", rsp);

  ret = st_cmd(st, "{\"cmd\":\"kvdb.get\",\"args\":"
                   "{\"key\":\"selftest.key\",\"raw\":true}}",
               5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "sk-0123456789abcdef0123") != NULL,
           "raw:true shows it in full", "%.220s", rsp);

  (void)st_cmd(st, "{\"cmd\":\"kvdb.del\",\"args\":"
                   "{\"key\":\"selftest.key\"}}", 5000, rsp, sizeof(rsp));

  ret = st_cmd(st, "{\"cmd\":\"kvdb.list\"}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"items\"") != NULL,
           "kvdb.list", "%.300s", rsp);

  /* ---- wifi.status --------------------------------------------------- */

  ret = st_cmd(st, "{\"cmd\":\"wifi.status\"}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"running\"") != NULL
           && strstr(rsp, "\"ip\"") != NULL,
           "wifi.status has the documented fields", "%.300s", rsp);

  /* ---- log subscription and replay ----------------------------------- */

  ret = st_cmd(st, "{\"cmd\":\"log.subscribe\",\"args\":{\"on\":true}}",
               8000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"replayed\"") != NULL,
           "log.subscribe replays the ring",
           "%.200s  (the ring fills from start-up whether or not anyone "
           "is subscribed, which is what makes replay possible)", rsp);

  /* ---- unknown command and bad geometry ------------------------------ */

  ret = st_cmd(st, "{\"cmd\":\"nonsense.cmd\"}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"errname\":\"ENOSYS\"") != NULL,
           "an unknown command answers ENOSYS", "%.200s", rsp);

  ret = st_cmd(st, "{\"cmd\":\"camera.start\",\"args\":"
                   "{\"width\":320,\"height\":240}}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"errname\":\"EINVAL\"") != NULL,
           "an unsupported geometry is refused, not silently changed",
           "%.220s", rsp);

  ret = st_cmd(st, "{\"cmd\":\"nonsense\":}", 5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"errname\":\"EINVAL\"") != NULL,
           "malformed JSON is refused without dropping the link",
           "%.200s", rsp);

  /* ---- shell passthrough and its gate -------------------------------- */

  st->log_lines = 0;
  ret = st_cmd(st, "{\"cmd\":\"shell.exec\","
                   "\"args\":{\"cmdline\":\"sleep 2; free\"}}",
               5000, rsp, sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"accepted\":true") != NULL,
           "shell.exec accepted", "%.200s", rsp);

  {
    char busy[512];
    int r2 = st_cmd(st, "{\"cmd\":\"shell.exec\",\"args\":"
                        "{\"cmdline\":\"ps\"}}", 5000, busy, sizeof(busy));

    st_check(st, r2 == 0 && strstr(busy, "\"err\":\"busy\"") != NULL,
             "a second shell.exec answers busy",
             "%.200s  (passthrough can start any app on the board; without "
             "this gate a few clicks leave a dozen tasks running)", busy);
  }

  /* Let the relayed output arrive.  sys.status is just a way to spend a
   * second inside the receive loop, where the log events are counted.
   */

  (void)st_drain(st, 4000);

  (void)st_cmd(st, "{\"cmd\":\"shell.kill\"}", 4000, rsp, sizeof(rsp));
  st_check(st, st->log_lines > 0,
           "the shell output came back as log events",
           "%u log event(s) seen, %u dropped notice(s)",
           st->log_lines, st->dropped);

  /* ---- camera -------------------------------------------------------- */

  st->frames = 0;
  st->frame_bad = 0;
  st->frame_min = 0;
  st->frame_max = 0;
  st->first_frame_ms = 0;
  st->last_frame_ms = 0;

  ret = st_cmd(st, "{\"cmd\":\"camera.start\",\"args\":"
                   "{\"width\":640,\"height\":480}}", 30000, rsp,
               sizeof(rsp));
  st_check(st, ret == 0 && strstr(rsp, "\"ok\":true") != NULL,
           "camera.start 640x480",
           "ret=%d %.200s  (the first open runs the sensor bring-up: 585 "
           "bit-banged I2C writes, so a cold start is slow)", ret, rsp);

  if (ret == 0 && strstr(rsp, "\"ok\":true") != NULL)
    {
      char again[256];
      int r2 = st_cmd(st, "{\"cmd\":\"camera.start\",\"args\":"
                          "{\"width\":640,\"height\":480}}",
                      5000, again, sizeof(again));

      st_check(st, r2 == 0 && strstr(again, "\"errname\":\"EBUSY\"") != NULL,
               "a second camera.start says EBUSY", "%.200s", again);

      /* Ten seconds of nothing but reading, which is what the host does. */

      (void)st_drain(st, 10000);

      ret = st_cmd(st, "{\"cmd\":\"camera.stop\"}", 8000, rsp, sizeof(rsp));

      if (st->frames >= 2 && st->last_frame_ms > st->first_frame_ms)
        {
          uint32_t span = st->last_frame_ms - st->first_frame_ms;
          unsigned int mfps = (st->frames - 1) * 1000u * 100u / span;

          st_check(st, mfps >= 350 && mfps <= 600,
                   "preview frame rate",
                   "%u frames in %lu ms -> %u.%02u fps, sizes %zu..%zu "
                   "bytes, %u failed the checksum or the markers\n"
                   "       camera.stop -> %.160s\n"
                   "       expected about 5 fps: the driver paces at "
                   "CONFIG_BK7258_CAMERA_JPEG_FPS and web_tool adds no "
                   "throttle of its own",
                   st->frames, (unsigned long)span, mfps / 100, mfps % 100,
                   st->frame_min, st->frame_max, st->frame_bad, rsp);
        }
      else
        {
          st_check(st, false, "preview frame rate",
                   "only %u frame(s) arrived; camera.stop -> %.160s",
                   st->frames, rsp);
        }

      st_check(st, st->frames > 0 && st->frame_bad == 0,
               "every frame verified end to end on the board",
               "%u frame(s), %u bad -- FNV-1a over the JPEG plus SOI/EOI, "
               "the same two checks the host performs", st->frames,
               st->frame_bad);
    }

  /* ---- PING / PONG --------------------------------------------------- */

  {
    uint8_t hdr[WT_HDR_LEN];

    wt_hdr_encode(hdr, sizeof(hdr), WT_TYPE_PING, 0x4242, 0);
    ret = (int)write(st->fd, hdr, sizeof(hdr));

    /* The PONG is consumed inside the wait loop, so ask for something else
     * and check the link is still healthy afterwards.
     */

    ret = st_cmd(st, "{\"cmd\":\"sys.status\"}", 5000, rsp, sizeof(rsp));
    st_check(st, ret == 0,
             "PING does not disturb the command stream", "ret=%d", ret);
  }

  /* ---- a frame that declares more than the limit --------------------- */

  /* Last, because the correct response is to drop the connection.  Built by
   * hand: wt_hdr_encode() refuses to produce it, which is the point.
   */

  {
    uint8_t bad[WT_HDR_LEN];
    ssize_t got;
    uint8_t sink[64];

    bad[0] = WT_TYPE_EVT_FRAME;
    bad[1] = 0;
    bad[2] = 0;
    bad[3] = 0;
    bad[4] = 0x01;
    bad[5] = 0x00;
    bad[6] = 0x01;
    bad[7] = 0x00;                     /* 64 KB + 1 */

    (void)write(st->fd, bad, sizeof(bad));

    /* Drain until the far end closes.  Frames already in flight may arrive
     * first, so read until EOF or the timeout.
     */

    for (; ; )
      {
        got = recv(st->fd, sink, sizeof(sink), 0);
        if (got <= 0)
          {
            break;
          }
      }

    st_check(st, got == 0,
             "an over-long frame drops the connection",
             "recv returned %d -- there is no resync that is not a guess, "
             "and a wrong guess yields frames assembled out of the middle "
             "of a JPEG", (int)got);
  }

  close(st->fd);

  failures = st->failures;
  printf("\n%d check(s), %d failure(s)\n", st->checks, failures);

  free(st->rxbuf);
  free(st);
  return failures == 0 ? 0 : 1;
}
