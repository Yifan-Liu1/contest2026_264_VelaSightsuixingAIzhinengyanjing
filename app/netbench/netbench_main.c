/****************************************************************************
 * packages/demos/contest2026_264_netbench/netbench_main.c
 *
 * Network benchmark for the BK7258 AP core, instrumented with the inter-core
 * mailbox counters.
 *
 * The AP has no radio of its own.  Every frame in either direction crosses
 * the mailbox to the CP, which carries one transaction at a time across all
 * of its channels -- and the console is one of those channels.  That makes
 * "the transfer is slow" and "the mailbox is the reason" two different
 * claims, and telling them apart needs the transport's own counters read
 * around the transfer rather than a guess from the byte rate.
 *
 * Hence the shape of this tool.  Each mode moves bytes one way or another,
 * brackets the transfer with bk7258_net_get_counters(), and prints once at
 * the end.  Printing during a run would put console traffic on the very
 * mailbox being measured, so nothing is reported until the sockets are shut.
 *
 * The modes exist to bracket the question from both sides:
 *
 *   loop    stays inside the stack -- no mailbox, no radio.  The ceiling it
 *           reports is sockets, IOB and memcpy, so a Wi-Fi mode that lands
 *           near it is not being held back by the transport at all.
 *   http    bulk download from any plain-HTTP server.
 *   post    bulk upload to any endpoint that accepts and discards a body.
 *           The two directions are not symmetric here: inbound frames arrive
 *           chained, up to BK7258_WIFI_MAX_LIST per mailbox transaction,
 *           while outbound ones are batched only as far as arrival timing
 *           allows, so each direction needs measuring on its own.
 *   hrtt    per-exchange latency, HEAD on one kept-alive connection.  A
 *           mailbox transaction that times out costs MB_TIMEOUT, 200 ms, so
 *           latency outliers are where such a stall shows up; a byte rate
 *           would only average it away.
 *   dns     the same round trip over UDP, which has no window, no
 *           retransmission and no acknowledgements.  A pattern present in
 *           both dns and hrtt sits below TCP; one present only in hrtt does
 *           not.
 *
 * Those four need only a public server, which is the usual case: the board
 * and the workstation are often not on the same network, and a benchmark
 * that depends on a cooperating peer cannot run at all when they are not.
 * The rx, tx, duplex, udprx, udptx and rtt modes do want such a peer
 * (netbench_server.py) and give cleaner numbers when one is reachable,
 * because then nothing between the two ends is shared with anyone else.
 *
 * What the report settles: if a slow run shows mbfail at zero and an inbound
 * batching factor near 1, the mailbox was idle between arrivals rather than
 * saturated, and the limit is upstream of it.  If mbfail climbs, or the
 * batching factor sits near its ceiling, the mailbox is a real constraint.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <nuttx/mm/iob.h>

#include <arch/chip/bk7258_netstats.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NETBENCH_BUFFER      4096
#define NETBENCH_SECONDS     10
#define NETBENCH_MAX_SECONDS 300
#define NETBENCH_LOOP_PORT   15321
#define NETBENCH_UDP_PAYLOAD 1472
#define NETBENCH_HTTP_PORT   80

/* Long enough that the deadline is not checked in a tight spin, short enough
 * that a run stops promptly once its time is up.
 */

#define NETBENCH_POLL_MS     200

/* One exchange of this size per round trip in rtt mode: large enough to be a
 * real frame, small enough to fit one segment in each direction.
 */

#define NETBENCH_RTT_BYTES   64
#define NETBENCH_RTT_COUNT   20
#define NETBENCH_RTT_TIMEOUT 2000

/* Any name resolves the path; the answer is not what is being measured, so a
 * short one that every resolver already has cached keeps the reply small.
 */

#define NETBENCH_DNS_NAME    "example.com"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct netbench_run
{
  const char *mode;
  unsigned long down_bytes;
  unsigned long up_bytes;
  uint32_t elapsed_ms;
  struct bk7258_net_counters before;
  struct bk7258_net_counters after;
};

/* Shared with the thread that drives the opposite direction in loop and
 * duplex mode.  stop is written by the main thread and read by the sender,
 * which is the whole of the synchronisation: a byte-sized flag polled once
 * per buffer needs no more, and the join afterwards is what publishes bytes.
 */

struct netbench_sender
{
  int sock;
  const char *host;
  int port;
  volatile bool stop;
  unsigned long bytes;
  int error;
};

/* Shared by every mode that times individual exchanges rather than a bulk
 * transfer.  The two tail counts are the point of it: a mailbox transaction
 * that waited out MB_TIMEOUT costs 200 ms, and an average over a hundred
 * samples would hide that where a count of them cannot.
 */

struct netbench_latency
{
  unsigned long samples;
  unsigned long total;
  unsigned long over_100;
  unsigned long over_200;
  uint32_t best;
  uint32_t worst;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t netbench_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                    (uint64_t)ts.tv_nsec / 1000000ULL);
}

/* Hundredths, so that the report can show two decimals without depending on
 * floating point formatting.
 */

static unsigned long netbench_kb_x100(unsigned long bytes, uint32_t ms)
{
  if (ms == 0)
    {
      return 0;
    }

  return (unsigned long)(((uint64_t)bytes * 100000ULL) /
                         ((uint64_t)ms * 1024ULL));
}

static unsigned long netbench_ratio_x100(unsigned long numerator,
                                         unsigned long denominator)
{
  if (denominator == 0)
    {
      return 0;
    }

  return (unsigned long)(((uint64_t)numerator * 100ULL) / denominator);
}

static unsigned long netbench_per_second(unsigned long count, uint32_t ms)
{
  if (ms == 0)
    {
      return 0;
    }

  return (unsigned long)(((uint64_t)count * 1000ULL) / ms);
}

