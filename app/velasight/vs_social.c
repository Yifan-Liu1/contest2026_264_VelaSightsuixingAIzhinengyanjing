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
#include <sched.h>
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
#include "include/vs_tts.h"
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



/* Nothing here bounds the spoken-minutes download itself: the transport's own
 * CONFIG_VS_SOCIAL_IO_TIMEOUT_MS ends a transfer that has stalled, and a
 * transfer that is merely slow is still progress.  What catches a download that
 * neither finishes nor fails is the UI's per-stage deadline -- see
 * VS_SOCIAL_FETCH_STAGE_TIMEOUT_MS in vs_app.c, which is expressed against
 * CONFIG_VS_SOCIAL_FETCH_TIMEOUT_MS so the two cannot drift apart.
 */

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

#ifndef CONFIG_VS_SOCIAL_AUDIO_QUEUE_SLOTS
#  define CONFIG_VS_SOCIAL_AUDIO_QUEUE_SLOTS 16
#endif

#ifndef CONFIG_VS_SOCIAL_IMAGE_QUEUE_SLOTS
#  define CONFIG_VS_SOCIAL_IMAGE_QUEUE_SLOTS 2
#endif

#ifndef CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS
#  define CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS 2000
#endif

/* How the sampler moves between CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS and
 * CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS.
 *
 * Multiplicative backoff, additive recovery.  A dropped frame means the link
 * is already oversubscribed, so halving the rate immediately is the cheap
 * mistake to make; walking back up in steps, and only after a run of frames
 * that made it through, keeps a single bad second from being paid for twice.
 */

#define SOCIAL_IMAGE_RECOVER_STREAK 4
#define SOCIAL_IMAGE_RECOVER_STEP_MS 250u

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

/* How many individual microphone gaps are named before the log falls back to
 * the teardown summary.  A handful is a diagnosis; a hundred is noise that
 * pushes the cause off the screen, and the count carries it either way.
 */

#define SOCIAL_GAP_REPORT_MAX 5

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

/* One bounded ring of slots.  The storage lives in social_state_s so both
 * rings are still one allocation-free static structure; this holds only the
 * indices, so the push and pop logic is written once for a queue whose depth
 * and drop policy differ between the two media.
 */

struct social_ring_s
{
  struct social_slot_s *slot;
  uint8_t cap;
  uint8_t read;
  uint8_t write;
  uint8_t count;
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

  /* One ring per medium, between the producers and the uploader.
   *
   * Separate because the two media are not interchangeable and a shared ring
   * forced them to be.  A frame lost costs one sample from a timeline with
   * many; a chunk lost removes those seconds of speech from the transcript and
   * from the spoken minutes with nothing else holding a copy.  Sharing four
   * slots let a burst of 160 KB frames evict 6 KB of irreplaceable audio.
   *
   * The uploader takes audio first.  Strict priority is safe at these rates --
   * one chunk every CONFIG_VS_SOCIAL_AUDIO_CHUNK_MS against an upload measured
   * near a second means audio can claim about half the uplink at worst -- and
   * it is what makes "audio is not dropped" mean something rather than being a
   * queue depth with hope attached.
   */

  struct social_ring_s audio;
  struct social_ring_s image;

  struct social_slot_s audio_slot[CONFIG_VS_SOCIAL_AUDIO_QUEUE_SLOTS];
  struct social_slot_s image_slot[CONFIG_VS_SOCIAL_IMAGE_QUEUE_SLOTS];

  uint32_t image_seq;
  uint32_t audio_seq;
  uint32_t dropped_image;
  uint32_t dropped_audio;
  uint32_t uploaded;
  uint32_t upload_failed;

  /* What a retry needs to resume from, and whether there is anything to
   * resume.
   *
   * The close half of a finalize is not repeatable -- DELETE /session has
   * already been accepted and the cloud has already handed over the msgId the
   * result will appear under -- but everything after it is: polling that
   * msgId, writing the record and fetching the audio all read cloud state that
   * is still there.  So a failure in the tail keeps the msgId and the error
   * page offers to run the tail again, rather than making the user hold a
   * whole new conversation.
   *
   * Cleared by vs_social_start(), because a new session's msgId supersedes it
   * and re-polling the old one would report the previous conversation.
   */

  char retry_msg_id[VS_CLOUD_MSG_ID_MAX];
  bool retry_ready;

  /* Where the finalize sequence is, and when it got there.  The stage is what
   * the finalizing page shows; the timestamps are what the log lines subtract
   * to say how long each step actually took.
   */

  enum vs_social_stage_e stage;
  uint64_t stage_began_ms;
  uint64_t finalize_began_ms;

  /* The camera's malformed-frame count, copied out of the stream before it is
   * closed.
   *
   * A snapshot rather than a read through g_social.camera at print time,
   * because those two things wanted opposite orderings.  social_log_totals()
   * needed the handle to still exist; the hardware needed the handle to be
   * gone as early as possible.  Taking the number at the moment the capture
   * thread stops -- when it is final and the stream is still open -- lets the
   * stream be closed immediately and printed about afterwards.
   *
   * Only this one, because the delivered count is already reported by
   * vs_media_stream_close() and the frame total in the line below is
   * image_seq, which is what actually reached the ring.
   */

  uint32_t frames_malformed;

  /* The sampler's current interval, and the streak that walks it back down.
   *
   * Owned by the lock rather than by the capture thread, because the thread
   * that discovers the link cannot keep up is the one pushing frames and the
   * thread that discovers it recovered is the uploader.  The capture thread
   * only reads it.
   */

  uint32_t image_interval_ms;
  uint32_t image_ok_streak;

