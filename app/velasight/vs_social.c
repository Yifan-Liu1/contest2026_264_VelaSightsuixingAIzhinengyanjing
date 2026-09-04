/****************************************************************************
 * app/velasight/vs_social.c
 *
 * The social session's schedule.  See include/vs_social.h for what this layer
 * owns and why it is four threads.
 *
 * Two things in here are worth reading before changing anything.
 *
 * The bounded ring between capture and upload is not a buffer, it is a policy.
 * When the network stalls it drops the oldest entry, which is the correct
 * answer for this product: an emotion result is about the moment it was
 * sampled, and a frame from four seconds ago is worth less than the frame
 * arriving now.  Growing the ring to "avoid losing data" would trade a visible
 * drop count for an invisible latency, and the latency is the worse of the two.
 *
 * The alert generation counter is what keeps late advice from contradicting the
 * screen.  The cloud produces the emotion for a frame and, when that frame was
 * extreme, the spoken advice for it some seconds later.  By the time the advice
 * lands the user's expression may have moved on.  Advice is therefore stamped
 * with the generation of the alert that caused it and discarded if that alert
 * has since cleared -- the same idea as vs_app.c's request_id, one level down.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <arch/chip/bk7258_psram.h>

#include <agent_config.h>

#include "audio_test_ogg.h"

#include "include/vs_app.h"
#include "include/vs_audio.h"
#include "include/vs_cloud.h"
#include "include/vs_history.h"
#include "include/vs_media.h"
#include "include/vs_social.h"
#include "include/vs_types.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Fallbacks for a build without this application's Kconfig fragment.  Same
 * values as the Kconfig defaults; see there for why each one is what it is.
 */

#ifndef CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS
#  define CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS 340
#endif

#ifndef CONFIG_VS_SOCIAL_IMAGE_WIDTH
#  define CONFIG_VS_SOCIAL_IMAGE_WIDTH 480
#endif

#ifndef CONFIG_VS_SOCIAL_IMAGE_HEIGHT
#  define CONFIG_VS_SOCIAL_IMAGE_HEIGHT 480
#endif

#ifndef CONFIG_VS_SOCIAL_CAPTURE_FPS
#  define CONFIG_VS_SOCIAL_CAPTURE_FPS 5
#endif

#ifndef CONFIG_VS_SOCIAL_STACKSIZE_SESSION
#  define CONFIG_VS_SOCIAL_STACKSIZE_SESSION 16384
#endif

#ifndef CONFIG_VS_SOCIAL_STACKSIZE_AUDIO
#  define CONFIG_VS_SOCIAL_STACKSIZE_AUDIO 32768
#endif

#ifndef CONFIG_VS_SOCIAL_STACKSIZE_UPLOAD
#  define CONFIG_VS_SOCIAL_STACKSIZE_UPLOAD 16384
#endif

#ifndef CONFIG_VS_SOCIAL_STACKSIZE_CAPTURE
#  define CONFIG_VS_SOCIAL_STACKSIZE_CAPTURE 8192
#endif

#ifndef CONFIG_VS_SOCIAL_AUDIO_CHUNK_MS
#  define CONFIG_VS_SOCIAL_AUDIO_CHUNK_MS 2000
#endif

#ifndef CONFIG_VS_SOCIAL_AUDIO_BITRATE
#  define CONFIG_VS_SOCIAL_AUDIO_BITRATE 24000
#endif

#ifndef CONFIG_VS_SOCIAL_POLL_INTERVAL_MS
#  define CONFIG_VS_SOCIAL_POLL_INTERVAL_MS 1500
#endif

#ifndef CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS
#  define CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS 60000
#endif

#ifndef CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES
#  define CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES 8388608
#endif

#ifndef CONFIG_VS_SOCIAL_MINUTES_AUDIO_PATH
#  define CONFIG_VS_SOCIAL_MINUTES_AUDIO_PATH \
     "/mnt/sdnand/ai_agent/minutes.wav"
#endif

/* One read from storage and one write to the DAC ring per iteration.
 *
 * 8 KB is 170 ms of 24 kHz 16-bit mono, which is short enough that a request
 * to stop speaking is acted on promptly and long enough that the file is read
 * in a few dozen syscalls rather than a few thousand.  It is also the whole
 * per-transfer memory cost of playback, so it is deliberately not generous.
 */

#define SOCIAL_PLAY_CHUNK 8192

/* How the two progress lines are throttled.
 *
 * Uploads are regular, so a count stride suits them: one line per ten
 * transfers is four or five lines in a typical session.  Drops are not
 * regular -- a session may lose two frames or fifty in one stall -- so those
 * are throttled by time instead, which reports a lone drop immediately and
 * still collapses a burst into a single line.
 */

#define SOCIAL_UPLOAD_REPORT_EVERY 10
#define SOCIAL_DROP_REPORT_MS      5000

#ifndef CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS
#  define CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS 3
#endif

#ifndef CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS
#  define CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS 8000
#endif

#ifndef CONFIG_VS_SOCIAL_QUEUE_SLOTS
#  define CONFIG_VS_SOCIAL_QUEUE_SLOTS 4
#endif

#ifndef CONFIG_VS_SOCIAL_INFLIGHT_MAX
#  define CONFIG_VS_SOCIAL_INFLIGHT_MAX 16
#endif

#ifndef CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MIN_MS
#  define CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MIN_MS 250
#endif

#ifndef CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MAX_MS
#  define CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MAX_MS 8000
#endif

#ifndef CONFIG_VS_SOCIAL_DEAD_SESSION_STRIKES
#  define CONFIG_VS_SOCIAL_DEAD_SESSION_STRIKES 12
#endif



#define SOCIAL_TAG "vs_social"

/* Microphone format.  Fixed rather than configurable: the cloud interface
 * document settles the container (.ogg) but says nothing about the rate, and
 * 16 kHz mono is both what the driver does well and what the idle assistant's
 * ASR path already uses -- so a session and a voice question sound the same to
 * whatever is on the other end.  vs_audio_capture_open() accepts only 8000,
 * 16000 and 32000, and Opus encodes all three natively.
 */

#define SOCIAL_AUDIO_RATE     16000
#define SOCIAL_AUDIO_CHANNELS 1
#define SOCIAL_AUDIO_BITS     16

/* PCM bytes in one chunk. */

#define SOCIAL_CHUNK_SAMPLES \
  ((size_t)SOCIAL_AUDIO_RATE * CONFIG_VS_SOCIAL_AUDIO_CHUNK_MS / 1000u)
#define SOCIAL_CHUNK_BYTES (SOCIAL_CHUNK_SAMPLES * sizeof(int16_t))

/* Room for the encoded chunk.  Generous against the arithmetic -- a 2 s chunk
 * at 24 kbps is about 6 KB plus two header pages -- because an encode that
 * does not fit is a silently dropped chunk, and the buffer is drawn from PSRAM
 * once per session rather than per chunk.
 */

#define SOCIAL_OGG_BYTES 32768

/* How long one audio read may find nothing before the loop checks its flags
 * again.  Short enough that a pause or a finalize is acted on promptly,
 * long enough not to spin: vs_audio_capture_read() never blocks, so without a
 * sleep here this loop would be a busy wait.
 */

#define SOCIAL_AUDIO_IDLE_MS 20

/* Upper bound on the JSON the end-of-session response can occupy, drawn from
 * the same budget vs_cloud.c parses it into.  The record is written straight
 * from that buffer to SD-NAND, so one allocation serves both.
 */

#ifndef CONFIG_VS_SOCIAL_RESP_MAX_BYTES
#  define CONFIG_VS_SOCIAL_RESP_MAX_BYTES 65536
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One captured item waiting for the uploader.  data is owned by the slot and
 * released either by the uploader or by social_queue_flush().
 */

struct social_slot_s
{
  enum vs_cloud_media_e type;
  unsigned char        *data;
  size_t                len;
  bool                  from_psram;
  uint32_t              sequence;

  /* The alert generation live when this item was captured.  Only meaningful
   * for audio: it is what lets a piece of advice be matched against the alert
   * that asked for it.
   */

  uint32_t alert_gen;
};

/* One uploaded message whose result has not arrived. */

struct social_inflight_s
{
  char     msg_id[VS_CLOUD_MSG_ID_MAX];
  uint32_t alert_gen;

  /* Set once an IMAGE entry for this id has been seen.  The cloud emits an
   * AUDIO entry under the same msgId for an extreme frame, so an id is not
   * finished just because one entry arrived.
   */

  bool image_seen;
  bool audio_seen;
};

/* What social_poll_once() needs to do its work, gathered so it can live
 * somewhere other than that function's stack.  See social_state_s::poll.
 */

struct social_poll_scratch_s
{
  const char *ids[VS_CLOUD_POLL_MAX_IDS];
  char id_store[VS_CLOUD_POLL_MAX_IDS][VS_CLOUD_MSG_ID_MAX];
  struct vs_social_event_s events[VS_CLOUD_POLL_MAX_IDS * 2];
};

struct social_state_s
{
  pthread_mutex_t lock;
  pthread_cond_t  cond;

  bool     running;
  bool     paused;
  bool     stop_capture;   /* producers should stop */
  bool     finalize;       /* finalize was requested */
  bool     abort;          /* abandon without minutes */
  uint32_t request_id;

  /* Both producers have exited and nothing more will enter the ring.
   *
   * Distinct from stop_capture, and the distinction is load-bearing.  The
   * audio worker pushes its tail chunk *while* it is exiting, which is after
   * stop_capture is set; an uploader that treated stop_capture as "no more
   * work is coming" would leave on the broadcast that requested the stop and
   * the last two seconds of the conversation would be freed unsent.
   */

  bool     producers_done;

  struct vs_cloud_session_s session;

  /* Ring between the two producers and the uploader. */

  struct social_slot_s slot[CONFIG_VS_SOCIAL_QUEUE_SLOTS];
  uint8_t  read;
  uint8_t  write;
  uint8_t  count;
  uint32_t image_seq;
  uint32_t audio_seq;
  uint32_t dropped;
  uint32_t uploaded;
  uint32_t upload_failed;

