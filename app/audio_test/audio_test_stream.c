/****************************************************************************
 * app/audio_test/audio_test_stream.c
 *
 * Continuous capture split into fixed-length chunks and uploaded while the
 * next chunk is still being recorded.
 *
 * The shape of this file follows one requirement: network congestion must not
 * stall audio capture.  That rules out encoding or sending anything on the
 * thread that services the ADC, because both can take arbitrarily long -- an
 * Opus encode is tens of milliseconds of solid CPU and a TCP write to a
 * congested link is unbounded.  So the two run on separate threads with a
 * ring of chunk slots between them, and the only thing the capture side ever
 * does is copy samples into a slot and signal.
 *
 * When the uploader cannot keep up, something has to give.  Blocking capture
 * would corrupt the recording (the driver's buffers would go unclaimed and
 * the ADC would overrun), and growing the ring without bound would exhaust
 * PSRAM and then fail anyway, later and less predictably.  So the oldest
 * queued chunk is dropped instead: the upload protocol allows a gap in the
 * sequence numbers, which makes a dropped chunk a documented outcome rather
 * than a corrupted stream.  Drops are counted and reported.
 *
 * Each chunk is a complete Ogg Opus stream rather than a slice of one
 * continuous stream.  A slice would only be decodable after everything
 * before it had arrived, which contradicts both allowances the plan makes --
 * that a sequence number may be missing, and that a chunk may be retried on
 * its own.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arch/chip/bk7258_psram.h>

#include "audio_test_ogg.h"
#include "audio_test_stream.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Chunk slots in the ring.
 *
 * Four is the smallest number that is not fragile.  One is impossible (the
 * uploader holds a slot while capture needs another), two leaves no margin at
 * all, and three tolerates only a single late chunk.  Four means the uploader
 * can be a whole chunk behind and still catch up without a drop, at a cost of
 * 64 KiB of PSRAM per slot at 16 kHz and two seconds.
 */

#define STREAM_SLOTS            4

/* Ceiling on the encoded size of one chunk.  Opus at speech bitrates puts a
 * two second chunk in a few kilobytes; 64 KiB is far past any VBR excursion
 * and costs nothing in PSRAM.
 */

#define STREAM_ENCODE_CAP       65536

/* Uploader stack.
 *
 * The encode runs on this thread, not the caller's, and libopus is built
 * with VAR_ARRAYS: its working arrays live on the stack of whoever calls
 * opus_encode() rather than in the encoder state.  That is why the task
 * itself asks for 32 KiB -- 8 KiB was measured to be too little -- so this
 * thread gets the same rather than a guess at how much of it the encoder
 * was actually using.
 */

#define STREAM_STACKSIZE        32768

/* How far below the capture thread the uploader runs.
 *
 * NuttX schedules FIFO within a priority, so an equal-priority uploader
 * would be able to hold the CPU across a whole encode and make the capture
 * thread late for the ADC.  A lower priority makes the ordering explicit
 * instead of relying on where the yields happen to fall.
 */

#define STREAM_PRIO_BELOW       10

/* Bound on a connect() to an address that is not answering.
 *
 * The default would be tens of seconds, and although a stalled uploader
 * cannot stall capture, it does mean every chunk in the meantime is dropped.
 * Failing fast turns "server is down" into a couple of lost chunks and a
 * clear error rather than a minute of silence.
 */

#define STREAM_CONNECT_MS       2000

/* Send and receive timeouts once connected. */

#define STREAM_SEND_MS          3000
#define STREAM_RECV_MS          1000

/* Attempts per chunk.  The plan allows one retry, so two attempts. */

#define STREAM_ATTEMPTS         2

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum stream_slot_state_e
{
  SLOT_FREE = 0,    /* nobody owns it */
  SLOT_FILLING,     /* capture is writing into it */
  SLOT_READY,       /* queued, waiting for the uploader */
  SLOT_BUSY         /* the uploader is encoding or sending it */
};

struct stream_slot_s
{
  uint8_t *pcm;
  size_t used;
  uint32_t sequence;
  enum stream_slot_state_e state;
};