  /* Progress reporting for the two things that used to be visible only in the
   * end-of-session totals.  A count alone says how many; these say when, which
   * is what distinguishes a queue that is steadily behind from one that stalled
   * for a moment.  Image drops and upload progress are coalesced -- see
   * social_image_push() and the upload worker -- because at several frames a
   * second a line each would bury the log.  Audio drops are not coalesced:
   * they should not happen, and each one is a hole in the transcript.
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
 * Name: social_stage_name
 *
 * Description:
 *   The log name for a stage.  Separate from the on-screen text below because
 *   the two have different jobs: this one is grepped, that one is read by
 *   someone wearing the device and is limited to what the panel can hold.
 *
 ****************************************************************************/

static const char *social_stage_name(enum vs_social_stage_e stage)
{
  switch (stage)
    {
      case VS_SOCIAL_STAGE_STOPPING:
        return "stopping capture";

      case VS_SOCIAL_STAGE_CLOSING:
        return "closing the cloud session";

      case VS_SOCIAL_STAGE_WAITING:
        return "waiting for the minutes";

      case VS_SOCIAL_STAGE_SAVING:
        return "saving the record";

      case VS_SOCIAL_STAGE_FETCHING:
        return "fetching the spoken minutes";

      default:
        return "idle";
    }
}

/****************************************************************************
 * Name: social_stage_text
 *
 * Description:
 *   What the finalizing page's middle line says during this stage.  Four
 *   characters of title plus one short line is all that panel holds, so these
 *   are phrased as what is happening rather than as what it is waiting for.
 *
 ****************************************************************************/

static const char *social_stage_text(enum vs_social_stage_e stage)
{
  switch (stage)
    {
      case VS_SOCIAL_STAGE_STOPPING:
        return "正在停止采集";

      case VS_SOCIAL_STAGE_CLOSING:
        return "正在通知云端";

      case VS_SOCIAL_STAGE_WAITING:
        return "正在生成建议";

      case VS_SOCIAL_STAGE_SAVING:
        return "正在保存记录";

      case VS_SOCIAL_STAGE_FETCHING:
        return "正在接收语音";

      default:
        return "整理中";
    }
}

/****************************************************************************
 * Name: social_stage
 *
 * Description:
 *   Enter one stage of the finalize sequence: log how long the previous one
 *   took, and tell the UI what to show and that progress was made.
 *
 *   The event is what refreshes the finalizing page's watchdog, so a stage that
 *   is merely slow is distinguishable from one that has stopped.  Before this
 *   existed the page had a single deadline covering every step, which meant a
 *   slow cloud and a wedged download looked identical -- and the deadline was
 *   sized for the sum, so it was also nearly useless as a hang detector.
 *
 ****************************************************************************/

static void social_stage(enum vs_social_stage_e stage)
{
  uint64_t now = social_now_ms();
  enum vs_social_stage_e previous;
  uint64_t spent;
  uint64_t total;

  pthread_mutex_lock(&g_social.lock);
  previous = g_social.stage;
  spent    = g_social.stage_began_ms != 0 ? now - g_social.stage_began_ms : 0;
  total    = g_social.finalize_began_ms != 0 ?
             now - g_social.finalize_began_ms : 0;
  g_social.stage          = stage;
  g_social.stage_began_ms = now;
  pthread_mutex_unlock(&g_social.lock);

  if (previous != VS_SOCIAL_STAGE_NONE)
    {
      printf("%s: finalize: %s took %lu ms\n", SOCIAL_TAG,
             social_stage_name(previous), (unsigned long)spent);
    }

  printf("%s: finalize: %s (t+%lu ms)\n", SOCIAL_TAG,
         social_stage_name(stage), (unsigned long)total);

  {
    struct vs_app_event_s event;
    unsigned int attempt;

    memset(&event, 0, sizeof(event));
    event.type       = VS_APP_EVENT_SOCIAL_STAGE;
    event.request_id = g_social.request_id;
    event.stage      = stage;
    snprintf(event.text, sizeof(event.text), "%s", social_stage_text(stage));

    for (attempt = 0; attempt < SOCIAL_POST_ATTEMPTS; attempt++)
      {
        if (vs_app_post_event(&event) != -EAGAIN)
          {
            return;
          }

        usleep(SOCIAL_POST_RETRY_US);
      }

    /* A dropped stage costs a stale line and one watchdog period that was not
     * refreshed.  Worth a word, not worth failing over: the next stage will
     * refresh it, and the terminal event does not depend on any of these
     * having arrived.
     */

    printf("%s: stage %s not shown, UI queue full\n", SOCIAL_TAG,
           social_stage_name(stage));
  }
}

/****************************************************************************
 * Name: social_fail_finalize
 *
 * Description:
 *   Report a finalize that could not be completed, saying which stage it died
 *   in and whether the error page may offer to run the tail again.
 *
 ****************************************************************************/

static void social_fail_finalize(int error, const char *what)
{
  enum vs_social_stage_e stage;
  bool retryable;

  pthread_mutex_lock(&g_social.lock);
  stage     = g_social.stage;
  retryable = g_social.retry_ready;
  g_social.stage = VS_SOCIAL_STAGE_NONE;
  pthread_mutex_unlock(&g_social.lock);

  printf("%s: finalize failed while %s: %s (%d)%s\n", SOCIAL_TAG,
         social_stage_name(stage), what, error,
         retryable ? ", retry available" : "");

  social_post(VS_APP_EVENT_SOCIAL_FINALIZE_FAILED, error, VS_EMOTION_NONE, 0,
              NULL);
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

/* Evict the oldest slot and hand its buffer back, or NULL when the ring has
 * room.  Called with the lock held; the caller frees outside it, because
 * bk7258_psram_free() is not something to hold a lock across.
 */

static unsigned char *social_ring_make_room(struct social_ring_s *ring,
                                            bool *from_psram)
{
  unsigned char *evicted;

  *from_psram = false;

  if (ring->count < ring->cap)
    {
      return NULL;
    }

  evicted     = ring->slot[ring->read].data;
  *from_psram = ring->slot[ring->read].from_psram;
  ring->read  = (uint8_t)((ring->read + 1) % ring->cap);
  ring->count--;
  return evicted;
}

/* Store one item.  Called with the lock held and with room already made. */

static void social_ring_store(struct social_ring_s *ring,
                              enum vs_cloud_media_e type,
                              unsigned char *data, size_t len,
                              bool from_psram, uint32_t sequence,
                              uint32_t alert_gen)
{
  struct social_slot_s *slot = &ring->slot[ring->write];

  slot->type       = type;
  slot->data       = data;
  slot->len        = len;
  slot->from_psram = from_psram;
  slot->sequence   = sequence;
  slot->alert_gen  = alert_gen;

  ring->write = (uint8_t)((ring->write + 1) % ring->cap);
  ring->count++;
}

/****************************************************************************
 * Name: social_audio_push
 *
 * Description:
 *   Hand one encoded chunk to the uploader.  Takes ownership on every path.
 *
 *   Audio is the medium that must not be lost.  Nothing else recorded these
 *   seconds: the transcript in audioTimeline and the spoken minutes are both
 *   derived from these chunks, so a chunk dropped here is a hole in the
 *   session's only record of what was said.  A frame dropped is one sample
 *   fewer in a timeline that has many.
 *
 *   Which is why this queue is deep and the image one is shallow, and why a
 *   drop here is logged every single time instead of being coalesced the way
 *   image drops are.  It should not happen; if it does, each occurrence is
 *   worth a line.
 *
 *   It can still happen.  The honest alternative -- blocking until the
 *   uploader catches up -- would stall the thread that drains the microphone
 *   ring, and the audio would be lost inside vs_audio one level down where
 *   this counter cannot see it.  Dropping the oldest and saying so is the
 *   lesser failure.
 *
 ****************************************************************************/

static void social_audio_push(unsigned char *data, size_t len,
                              bool from_psram, uint32_t sequence,
                              uint32_t alert_gen)
{
  unsigned char *evicted;
  bool evicted_psram;
  uint32_t total = 0;
  bool dropped = false;

  pthread_mutex_lock(&g_social.lock);

  evicted = social_ring_make_room(&g_social.audio, &evicted_psram);
  if (evicted != NULL || g_social.audio.count >= g_social.audio.cap)
    {
      g_social.dropped_audio++;
      total = g_social.dropped_audio;
      dropped = true;
    }

  social_ring_store(&g_social.audio, VS_CLOUD_MEDIA_AUDIO, data, len,
                    from_psram, sequence, alert_gen);
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  if (dropped)
    {
      printf("%s: AUDIO DROPPED, queue of %u full, %lu lost this session\n",
             SOCIAL_TAG, (unsigned)g_social.audio.cap, (unsigned long)total);
    }

  social_free(evicted, evicted_psram);
}

/****************************************************************************
 * Name: social_image_push
 *
 * Description:
 *   Hand one frame to the uploader, dropping the oldest when the ring is
 *   full and telling the sampler to slow down.
 *
 *   Dropping the oldest is the right answer for this medium: an emotion result
 *   is about the moment it was sampled, so the newest frame is worth more than
 *   any predecessor.  With a two-slot ring that means what the cloud receives
 *   is always the freshest frame plus at most one behind it.
 *
 *   The feedback is the new part.  A full ring is the link telling the sampler
 *   it is producing faster than the uplink carries, and continuing at the same
 *   rate does not deliver more frames -- it delivers the same number with a
 *   backlog in front of them, which makes every frame the cloud sees older
 *   than it needed to be.  So the interval doubles here, bounded by
 *   CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS, and the uploader walks it back
 *   down once frames start arriving unimpeded again.
 *
 ****************************************************************************/

static void social_image_push(unsigned char *data, size_t len,
                              bool from_psram, uint32_t sequence,
                              uint32_t alert_gen)
{
  unsigned char *evicted;
  bool evicted_psram;
  bool report = false;
  uint32_t drop_total = 0;
  uint32_t drop_since = 0;
  uint32_t interval = 0;
  bool slowed = false;

  pthread_mutex_lock(&g_social.lock);

  evicted = social_ring_make_room(&g_social.image, &evicted_psram);
  if (evicted != NULL)
    {
      uint64_t now = social_now_ms();

      g_social.dropped_image++;

      /* Back off, and reset the recovery streak: the run of clean uploads
       * that would have earned a step back up has just been interrupted.
       */

      g_social.image_ok_streak = 0;
      if (g_social.image_interval_ms < CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS)
        {
          uint32_t next = g_social.image_interval_ms * 2u;

          if (next > CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS)
            {
              next = CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS;
            }

          g_social.image_interval_ms = next;
          interval = next;
          slowed = true;
        }

      /* Rate limited by time rather than by count, which is what suits an
       * event this uneven.  A first drop after a quiet stretch prints at once
       * -- that timestamp is the point, it says when the queue started losing
       * -- and a burst collapses into one line carrying the count, instead of
       * fifty lines that push the cause off the screen.
       */

      if (g_social.drop_report_ms == 0 ||
          now - g_social.drop_report_ms >= SOCIAL_DROP_REPORT_MS)
        {
          report     = true;
          drop_total = g_social.dropped_image;
          drop_since = g_social.dropped_image - g_social.drop_reported;

          g_social.drop_reported  = g_social.dropped_image;
          g_social.drop_report_ms = now;
        }
    }

  social_ring_store(&g_social.image, VS_CLOUD_MEDIA_IMAGE, data, len,
                    from_psram, sequence, alert_gen);
  pthread_cond_broadcast(&g_social.cond);
  pthread_mutex_unlock(&g_social.lock);

  /* Printed outside the lock, for the same reason the eviction is freed
   * outside it: this runs on the capture threads, and the console is a
   * mailbox channel that can block.
   */

  if (report)
    {
      printf("%s: image queue full, dropped %lu (+%lu since the last line)\n",
             SOCIAL_TAG, (unsigned long)drop_total,
             (unsigned long)drop_since);
    }

  if (slowed)
    {
      printf("%s: image sampling slowed to %lu ms (%.2f fps)\n", SOCIAL_TAG,
             (unsigned long)interval, 1000.0 / (double)interval);
    }

  social_free(evicted, evicted_psram);
}

/* Take the next item, waiting until one arrives or the uploader should stop.
 *
 * Returns true with *out filled.  False means "stop": the producers have
 * finished and the ring is empty.
 */

/* Take from a ring.  Called with the lock held and the ring known non-empty. */

static void social_ring_take(struct social_ring_s *ring,
                             struct social_slot_s *out)
{
  *out = ring->slot[ring->read];
  memset(&ring->slot[ring->read], 0, sizeof(ring->slot[0]));
  ring->read = (uint8_t)((ring->read + 1) % ring->cap);
  ring->count--;
}

static bool social_queue_pop(struct social_slot_s *out)
{
  pthread_mutex_lock(&g_social.lock);

  while (g_social.audio.count == 0 && g_social.image.count == 0)
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

  /* Audio first, always.  See social_state_s::audio for why the priority is
   * strict rather than weighted: at one chunk per audio interval against an
   * upload measured near a second, audio cannot starve images, and anything
   * softer than "audio goes first" would make the depth of its queue the only
   * thing protecting it.
   */

  if (g_social.audio.count > 0)
    {
      social_ring_take(&g_social.audio, out);
    }
  else
    {
      social_ring_take(&g_social.image, out);
    }

  pthread_mutex_unlock(&g_social.lock);
  return true;
}

/* Release everything still queued.  For the abort path and for teardown. */

static void social_queue_flush(void)
{
  struct social_slot_s drop[CONFIG_VS_SOCIAL_AUDIO_QUEUE_SLOTS +
                            CONFIG_VS_SOCIAL_IMAGE_QUEUE_SLOTS];
  uint8_t n = 0;
  uint8_t i;

  pthread_mutex_lock(&g_social.lock);
  while (g_social.audio.count != 0)
    {
      social_ring_take(&g_social.audio, &drop[n++]);
    }

  while (g_social.image.count != 0)
    {
      social_ring_take(&g_social.image, &drop[n++]);
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
      uint32_t interval;
      bool paused;
      int ret;

      pthread_mutex_lock(&g_social.lock);
      if (g_social.stop_capture || g_social.abort)
        {
          pthread_mutex_unlock(&g_social.lock);
          break;
        }

      paused = g_social.paused;

      /* Read every iteration rather than once at the top.  social_image_push()
       * lengthens it when the ring overflows and the uploader shortens it again
       * when frames start getting through, so the sampler follows the link
       * within one frame of the link changing.
       */

      interval = g_social.image_interval_ms;
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

      next += interval;
      if (next < now)
        {
          next = now + interval;
        }

      /* The grab timeout follows the configured ceiling rather than the
       * current interval: a backed-off sampler is waiting on the uplink, not
       * on the camera, and stretching the camera's patience with it would turn
       * one slow upload into a slow frame as well.
       */

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

        social_image_push(frame.data, frame.len, frame.from_psram, seq, gen);
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

  /* Where the ring's drop counter was last time this looked, and what the gaps
   * it found cost.  See the check at the top of the loop.
   */

  size_t dropped_seen = 0;
  size_t discarded = 0;
  unsigned int gaps = 0;

  (void)arg;

  pcm = (int16_t *)social_alloc(SOCIAL_CHUNK_BYTES, &pcm_psram);
  ogg = social_alloc(SOCIAL_OGG_BYTES, &ogg_psram);

  /* SRAM for the encoder state when it fits, PSRAM only as the fallback.
   *
   * This asked for PSRAM outright until now, on the grounds that the 38 KiB of
   * encoder state competed with pthread stacks, which can only come from SRAM.
   * That was true of the ordering it was written for, and is not true here: the
   * encoder is created inside this worker, so all four of the session's stacks
   * -- including this thread's own 32 KiB -- are already reserved by the time
   * this line runs.  Nothing is created after it.
   *
   * The reason to want SRAM is that PSRAM on this board is mapped
   * non-cacheable ("PSRAM online ... RW/XN/non-cacheable" at boot), and the
   * encoder touches its state constantly.  An uncached 38 KiB working set is a
   * plausible part of why encoding 2 s of audio measured 3.2 s of CPU.
   *
   * Safe either way: audio_test_ogg_encoder_create() falls back to PSRAM by
   * itself when the SRAM allocation fails, so the worst case is exactly the old
   * behaviour rather than a session without audio.
   */

  encoder = audio_test_ogg_encoder_create(SOCIAL_AUDIO_RATE,
                                          CONFIG_VS_SOCIAL_AUDIO_BITRATE,
                                          false);

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

      /* Has the ring lost anything since the last look?
       *
       * It can, and when it does the loss is invisible to the read below:
       * vs_audio_capture_read() hands back whatever is staged with no way to
       * say that something is missing from in front of it.  So a chunk quietly
       * ends up holding the speech from both sides of a hole, presented to the
       * cloud as two contiguous seconds -- which is exactly what the pause
       * branch above refuses to do, for exactly the same reason, and it had no
       * effect here because an overflow is not a pause.
       *
       * Measured 2026-09-07: 44.8% of a 74 s session was lost this way and
       * every one of the eighteen chunks that reached the cloud spanned a gap.
       * The timeline built from them cannot be right.
       *
       * So the partial chunk is abandoned and the next one starts clean.  That
       * discards up to two seconds of good audio to avoid mislabelling it,
       * which is the same trade the pause branch makes.
       */

      {
        struct vs_audio_level_s level;

        vs_audio_capture_level(g_social.mic, &level);

        if (level.dropped != dropped_seen)
          {
            size_t lost = level.dropped - dropped_seen;

            dropped_seen = level.dropped;
            gaps++;

            if (filled > 0)
              {
                discarded += filled;
                filled = 0;
              }

            /* Rate limited by count, not by time: these should be rare, and
             * when they are not the count is the diagnosis.  One line for the
             * first few and then a summary at teardown.
             */

            if (gaps <= SOCIAL_GAP_REPORT_MAX)
              {
                printf("%s: microphone gap %u, %zu bytes (%lu ms) lost, chunk "
                       "restarted\n", SOCIAL_TAG, gaps, lost,
                       (unsigned long)(lost / 2u * 1000u / SOCIAL_AUDIO_RATE));
              }
          }
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
        social_audio_push(copy, encoded, copy_psram, seq, gen);
      }

      /* One guaranteed scheduling point per chunk.
       *
       * Every other path through this loop either sleeps or is short, but the
       * one that just encoded a full chunk can go straight back to a ring that
       * still has data in it -- and then it never sleeps at all.  That is only
       * survivable because this thread now runs below the UI; it was a frozen
       * device when it did not.  Making the yield unconditional means the same
       * mistake cannot be reintroduced by a priority change alone.
       *
       * A yield rather than a delay: there is nothing to wait for, and the
       * encoder is already the slowest link in the chain.
       */

      sched_yield();
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
                social_audio_push(copy, encoded, copy_psram, seq, gen);
                printf("%s: tail chunk %lu, %zu ms\n", SOCIAL_TAG,
                       (unsigned long)seq,
                       filled / sizeof(int16_t) * 1000u / SOCIAL_AUDIO_RATE);
              }
          }
      }
  }