  /* Progress reporting for the two things that used to be visible only in the
   * end-of-session totals.  A count alone says how many; these say when, which
   * is what distinguishes a queue that is steadily behind from one that stalled
   * for a moment.  Both are coalesced -- see social_queue_push() and the upload
   * worker -- because at three frames a second a line each would bury the log.
   */

  uint32_t drop_reported;    /* value of dropped when the last line printed */
  uint64_t drop_report_ms;   /* when that was, 0 for never */
  uint32_t upload_reported;  /* value of uploaded when the last line printed */
  uint64_t upload_window_ms; /* total upload time since that line */

  /* Messages awaiting results. */

  struct social_inflight_s inflight[CONFIG_VS_SOCIAL_INFLIGHT_MAX];
  uint8_t inflight_count;
  uint32_t inflight_retired;

  /* Alert debounce.  See the file header for what alert_gen is for. */

  uint32_t alert_gen;
  uint8_t  extreme_streak;
  uint8_t  calm_streak;
  bool     alert_active;
  uint64_t alert_since_ms;

  struct vs_media_stream_s *camera;
  struct vs_audio_cap_s    *mic;

  /* social_poll_once()'s working set, off its stack.
   *
   * These three arrays are 12 KiB together, almost all of it the events one:
   * VS_CLOUD_POLL_MAX_IDS is 16, each id can answer with both an image and an
   * audio event so the array is twice that, and vs_social_event_s is 364 bytes
   * because it carries msg_id, display_text, suggestion and log inline.
   *
   * As locals they were a 12 KiB frame on an 8 KiB thread stack, which the
   * ARMv8-M stack limit caught as a UsageFault on the first call -- three
   * milliseconds into every session, every time.  Enlarging the stack alone
   * would have left a function whose frame is larger than most tasks' entire
   * stacks, so they moved here instead, allocated once per session from PSRAM
   * for the same reason the ogg encoder's state is.
   *
   * A single allocation shared by one caller is safe because there is one:
   * social_poll_once() is called from social_session_worker() and from nowhere
   * else, on that one thread.
   */

  struct social_poll_scratch_s *poll;
  bool poll_psram;