struct audio_test_stream_ctx_s
{
  struct stream_slot_s slot[STREAM_SLOTS];
  struct stream_slot_s *cur;           /* capture's slot; capture-only */

  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t uploader;
  bool running;
  bool quit;

  char host[16];
  int port;
  uint32_t session;

  unsigned int rate;
  unsigned int chunk_ms;
  unsigned int bitrate;
  size_t chunk_bytes;

  void *encoder;
  uint8_t *encbuf;

  uint32_t next_seq;

  /* Counters.  Read under the lock by the report, written by whichever
   * thread owns the event.
   */

  uint32_t captured;
  uint32_t sent;
  uint32_t dropped;
  uint32_t failed;
  uint32_t noreply;
  uint64_t bytes;
  unsigned int worst_backlog;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stream_pick_ready
 *
 * Description:
 *   Lowest-numbered queued chunk, or NULL.  Called with the lock held.
 *
 *   Lowest-numbered rather than first-found so chunks leave in the order
 *   they were recorded: the receiver can cope with gaps but reassembling
 *   out-of-order arrivals is needless work to hand it.
 *
 ****************************************************************************/

static struct stream_slot_s *
stream_pick_ready(struct audio_test_stream_ctx_s *ctx)
{
  struct stream_slot_s *best = NULL;
  unsigned int i;

  for (i = 0; i < STREAM_SLOTS; i++)
    {
      if (ctx->slot[i].state != SLOT_READY)
        {
          continue;
        }

      if (best == NULL || ctx->slot[i].sequence < best->sequence)
        {
          best = &ctx->slot[i];
        }
    }

  return best;
}

/****************************************************************************
 * Name: stream_backlog
 *
 * Description:
 *   Queued chunk count.  Called with the lock held.
 *
 ****************************************************************************/

static unsigned int stream_backlog(struct audio_test_stream_ctx_s *ctx)
{
  unsigned int n = 0;
  unsigned int i;

  for (i = 0; i < STREAM_SLOTS; i++)
    {
      if (ctx->slot[i].state == SLOT_READY)
        {
          n++;
        }
    }

  return n;
}

/****************************************************************************
 * Name: stream_claim_slot
 *
 * Description:
 *   Give capture a slot to fill, dropping the oldest queued chunk if that is
 *   what it takes.  Called with the lock held.
 *
 *   A slot the uploader is working on is never taken: it is reading that
 *   memory, and overwriting it would corrupt the chunk in flight rather than
 *   the one being abandoned.  With one uploader at most one slot is BUSY, so
 *   there is always something else to take.
 *
 ****************************************************************************/

static struct stream_slot_s *
stream_claim_slot(struct audio_test_stream_ctx_s *ctx)
{
  struct stream_slot_s *victim;
  unsigned int i;

  for (i = 0; i < STREAM_SLOTS; i++)
    {
      if (ctx->slot[i].state == SLOT_FREE)
        {
          return &ctx->slot[i];
        }
    }

  victim = stream_pick_ready(ctx);
  if (victim != NULL)
    {
      ctx->dropped++;
      return victim;
    }

  return NULL;
}

/****************************************************************************
 * Name: stream_connect
 *
 * Description:
 *   Connected socket to the receiver, or a negated errno.
 *
 *   The connect is done non-blocking with an explicit poll() so it cannot
 *   sit for the stack's default timeout: see STREAM_CONNECT_MS.
 *
 ****************************************************************************/

static int stream_connect(struct audio_test_stream_ctx_s *ctx)
{
  struct sockaddr_in addr;
  struct timeval tv;
  struct pollfd pfd;
  int flags;
  int sock;
  int ret;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)ctx->port);
  if (inet_pton(AF_INET, ctx->host, &addr.sin_addr) != 1)
    {
      return -EINVAL;
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      return -errno;
    }

  flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS)
    {
      ret = -errno;
      close(sock);
      return ret;
    }

  if (ret < 0)
    {
      pfd.fd = sock;
      pfd.events = POLLOUT;
      pfd.revents = 0;

      ret = poll(&pfd, 1, STREAM_CONNECT_MS);
      if (ret <= 0)
        {
          close(sock);
          return ret == 0 ? -ETIMEDOUT : -errno;
        }

      if ((pfd.revents & POLLOUT) == 0)
        {
          close(sock);
          return -ECONNREFUSED;
        }

      /* POLLOUT alone does not mean success: a refused connection also
       * wakes the poll, and the error is only visible through the socket.
       */

      {
        int err = 0;
        socklen_t len = sizeof(err);

        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
            err != 0)
          {
            close(sock);
            return -err;
          }
      }
    }

  fcntl(sock, F_SETFL, flags);

  tv.tv_sec = STREAM_SEND_MS / 1000;
  tv.tv_usec = (STREAM_SEND_MS % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  tv.tv_sec = STREAM_RECV_MS / 1000;
  tv.tv_usec = (STREAM_RECV_MS % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  return sock;
}

/****************************************************************************
 * Name: stream_send_all
 ****************************************************************************/

static int stream_send_all(int sock, const void *data, size_t len)
{
  const uint8_t *p = data;
  size_t sent = 0;

  while (sent < len)
    {
      ssize_t n = send(sock, p + sent, len - sent, 0);

      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          return n < 0 ? -errno : -EIO;
        }

      sent += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Name: stream_post
 *
 * Description:
 *   Upload one encoded chunk.
 *
 *   Framed as an HTTP POST because that is what the real endpoint expects,
 *   and because the metadata the plan calls for -- sequence, start offset,
 *   duration -- has to travel with the audio somehow.  Putting it in headers
 *   rather than inventing a binary prefix means the same bytes work against a
 *   plain HTTP server, and a stand-in receiver stays a few lines long.
 *
 * Returned Value:
 *   Zero if the chunk was accepted, -ENOMSG if it was sent but nothing
 *   answered, another negated errno if it was not sent.
 *
 ****************************************************************************/

static int stream_post(struct audio_test_stream_ctx_s *ctx,
                       const struct stream_slot_s *slot,
                       const uint8_t *body, size_t bodylen,
                       unsigned int duration_ms)
{
  char header[512];
  char reply[128];
  int sock;
  int len;
  int ret;
  ssize_t got;

  sock = stream_connect(ctx);
  if (sock < 0)
    {
      return sock;
    }

  len = snprintf(header, sizeof(header),
                 "POST /v1/audio/chunk HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "User-Agent: bk7258-audio_test\r\n"
                 "Content-Type: audio/ogg\r\n"
                 "Content-Length: %zu\r\n"
                 "X-Session-Id: %08lx\r\n"
                 "X-Sequence: %lu\r\n"
                 "X-Start-Ms: %lu\r\n"
                 "X-Duration-Ms: %u\r\n"
                 "X-Sample-Rate: %u\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 ctx->host, ctx->port, bodylen,
                 (unsigned long)ctx->session,
                 (unsigned long)slot->sequence,
                 (unsigned long)slot->sequence * ctx->chunk_ms,
                 duration_ms, ctx->rate);

  if (len <= 0 || (size_t)len >= sizeof(header))
    {
      close(sock);
      return -EINVAL;
    }

  ret = stream_send_all(sock, header, (size_t)len);
  if (ret == OK)
    {
      ret = stream_send_all(sock, body, bodylen);
    }

  if (ret < 0)
    {
      close(sock);
      return ret;
    }

  /* Read the status line if one comes.  A stand-in receiver that just
   * writes files may close without replying, which is not a reason to call
   * the upload failed -- the bytes are already there -- so that case is
   * counted separately instead.
   */

  got = recv(sock, reply, sizeof(reply) - 1, 0);
  close(sock);

  if (got <= 0)
    {
      return -ENOMSG;
    }

  reply[got] = '\0';
  if (strstr(reply, " 200") == NULL && strstr(reply, " 204") == NULL)
    {
      printf("audio_test: chunk %lu rejected: %.32s\n",
             (unsigned long)slot->sequence, reply);
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: stream_upload_one
 ****************************************************************************/

static void stream_upload_one(struct audio_test_stream_ctx_s *ctx,
                              struct stream_slot_s *slot)
{
  unsigned int duration_ms;
  size_t nsamples;
  size_t enclen = 0;
  unsigned int attempt;
  int ret;

  nsamples = slot->used / sizeof(int16_t);
  duration_ms = (unsigned int)(nsamples * 1000 / ctx->rate);

  ret = audio_test_ogg_encode(ctx->encoder, (const int16_t *)slot->pcm,
                              nsamples,
                              /* A serial per chunk: these are separate
                               * logical streams that happen to share a
                               * connection sequence, and a decoder that sees
                               * two streams with one serial treats the second
                               * as a continuation of the first.
                               */
                              0x4f505553u ^ slot->sequence,
                              ctx->encbuf, STREAM_ENCODE_CAP, &enclen);
  if (ret < 0)
    {
      printf("audio_test: chunk %lu encode failed: %d\n",
             (unsigned long)slot->sequence, ret);
      pthread_mutex_lock(&ctx->lock);
      ctx->failed++;
      pthread_mutex_unlock(&ctx->lock);
      return;
    }

  for (attempt = 0; attempt < STREAM_ATTEMPTS; attempt++)
    {
      ret = stream_post(ctx, slot, ctx->encbuf, enclen, duration_ms);
      if (ret == OK || ret == -ENOMSG)
        {
          break;
        }

      if (attempt + 1 < STREAM_ATTEMPTS)
        {
          printf("audio_test: chunk %lu attempt %u failed (%d), retrying\n",
                 (unsigned long)slot->sequence, attempt + 1, ret);
        }
    }

  pthread_mutex_lock(&ctx->lock);

  if (ret == OK || ret == -ENOMSG)
    {
      ctx->sent++;
      ctx->bytes += enclen;

      if (ret == -ENOMSG)
        {
          ctx->noreply++;
        }
    }
  else
    {
      ctx->failed++;
      printf("audio_test: chunk %lu gave up: %d\n",
             (unsigned long)slot->sequence, ret);
    }

  pthread_mutex_unlock(&ctx->lock);
}

/****************************************************************************
 * Name: stream_uploader
 ****************************************************************************/

static void *stream_uploader(void *arg)
{
  struct audio_test_stream_ctx_s *ctx = arg;

  pthread_mutex_lock(&ctx->lock);

  for (; ; )
    {
      struct stream_slot_s *slot;

      while ((slot = stream_pick_ready(ctx)) == NULL && !ctx->quit)
        {
          pthread_cond_wait(&ctx->cond, &ctx->lock);
        }

      /* Only leave once the queue is empty as well as the flag set, so a
       * stop does not throw away chunks that were already recorded.
       */

      if (slot == NULL)
        {
          break;
        }

      slot->state = SLOT_BUSY;
      pthread_mutex_unlock(&ctx->lock);

      stream_upload_one(ctx, slot);

      pthread_mutex_lock(&ctx->lock);
      slot->used = 0;
      slot->state = SLOT_FREE;

      /* Capture may be waiting for nothing in particular, but a stop
       * waiting for the queue to drain is watching this.
       */

      pthread_cond_broadcast(&ctx->cond);
    }

  pthread_mutex_unlock(&ctx->lock);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct audio_test_stream_ctx_s *
audio_test_stream_open(const char *host, int port, unsigned int rate,
                       unsigned int chunk_ms, unsigned int bitrate)
{
  struct audio_test_stream_ctx_s *ctx;
  struct sched_param param;
  pthread_attr_t attr;
  unsigned int i;
  int prio;
  int ret;

  if (host == NULL || rate == 0 || chunk_ms == 0)
    {
      return NULL;
    }

  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    {
      printf("audio_test: no memory for the stream context\n");
      return NULL;
    }

  strncpy(ctx->host, host, sizeof(ctx->host) - 1);
  ctx->port = port;
  ctx->rate = rate;
  ctx->chunk_ms = chunk_ms;
  ctx->bitrate = bitrate;
  ctx->chunk_bytes = (size_t)rate * chunk_ms / 1000 * sizeof(int16_t);
  ctx->session = (uint32_t)time(NULL);

  /* PSRAM for the ring and the encode buffer: together they are most of a
   * megabyte at the larger chunk sizes, which SRAM does not have, and both
   * are touched at audio rate rather than at memory speed.
   */

  for (i = 0; i < STREAM_SLOTS; i++)
    {
      ctx->slot[i].pcm = bk7258_psram_malloc(ctx->chunk_bytes);
      if (ctx->slot[i].pcm == NULL)
        {
          printf("audio_test: PSRAM has no room for %u slots of %zu KiB\n",
                 STREAM_SLOTS, ctx->chunk_bytes / 1024);
          goto err;
        }
    }

  ctx->encbuf = bk7258_psram_malloc(STREAM_ENCODE_CAP);
  if (ctx->encbuf == NULL)
    {
      printf("audio_test: PSRAM has no room for the encode buffer\n");
      goto err;
    }

  /* PSRAM for the encoder state, deliberately: the 32 KiB uploader stack can
   * only come from the SRAM heap, and letting the 38 KiB encoder state take
   * SRAM first is what made pthread_create() below fail with ENOMEM.
   */

  ctx->encoder = audio_test_ogg_encoder_create(rate, bitrate, true);
  if (ctx->encoder == NULL)
    {
      printf("audio_test: could not create the opus encoder\n");
      goto err;
    }

  pthread_mutex_init(&ctx->lock, NULL);
  pthread_cond_init(&ctx->cond, NULL);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, STREAM_STACKSIZE);

  prio = sched_getparam(0, &param) == 0 ? param.sched_priority : 100;
  prio -= STREAM_PRIO_BELOW;
  if (prio < 1)
    {
      prio = 1;
    }

  param.sched_priority = prio;
  pthread_attr_setschedparam(&attr, &param);

  ret = pthread_create(&ctx->uploader, &attr, stream_uploader, ctx);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      struct mallinfo mem = mallinfo();

      /* Report the SRAM heap alongside the error: this call fails with
       * ENOMEM when the stack does not fit, and the number that decides
       * that is invisible from the error code alone.
       */

      printf("audio_test: pthread_create failed: %d (wanted a %d byte "
             "stack, SRAM heap has %zu byte(s) free)\n",
             ret, STREAM_STACKSIZE, (size_t)mem.fordblks);
      pthread_cond_destroy(&ctx->cond);
      pthread_mutex_destroy(&ctx->lock);
      goto err;
    }

  ctx->running = true;

  printf("audio_test: streaming to %s:%d, %u ms chunks of %zu KiB, "
         "%u slots, uploader at priority %d\n",
         ctx->host, ctx->port, chunk_ms, ctx->chunk_bytes / 1024,
         STREAM_SLOTS, prio);

  return ctx;

err:
  if (ctx->encoder != NULL)
    {
      audio_test_ogg_encoder_destroy(ctx->encoder);
    }

  if (ctx->encbuf != NULL)
    {
      bk7258_psram_free(ctx->encbuf);
    }

  for (i = 0; i < STREAM_SLOTS; i++)
    {
      if (ctx->slot[i].pcm != NULL)
        {
          bk7258_psram_free(ctx->slot[i].pcm);
        }
    }

  free(ctx);
  return NULL;
}

void audio_test_stream_feed(struct audio_test_stream_ctx_s *ctx,
                            const int16_t *samples, size_t nsamples,
                            unsigned int stride)
{
  size_t index = 0;

  if (ctx == NULL || samples == NULL || stride == 0)
    {
      return;
    }

  while (index + stride <= nsamples)
    {
      int16_t *dst;
      size_t room;
      size_t take;
      size_t i;

      if (ctx->cur == NULL)
        {
          pthread_mutex_lock(&ctx->lock);

          ctx->cur = stream_claim_slot(ctx);
          if (ctx->cur != NULL)
            {
              ctx->cur->state = SLOT_FILLING;
              ctx->cur->used = 0;
              ctx->cur->sequence = ctx->next_seq++;
            }

          pthread_mutex_unlock(&ctx->lock);

          if (ctx->cur == NULL)
            {
              /* Every slot is in flight.  With one uploader this cannot
               * happen, so rather than silently discarding audio say so:
               * if it ever does, the ring is the wrong size.
               */

              printf("audio_test: no slot available, %zu samples lost\n",
                     nsamples - index);
              return;
            }
        }

      dst = (int16_t *)(ctx->cur->pcm + ctx->cur->used);
      room = (ctx->chunk_bytes - ctx->cur->used) / sizeof(int16_t);
      take = (nsamples - index) / stride;

      if (take > room)
        {
          take = room;
        }

      if (stride == 1)
        {
          memcpy(dst, samples + index, take * sizeof(int16_t));
        }
      else
        {
          for (i = 0; i < take; i++)
            {
              dst[i] = samples[index + i * stride];
            }
        }

      index += take * stride;
      ctx->cur->used += take * sizeof(int16_t);

      if (ctx->cur->used >= ctx->chunk_bytes)
        {
          unsigned int backlog;

          pthread_mutex_lock(&ctx->lock);

          ctx->cur->state = SLOT_READY;
          ctx->captured++;

          backlog = stream_backlog(ctx);
          if (backlog > ctx->worst_backlog)
            {
              ctx->worst_backlog = backlog;
            }

          pthread_cond_signal(&ctx->cond);
          pthread_mutex_unlock(&ctx->lock);

          ctx->cur = NULL;
        }
    }
}

void audio_test_stream_close(struct audio_test_stream_ctx_s *ctx)
{
  unsigned int i;

  if (ctx == NULL)
    {
      return;
    }

  /* Queue the tail of the recording before stopping.  A session usually
   * ends because whatever was being recorded just finished, so the last
   * partial chunk is the least disposable one.
   */

  if (ctx->cur != NULL && ctx->cur->used > 0)
    {
      pthread_mutex_lock(&ctx->lock);
      ctx->cur->state = SLOT_READY;
      ctx->captured++;
      pthread_cond_signal(&ctx->cond);
      pthread_mutex_unlock(&ctx->lock);
      ctx->cur = NULL;
    }
  else if (ctx->cur != NULL)
    {
      pthread_mutex_lock(&ctx->lock);
      ctx->cur->state = SLOT_FREE;
      pthread_mutex_unlock(&ctx->lock);
      ctx->cur = NULL;
    }

  if (ctx->running)
    {
      pthread_mutex_lock(&ctx->lock);
      ctx->quit = true;
      pthread_cond_broadcast(&ctx->cond);
      pthread_mutex_unlock(&ctx->lock);

      pthread_join(ctx->uploader, NULL);
      ctx->running = false;
    }

  /* Reported from here rather than left to the caller: the counters only
   * describe a finished session once the uploader has been joined, and by
   * then the caller has no way to ask because the context is about to go.
   */

  audio_test_stream_report(ctx);

  pthread_cond_destroy(&ctx->cond);
  pthread_mutex_destroy(&ctx->lock);

  audio_test_ogg_encoder_destroy(ctx->encoder);
  bk7258_psram_free(ctx->encbuf);

  for (i = 0; i < STREAM_SLOTS; i++)
    {
      bk7258_psram_free(ctx->slot[i].pcm);
    }

  free(ctx);
}

void audio_test_stream_report(struct audio_test_stream_ctx_s *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  pthread_mutex_lock(&ctx->lock);

  printf("audio_test: chunks captured=%lu sent=%lu dropped=%lu failed=%lu"
         " no-reply=%lu, %llu byte(s) uploaded, worst backlog %u/%u\n",
         (unsigned long)ctx->captured, (unsigned long)ctx->sent,
         (unsigned long)ctx->dropped, (unsigned long)ctx->failed,
         (unsigned long)ctx->noreply, (unsigned long long)ctx->bytes,
         ctx->worst_backlog, STREAM_SLOTS);

  pthread_mutex_unlock(&ctx->lock);
}