out:
  if (gaps > 0)
    {
      printf("%s: %u microphone gap(s) this session, %zu bytes of partial "
             "chunks discarded to keep the timeline honest\n", SOCIAL_TAG,
             gaps, discarded);
    }

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
        uint8_t queued_audio = 0;
        uint8_t queued_image = 0;
        uint8_t inflight = 0;
        uint32_t recovered = 0;
        bool sped_up = false;

        pthread_mutex_lock(&g_social.lock);
        g_social.uploaded++;
        g_social.upload_window_ms += spent_ms;
        social_inflight_add(result.msg_id, slot.alert_gen);

        /* An image that made it through is the evidence that the link has room
         * again, so this is where the sampler is allowed back up.  Only images
         * count: audio uploads happen whatever the frame rate, so crediting
         * them would let a silent-but-congested link talk the sampler into
         * speeding up on the strength of traffic that was never the problem.
         *
         * A run rather than a single success, and a step rather than a halving,
         * because the cost of guessing wrong in this direction is another drop.
         */

        if (slot.type == VS_CLOUD_MEDIA_IMAGE &&
            g_social.image_interval_ms > CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS &&
            ++g_social.image_ok_streak >= SOCIAL_IMAGE_RECOVER_STREAK)
          {
            uint32_t next = g_social.image_interval_ms;

            g_social.image_ok_streak = 0;
            next = next > SOCIAL_IMAGE_RECOVER_STEP_MS +
                          CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS ?
                   next - SOCIAL_IMAGE_RECOVER_STEP_MS :
                   CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;

            g_social.image_interval_ms = next;
            recovered = next;
            sped_up = true;
          }

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
            report       = true;
            total        = g_social.uploaded;
            since        = g_social.uploaded - g_social.upload_reported;
            window       = g_social.upload_window_ms;
            queued_audio = g_social.audio.count;
            queued_image = g_social.image.count;
            inflight     = g_social.inflight_count;

            g_social.upload_reported  = g_social.uploaded;
            g_social.upload_window_ms = 0;
          }

        pthread_mutex_unlock(&g_social.lock);

        if (report)
          {
            printf("%s: uploaded %lu, last %lu averaged %lu ms, "
                   "audio %u/%u image %u/%u, inflight %u\n", SOCIAL_TAG,
                   (unsigned long)total, (unsigned long)since,
                   (unsigned long)(since > 0 ? window / since : 0),
                   (unsigned)queued_audio,
                   (unsigned)CONFIG_VS_SOCIAL_AUDIO_QUEUE_SLOTS,
                   (unsigned)queued_image,
                   (unsigned)CONFIG_VS_SOCIAL_IMAGE_QUEUE_SLOTS,
                   (unsigned)inflight);
          }

        if (sped_up)
          {
            printf("%s: image sampling restored to %lu ms (%.2f fps)\n",
                   SOCIAL_TAG, (unsigned long)recovered,
                   1000.0 / (double)recovered);
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

static void social_release_devices(void);
static void social_log_capture_level(void);

/* Stop the producers, join them, and hand the hardware back.  Called on the
 * session thread.
 *
 * The release used to happen much later, in the session worker, after
 * social_finalize_sequence() had returned.  That put the whole tail of a
 * session -- up to a minute of polling for the minutes, the text fetch, the
 * history write, the spoken-minutes download and its playback -- between the
 * last frame anyone wanted and VIDIOC_STREAMOFF.  Nothing was leaking, but
 * the sensor, the YUV block, the JPEG encoder and the DMA channel all kept
 * running at the negotiated rate the entire time, with no consumer: measured
 * 2026-09-07, a session that ended at 10:45:43 was still emitting a frame
 * every 507 ms at 10:47:24, a hundred seconds later, and every one of those
 * frames also cost a console line over the mailbox.  That is CPU taken from
 * the download and the playback that were the only things still working.
 *
 * So the devices go here, at the one moment that is provably safe: both
 * producers have exited, so nothing is going to call grab() or read() again,
 * and the uploader has drained the rings, so nothing still references a frame.
 * The later social_release_devices() calls are idempotent and become no-ops.
 */

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

  /* The camera's counter, while the stream is still open and now that the only
   * thread incrementing it has stopped.  See social_state_s.
   */

  vs_media_stream_stats(g_social.camera, NULL, &g_social.frames_malformed);

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

  /* The microphone's counters, after the only thread reading it has stopped so
   * they are final, and before the release below closes the handle they live
   * in.  This is the one window where they can be read at all.
   */

  social_log_capture_level();

  /* Nothing above this line will touch either device again, so the camera and
   * the microphone are released now rather than at the end of the session.
   * See the header comment for what the delay used to cost.
   *
   * This also frees the poll scratch, which is correct here: the session
   * thread's polling loop has already ended by the time either caller reaches
   * this function.
   */

  social_release_devices();
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

/****************************************************************************
 * Name: social_log_capture_level
 *
 * Description:
 *   Report what the microphone actually delivered.  Called while the capture
 *   handle is still open, which is the only time these counters can be read.
 *
 *   Here because the chunk count alone cannot explain itself.  Measured
 *   2026-09-07 a 27.5 s session produced six 2-second chunks instead of the
 *   expected thirteen, with zero drops recorded on this module's own ring --
 *   so roughly half the conversation never reached it, and nothing in the log
 *   said where it went.  These four counters separate the candidates: dropped
 *   means the staging ring overflowed because this module read too slowly,
 *   settled means the analog front end was still stabilising, clipped against
 *   samples means the gain is wrong, and all of them zero with a low sample
 *   count means the ADC itself under-delivered.
 *
 ****************************************************************************/

static void social_log_capture_level(void)
{
  struct vs_audio_level_s level;

  if (g_social.mic == NULL)
    {
      printf("%s: no microphone this session\n", SOCIAL_TAG);
      return;
    }

  memset(&level, 0, sizeof(level));
  vs_audio_capture_level(g_social.mic, &level);

  printf("%s: microphone: %llu samples (%lu ms), peak %u rms %u, "
         "%zu bytes dropped, %zu settling, %llu clipped\n", SOCIAL_TAG,
         (unsigned long long)level.samples,
         (unsigned long)(level.samples * 1000ull / SOCIAL_AUDIO_RATE),
         level.peak, level.rms, level.dropped, level.settled,
         (unsigned long long)level.clipped);
}

static void social_log_totals(void)
{
  uint32_t malformed = g_social.frames_malformed;

  /* Drops are reported per medium.  One combined figure could not answer the
   * question that matters -- whether any speech was lost -- because an image
   * drop is routine at these frame rates and an audio drop is not.
   */

  printf("%s: session %s totals: %lu frames (%lu malformed), %lu chunks, "
         "%lu uploaded, %lu upload failures, %lu images dropped, "
         "%lu audio dropped, %lu unanswered, image interval %lu ms\n",
         SOCIAL_TAG, g_social.session.session_id,
         (unsigned long)g_social.image_seq, (unsigned long)malformed,
         (unsigned long)g_social.audio_seq, (unsigned long)g_social.uploaded,
         (unsigned long)g_social.upload_failed,
         (unsigned long)g_social.dropped_image,
         (unsigned long)g_social.dropped_audio,
         (unsigned long)g_social.inflight_retired,
         (unsigned long)g_social.image_interval_ms);
}

/****************************************************************************
 * Name: social_persist_minutes
 *
 * Description:
 *   Write the end-of-session record.  Runs before SOCIAL_RESULT is posted, so
 *   a summary on screen is a summary on the card.
 *
 * Input Parameters:
 *   key_out - receives the record key the store assigned, which is what names
 *             this session's audio file.  vs_history_append() generates it, so
 *             it cannot be known before this returns.
 *
 * Returned Value:
 *   0 on success, or a negative errno.  A failure here is reported to the UI:
 *   telling the user their conversation was saved when it was not is worse
 *   than telling them it was not.
 *
 ****************************************************************************/

static int social_persist_minutes(const struct vs_cloud_minutes_s *minutes,
                                  const char *full_json, char *key_out,
                                  size_t key_cap)
{
  struct vs_history_index_s index;
  time_t now;
  struct tm tm;
  int ret;

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

  ret = vs_history_append(VS_HISTORY_KIND_SOCIAL, &index, full_json);
  if (ret < 0)
    {
      return ret;
    }

  /* index carries the assigned key back out of the append. */

  snprintf(key_out, key_cap, "%s", index.record_key);
  printf("%s: record %s saved (%s, calm %u happy %u tense %u)\n", SOCIAL_TAG,
         index.record_key, index.incomplete ? "partial" : "complete",
         index.calm, index.happy, index.tense);
  return 0;
}

/****************************************************************************
 * Name: social_minutes_dir
 *
 * Description:
 *   Make sure the directory holding the spoken minutes exists.
 *
 *   vs_history_open() already creates the history tree at startup, so on a
 *   healthy boot this finds it there.  It is done again here because a failed
 *   history init should not silently take the audio down with it.
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
   * failure shows up immediately as the download failing with a message that
   * names the actual path.
   */

  (void)mkdir(dir, 0700);
}