  pthread_t session_thread;
  pthread_t capture_thread;
  pthread_t audio_thread;
  pthread_t upload_thread;
  bool      capture_joinable;
  bool      audio_joinable;
  bool      upload_joinable;
  bool      session_joinable;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct social_state_s g_social =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t social_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Post one event, stamped with this session's request id.
 *
 * Retries while the UI queue is full rather than dropping.  The queue is eight
 * deep and drained every frame, so a full queue means the UI is momentarily
 * behind, not that it has stopped -- and the events this module sends are all
 * ones the page transition depends on.
 */

/* How hard social_post() tries before giving up on a full UI queue.
 *
 * Deliberately short.  The caller may be the UI thread itself, which is the
 * only thing that drains that queue, so waiting cannot help there and the
 * whole budget is spent before the user notices a stall.  For a worker thread
 * it is enough to ride out a UI pass that is busy pushing a panel.
 */

#define SOCIAL_POST_ATTEMPTS  3
#define SOCIAL_POST_RETRY_US  2000

static void social_post(enum vs_app_event_e type, int error,
                        enum vs_emotion_e emotion, uint32_t color,
                        const char *text)
{
  struct vs_app_event_s event;
  unsigned int attempt;

  memset(&event, 0, sizeof(event));
  event.type       = type;
  event.request_id = g_social.request_id;
  event.error      = error;
  event.emotion    = emotion;
  event.color      = color;

  if (text != NULL)
    {
      snprintf(event.text, sizeof(event.text), "%s", text);
    }

  /* Bounded, and the bound is the point: this used to retry forever.
   *
   * vs_social_pause() and vs_social_resume() are called from vs_handle_event(),
   * which runs on the UI thread -- and the UI thread is the only consumer of
   * this queue.  So on a full queue that loop waited for space that only the
   * caller could free, inside the caller.  The screen froze with no highlight,
   * because the UI never got back to vs_render() let alone vs_display_tick(),
   * and every later key was dead too.  That is what "pressing pause does
   * nothing and the screen stops responding" was.
   *
   * A worker thread spinning here is merely wrong rather than fatal, but it is
   * still wrong: a producer that cannot deliver should drop and carry on, not
   * stall the session.
   *
   * Dropping a UI notification is a visible glitch -- a page that does not
   * advance until the next event arrives.  Freezing the display is not
   * recoverable at all.  The transient social pages carry their own deadline
   * (see vs_app.c) precisely so that a dropped event costs a timeout and an
   * error page rather than a session that can never be left.
   */

  for (attempt = 0; attempt < SOCIAL_POST_ATTEMPTS; attempt++)
    {
      if (vs_app_post_event(&event) != -EAGAIN)
        {
          return;
        }

      usleep(SOCIAL_POST_RETRY_US);
    }

  printf("%s: UI event %d dropped, queue full\n", SOCIAL_TAG, (int)type);
}

/****************************************************************************
 * Name: social_alloc / social_free
 *
 * Description:
 *   PSRAM first, heap second, for the buffers a session holds across a network
 *   round trip.  Same reasoning as vs_media.c: the SRAM heap is the only place
 *   pthread stacks can come from, and four session threads want theirs.
 *
 ****************************************************************************/

static unsigned char *social_alloc(size_t len, bool *from_psram)
{
  unsigned char *p = bk7258_psram_malloc(len);

  *from_psram = true;
  if (p == NULL)
    {
      *from_psram = false;
      p = malloc(len);
    }

  return p;
}

static void social_free(unsigned char *p, bool from_psram)
{
  if (p == NULL)
    {
      return;
    }

  if (from_psram)
    {
      bk7258_psram_free(p);
    }
  else
    {
      free(p);
    }
}

/****************************************************************************
 * The capture/upload ring
 ****************************************************************************/

/* Hand one item to the uploader.  Takes ownership of data on every path,
 * including the drop path, so a caller never has to know whether it was
 * accepted.
 *
 * Called with the lock not held.
 */

static void social_queue_push(enum vs_cloud_media_e type,
                              unsigned char *data, size_t len,
                              bool from_psram, uint32_t sequence,
                              uint32_t alert_gen)
{
  struct social_slot_s *slot;
  unsigned char *evicted = NULL;
  bool evicted_psram = false;
  bool report = false;
  uint32_t drop_total = 0;
  uint32_t drop_since = 0;

  pthread_mutex_lock(&g_social.lock);

  if (g_social.count == CONFIG_VS_SOCIAL_QUEUE_SLOTS)
    {
      /* Drop the oldest rather than the newest.  Deliberate: see the file
       * header.  The eviction is freed after the lock is released, because
       * bk7258_psram_free() is not something to hold a lock across.
       */

      evicted       = g_social.slot[g_social.read].data;
      evicted_psram = g_social.slot[g_social.read].from_psram;
      g_social.read = (uint8_t)((g_social.read + 1) %
                                CONFIG_VS_SOCIAL_QUEUE_SLOTS);
      g_social.count--;
      g_social.dropped++;

      /* Rate limited by time rather than by count, which is what suits an
       * event this uneven.  A first drop after a quiet stretch prints at once
       * -- that timestamp is the point, it says when the queue started losing
       * -- and a burst collapses into one line carrying the count, instead of
       * fifty lines that push the cause off the screen.
       */

      {
        uint64_t now = social_now_ms();

        if (g_social.drop_report_ms == 0 ||
            now - g_social.drop_report_ms >= SOCIAL_DROP_REPORT_MS)
          {
            report     = true;
            drop_total = g_social.dropped;
            drop_since = g_social.dropped - g_social.drop_reported;

            g_social.drop_reported  = g_social.dropped;
            g_social.drop_report_ms = now;
          }
      }
    }

  slot = &g_social.slot[g_social.write];
  slot->type       = type;
  slot->data       = data;
  slot->len        = len;
  slot->from_psram = from_psram;
  slot->sequence   = sequence;
  slot->alert_gen  = alert_gen;

  g_social.write = (uint8_t)((g_social.write + 1) %
                             CONFIG_VS_SOCIAL_QUEUE_SLOTS);
  g_social.count++;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  /* Printed outside the lock, for the same reason the eviction is freed
   * outside it: this runs on the capture threads, and the console is a
   * mailbox channel that can block.
   */

  if (report)
    {
      printf("%s: queue full, dropped %lu (+%lu since the last line)\n",
             SOCIAL_TAG, (unsigned long)drop_total,
             (unsigned long)drop_since);
    }

  social_free(evicted, evicted_psram);
}

/* Take the next item, waiting until one arrives or the uploader should stop.
 *
 * Returns true with *out filled.  False means "stop": the producers have
 * finished and the ring is empty.
 */

static bool social_queue_pop(struct social_slot_s *out)
{
  pthread_mutex_lock(&g_social.lock);

  while (g_social.count == 0)
    {
      /* Waits on producers_done, not on stop_capture.  See its declaration:
       * leaving when the stop is merely *requested* would drop the tail audio
       * chunk, which the audio worker pushes on its way out.
       *
       * abort is the exception -- there nothing is going to be uploaded at all,
       * so waiting for the producers to finish producing would only delay the
       * teardown.
       */

      if (g_social.abort || g_social.producers_done)
        {
          pthread_mutex_unlock(&g_social.lock);
          return false;
        }

      pthread_cond_wait(&g_social.cond, &g_social.lock);
    }

  *out = g_social.slot[g_social.read];
  memset(&g_social.slot[g_social.read], 0, sizeof(g_social.slot[0]));
  g_social.read = (uint8_t)((g_social.read + 1) %
                            CONFIG_VS_SOCIAL_QUEUE_SLOTS);
  g_social.count--;
  pthread_mutex_unlock(&g_social.lock);
  return true;
}

/* Release everything still queued.  For the abort path and for teardown. */

static void social_queue_flush(void)
{
  struct social_slot_s drop[CONFIG_VS_SOCIAL_QUEUE_SLOTS];
  uint8_t n = 0;
  uint8_t i;

  pthread_mutex_lock(&g_social.lock);
  while (g_social.count != 0)
    {
      drop[n++] = g_social.slot[g_social.read];
      memset(&g_social.slot[g_social.read], 0, sizeof(g_social.slot[0]));
      g_social.read = (uint8_t)((g_social.read + 1) %
                                CONFIG_VS_SOCIAL_QUEUE_SLOTS);
      g_social.count--;
    }
  pthread_mutex_unlock(&g_social.lock);

  for (i = 0; i < n; i++)
    {
      social_free(drop[i].data, drop[i].from_psram);
    }
}

/****************************************************************************
 * In-flight message tracking
 ****************************************************************************/

/* Called with the lock held. */

static void social_inflight_add(const char *msg_id, uint32_t alert_gen)
{
  struct social_inflight_s *entry;

  if (g_social.inflight_count == CONFIG_VS_SOCIAL_INFLIGHT_MAX)
    {
      /* Retire the oldest unanswered.  This loses one frame's emotion result
       * and nothing more: the cloud still has the data and still counts it in
       * the end-of-session timeline, which is where the record comes from.
       */

      memmove(&g_social.inflight[0], &g_social.inflight[1],
              sizeof(g_social.inflight[0]) *
              (CONFIG_VS_SOCIAL_INFLIGHT_MAX - 1));
      g_social.inflight_count--;
      g_social.inflight_retired++;
    }

  entry = &g_social.inflight[g_social.inflight_count++];
  memset(entry, 0, sizeof(*entry));
  snprintf(entry->msg_id, sizeof(entry->msg_id), "%s", msg_id);
  entry->alert_gen = alert_gen;
}

/* Drop one entry by index.  Called with the lock held. */

static void social_inflight_remove(uint8_t index)
{
  if (index >= g_social.inflight_count)
    {
      return;
    }

  if (index + 1 < g_social.inflight_count)
    {
      memmove(&g_social.inflight[index], &g_social.inflight[index + 1],
              sizeof(g_social.inflight[0]) *
              (size_t)(g_social.inflight_count - index - 1));
    }

  g_social.inflight_count--;
}

/****************************************************************************
 * The capture thread
 ****************************************************************************/

static void *social_capture_worker(void *arg)
{
  uint64_t next = social_now_ms();

  (void)arg;

  for (; ; )
    {
      struct vs_media_frame_s frame;
      uint64_t now;
      bool paused;
      int ret;

      pthread_mutex_lock(&g_social.lock);
      if (g_social.stop_capture || g_social.abort)
        {
          pthread_mutex_unlock(&g_social.lock);
          break;
        }

      paused = g_social.paused;
      pthread_mutex_unlock(&g_social.lock);

      if (paused)
        {
          /* The camera stays streaming.  Frames it produces are dequeued and
           * dropped by the driver's two-deep queue, which costs nothing and
           * avoids a STREAMOFF/STREAMON pair on every pause -- long enough to
           * be visible, and the resume is the moment the user is watching.
           */

          usleep(SOCIAL_AUDIO_IDLE_MS * 1000);
          next = social_now_ms();
          continue;
        }

      now = social_now_ms();
      if (now < next)
        {
          usleep((useconds_t)(next - now) * 1000);
          continue;
        }

      /* The deadline advances from the previous deadline, not from now, so a
       * frame that ran long is absorbed instead of pushing every later frame
       * back.  Resynchronised when it has fallen more than one interval
       * behind, which is the case where trying to catch up would just capture
       * two frames back to back for no benefit.
       */

      next += CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;
      if (next < now)
        {
          next = now + CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;
        }

      ret = vs_media_stream_grab(g_social.camera, &frame,
                                 CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS * 3u);
      if (ret == -ECANCELED)
        {
          break;
        }

      if (ret < 0)
        {
          /* -EBADMSG and -ETIMEDOUT are both recoverable and both expected
           * occasionally: this encoder does emit the odd frame without a scan
           * header.  Logging every one would drown the session log at three
           * frames a second, so the count in vs_media_stream_stats() is what
           * carries it, printed once at teardown.
           */

          continue;
        }

      {
        uint32_t seq;
        uint32_t gen;

        pthread_mutex_lock(&g_social.lock);
        seq = ++g_social.image_seq;
        gen = g_social.alert_gen;
        pthread_mutex_unlock(&g_social.lock);

        /* Ownership of frame.data moves into the ring.  Not released here on
         * any path, which is why vs_media_frame_release() is not called.
         */

        social_queue_push(VS_CLOUD_MEDIA_IMAGE, frame.data, frame.len,
                          frame.from_psram, seq, gen);
      }
    }

  return NULL;
}

/****************************************************************************
 * The audio thread
 ****************************************************************************/

static void *social_audio_worker(void *arg)
{
  int16_t *pcm = NULL;
  unsigned char *ogg = NULL;
  bool pcm_psram = false;
  bool ogg_psram = false;
  void *encoder = NULL;
  size_t filled = 0;

  (void)arg;

  pcm = (int16_t *)social_alloc(SOCIAL_CHUNK_BYTES, &pcm_psram);
  ogg = social_alloc(SOCIAL_OGG_BYTES, &ogg_psram);

  /* PSRAM for the encoder state, for the reason its own header gives: the
   * 38 KiB of encoder plus 62 KiB of packet scratch in SRAM starved
   * pthread_create() of stack, and this module creates four threads.
   */

  encoder = audio_test_ogg_encoder_create(SOCIAL_AUDIO_RATE,
                                          CONFIG_VS_SOCIAL_AUDIO_BITRATE,
                                          true);

  if (pcm == NULL || ogg == NULL || encoder == NULL)
    {
      printf("%s: audio worker has no memory, session continues without "
             "audio\n", SOCIAL_TAG);
      goto out;
    }

  for (; ; )
    {
      bool paused;
      bool stopping;
      int got;

      pthread_mutex_lock(&g_social.lock);
      stopping = g_social.stop_capture || g_social.abort;
      paused   = g_social.paused;
      pthread_mutex_unlock(&g_social.lock);

      if (stopping)
        {
          break;
        }

      if (paused)
        {
          /* Drop what has accumulated rather than carrying it across the
           * pause.  A chunk spliced from before and after an arbitrary gap
           * would place speech at a timestamp it did not happen at, and the
           * timeline is the deliverable.
           *
           * The device stays open and running, and the ring is drained into
           * the buffer that is about to be reset.  Draining rather than
           * ignoring matters for the diagnostics: an untouched ring overflows
           * within a few hundred milliseconds and vs_audio_capture_level()
           * would then report dropped bytes for the whole pause, which is the
           * counter used to diagnose a consumer that cannot keep up.
           *
           * Stopping the ADC instead is not an option -- vs_audio_capture_stop()
           * is one-way and resuming would mean reopening the device.
           */

          filled = 0;
          (void)vs_audio_capture_read(g_social.mic, pcm, SOCIAL_CHUNK_BYTES);
          usleep(SOCIAL_AUDIO_IDLE_MS * 1000);
          continue;
        }

      got = vs_audio_capture_read(g_social.mic, (uint8_t *)pcm + filled,
                                  SOCIAL_CHUNK_BYTES - filled);
      if (got == -EAGAIN)
        {
          usleep(SOCIAL_AUDIO_IDLE_MS * 1000);
          continue;
        }

      if (got == -ECANCELED)
        {
          break;
        }

      if (got < 0)
        {
          printf("%s: capture read failed: %d\n", SOCIAL_TAG, got);
          break;
        }

      filled += (size_t)got;
      if (filled < SOCIAL_CHUNK_BYTES)
        {
          continue;
        }

      {
        unsigned char *copy;
        bool copy_psram;
        size_t encoded = 0;
        uint32_t seq;
        uint32_t gen;
        int ret;

        pthread_mutex_lock(&g_social.lock);
        seq = ++g_social.audio_seq;
        gen = g_social.alert_gen;
        pthread_mutex_unlock(&g_social.lock);

        ret = audio_test_ogg_encode(encoder, pcm,
                                    filled / sizeof(int16_t), seq,
                                    ogg, SOCIAL_OGG_BYTES, &encoded);
        filled = 0;

        if (ret < 0 || encoded == 0)
          {
            printf("%s: chunk %lu encode failed: %d\n", SOCIAL_TAG,
                   (unsigned long)seq, ret);
            continue;
          }

        /* The ring owns what it holds, and ogg is reused for the next chunk,
         * so this has to be a copy.  Sized to the encode rather than to
         * SOCIAL_OGG_BYTES: the difference is most of the buffer.
         */

        copy = social_alloc(encoded, &copy_psram);
        if (copy == NULL)
          {
            printf("%s: chunk %lu dropped, no memory\n", SOCIAL_TAG,
                   (unsigned long)seq);
            continue;
          }

        memcpy(copy, ogg, encoded);
        social_queue_push(VS_CLOUD_MEDIA_AUDIO, copy, encoded, copy_psram,
                          seq, gen);
      }
    }

  /* The tail.  A partial chunk is the end of the conversation, so it is
   * uploaded rather than discarded -- the integration plan asks for this
   * explicitly.  Skipped on abort, where nothing is being asked of the cloud,
   * and when there is too little to be speech.
   */

  {
    bool aborting;

    pthread_mutex_lock(&g_social.lock);
    aborting = g_social.abort;
    pthread_mutex_unlock(&g_social.lock);

    if (!aborting && filled >= SOCIAL_CHUNK_BYTES / 8u)
      {
        unsigned char *copy;
        bool copy_psram;
        size_t encoded = 0;
        uint32_t seq;
        uint32_t gen;

        pthread_mutex_lock(&g_social.lock);
        seq = ++g_social.audio_seq;
        gen = g_social.alert_gen;
        pthread_mutex_unlock(&g_social.lock);

        if (audio_test_ogg_encode(encoder, pcm, filled / sizeof(int16_t),
                                  seq, ogg, SOCIAL_OGG_BYTES,
                                  &encoded) == 0 && encoded != 0)
          {
            copy = social_alloc(encoded, &copy_psram);
            if (copy != NULL)
              {
                memcpy(copy, ogg, encoded);
                social_queue_push(VS_CLOUD_MEDIA_AUDIO, copy, encoded,
                                  copy_psram, seq, gen);
                printf("%s: tail chunk %lu, %zu ms\n", SOCIAL_TAG,
                       (unsigned long)seq,
                       filled / sizeof(int16_t) * 1000u / SOCIAL_AUDIO_RATE);
              }
          }
      }
  }

out:
  if (encoder != NULL)
    {
      audio_test_ogg_encoder_destroy(encoder);
    }

  social_free((unsigned char *)pcm, pcm_psram);
  social_free(ogg, ogg_psram);
  return NULL;
}

/****************************************************************************
 * The upload thread
 ****************************************************************************/

static void *social_upload_worker(void *arg)
{
  uint32_t backoff_ms = 0;
  unsigned int dead_strikes = 0;

  (void)arg;

  for (; ; )
    {
      struct social_slot_s slot;
      struct vs_cloud_media_packet_s packet;
      struct vs_cloud_upload_s result;
      uint64_t began_ms;
      uint64_t spent_ms;
      int ret;

      /* The backoff earned by the previous iteration's failure, paid here
       * rather than around the pop below.  Paying it before the pop means a
       * queue that has gone quiet (capture paused, session ending) is not
       * held up by it -- social_queue_pop() already blocks on the condvar in
       * that case, and there is nothing to slow down.  It only matters when
       * there is a next item ready immediately, which is exactly the case
       * this exists for.
       */

      if (backoff_ms != 0)
        {
          usleep(backoff_ms * 1000);
        }

      if (!social_queue_pop(&slot))
        {
          break;
        }

      memset(&packet, 0, sizeof(packet));
      packet.type     = slot.type;
      packet.data     = slot.data;
      packet.len      = slot.len;
      packet.sequence = slot.sequence;

      memset(&result, 0, sizeof(result));
      began_ms = social_now_ms();
      ret = vs_cloud_social_upload(g_social.session.session_id, &packet,
                                   &result);
      spent_ms = social_now_ms() - began_ms;

      /* Released as soon as the transfer is over, success or not.  Holding it
       * any longer would keep a 160 KB frame in PSRAM for no reason, and the
       * plan requires raw media not to linger.
       */

      social_free(slot.data, slot.from_psram);

      if (ret < 0)
        {
          pthread_mutex_lock(&g_social.lock);
          g_social.upload_failed++;
          pthread_mutex_unlock(&g_social.lock);

          /* -ESTALE means a later session replaced this one, so nothing here
            * is going to succeed again.  Everything else is per-item: one lost
            * frame does not end a conversation.
            */

          if (ret == -ESTALE)
            {
              printf("%s: session superseded, stopping uploads\n",
                     SOCIAL_TAG);
              pthread_mutex_lock(&g_social.lock);
              g_social.stop_capture = true;
              pthread_cond_broadcast(&g_social.cond);
              pthread_mutex_unlock(&g_social.lock);
              break;
            }

          /* -ENOTCONN is status 30: the cloud has no session for this id.
            * Unlike the failures below it does not get better by waiting, so
            * counting instead of backing off forever.  A strike limit rather
            * than an immediate stop because the document lists 30 as the
            * generic failure code, not a session-specific one, and one
            * transient 30 should not end a conversation the user is having.
            * Consecutive is the point: any success resets the count.
            */

          if (ret == -ENOTCONN)
            {
              if (++dead_strikes >= CONFIG_VS_SOCIAL_DEAD_SESSION_STRIKES)
                {
                  printf("%s: cloud has no session after %u refusals, "
                         "stopping uploads\n", SOCIAL_TAG, dead_strikes);
                  pthread_mutex_lock(&g_social.lock);
                  g_social.stop_capture = true;
                  pthread_cond_broadcast(&g_social.cond);
                  pthread_mutex_unlock(&g_social.lock);
                  break;
                }
            }

          /* Doubled from whatever it last was, starting from the floor on the
           * first failure after a success.  vs_cloud_social_upload() folds
           * every failure mode into one negative return -- a register call
           * refused outright, a presigned PUT the object store 403s on a
           * signature mismatch, a connect that times out, a malformed
           * response -- and this backoff does not need to tell them apart to
           * do its job: none of them are fixed by asking again immediately.
           *
           * Measured 2026-09-01: without this, a store that answers 403 on
           * every attempt (a real case, not hypothetical -- signature
           * mismatch is deterministic per request shape) turns this loop into
           * one bounded only by round-trip latency, at up to five uploads a
           * second.  That is not a transfer problem, since nothing is
           * transferring; it is priority-105 CPU time taken from every lower-
           * priority task including input polling, for as long as the queue
           * keeps handing this thread work.
           *
           * The floor stays close to the 340 ms image sampling interval so an
           * isolated failure -- one bad frame, one dropped packet -- costs
           * about one sample period, not a visible stall.  The ceiling is a
           * few seconds: long enough that a store stuck failing every request
           * spends most of its time asleep rather than retrying into a wall,
           * short enough that a real recovery is felt within a few attempts
           * rather than after however long capping at the floor would have
           * taken to notice on its own.
           */

          backoff_ms = backoff_ms == 0 ? CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MIN_MS
                                       : backoff_ms * 2;
          if (backoff_ms > CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MAX_MS)
            {
              backoff_ms = CONFIG_VS_SOCIAL_UPLOAD_BACKOFF_MAX_MS;
            }

          continue;
        }

      /* Back to no backoff the moment the cloud answers something other than
       * failure, so a transient stall does not keep taxing the next item once
       * whatever caused it has passed.  The strike count goes with it: what
       * ends a session is a run of refusals, not a total.
       */

      backoff_ms = 0;
      dead_strikes = 0;

      {
        bool report = false;
        uint32_t total = 0;
        uint32_t since = 0;
        uint64_t window = 0;
        uint8_t queued = 0;
        uint8_t inflight = 0;

        pthread_mutex_lock(&g_social.lock);
        g_social.uploaded++;
        g_social.upload_window_ms += spent_ms;
        social_inflight_add(result.msg_id, slot.alert_gen);

        /* Every SOCIAL_UPLOAD_REPORT_EVERY rather than every upload: at three
         * frames a second a line each would be most of the log.  What the line
         * carries is what the totals cannot -- the average time a transfer took
         * over this window, and how full the queue and the inflight table were
         * as it finished.  A rising average next to a rising drop count is the
         * signature of uploads being the bottleneck; a flat average next to
         * drops means the producers are simply faster than the link.
         */

        if (g_social.uploaded - g_social.upload_reported >=
            SOCIAL_UPLOAD_REPORT_EVERY)
          {
            report   = true;
            total    = g_social.uploaded;
            since    = g_social.uploaded - g_social.upload_reported;
            window   = g_social.upload_window_ms;
            queued   = g_social.count;
            inflight = g_social.inflight_count;

            g_social.upload_reported  = g_social.uploaded;
            g_social.upload_window_ms = 0;
          }

        pthread_mutex_unlock(&g_social.lock);

        if (report)
          {
            printf("%s: uploaded %lu, last %lu averaged %lu ms, "
                   "queue %u/%u, inflight %u\n", SOCIAL_TAG,
                   (unsigned long)total, (unsigned long)since,
                   (unsigned long)(since > 0 ? window / since : 0),
                   (unsigned)queued, (unsigned)CONFIG_VS_SOCIAL_QUEUE_SLOTS,
                   (unsigned)inflight);
          }
      }
    }

  return NULL;
}

/****************************************************************************
 * Emotion debounce
 ****************************************************************************/

/* Fold one image result into the alert state and say what the UI should be
 * told.  Called with the lock held.
 *
 * The two streaks are counted separately and each resets the other, so
 * "three extreme in a row" means exactly that rather than "three more extreme
 * than calm".  A single calm frame between two extreme ones restarts the
 * count, which is the conservative direction: raising an alert is the
 * interrupting action.
 */

static bool social_emotion_step(const struct vs_social_event_s *event,
                                bool *raise, bool *clear)
{
  *raise = false;
  *clear = false;

  if (event->extreme)
    {
      g_social.calm_streak = 0;
      if (g_social.extreme_streak < 255)
        {
          g_social.extreme_streak++;
        }

      if (!g_social.alert_active &&
          g_social.extreme_streak >= CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS)
        {
          g_social.alert_active   = true;
          g_social.alert_since_ms = social_now_ms();

          /* A new alert invalidates advice still in flight for the previous
           * one.  See the file header.
           */

          g_social.alert_gen++;
          *raise = true;
        }
      else if (g_social.alert_active)
        {
          /* Still extreme, and the emotion may have changed from 生气 to
           * 反感.  Refresh the text without bumping the generation: it is the
           * same alert, so advice already on its way still applies.
           */

          *raise = true;
        }

      return true;
    }

  g_social.extreme_streak = 0;
  if (g_social.calm_streak < 255)
    {
      g_social.calm_streak++;
    }

  if (g_social.alert_active &&
      g_social.calm_streak >= CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS &&
      social_now_ms() - g_social.alert_since_ms >=
        CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS)
    {
      g_social.alert_active = false;
      g_social.alert_gen++;
      *clear = true;
    }

  return true;
}

/****************************************************************************
 * Name: social_poll_once
 *
 * Description:
 *   One getResult for everything in flight, and the events that follow from
 *   it.  Runs on the session thread.
 *
 ****************************************************************************/

static void social_poll_once(void)
{
  struct social_poll_scratch_s *scratch = g_social.poll;
  size_t n = 0;
  size_t got = 0;
  size_t i;
  int ret;

  /* Read once, without the lock, and only here.  The pointer is written by
   * social_session_worker() before this thread reaches its polling loop and
   * cleared after that loop has ended, so on this thread it cannot change
   * across a call.
   */

  if (scratch == NULL)
    {
      return;
    }

  /* Snapshot the identifiers under the lock, then release it: the GET blocks
   * for a round trip and the uploader has to keep making progress.
   */

  pthread_mutex_lock(&g_social.lock);
  for (i = 0; i < g_social.inflight_count && n < VS_CLOUD_POLL_MAX_IDS; i++)
    {
      snprintf(scratch->id_store[n], sizeof(scratch->id_store[n]), "%s",
               g_social.inflight[i].msg_id);
      scratch->ids[n] = scratch->id_store[n];
      n++;
    }
  pthread_mutex_unlock(&g_social.lock);

  if (n == 0)
    {
      return;
    }

  ret = vs_cloud_social_poll_event(g_social.session.session_id,
                                   scratch->ids, n, scratch->events,
                                   sizeof(scratch->events) /
                                   sizeof(scratch->events[0]), &got);
  if (ret < 0)
    {
      /* Transport failures here are not fatal to the session: the data is
       * uploaded, the cloud still has it, and the end-of-session minutes are
       * built from the cloud's own timeline rather than from what these polls
       * managed to collect.  Worth one line, not worth stopping.
       */

      printf("%s: poll failed: %d\n", SOCIAL_TAG, ret);
      return;
    }

  for (i = 0; i < got; i++)
    {
      struct vs_social_event_s *ev = &scratch->events[i];
      bool raise = false;
      bool clear = false;
      bool deliver_advice = false;
      uint32_t gen = 0;
      uint32_t color = 0;
      enum vs_emotion_e emotion = VS_EMOTION_NONE;
      char text[VS_TEXT_LONG];

      text[0] = '\0';

      pthread_mutex_lock(&g_social.lock);

      /* Find the entry and mark what has been seen, so an id is only retired
       * once every event it can produce has arrived or failed.
       */

      {
        uint8_t index;
        bool matched = false;

        for (index = 0; index < g_social.inflight_count; index++)
          {
            if (strcmp(g_social.inflight[index].msg_id, ev->msg_id) == 0)
              {
                matched = true;
                break;
              }
          }

        if (!matched)
          {
            pthread_mutex_unlock(&g_social.lock);
            continue;
          }

        gen = g_social.inflight[index].alert_gen;

        switch (ev->peer_state)
          {
            case VS_CLOUD_PEER_EMOTION_DONE:
              if (ev->msg_event == VS_CLOUD_MSG_EVENT_IMAGE)
                {
                  g_social.inflight[index].image_seen = true;
                  (void)social_emotion_step(ev, &raise, &clear);
                  emotion = ev->emotion;
                  color   = ev->color;
                  snprintf(text, sizeof(text), "%s", ev->display_text);

                  /* An image that did not raise an alert produced an
                   * emotionTimeline entry on the cloud and nothing on screen.
                   * That is the design: the screen is for the moments worth
                   * interrupting, the timeline is for the record.
                   *
                   * Retire it now.  A calm frame never grows an audio entry --
                   * the cloud only starts the advice chain for a frame it
                   * judged extreme -- so waiting for one would hold the slot
                   * until it aged out.
                   */

                  if (!ev->extreme)
                    {
                      social_inflight_remove(index);
                    }
                }
              break;

            case VS_CLOUD_PEER_ADVICE_DONE:
              if (ev->msg_event == VS_CLOUD_MSG_EVENT_AUDIO)
                {
                  g_social.inflight[index].audio_seen = true;

                  /* The generation test.  Advice for an alert that has since
                   * cleared would contradict what is on screen, so it is
                   * dropped rather than shown late.
                   */

                  if (g_social.alert_active && gen == g_social.alert_gen - 1)
                    {
                      deliver_advice = true;
                      snprintf(text, sizeof(text), "%s", ev->suggestion);
                      emotion = VS_EMOTION_TENSE;
                    }

                  social_inflight_remove(index);
                }
              break;

            case VS_CLOUD_PEER_FAILED:
              /* 30 collapses four server-side reasons, the common one being
               * "no usable face in the frame".  At three frames a second that
               * is ordinary rather than exceptional, so it is retired quietly.
               */

              social_inflight_remove(index);
              break;

            default:
              /* 10, 11 and 40 all mean "still working".  Leave it in flight. */
              break;
          }
      }

      pthread_mutex_unlock(&g_social.lock);

      if (raise)
        {
          social_post(VS_APP_EVENT_SOCIAL_ALERT, 0, emotion, color, text);
        }
      else if (clear)
        {
          social_post(VS_APP_EVENT_SOCIAL_ALERT_CLEARED, 0, VS_EMOTION_NONE,
                      0, NULL);
        }

      if (deliver_advice && text[0] != '\0')
        {
          /* Still an ALERT rather than a new event type: the page is already
           * showing the alert, and this replaces its text with the advice the
           * cloud derived from the surrounding audio.
           */

          social_post(VS_APP_EVENT_SOCIAL_ALERT, 0, emotion, color, text);
        }
    }
}

/****************************************************************************
 * Teardown helpers
 ****************************************************************************/

/* Stop the producers and join them.  Called on the session thread. */

static void social_stop_producers(void)
{
  pthread_mutex_lock(&g_social.lock);
  g_social.stop_capture = true;
  g_social.paused       = false;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  /* Shorten the poll the capture thread may be sitting in, and stop the
   * microphone so the audio thread's read loop terminates instead of waiting
   * for a chunk that will never fill.
   *
   * capture_stop() rather than abort(): stop leaves what is already staged
   * readable, which is the tail of the conversation.
   */

  vs_media_stream_wake(g_social.camera, true);
  vs_audio_capture_stop(g_social.mic);

  if (g_social.capture_joinable)
    {
      pthread_join(g_social.capture_thread, NULL);
      g_social.capture_joinable = false;
    }

  if (g_social.audio_joinable)
    {
      pthread_join(g_social.audio_thread, NULL);
      g_social.audio_joinable = false;
    }

  /* Only now can the uploader be told no more work is coming.  Setting this
   * any earlier -- with stop_capture, which is the obvious place -- would let
   * it leave before the audio worker had pushed the tail chunk, and those last
   * two seconds would be freed unsent.  The uploader drains whatever is in the
   * ring before it acts on this.
   */

  pthread_mutex_lock(&g_social.lock);
  g_social.producers_done = true;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  if (g_social.upload_joinable)
    {
      pthread_join(g_social.upload_thread, NULL);
      g_social.upload_joinable = false;
    }
}

/* Close both devices.  Runs on the session thread.
 *
 * The handles are detached under the lock before anything is closed, because
 * vs_social_abort() may be reaching for them from the UI thread at the same
 * moment.  Clearing them first means that call sees NULL rather than a pointer
 * this function is in the middle of freeing; closing outside the lock keeps a
 * STREAMOFF and an ADC teardown off it.
 *
 * Idempotent: called on the normal path and again from the shared cleanup
 * tail, and the second call finds nothing to do.
 */

static void social_release_devices(void)
{
  struct vs_media_stream_s *camera;
  struct vs_audio_cap_s *mic;
  struct social_poll_scratch_s *poll;
  bool poll_psram;

  pthread_mutex_lock(&g_social.lock);
  camera = g_social.camera;
  mic    = g_social.mic;
  poll   = g_social.poll;
  poll_psram = g_social.poll_psram;
  g_social.camera = NULL;
  g_social.mic    = NULL;

  /* Cleared under the lock and before the free, so a poll that has already
   * loaded the pointer cannot be joined by one that loads it after the memory
   * is gone.  Only the session thread calls social_poll_once(), and it is the
   * thread running this, so in practice there is no such race -- this keeps
   * that from being a requirement of the code rather than of the comment.
   */

  g_social.poll = NULL;
  pthread_mutex_unlock(&g_social.lock);

  if (poll != NULL)
    {
      social_free((unsigned char *)poll, poll_psram);
    }

  if (camera != NULL)
    {
      vs_media_stream_close(camera);
    }

  if (mic != NULL)
    {
      vs_audio_capture_close(mic);
    }
}

static void social_log_totals(void)
{
  uint32_t delivered = 0;
  uint32_t malformed = 0;

  vs_media_stream_stats(g_social.camera, &delivered, &malformed);

  printf("%s: session %s totals: %lu frames (%lu malformed), %lu chunks, "
         "%lu uploaded, %lu upload failures, %lu dropped, %lu unanswered\n",
         SOCIAL_TAG, g_social.session.session_id,
         (unsigned long)g_social.image_seq, (unsigned long)malformed,
         (unsigned long)g_social.audio_seq, (unsigned long)g_social.uploaded,
         (unsigned long)g_social.upload_failed,
         (unsigned long)g_social.dropped,
         (unsigned long)g_social.inflight_retired);
}

/****************************************************************************
 * Name: social_persist_minutes
 *
 * Description:
 *   Write the end-of-session record.  Runs before SOCIAL_RESULT is posted, so
 *   a summary on screen is a summary on the card.
 *
 * Returned Value:
 *   0 on success, or a negative errno.  A failure here is reported to the UI:
 *   telling the user their conversation was saved when it was not is worse
 *   than telling them it was not.
 *
 ****************************************************************************/

static int social_persist_minutes(const struct vs_cloud_minutes_s *minutes,
                                  const char *full_json)
{
  struct vs_history_index_s index;
  time_t now;
  struct tm tm;

  memset(&index, 0, sizeof(index));
  index.kind  = VS_HISTORY_KIND_SOCIAL;
  index.calm  = minutes->calm;
  index.happy = minutes->happy;
  index.tense = minutes->tense;

  /* The clock may never have been set: this board has no RTC, and SNTP is not
   * on the social path.  A wrong date is still more useful than none -- it
   * orders the records correctly within a boot -- so it is written rather than
   * left blank, and the year makes it obvious when it is not real.
   */

  now = time(NULL);
  if (localtime_r(&now, &tm) != NULL)
    {
      snprintf(index.date, sizeof(index.date), "%04d-%02d-%02d %02d:%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
               tm.tm_min);
    }

  snprintf(index.title, sizeof(index.title), "面对面交流");
  snprintf(index.summary, sizeof(index.summary), "%s",
           minutes->summary[0] != '\0' ? minutes->summary : "本次没有生成摘要");

  /* The record is complete only if the cloud gave both a summary and a
   * timeline to build the percentages from.  Marking it otherwise lets the
   * history page show it as partial instead of as a session that was calm 0%,
   * happy 0%, tense 0%.
   */

  index.incomplete = minutes->summary[0] == '\0' ||
                     minutes->emotion_samples == 0;

  return vs_history_append(VS_HISTORY_KIND_SOCIAL, &index, full_json);
}

/****************************************************************************
 * Name: social_wav_le16 / social_wav_le32
 *
 * Description:
 *   Read a little-endian field out of a RIFF header without assuming the
 *   host's byte order or that the field is aligned.  Both hold on this chip,
 *   and neither is worth depending on for eight lines of code.
 *
 ****************************************************************************/

static uint16_t social_wav_le16(const unsigned char *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t social_wav_le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/****************************************************************************
 * Name: social_read_exact
 *
 * Description:
 *   read() that either fills the buffer or says why it could not.  Short
 *   reads are normal on a file and a header parser that treated one as an
 *   error would fail on a perfectly good WAV.
 *
 ****************************************************************************/

static int social_read_exact(int fd, void *buf, size_t len)
{
  unsigned char *p = buf;
  size_t off = 0;

  while (off < len)
    {
      ssize_t n = read(fd, p + off, len - off);

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
          return -ENODATA;
        }

      off += (size_t)n;
    }

  return 0;
}

/****************************************************************************
 * Name: social_minutes_dir
 *
 * Description:
 *   Make sure the directory holding the spoken minutes exists.
 *
 *   vs_history_open() already creates /mnt/sdnand/ai_agent at startup, so on
 *   a healthy boot this finds it there.  It is done again here because the
 *   path is a configuration value that may point elsewhere, and because a
 *   failed history init should not silently take the audio down with it.
 *
 ****************************************************************************/

static void social_minutes_dir(const char *path)
{
  char dir[128];
  const char *slash = strrchr(path, '/');
  size_t len;

  if (slash == NULL || slash == path)
    {
      return;
    }

  len = (size_t)(slash - path);
  if (len >= sizeof(dir))
    {
      return;
    }

  memcpy(dir, path, len);
  dir[len] = '\0';

  /* Errors are not reported: EEXIST is the expected case, and any other
   * failure shows up immediately as the open() below failing with a message
   * that names the actual path.
   */

  (void)mkdir(dir, 0700);
}

/****************************************************************************
 * Name: social_wav_open
 *
 * Description:
 *   Open a WAV file and leave it positioned at the first PCM sample.
 *
 *   Walks the chunk list rather than assuming the canonical 44-byte header: a
 *   writer is free to put LIST or fact ahead of data, and one that does would
 *   otherwise have its metadata played as audio.
 *
 *   Reading the format from the bytes rather than from Content-Type is not
 *   fastidiousness -- FDS answers application/octet-stream for everything, so
 *   the header is the only description of the file there is.  Measured
 *   2026-09-04: RIFF/WAVE, PCM, 24 kHz, 16-bit, mono.
 *
 * Input Parameters:
 *   file_len - the file's size, used to clamp a data chunk that claims more
 *              than arrived.  A writer's size field and the bytes on disk are
 *              independent claims and only the smaller one is safe to read.
 *
 * Returned Value:
 *   0 with *fd_out open and seeked to the data, or a negative errno with
 *   nothing left open.
 *
 ****************************************************************************/

static int social_wav_open(const char *path, size_t file_len, int *fd_out,
                          unsigned int *rate, unsigned int *channels,
                          unsigned int *bits, size_t *data_len)
{
  unsigned char head[16];
  bool have_fmt = false;
  int guard;
  int fd;
  int ret;

  *rate = *channels = *bits = 0;
  *data_len = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  ret = social_read_exact(fd, head, 12);
  if (ret < 0)
    {
      goto err;
    }

  if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0)
    {
      printf("%s: spoken minutes are not RIFF/WAVE\n", SOCIAL_TAG);
      ret = -EINVAL;
      goto err;
    }