/* Lowest read-ahead buffer availability seen while a transfer was running.
 *
 * This is here because it decides the advertised receive window, and thus how
 * much the peer is allowed to have in flight.  tcp_get_recvwindow() offers
 * tailroom + iob_navail(true) * CONFIG_IOB_BUFSIZE while throttled buffers
 * remain, one MSS when they are gone and the connection holds no read-ahead,
 * and only the tailroom of the last buffer when they are gone and it does.
 * That last case collapses the window to a few hundred bytes, which a byte
 * rate alone cannot be told apart from a slow link.
 */

static int g_iob_min_throttled;
static int g_iob_min_total;

static void netbench_iob_reset(void)
{
  g_iob_min_throttled = INT_MAX;
  g_iob_min_total = INT_MAX;
}

static void netbench_iob_sample(void)
{
  int value = iob_navail(true);

  if (value < g_iob_min_throttled)
    {
      g_iob_min_throttled = value;
    }

  value = iob_navail(false);
  if (value < g_iob_min_total)
    {
      g_iob_min_total = value;
    }
}

static void netbench_iob_report(void)
{
  if (g_iob_min_throttled == INT_MAX)
    {
      return;
    }

  printf("  iob      min avail throttled=%d free=%d of %d, throttle %d\n",
         g_iob_min_throttled, g_iob_min_total, CONFIG_IOB_NBUFFERS,
         CONFIG_IOB_THROTTLE);
}

static void netbench_latency_init(struct netbench_latency *latency)
{
  memset(latency, 0, sizeof(*latency));
  latency->best = 0xffffffff;
}

static void netbench_latency_add(struct netbench_latency *latency,
                                 uint32_t elapsed)
{
  latency->samples++;
  latency->total += elapsed;

  if (elapsed < latency->best)
    {
      latency->best = elapsed;
    }

  if (elapsed > latency->worst)
    {
      latency->worst = elapsed;
    }

  if (elapsed >= 100)
    {
      latency->over_100++;
    }

  if (elapsed >= 200)
    {
      latency->over_200++;
    }
}

static void netbench_latency_report(const struct netbench_latency *latency)
{
  if (latency->samples == 0)
    {
      printf("  latency  no samples\n");
      return;
    }

  printf("  latency  n=%lu min=%lu avg=%lu max=%lu ms  "
         ">=100ms:%lu >=200ms:%lu\n",
         latency->samples, (unsigned long)latency->best,
         latency->total / latency->samples, (unsigned long)latency->worst,
         latency->over_100, latency->over_200);
}

static int netbench_seconds(const char *argument)
{
  int seconds;

  if (argument == NULL)
    {
      return NETBENCH_SECONDS;
    }

  seconds = atoi(argument);
  if (seconds <= 0 || seconds > NETBENCH_MAX_SECONDS)
    {
      return NETBENCH_SECONDS;
    }

  return seconds;
}

static int netbench_resolve(const char *host, struct sockaddr_in *addr)
{
  struct hostent *entry;

  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;

  if (inet_pton(AF_INET, host, &addr->sin_addr) == 1)
    {
      return OK;
    }

  entry = gethostbyname(host);
  if (entry == NULL || entry->h_addrtype != AF_INET ||
      entry->h_addr_list[0] == NULL)
    {
      fprintf(stderr, "netbench: cannot resolve %s\n", host);
      return -ENOENT;
    }

  memcpy(&addr->sin_addr, entry->h_addr_list[0], sizeof(addr->sin_addr));
  return OK;
}

/* A receive timeout is what lets every loop below notice its deadline
 * without spinning: recv returns EAGAIN, the deadline is checked, and the
 * wait resumes.
 */

static void netbench_set_timeout(int sock, int ms)
{
  struct timeval tv;

  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int netbench_connect(const char *host, int port)
{
  struct sockaddr_in addr;
  int sock;

  if (netbench_resolve(host, &addr) < 0)
    {
      return -1;
    }

  addr.sin_port = htons((uint16_t)port);

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      fprintf(stderr, "netbench: socket failed (%d)\n", errno);
      return -1;
    }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      fprintf(stderr, "netbench: connect to %s:%d failed (%d)\n",
              host, port, errno);
      close(sock);
      return -1;
    }

  return sock;
}

static int netbench_listen(const char *address, int port)
{
  struct sockaddr_in addr;
  int reuse = 1;
  int sock;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, address, &addr.sin_addr) != 1)
    {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      fprintf(stderr, "netbench: socket failed (%d)\n", errno);
      return -1;
    }

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(sock, 1) < 0)
    {
      fprintf(stderr, "netbench: bind/listen on %d failed (%d)\n",
              port, errno);
      close(sock);
      return -1;
    }

  return sock;
}

static unsigned long netbench_drain(int sock, uint32_t deadline)
{
  unsigned long total = 0;
  char *buffer;

  buffer = malloc(NETBENCH_BUFFER);
  if (buffer == NULL)
    {
      return 0;
    }

  netbench_set_timeout(sock, NETBENCH_POLL_MS);

  while ((int32_t)(netbench_now_ms() - deadline) < 0)
    {
      ssize_t received;

      /* Sampled before the read as well as after it, because the interesting
       * moment is while the connection is waiting rather than once this task
       * has just freed a buffer by draining it.
       */

      netbench_iob_sample();
      received = recv(sock, buffer, NETBENCH_BUFFER, 0);
      netbench_iob_sample();

      if (received > 0)
        {
          total += (unsigned long)received;
          continue;
        }

      if (received == 0)
        {
          break;
        }

      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
          continue;
        }

      fprintf(stderr, "netbench: recv failed (%d)\n", errno);
      break;
    }

  free(buffer);
  return total;
}