/****************************************************************************
 * Name: social_fetch_minutes_audio
 *
 * Description:
 *   Download this record's spoken minutes and leave them on the card under the
 *   record's own name.
 *
 *   Filed per record rather than at one fixed path, which is what lets the
 *   history page play a session back later.  The presigned URL is good for an
 *   hour, so there is no fetching it on demand -- if it is not taken now it is
 *   gone, and the record would be silent forever.
 *
 *   Streamed to storage rather than buffered.  The spoken minutes are not a
 *   bounded object: measured 2026-09-04 the shortest one this cloud can
 *   produce -- a session that detected no emotion at all -- was already 585 KB.
 *   Buffering put a ceiling on session length in the shape of a heap
 *   allocation; streaming costs one transfer buffer whatever the length, so
 *   what is left is a limit on the filesystem, which is a place a limit can
 *   live.
 *
 * Returned Value:
 *   0 with path_out naming a playable file, or a negative errno.  A failure is
 *   not fatal to the session: the record is already on the card and the summary
 *   is what the user asked for.  It is returned rather than swallowed so the
 *   caller can say so on screen and offer a retry.
 *
 ****************************************************************************/

static int social_fetch_minutes_audio(const char *url, const char *record_key,
                                      char *path_out, size_t path_cap)
{
  char path[VS_TTS_PATH_MAX];
  size_t file_len = 0;
  uint64_t began;
  int ret;

  path_out[0] = '\0';

  if (url == NULL || url[0] == '\0')
    {
      /* A session the cloud produced no audio for.  Not an error -- the seed
       * records are permanently in this state -- so it is reported as success
       * with no path and the caller simply has nothing to play.
       */

      printf("%s: no spoken minutes for %s\n", SOCIAL_TAG, record_key);
      return 0;
    }

  ret = vs_history_audio_path(VS_HISTORY_KIND_SOCIAL, record_key, path,
                              sizeof(path));
  if (ret < 0)
    {
      printf("%s: cannot name the audio for %s: %d\n", SOCIAL_TAG, record_key,
             ret);
      return ret;
    }

  social_minutes_dir(path);

  began = social_now_ms();
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
      return ret;
    }

  printf("%s: spoken minutes saved to %s, %zu bytes in %lu ms\n", SOCIAL_TAG,
         path, file_len, (unsigned long)(social_now_ms() - began));

  snprintf(path_out, path_cap, "%s", path);
  return 0;
}

