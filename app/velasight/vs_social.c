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

#ifndef CONFIG_VS_SOCIAL_UPLOAD_WORKERS
#  define CONFIG_VS_SOCIAL_UPLOAD_WORKERS 2
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

#ifndef CONFIG_VS_SOCIAL_DOWNLOAD_TIMEOUT_MS
#  define CONFIG_VS_SOCIAL_DOWNLOAD_TIMEOUT_MS 60000
#endif



/* Three bounds sit over the spoken-minutes download, nested, and the nesting is
 * what makes any of them work.  Innermost is the transport's receive timeout,
 * which is the only thing that can end a request the peer has stopped
 * answering.  Then CONFIG_VS_SOCIAL_DOWNLOAD_TIMEOUT_MS, checked between
 * windows, which stops the transfer paying that cost a second time.  Outermost
 * is the UI's per-stage deadline, VS_SOCIAL_FETCH_STAGE_TIMEOUT_MS in vs_app.c,
 * derived from the middle one plus one transport stall so it cannot end up
 * inside it.
 *
 * It was inside it.  Measured 2026-09-08: the page's 110 s fired 12.6 s before
 * the transport's 120 s, so the download's own timeout and its attempt count
 * were both unreachable and an abort arrived instead of a diagnosis.
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

/* How many consecutive poll cycles may be given up to let a registration have
 * the cleartext connection to itself.  See social_poll_once().
 *
 * Two, against CONFIG_VS_SOCIAL_POLL_INTERVAL_MS of 1500, so the worst case is
 * a result arriving 3 s later than it would have.  That is far inside the
 * windows anything acts on -- VS_SOCIAL_ALERT_WINDOW_MS is 20 s and
 * VS_SOCIAL_ADVICE_TIMEOUT_MS is 30 s -- while the registrations it protects
 * are on the critical path of an item each.
 *
 * The sweep and the stale-alert release run before the yield is even
 * considered, so a yielded cycle still ages the alert state on time.
 */

#define SOCIAL_POLL_YIELD_MAX 2

#ifndef CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS
#  define CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS 1
#endif

#ifndef CONFIG_VS_SOCIAL_ALERT_WINDOW_MS
#  define CONFIG_VS_SOCIAL_ALERT_WINDOW_MS 20000
#endif

#ifndef CONFIG_VS_SOCIAL_ALERT_MIN_CONFIDENCE
#  define CONFIG_VS_SOCIAL_ALERT_MIN_CONFIDENCE 0
#endif

#ifndef CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS
#  define CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS 8000
#endif

#ifndef CONFIG_VS_SOCIAL_ALERT_HOLD_MS
#  define CONFIG_VS_SOCIAL_ALERT_HOLD_MS 12000
#endif

#ifndef CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS
#  define CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS 60000
#endif

#ifndef CONFIG_VS_SOCIAL_ADVICE_GRACE_MS
#  define CONFIG_VS_SOCIAL_ADVICE_GRACE_MS 6000
#endif

/* Room for the timestamp ring the raise decision keeps.  Sized to the ceiling
 * of VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS's Kconfig range so the array is a
 * compile-time constant whatever the option is set to.
 */

#define SOCIAL_ALERT_STAMP_MAX 10

#define SOCIAL_ALERT_NEEDED \
  (CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS < SOCIAL_ALERT_STAMP_MAX ? \
   CONFIG_VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS : SOCIAL_ALERT_STAMP_MAX)

/* Size of the in-flight table, which is allocated rather than declared.  Spelled
 * once here because sizeof() cannot be taken through the pointer that holds it.
 */

#define SOCIAL_INFLIGHT_BYTES \
  (sizeof(struct social_inflight_s) * CONFIG_VS_SOCIAL_INFLIGHT_MAX)

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
 *
 * Recovery stops at social_state_s::image_floor_ms, the slowest rate observed
 * to overflow, and only that floor's own re-probe can go below it.  The two
 * streaks are what make the loop settle: approaching a known-good rate is
 * cheap, guessing that a known-bad one has become good is not.  4 against 16 is
 * the ratio, sized so a re-probe costs about twenty seconds at the measured
 * delivery rate rather than about five -- long enough that a session spends its
 * time at capacity instead of hunting for it.
 */

#define SOCIAL_IMAGE_RECOVER_STREAK 4
#define SOCIAL_IMAGE_REPROBE_STREAK 16
#define SOCIAL_IMAGE_RECOVER_STEP_MS 250u

#ifndef CONFIG_VS_SOCIAL_INFLIGHT_MAX
#  define CONFIG_VS_SOCIAL_INFLIGHT_MAX 24
#endif

/* The table cannot be deeper than one getResult can ask about.  See
 * VS_CLOUD_POLL_MAX_IDS: the snapshot below stops at that limit and takes
 * entries in arrival order, so a deeper table has a tail that is never polled.
 */

#if CONFIG_VS_SOCIAL_INFLIGHT_MAX > VS_CLOUD_POLL_MAX_IDS
#  error "CONFIG_VS_SOCIAL_INFLIGHT_MAX exceeds VS_CLOUD_POLL_MAX_IDS"
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

  /* When this item was produced, taken on the producer thread as it enters the
   * ring, and handed to the cloud through vs_cloud_media_packet_s::produced_ms.
   *
   * Taken here rather than in vs_cloud_social_upload() because the two are
   * different moments and only this one is the truth.  An item can wait behind
   * others, so stamping at registration dates a frame from when the uploader
   * got to it; and with more than one upload worker two items can reach
   * registration in the opposite order to the one they were captured in, which
   * would reorder the audio the transcript is built from.
   *
   * social_now_ms(), so monotonic, which is the domain produced_ms is
   * specified in.  vs_cloud converts it to a wall-clock instant by subtracting
   * its age; sending a wall-clock reading from here would be stepped between
   * production and upload and is what produced_ms exists to avoid.
   */

  uint64_t stamp_ms;
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

/* One uploaded image whose result has not finished arriving.
 *
 * Images only.  Audio uploads are deliberately not tracked here, and that
 * follows from the interface: the cloud attaches an extreme moment's advice to
 * the msgId of an *image*, not to any audio msgId.  So an audio msgId can only
 * ever answer 11 ("still working", forever) or 30, and neither is acted on.
 * They used to occupy this table anyway -- at one chunk per two seconds against
 * sixteen slots, roughly half of it -- and the slots they held were taken from
 * the images that were waiting for the one result the feature exists to
 * produce.  See social_inflight_add().
 *
 * An entry is not retired when its result arrives, and that is the correction
 * this structure exists in its current shape for.  The device cannot predict
 * which msgId an advice will come under.  Two things establish that, both from
 * the AI-side log of 2026-09-08:
 *
 *   The cloud applies its extreme rule to a ten-second window rather than to a
 *   frame.  Eight frames classified 害怕 -- which is none of 生气, 反感 and
 *   伤心 -- were announced "[extreme] 检测到极端情绪" because the window's
 *   aggregate emotion was 生气.  A frame this device reads as calm can belong to
 *   an extreme moment.
 *
 *   The advice is attached to whichever msgId the cloud's pipeline holds when
 *   the text is ready, including one whose image never classified.  At
 *   14:52:59 an advice landed on msgId 54, whose image had 404ed three times
 *   and would answer only with a failure.
 *
 * Retiring on the result therefore threw away exactly the messages the advice
 * was about to arrive under -- counted afterwards as "a result for a retired
 * id", which is why the device almost never received one.  So every answered
 * entry is now held for CONFIG_VS_SOCIAL_ADVICE_GRACE_MS on the chance, and
 * promoted to CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS the moment there is a reason
 * to expect advice.  See social_inflight_hold().
 */

struct social_inflight_s
{
  char     msg_id[VS_CLOUD_MSG_ID_MAX];
  uint32_t alert_gen;

  /* A result for the msgEvent 0 side has arrived, of any kind -- an emotion or
   * a failure.  Says the cloud has answered about the image, not that the
   * answer was usable.
   */

  bool image_seen;

  /* This message belongs to an extreme moment, by either signal: the rule
   * reproduced from emotionDetail, or the cloud opening a msgEvent 1 slot.
   *
   * What it buys is the long deadline rather than the grace window.  It is not
   * a precondition for collecting advice any more -- the cloud's choice of
   * msgId does not respect it -- only a reason to wait longer.
   */

  bool extreme;

  /* Whether a msgEvent 1 entry has ever been seen for this msgId, in any
   * state.
   *
   * Both a promotion trigger and the answer to the one question the counters
   * could not settle when no advice arrives: did the cloud open a slot at all?
   * Measured 2026-09-08: ten expired waits, with no way to tell a cloud stuck
   * at 11 from a cloud that never answered msgEvent 1.
   */

  bool advice_seen;

  /* When the msgEvent 0 result arrived, for the log line that reports how long
   * a wait lasted.  Zero until it does.
   */

  uint64_t image_at_ms;

  /* When this entry was registered, for the same log line. */

  uint64_t added_at_ms;

  /* When to give up, absolute and monotonic.
   *
   * One deadline rather than a rule applied at each site, because the reasons
   * to hold an entry accumulate -- registered, answered, judged extreme, slot
   * observed -- and each one only ever extends it.  The interface defines no
   * terminal state for a msgEvent 1 that will not be answered, so a deadline
   * is the only thing that can end such a wait.
   */

  uint64_t retire_at_ms;
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

  /* The slowest interval seen to overflow the queue, plus one recovery step:
   * the fastest rate recovery is allowed to reach on its own.  0 until the
   * first drop, which means "no evidence yet, the configured interval stands".
   *
   * This exists because the interval alone cannot remember anything.  Recovery
   * walked to CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS every time, and since that
   * value is a wish rather than a measurement it overflowed on arrival, backed
   * off, and set out again -- see the drop path in social_image_push() for the
   * measurement.  Holding the failure point is what turns that sawtooth into a
   * settle.
   *
   * Written by the capture threads on a drop and by the uploader on a re-probe,
   * so it lives under the same lock as image_interval_ms.
   */

  uint32_t image_floor_ms;

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

  /* The same window split into the two round trips an upload is made of.
   *
   * Kept separately because only one of the two says anything about itself in
   * the log.  The transfer goes to the object store over TLS, which prints a
   * line per request; the registration is a cleartext call to the business
   * server that prints nothing, so a session whose uploads are slow looked the
   * same whichever half was responsible.  The only way to tell was to measure
   * the gaps between the store's own lines, which is not a thing a log should
   * require.
   */

  uint64_t upload_register_ms;
  uint64_t upload_transfer_ms;

  /* The transfer half again, split by what was in it, with the byte counts
   * that go with it.  This is a measurement, not a control input: nothing
   * reads it but the log line.
   *
   * It exists to settle one question that decides what is worth optimising
   * next.  A transfer's cost is either the bytes on the wire or the round trip
   * around them, and the two call for opposite work -- compress harder, or
   * overlap more requests -- so guessing wrong means building the wrong thing.
   * An image is about 12.9 KB and an audio chunk about 6 KB, so the same
   * measurement on both is a two-to-one byte ratio at a fixed request cost:
   *
   *   put(audio) ~ put(image)          latency-bound.  Concurrency helps,
   *                                    smaller frames do not.
   *   put(audio) ~ put(image) / 2      bandwidth-bound.  The reverse.
   *
   * The evidence so far is indirect and points at latency -- 26% smaller
   * frames bought 22% faster transfers, and an audio-heavy report window was
   * measured slower per item than an image-heavy one -- but that window figure
   * included the registration, so it cannot separate the two halves.  This
   * can.
   */

  uint64_t upload_image_put_ms;
  uint64_t upload_audio_put_ms;
  uint64_t upload_image_bytes;
  uint64_t upload_audio_bytes;
  uint32_t upload_image_count;
  uint32_t upload_audio_count;