static unsigned long netbench_blast(int sock, uint32_t deadline)
{
  unsigned long total = 0;
  char *buffer;

  buffer = malloc(NETBENCH_BUFFER);
  if (buffer == NULL)
    {
      return 0;
    }

  memset(buffer, 0x5a, NETBENCH_BUFFER);

  while ((int32_t)(netbench_now_ms() - deadline) < 0)
    {
      ssize_t sent = send(sock, buffer, NETBENCH_BUFFER, 0);

      if (sent > 0)
        {
          total += (unsigned long)sent;
          continue;
        }

      if (sent < 0 && (errno == EINTR || errno == EAGAIN ||
                       errno == EWOULDBLOCK))
        {
          continue;
        }

      if (sent < 0)
        {
          fprintf(stderr, "netbench: send failed (%d)\n", errno);
        }

      break;
    }

  free(buffer);
  return total;
}

static void *netbench_sender_thread(void *argument)
{
  struct netbench_sender *sender = argument;
  char *buffer;

  buffer = malloc(NETBENCH_BUFFER);
  if (buffer == NULL)
    {
      sender->error = -ENOMEM;
      return NULL;
    }

  memset(buffer, 0xa5, NETBENCH_BUFFER);

  if (sender->sock < 0)
    {
      sender->sock = netbench_connect(sender->host, sender->port);
      if (sender->sock < 0)
        {
          free(buffer);
          sender->error = -ECONNREFUSED;
          return NULL;
        }
    }

  while (!sender->stop)
    {
      ssize_t sent = send(sender->sock, buffer, NETBENCH_BUFFER, 0);

      if (sent > 0)
        {
          sender->bytes += (unsigned long)sent;
          continue;
        }

      if (sent < 0 && (errno == EINTR || errno == EAGAIN ||
                       errno == EWOULDBLOCK))
        {
          continue;
        }

      if (sent < 0)
        {
          sender->error = -errno;
        }

      break;
    }

  free(buffer);
  return NULL;
}

/****************************************************************************
 * Name: netbench_report
 *
 * Description:
 *   The whole readout, printed once now that every socket is closed.  Doing
 *   it here rather than as the run progresses is not tidiness: the console
 *   is a mailbox channel, so a progress line would add traffic to the
 *   transport whose behaviour is the measurement.
 *
 ****************************************************************************/

static void netbench_report(const struct netbench_run *run)
{
  const struct bk7258_net_counters *a = &run->after;
  const struct bk7258_net_counters *b = &run->before;
  unsigned long down_rate;
  unsigned long up_rate;
  unsigned long mbfail;
  unsigned long tx_batches;
  unsigned long tx_frames;
  unsigned long rx_lists;
  unsigned long rx_frames;
  unsigned long value;

  down_rate = netbench_kb_x100(run->down_bytes, run->elapsed_ms);
  up_rate = netbench_kb_x100(run->up_bytes, run->elapsed_ms);

  tx_batches = a->wifi_tx_batches - b->wifi_tx_batches;
  tx_frames = a->wifi_tx_frames - b->wifi_tx_frames;
  rx_lists = a->wifi_rx_lists - b->wifi_rx_lists;
  rx_frames = a->wifi_rx_frames - b->wifi_rx_frames;

  /* One number for "did the mailbox actually fail, as opposed to merely
   * being busy".  Every term is a refusal or a loss, not a wait.
   */

  mbfail = (a->mb_timeout - b->mb_timeout) +
           (a->mb_fifo_full - b->mb_fifo_full) +
           (a->mb_ack_overflow - b->mb_ack_overflow) +
           (a->mb0_write_full - b->mb0_write_full) +
           (a->mb0_desc_full - b->mb0_desc_full);

  printf("netbench %s: down %lu B, up %lu B, %lu ms\n",
         run->mode, run->down_bytes, run->up_bytes,
         (unsigned long)run->elapsed_ms);

  printf("  rate     down %lu.%02lu KB/s   up %lu.%02lu KB/s\n",
         down_rate / 100, down_rate % 100, up_rate / 100, up_rate % 100);

  printf("  mbfail   %lu  (timeout=%lu fifo=%lu ackovf=%lu wrfull=%lu "
         "descfull=%lu)\n",
         mbfail,
         (unsigned long)(a->mb_timeout - b->mb_timeout),
         (unsigned long)(a->mb_fifo_full - b->mb_fifo_full),
         (unsigned long)(a->mb_ack_overflow - b->mb_ack_overflow),
         (unsigned long)(a->mb0_write_full - b->mb0_write_full),
         (unsigned long)(a->mb0_desc_full - b->mb0_desc_full));

  printf("  mb       tx=%lu rx=%lu defer=%lu badack=%lu badhdr=%lu "
         "recov=%lu/%lu\n",
         (unsigned long)(a->mb_tx - b->mb_tx),
         (unsigned long)(a->mb_rx - b->mb_rx),
         (unsigned long)(a->mb_deferred - b->mb_deferred),
         (unsigned long)(a->mb_bad_ack - b->mb_bad_ack),
         (unsigned long)(a->mb_bad_header - b->mb_bad_header),
         (unsigned long)(a->mb_recovery_cycle - b->mb_recovery_cycle),
         (unsigned long)(a->mb_recovery_replay - b->mb_recovery_replay));

  printf("  mb0      rx=%lu wrerr=%lu rderr=%lu descdefer=%lu\n",
         (unsigned long)(a->mb0_rx - b->mb0_rx),
         (unsigned long)(a->mb0_write_error - b->mb0_write_error),
         (unsigned long)(a->mb0_read_error - b->mb0_read_error),
         (unsigned long)(a->mb0_desc_deferred - b->mb0_desc_deferred));

  value = netbench_ratio_x100(tx_frames, tx_batches);
  printf("  wifi-tx  batches=%lu frames=%lu f/b=%lu.%02lu max=%u\n",
         tx_batches, tx_frames, value / 100, value % 100,
         a->wifi_tx_batch_max);

  value = netbench_ratio_x100(rx_frames, rx_lists);
  printf("  wifi-rx  lists=%lu frames=%lu f/l=%lu.%02lu max=%u of %u "
         "txdone=%lu\n",
         rx_lists, rx_frames, value / 100, value % 100,
         a->wifi_rx_list_max, a->wifi_rx_list_ceiling,
         (unsigned long)(a->wifi_tx_completions - b->wifi_tx_completions));

  printf("  drops    enetdown=%lu slots=%lu rejected=%lu queuefull=%lu "
         "nobuf=%lu\n",
         (unsigned long)(a->wifi_tx_enetdown - b->wifi_tx_enetdown),
         (unsigned long)(a->wifi_tx_slots_full - b->wifi_tx_slots_full),
         (unsigned long)(a->wifi_tx_rejected - b->wifi_tx_rejected),
         (unsigned long)(a->wifi_rx_queue_full - b->wifi_rx_queue_full),
         (unsigned long)(a->wifi_rx_alloc_fail - b->wifi_rx_alloc_fail));

  printf("  per-sec  mbtx=%lu mbrx=%lu rxlists=%lu rxframes=%lu\n",
         netbench_per_second(a->mb_tx - b->mb_tx, run->elapsed_ms),
         netbench_per_second(a->mb_rx - b->mb_rx, run->elapsed_ms),
         netbench_per_second(rx_lists, run->elapsed_ms),
         netbench_per_second(rx_frames, run->elapsed_ms));

  if (rx_frames != 0)
    {
      printf("  payload  %lu B per inbound frame\n",
             run->down_bytes / rx_frames);
    }

  printf("  link     state=%u busy=%u ackslots=%u ready=%lu down=%lu\n",
         a->mb_link_state, a->mb_busy ? 1u : 0u, a->mb_ack_slots_used,
         (unsigned long)(a->mb_link_ready - b->mb_link_ready),
         (unsigned long)(a->mb_link_down - b->mb_link_down));

  netbench_iob_report();
}