  /* Bounded rather than "until the data chunk".  A file whose size fields are
   * garbage -- an HTML error page saved with a .wav name, a truncated
   * download -- would otherwise be walked forever.
   */

  for (guard = 0; guard < 64; guard++)
    {
      uint32_t size;
      size_t skip;

      ret = social_read_exact(fd, head, 8);
      if (ret < 0)
        {
          /* Ran off the end without finding data. */

          ret = -EINVAL;
          goto err;
        }

      size = social_wav_le32(head + 4);
      skip = (size_t)size + (size & 1u); /* chunks are word aligned */

      if (memcmp(head, "data", 4) == 0)
        {
          off_t here;
          size_t avail;

          if (!have_fmt)
            {
              printf("%s: spoken minutes have data before fmt\n", SOCIAL_TAG);
              ret = -EINVAL;
              goto err;
            }

          /* Ask the descriptor where it is rather than tracking it alongside.
           * A parallel counter has to be adjusted at every read and every
           * seek, and the one place it was missed -- the sixteen fmt bytes
           * read below -- made this overstate what the file held.
           */

          here = lseek(fd, 0, SEEK_CUR);
          if (here < 0)
            {
              ret = -errno;
              goto err;
            }

          /* Trust the smaller of what the writer declared and what is
           * actually there.  They are independent claims, and reading past
           * the end of a truncated download would play whatever follows.
           */

          avail = file_len > (size_t)here ? file_len - (size_t)here : 0;
          *data_len = (size_t)size < avail ? (size_t)size : avail;

          if (*data_len == 0)
            {
              ret = -ENODATA;
              goto err;
            }

          *fd_out = fd;
          return 0;
        }

      if (memcmp(head, "fmt ", 4) == 0 && size >= 16)
        {
          unsigned char fmt[16];

          ret = social_read_exact(fd, fmt, sizeof(fmt));
          if (ret < 0)
            {
              goto err;
            }

          if (social_wav_le16(fmt) != 1)
            {
              printf("%s: spoken minutes are compressed, format %u\n",
                     SOCIAL_TAG, social_wav_le16(fmt));
              ret = -ENOTSUP;
              goto err;
            }

          *channels = social_wav_le16(fmt + 2);
          *rate     = social_wav_le32(fmt + 4);
          *bits     = social_wav_le16(fmt + 14);
          have_fmt  = true;
          skip -= sizeof(fmt);
        }

      if (skip > 0 && lseek(fd, (off_t)skip, SEEK_CUR) < 0)
        {
          ret = -errno;
          goto err;
        }
    }