/****************************************************************************
 * Name: social_collect_minutes
 *
 * Description:
 *   Everything after the cloud has accepted the close: poll for the minutes,
 *   save them, fetch the audio, then report and speak.  Runs on the session
 *   thread, and again on the retry thread if the first attempt failed.
 *
 *   Split out of social_finalize_sequence() precisely so it can run twice.
 *   Every step in here reads cloud or local state that is still there after a
 *   failure -- the msgId's result is stored server side, the record either was
 *   or was not written, the audio URL is valid for an hour -- so a retry is a
 *   repeat rather than a recovery.
 *
 *   The order is what the UI asked for and is load-bearing: the audio is
 *   fetched *before* the result is posted, so the finalizing page stays up
 *   until there is genuinely nothing left to wait for, and the page change and
 *   the first sound then happen together.  Fetching after the post -- which is
 *   what this used to do -- put the summary on screen and then left a silent
 *   device for however long the transfer took, with no way to tell that from a
 *   session that simply had no audio.
 *
 ****************************************************************************/

static void social_collect_minutes(const char *msg_id)
{
  struct vs_cloud_minutes_s minutes;
  char record_key[VS_HISTORY_KEY_MAX];
  char audio[VS_TTS_PATH_MAX];
  char *body = NULL;
  bool body_psram = false;
  uint64_t deadline;
  int ret;

  record_key[0] = '\0';
  audio[0] = '\0';

  body = (char *)social_alloc(CONFIG_VS_SOCIAL_RESP_MAX_BYTES, &body_psram);
  if (body == NULL)
    {
      social_fail_finalize(-ENOMEM, "no response buffer");
      return;
    }

  social_stage(VS_SOCIAL_STAGE_WAITING);

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
          /* Abandoned rather than failed: the UI has already left the page and
           * posting a failure would put an error in front of a user who asked
           * for none.
           */

          printf("%s: finalize abandoned while %s\n", SOCIAL_TAG,
                 social_stage_name(VS_SOCIAL_STAGE_WAITING));
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
      social_fail_finalize(ret, "the minutes never arrived");
      return;
    }

  printf("%s: minutes: calm %u happy %u tense %u, %u emotion / %u audio "
         "samples, text %s, tts %s\n", SOCIAL_TAG, minutes.calm,
         minutes.happy, minutes.tense, minutes.emotion_samples,
         minutes.audio_samples,
         minutes.txt_url[0] != '\0' ? "downloaded" : "inline",
         minutes.tts_url[0] != '\0' ? "available" : "(none)");

  social_stage(VS_SOCIAL_STAGE_SAVING);

  ret = social_persist_minutes(&minutes, body, record_key,
                               sizeof(record_key));
  social_free((unsigned char *)body, body_psram);

  if (ret < 0)
    {
      social_fail_finalize(ret, "the record could not be saved");
      return;
    }

  /* The audio, while the page still says so.
   *
   * A failure here does not fail the session.  The record is on the card and
   * the summary is the deliverable; refusing to show it because the speaker
   * could not be fed would throw away the part the user actually asked for.
   * It is logged and the session continues silently.
   */

  social_stage(VS_SOCIAL_STAGE_FETCHING);
  (void)social_fetch_minutes_audio(minutes.tts_url, record_key, audio,
                                   sizeof(audio));

  /* Nothing left to wait for.  The retry state goes now: from here on a repeat
   * would re-download audio that is already on the card and re-append a record
   * that is already there.
   */

  pthread_mutex_lock(&g_social.lock);
  g_social.retry_ready = false;
  g_social.stage       = VS_SOCIAL_STAGE_NONE;
  pthread_mutex_unlock(&g_social.lock);

  /* The summary text, truncated to what the result page can hold.  The full
   * text is in the record that was just written, which is what the Web history
   * page serves.
   */

  social_post(VS_APP_EVENT_SOCIAL_RESULT, 0, VS_EMOTION_NONE, 0,
              minutes.summary[0] != '\0' ? minutes.summary :
                                           "本次没有生成摘要");

  (void)vs_cloud_social_ack(g_social.session.session_id);

  /* And the sound, at the same moment as the page change.
   *
   * vs_tts_play() hands the file to its own worker and returns, so this thread
   * is free to finish tearing the session down while the speaker runs.  That is
   * the whole reason playback moved out of here: it used to hold the session
   * thread -- and therefore the session -- for the length of the audio.
   */

  if (audio[0] != '\0')
    {
      (void)vs_tts_play(audio);
    }

  printf("%s: finalize complete in %lu ms\n", SOCIAL_TAG,
         (unsigned long)(g_social.finalize_began_ms != 0 ?
                         social_now_ms() - g_social.finalize_began_ms : 0));
}