  /* Messages awaiting results.
   *
   * Allocated per session from PSRAM rather than living here, and the reason is
   * the AP's SRAM budget rather than the size of the table.
   *
   * The kernel heap on this board is the tail of the 336 KiB RAM region --
   * __heap_start is placed immediately above .bss by the linker -- so every byte
   * of static data in this application is a byte the heap does not have.  It is
   * 17 KiB at boot, and it is the *only* heap that exists during bring-up:
   * bk7258_psram_initialize() adds the PSRAM region with kmm_addregion() at the
   * end of bk7258_pwc_start(), while the "pwc" kernel thread that gets it there
   * is created from the SRAM heap earlier in that same function.
   *
   * Measured 2026-09-08: growing this table from 16 to 24 entries added 512
   * bytes of .bss, took the boot heap from 17384 to 16872 bytes, and
   * kthread_create("pwc") then failed -- bring-up returned before PSRAM was ever
   * brought online, and CP reported "CPU1 boot timeout".  A social feature had
   * broken the board's boot by consuming heap it never touched.
   *
   * A pointer costs four bytes here and the table costs nothing until a session
   * runs, which is long after PSRAM is online.  Indexing is unchanged.
   */

  struct social_inflight_s *inflight;
  bool inflight_psram;
  uint8_t inflight_count;

  /* Entries thrown out because the table was full, and so never polled to a
   * conclusion.  This is the only counter that means "the device gave up on a
   * message it had no answer for".
   *
   * It used to also be incremented by social_advice_sweep(), and the totals
   * line printed the sum as "unanswered".  Those are opposite situations: a
   * swept entry has its emotion result and is being released from a wait for
   * the advice that follows it.  Measured 2026-09-07: the line read
   * "6 unanswered" in a session where the table peaked at 7 of 16 and so could
   * not have evicted anything -- all six were sweeps, and nothing at all went
   * unanswered.
   */

  uint32_t inflight_evicted;

  /* Alert debounce.  See the file header for what alert_gen is for. */

  uint32_t alert_gen;

  /* When the last SOCIAL_ALERT_NEEDED extreme frames arrived, as a ring.
   *
   * This replaced a plain consecutive counter, and the reason is arithmetic.
   * The counter required N extreme results in a row and was reset by any
   * result that was not extreme -- but the stream it counts is neither dense
   * nor clean.  Measured 2026-09-07: 43 images uploaded produced 7 emotion
   * results, one every 9.6 s on average.  "Three in a row" therefore asked for
   * anger to be detected continuously across about nineteen seconds with not a
   * single misclassification in between, and one stray green in the middle of a
   * genuine argument put the count back to zero.
   *
   * A window asks the question that was meant instead: were there N extreme
   * readings within CONFIG_VS_SOCIAL_ALERT_WINDOW_MS.  A calm reading no longer
   * erases the evidence, it just stops adding to it, and the window expiring is
   * what forgets.
   */

  uint64_t extreme_at[SOCIAL_ALERT_STAMP_MAX];
  uint8_t  extreme_head;
  uint8_t  extreme_have;
  uint8_t  calm_streak;
  bool     alert_active;
  uint64_t alert_since_ms;

  /* What the cloud actually answered, counted so a session can be read as a
   * whole rather than reconstructed from the per-occurrence lines.
   *
   * These exist because the previous session could not be accounted for from
   * its log.  It showed about fifty extreme readings and ended with a summary
   * claiming the conversation was 97% calm, and there was no way to tell
   * whether the device had seen fifty frames or the same few frames fifty
   * times.  results_repeated answers exactly that; failed_results is the
   * "no usable face" count the cloud's own yield question needs, which its
   * status 30 otherwise hides.
   */

  uint32_t emotion_results;   /* msgEvent 0 results folded in, once each */
  uint32_t emotion_extreme;   /* of those, extreme by either signal */
  uint32_t emotion_low_conf;  /* extreme, but under the confidence floor */

  /* How the two extreme signals compare, which is the only way to tell whether
   * the out-of-band rule still matches what the cloud is doing.
   *
   * by_slot counts frames the document's signal caught and the local rule did
   * not -- so a rising by_slot means the rule reproduced in
   * cloud_classify_emotion() has drifted from the cloud's, and the red bucket
   * and the three emotions it names both need revisiting.  Zero means they
   * agree.
   *
   * late is the subset of those that arrived a poll too late to raise on; see
   * the VS_CLOUD_PEER_ADVICE_PENDING arm.
   */

  uint32_t emotion_extreme_slot;
  uint32_t emotion_extreme_late;

  /* Results for an id no longer tracked.  See the unmatched arm of the poll
   * loop: this is where a cloud signal can still go missing.
   */

  uint32_t poll_unmatched;
  uint32_t results_repeated;  /* results the cloud re-sent, ignored here */
  uint32_t failed_results;    /* peer status 30, whatever its real reason */
  uint32_t alerts_raised;

  /* What became of the advice for every extreme frame.  Four outcomes, counted
   * apart, because one "missed" figure covering all of them could not say
   * whether the feature was failing on the device or simply not being fed.
   *
   * Measured 2026-09-07: "advice 0 delivered / 7 missed" was, in fact, one
   * advice sent by the cloud and thrown away here, plus six frames the cloud
   * was never going to advise on -- it advises once per extreme run, on the
   * last frame of it, not once per extreme frame.  Read as six cloud
   * failures it pointed at the wrong side entirely; discarded is the number to
   * act on.
   */

  uint32_t advice_delivered;  /* reached the screen */
  uint32_t advice_discarded;  /* arrived, but nothing left to attach it to */
  uint32_t advice_refused;    /* status 30 on the wait: cloud says never */
  uint32_t advice_expired;    /* VS_SOCIAL_ADVICE_TIMEOUT_MS elapsed */

  /* Where the delivered advice came from, which is the measurement that says
   * whether the grace window was worth its table slots.
   *
   * grace counts advice collected on a message this device had no reason to
   * expect any from -- one it read as calm, or one whose image failed outright.
   * Under the previous code every one of these was lost, so a non-zero figure
   * here is the fix working and the size of it is how wrong the old assumption
   * was.  See social_inflight_s for why the cloud does this.
   *
   * rescued is the narrower case of an advice that arrived in the same batch as
   * the result that retired its message, and so was already unmatched by the
   * time it was read.  Delivered from the event itself rather than from a table
   * entry; see the unmatched arm of the poll loop.
   *
   * quiet counts delivery with no alert standing.  Not an error -- the advice is
   * the scarce output and is shown regardless -- but it is the number that says
   * the alert lifetime and the cloud's latency still do not line up.
   */

  uint32_t advice_from_grace;
  uint32_t advice_rescued;
  uint32_t advice_quiet;

  /* Speculative holds that ran out, and messages the cloud never answered at
   * all.  Split from advice_expired, which now means only "an entry that was
   * expecting advice did not get it" -- the one figure that reflects on the
   * cloud.  Together with it these account for every entry the sweep removes.
   */

  uint32_t grace_expired;
  uint32_t image_unanswered;

  /* Polls on which the alert would have been released but for an outstanding
   * advice.  Bounded by CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS, so a large value
   * means many alerts deferred rather than one deferred forever.
   */

  uint32_t release_deferred;

  /* Whether the cloud ever opened an advice slot, which the four counters
   * above cannot say.  All of them describe a slot that reached a terminal
   * state; none of them fires for one that stays at status 11, and status 11 is
   * what the cloud appears to do.  Measured 2026-09-08: 0 delivered, 0
   * discarded, 0 refused, 10 expired -- consistent both with a cloud stuck at
   * pending and with a cloud that never answers msgEvent 1, and there was no
   * way to choose between them.
   *
   * slots is per frame and is the number to read; pending is every arrival, so
   * it grows once per poll per waiting entry and only confirms the poll really
   * is receiving them.
   */

  uint32_t advice_slots_seen;
  uint32_t advice_pending_seen;

  /* Folded results that carried nothing to classify.
   *
   * These are counted because emotion_results and the minutes' own sample count
   * are not the same measurement, and comparing them was misleading.
   * cloud_summarize_timeline() drops a timeline entry whose colour does not map
   * -- "an unrecognised colour is not evidence of anything" -- while this fold
   * path has no such test, so a status 20 with no response at all becomes one
   * more non-extreme result here and no result at all there.
   *
   * Measured 2026-09-08, session vs-264-c01a7208: 68 folded results with 12
   * extreme against a timeline of 114 entries with 51 extreme.  Those rates,
   * 17.6% and 44.7%, cannot both describe the same frames; if roughly forty of
   * the sixty-eight carried no emotion the two agree.  blind is that number.
   *
   * no_response is the subset where the response object was absent entirely,
   * as opposed to present with a colour outside the palette.
   */

  uint32_t emotion_blind;
  uint32_t emotion_no_response;

  /* What the poll's deference to registrations actually did.  See the block in
   * social_poll_once() that reads them.
   *
   * Written from the session thread only, so unlocked increments there are
   * safe; read by social_log_totals() on the same thread.
   */

  uint32_t poll_yielded;      /* cycles given up so a register could go first */
  uint32_t poll_forced;       /* busy, but out of yields, so it went anyway */

  /* When the last extreme frame arrived, which is what releases an alert that
   * the calm streak cannot.
   *
   * The streak needs SOCIAL_ALERT_NEEDED consecutive frames the cloud judged
   * calm, and calm results are not most of what a session receives.  A frame
   * with no usable face comes back as status 30 and is retired without touching
   * either streak -- the ordinary case, per social_poll_once() -- and a frame
   * whose result never arrives at all is retired by social_inflight_add()'s
   * eviction.  Measured 2026-09-08: 72 images uploaded produced 25 results of
   * which 42 were status 30, so a run of consecutive calm results is a
   * coincidence rather than an expectation even now that the run is short.
   *
   * The consequence was that an alert raised once stayed on screen for the rest
   * of the session however calm the conversation became.  So "no longer
   * extreme" is also expressed the way it is actually observable: nothing
   * extreme for CONFIG_VS_SOCIAL_ALERT_HOLD_MS.  The streak stays as the fast
   * path for when the cloud does report calm faces.
   */

  uint64_t alert_extreme_ms;

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

  /* More than one, because an upload is two serial round trips to two
   * different hosts and one worker can only be inside one of them at a time.
   *
   * Measured 2026-09-07: 984 ms per item against a design budget of two
   * pooled round trips at about 135 ms each.  The item cost is also very
   * nearly independent of payload size -- an image-heavy report window
   * averaged 869 ms and an audio-heavy one, carrying a quarter of the bytes,
   * averaged 1636 ms -- so the limit is per-request latency rather than
   * bandwidth, and latency is what overlapping requests hides.
   *
   * The registration halves still take turns, because they share one cleartext
   * connection and vs_cloud.c serializes it.  What overlaps is a registration
   * against another item's transfer, which is the half that goes to the object
   * store over TLS.  See CONFIG_VS_SOCIAL_UPLOAD_WORKERS for why two is the
   * ceiling.
   */