static void netbench_snapshot(void)
{
  struct bk7258_net_counters c;

  bk7258_net_get_counters(&c);

  printf("netbench snap:\n");
  printf("  mb       tx=%lu rx=%lu timeout=%lu fifo=%lu ackovf=%lu "
         "defer=%lu\n",
         (unsigned long)c.mb_tx, (unsigned long)c.mb_rx,
         (unsigned long)c.mb_timeout, (unsigned long)c.mb_fifo_full,
         (unsigned long)c.mb_ack_overflow, (unsigned long)c.mb_deferred);
  printf("  mb       badack=%lu badhdr=%lu recov=%lu/%lu ready=%lu "
         "down=%lu\n",
         (unsigned long)c.mb_bad_ack, (unsigned long)c.mb_bad_header,
         (unsigned long)c.mb_recovery_cycle,
         (unsigned long)c.mb_recovery_replay,
         (unsigned long)c.mb_link_ready, (unsigned long)c.mb_link_down);
  printf("  mb0      rx=%lu wrfull=%lu wrerr=%lu rderr=%lu descfull=%lu "
         "descdefer=%lu\n",
         (unsigned long)c.mb0_rx, (unsigned long)c.mb0_write_full,
         (unsigned long)c.mb0_write_error, (unsigned long)c.mb0_read_error,
         (unsigned long)c.mb0_desc_full,
         (unsigned long)c.mb0_desc_deferred);
  printf("  mb0      badsrc=%lu badlen=%lu badaddr=%lu\n",
         (unsigned long)c.mb0_bad_source, (unsigned long)c.mb0_bad_length,
         (unsigned long)c.mb0_bad_address);
  printf("  wifi-tx  batches=%lu frames=%lu max=%u\n",
         (unsigned long)c.wifi_tx_batches, (unsigned long)c.wifi_tx_frames,
         c.wifi_tx_batch_max);
  printf("  wifi-rx  lists=%lu frames=%lu max=%u of %u txdone=%lu\n",
         (unsigned long)c.wifi_rx_lists, (unsigned long)c.wifi_rx_frames,
         c.wifi_rx_list_max, c.wifi_rx_list_ceiling,
         (unsigned long)c.wifi_tx_completions);
  printf("  drops    enetdown=%lu slots=%lu rejected=%lu queuefull=%lu "
         "nobuf=%lu\n",
         (unsigned long)c.wifi_tx_enetdown,
         (unsigned long)c.wifi_tx_slots_full,
         (unsigned long)c.wifi_tx_rejected,
         (unsigned long)c.wifi_rx_queue_full,
         (unsigned long)c.wifi_rx_alloc_fail);
  printf("  link     state=%u busy=%u ackslots=%u\n",
         c.mb_link_state, c.mb_busy ? 1u : 0u, c.mb_ack_slots_used);
}

/****************************************************************************
 * Modes
 ****************************************************************************/