/****************************************************************************
 * Name: social_finalize_sequence
 *
 * Description:
 *   Everything after the user asks to stop.  Runs on the session thread.
 *
 ****************************************************************************/

static void social_finalize_sequence(void)
{
  char msg_id[VS_CLOUD_MSG_ID_MAX];
  int ret;

  pthread_mutex_lock(&g_social.lock);
  g_social.finalize_began_ms = social_now_ms();
  g_social.stage_began_ms    = 0;
  g_social.stage             = VS_SOCIAL_STAGE_NONE;
  pthread_mutex_unlock(&g_social.lock);

  social_stage(VS_SOCIAL_STAGE_STOPPING);
  social_stop_producers();

  social_stage(VS_SOCIAL_STAGE_CLOSING);
  ret = vs_cloud_social_finalize(g_social.session.session_id, msg_id,
                                 sizeof(msg_id));
  if (ret < 0)
    {
      /* No msgId means there is nothing for a retry to poll, so this one is
       * reported as final.  retry_ready is still false here.
       */

      social_fail_finalize(ret, "the cloud would not close the session");
      return;
    }

  /* From here a failure is resumable: the cloud has the close registered under
   * this msgId and will keep answering for it.
   */

  pthread_mutex_lock(&g_social.lock);
  snprintf(g_social.retry_msg_id, sizeof(g_social.retry_msg_id), "%s",
           msg_id);
  g_social.retry_ready = true;
  pthread_mutex_unlock(&g_social.lock);

  social_collect_minutes(msg_id);
}