  ret = -EINVAL;

err:
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: social_play_minutes_audio
 *
 * Description:
 *   Fetch ttsMinutes to storage and speak it from there.
 *
 *   This is the step the interface document describes and the firmware never
 *   took: the download existed with no caller, so tts_url was logged and
 *   dropped.  The AI端 confirmed it is the only way to get the voice --
 *   "你要语音是必须要走url下载的" -- and it is a separate file from the text,
 *   not a spoken copy of it.
 *
 *   Through storage rather than through memory, and that is the whole design.
 *   The spoken minutes are not a bounded object: measured 2026-09-04 the
 *   shortest one this cloud can produce -- a session that detected no emotion
 *   at all -- was already 585 KB, 12.5 seconds at 48000 bytes a second.
 *   Buffering that put a ceiling on session length in the shape of a heap
 *   allocation, and the 768 KB it wanted bought sixteen seconds of speech.
 *   Streaming to a file costs one 8 KB buffer whatever the length, so what is
 *   left is a limit on the filesystem, which is a place a limit can live.
 *
 *   Everything here is best effort and reported rather than propagated.  By
 *   the time this runs the summary is on screen and the record is on disk;
 *   failing the session because the speaker could not be fed would throw away
 *   the part the user actually asked for.
 *
 *   The file is left in place afterwards.  It is one fixed path, so it cannot
 *   accumulate, and having the last session's audio on disk is what makes a
 *   complaint about the voice something that can be investigated rather than
 *   reproduced.
 *
 ****************************************************************************/

static void social_play_minutes_audio(const char *url)
{
  const char *path = CONFIG_VS_SOCIAL_MINUTES_AUDIO_PATH;
  struct vs_audio_pb_s *pb = NULL;
  unsigned char *buf = NULL;
  bool buf_psram = false;
  size_t file_len = 0;
  size_t data_len = 0;
  size_t done = 0;
  unsigned int rate = 0;
  unsigned int channels = 0;
  unsigned int bits = 0;
  int fd = -1;
  int ret;

  if (url == NULL || url[0] == '\0')
    {
      return;
    }

  social_minutes_dir(path);

  ret = vs_cloud_download_to_file(url, path,
                                  CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES,
                                  &file_len);
  if (ret < 0)
    {
      /* -EFBIG is the interesting one: it means the spoken minutes outgrew
       * CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES, which is a number to raise
       * rather than a fault to chase.  Say so specifically.
       */

      printf("%s: spoken minutes not fetched: %d%s\n", SOCIAL_TAG, ret,
             ret == -EFBIG ? " (raise VS_SOCIAL_DOWNLOAD_MAX_BYTES)" : "");
      return;
    }

  printf("%s: spoken minutes saved to %s, %zu bytes\n", SOCIAL_TAG, path,
         file_len);

  ret = social_wav_open(path, file_len, &fd, &rate, &channels, &bits,
                        &data_len);
  if (ret < 0)
    {
      printf("%s: spoken minutes unplayable: %d\n", SOCIAL_TAG, ret);
      return;
    }

  if (bits != 16)
    {
      printf("%s: spoken minutes are %u-bit, only 16 is supported\n",
             SOCIAL_TAG, bits);
      goto out;
    }

  printf("%s: spoken minutes %zu bytes, %u Hz %u ch %u-bit, %.1f s\n",
         SOCIAL_TAG, data_len, rate, channels, bits,
         rate * channels > 0 ?
           (double)data_len / (double)(rate * channels * 2u) : 0.0);

  /* From PSRAM, not the stack.  CONFIG_VS_SOCIAL_STACKSIZE_SESSION is 16 KB
   * and this thread has already been through a TLS handshake and a JSON parse
   * on it.
   */

  buf = social_alloc(SOCIAL_PLAY_CHUNK, &buf_psram);
  if (buf == NULL)
    {
      printf("%s: spoken minutes: no buffer\n", SOCIAL_TAG);
      goto out;
    }

  pb = vs_audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV, rate, channels, bits);
  if (pb == NULL)
    {
      printf("%s: spoken minutes: playback open failed\n", SOCIAL_TAG);
      goto out;
    }