static int netbench_mode_stream(struct netbench_run *run, const char *host,
                                int port, int seconds, bool download,
                                bool upload)
{
  struct netbench_sender sender;
  pthread_t thread;
  bool threaded = false;
  uint32_t start;
  uint32_t deadline;
  int sock;

  sock = netbench_connect(host, port);
  if (sock < 0)
    {
      return -1;
    }

  memset(&sender, 0, sizeof(sender));
  sender.sock = sock;

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();
  deadline = start + (uint32_t)seconds * 1000u;

  if (download && upload)
    {
      if (pthread_create(&thread, NULL, netbench_sender_thread,
                         &sender) == 0)
        {
          threaded = true;
        }
      else
        {
          fprintf(stderr, "netbench: cannot start sender thread\n");
        }
    }

  if (download)
    {
      run->down_bytes = netbench_drain(sock, deadline);
    }
  else
    {
      run->up_bytes = netbench_blast(sock, deadline);
    }

  if (threaded)
    {
      sender.stop = true;
      pthread_join(thread, NULL);
      run->up_bytes = sender.bytes;
    }

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);

  close(sock);
  return OK;
}

/* Loopback: the same socket code over lo, which the Wi-Fi driver and the
 * mailbox never see.  Whatever this reports is the ceiling the software
 * imposes on its own, and every Wi-Fi number belongs underneath it.
 */

static int netbench_mode_loop(struct netbench_run *run, int seconds)
{
  struct netbench_sender sender;
  pthread_t thread;
  uint32_t start;
  uint32_t deadline;
  int listener;
  int conn;

  listener = netbench_listen("127.0.0.1", NETBENCH_LOOP_PORT);
  if (listener < 0)
    {
      return -1;
    }

  memset(&sender, 0, sizeof(sender));
  sender.sock = -1;
  sender.host = "127.0.0.1";
  sender.port = NETBENCH_LOOP_PORT;

  if (pthread_create(&thread, NULL, netbench_sender_thread, &sender) != 0)
    {
      fprintf(stderr, "netbench: cannot start sender thread\n");
      close(listener);
      return -1;
    }

  conn = accept(listener, NULL, NULL);
  if (conn < 0)
    {
      fprintf(stderr, "netbench: accept failed (%d)\n", errno);
      sender.stop = true;
      pthread_join(thread, NULL);
      close(listener);
      return -1;
    }

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();
  deadline = start + (uint32_t)seconds * 1000u;

  run->down_bytes = netbench_drain(conn, deadline);

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);

  sender.stop = true;
  pthread_join(thread, NULL);
  run->up_bytes = sender.bytes;

  if (sender.sock >= 0)
    {
      close(sender.sock);
    }

  close(conn);
  close(listener);
  return OK;
}

/* A download that needs nothing installed on the other end, which matters
 * when the board and the workstation are not on the same network and no
 * cooperating server is reachable.  Plain HTTP on purpose: TLS would put
 * decryption on the critical path and confuse a transport measurement.
 */

static int netbench_mode_http(struct netbench_run *run, const char *host,
                              const char *path, int seconds)
{
  char request[256];
  uint32_t start;
  uint32_t deadline;
  int length;
  int sock;

  sock = netbench_connect(host, NETBENCH_HTTP_PORT);
  if (sock < 0)
    {
      return -1;
    }

  length = snprintf(request, sizeof(request),
                    "GET %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: netbench\r\n"
                    "Accept: */*\r\n"
                    "Connection: close\r\n"
                    "\r\n", path, host);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();
  deadline = start + (uint32_t)seconds * 1000u;

  if (send(sock, request, (size_t)length, 0) != length)
    {
      fprintf(stderr, "netbench: cannot send request (%d)\n", errno);
      close(sock);
      return -1;
    }

  run->up_bytes = (unsigned long)length;

  /* Headers are counted with the body.  They are a couple of hundred bytes
   * against megabytes, and excluding them would mean parsing the stream on
   * the critical path.
   */

  run->down_bytes = netbench_drain(sock, deadline);

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);

  close(sock);
  return OK;
}

static int netbench_mode_udprx(struct netbench_run *run, int port,
                               int seconds)
{
  struct sockaddr_in addr;
  unsigned long total = 0;
  uint32_t start;
  uint32_t deadline;
  char *buffer;
  int sock;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      fprintf(stderr, "netbench: socket failed (%d)\n", errno);
      return -1;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      fprintf(stderr, "netbench: bind %d failed (%d)\n", port, errno);
      close(sock);
      return -1;
    }

  buffer = malloc(NETBENCH_BUFFER);
  if (buffer == NULL)
    {
      close(sock);
      return -1;
    }

  netbench_set_timeout(sock, NETBENCH_POLL_MS);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();
  deadline = start + (uint32_t)seconds * 1000u;

  while ((int32_t)(netbench_now_ms() - deadline) < 0)
    {
      ssize_t received = recv(sock, buffer, NETBENCH_BUFFER, 0);

      if (received > 0)
        {
          total += (unsigned long)received;
          continue;
        }

      if (received < 0 && (errno == EINTR || errno == EAGAIN ||
                           errno == EWOULDBLOCK))
        {
          continue;
        }

      break;
    }

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);
  run->down_bytes = total;

  free(buffer);
  close(sock);
  return OK;
}

static int netbench_mode_udptx(struct netbench_run *run, const char *host,
                               int port, int seconds)
{
  struct sockaddr_in addr;
  unsigned long total = 0;
  uint32_t start;
  uint32_t deadline;
  char *buffer;
  int sock;

  if (netbench_resolve(host, &addr) < 0)
    {
      return -1;
    }

  addr.sin_port = htons((uint16_t)port);

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      fprintf(stderr, "netbench: socket failed (%d)\n", errno);
      return -1;
    }

  buffer = malloc(NETBENCH_UDP_PAYLOAD);
  if (buffer == NULL)
    {
      close(sock);
      return -1;
    }

  memset(buffer, 0x3c, NETBENCH_UDP_PAYLOAD);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();
  deadline = start + (uint32_t)seconds * 1000u;

  while ((int32_t)(netbench_now_ms() - deadline) < 0)
    {
      ssize_t sent = sendto(sock, buffer, NETBENCH_UDP_PAYLOAD, 0,
                            (struct sockaddr *)&addr, sizeof(addr));

      if (sent > 0)
        {
          total += (unsigned long)sent;
          continue;
        }

      if (sent < 0 && (errno == EINTR || errno == EAGAIN ||
                       errno == EWOULDBLOCK))
        {
          continue;
        }

      break;
    }

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);
  run->up_bytes = total;

  free(buffer);
  close(sock);
  return OK;
}