/****************************************************************************
 * The session thread
 ****************************************************************************/

static int social_spawn(pthread_t *thread, void *(*entry)(void *),
                        size_t stacksize, int priority, const char *what)
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

  /* Per thread, because they are not all the same kind of work.
   *
   * Three of the four belong below the UI: they produce or send data that is
   * worth less the older it gets, so being late costs a dropped frame rather
   * than a gap the user hears.  The comment here used to say "above the UI ...
   * must not be held off by a redraw", which had it backwards -- with four of
   * these doing continuous TLS at VS_PRIORITY_VOICE, it was the redraw that
   * never happened.
   *
   * The audio drain is the exception and gets VS_PRIORITY_SOCIAL_AUDIO; see
   * vs_types.h for why a consumer below its producer loses samples outright.
   */

  param.sched_priority = priority;
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
                   CONFIG_VS_SOCIAL_STACKSIZE_UPLOAD, VS_PRIORITY_SOCIAL,
                   "upload") == 0)
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
                   CONFIG_VS_SOCIAL_STACKSIZE_CAPTURE, VS_PRIORITY_SOCIAL,
                   "capture") == 0)
    {
      g_social.capture_joinable = true;
    }

  /* The one that runs above the UI.  See VS_PRIORITY_SOCIAL_AUDIO. */

  if (g_social.mic != NULL &&
      social_spawn(&g_social.audio_thread, social_audio_worker,
                   CONFIG_VS_SOCIAL_STACKSIZE_AUDIO,
                   VS_PRIORITY_SOCIAL_AUDIO, "audio") == 0)
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
   * way in, so every counter the line below prints is final rather than being
   * read while a thread is still incrementing it -- and the tail audio chunk
   * is included.
   *
   * The devices are already gone by now: social_stop_producers(), at the top of
   * finalize_sequence(), releases them as soon as the producers have exited, so
   * the camera is not left streaming through the minutes and the playback.  The
   * malformed-frame count it printed was snapshotted there.  The call below is
   * kept because it is idempotent and because the paths that jump straight to
   * the labels need it.
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
 * Name: social_retry_worker
 *
 * Description:
 *   Run the resumable half of a finalize again.  Spawned by
 *   vs_social_retry_finalize() and shaped like the session thread on purpose:
 *   it uses the same handle, the same joinable flag and the same running flag,
 *   so the "one session at a time" machinery covers it without a second copy.
 *
 *   No devices are opened and no producers exist, so there is nothing to stop
 *   and nothing to release.  The cloud session is already closed; what is being
 *   repeated is only the reading of its result.
 *
 ****************************************************************************/