  /* A chunk at a time rather than the whole file, which is now the only option
   * anyway: an abandon while speaking is acted on within a chunk instead of
   * after the file, and the ring absorbs the writes without pacing, so the
   * size only bounds the reaction time.
   *
   * Reading from storage in the same loop that feeds the DAC is what makes an
   * underrun conceivable here, so the count is reported below.  At 48000 bytes
   * a second a chunk lasts 170 ms, which is a long time for an 8 KB read from
   * SD-NAND to take.
   */

  while (done < data_len)
    {
      size_t want = data_len - done;
      bool aborting;
      ssize_t n;

      if (want > SOCIAL_PLAY_CHUNK)
        {
          want = SOCIAL_PLAY_CHUNK;
        }

      pthread_mutex_lock(&g_social.lock);
      aborting = g_social.abort;
      pthread_mutex_unlock(&g_social.lock);

      if (aborting)
        {
          vs_audio_playback_stop(pb);
          break;
        }

      n = read(fd, buf, want);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          printf("%s: spoken minutes read failed: %d\n", SOCIAL_TAG, errno);
          break;
        }

      if (n == 0)
        {
          /* Shorter than the header claimed even after clamping to the file
           * size.  Nothing to do about it but stop; what played, played.
           */

          break;
        }

      ret = vs_audio_playback_write(pb, buf, (size_t)n);
      if (ret < 0)
        {
          if (ret != -ECANCELED)
            {
              printf("%s: spoken minutes write failed: %d\n", SOCIAL_TAG,
                     ret);
            }

          break;
        }