/* Round-trip latency against an echo, one exchange at a time with Nagle off.
 * Needs a cooperating peer; hrtt below measures the same thing against any
 * public web server and is the one to reach for by default.
 */

static int netbench_mode_rtt(struct netbench_run *run, const char *host,
                             int port, int count)
{
  struct netbench_latency latency;
  char buffer[NETBENCH_RTT_BYTES];
  uint32_t start;
  int nodelay = 1;
  int sock;
  int i;

  sock = netbench_connect(host, port);
  if (sock < 0)
    {
      return -1;
    }

  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  netbench_set_timeout(sock, NETBENCH_RTT_TIMEOUT);
  memset(buffer, 0x77, sizeof(buffer));
  netbench_latency_init(&latency);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();

  for (i = 0; i < count; i++)
    {
      uint32_t issued = netbench_now_ms();
      size_t got = 0;

      if (send(sock, buffer, sizeof(buffer), 0) != (ssize_t)sizeof(buffer))
        {
          fprintf(stderr, "netbench: rtt send failed (%d)\n", errno);
          break;
        }

      run->up_bytes += sizeof(buffer);

      while (got < sizeof(buffer))
        {
          ssize_t received = recv(sock, buffer + got, sizeof(buffer) - got,
                                  0);

          if (received > 0)
            {
              got += (size_t)received;
              continue;
            }

          if (received < 0 && errno == EINTR)
            {
              continue;
            }

          break;
        }

      if (got < sizeof(buffer))
        {
          fprintf(stderr, "netbench: rtt exchange %d incomplete\n", i);
          break;
        }

      run->down_bytes += got;
      netbench_latency_add(&latency, netbench_now_ms() - issued);
    }

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);
  close(sock);

  if (latency.samples == 0)
    {
      fprintf(stderr, "netbench: no rtt samples\n");
      return -1;
    }

  netbench_report(run);
  netbench_latency_report(&latency);
  return 1;
}

/* Read to the end of one HTTP header block.
 *
 * Reading in blocks rather than a byte at a time is safe here even though the
 * connection is reused: one request is outstanding at a time, so the server
 * has sent nothing beyond this response for an over-read to steal.
 */

static int netbench_read_headers(int sock, char *buffer, size_t size)
{
  unsigned int matched = 0;
  unsigned long total = 0;

  while (matched < 4)
    {
      ssize_t received = recv(sock, buffer, size, 0);
      ssize_t i;

      if (received <= 0)
        {
          if (received < 0 && errno == EINTR)
            {
              continue;
            }

          return -1;
        }

      total += (unsigned long)received;

      for (i = 0; i < received && matched < 4; i++)
        {
          char c = buffer[i];

          if ((matched == 0 || matched == 2) && c == '\r')
            {
              matched++;
            }
          else if ((matched == 1 || matched == 3) && c == '\n')
            {
              matched++;
            }
          else
            {
              matched = (c == '\r') ? 1 : 0;
            }
        }
    }

  return (int)total;
}

/* Upload to any public endpoint that accepts a POST body and discards it.
 *
 * Chunked rather than a declared length because the run is bounded by time,
 * not by a byte count: the terminating chunk closes the body properly at the
 * deadline, so the server answers instead of seeing a truncated request.
 * up_bytes counts payload only; the chunk framing adds about a further two
 * parts in a thousand.
 */

static int netbench_mode_post(struct netbench_run *run, const char *host,
                              const char *path, int seconds)
{
  char header[256];
  char frame[16];
  uint32_t start;
  uint32_t deadline;
  char *buffer;
  int length;
  int sock;

  sock = netbench_connect(host, NETBENCH_HTTP_PORT);
  if (sock < 0)
    {
      return -1;
    }

  buffer = malloc(NETBENCH_BUFFER);
  if (buffer == NULL)
    {
      close(sock);
      return -1;
    }

  memset(buffer, 0x5a, NETBENCH_BUFFER);

  length = snprintf(header, sizeof(header),
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: netbench\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Connection: close\r\n"
                    "\r\n", path, host);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();
  deadline = start + (uint32_t)seconds * 1000u;

  if (send(sock, header, (size_t)length, 0) != length)
    {
      fprintf(stderr, "netbench: cannot send request (%d)\n", errno);
      free(buffer);
      close(sock);
      return -1;
    }

  length = snprintf(frame, sizeof(frame), "%x\r\n", NETBENCH_BUFFER);

  while ((int32_t)(netbench_now_ms() - deadline) < 0)
    {
      if (send(sock, frame, (size_t)length, 0) != length ||
          send(sock, buffer, NETBENCH_BUFFER, 0) != NETBENCH_BUFFER ||
          send(sock, "\r\n", 2, 0) != 2)
        {
          fprintf(stderr, "netbench: upload stopped (%d)\n", errno);
          break;
        }

      run->up_bytes += NETBENCH_BUFFER;
    }

  send(sock, "0\r\n\r\n", 5, 0);

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);

  /* The response is read only so that the endpoint is seen to have accepted
   * the body; its bytes are not part of the upload measurement.
   */

  netbench_set_timeout(sock, NETBENCH_RTT_TIMEOUT);
  if (netbench_read_headers(sock, buffer, NETBENCH_BUFFER) < 0)
    {
      fprintf(stderr, "netbench: no response to upload\n");
    }

  free(buffer);
  close(sock);
  return OK;
}