static void *social_retry_worker(void *arg)
{
  char msg_id[VS_CLOUD_MSG_ID_MAX];

  (void)arg;

  pthread_mutex_lock(&g_social.lock);
  snprintf(msg_id, sizeof(msg_id), "%s", g_social.retry_msg_id);
  g_social.finalize_began_ms = social_now_ms();
  g_social.stage_began_ms    = 0;
  g_social.stage             = VS_SOCIAL_STAGE_NONE;
  pthread_mutex_unlock(&g_social.lock);

  printf("%s: retrying the minutes for session %s, msg %s\n", SOCIAL_TAG,
         g_social.session.session_id, msg_id);

  social_collect_minutes(msg_id);

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

  /* Both rings, including their backing storage pointers: social_state_s is
   * static so a second session would otherwise inherit the first one's
   * indices, and the pointers have to be wired up somewhere.
   */

  g_social.audio.slot  = g_social.audio_slot;
  g_social.audio.cap   = CONFIG_VS_SOCIAL_AUDIO_QUEUE_SLOTS;
  g_social.audio.read  = 0;
  g_social.audio.write = 0;
  g_social.audio.count = 0;

  g_social.image.slot  = g_social.image_slot;
  g_social.image.cap   = CONFIG_VS_SOCIAL_IMAGE_QUEUE_SLOTS;
  g_social.image.read  = 0;
  g_social.image.write = 0;
  g_social.image.count = 0;

  g_social.image_seq      = 0;
  g_social.audio_seq      = 0;
  g_social.dropped_image  = 0;
  g_social.dropped_audio  = 0;
  g_social.uploaded       = 0;
  g_social.upload_failed  = 0;
  g_social.frames_malformed = 0;

  /* Start at the configured ceiling and let the link argue it down. */

  g_social.image_interval_ms = CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;
  g_social.image_ok_streak   = 0;

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

  /* A new session's close will produce its own msgId, and polling the previous
   * one would report the previous conversation into this one's UI.
   */

  g_social.retry_ready       = false;
  g_social.retry_msg_id[0]   = '\0';
  g_social.stage             = VS_SOCIAL_STAGE_NONE;
  g_social.stage_began_ms    = 0;
  g_social.finalize_began_ms = 0;
  g_social.alert_gen      = 1;
  g_social.extreme_streak = 0;
  g_social.calm_streak    = 0;
  g_social.alert_active   = false;
  g_social.alert_since_ms = 0;
  g_social.camera         = NULL;
  g_social.mic            = NULL;
  memset(g_social.audio_slot, 0, sizeof(g_social.audio_slot));
  memset(g_social.image_slot, 0, sizeof(g_social.image_slot));
  memset(g_social.inflight, 0, sizeof(g_social.inflight));

  g_social.running = true;
  pthread_mutex_unlock(&g_social.lock);

  ret = social_spawn(&g_social.session_thread, social_session_worker,
                     CONFIG_VS_SOCIAL_STACKSIZE_SESSION, VS_PRIORITY_SOCIAL,
                     "session");
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

int vs_social_retry_finalize(uint32_t request_id)
{
  int ret;

  pthread_mutex_lock(&g_social.lock);

  if (g_social.running)
    {
      pthread_mutex_unlock(&g_social.lock);
      return -EBUSY;
    }

  if (!g_social.retry_ready || g_social.retry_msg_id[0] == '\0')
    {
      pthread_mutex_unlock(&g_social.lock);
      return -EINVAL;
    }

  /* Join the thread that reported the failure before reusing its handle. */

  if (g_social.session_joinable)
    {
      pthread_t previous = g_social.session_thread;

      g_social.session_joinable = false;
      pthread_mutex_unlock(&g_social.lock);
      pthread_join(previous, NULL);
      pthread_mutex_lock(&g_social.lock);
    }

  /* The abort that a stage timeout set has to be cleared, or the poll loop
   * would abandon itself on its first pass and the retry would look like it
   * did nothing.
   */

  g_social.abort      = false;
  g_social.finalize   = true;
  g_social.request_id = request_id;
  g_social.running    = true;
  pthread_mutex_unlock(&g_social.lock);

  ret = social_spawn(&g_social.session_thread, social_retry_worker,
                     CONFIG_VS_SOCIAL_STACKSIZE_SESSION, VS_PRIORITY_SOCIAL,
                     "retry");
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

bool vs_social_can_retry(void)
{
  bool can;

  /* Deliberately not "and nothing is running".
   *
   * This is asked at the moment a failure is reported, which is while the
   * thread reporting it is still unwinding -- so a running check here would
   * answer false every time and the key would never be offered.  What it
   * reports is whether the work is resumable at all; whether it can start this
   * instant is vs_social_retry_finalize()'s business, and it says -EBUSY.
   */

  pthread_mutex_lock(&g_social.lock);
  can = g_social.retry_ready && g_social.retry_msg_id[0] != '\0';
  pthread_mutex_unlock(&g_social.lock);
  return can;
}

void vs_social_abort(void)
{
  /* Silence anything this session put on the speaker.  Outside the lock and
   * before the running check, because vs_tts has its own and because an abort
   * arriving after the session thread has already exited still has to stop the
   * playback that thread started.
   */

  vs_tts_stop();

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

  /* Whether or not a session is running, the last one may still be speaking:
   * playback outlives the session thread now that it has its own worker.
   */

  vs_tts_stop();
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