  pthread_t upload_thread[CONFIG_VS_SOCIAL_UPLOAD_WORKERS];
  uint8_t   upload_count;
  bool      capture_joinable;
  bool      audio_joinable;
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

static void social_post_full(enum vs_app_event_e type, int error,
                             enum vs_emotion_e emotion, uint32_t color,
                             bool extreme, const char *text)
{
  struct vs_app_event_s event;
  unsigned int attempt;

  memset(&event, 0, sizeof(event));
  event.type       = type;
  event.request_id = g_social.request_id;
  event.error      = error;
  event.emotion    = emotion;
  event.color      = color;
  event.extreme    = extreme;

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

/* The common case, where the event carries no emotion verdict.
 *
 * A wrapper rather than an extra argument at every call site: only the two
 * emotion-bearing events have anything to say about extreme, and the other
 * fourteen callers would have had to pass a literal false to express it.
 */

static void social_post(enum vs_app_event_e type, int error,
                        enum vs_emotion_e emotion, uint32_t color,
                        const char *text)
{
  social_post_full(type, error, emotion, color, false, text);
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
                              uint32_t alert_gen, uint64_t stamp_ms)
{
  struct social_slot_s *slot = &ring->slot[ring->write];

  slot->type       = type;
  slot->data       = data;
  slot->len        = len;
  slot->from_psram = from_psram;
  slot->sequence   = sequence;
  slot->alert_gen  = alert_gen;
  slot->stamp_ms   = stamp_ms;

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

  /* Before the lock, so a wait on it does not become part of the timestamp. */

  uint64_t stamp = social_now_ms();

  pthread_mutex_lock(&g_social.lock);

  evicted = social_ring_make_room(&g_social.audio, &evicted_psram);
  if (evicted != NULL || g_social.audio.count >= g_social.audio.cap)
    {
      g_social.dropped_audio++;
      total = g_social.dropped_audio;
      dropped = true;
    }

  social_ring_store(&g_social.audio, VS_CLOUD_MEDIA_AUDIO, data, len,
                    from_psram, sequence, alert_gen, stamp);
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

  /* Before the lock, so a wait on it does not become part of the timestamp.
   * Doubles as the drop-report clock below, which wanted the same instant.
   */

  uint64_t now = social_now_ms();

  pthread_mutex_lock(&g_social.lock);

  evicted = social_ring_make_room(&g_social.image, &evicted_psram);
  if (evicted != NULL)
    {
      g_social.dropped_image++;

      /* Back off, and reset the recovery streak: the run of clean uploads
       * that would have earned a step back up has just been interrupted.
       */

      g_social.image_ok_streak = 0;

      /* Remember the rate that could not be sustained, one step slower than
       * the one that just failed.  This is what stops the recovery walking
       * straight back into the same overflow, and it is the difference between
       * a loop that converges and one that oscillates for the whole session.
       *
       * Measured 2026-09-07, without it: three full sawtooths in 79 s.  The
       * interval doubled to 2000 ms on a drop, stepped 250 ms at a time back
       * down to the configured 500 ms, overflowed again on arrival, and
       * repeated.  Mean delivered rate 0.76 images/s against a 2 fps target,
       * six frames lost, and the sampler spent most of the session in the slow
       * half of its range because that is where a sawtooth spends its time.
       * The link's actual capacity was about one image every 1250 ms
       * throughout -- a figure the loop rediscovered and forgot three times.
       */

      /* One step slower than the interval that failed, so the floor is a rate
       * the link has not refused rather than the one it just did.  A plain
       * assignment: the interval is never below the floor -- a re-probe lowers
       * the floor first and lets the interval follow -- so this only ever
       * ratchets upwards, which is what "worst seen" has to do.
       */

      {
        uint32_t bound = g_social.image_interval_ms +
                         SOCIAL_IMAGE_RECOVER_STEP_MS;

        g_social.image_floor_ms =
          bound > CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS ?
          CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MAX_MS : bound;
      }

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
                    from_psram, sequence, alert_gen, now);
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

/* True when this entry has a reason to expect advice and is still waiting for
 * it.  Called with the lock held.
 *
 * Two readers, and they want the same question answered.  The eviction below
 * will not throw such an entry away while it has an alternative, because that
 * entry is the only thing that can collect what the cloud produces for an
 * extreme moment.  And social_alert_release_stale() will not clear an alert
 * while one exists, because an advice arriving after its alert has gone has
 * nothing on screen to attach to and is discarded.
 *
 * That second reader was removed once, on the measurement that it held an alert
 * 18.6 s past its hold for advice that never came, and reinstating it is part
 * of the advice fix.  The measurement was real but the conclusion was wrong in
 * one respect: with the hold shorter than the advice latency, *every* advice
 * arrived to a cleared alert.  The bound that makes the wait safe is
 * CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS via social_advice_sweep(), which runs
 * ahead of the release on each poll -- so an entry whose advice has timed out
 * stops holding the alert in the same pass rather than the next one.
 */

static bool social_inflight_waiting(const struct social_inflight_s *entry)
{
  return entry->extreme || entry->advice_seen;
}

/* True when this entry is answered and is only being kept on the chance that
 * the cloud attaches an advice to it.  Called with the lock held.
 *
 * These are the speculative ones, and telling them apart matters in exactly one
 * place: they are what eviction takes first among answered entries, because
 * they are the answered entries least likely to still produce anything.
 */

static bool social_inflight_grace(const struct social_inflight_s *entry)
{
  return entry->image_seen && !social_inflight_waiting(entry);
}

/* Extend this entry's deadline to whatever its current state entitles it to.
 * Called with the lock held.
 *
 * Only ever forward: the reasons to keep an entry accumulate and none of them
 * revokes an earlier one, so a promotion cannot be undone by a later result
 * that happens to arrive with a shorter entitlement.
 */

static void social_inflight_hold(struct social_inflight_s *entry, uint64_t now)
{
  uint64_t want = now + (social_inflight_waiting(entry) ?
                         (uint64_t)CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS :
                         (uint64_t)CONFIG_VS_SOCIAL_ADVICE_GRACE_MS);

  if (want > entry->retire_at_ms)
    {
      entry->retire_at_ms = want;
    }
}

/* Drop one entry by index.  Called with the lock held. */

static void social_inflight_remove(uint8_t index);

/* Called with the lock held. */

static void social_inflight_add(const char *msg_id, uint32_t alert_gen)
{
  struct social_inflight_s *entry;
  uint64_t now = social_now_ms();

  /* The table is allocated by the session thread and this runs on an upload
   * worker, which that thread starts only after the allocation succeeded and
   * joins before releasing it.  So this cannot fire -- and it is here anyway,
   * because the cost is one compare against writing 72 bytes through a null
   * pointer if that ordering is ever changed.
   */

  if (g_social.inflight == NULL)
    {
      return;
    }

  if (g_social.inflight_count == CONFIG_VS_SOCIAL_INFLIGHT_MAX)
    {
      uint8_t victim = 0;
      bool sacrificed = true;
      uint8_t i;

      /* Evicted by what the entry can still produce, worst first, and within a
       * class by age -- the array is in arrival order, so the first match is
       * the oldest of its kind.  Three passes rather than one comparison
       * because the classes are not orderable by a single field.
       *
       *   past its deadline   nothing; it is about to be swept anyway
       *   grace only          a speculative wait for advice that may not come
       *   no result yet       one frame's emotion, and the cloud still counts
       *                       it in the end-of-session timeline
       *   waiting for advice  the only output an extreme moment produces
       *
       * This used to be an unconditional "drop index 0", justified in a comment
       * that said it lost one frame's emotion result and nothing more.  Against
       * this interface that was wrong: an advice is delivered under an image's
       * msgId, so the entry waiting longest for its advice is exactly the entry
       * a first-in-first-out eviction removes -- and once removed the id is no
       * longer polled at all, so the advice is never even fetched.
       */

      for (i = 0; i < g_social.inflight_count; i++)
        {
          if (now >= g_social.inflight[i].retire_at_ms)
            {
              victim = i;
              sacrificed = false;
              break;
            }
        }

      if (sacrificed)
        {
          for (i = 0; i < g_social.inflight_count; i++)
            {
              if (social_inflight_grace(&g_social.inflight[i]))
                {
                  victim = i;
                  sacrificed = false;
                  break;
                }
            }
        }

      if (sacrificed)
        {
          for (i = 0; i < g_social.inflight_count; i++)
            {
              if (!g_social.inflight[i].image_seen &&
                  !social_inflight_waiting(&g_social.inflight[i]))
                {
                  victim = i;
                  sacrificed = false;
                  break;
                }
            }
        }

      if (sacrificed)
        {
          /* Every slot is waiting for advice, so one of them has to go.  Worth
           * a line: it means the table is too small for the rate at which this
           * conversation is producing extreme moments, and an advice is being
           * abandoned rather than merely delayed.
           */

          printf("%s: inflight full of pending advice (%u slots), abandoning "
                 "msg %s after %lu ms\n", SOCIAL_TAG,
                 (unsigned)CONFIG_VS_SOCIAL_INFLIGHT_MAX,
                 g_social.inflight[0].msg_id,
                 (unsigned long)(now - g_social.inflight[0].added_at_ms));
        }

      social_inflight_remove(victim);
      g_social.inflight_evicted++;
    }

  entry = &g_social.inflight[g_social.inflight_count++];
  memset(entry, 0, sizeof(*entry));
  snprintf(entry->msg_id, sizeof(entry->msg_id), "%s", msg_id);
  entry->alert_gen   = alert_gen;
  entry->added_at_ms = now;

  /* The full timeout before any result has arrived, not the grace window.  The
   * grace window is what an *answered* message gets; this one is still waiting
   * to be answered, and an image result was measured arriving up to twelve
   * seconds after its upload.  A deadline shorter than that would retire
   * messages the cloud was still working on.
   */

  entry->retire_at_ms = now + (uint64_t)CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS;
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
      packet.type        = slot.type;
      packet.data        = slot.data;
      packet.len         = slot.len;
      packet.sequence    = slot.sequence;
      packet.produced_ms = slot.stamp_ms;

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
        uint64_t reg_window = 0;
        uint64_t put_window = 0;
        uint8_t queued_audio = 0;
        uint8_t queued_image = 0;
        uint8_t inflight = 0;
        uint32_t recovered = 0;
        uint32_t reprobed = 0;
        bool sped_up = false;

        pthread_mutex_lock(&g_social.lock);
        g_social.uploaded++;
        g_social.upload_window_ms   += spent_ms;
        g_social.upload_register_ms += result.register_ms;
        g_social.upload_transfer_ms += result.transfer_ms;

        /* Only a transfer that actually happened.  payload_sent is false for
         * the mock cloud's untransferable URL, and counting a skipped PUT as a
         * 0 ms one would report the link as faster than it is.
         */

        if (result.payload_sent)
          {
            if (slot.type == VS_CLOUD_MEDIA_IMAGE)
              {
                g_social.upload_image_put_ms += result.transfer_ms;
                g_social.upload_image_bytes  += slot.len;
                g_social.upload_image_count++;
              }
            else
              {
                g_social.upload_audio_put_ms += result.transfer_ms;
                g_social.upload_audio_bytes  += slot.len;
                g_social.upload_audio_count++;
              }
          }

        /* Images only.  An audio msgId cannot answer anything this session
         * acts on -- the cloud attaches an extreme frame's advice to the
         * image's msgId, not to any audio one -- so tracking it spent a slot
         * that the images waiting for that advice needed.  See
         * social_inflight_s.
         */

        if (slot.type == VS_CLOUD_MEDIA_IMAGE)
          {
            social_inflight_add(result.msg_id, slot.alert_gen);
          }

        /* An image that made it through is the evidence that the link has room
         * again, so this is where the sampler is allowed back up.  Only images
         * count: audio uploads happen whatever the frame rate, so crediting
         * them would let a silent-but-congested link talk the sampler into
         * speeding up on the strength of traffic that was never the problem.
         *
         * A run rather than a single success, and a step rather than a halving,
         * because the cost of guessing wrong in this direction is another drop.
         *
         * Two different moves, and the distinction is the whole fix.  Above the
         * remembered congestion point the interval steps down towards it, which
         * is walking back into territory known to work.  Once it has arrived
         * there the only way forward is to move the congestion point itself,
         * which is a guess about territory known to have failed -- so it costs
         * SOCIAL_IMAGE_REPROBE_STREAK clean uploads rather than
         * SOCIAL_IMAGE_RECOVER_STREAK.  Without that asymmetry the floor is
         * re-probed as cheaply as it is approached and the loop is back to
         * sawtoothing; with it, capacity is rediscovered slowly and held.
         */

        if (slot.type == VS_CLOUD_MEDIA_IMAGE)
          {
            uint32_t floor =
              g_social.image_floor_ms > CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS ?
              g_social.image_floor_ms : CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;
            bool at_floor = g_social.image_interval_ms <= floor;
            uint8_t needed = at_floor ? SOCIAL_IMAGE_REPROBE_STREAK :
                                        SOCIAL_IMAGE_RECOVER_STREAK;

            if (++g_social.image_ok_streak >= needed)
              {
                g_social.image_ok_streak = 0;

                if (!at_floor)
                  {
                    uint32_t next = g_social.image_interval_ms;

                    next = next > SOCIAL_IMAGE_RECOVER_STEP_MS + floor ?
                           next - SOCIAL_IMAGE_RECOVER_STEP_MS : floor;

                    g_social.image_interval_ms = next;
                    recovered = next;
                    sped_up = true;
                  }
                else if (floor > CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS)
                  {
                    /* Open the floor by one step.  The interval follows on the
                     * next run, so a re-probe that was wrong costs one step of
                     * rate and one drop, not a return to the ceiling.
                     */

                    g_social.image_floor_ms =
                      floor > SOCIAL_IMAGE_RECOVER_STEP_MS +
                              CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS ?
                      floor - SOCIAL_IMAGE_RECOVER_STEP_MS :
                      CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;

                    reprobed = g_social.image_floor_ms;
                  }
              }
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
            reg_window   = g_social.upload_register_ms;
            put_window   = g_social.upload_transfer_ms;
            queued_audio = g_social.audio.count;
            queued_image = g_social.image.count;
            inflight     = g_social.inflight_count;

            g_social.upload_reported    = g_social.uploaded;
            g_social.upload_window_ms   = 0;
            g_social.upload_register_ms = 0;
            g_social.upload_transfer_ms = 0;
          }

        pthread_mutex_unlock(&g_social.lock);

        if (report)
          {
            /* The total and its two halves on one line.  With more than one
             * worker the total is wall time per item summed across workers, so
             * it can exceed the elapsed time -- what it measures is how busy
             * the uploaders were, and reg against put is which half they spent
             * it in.
             */

            printf("%s: uploaded %lu, last %lu averaged %lu ms "
                   "(reg %lu + put %lu), audio %u/%u image %u/%u, "
                   "inflight %u\n", SOCIAL_TAG,
                   (unsigned long)total, (unsigned long)since,
                   (unsigned long)(since > 0 ? window / since : 0),
                   (unsigned long)(since > 0 ? reg_window / since : 0),
                   (unsigned long)(since > 0 ? put_window / since : 0),
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

        if (reprobed != 0)
          {
            /* Worth its own line: this is the loop deciding the link may have
             * got faster, which is the only decision here that can cause a
             * drop.  A session full of these next to a rising drop count means
             * SOCIAL_IMAGE_REPROBE_STREAK is too cheap.
             */

            printf("%s: image rate floor re-probed to %lu ms (%.2f fps)\n",
                   SOCIAL_TAG, (unsigned long)reprobed,
                   1000.0 / (double)reprobed);
          }
      }
    }

  return NULL;
}

/****************************************************************************
 * Emotion debounce
 ****************************************************************************/

/****************************************************************************
 * Name: social_download_cancelled
 *
 * Description:
 *   Whether a download in progress should stop, asked by vs_cloud's body sink
 *   once per record.
 *
 *   This is how an abort reaches a transfer.  vs_social_abort() runs on the UI
 *   thread and can wake the camera and the microphone, but a read already in
 *   flight is not something it can interrupt -- so before this existed, an
 *   abort during the spoken-minutes fetch did nothing observable until the
 *   transfer ended on its own.  Measured 2026-09-08 that was 193.7 s, and the
 *   session's totals stayed unprinted for all of it because the thread that
 *   prints them was inside the read.
 *
 *   Deliberately not taking g_social.lock, for two reasons.  It runs once per
 *   record on a transfer that may be megabytes, and the lock it would be taking
 *   is the one the poll loop and both upload workers are already contending
 *   for.  More importantly it cannot be known from here whether some future
 *   caller starts a download with the lock held, and this mutex is not
 *   recursive, so a lock here would turn that into a deadlock instead of a
 *   review comment.
 *
 *   The read is of one bool that only ever goes false to true within a session,
 *   so both answers are safe: a stale false costs one more record before the
 *   next call asks again, and no interleaving can produce a spurious true.
 *   Read through a volatile lvalue so the load is guaranteed to happen rather
 *   than being assumed loop-invariant -- the indirect call from vs_cloud.c
 *   already forces it in practice, but that is a property of the call site
 *   rather than of this function, and this is where the guarantee belongs.
 *
 ****************************************************************************/

static bool social_download_cancelled(void)
{
  return *(const volatile bool *)&g_social.abort;
}

/****************************************************************************
 * Name: social_advice_slot_in_batch
 *
 * Description:
 *   Whether this batch of results carries a msgEvent 1 entry for msg_id.
 *
 *   This is the interface document's own way of saying "I judged that frame
 *   extreme", and it is the strongest signal the device has.  The cloud opens
 *   an advice slot under the *image's* msgId as soon as it decides, and the
 *   document's debug example shows the pair arriving together:
 *
 *     // 返回图片结果，但是极端情绪的建议未生成
 *     {msgId:1, status:20, msgEvent:0, response:{red, 生气, 0.95}}
 *     {msgId:1, status:11, msgEvent:1}
 *
 *   Why this rather than the emotion detail.  The document never states which
 *   emotions are extreme -- it only enumerates the palette -- so the detail
 *   test in cloud_classify_emotion() encodes a rule that arrived out of band
 *   and can change on the cloud without the wire format changing at all.  This
 *   cannot: whatever the rule becomes, the slot appearing means the cloud
 *   applied it and answered yes.
 *
 *   Any state counts, not just 11.  A slot that has already failed (status 30,
 *   "音频无有效声音") or already answered (21) is still a slot that was opened,
 *   which is still the cloud having judged the frame extreme.
 *
 *   Scanned rather than pre-collected because the ordering of entries within a
 *   batch is not specified: the image result may be processed before or after
 *   its slot appears in the same array.  A scan is order-independent, and the
 *   array is at most two entries per polled id.
 *
 ****************************************************************************/

static bool social_advice_slot_in_batch(
                             const struct vs_social_event_s *events,
                             size_t count, const char *msg_id)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      if (events[i].msg_event == VS_CLOUD_MSG_EVENT_AUDIO &&
          strcmp(events[i].msg_id, msg_id) == 0)
        {
          return true;
        }
    }

  return false;
}

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
  uint64_t now = social_now_ms();