/* Request/response latency against any public web server, using HEAD on one
 * kept-alive connection.  Same measurement as rtt without needing anything
 * installed at the far end: HEAD has no body, so each exchange is one small
 * request and one small response.
 */

static int netbench_mode_hrtt(struct netbench_run *run, const char *host,
                              const char *path, int count)
{
  struct netbench_latency latency;
  char request[256];
  uint32_t start;
  char *buffer;
  int nodelay = 1;
  int length;
  int sock;
  int i;

  sock = netbench_connect(host, NETBENCH_HTTP_PORT);
  if (sock < 0)
    {
      return -1;
    }

  buffer = malloc(NETBENCH_BUFFER);
  if (buffer == NULL)
    {
      close(sock);
      return -1;
    }

  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  netbench_set_timeout(sock, NETBENCH_RTT_TIMEOUT);
  netbench_latency_init(&latency);

  length = snprintf(request, sizeof(request),
                    "HEAD %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: netbench\r\n"
                    "Accept: */*\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n", path, host);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();

  for (i = 0; i < count; i++)
    {
      uint32_t issued = netbench_now_ms();
      int received;

      if (send(sock, request, (size_t)length, 0) != length)
        {
          fprintf(stderr, "netbench: hrtt send failed (%d)\n", errno);
          break;
        }

      run->up_bytes += (unsigned long)length;

      received = netbench_read_headers(sock, buffer, NETBENCH_BUFFER);
      if (received < 0)
        {
          fprintf(stderr, "netbench: hrtt exchange %d incomplete\n", i);
          break;
        }

      run->down_bytes += (unsigned long)received;
      netbench_latency_add(&latency, netbench_now_ms() - issued);
    }

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);

  free(buffer);
  close(sock);

  if (latency.samples == 0)
    {
      return -1;
    }

  netbench_report(run);
  netbench_latency_report(&latency);
  return 1;
}

static int netbench_dns_query(uint8_t *packet, size_t size, uint16_t id,
                              const char *name)
{
  size_t offset = 12;

  if (size < 12 + strlen(name) + 6)
    {
      return -1;
    }

  packet[0] = (uint8_t)(id >> 8);
  packet[1] = (uint8_t)(id & 0xff);
  packet[2] = 0x01;                     /* recursion desired            */
  packet[3] = 0x00;
  packet[4] = 0x00;
  packet[5] = 0x01;                     /* one question                 */
  memset(&packet[6], 0, 6);

  while (*name != '\0')
    {
      const char *dot = strchr(name, '.');
      size_t label = dot != NULL ? (size_t)(dot - name) : strlen(name);

      if (label == 0 || label > 63)
        {
          return -1;
        }

      packet[offset++] = (uint8_t)label;
      memcpy(&packet[offset], name, label);
      offset += label;

      if (dot == NULL)
        {
          break;
        }

      name = dot + 1;
    }

  packet[offset++] = 0x00;              /* end of name                  */
  packet[offset++] = 0x00;
  packet[offset++] = 0x01;              /* A                            */
  packet[offset++] = 0x00;
  packet[offset++] = 0x01;              /* IN                           */
  return (int)offset;
}

/* Round trips over UDP against a public resolver.
 *
 * The point is not the answer but the path: this exercises delivery without
 * TCP's window, retransmission or acknowledgement machinery, so a latency
 * pattern that appears in both this and hrtt sits below TCP, while one that
 * appears only in hrtt does not.
 */

static int netbench_mode_dns(struct netbench_run *run, const char *server,
                             const char *name, int count)
{
  struct netbench_latency latency;
  struct sockaddr_in addr;
  uint8_t query[300];
  uint8_t reply[512];
  unsigned long lost = 0;
  uint32_t start;
  int sock;
  int i;

  if (netbench_resolve(server, &addr) < 0)
    {
      return -1;
    }

  addr.sin_port = htons(53);

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      fprintf(stderr, "netbench: socket failed (%d)\n", errno);
      return -1;
    }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      fprintf(stderr, "netbench: cannot address %s (%d)\n", server, errno);
      close(sock);
      return -1;
    }

  netbench_set_timeout(sock, NETBENCH_RTT_TIMEOUT);
  netbench_latency_init(&latency);

  bk7258_net_get_counters(&run->before);
  start = netbench_now_ms();

  for (i = 0; i < count; i++)
    {
      uint16_t id = (uint16_t)(0x4e00 + i);
      uint32_t issued;
      ssize_t received;
      int length;

      length = netbench_dns_query(query, sizeof(query), id, name);
      if (length < 0)
        {
          fprintf(stderr, "netbench: cannot build a query for %s\n", name);
          break;
        }

      issued = netbench_now_ms();
      if (send(sock, query, (size_t)length, 0) != length)
        {
          fprintf(stderr, "netbench: dns send failed (%d)\n", errno);
          break;
        }

      run->up_bytes += (unsigned long)length;

      received = recv(sock, reply, sizeof(reply), 0);
      if (received < 12 || reply[0] != query[0] || reply[1] != query[1])
        {
          /* A dropped or mismatched reply is a datagram that did not make it
           * back, which is a result rather than an error: UDP has nothing to
           * retransmit it.
           */

          lost++;
          continue;
        }

      run->down_bytes += (unsigned long)received;
      netbench_latency_add(&latency, netbench_now_ms() - issued);
    }

  run->elapsed_ms = netbench_now_ms() - start;
  bk7258_net_get_counters(&run->after);
  close(sock);

  if (latency.samples == 0 && lost == 0)
    {
      return -1;
    }

  netbench_report(run);
  netbench_latency_report(&latency);
  printf("  dns      server=%s name=%s sent=%d lost=%lu\n",
         server, name, i, lost);
  return 1;
}