      done += (size_t)n;
    }

  vs_audio_playback_drain(pb);

  if (vs_audio_playback_underruns(pb) > 0)
    {
      printf("%s: spoken minutes underran %u times\n", SOCIAL_TAG,
             vs_audio_playback_underruns(pb));
    }

  vs_audio_playback_close(pb);

out:
  social_free(buf, buf_psram);

  if (fd >= 0)
    {
      close(fd);
    }
}

/****************************************************************************
 * Name: social_finalize_sequence
 *
 * Description:
 *   Everything after the user asks to stop: drain, close, poll for minutes,
 *   persist, report.  Runs on the session thread.
 *
 ****************************************************************************/

static void social_finalize_sequence(void)
{
  struct vs_cloud_minutes_s minutes;
  char msg_id[VS_CLOUD_MSG_ID_MAX];
  char *body = NULL;
  bool body_psram = false;
  uint64_t deadline;
  int ret;

  social_stop_producers();

  ret = vs_cloud_social_finalize(g_social.session.session_id, msg_id,
                                 sizeof(msg_id));
  if (ret < 0)
    {
      printf("%s: finalize failed: %d\n", SOCIAL_TAG, ret);
      social_post(VS_APP_EVENT_SOCIAL_FINALIZE_FAILED, ret, VS_EMOTION_NONE,
                  0, NULL);
      return;
    }

  body = (char *)social_alloc(CONFIG_VS_SOCIAL_RESP_MAX_BYTES, &body_psram);
  if (body == NULL)
    {
      social_post(VS_APP_EVENT_SOCIAL_FINALIZE_FAILED, -ENOMEM,
                  VS_EMOTION_NONE, 0, NULL);
      return;
    }

  /* Poll rather than one long call.  vs_cloud_social_get_result() is
   * deliberately single-shot so this loop stays responsive; a call that
   * blocked for the whole timeout could not be interrupted, and the user may
   * press back again.
   */

  deadline = social_now_ms() + CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS;

  for (; ; )
    {
      bool aborting;

      ret = vs_cloud_social_get_result(g_social.session.session_id, msg_id,
                                       &minutes, body,
                                       CONFIG_VS_SOCIAL_RESP_MAX_BYTES);
      if (ret != -EAGAIN)
        {
          break;
        }

      pthread_mutex_lock(&g_social.lock);
      aborting = g_social.abort;
      pthread_mutex_unlock(&g_social.lock);

      if (aborting)
        {
          social_free((unsigned char *)body, body_psram);
          return;
        }

      if (social_now_ms() >= deadline)
        {
          printf("%s: minutes did not arrive within %d ms\n", SOCIAL_TAG,
                 CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS);
          ret = -ETIMEDOUT;
          break;
        }

      usleep(CONFIG_VS_SOCIAL_POLL_INTERVAL_MS * 1000);
    }

  if (ret < 0)
    {
      social_free((unsigned char *)body, body_psram);
      social_post(VS_APP_EVENT_SOCIAL_FINALIZE_FAILED, ret, VS_EMOTION_NONE,
                  0, NULL);
      return;
    }

  printf("%s: minutes: calm %u happy %u tense %u, %u emotion / %u audio "
         "samples, text %s, tts %s\n", SOCIAL_TAG, minutes.calm,
         minutes.happy, minutes.tense, minutes.emotion_samples,
         minutes.audio_samples,
         minutes.txt_url[0] != '\0' ? "downloaded" : "inline",
         minutes.tts_url[0] != '\0' ? "available" : "(none)");

  ret = social_persist_minutes(&minutes, body);
  social_free((unsigned char *)body, body_psram);

  if (ret < 0)
    {
      printf("%s: minutes not persisted: %d\n", SOCIAL_TAG, ret);
      social_post(VS_APP_EVENT_SOCIAL_FINALIZE_FAILED, ret, VS_EMOTION_NONE,
                  0, NULL);
      return;
    }

  /* The summary text, truncated to what the result page can hold.  The full
   * text is in the record that was just written, which is what the Web history
   * page serves.
   */

  social_post(VS_APP_EVENT_SOCIAL_RESULT, 0, VS_EMOTION_NONE, 0,
              minutes.summary[0] != '\0' ? minutes.summary :
                                           "本次没有生成摘要");

  (void)vs_cloud_social_ack(g_social.session.session_id);

  /* Speak the minutes last, deliberately.
   *
   * The order is what the UI discussion asked for -- show the text, then talk
   * over it -- and it is also the safe order: the record is written and the
   * result page is up before a 585 KB fetch and a DAC open are attempted, so
   * neither can cost the user the summary.
   *
   * It has to happen before the caller releases the devices, and it has to
   * happen inside this session's lifetime: the presigned URL is good for one
   * hour, so there is no later.
   */

  social_play_minutes_audio(minutes.tts_url);
}

/****************************************************************************
 * The session thread
 ****************************************************************************/

static int social_spawn(pthread_t *thread, void *(*entry)(void *),
                        size_t stacksize, const char *what)
{
  pthread_attr_t attr;
  struct sched_param param;
  int ret;

  pthread_attr_init(&attr);

  /* Per thread, because one shared size cannot fit all four: the audio thread
   * runs libopus with its working arrays on the stack, while the capture thread
   * measures a 24-byte frame.  A single value big enough for the first is four
   * times what the others will ever touch, and the value that used to be here
   * was not big enough for either.
   */

  pthread_attr_setstacksize(&attr, stacksize);

  /* Below the UI.  The comment here used to say "above the UI ... must not be
   * held off by a redraw", which had it backwards: with four of these threads
   * doing continuous TLS at VS_PRIORITY_VOICE, it was the redraw that never
   * happened.  See VS_PRIORITY_SOCIAL in vs_types.h for the measurement.
   */

  param.sched_priority = VS_PRIORITY_SOCIAL;
  pthread_attr_setschedparam(&attr, &param);

  ret = pthread_create(thread, &attr, entry, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      printf("%s: cannot start %s thread: %d\n", SOCIAL_TAG, what, ret);
      return -ret;
    }

  return 0;
}