  *raise = false;
  *clear = false;

  if (event->extreme)
    {
      uint64_t oldest;

      g_social.calm_streak = 0;
      g_social.alert_extreme_ms = now;

      /* A reading the cloud is not sure about does not count towards raising,
       * but it does count as "still extreme" above -- so a run of low
       * confidence reds holds an existing alert open without being able to
       * start one.  That asymmetry is deliberate: raising interrupts the user,
       * releasing does not.
       *
       * 101 is "the cloud sent no confidence at all", and it has to pass.  A
       * server that omits the field must not silently disable the feature.
       */

      if (event->confidence <= 100 &&
          event->confidence < CONFIG_VS_SOCIAL_ALERT_MIN_CONFIDENCE)
        {
          g_social.emotion_low_conf++;
          printf("%s: extreme frame ignored, confidence %u below %d\n",
                 SOCIAL_TAG, (unsigned)event->confidence,
                 CONFIG_VS_SOCIAL_ALERT_MIN_CONFIDENCE);
          return true;
        }

      /* Record it, then ask whether the oldest of the last N is still inside
       * the window.  After the head advances it indexes the oldest of them,
       * because that is the slot the next write will overwrite.
       */

      g_social.extreme_at[g_social.extreme_head] = now;
      g_social.extreme_head =
        (uint8_t)((g_social.extreme_head + 1) % SOCIAL_ALERT_NEEDED);
      if (g_social.extreme_have < SOCIAL_ALERT_NEEDED)
        {
          g_social.extreme_have++;
        }

      oldest = g_social.extreme_at[g_social.extreme_head];

      if (!g_social.alert_active &&
          g_social.extreme_have >= SOCIAL_ALERT_NEEDED &&
          now - oldest <= CONFIG_VS_SOCIAL_ALERT_WINDOW_MS)
        {
          g_social.alert_active   = true;
          g_social.alert_since_ms = now;

          /* A new alert invalidates advice still in flight for the previous
           * one.  See the file header.
           */

          g_social.alert_gen++;
          g_social.alerts_raised++;
          *raise = true;

          printf("%s: alert raised, %d extreme frame(s) within %lu ms, "
                 "confidence %u, %s\n", SOCIAL_TAG, SOCIAL_ALERT_NEEDED,
                 (unsigned long)(now - oldest), (unsigned)event->confidence,
                 event->display_text[0] != '\0' ? event->display_text : "-");
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

  /* Not extreme.  The extreme ring is deliberately left alone -- see its
   * declaration -- so a single misread does not erase a real run.
   */

  if (g_social.calm_streak < 255)
    {
      g_social.calm_streak++;
    }

  if (g_social.alert_active &&
      g_social.calm_streak >= SOCIAL_ALERT_NEEDED &&
      now - g_social.alert_since_ms >= CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS)
    {
      g_social.alert_active = false;
      g_social.alert_gen++;
      *clear = true;

      /* Forget the readings that raised this one.  Without it the ring still
       * holds them, so a single extreme frame arriving just after a clear finds
       * the count already satisfied and raises again immediately -- which is the
       * debounce this window replaced, defeated from the other end.
       */

      g_social.extreme_have = 0;
      g_social.extreme_head = 0;

      printf("%s: alert cleared, %u calm frame(s)\n", SOCIAL_TAG,
             (unsigned)g_social.calm_streak);
    }

  return true;
}

/****************************************************************************
 * Name: social_alert_release_stale
 *
 * Description:
 *   Clear an alert that nothing has renewed, and say whether it did.
 *
 *   The other clear path lives inside social_emotion_step(), which only runs
 *   when a frame's emotion result arrives.  That makes it unreachable in the
 *   case that matters most: a user who has calmed down and looked away produces
 *   neither extreme results nor calm ones, only "no usable face", so the alert
 *   it raised had nothing left that could take it down.  See
 *   social_state_s::alert_extreme_ms for the measurement.
 *
 *   Called from the poll loop rather than from the result handler, so it runs
 *   on a timer the cloud cannot withhold.  Takes the lock itself because that
 *   loop releases it around each event.
 *
 * Returned Value:
 *   true when this call ended an alert, and the caller should post
 *   VS_APP_EVENT_SOCIAL_ALERT_CLEARED.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: social_advice_sweep
 *
 * Description:
 *   Retire every entry whose deadline has passed, whatever it was holding on
 *   for.
 *
 *   The interface defines no terminal state for a msgEvent 1 that will not be
 *   answered -- it goes to 11 and, if nothing comes, stays there -- so without a
 *   bound here an entry expecting advice is held for the rest of the session.
 *   Answered entries kept on speculation for CONFIG_VS_SOCIAL_ADVICE_GRACE_MS
 *   need the same treatment for the same reason, and messages the cloud never
 *   answers at all need it too.  One deadline per entry covers all three; see
 *   social_inflight_hold().
 *
 *   The line it prints is the one to read when no advice arrives, because it
 *   names which of the two situations happened: "slot seen" is a cloud that
 *   opened a msgEvent 1 entry and then produced nothing, no slot is a message
 *   the cloud was never going to advise on.  The previous version could not
 *   tell those apart and the counters blamed the wrong side for both.
 *
 *   Runs on the poll loop, taking the lock itself.
 *
 ****************************************************************************/

static void social_advice_sweep(void)
{
  uint64_t now = social_now_ms();
  uint8_t i = 0;

  pthread_mutex_lock(&g_social.lock);

  while (i < g_social.inflight_count)
    {
      struct social_inflight_s *entry = &g_social.inflight[i];

      if (now >= entry->retire_at_ms)
        {
          char msg_id[VS_CLOUD_MSG_ID_MAX];
          bool waiting = social_inflight_waiting(entry);
          bool answered = entry->image_seen;
          bool slot = entry->advice_seen;
          bool extreme = entry->extreme;
          unsigned long held = (unsigned long)(now - entry->added_at_ms);

          /* Everything the log line needs is copied out first: the removal
           * memmoves the tail down, so entry stops describing this message the
           * moment it returns.
           */

          snprintf(msg_id, sizeof(msg_id), "%s", entry->msg_id);
          social_inflight_remove(i);

          /* Counted apart, because they are different failures.  An entry that
           * was expecting advice and did not get it is the feature not working;
           * one that merely ran out its speculative window is the ordinary
           * case, and lumping them together made the advice numbers unreadable.
           * An entry the cloud never answered at all is a third thing again.
           */

          if (waiting)
            {
              g_social.advice_expired++;
            }
          else if (answered)
            {
              g_social.grace_expired++;
            }
          else
            {
              g_social.image_unanswered++;
            }

          pthread_mutex_unlock(&g_social.lock);

          if (waiting)
            {
              printf("%s: no advice for msg %s after %lu ms (%s, extreme %s)\n",
                     SOCIAL_TAG, msg_id, held,
                     slot ? "slot seen, cloud produced nothing" :
                            "no msgEvent 1 slot ever opened",
                     extreme ? "yes" : "no");
            }
          else if (!answered)
            {
              printf("%s: msg %s never answered, %lu ms after upload\n",
                     SOCIAL_TAG, msg_id, held);
            }

          pthread_mutex_lock(&g_social.lock);

          /* The removal shifted the tail down, so this index has to be looked
           * at again rather than advanced past.
           */

          continue;
        }

      i++;
    }

  pthread_mutex_unlock(&g_social.lock);
}

static bool social_alert_release_stale(void)
{
  uint64_t now = social_now_ms();
  bool cleared = false;
  bool deferred = false;

  pthread_mutex_lock(&g_social.lock);

  /* All three bounds have to pass, and they mean different things.  The
   * cooldown is the readability floor the alert is entitled to whatever happens
   * next; the hold is how long the absence of an extreme frame is allowed to be
   * read as "over" rather than as "between frames"; and the outstanding advice
   * is the one thing the alert still owes the user.
   *
   * The third was absent, and that was the whole of "the device almost never
   * shows an advice".  The cloud's advice arrives six to eighteen seconds after
   * the moment (AI-side log, 2026-09-08) against a twelve-second hold, so the
   * alert had usually gone by the time the text existed -- and the delivery test
   * requires a standing alert, so every one of those arrivals was discarded.
   *
   * Bounded by social_advice_sweep(), which runs immediately before this on
   * every poll: an entry past CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS is gone by the
   * time this looks, so the deferral cannot outlast that.
   */

  if (g_social.alert_active &&
      now - g_social.alert_since_ms >= CONFIG_VS_SOCIAL_ALERT_COOLDOWN_MS &&
      now - g_social.alert_extreme_ms >= CONFIG_VS_SOCIAL_ALERT_HOLD_MS)
    {
      uint8_t i;

      for (i = 0; i < g_social.inflight_count; i++)
        {
          if (social_inflight_waiting(&g_social.inflight[i]))
            {
              deferred = true;
              break;
            }
        }

      if (deferred)
        {
          g_social.release_deferred++;
        }
      else
        {
          g_social.alert_active = false;

          /* Bumped for the same reason the other two paths bump it: advice
           * still in flight for this alert would contradict a screen that has
           * moved on.
           */

          g_social.alert_gen++;
          g_social.extreme_have = 0;
          g_social.extreme_head = 0;
          cleared = true;
        }
    }

  pthread_mutex_unlock(&g_social.lock);

  if (cleared)
    {
      printf("%s: alert released, no extreme frame for %d ms\n", SOCIAL_TAG,
             CONFIG_VS_SOCIAL_ALERT_HOLD_MS);
    }
  else if (deferred)
    {
      /* Every poll while it defers, which is at most
       * CONFIG_VS_SOCIAL_ADVICE_TIMEOUT_MS / CONFIG_VS_SOCIAL_POLL_INTERVAL_MS
       * lines per alert.  Worth that: an alert staying up past its hold is
       * visible to the user and this is the only thing that explains it.
       */

      printf("%s: alert held past %d ms, advice still outstanding\n",
             SOCIAL_TAG, CONFIG_VS_SOCIAL_ALERT_HOLD_MS);
    }

  return cleared;
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

  /* Both of these run above every early return below, because the cases they
   * exist for are exactly the ones where there is nothing to poll: an alert
   * raised, the conversation calmed, and no further result of any kind
   * arriving.  Putting them after the "nothing in flight" return would have
   * left them unreachable precisely when they were needed.
   *
   * The sweep goes first so that an entry whose advice has timed out stops
   * holding the release back in this same pass.
   */

  social_advice_sweep();

  if (social_alert_release_stale())
    {
      social_post(VS_APP_EVENT_SOCIAL_ALERT_CLEARED, 0, VS_EMOTION_NONE, 0,
                  NULL);
    }

  /* Read once, without the lock, and only here.  The pointer is written by
   * social_session_worker() before this thread reaches its polling loop and
   * cleared after that loop has ended, so on this thread it cannot change
   * across a call.
   */

  if (scratch == NULL)
    {
      return;
    }

  /* Stay out of a registration's way when there is one.
   *
   * The poll and the upload registrations share one cleartext connection and
   * one mutex, and they are not equally urgent: a registration is on the
   * critical path of the item being uploaded, while a poll only asks about
   * results that will still be there next cycle.  Landing on top of a
   * registration charges that item this poll's entire round trip, which for a
   * response of up to CONFIG_VS_SOCIAL_RESP_MAX_BYTES is not small.  Measured
   * 2026-09-07: registration averages split into a 69-70 ms population and a
   * 240-303 ms one, the second being the polls.
   *
   * Bounded, and the bound is the point.  vs_cloud_cleartext_busy() is a probe
   * whose answer can be stale and, if the counter behind it were ever wrong,
   * could be stale permanently -- so yielding is capped at
   * SOCIAL_POLL_YIELD_MAX consecutive cycles, after which this poll goes ahead
   * and blocks like it always did.  A starved poll would stall the alert
   * decision and the advice sweep, which is a worse failure than a slow upload.
   *
   * The consecutive count is a function-static: only social_poll_once() reads
   * or writes it and only the session thread runs it, so it needs neither the
   * lock nor a home in g_social, and a stale value across sessions costs at
   * most one cycle of not yielding.  The session totals are separate and do
   * live in g_social, because they are read by social_log_totals() on the same
   * thread but from a different function.
   *
   * Both are counted, and the pair is the whole point of measuring this.  The
   * first version of this shipped with no log at all, so the board run that
   * followed could not say whether it had ever fired -- and its registration
   * timings got worse rather than better, which is a result that means nothing
   * without knowing which of the two things happened.  yields against forced
   * says it directly: a session with yields and no forced is the poll always
   * finding room to step aside, forced climbing means it is being held off
   * hard enough to hit SOCIAL_POLL_YIELD_MAX and go anyway.
   */

  {
    static unsigned int yielded;

    if (vs_cloud_cleartext_busy())
      {
        if (yielded < SOCIAL_POLL_YIELD_MAX)
          {
            yielded++;
            g_social.poll_yielded++;
            return;
          }

        /* Out of yields and the connection is still busy, so this poll takes
         * its turn in the queue.  Counted separately: this is the case the
         * bound exists for, and a session full of them is a session where
         * yielding bought nothing.
         */

        g_social.poll_forced++;
      }

    yielded = 0;
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
      bool advice_quiet = false;
      bool advice_refused = false;
      bool report_emotion = false;
      bool report_slot = false;
      uint8_t tracked = 0;
      uint32_t gen = 0;
      uint32_t color = 0;
      uint32_t waited_ms = 0;
      enum vs_emotion_e emotion = VS_EMOTION_NONE;
      bool extreme = false;
      const char *extreme_src = "-";
      char dropped_id[VS_CLOUD_MSG_ID_MAX];
      char text[VS_TEXT_LONG];

      text[0] = '\0';
      dropped_id[0] = '\0';

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
            /* A result for an id this device is no longer tracking.
             *
             * With answered entries now held for their grace window this should
             * be rare, and what is left is the one ordering it cannot prevent:
             * an advice arriving in the same batch as the result that retired
             * its own message.  The entries in a batch are not ordered, so the
             * removal can be processed first, and then the advice -- the only
             * output an extreme moment produces -- would be dropped here for
             * want of a table row.
             *
             * It does not need one.  A msgEvent 1 result carries its own text;
             * the entry was only ever the place the generation was remembered.
             * So deliver it and say so.  Anything else unmatched is counted and
             * named, because this is the one remaining place a cloud signal can
             * go missing and a bare counter could not say which signal.
             */

            bool rescue = ev->msg_event == VS_CLOUD_MSG_EVENT_AUDIO &&
                          ev->peer_state == VS_CLOUD_PEER_ADVICE_DONE &&
                          ev->suggestion[0] != '\0';
            bool quiet = !g_social.alert_active;

            g_social.poll_unmatched++;

            if (rescue)
              {
                g_social.advice_delivered++;
                g_social.advice_rescued++;
                if (quiet)
                  {
                    g_social.advice_quiet++;
                  }
              }

            pthread_mutex_unlock(&g_social.lock);

            if (rescue)
              {
                printf("%s: advice for retired msg %s recovered%s: %s\n",
                       SOCIAL_TAG, ev->msg_id,
                       quiet ? " (no alert standing)" : "", ev->suggestion);
                social_post_full(VS_APP_EVENT_SOCIAL_ADVICE, 0,
                                 VS_EMOTION_TENSE, 0, true, ev->suggestion);
              }
            else
              {
                printf("%s: result for retired msg %s ignored, msgEvent %d "
                       "status %d\n", SOCIAL_TAG, ev->msg_id,
                       (int)ev->msg_event, ev->raw_status);
              }

            continue;
          }

        gen = g_social.inflight[index].alert_gen;

        switch (ev->peer_state)
          {
            case VS_CLOUD_PEER_EMOTION_DONE:

              /* Once per message, whatever the cloud repeats.
               *
               * An extreme frame's entry is deliberately kept so its advice can
               * be collected under the same msgId -- and every poll while it is
               * kept returns the emotion result again, because getResult
               * reports the current state of every id it is asked about rather
               * than only what changed.  Nothing here noticed, so one frame was
               * folded into the alert decision once per poll for as long as it
               * waited.
               *
               * Measured 2026-09-07: "extreme frame ignored, confidence 40
               * below 60" printed about fifty times at a steady 2.02 s, which
               * is the poll interval plus one round trip, not the image
               * interval.  Two things followed.  The advice deadline was pushed
               * forward on every repeat, so social_advice_sweep() could never
               * reach it -- there is not one timeout line in a session where
               * entries waited over a hundred seconds.  The deadline now lives
               * in social_inflight_s::retire_at_ms and this guard is what keeps
               * a repeat from extending it.  And had the confidence gate not
               * been rejecting these
               * frames, a single one counted twice would have satisfied
               * VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS on its own, which is the whole
               * debounce defeated by one frame.
               */

              if (ev->msg_event != VS_CLOUD_MSG_EVENT_IMAGE)
                {
                  /* Nothing else is expected to answer with this state. */
                }
              else if (g_social.inflight[index].image_seen)
                {
                  g_social.results_repeated++;
                }
              else
                {
                  /* The cloud's own verdict, ORed over the rule reproduced from
                   * emotionDetail.  See social_advice_slot_in_batch() for why
                   * the slot is the authoritative half: the document defines
                   * the slot and does not define the rule.
                   *
                   * Written back into ev->extreme rather than kept beside it,
                   * because social_emotion_step() reads the event and this has
                   * to be the value it sees.  ev points into the poll scratch,
                   * which is this thread's own working copy.
                   */

                  /* Which half of the local rule fired, told apart by the
                   * emotion the colour mapped to.
                   *
                   * VS_EMOTION_TENSE is the red bucket, so an extreme frame that
                   * is also tense was caught by the colour and one that is not
                   * was caught by an emotionDetail name outside red.  Under the
                   * current cloud every extreme emotion is red, so "detail"
                   * should never appear -- and if it starts to, the cloud has
                   * moved one of 生气, 反感 or 伤心 out of the red bucket and
                   * cloud_classify_emotion() needs revisiting.
                   */

                  extreme_src = !ev->extreme ? "-" :
                                ev->emotion == VS_EMOTION_TENSE ? "red" :
                                                                  "detail";

                  if (!ev->extreme &&
                      social_advice_slot_in_batch(scratch->events, got,
                                                  ev->msg_id))
                    {
                      ev->extreme = true;
                      extreme_src = "slot(batch)";
                      g_social.emotion_extreme_slot++;
                    }

                  /* A slot seen on an earlier poll counts too.  It is recorded
                   * on the entry precisely so it can outlive the batch it
                   * arrived in, and without this the entry would be folded in as
                   * calm despite the cloud having already said otherwise.
                   */

                  if (!ev->extreme && g_social.inflight[index].advice_seen)
                    {
                      ev->extreme = true;
                      extreme_src = "slot(earlier)";
                      g_social.emotion_extreme_slot++;
                    }

                  /* Recorded before the step below, because the step's release
                   * decision asks whether anything is waiting for advice and
                   * this entry may be the answer.
                   */

                  g_social.inflight[index].image_seen  = true;
                  g_social.inflight[index].image_at_ms = social_now_ms();
                  if (ev->extreme)
                    {
                      g_social.inflight[index].extreme = true;
                    }

                  g_social.emotion_results++;
                  if (ev->extreme)
                    {
                      g_social.emotion_extreme++;
                    }

                  /* Counted, not rejected.  A result with nothing to classify
                   * still retires its msgId and still means the cloud answered,
                   * so the fold has to happen; what was missing was any record
                   * that it carried no emotion.  VS_EMOTION_NONE is exactly the
                   * condition cloud_summarize_timeline() excludes, which is
                   * what makes this counter bridge the two totals.
                   */

                  if (ev->emotion == VS_EMOTION_NONE)
                    {
                      g_social.emotion_blind++;
                      if (!ev->has_response)
                        {
                          g_social.emotion_no_response++;
                        }
                    }

                  (void)social_emotion_step(ev, &raise, &clear);
                  emotion = ev->emotion;
                  color   = ev->color;
                  extreme = ev->extreme;
                  snprintf(text, sizeof(text), "%s", ev->display_text);

                  /* Held, not retired, and this is the correction that makes
                   * the feature work at all.
                   *
                   * The old code retired a non-extreme entry here, reasoning
                   * that the cloud only opens an advice chain for a frame it
                   * judged extreme so a calm frame could never grow one.  The
                   * cloud does not work that way: its rule runs over a
                   * ten-second window, so a frame this device reads as calm can
                   * belong to an extreme moment, and the advice for that moment
                   * is attached to whichever msgId the pipeline is holding when
                   * the text is ready.  Retiring on the result therefore threw
                   * away most of the messages the advice was about to arrive
                   * under.  See social_inflight_s for the two log findings.
                   *
                   * social_inflight_hold() gives an extreme entry the full
                   * timeout and everything else the short grace window, so the
                   * speculation costs table slots for a few poll cycles rather
                   * than for the session.
                   */

                  social_inflight_hold(&g_social.inflight[index],
                                       social_now_ms());
                  report_emotion = true;

                  /* Snapshotted here rather than read at the printf, which is
                   * outside the lock.  Only a log value, but the uploader adds
                   * to this table from another thread and a torn read would be
                   * a torn read whatever it was for.
                   */

                  tracked = g_social.inflight_count;
                }
              break;

            case VS_CLOUD_PEER_ADVICE_PENDING:

              /* An advice slot the batch scan above did not already account
               * for, which means it arrived in a later poll than its image
               * result.  The entry is still tracked, so the cloud's verdict can
               * still be recorded even though the moment to raise on it has
               * passed.
               *
               * This case existed in the status mapping and nowhere else: the
               * switch had no arm for it, so every one of these fell through
               * default and the document's own extreme signal was discarded.
               *
               * Marking rather than raising, deliberately.  A msgEvent 1 entry
               * carries no response -- no emotionDetail, no colour, no
               * confidence -- so an alert raised from it alone would have
               * nothing to put on the screen.  What marking buys is that the
               * entry now waits for its advice instead of being retired as
               * calm, which is the part that still works after the fact.
               */

              /* Recorded before the arm below, and unconditionally, because the
               * arm below cannot see the common case.  It requires the entry
               * not to be extreme already, which is false for every entry
               * actually waiting for advice -- so the slots this session spends
               * thirty seconds waiting on were the ones it never counted.
               */

              if (ev->msg_event == VS_CLOUD_MSG_EVENT_AUDIO)
                {
                  g_social.advice_pending_seen++;
                  if (!g_social.inflight[index].advice_seen)
                    {
                      g_social.inflight[index].advice_seen = true;
                      g_social.advice_slots_seen++;
                      report_slot = true;
                    }

                  /* The promotion, and it no longer waits for the image result.
                   *
                   * It used to require image_seen, on the reasoning that a slot
                   * only means something once the frame it belongs to has been
                   * classified.  That had it backwards: an open slot is the
                   * cloud's own statement that this moment is extreme, which is
                   * a stronger signal than any emotionDetail rule and does not
                   * need corroborating.  Requiring the image first meant a slot
                   * that opened before its image result -- ordinary, since the
                   * cloud pushes both within a second of each other -- left the
                   * entry on the short grace window and it could age out before
                   * the advice landed.
                   */

                  if (!g_social.inflight[index].extreme)
                    {
                      g_social.inflight[index].extreme = true;
                      g_social.emotion_extreme++;
                      g_social.emotion_extreme_slot++;
                      if (g_social.inflight[index].image_seen)
                        {
                          g_social.emotion_extreme_late++;
                        }
                    }

                  social_inflight_hold(&g_social.inflight[index],
                                       social_now_ms());
                }
              break;

            case VS_CLOUD_PEER_ADVICE_DONE:
              if (ev->msg_event == VS_CLOUD_MSG_EVENT_AUDIO)
                {
                  /* Delivered whenever it has text, and the removal of the
                   * conditions that used to guard this is deliberate.
                   *
                   * There were two, a standing alert and a generation test, and
                   * between them they rejected essentially every advice this
                   * cloud sends.  The alert test is the fatal one: the advice
                   * arrives six to eighteen seconds after the moment (AI-side
                   * log, 2026-09-08) against a twelve-second alert hold, so by
                   * the time the text existed there was usually no alert left.
                   * social_alert_release_stale() now defers the release while an
                   * advice is outstanding, which closes most of that gap -- but
                   * only most, since an advice can still arrive on a message the
                   * device never knew was extreme, and the deferral is bounded.
                   *
                   * The generation test was written to stop an advice
                   * contradicting a screen that had moved on.  That concern is
                   * answered better by pinning: VS_APP_EVENT_SOCIAL_ADVICE is
                   * its own event, held on screen until another advice replaces
                   * it, so it no longer competes with the emotion line for the
                   * same field and cannot be contradicted by it.  Against that,
                   * an advice is the only thing this feature produces for an
                   * extreme moment and it arrives once; showing it a few seconds
                   * late is worth far more than withholding it.
                   *
                   * gen is still read and still logged.  It is the measurement
                   * that says how far behind the alert state these arrive, which
                   * is what a future tightening would have to be argued from.
                   */

                  if (ev->suggestion[0] != '\0')
                    {
                      deliver_advice = true;
                      advice_quiet = !g_social.alert_active;
                      g_social.advice_delivered++;

                      if (advice_quiet)
                        {
                          g_social.advice_quiet++;
                        }

                      /* Collected on a message this device had no reason to
                       * expect advice from.  Every one of these was lost before
                       * the grace window existed, so the count is how much the
                       * old assumption was costing.
                       */

                      if (!g_social.inflight[index].extreme &&
                          !g_social.inflight[index].advice_seen)
                        {
                          g_social.advice_from_grace++;
                        }

                      snprintf(text, sizeof(text), "%s", ev->suggestion);
                      emotion = VS_EMOTION_TENSE;
                      extreme = true;
                      waited_ms = (uint32_t)
                        (social_now_ms() -
                         g_social.inflight[index].added_at_ms);
                    }
                  else
                    {
                      /* Terminal and empty.  The cloud says the advice is done
                       * and sent nothing, which is not a device fault but is
                       * still an extreme moment that produced no output.
                       */

                      g_social.advice_discarded++;
                      snprintf(dropped_id, sizeof(dropped_id), "%s",
                               ev->msg_id);
                    }

                  social_inflight_remove(index);
                }
              break;

            case VS_CLOUD_PEER_FAILED:

              /* 30 collapses four server-side reasons, and which side failed
               * decides what to do about it.  msgEvent tells them apart, and
               * this arm used to ignore it and treat both the same way.
               *
               *   msgEvent 1  "音频无有效声音" or the audio could not be read.
               *               A real terminal answer to the wait: there will be
               *               no advice for this message, so the entry is done.
               *
               *   msgEvent 0  no usable face, or the image could not be read.
               *               Ordinary at two frames a second -- and emphatically
               *               not a statement about advice.  Measured on the
               *               AI side 2026-09-08 at 14:52:59: an advice landed on
               *               msgId 54 whose image had 404ed three times.  So the
               *               image is written off and the message is held on the
               *               same grace window an answered one gets.
               *
               * Retiring on an image failure was the second largest source of
               * lost advice after retiring on a calm result, and for the same
               * reason: the device was deciding on the cloud's behalf which
               * messages could still produce something.
               */

              g_social.failed_results++;

              if (ev->msg_event == VS_CLOUD_MSG_EVENT_AUDIO)
                {
                  advice_refused = true;
                  g_social.advice_refused++;
                  snprintf(dropped_id, sizeof(dropped_id), "%s", ev->msg_id);
                  snprintf(text, sizeof(text), "%s", ev->log);
                  social_inflight_remove(index);
                }
              else
                {
                  g_social.inflight[index].image_seen  = true;
                  g_social.inflight[index].image_at_ms = social_now_ms();
                  social_inflight_hold(&g_social.inflight[index],
                                       social_now_ms());
                }
              break;

            default:

              /* 0, 10 and 40 all mean "still working"; 11 has its own arm
               * above.  Leave the entry in flight on its existing deadline.
               */

              break;
          }
      }

      pthread_mutex_unlock(&g_social.lock);

      /* One line per folded emotion result, and it is the line the whole
       * extreme path is diagnosed from.
       *
       * Nothing printed the per-frame verdict before.  The session totals said
       * how many results were extreme and the alert path said when it fired,
       * but between "the cloud classified this frame" and "the device decided
       * it was not extreme" there was no record at all -- so a session where the
       * cloud saw fifty extreme frames and the device raised nothing could not
       * be explained without guessing which of the two rules disagreed.
       *
       * extreme_src is what settles it: "detail" means the emotionDetail rule
       * fired, "slot" means the cloud's own msgEvent 1 did and the local rule
       * did not, "-" means neither.  A session full of "slot" lines is a local
       * rule that has drifted from the cloud's.
       */

      if (report_emotion)
        {
          printf("%s: msg %s emotion %s conf %u extreme %s (%s), "
                 "inflight %u\n", SOCIAL_TAG, ev->msg_id,
                 text[0] != '\0' ? text : "-",
                 (unsigned)ev->confidence, extreme ? "yes" : "no",
                 extreme_src, (unsigned)tracked);
        }

      if (report_slot)
        {
          /* The cloud has committed to advising on this message.  Worth a line
           * on its own: from here the only outcomes are a delivery, a refusal or
           * a timeout, and knowing the slot opened is what makes a later timeout
           * mean "the cloud did not follow through" rather than "there was
           * nothing to follow through on".
           */

          printf("%s: advice slot opened on msg %s\n", SOCIAL_TAG, ev->msg_id);
        }

      if (raise)
        {
          social_post_full(VS_APP_EVENT_SOCIAL_ALERT, 0, emotion, color,
                           extreme, text);
        }
      else if (clear)
        {
          social_post(VS_APP_EVENT_SOCIAL_ALERT_CLEARED, 0, VS_EMOTION_NONE,
                      0, NULL);
        }

      if (deliver_advice && text[0] != '\0')
        {
          /* Its own event, not an ALERT.  See VS_APP_EVENT_SOCIAL_ADVICE: an
           * advice arrives once and has to survive the emotion readings that
           * follow it, which it could not while both wrote the same field.
           */

          printf("%s: advice for msg %s after %lu ms%s: %s\n", SOCIAL_TAG,
                 ev->msg_id, (unsigned long)waited_ms,
                 advice_quiet ? " (no alert standing)" : "", text);
          social_post_full(VS_APP_EVENT_SOCIAL_ADVICE, 0, emotion, color,
                           true, text);
        }

      if (advice_refused)
        {
          printf("%s: cloud will not advise on msg %s%s%s\n", SOCIAL_TAG,
                 dropped_id, text[0] != '\0' ? ": " : "", text);
        }

      if (!deliver_advice && dropped_id[0] != '\0' && !advice_refused)
        {
          printf("%s: advice for msg %s arrived empty (gen %lu)\n", SOCIAL_TAG,
                 dropped_id, (unsigned long)gen);
        }
    }
}