static void netbench_usage(void)
{
  printf(
    "usage: netbench <mode> [args]\n"
    "\n"
    "needs nothing but the board:\n"
    "  snap                          counters only, no traffic\n"
    "  loop   [sec]                  loopback TCP: no mailbox, no radio\n"
    "\n"
    "needs only a public server:\n"
    "  http   <host> <path> [sec]    plain HTTP GET download, port 80\n"
    "  post   <host> <path> [sec]    chunked POST upload, port 80\n"
    "  hrtt   <host> <path> [count]  HEAD round trips on one connection\n"
    "  dns    <resolver> [count]     UDP round trips, no TCP window\n"
    "\n"
    "needs a cooperating peer (netbench_server.py):\n"
    "  rx     <host> <port> [sec]    bulk TCP download\n"
    "  tx     <host> <port> [sec]    bulk TCP upload\n"
    "  duplex <host> <port> [sec]    both directions at once\n"
    "  udprx  <port> [sec]           bulk UDP download\n"
    "  udptx  <host> <port> [sec]    bulk UDP upload\n"
    "  rtt    <host> <port> [count]  round trips against an echo\n"
    "\n"
    "examples:\n"
    "  netbench snap\n"
    "  netbench loop 10\n"
    "  netbench http mirrors.163.com "
    "/ubuntu-releases/22.04/ubuntu-22.04.5-live-server-amd64.iso 15\n"
    "  netbench post speedtest.tele2.net /upload.php 10\n"
    "  netbench hrtt mirrors.aliyun.com /ubuntu/ls-lR.gz 30\n"
    "  netbench dns 223.5.5.5 30\n"
    "\n"
    "Default duration %d s, default count %d, dns name %s.\n"
    "Nothing prints until a run ends: the console shares the mailbox being\n"
    "measured, so progress output would add the traffic under test.\n",
    NETBENCH_SECONDS, NETBENCH_RTT_COUNT, NETBENCH_DNS_NAME);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct netbench_run run;
  const char *mode;
  int status;

  if (argc < 2)
    {
      netbench_usage();
      return EXIT_FAILURE;
    }

  mode = argv[1];
  memset(&run, 0, sizeof(run));
  run.mode = mode;
  netbench_iob_reset();

  if (strcmp(mode, "snap") == 0)
    {
      netbench_snapshot();
      return EXIT_SUCCESS;
    }

  if (strcmp(mode, "loop") == 0)
    {
      status = netbench_mode_loop(&run, netbench_seconds(argv[2]));
    }
  else if (strcmp(mode, "rx") == 0 || strcmp(mode, "tx") == 0 ||
           strcmp(mode, "duplex") == 0)
    {
      bool download = strcmp(mode, "tx") != 0;
      bool upload = strcmp(mode, "rx") != 0;

      if (argc < 4)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      status = netbench_mode_stream(&run, argv[2], atoi(argv[3]),
                                    netbench_seconds(argv[4]),
                                    download, upload);
    }
  else if (strcmp(mode, "http") == 0)
    {
      if (argc < 4)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      status = netbench_mode_http(&run, argv[2], argv[3],
                                  netbench_seconds(argv[4]));
    }
  else if (strcmp(mode, "post") == 0)
    {
      if (argc < 4)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      status = netbench_mode_post(&run, argv[2], argv[3],
                                  netbench_seconds(argv[4]));
    }
  else if (strcmp(mode, "hrtt") == 0)
    {
      int count = argc > 4 ? atoi(argv[4]) : NETBENCH_RTT_COUNT;

      if (argc < 4)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      if (count <= 0 || count > 1000)
        {
          count = NETBENCH_RTT_COUNT;
        }

      status = netbench_mode_hrtt(&run, argv[2], argv[3], count);
    }
  else if (strcmp(mode, "dns") == 0)
    {
      int count = argc > 3 ? atoi(argv[3]) : NETBENCH_RTT_COUNT;
      const char *name = argc > 4 ? argv[4] : NETBENCH_DNS_NAME;

      if (argc < 3)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      if (count <= 0 || count > 1000)
        {
          count = NETBENCH_RTT_COUNT;
        }

      status = netbench_mode_dns(&run, argv[2], name, count);
    }
  else if (strcmp(mode, "udprx") == 0)
    {
      if (argc < 3)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      status = netbench_mode_udprx(&run, atoi(argv[2]),
                                   netbench_seconds(argv[3]));
    }
  else if (strcmp(mode, "udptx") == 0)
    {
      if (argc < 4)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      status = netbench_mode_udptx(&run, argv[2], atoi(argv[3]),
                                   netbench_seconds(argv[4]));
    }
  else if (strcmp(mode, "rtt") == 0)
    {
      int count = argc > 4 ? atoi(argv[4]) : NETBENCH_RTT_COUNT;

      if (argc < 4)
        {
          netbench_usage();
          return EXIT_FAILURE;
        }

      if (count <= 0 || count > 1000)
        {
          count = NETBENCH_RTT_COUNT;
        }

      status = netbench_mode_rtt(&run, argv[2], atoi(argv[3]), count);
    }
  else
    {
      netbench_usage();
      return EXIT_FAILURE;
    }

  if (status < 0)
    {
      return EXIT_FAILURE;
    }

  /* rtt prints its own trailer after the shared block, so it reports for
   * itself and says so with a positive status.
   */

  if (status == 0)
    {
      netbench_report(&run);
    }

  return EXIT_SUCCESS;
}