static void *social_session_worker(void *arg)
{
  bool finalize;
  bool aborting;
  int ret;

  (void)arg;

  memset(&g_social.session, 0, sizeof(g_social.session));

  ret = vs_cloud_social_open(&g_social.session);
  if (ret < 0)
    {
      const char *why;

      switch (ret)
        {
          case -EBUSY:
            why = "云端仍有未结束的会话";
            break;
          case -ENODATA:
            why = "还没有配置社交云地址";
            break;
          default:
            why = NULL;
            break;
        }

      printf("%s: open failed: %d\n", SOCIAL_TAG, ret);
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, ret, VS_EMOTION_NONE, 0,
                  why);
      goto done;
    }

  /* Devices next.  Camera first, because it is the one that can be owned by
   * something else and is therefore the likelier failure.
   */

  /* The polling scratch, before the devices, because it is the cheapest thing
   * to fail and the only one whose failure means the session could not report
   * anything even if every device worked.
   */

  g_social.poll = (struct social_poll_scratch_s *)
    social_alloc(sizeof(*g_social.poll), &g_social.poll_psram);
  if (g_social.poll == NULL)
    {
      printf("%s: no memory for the %zu-byte poll scratch\n", SOCIAL_TAG,
             sizeof(*g_social.poll));
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, -ENOMEM, VS_EMOTION_NONE,
                  0, "系统资源不足");
      goto close_session;
    }

  ret = vs_media_stream_open(&g_social.camera, CONFIG_VS_SOCIAL_IMAGE_WIDTH,
                             CONFIG_VS_SOCIAL_IMAGE_HEIGHT,
                             CONFIG_VS_SOCIAL_CAPTURE_FPS);
  if (ret < 0)
    {
      printf("%s: camera unavailable: %d\n", SOCIAL_TAG, ret);
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, ret, VS_EMOTION_NONE, 0,
                  ret == -EBUSY ? "相机正被其他功能占用" : "相机打不开");
      goto close_session;
    }

  g_social.mic = vs_audio_capture_open(AGENT_AUDIO_CAPTURE_DEV,
                                       SOCIAL_AUDIO_RATE,
                                       SOCIAL_AUDIO_CHANNELS,
                                       SOCIAL_AUDIO_BITS);
  if (g_social.mic == NULL || vs_audio_capture_start(g_social.mic) < 0)
    {
      /* The session continues without audio rather than failing.  The emotion
       * timeline comes from the camera, and a conversation with expressions
       * and no advice is a degraded session; refusing to start would make it
       * no session at all.
       */

      printf("%s: microphone unavailable, continuing without audio\n",
             SOCIAL_TAG);
      vs_audio_capture_close(g_social.mic);
      g_social.mic = NULL;
    }

  if (social_spawn(&g_social.upload_thread, social_upload_worker,
                   CONFIG_VS_SOCIAL_STACKSIZE_UPLOAD, "upload") == 0)
    {
      g_social.upload_joinable = true;
    }
  else
    {
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, -EAGAIN, VS_EMOTION_NONE,
                  0, "系统资源不足");
      goto release;
    }

  if (social_spawn(&g_social.capture_thread, social_capture_worker,
                   CONFIG_VS_SOCIAL_STACKSIZE_CAPTURE, "capture") == 0)
    {
      g_social.capture_joinable = true;
    }

  if (g_social.mic != NULL &&
      social_spawn(&g_social.audio_thread, social_audio_worker,
                   CONFIG_VS_SOCIAL_STACKSIZE_AUDIO, "audio") == 0)
    {
      g_social.audio_joinable = true;
    }

  if (!g_social.capture_joinable)
    {
      /* No camera thread means no emotion results at all, which is the whole
       * feature.  Unwind rather than run an empty session.
       */

      pthread_mutex_lock(&g_social.lock);
      g_social.stop_capture = true;
      pthread_cond_broadcast(&g_social.cond);
      pthread_mutex_unlock(&g_social.lock);
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, -EAGAIN, VS_EMOTION_NONE,
                  0, "系统资源不足");
      goto stop;
    }

  /* Only now is the session actually sampling.  See vs_social.h for why the
   * event waits until here.
   */

  social_post(VS_APP_EVENT_SOCIAL_STARTED, 0, VS_EMOTION_NONE, 0, NULL);

  for (; ; )
    {
      pthread_mutex_lock(&g_social.lock);
      finalize = g_social.finalize;
      aborting = g_social.abort;
      pthread_mutex_unlock(&g_social.lock);

      if (finalize || aborting)
        {
          break;
        }

      social_poll_once();
      usleep(CONFIG_VS_SOCIAL_POLL_INTERVAL_MS * 1000);
    }

  if (aborting)
    {
      goto stop;
    }

  /* finalize_sequence() first, then the totals.  It joins the producers on the
   * way in, which matters twice: the tail audio chunk is counted, and the
   * counters are read after the only threads that write them have stopped
   * rather than while they are still incrementing.
   *
   * The camera is still open at this point -- finalize_sequence() does not
   * release devices -- which is what lets the frame statistics be read.
   */

  social_finalize_sequence();
  social_log_totals();
  social_release_devices();
  goto done;

stop:
  social_stop_producers();
  social_log_totals();

release:
  social_queue_flush();
  social_release_devices();

close_session:
  /* Close the cloud session even when abandoning it.  One deviceId may hold
   * one live session, so walking away would make the next attempt fail with
   * -EBUSY and report a problem unrelated to what actually happened.
   */

  {
    char abandon[VS_CLOUD_MSG_ID_MAX];

    if (vs_cloud_social_finalize(g_social.session.session_id, abandon,
                                 sizeof(abandon)) < 0)
      {
        printf("%s: could not close abandoned session %s\n", SOCIAL_TAG,
               g_social.session.session_id);
      }
  }

done:
  social_queue_flush();
  social_release_devices();

  pthread_mutex_lock(&g_social.lock);
  g_social.running = false;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_social_start(uint32_t request_id)
{
  int ret;

  pthread_mutex_lock(&g_social.lock);

  if (g_social.running)
    {
      pthread_mutex_unlock(&g_social.lock);
      return -EBUSY;
    }

  if (!vs_cloud_configured())
    {
      pthread_mutex_unlock(&g_social.lock);
      printf("%s: no cloud endpoint, refusing to start\n", SOCIAL_TAG);
      return -ENODATA;
    }

  /* A previous session's thread may still be unwinding after posting its
   * terminal event.  Join it here rather than detaching, so the state below is
   * reset with nothing else looking at it.
   */

  if (g_social.session_joinable)
    {
      pthread_t previous = g_social.session_thread;

      g_social.session_joinable = false;
      pthread_mutex_unlock(&g_social.lock);
      pthread_join(previous, NULL);
      pthread_mutex_lock(&g_social.lock);
    }

  /* Everything except the mutex and the condition variable. */

  g_social.paused         = false;
  g_social.stop_capture   = false;
  g_social.producers_done = false;
  g_social.finalize       = false;
  g_social.abort          = false;
  g_social.request_id     = request_id;
  g_social.read           = 0;
  g_social.write          = 0;
  g_social.count          = 0;
  g_social.image_seq      = 0;
  g_social.audio_seq      = 0;
  g_social.dropped        = 0;
  g_social.uploaded       = 0;
  g_social.upload_failed  = 0;

  /* Cleared with the counters they throttle.  Carried over, drop_report_ms
   * would suppress the first drop of a new session for as long as the gap
   * between sessions was short, and the two "reported" marks would make the
   * first line of the new session report a delta against the old one.
   */

  g_social.drop_reported    = 0;
  g_social.drop_report_ms   = 0;
  g_social.upload_reported  = 0;
  g_social.upload_window_ms = 0;
  g_social.inflight_count = 0;
  g_social.inflight_retired = 0;
  g_social.alert_gen      = 1;
  g_social.extreme_streak = 0;
  g_social.calm_streak    = 0;
  g_social.alert_active   = false;
  g_social.alert_since_ms = 0;
  g_social.camera         = NULL;
  g_social.mic            = NULL;
  memset(g_social.slot, 0, sizeof(g_social.slot));
  memset(g_social.inflight, 0, sizeof(g_social.inflight));

  g_social.running = true;
  pthread_mutex_unlock(&g_social.lock);

  ret = social_spawn(&g_social.session_thread, social_session_worker,
                     CONFIG_VS_SOCIAL_STACKSIZE_SESSION, "session");
  if (ret < 0)
    {
      pthread_mutex_lock(&g_social.lock);
      g_social.running = false;
      pthread_mutex_unlock(&g_social.lock);
      return -EAGAIN;
    }

  g_social.session_joinable = true;
  return 0;
}

int vs_social_pause(void)
{
  pthread_mutex_lock(&g_social.lock);

  if (!g_social.running || g_social.paused || g_social.finalize)
    {
      pthread_mutex_unlock(&g_social.lock);
      return -EINVAL;
    }

  g_social.paused = true;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  printf("%s: paused\n", SOCIAL_TAG);
  social_post(VS_APP_EVENT_SOCIAL_PAUSED, 0, VS_EMOTION_NONE, 0, NULL);
  return 0;
}

int vs_social_resume(void)
{
  pthread_mutex_lock(&g_social.lock);

  if (!g_social.running || !g_social.paused || g_social.finalize)
    {
      pthread_mutex_unlock(&g_social.lock);
      return -EINVAL;
    }

  g_social.paused = false;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  printf("%s: resumed\n", SOCIAL_TAG);
  social_post(VS_APP_EVENT_SOCIAL_RESUMED, 0, VS_EMOTION_NONE, 0, NULL);
  return 0;
}

int vs_social_finalize(uint32_t request_id)
{
  pthread_mutex_lock(&g_social.lock);

  if (!g_social.running || g_social.finalize)
    {
      pthread_mutex_unlock(&g_social.lock);
      return -EINVAL;
    }

  /* Adopt the id the UI is waiting on.  Normally identical to the one the
   * session started with; taking it again means an event cannot be stamped
   * with an id the UI has already retired.
   */

  g_social.request_id = request_id;
  g_social.finalize   = true;
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  printf("%s: finalizing\n", SOCIAL_TAG);
  return 0;
}

void vs_social_abort(void)
{
  pthread_mutex_lock(&g_social.lock);

  if (!g_social.running)
    {
      pthread_mutex_unlock(&g_social.lock);
      return;
    }

  g_social.abort        = true;
  g_social.stop_capture = true;
  g_social.paused       = false;
  pthread_cond_broadcast(&g_social.cond);

  /* Unblock whatever the workers are waiting on so the session thread reaches
   * its cleanup without waiting out a poll or a chunk.  abort() rather than
   * stop() on the microphone: nothing staged is going to be uploaded.
   *
   * Both calls stay inside the lock, which is the only thing preventing a
   * use-after-free here.  The session thread is racing to reach
   * social_release_devices(), and that releases these two handles; if the lock
   * were dropped first, the flag store above is exactly what sets that thread
   * running, and these two lines could then be handed pointers it had already
   * freed.  Holding the lock means the fields are either still valid or
   * already NULL, because release_devices() clears them under it too.
   *
   * Safe to hold across: vs_media_stream_wake() only stores a bool, and
   * vs_audio_capture_abort() takes the capture handle's own lock briefly and
   * never reaches back for this one, so the ordering stays one-way.
   */

  vs_media_stream_wake(g_social.camera, true);
  vs_audio_capture_abort(g_social.mic);
  pthread_mutex_unlock(&g_social.lock);

  printf("%s: aborted\n", SOCIAL_TAG);
}

bool vs_social_active(void)
{
  bool active;

  pthread_mutex_lock(&g_social.lock);
  active = g_social.running;
  pthread_mutex_unlock(&g_social.lock);
  return active;
}

void vs_social_close(void)
{
  pthread_t thread;
  bool joinable;

  vs_social_abort();

  pthread_mutex_lock(&g_social.lock);
  joinable = g_social.session_joinable;
  thread   = g_social.session_thread;
  g_social.session_joinable = false;
  pthread_mutex_unlock(&g_social.lock);

  if (joinable)
    {
      pthread_join(thread, NULL);
    }

  social_queue_flush();
}