/****************************************************************************
 * Teardown helpers
 ****************************************************************************/

static void social_release_camera(void);
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

  /* And now the camera itself, which is the last thing that needed it.  Ahead
   * of the audio join and the upload drain below, both of which can take
   * seconds the sensor would otherwise spend producing frames nobody reads.
   */

  social_release_camera();

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

  /* Every worker, and the count is cleared only after the last join so a
   * second call to this function -- it is reachable from both the finalize and
   * the abandon paths -- does not join a thread twice.
   */

  while (g_social.upload_count > 0)
    {
      pthread_join(g_social.upload_thread[--g_social.upload_count], NULL);
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

/* Close the camera alone, as soon as the thread that reads it has gone.
 *
 * Split out of social_release_devices() because the two devices stop being
 * needed at different times and the camera's is much earlier.  The capture
 * thread is the only caller of vs_media_stream_grab(), so once it is joined --
 * and once its malformed-frame count has been read out of the stream -- nothing
 * will touch the camera again.  The microphone has to outlive that: its
 * counters are printed by social_log_capture_level() after the audio worker's
 * tail chunk has been uploaded.
 *
 * Measured 2026-09-07: the stopping stage took 7773 ms on one run, and the
 * driver emitted a frame every ~500 ms for the whole of it, because the release
 * sat at the end of social_stop_producers() behind the audio join and the
 * upload drain.  Every one of those frames also cost a JPEG encode, an entropy
 * validation and a console line over the mailbox -- taken from the uploads that
 * were the only thing left to finish.
 *
 * Same detach-under-the-lock discipline as social_release_devices(), and for
 * the same reason: vs_social_abort() reaches for this handle from the UI thread.
 */

static void social_release_camera(void)
{
  struct vs_media_stream_s *camera;

  pthread_mutex_lock(&g_social.lock);
  camera = g_social.camera;
  g_social.camera = NULL;
  pthread_mutex_unlock(&g_social.lock);

  if (camera != NULL)
    {
      vs_media_stream_close(camera);
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
 * tail, and the second call finds nothing to do.  That also covers the camera
 * having already gone through social_release_camera().
 */

static void social_release_devices(void)
{
  struct vs_media_stream_s *camera;
  struct vs_audio_cap_s *mic;
  struct social_poll_scratch_s *poll;
  struct social_inflight_s *inflight;
  bool poll_psram;
  bool inflight_psram;

  pthread_mutex_lock(&g_social.lock);
  camera = g_social.camera;
  mic    = g_social.mic;
  poll   = g_social.poll;
  poll_psram = g_social.poll_psram;
  inflight   = g_social.inflight;
  inflight_psram = g_social.inflight_psram;
  g_social.camera = NULL;
  g_social.mic    = NULL;

  /* Cleared under the lock and before the free, so a poll that has already
   * loaded the pointer cannot be joined by one that loads it after the memory
   * is gone.  Only the session thread calls social_poll_once(), and it is the
   * thread running this, so in practice there is no such race -- this keeps
   * that from being a requirement of the code rather than of the comment.
   */

  g_social.poll = NULL;

  /* The in-flight table goes the same way, and here the race is real rather
   * than notional: social_inflight_add() runs on the upload workers.  They have
   * been joined by the time this is reached -- social_stop_producers() and the
   * upload drain both precede it -- so clearing the pointer under the lock is
   * belt and braces, and the count goes with it so nothing can index a table
   * that is no longer there.
   */

  g_social.inflight = NULL;
  g_social.inflight_count = 0;
  pthread_mutex_unlock(&g_social.lock);

  if (poll != NULL)
    {
      social_free((unsigned char *)poll, poll_psram);
    }

  if (inflight != NULL)
    {
      social_free((unsigned char *)inflight, inflight_psram);
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
         "%lu audio dropped, %lu evicted unanswered, image interval %lu ms\n",
         SOCIAL_TAG, g_social.session.session_id,
         (unsigned long)g_social.image_seq, (unsigned long)malformed,
         (unsigned long)g_social.audio_seq, (unsigned long)g_social.uploaded,
         (unsigned long)g_social.upload_failed,
         (unsigned long)g_social.dropped_image,
         (unsigned long)g_social.dropped_audio,
         (unsigned long)g_social.inflight_evicted,
         (unsigned long)g_social.image_interval_ms);

  /* A second line for the emotion path, because the first one is about
   * transport and this one is about whether the feature worked.
   *
   * Read it against the images uploaded: results plus failures should account
   * for nearly all of them, and whatever is left is still in flight at the end.
   * repeated is how many results the cloud re-sent for a message already folded
   * in -- it should now be the only large number here, and it costs nothing.
   */

  printf("%s: emotion: %lu result(s) (%lu extreme, %lu below confidence), "
         "%lu repeated, %lu failed, %lu alert(s)\n", SOCIAL_TAG,
         (unsigned long)g_social.emotion_results,
         (unsigned long)g_social.emotion_extreme,
         (unsigned long)g_social.emotion_low_conf,
         (unsigned long)g_social.results_repeated,
         (unsigned long)g_social.failed_results,
         (unsigned long)g_social.alerts_raised);

  /* Which of the two extreme signals did the work.  by_slot is the document's
   * own signal catching a frame the emotionDetail rule missed, so anything
   * other than zero means the out-of-band rule has drifted from the cloud's;
   * late is the part of that which arrived too late to raise on, and unmatched
   * is results for ids already retired.  See social_advice_slot_in_batch().
   */

  printf("%s: extreme signal: %lu by advice slot (%lu of them late), "
         "%lu result(s) for a retired id\n", SOCIAL_TAG,
         (unsigned long)g_social.emotion_extreme_slot,
         (unsigned long)g_social.emotion_extreme_late,
         (unsigned long)g_social.poll_unmatched);

  /* A third line, because the four outcomes are not interchangeable and the
   * single "missed" they used to share hid which side was at fault.
   *
   *   delivered  reached the screen.  The only success.
   *   discarded  the cloud said the advice was done and sent no text.
   *   refused    the cloud answered the wait with status 30.  A real answer.
   *   expired    a message that was expecting advice did not get it within
   *              VS_SOCIAL_ADVICE_TIMEOUT_MS.  Partly ordinary: the cloud
   *              advises once per extreme run, so earlier frames in a run
   *              expire by design.
   */

  printf("%s: advice: %lu delivered, %lu discarded, %lu refused, "
         "%lu expired\n", SOCIAL_TAG,
         (unsigned long)g_social.advice_delivered,
         (unsigned long)g_social.advice_discarded,
         (unsigned long)g_social.advice_refused,
         (unsigned long)g_social.advice_expired);

  /* Where the deliveries came from, which is what says whether the grace window
   * earns its table slots.
   *
   *   grace    collected on a message this device had no reason to expect any
   *            advice from -- read as calm, or its image failed outright.  Every
   *            one of these was lost before the window existed, so this figure
   *            is the size of the bug that was fixed.
   *   rescued  arrived in the same batch as the result that retired its own
   *            message, and was delivered from the event rather than a table
   *            entry.
   *   quiet    delivered with no alert standing.  Not a fault -- the advice is
   *            shown regardless -- but it measures how far the cloud's latency
   *            still runs past VS_SOCIAL_ALERT_HOLD_MS.
   */

  printf("%s: advice source: %lu from grace window, %lu rescued unmatched, "
         "%lu with no alert standing\n", SOCIAL_TAG,
         (unsigned long)g_social.advice_from_grace,
         (unsigned long)g_social.advice_rescued,
         (unsigned long)g_social.advice_quiet);

  /* Whether the cloud ever opened a slot, and whether the results it did send
   * carried anything.  The four counters above describe terminal states only,
   * and the two totals below are what they cannot say.  See
   * social_state_s::advice_slots_seen and ::emotion_blind.
   */

  printf("%s: advice slots: %lu opened, %lu pending result(s) seen\n",
         SOCIAL_TAG, (unsigned long)g_social.advice_slots_seen,
         (unsigned long)g_social.advice_pending_seen);

  /* What the sweep removed that was not waiting for advice, and how often the
   * alert was held back for one that was.  Read grace against
   * advice_from_grace: the ratio is how speculative the window is, and a
   * from_grace of zero over many grace_expired means the cloud has stopped
   * attaching advice to unexpected messages and the window can be shortened.
   */

  printf("%s: holds: %lu grace window(s) expired, %lu message(s) never "
         "answered, %lu alert release(s) deferred\n", SOCIAL_TAG,
         (unsigned long)g_social.grace_expired,
         (unsigned long)g_social.image_unanswered,
         (unsigned long)g_social.release_deferred);

  printf("%s: payload: %lu of %lu result(s) carried no emotion "
         "(%lu sent no response)\n", SOCIAL_TAG,
         (unsigned long)g_social.emotion_blind,
         (unsigned long)g_social.emotion_results,
         (unsigned long)g_social.emotion_no_response);

  /* A fourth line, and the only one here that exists to answer a question
   * rather than to describe the session.  See social_state_s's per-type
   * transfer counters: the mean PUT for each media type against its mean size
   * says whether a transfer costs its bytes or costs its round trip, and those
   * two answers call for opposite work.  Whole-session means rather than the
   * ten-upload window, because a window holds too few of either type to
   * average.
   *
   * KB/s is printed for both so the comparison does not have to be done by
   * hand: two similar rates mean bandwidth, and a rate that rises with size
   * means the fixed cost dominates.
   */

  {
    uint32_t images = g_social.upload_image_count;
    uint32_t audios = g_social.upload_audio_count;

    printf("%s: transfer: image %lu x %lu B in %lu ms avg (%.1f KB/s), "
           "audio %lu x %lu B in %lu ms avg (%.1f KB/s)\n", SOCIAL_TAG,
           (unsigned long)images,
           (unsigned long)(images > 0 ?
                           g_social.upload_image_bytes / images : 0),
           (unsigned long)(images > 0 ?
                           g_social.upload_image_put_ms / images : 0),
           g_social.upload_image_put_ms > 0 ?
             (double)g_social.upload_image_bytes * 1000.0 /
             (double)g_social.upload_image_put_ms / 1024.0 : 0.0,
           (unsigned long)audios,
           (unsigned long)(audios > 0 ?
                           g_social.upload_audio_bytes / audios : 0),
           (unsigned long)(audios > 0 ?
                           g_social.upload_audio_put_ms / audios : 0),
           g_social.upload_audio_put_ms > 0 ?
             (double)g_social.upload_audio_bytes * 1000.0 /
             (double)g_social.upload_audio_put_ms / 1024.0 : 0.0);
  }

  /* And whether the poll stepping aside for registrations did anything.  Read
   * against the "reg" half of the upload report: yields with no forced and a
   * flat register time means the deference is working, forced climbing means
   * the registrations are dense enough that SOCIAL_POLL_YIELD_MAX is reached
   * and the poll queues behind them anyway.
   */

  printf("%s: poll: %lu cycle(s) yielded to a register, %lu forced through\n",
         SOCIAL_TAG, (unsigned long)g_social.poll_yielded,
         (unsigned long)g_social.poll_forced);
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
   * left blank.
   *
   * The full year is kept here even though the screen shows two digits of it.
   * A record is the durable copy, and dropping the century to save 16 px on a
   * 160 px display would throw it away everywhere -- including from a store
   * dump, which is the one place a reader has no other way to date a session.
   * vs_app.c trims it on the way to the label, which is also what makes the
   * records already on a board show the short form.
   *
   * A comment here used to say the year made a wrong date obvious.  That
   * stopped being true: the constant vela_tls.c forces when the clock is unset
   * is in 2026, so the year now looks entirely reasonable and only the month
   * and day are wrong.  Whether the clock is real is reported where it is
   * decided, in vs_cloud.c's sync line.
   */

  now = time(NULL);
  if (localtime_r(&now, &tm) != NULL)
    {
      snprintf(index.date, sizeof(index.date), "%04d-%02d-%02d %02d:%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
               tm.tm_min);
    }

  /* Written so a dump of the store still says what the record is, not because
   * the screen reads it back -- vs_app.c uses the same constant directly, which
   * is what makes the records already on a board render at the new length.  See
   * VS_HISTORY_SOCIAL_TITLE for why it is three glyphs.
   */

  snprintf(index.title, sizeof(index.title), VS_HISTORY_SOCIAL_TITLE);
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
  char temp[VS_TTS_PATH_MAX];
  size_t file_len = 0;
  size_t stem;
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

  /* Fetch to a name nothing plays, then rename.
   *
   * The download writes as it goes, so for the length of the transfer there is
   * a partial file wherever it was told to put it -- and
   * vs_history_audio_path() names exactly the file the UI's browse page
   * auto-plays.  Those two facts met on 2026-09-08: the fetch stalled, the
   * finalizing page timed out and dropped the user on the history entry this
   * session had just written, the dwell timer expired, and vs_tts opened
   * 131072 bytes of an unfinished WAV and reported it unplayable (-61).  The
   * record was on the card and correct; only its audio was half-written, and
   * nothing in the naming said so.
   *
   * ".TMP" rather than a dotfile or a subdirectory: the volume is VFAT and
   * vs_history_audio_path() documents that "R" plus seven digits is exactly
   * eight characters to stay 8.3-safe, so swapping the extension keeps that
   * property and keeps the temporary beside its record.  Nothing looks for
   * .TMP -- vs_history_audio_path() always says .WAV -- so a leftover from a
   * power cut is invisible rather than audible, and the unlink below clears it
   * on the next attempt for the same record.
   */

  snprintf(temp, sizeof(temp), "%s", path);
  stem = strlen(temp);
  if (stem < 4)
    {
      return -EINVAL;
    }

  snprintf(temp + stem - 4, sizeof(temp) - (stem - 4), ".TMP");
  (void)unlink(temp);

  began = social_now_ms();
  ret = vs_cloud_download_to_file(url, temp,
                                  CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES,
                                  CONFIG_VS_SOCIAL_DOWNLOAD_TIMEOUT_MS,
                                  social_download_cancelled,
                                  &file_len);
  if (ret < 0)
    {
      /* Three worth naming.  -EFBIG means the spoken minutes outgrew
       * CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES, which is a number to raise rather
       * than a fault to chase.  -ETIMEDOUT means the transfer ran past
       * CONFIG_VS_SOCIAL_DOWNLOAD_TIMEOUT_MS, the bound that keeps this step
       * inside the finalizing page's own budget -- if it is being hit routinely
       * the two want raising together, in that order.  -ECANCELED is not a
       * fault at all: the user or the stage budget asked for the session to
       * stop and the transfer let go, which is the whole point of passing a
       * cancel predicate.
       */

      printf("%s: spoken minutes not fetched: %d%s\n", SOCIAL_TAG, ret,
             ret == -EFBIG ? " (raise VS_SOCIAL_DOWNLOAD_MAX_BYTES)" :
             ret == -ETIMEDOUT ? " (raise VS_SOCIAL_DOWNLOAD_TIMEOUT_MS)" :
             ret == -ECANCELED ? " (session aborted)" : "");
      return ret;
    }

  /* Only now does the name anything plays exist.  rename() over an existing
   * file is atomic on this filesystem, and the only thing that could be there
   * is a previous attempt at the same record.
   */

  if (rename(temp, path) < 0)
    {
      ret = -errno;
      printf("%s: cannot publish the audio for %s: %d\n", SOCIAL_TAG,
             record_key, ret);
      (void)unlink(temp);
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

  /* The extreme count is printed next to the polled one on purpose.  tense
   * folds red and blue together -- see vs_cloud_minutes_s -- so it cannot say
   * how much of the conversation the cloud actually flagged, and the two figures
   * side by side are what shows whether polling saw the extreme frames the
   * cloud's own timeline recorded.
   */

  printf("%s: minutes: calm %u happy %u tense %u, %u emotion / %u audio "
         "samples, %u extreme (%lu seen while polling), text %s, tts %s\n",
         SOCIAL_TAG, minutes.calm,
         minutes.happy, minutes.tense, minutes.emotion_samples,
         minutes.audio_samples, minutes.extreme_samples,
         (unsigned long)g_social.emotion_extreme,
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

  /* The in-flight table, for the same reason and from the same pool.  See
   * social_state_s::inflight: keeping it static cost the boot heap 1.7 KiB it
   * needs before PSRAM exists.
   */

  g_social.inflight = (struct social_inflight_s *)
    social_alloc(SOCIAL_INFLIGHT_BYTES, &g_social.inflight_psram);
  if (g_social.inflight == NULL)
    {
      printf("%s: no memory for the %zu-byte inflight table\n", SOCIAL_TAG,
             (size_t)SOCIAL_INFLIGHT_BYTES);
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, -ENOMEM, VS_EMOTION_NONE,
                  0, "系统资源不足");
      goto close_session;
    }

  memset(g_social.inflight, 0, SOCIAL_INFLIGHT_BYTES);

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

  /* The uploaders.  One is enough to run a session; the rest are throughput.
   *
   * So a partial spawn is not a failure: the loop keeps whatever it got and
   * upload_count says how many to join.  Only zero is fatal, because then
   * nothing would ever leave the rings.
   */

  g_social.upload_count = 0;
  while (g_social.upload_count < CONFIG_VS_SOCIAL_UPLOAD_WORKERS)
    {
      if (social_spawn(&g_social.upload_thread[g_social.upload_count],
                       social_upload_worker,
                       CONFIG_VS_SOCIAL_STACKSIZE_UPLOAD, VS_PRIORITY_SOCIAL,
                       "upload") != 0)
        {
          break;
        }

      g_social.upload_count++;
    }

  if (g_social.upload_count == 0)
    {
      social_post(VS_APP_EVENT_SOCIAL_START_FAILED, -EAGAIN, VS_EMOTION_NONE,
                  0, "系统资源不足");
      goto release;
    }

  if (g_social.upload_count != CONFIG_VS_SOCIAL_UPLOAD_WORKERS)
    {
      printf("%s: %u of %d upload workers started\n", SOCIAL_TAG,
             (unsigned)g_social.upload_count,
             CONFIG_VS_SOCIAL_UPLOAD_WORKERS);
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

  /* Start at the configured ceiling and let the link argue it down.
   *
   * The floor starts at zero rather than carrying over.  What it holds is one
   * link's behaviour at one moment; a session beginning on a different network,
   * or the same one an hour later, is entitled to find out for itself.
   */

  g_social.image_interval_ms = CONFIG_VS_SOCIAL_IMAGE_INTERVAL_MS;
  g_social.image_ok_streak   = 0;
  g_social.image_floor_ms    = 0;

  /* Cleared with the counters they throttle.  Carried over, drop_report_ms
   * would suppress the first drop of a new session for as long as the gap
   * between sessions was short, and the two "reported" marks would make the
   * first line of the new session report a delta against the old one.
   */

  g_social.drop_reported    = 0;
  g_social.drop_report_ms   = 0;
  g_social.upload_reported    = 0;
  g_social.upload_window_ms   = 0;
  g_social.upload_register_ms = 0;
  g_social.upload_transfer_ms = 0;

  /* Not reset by the ten-upload report, unlike the three above: these are
   * whole-session means and the window is too short to average either type.
   */

  g_social.upload_image_put_ms = 0;
  g_social.upload_audio_put_ms = 0;
  g_social.upload_image_bytes  = 0;
  g_social.upload_audio_bytes  = 0;
  g_social.upload_image_count  = 0;
  g_social.upload_audio_count  = 0;

  /* Zero here as well as where the workers are spawned.  A session that failed
   * before reaching the spawn loop would otherwise leave the previous one's
   * count behind for social_stop_producers() to join stale handles from.
   */

  g_social.upload_count = 0;
  g_social.inflight_count = 0;
  g_social.inflight_evicted = 0;

  /* A new session's close will produce its own msgId, and polling the previous
   * one would report the previous conversation into this one's UI.
   */

  g_social.retry_ready       = false;
  g_social.retry_msg_id[0]   = '\0';
  g_social.stage             = VS_SOCIAL_STAGE_NONE;
  g_social.stage_began_ms    = 0;
  g_social.finalize_began_ms = 0;
  g_social.emotion_results  = 0;
  g_social.emotion_extreme  = 0;
  g_social.emotion_extreme_slot = 0;
  g_social.emotion_extreme_late = 0;
  g_social.poll_unmatched   = 0;
  g_social.emotion_low_conf = 0;
  g_social.results_repeated = 0;
  g_social.failed_results   = 0;
  g_social.alerts_raised    = 0;
  g_social.advice_delivered = 0;
  g_social.advice_discarded = 0;
  g_social.advice_refused   = 0;
  g_social.advice_expired   = 0;
  g_social.advice_slots_seen   = 0;
  g_social.advice_pending_seen = 0;
  g_social.advice_from_grace   = 0;
  g_social.advice_rescued      = 0;
  g_social.advice_quiet        = 0;
  g_social.grace_expired       = 0;
  g_social.image_unanswered    = 0;
  g_social.release_deferred    = 0;
  g_social.emotion_blind       = 0;
  g_social.emotion_no_response = 0;
  g_social.poll_yielded     = 0;
  g_social.poll_forced      = 0;
  g_social.alert_gen        = 1;
  g_social.extreme_head     = 0;
  g_social.extreme_have     = 0;
  g_social.calm_streak      = 0;
  g_social.alert_active     = false;
  g_social.alert_since_ms   = 0;
  g_social.alert_extreme_ms = 0;
  memset(g_social.extreme_at, 0, sizeof(g_social.extreme_at));
  g_social.camera         = NULL;
  g_social.mic            = NULL;
  memset(g_social.audio_slot, 0, sizeof(g_social.audio_slot));
  memset(g_social.image_slot, 0, sizeof(g_social.image_slot));

  /* The in-flight table is not cleared here and cannot be: it does not exist
   * yet.  The session worker allocates it from PSRAM and zeroes it there, which
   * is also the only place that knows whether the allocation succeeded.  The
   * pointer is cleared instead, so nothing between here and that allocation can
   * reach a table left over from the previous session.
   */

  g_social.inflight = NULL;

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
