/****************************************************************************
 * app/web_tool/web_tool_main.c
 *
 * `web_tool` -- a TCP service that lets a browser on the development machine
 * configure, drive and watch this board.
 *
 * What it is not: it does not speak HTTP, does not know what a WebSocket is,
 * does not serve a page and does not write files.  All of that is the
 * development machine's job (host/console.py).  The board's side of the line
 * is exactly this: accept one connection from a whitelisted address, turn
 * request frames into calls on kvdb / V4L2 / syslog / the shell, and push
 * responses, log lines and JPEG frames back.  Keeping HTTP off the board is
 * what makes the page editable without reflashing.
 *
 * Concurrency: four producers, one bounded queue, one sender thread.  See
 * wt_queue.h for why nobody writes the socket directly and why each class of
 * traffic gets its own drop policy.
 *
 * Access control: the source address must match kvdb's `web.allow`.  With no
 * such key the service does not listen at all.  A permissive default would
 * turn the only access control in this design into decoration, and would
 * create the state where someone believes a whitelist is in force while the
 * board is in fact open to a /23.  The cost is that the first setup has to go
 * through the serial console once.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/boardctl.h>
#include <sys/socket.h>

#include <nuttx/syslog/syslog.h>

#include <arch/board/kvdb.h>

#include "wt_command.h"
#include "wt_protocol.h"
#include "wt_queue.h"
#include "wt_io.h"
#include "wt_selftest.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Registering a second channel when there is only one slot *replaces* the
 * first one (syslog_channel.c:408), and the first one is the serial console.
 * That would take away the boot log and the rescue path -- the two things the
 * design says the serial link exists for -- and it would happen silently.
 * Fail the build instead.
 */

#if !defined(CONFIG_SYSLOG_MAX_CHANNELS) || CONFIG_SYSLOG_MAX_CHANNELS < 2
#  error "web_tool needs CONFIG_SYSLOG_MAX_CHANNELS >= 2"
#endif

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_WEB_TOOL_PORT
#  define WT_DEFAULT_PORT   CONFIG_LVX_USE_DEMO_CONTEST2026_264_WEB_TOOL_PORT
#else
#  define WT_DEFAULT_PORT   8888
#endif

#define WT_ALLOW_KEY        "web.allow"

/* Where the console is, and which certificate it is allowed to present.  Both
 * live in kvdb for the same reason every other setting does: they are typed by
 * a human once, and compiling them in would put them in a public repository.
 */

#define WT_HOST_KEY         "web.host"
#define WT_PORT_KEY         "web.port"
#define WT_FP_KEY           "web.fp"
#define WT_TOKEN_KEY        "web.token"

#define WT_DEFAULT_OUT_PORT 8899
#define WT_ALLOW_MAX        8
#define WT_ALLOW_VALUE_MAX  256

/* Queue depth.  One frame plus room for a burst of log lines and the
 * responses in flight.  Frames are capped at one by policy, not by depth, so
 * this is 16 * (a JSON body) plus at most one 33 KB frame.
 */

#define WT_QUEUE_DEPTH      16

/* 64 KB of log backlog: enough to hold this board's whole boot transcript,
 * which is the point -- the host usually connects after the boot it wants to
 * read.
 */

#define WT_LOGRING_BYTES    (64 * 1024)

/* How long a producer of an undroppable frame waits for queue space before
 * the connection is declared dead.
 */

#define WT_RSP_TIMEOUT_MS   5000

/* Log lines moved from the ring into the queue per sender iteration.  Bounded
 * so a large backlog cannot starve frames and responses.
 */

#define WT_LOG_BATCH        16

#define WT_SEND_TIMEOUT_S   10

/* How long the sender waits for room before giving up on a frame.  Short on
 * purpose: at 5 fps the next frame is 200 ms away, so waiting longer than
 * this can only make the preview stale, never smoother.
 */

#define WT_FRAME_WRITE_WAIT_MS 150

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wt_allow_s
{
  in_addr_t addr;
  in_addr_t mask;
};

struct wt_session_s
{
  struct wt_io_s  *io;         /* plaintext or TLS; the loop cannot tell */
  char             origin[48]; /* who this session is with, for the log  */
  struct wt_ctx_s  ctx;
  struct wt_queue_s queue;
  pthread_t        sender;
  volatile bool    stop;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The syslog ring outlives every connection and is never freed: the syslog
 * channel keeps a pointer to it, and a channel cannot be unregistered from
 * interrupt context.  One 64 KB allocation for the life of the process is the
 * price of being able to log from anywhere.
 */

static struct wt_logring_s g_logring;
static bool g_logring_ready;

static struct wt_allow_s g_allow[WT_ALLOW_MAX];
static int g_nallow;

static struct wt_session_s *g_session;
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions: syslog channel
 ****************************************************************************/

/* Only ever a memcpy into the ring.  syslog() can be called from interrupt
 * context and from inside kernel locks; touching a socket or taking a mutex
 * here is the classic way to deadlock a system through its logging path.
 */

static int wt_syslog_putc(syslog_channel_t *channel, int ch)
{
  char c = (char)ch;

  UNUSED(channel);
  wt_logring_put(&g_logring, &c, 1);
  return ch;
}

static ssize_t wt_syslog_write(syslog_channel_t *channel,
                               const char *buffer, size_t length)
{
  UNUSED(channel);
  wt_logring_put(&g_logring, buffer, length);
  return (ssize_t)length;
}

static int wt_syslog_flush(syslog_channel_t *channel)
{
  UNUSED(channel);
  return OK;
}

static const struct syslog_channel_ops_s g_wt_syslog_ops =
{
  .sc_putc        = wt_syslog_putc,
  .sc_force       = wt_syslog_putc,
  .sc_flush       = wt_syslog_flush,
  .sc_write       = wt_syslog_write,
  .sc_write_force = wt_syslog_write,
  .sc_close       = NULL,
};

static syslog_channel_t g_wt_syslog_channel =
{
  .sc_ops = &g_wt_syslog_ops,
};

/****************************************************************************
 * Private Functions: whitelist
 ****************************************************************************/

/* Accepts a single address or a CIDR block, several separated by commas.  A
 * prefix length is allowed because the development machine's address is handed
 * out by DHCP and pinning a whole /21 is sometimes the only workable answer;
 * it still narrows the exposure from "anyone who can route to the board" to
 * "one office subnet", which is the point.
 */

static int wt_allow_parse(const char *spec)
{
  char work[WT_ALLOW_VALUE_MAX];
  char *save = NULL;
  char *tok;
  int n = 0;

  strlcpy(work, spec, sizeof(work));

  for (tok = strtok_r(work, ",", &save);
       tok != NULL && n < WT_ALLOW_MAX;
       tok = strtok_r(NULL, ",", &save))
    {
      struct in_addr in;
      char *slash;
      int prefix = 32;

      while (*tok == ' ')
        {
          tok++;
        }

      slash = strchr(tok, '/');
      if (slash != NULL)
        {
          *slash = '\0';
          prefix = atoi(slash + 1);
          if (prefix < 0 || prefix > 32)
            {
              printf("web_tool: %s has a bad prefix length, ignored\n", tok);
              continue;
            }
        }

      if (inet_pton(AF_INET, tok, &in) != 1)
        {
          printf("web_tool: %s is not an IPv4 address, ignored\n", tok);
          continue;
        }

      g_allow[n].mask = prefix == 0 ? 0 :
                        htonl(0xffffffffu << (32 - prefix));
      g_allow[n].addr = in.s_addr & g_allow[n].mask;
      n++;
    }

  return n;
}

static bool wt_allow_match(in_addr_t peer)
{
  int i;

  for (i = 0; i < g_nallow; i++)
    {
      if ((peer & g_allow[i].mask) == g_allow[i].addr)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Private Functions: socket helpers
 ****************************************************************************/


/****************************************************************************
 * Name: wt_writable
 *
 * Description:
 *   Whether the socket can take a frame right now.  Asked before writing an
 *   EVT_FRAME, and only for EVT_FRAME.
 *
 *   The reason is that a frame is the one thing here that may be dropped, but
 *   it may only be dropped *before* any of its bytes are on the wire: half a
 *   frame followed by the next header desynchronises the stream, and there is
 *   no recovery from that which is not a guess.  So the decision has to be
 *   taken in advance, which is what this is for.
 *
 *   The first board run of this service did it the other way round -- it
 *   started writing and let SO_SNDTIMEO decide -- and a client that stopped
 *   reading for ten seconds got the whole connection dropped, preview and
 *   command channel together, because of a frame nobody was looking at.
 *
 ****************************************************************************/

static bool wt_writable(struct wt_io_s *io, int timeout_ms)
{
  struct pollfd pfd;

  pfd.fd = wt_io_fd(io);
  pfd.events = POLLOUT;
  pfd.revents = 0;

  return poll(&pfd, 1, timeout_ms) == 1 && (pfd.revents & POLLOUT) != 0;
}

static int wt_send_frame(struct wt_io_s *io, uint8_t type, uint16_t req_id,
                         const uint8_t *payload, size_t len)
{
  uint8_t hdr[WT_HDR_LEN];
  int ret;

  if (wt_hdr_encode(hdr, sizeof(hdr), type, req_id, (uint32_t)len) < 0)
    {
      return -EINVAL;
    }

  ret = wt_io_write(io, hdr, sizeof(hdr));
  if (ret < 0 || len == 0)
    {
      return ret;
    }

  return wt_io_write(io, payload, len);
}

/****************************************************************************
 * Private Functions: sender thread
 ****************************************************************************/

/* Moves whole log lines out of the ring and into the queue.  Bounded per
 * call, and only while someone is subscribed -- the ring keeps filling either
 * way, which is what makes log.subscribe able to replay the past.
 */

static void wt_pump_logs(struct wt_session_s *s, char *line, char *body,
                         size_t bodycap)
{
  uint32_t dropped;
  int i;

  if (!s->ctx.log_on)
    {
      return;
    }

  for (i = 0; i < WT_LOG_BATCH; i++)
    {
      uint8_t *copy;
      int n;

      if (wt_logring_getline(&g_logring, line, WT_LOG_LINE_MAX) <= 0)
        {
          break;
        }

      n = wt_json_log(body, bodycap, wt_now_ms(), line);
      if (n <= 0)
        {
          continue;
        }

      copy = malloc((size_t)n);
      if (copy == NULL)
        {
          break;
        }

      memcpy(copy, body, (size_t)n);
      if (wt_queue_put(&s->queue, WT_TYPE_EVT_LOG, 0, copy,
                       (size_t)n, 0) < 0)
        {
          break;
        }
    }

  /* Two independent ways to lose a log line, reported the same way: the ring
   * overwrote bytes nobody had read yet, and the send queue was full.  Both
   * become a "dropped" notice, because from the far end they are the same
   * fact: there is a gap here.
   */

  dropped = wt_queue_take_log_dropped(&s->queue);
  if (wt_logring_take_lost(&g_logring) > 0)
    {
      dropped++;
    }

  if (dropped > 0)
    {
      int n = wt_json_dropped(body, bodycap, wt_now_ms(), dropped);

      if (n > 0)
        {
          uint8_t *copy = malloc((size_t)n);

          if (copy != NULL)
            {
              memcpy(copy, body, (size_t)n);

              /* Queued as a response, not as a log: the whole purpose of this
               * message is to be the one thing that never gets dropped.
               */

              wt_queue_put(&s->queue, WT_TYPE_RSP, 0, copy, (size_t)n,
                           WT_RSP_TIMEOUT_MS);
            }
        }
    }
}

static void *wt_sender_thread(void *arg)
{
  struct wt_session_s *s = arg;
  char *line = malloc(WT_LOG_LINE_MAX);
  char *body = malloc(WT_LOG_BODY_MAX);

  if (line == NULL || body == NULL)
    {
      syslog(LOG_ERR, "web_tool: sender out of memory\n");
      free(line);
      free(body);
      s->stop = true;
      wt_io_shutdown(s->io);
      return NULL;
    }

  while (!s->stop)
    {
      struct wt_item_s item;
      int ret;

      wt_pump_logs(s, line, body, WT_LOG_BODY_MAX);

      ret = wt_queue_get(&s->queue, &item, 50);
      if (ret == -ETIMEDOUT)
        {
          continue;
        }

      if (ret < 0)
        {
          break;                /* closed */
        }

      if (item.type == WT_TYPE_EVT_FRAME &&
          !wt_writable(s->io, WT_FRAME_WRITE_WAIT_MS))
        {
          /* The peer is not keeping up.  Drop this frame and keep the
           * connection: preview wants "now", and the command channel must
           * not die because a browser tab was busy.
           */

          wt_queue_frame_drop(&s->queue);
          free(item.payload);
          continue;
        }

      ret = wt_send_frame(s->io, item.type, item.req_id, item.payload,
                          item.len);

      if (item.type == WT_TYPE_EVT_FRAME && ret == 0)
        {
          wt_queue_frame_sent(&s->queue);
        }

      free(item.payload);

      if (ret < 0)
        {
          syslog(LOG_INFO, "web_tool: send failed (%d), dropping client\n",
                 ret);
          break;
        }

      /* Same ordering argument as the reboot below: re-associating tears down
       * this very socket, so it can only happen after the answer is out.
       */

      if (s->ctx.wifi_pending)
        {
          wt_wifi_apply_pending(&s->ctx);
        }

      /* A reboot is only safe once the response is really out of the socket,
       * which is here.  Doing it in the handler would race the answer.
       */

      if (s->ctx.reboot_pending)
        {
          syslog(LOG_INFO, "web_tool: rebooting on request\n");
          usleep(200000);
          boardctl(BOARDIOC_RESET, 0);
        }
    }

  s->stop = true;
  free(line);
  free(body);

  /* Unblock a reader parked in read(). */

  wt_io_shutdown(s->io);
  return NULL;
}

/****************************************************************************
 * Private Functions: session
 ****************************************************************************/

static void wt_session_run(struct wt_io_s *io, const char *origin)
{
  struct wt_session_s *s;
  struct wt_parser_s parser;
  uint8_t *rxbuf;
  uint8_t chunk[1460];
  struct timeval tv;
  int one = 1;

  s = calloc(1, sizeof(*s));
  rxbuf = malloc(WT_MAX_PAYLOAD);

  if (s == NULL || rxbuf == NULL)
    {
      free(s);
      free(rxbuf);
      wt_io_close(io);
      return;
    }

  s->io = io;
  strlcpy(s->origin, origin, sizeof(s->origin));

  if (wt_queue_init(&s->queue, WT_QUEUE_DEPTH) < 0)
    {
      free(s);
      free(rxbuf);
      wt_io_close(io);
      return;
    }

  s->ctx.queue   = &s->queue;
  s->ctx.logring = &g_logring;
  s->ctx.t0_ms   = wt_now_ms();
  pthread_mutex_init(&s->ctx.shell_lock, NULL);

  /* Nagle off: the traffic here is small request/response frames where an
   * extra round trip of latency is visible in the page, and large frames that
   * fill segments by themselves anyway.
   */

  setsockopt(wt_io_fd(io), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  tv.tv_sec  = WT_SEND_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(wt_io_fd(io), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  wt_parser_init(&parser, rxbuf, WT_MAX_PAYLOAD);

  pthread_mutex_lock(&g_session_lock);
  g_session = s;
  pthread_mutex_unlock(&g_session_lock);

  {
    pthread_attr_t sattr;
    int cret;

    /* Explicit, because the default is 4096 and this thread formats log lines.
     * Sizing threads by hoping the default is enough is how a stack overflow
     * ends up looking like a protocol error.
     */

    pthread_attr_init(&sattr);
    pthread_attr_setstacksize(&sattr, 4096);
    cret = pthread_create(&s->sender, &sattr, wt_sender_thread, s);
    pthread_attr_destroy(&sattr);

    if (cret != 0)
      {
        syslog(LOG_ERR, "web_tool: could not start sender thread (%d)\n",
               cret);
        goto out;
      }
  }

  syslog(LOG_INFO, "web_tool: session with %s over %s\n",
         s->origin, wt_io_describe(io));

  while (!s->stop)
    {
      int got = wt_io_read(s->io, chunk, sizeof(chunk));
      size_t off = 0;

      if (got == 0)
        {
          break;                /* orderly close */
        }

      if (got < 0)
        {
          /* A read timeout on an idle console is not a dead link.  Treating
           * it as one would disconnect anyone who stopped typing for half a
           * minute.
           */

          if (got == -EAGAIN || got == -EINTR)
            {
              continue;
            }

          break;
        }

      while (off < (size_t)got && !s->stop)
        {
          size_t consumed = 0;
          int pret = wt_parser_push(&parser, chunk + off,
                                    (size_t)got - off, &consumed);

          off += consumed;

          if (pret < 0)
            {
              /* Framing is gone.  There is no resync that is not a guess, and
               * a guess produces frames assembled from the middle of a
               * payload, so the connection goes instead.
               */

              syslog(LOG_ERR, "web_tool: bad frame (%d) from client, "
                              "dropping connection\n", pret);
              s->stop = true;
              break;
            }

          if (pret != WT_PARSE_FRAME)
            {
              if (consumed == 0)
                {
                  s->stop = true;
                  break;
                }

              continue;
            }

          if (parser.type == WT_TYPE_PING)
            {
              wt_queue_put(&s->queue, WT_TYPE_PONG, parser.req_id, NULL, 0,
                           WT_RSP_TIMEOUT_MS);
              continue;
            }

          if (parser.type != WT_TYPE_REQ)
            {
              syslog(LOG_WARNING, "web_tool: ignoring type 0x%02x from "
                                  "client\n", parser.type);
              continue;
            }

          {
            char *rsp = wt_command_dispatch(&s->ctx,
                                            (const char *)parser.payload,
                                            parser.paylen);
            size_t rlen;
            int qret;

            if (rsp == NULL)
              {
                continue;
              }

            rlen = strlen(rsp);
            qret = wt_queue_put(&s->queue, WT_TYPE_RSP, parser.req_id,
                                (uint8_t *)rsp, rlen, WT_RSP_TIMEOUT_MS);
            if (qret < 0)
              {
                syslog(LOG_ERR, "web_tool: response could not be queued "
                                "(%d), dropping connection\n", qret);
                s->stop = true;
              }
          }
        }
    }

out:
  s->stop = true;

  /* Stop the camera before the queue goes away: it is the only producer that
   * allocates 33 KB at a time and it must not be left pushing into a closed
   * queue.
   */

  if (s->ctx.cam_running)
    {
      s->ctx.cam_stop = true;
      pthread_join(s->ctx.cam_thread, NULL);
    }

  s->ctx.shell_kill = true;
  s->ctx.log_on = false;

  wt_queue_close(&s->queue);
  pthread_join(s->sender, NULL);

  pthread_mutex_lock(&g_session_lock);
  if (g_session == s)
    {
      g_session = NULL;
    }

  pthread_mutex_unlock(&g_session_lock);

  pthread_mutex_destroy(&s->ctx.shell_lock);
  wt_queue_destroy(&s->queue);
  wt_io_close(s->io);
  free(rxbuf);
  free(s);

  syslog(LOG_INFO, "web_tool: session ended\n");
}

struct wt_session_arg_s
{
  int       fd;
  in_addr_t peer;
};

static void *wt_session_thread(void *arg)
{
  struct wt_session_arg_s *a = arg;
  struct wt_io_s *io = wt_io_plain(a->fd);
  char origin[48];

  strlcpy(origin, inet_ntoa(*(struct in_addr *)&a->peer), sizeof(origin));
  free(a);

  if (io == NULL)
    {
      return NULL;
    }

  wt_session_run(io, origin);
  return NULL;
}

/****************************************************************************
 * Private Functions: outbound (the production path)
 ****************************************************************************/

/****************************************************************************
 * Name: wt_connect_mode
 *
 * Description:
 *   Dial the development machine's console over TLS and serve one session on
 *   that socket, reconnecting for as long as this task lives.
 *
 *   This is the mode that works in practice.  The board is behind the access
 *   point's NAT: measured 2026-08-18, outbound reaches the development machine
 *   in 91 ms while inbound never arrives, so "the board listens" is not an
 *   option on this network no matter how the whitelist is configured.
 *
 *   Reconnection backs off but never gives up, and says why each time.  The
 *   board has nothing else to do with the time, and a device that stops trying
 *   after a network blip has to be power-cycled by someone standing next to
 *   it -- which is precisely the situation this tool exists to avoid.
 *
 ****************************************************************************/

static int wt_connect_mode(const char *host, int port)
{
  char pin[WT_FP_HEX_LEN];
  char seen[WT_FP_HEX_LEN];
  unsigned int backoff_ms = 1000;
  unsigned int attempt = 0;

  if (bk7258_kvdb_get(WT_FP_KEY, pin, sizeof(pin)) <= 0)
    {
      pin[0] = '\0';
    }

  for (; ; )
    {
      struct wt_io_s *io;
      char origin[48];
      int err = WT_IO_OK;

      attempt++;
      io = wt_io_tls_connect(host, port, pin, seen, sizeof(seen), &err);

      if (io != NULL)
        {
          char token[80];
          char hello[128];
          int n;

          backoff_ms = 1000;
          snprintf(origin, sizeof(origin), "%s:%d", host, port);

          /* First frame: who we are.  Sent before anything else so a console
           * that does not recognise us can drop the connection without ever
           * having offered us a command -- and, more to the point, without the
           * operator's API key ever being sent to a peer that is not this
           * board.
           */

          if (bk7258_kvdb_get(WT_TOKEN_KEY, token, sizeof(token)) <= 0)
            {
              token[0] = '\0';
            }

          n = snprintf(hello, sizeof(hello),
                       "{\"token\":\"%s\",\"proto\":1}", token);

          if (n > 0 && (size_t)n < sizeof(hello) &&
              wt_send_frame(io, WT_TYPE_HELLO, 0,
                            (const uint8_t *)hello, (size_t)n) == 0)
            {
              wt_session_run(io, origin);
            }
          else
            {
              printf("web_tool: could not send hello, retrying\n");
              wt_io_close(io);
            }
        }
      else if (err == WT_IO_ERR_NOPIN)
        {
          /* Deliberately fatal rather than a warning that scrolls away: the
           * whole point of the pin is that there is no unauthenticated mode.
           */

          printf("web_tool: %s is not set, refusing to continue.\n"
                 "web_tool: the console at %s:%d presented\n"
                 "  %s\n"
                 "If that matches what console.py printed, run:\n"
                 "  kvdb set %s %s\n",
                 WT_FP_KEY, host, port, seen, WT_FP_KEY, seen);
          return EXIT_FAILURE;
        }
      else if (err == WT_IO_ERR_PIN)
        {
          printf("web_tool: certificate does not match %s.\n"
                 "  pinned: %s\n"
                 "  seen:   %s\n"
                 "Not connecting.  Either the console was reinstalled (update "
                 "the pin) or something is in the middle.\n",
                 WT_FP_KEY, pin, seen);
          return EXIT_FAILURE;
        }
      else
        {
          printf("web_tool: attempt %u to %s:%d failed (%d), retrying in "
                 "%u ms\n", attempt, host, port, err, backoff_ms);
        }

      usleep(backoff_ms * 1000);
      if (backoff_ms < 16000)
        {
          backoff_ms *= 2;
        }
    }
}

/****************************************************************************
 * Private Functions: start-up
 ****************************************************************************/

static int wt_logring_setup(void)
{
  int ret;

  /* Once per process, ever.  A builtin's statics persist across invocations
   * in this build, so a second `web_tool` reuses the ring and the channel
   * rather than registering a second one -- and the channel is never
   * unregistered, because its callback can run in interrupt context and there
   * is no safe moment to take the buffer away from it.
   */

  if (g_logring_ready)
    {
      return 0;
    }

  ret = wt_logring_init(&g_logring, WT_LOGRING_BYTES);
  if (ret < 0)
    {
      return ret;
    }

  ret = syslog_channel_register(&g_wt_syslog_channel);
  if (ret < 0)
    {
      wt_logring_free(&g_logring);
      return ret;
    }

  g_logring_ready = true;
  return 0;
}

static void wt_usage(void)
{
  printf("Usage:\n"
         "  web_tool                     dial the console over TLS (default)\n"
         "  web_tool connect [host [port]]\n"
         "  web_tool listen [port]       plaintext, for the loopback selftest\n"
         "  web_tool selftest [host [port]]\n"
         "\n"
         "Outbound is the mode that works: this board is behind the access\n"
         "point's NAT, so the development machine cannot reach it, and the\n"
         "board has to dial out.  Settings, all in kvdb:\n"
         "\n"
         "  kvdb set %s 10.192.225.231     console address\n"
         "  kvdb set %s %d              console port (optional)\n"
         "  kvdb set %s <sha256-hex>         console certificate, pinned\n"
         "\n"
         "console.py prints the fingerprint to pin when it starts.  Without\n"
         "%s the board refuses to connect: an unauthenticated TLS link\n"
         "would look protected while authenticating nobody.\n"
         "\n"
         "listen mode is plaintext and exists for `web_tool selftest` over\n"
         "loopback, where both ends are this process and encryption would buy\n"
         "nothing.  It still honours %s.\n",
         WT_HOST_KEY, WT_PORT_KEY, WT_DEFAULT_OUT_PORT, WT_FP_KEY,
         WT_FP_KEY, WT_ALLOW_KEY);
}

/****************************************************************************
 * Name: wt_listen_mode
 *
 * Description:
 *   The original inbound path: accept plaintext connections from addresses in
 *   `web.allow`, one at a time, newest wins.  Unusable across the NAT this
 *   board sits behind, kept because the loopback self-test runs on it.
 *
 ****************************************************************************/

static int wt_listen_mode(int port)
{
  char allow[WT_ALLOW_VALUE_MAX];
  struct sockaddr_in addr;
  int listenfd;
  int one = 1;
  int ret;

  ret = bk7258_kvdb_get(WT_ALLOW_KEY, allow, sizeof(allow));
  if (ret <= 0)
    {
      printf("web_tool: %s not set; run: kvdb set %s <host-ip>\n",
             WT_ALLOW_KEY, WT_ALLOW_KEY);
      return EXIT_FAILURE;
    }

  g_nallow = wt_allow_parse(allow);
  if (g_nallow == 0)
    {
      printf("web_tool: %s has no usable address, not listening\n",
             WT_ALLOW_KEY);
      return EXIT_FAILURE;
    }

  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    {
      printf("web_tool: socket failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      printf("web_tool: bind to port %d failed: %d\n", port, errno);
      close(listenfd);
      return EXIT_FAILURE;
    }

  if (listen(listenfd, 1) < 0)
    {
      printf("web_tool: listen failed: %d\n", errno);
      close(listenfd);
      return EXIT_FAILURE;
    }

  printf("web_tool: listening on 0.0.0.0:%d (plaintext), %d allowed "
         "source(s)\n", port, g_nallow);
  syslog(LOG_INFO, "web_tool: listening on port %d\n", port);

  for (; ; )
    {
      struct sockaddr_in peer;
      socklen_t peerlen = sizeof(peer);
      struct wt_session_arg_s *arg;
      pthread_attr_t attr;
      pthread_t tid;
      int fd = accept(listenfd, (struct sockaddr *)&peer, &peerlen);

      if (fd < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          printf("web_tool: accept failed: %d\n", errno);
          break;
        }

      if (!wt_allow_match(peer.sin_addr.s_addr))
        {
          /* Logged rather than silently dropped: a refused connection is
           * exactly the event someone will be looking for, whether they are
           * debugging their own whitelist or noticing someone else's scan.
           */

          printf("web_tool: refused %s (not in %s)\n",
                 inet_ntoa(peer.sin_addr), WT_ALLOW_KEY);
          syslog(LOG_WARNING, "web_tool: refused %s\n",
                 inet_ntoa(peer.sin_addr));
          close(fd);
          continue;
        }

      /* One connection at a time; a new one displaces the old.  Two would
       * mean two subscriptions, two camera lifetimes and no obvious owner of
       * the shell gate.
       */

      pthread_mutex_lock(&g_session_lock);
      if (g_session != NULL)
        {
          syslog(LOG_INFO, "web_tool: displacing the previous client\n");
          g_session->stop = true;
          wt_io_shutdown(g_session->io);
        }

      pthread_mutex_unlock(&g_session_lock);

      for (ret = 0; ret < 50; ret++)
        {
          bool gone;

          pthread_mutex_lock(&g_session_lock);
          gone = g_session == NULL;
          pthread_mutex_unlock(&g_session_lock);
          if (gone)
            {
              break;
            }

          usleep(20000);
        }

      arg = malloc(sizeof(*arg));
      if (arg == NULL)
        {
          close(fd);
          continue;
        }

      arg->fd = fd;
      arg->peer = peer.sin_addr.s_addr;

      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 8192);
      if (pthread_create(&tid, &attr, wt_session_thread, arg) != 0)
        {
          printf("web_tool: could not start session thread\n");
          close(fd);
          free(arg);
        }
      else
        {
          pthread_detach(tid);
        }

      pthread_attr_destroy(&attr);
    }

  close(listenfd);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  char host[40];
  char portbuf[16];
  int port = WT_DEFAULT_OUT_PORT;
  int ret;

  if (argc >= 2 && (strcmp(argv[1], "-h") == 0 ||
                    strcmp(argv[1], "--help") == 0))
    {
      wt_usage();
      return EXIT_SUCCESS;
    }

  if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
    {
      /* Client mode: drive a web_tool that is already running.  Kept in the
       * same program rather than as a second builtin so it always speaks the
       * exact codec the service was built with.
       */

      const char *shost = argc >= 3 ? argv[2] : "127.0.0.1";
      int sport = argc >= 4 ? atoi(argv[3]) : WT_DEFAULT_PORT;

      return wt_selftest_run(shost, sport);
    }

  /* The store answers for this boot even when flash is unavailable, so a
   * failure here is not fatal -- but every setting below would then be
   * whatever was typed during this boot, which is worth saying out loud.
   */

  if (bk7258_kvdb_init() < 0)
    {
      printf("web_tool: kvdb is not persistent; settings last until reset\n");
    }

  ret = wt_logring_setup();
  if (ret < 0)
    {
      printf("web_tool: log channel unavailable (%d)\n", ret);
      return EXIT_FAILURE;
    }

  if (argc >= 2 && strcmp(argv[1], "listen") == 0)
    {
      int lport = argc >= 3 ? atoi(argv[2]) : WT_DEFAULT_PORT;

      if (lport <= 0 || lport > 65535)
        {
          wt_usage();
          return EXIT_FAILURE;
        }

      return wt_listen_mode(lport);
    }

  /* Outbound, over TLS.  The default, because it is the mode that works from
   * behind the access point's NAT.
   */

  if (argc >= 3 && strcmp(argv[1], "connect") == 0)
    {
      strlcpy(host, argv[2], sizeof(host));
      if (argc >= 4)
        {
          port = atoi(argv[3]);
        }
    }
  else if (bk7258_kvdb_get(WT_HOST_KEY, host, sizeof(host)) > 0)
    {
      if (bk7258_kvdb_get(WT_PORT_KEY, portbuf, sizeof(portbuf)) > 0)
        {
          port = atoi(portbuf);
        }
    }
  else
    {
      printf("web_tool: %s not set; run: kvdb set %s <console-ip>\n"
             "  (or: web_tool connect <console-ip> [port],"
             " or: web_tool listen)\n",
             WT_HOST_KEY, WT_HOST_KEY);
      return EXIT_FAILURE;
    }

  if (port <= 0 || port > 65535)
    {
      printf("web_tool: %s is not a usable port\n", WT_PORT_KEY);
      return EXIT_FAILURE;
    }

  return wt_connect_mode(host, port);
}
