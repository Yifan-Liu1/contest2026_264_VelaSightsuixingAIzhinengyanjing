/****************************************************************************
 * app/velasight/vs_audio.c
 *
 * PCM capture and playback over the NuttX audio character devices.  See
 * include/vs_audio.h for why this exists rather than reusing
 * packages/ai_agent's audio_capture/audio_playback backends.
 *
 * Both directions use the same ioctl sequence, taken from app/audio_test
 * (itself nxrecorder's): RESERVE, CONFIGURE, GETBUFFERINFO, ALLOCBUFFER,
 * mq_open, REGISTERMQ, START, prime.  START deliberately comes before the
 * first buffer is queued; see vs_audio_capture_start() for why the reverse
 * order makes teardown unsafe.  GETBUFFERINFO is not informational --
 * the upper half latches its buffer quota from it and without it every
 * ALLOCBUFFER returns 0.
 *
 * Elapsed-time budgets here use CLOCK_MONOTONIC on purpose.  The realtime
 * clock on this board jumps from 1970 to the present the first time TLS
 * completes a handshake, which is the same trap that made the agent's LLM
 * watchdog fire spuriously; the corresponding complete target file is
 * managed at `external/packages/ai_agent/src/core/agent_loop.c`.  Only the
 * absolute deadlines handed to mq_timedreceive() are CLOCK_REALTIME, because
 * that is the clock POSIX defines for them; a jump there merely shortens one
 * poll inside a loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <nuttx/mm/iob.h>

#include <arch/chip/bk7258_psram.h>

#include "include/vs_audio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES
#  define CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES 655360
#endif

#ifndef CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS
#  define CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS 1000
#endif

#ifndef CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MAX_MS
#  define CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MAX_MS 4000
#endif

/* How much the watermark moves after a reply.
 *
 * Up fast, down slow: a starved reply is direct evidence that the current
 * value is too low, while a clean one only says it was sufficient for that
 * length -- which a short reply proves little about.  The asymmetry is the
 * usual shape for this kind of controller.
 */

#define VS_AUDIO_PB_WATERMARK_RISE 2
#define VS_AUDIO_PB_WATERMARK_FALL_MS 100

/* Audio that must build back up before a starved DAC is un-paused.  Half a
 * second is roughly two driver queues, enough that resuming does not
 * immediately starve again on the next buffer.
 */

#define VS_AUDIO_PB_RESUME_MS 500

/* Smallest playback ring worth running with, used only if PSRAM cannot supply
 * the configured size.  Four seconds still plays, it just cannot keep the TTS
 * socket read while it does -- so it is a degraded mode to report, not a
 * silent fallback.
 */

#define VS_AUDIO_PB_RING_MIN 131072

#ifndef CONFIG_VS_AUDIO_CAPTURE_SETTLE_BUFFERS
#  define CONFIG_VS_AUDIO_CAPTURE_SETTLE_BUFFERS 8
#endif

#ifndef CONFIG_VS_AUDIO_CAPTURE_RING_BYTES
#  define CONFIG_VS_AUDIO_CAPTURE_RING_BYTES 131072
#endif

#ifndef CONFIG_VS_AUDIO_MIC_GAIN
#  define CONFIG_VS_AUDIO_MIC_GAIN 6
#endif

#ifndef CONFIG_VS_AUDIO_ADC_GAIN
#  define CONFIG_VS_AUDIO_ADC_GAIN 0x2d
#endif

/* Mirrors BK7258_AUDIOIOC_SET_CAPGAIN and its argument in
 * board/.../src/bk7258_audio_bringup.h.  Duplicated for the same reason
 * app/audio_test duplicates it: applications do not get the board's private
 * src directory on their include path, and exporting a board header into
 * apps/ to share one constant would be worse coupling.  Keep in step.
 */

#define VS_AUDIO_IOC_CAPGAIN _AUDIOIOC(201)

struct vs_audio_capgain_s
{
  uint8_t mic_gain;
  uint8_t adc_gain;
  bool    hpf;
};

/* A buffer whose peak reaches this is treated as the analog front end still
 * settling rather than as audio.  The ADC returns full-scale samples for the
 * first buffers after it is enabled; at the default minimum microphone gain
 * real speech on this board measures two orders of magnitude below full
 * scale, so saturation is an unambiguous marker.  Discarding by measurement
 * instead of by a fixed count means a short utterance does not lose its
 * opening syllables to a worst-case guess.
 */

/* A sample this close to full scale has almost certainly been clipped by
 * the analog stage rather than merely being loud.
 */

#define VS_AUDIO_CLIP_LEVEL 32000

/* How saturated a buffer must be to count as the analog front end settling
 * rather than as speech, in sixteenths: 15 means "all but one sample in
 * sixteen is pinned at full scale".
 *
 * Saturation alone cannot tell the two apart, and the cost of confusing them
 * is asymmetric.  Passing a settling buffer through costs the recognizer one
 * burst of noise, which it discards; discarding a speech buffer costs the
 * opening syllables of the utterance, which nothing can recover.  So the test
 * is deliberately hard to satisfy: the front end pins essentially every sample
 * at full scale while it settles, whereas even badly overdriven speech
 * saturates only a few percent of its samples.  Speech that clips is kept and
 * sent, distortion and all -- a clipped word can still be recognised, a
 * missing one cannot.
 */

#define VS_AUDIO_SETTLE_CLIP_NUM 15
#define VS_AUDIO_SETTLE_CLIP_DEN 16

/* CONFIG_AUDIO_NUM_BUFFERS is 4 in both board configurations; the array is
 * sized with headroom so a larger quota is clamped rather than overflowing.
 */

#define VS_AUDIO_MAX_BUFFERS 8

/* The playback thread's reclaim wait, which also paces its refill loop.  One
 * driver buffer is 64 ms of 16 kHz mono audio, so this is well inside the
 * margin needed to keep the DAC fed.
 */

#define VS_AUDIO_PB_POLL_MS 10

/* The capture reader thread's wait for the next driver buffer.  One buffer is
 * 64 ms of 16 kHz mono audio, so this returns well before the driver's queue
 * could run dry.
 */

#define VS_AUDIO_CAP_POLL_MS 20

/* How long a blocked write or a drain waits between checks. */

#define VS_AUDIO_WAIT_MS 5

/* Slack added to a drain's computed playtime before it gives up.  A drain
 * that cannot finish must not hold a conversation open indefinitely.
 */

#define VS_AUDIO_DRAIN_SLACK_MS 2000

/* Bits per sample is fixed by the FIFO ports and the driver's copy loops. */

#define VS_AUDIO_BITS 16

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vs_audio_stream_s
{
  int      fd;
  mqd_t    mq;
  const char *mqname;
  struct ap_buffer_s *buffers[VS_AUDIO_MAX_BUFFERS];
  unsigned int nbuffers;
  unsigned int buffersize;
  bool     started;
};

struct vs_audio_cap_s
{
  struct vs_audio_stream_s s;
  pthread_mutex_t lock;
  pthread_t thread;
  bool     thread_valid;

  /* Staging ring between the driver and the consumer.
   *
   * The consumer sends each chunk to the ASR service, which is a blocking
   * TLS write.  The driver only holds CONFIG_AUDIO_NUM_BUFFERS x
   * CONFIG_AUDIO_BUFFER_NUMBYTES -- about 256 ms here -- and its ISR drops
   * samples outright once that queue is empty, without reporting anything.
   * A network write that stalled for longer therefore lost audio silently:
   * measured on this board, a 15 s window came back with 12.7 s of samples.
   * The reader thread below keeps the driver's queue serviced regardless of
   * what the consumer is doing, so a stall costs ring space instead of
   * speech.
   */

  unsigned char *ring;
  size_t   ring_size;
  size_t   head;
  size_t   tail;
  size_t   level;
  bool     ring_psram;

  /* Bytes the reader thread had to discard because the ring was full.  Zero
   * unless the consumer fell seconds behind; reported so overflow can never
   * be as invisible as the driver's own dropping was.
   */

  size_t   dropped;

  /* Remaining buffers this session may still discard as front-end settling.
   * Only saturated ones are actually dropped, so this is a ceiling rather
   * than a quota.
   */

  unsigned int settle;

  /* Level of everything captured, for vs_audio_capture_level. */

  uint64_t  sumsq;
  uint64_t  samples;
  unsigned int peak;

  /* Samples delivered at or beyond VS_AUDIO_CLIP_LEVEL.  Peak saturates at
   * 32767 and so cannot say how hard the analog stage is being overdriven;
   * this can, which is what makes CONFIG_VS_AUDIO_MIC_GAIN tunable from one
   * log line instead of by trial and error.
   */

  uint64_t  clipped;

  /* Bytes discarded as front-end settling.  Reported for the same reason
   * dropped is: this is the one place that deliberately throws captured audio
   * away, so how much it threw away must not be invisible.
   */

  size_t    settled;

  /* When input was closed, and the monotonic deadline until which the reader
   * thread still admits audio.  The ADC has already filled buffers that are
   * only waiting in the queue when the user presses 说完; slamming the gate
   * shut discarded roughly a buffer period of speech they had just finished
   * saying, which is the very tail the drain exists to keep.
   */

  bool     input_stopped;
  uint64_t input_deadline_ms;
  unsigned int bytes_per_sec;
  bool     aborted;
  bool     closing;
};

struct vs_audio_pb_s
{
  struct vs_audio_stream_s s;
  pthread_mutex_t lock;
  pthread_t thread;
  bool     thread_valid;

  /* Staging ring between the TTS producer and the DAC. */

  unsigned char *ring;
  size_t   ring_size;
  size_t   head;
  size_t   tail;
  size_t   level;
  bool     ring_psram;

  /* Buffers owned by us rather than by the driver. */

  struct ap_buffer_s *freebuf[VS_AUDIO_MAX_BUFFERS];
  unsigned int freecount;
  unsigned int inflight;

  unsigned int bytes_per_sec;

  /* Bytes that must accumulate before the DAC is fed, and whether that has
   * happened yet.  Latching rather than a running watermark: re-arming the
   * gate after every dip would turn one silence into many, and by the time the
   * ring has run dry once the audio is already late.
   */

  size_t   prebuffer;
  bool     flowing;

  /* When the first byte was written, and how long the gate may hold after
   * that.  The level alone is not a sufficient release condition: a reply
   * smaller than the mark would never reach it, and would then depend entirely
   * on drain() -- which the caller only reaches once the network stream has
   * ended, seconds after the audio arrived.
   */

  uint64_t first_write_ms;
  unsigned int prebuffer_ms;

  /* Set by drain(): no more audio is coming.  An empty pipeline after this is
   * the reply finishing, not the producer falling behind, and counting it as an
   * underrun reported a gap on every single reply.
   */

  bool     producer_done;

  /* DAC muted and its transmit interrupt disabled because the ring ran dry.
   * Buffers may still be queued while this is set: the driver's
   * enqueuebuffer() only re-arms the interrupt when it is not paused, so the
   * queue simply waits rather than playing into a gap.
   */

  bool     paused;

  /* Bytes that must accumulate before un-pausing. */

  size_t   resume_level;

  /* Lowest number of network buffers seen free while this reply was arriving.
   *
   * Sampled from the TTS receive callback, which is exactly when the pool is
   * under pressure.  A 16 KB TLS record needs eleven of them and cannot be
   * decrypted until all eleven have arrived, so a figure close to zero means
   * the sender's window is being held shut by this device rather than by the
   * link -- which is indistinguishable from a slow network in every other
   * measurement available here.
   */

  int      iob_min;

  /* Times the DAC held nothing while playback was live.  Each is a gap the
   * listener heard, which is otherwise invisible -- the driver treats an empty
   * queue as silence, not as an error.
   */

  unsigned int underruns;
  bool     starved;

  bool     stopped;
  bool     closing;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Carried across playback sessions on purpose.  Each reply opens a fresh
 * handle, so a per-handle watermark would relearn the link's behaviour from
 * scratch every sentence and starve the first one every time.
 */

static unsigned int g_pb_watermark_ms =
  CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Raise a buffer-moving thread above the UI.
 *
 * Both of these run briefly and often -- one driver buffer is 64 ms of audio,
 * and the queue is four of them -- so what they need is to be scheduled
 * promptly rather than for long.  Inheriting the creator's priority put them
 * behind frame pushes, which is how a 256 ms queue runs dry.
 *
 * Failure is not fatal: the thread still works at the inherited priority, it
 * is just easier to starve, so this reports and continues.
 */

static void vs_audio_thread_priority(pthread_attr_t *attr)
{
  struct sched_param param;
  int ret;

  param.sched_priority = VS_PRIORITY_AUDIO;
  ret = pthread_attr_setschedparam(attr, &param);
  if (ret == 0)
    {
      ret = pthread_attr_setschedpolicy(attr, SCHED_FIFO);
    }

  if (ret != 0)
    {
      printf("vs_audio: thread priority %d rejected: %d\n",
             VS_PRIORITY_AUDIO, ret);
    }
}

static uint64_t vs_audio_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int vs_audio_realtime_deadline(struct timespec *ts, unsigned int ms)
{
  if (clock_gettime(CLOCK_REALTIME, ts) < 0)
    {
      return -errno;
    }

  ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
  ts->tv_sec  += (time_t)(ms / 1000u);
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_nsec -= 1000000000L;
      ts->tv_sec++;
    }

  return 0;
}

/* Peak absolute amplitude of one buffer, used both to recognise a settling
 * buffer and to report the delivered level.
 */

static unsigned int vs_audio_peak(const int16_t *samples, size_t count,
                                  size_t *clipped)
{
  unsigned int peak = 0;
  size_t hits = 0;
  size_t i;

  for (i = 0; i < count; i++)
    {
      int32_t v = samples[i];
      unsigned int mag = (unsigned int)(v < 0 ? -v : v);

      if (mag > peak)
        {
          peak = mag;
        }

      if (mag >= VS_AUDIO_CLIP_LEVEL)
        {
          hits++;
        }
    }

  if (clipped != NULL)
    {
      *clipped = hits;
    }

  return peak;
}

/* Caller holds the lock.  Accumulated over delivered samples only, so
 * discarded settling buffers cannot flatter the reported level.
 */

static void vs_audio_level_add(struct vs_audio_cap_s *cap,
                               const int16_t *samples, size_t count)
{
  size_t clipped = 0;
  unsigned int peak = vs_audio_peak(samples, count, &clipped);
  size_t i;

  if (peak > cap->peak)
    {
      cap->peak = peak;
    }

  cap->clipped += clipped;

  for (i = 0; i < count; i++)
    {
      int32_t v = samples[i];

      cap->sumsq += (uint64_t)((int64_t)v * v);
    }

  cap->samples += count;
}

static bool vs_audio_params_valid(unsigned int sample_rate,
                                  unsigned int channels, unsigned int bits)
{
  /* Only the rates reachable from the 26 MHz crystal.  24000 is accepted by
   * aud_set_dac_samplerate() but belongs to the 48 kHz family, which needs
   * the APLL sequence aud_clk_config() rejects, so it would play at the
   * wrong pitch instead of failing.  Refuse it here where it is still a
   * diagnosable error.
   */

  if (sample_rate != 8000 && sample_rate != 16000 && sample_rate != 32000)
    {
      printf("vs_audio: sample rate %u unsupported (8000/16000/32000)\n",
             sample_rate);
      return false;
    }

  if (bits != VS_AUDIO_BITS)
    {
      printf("vs_audio: %u-bit samples unsupported (16 only)\n", bits);
      return false;
    }

  if (channels == 0 || channels > 2)
    {
      printf("vs_audio: %u channel(s) unsupported\n", channels);
      return false;
    }

  return true;
}

static int vs_audio_enqueue(struct vs_audio_stream_s *s,
                            struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = apb->nbytes;
  desc.u.buffer = apb;

  if (ioctl(s->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc) < 0)
    {
      return -errno;
    }

  return 0;
}

static void vs_audio_stream_close(struct vs_audio_stream_s *s)
{
  struct audio_buf_desc_s desc;
  unsigned int i;

  if (s->fd < 0)
    {
      return;
    }

  if (s->started)
    {
      (void)ioctl(s->fd, AUDIOIOC_STOP, 0);
      s->started = false;
    }

  if (s->mq != (mqd_t)-1)
    {
      (void)ioctl(s->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)s->mq);
    }

  for (i = 0; i < s->nbuffers; i++)
    {
      if (s->buffers[i] != NULL)
        {
          memset(&desc, 0, sizeof(desc));
          desc.u.buffer = s->buffers[i];
          (void)ioctl(s->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          s->buffers[i] = NULL;
        }
    }

  (void)ioctl(s->fd, AUDIOIOC_RELEASE, 0);

  if (s->mq != (mqd_t)-1)
    {
      mq_close(s->mq);
      mq_unlink(s->mqname);
      s->mq = (mqd_t)-1;
    }

  close(s->fd);
  s->fd = -1;
}

static int vs_audio_stream_open(struct vs_audio_stream_s *s,
                                const char *dev_path, const char *mqname,
                                bool playback, unsigned int sample_rate,
                                unsigned int channels)
{
  struct audio_caps_desc_s cap_desc;
  struct ap_buffer_info_s buf_info;
  struct audio_buf_desc_s desc;
  struct mq_attr attr;
  unsigned int i;
  int ret;

  memset(s, 0, sizeof(*s));
  s->fd = -1;
  s->mq = (mqd_t)-1;
  s->mqname = mqname;

  s->fd = open(dev_path, O_RDWR | O_CLOEXEC);
  if (s->fd < 0)
    {
      ret = -errno;
      printf("vs_audio: cannot open %s: %d\n", dev_path, ret);
      return ret;
    }

  if (ioctl(s->fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      ret = -errno;
      printf("vs_audio: RESERVE failed on %s: %d\n", dev_path, ret);
      goto err;
    }

  /* Gain staging must precede CONFIGURE: that is where the driver runs
   * adc_setup() and latches these into the analog front end.  The analog
   * stage sits ahead of the ADC, so it is the only one that changes the
   * signal-to-noise ratio -- but it also clips transients irrecoverably if
   * pushed too far, which is why this is configurable rather than maximal.
   */

  if (!playback)
    {
      struct vs_audio_capgain_s gain;

      gain.mic_gain = (uint8_t)CONFIG_VS_AUDIO_MIC_GAIN;
      gain.adc_gain = (uint8_t)CONFIG_VS_AUDIO_ADC_GAIN;
      gain.hpf = false;

      if (ioctl(s->fd, VS_AUDIO_IOC_CAPGAIN, (unsigned long)&gain) < 0)
        {
          /* Not fatal: the driver's own defaults still record, just quieter.
           * Report it once so a level problem is not mistaken for one.
           */

          printf("vs_audio: capture gain request rejected: %d\n", -errno);
        }
    }

  /* PCM format.  The sample rate is split across hw[0] and b[3] because that
   * is how nxplayer sends it and how the lower half recombines it.
   */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels = (uint8_t)channels;
  cap_desc.caps.ac_controls.hw[0] = (uint16_t)(sample_rate & 0xffff);
  cap_desc.caps.ac_controls.b[3] = (uint8_t)(sample_rate >> 16);
  cap_desc.caps.ac_controls.b[2] = VS_AUDIO_BITS;
  cap_desc.caps.ac_subtype = AUDIO_FMT_PCM;

  if (ioctl(s->fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      ret = -errno;
      printf("vs_audio: CONFIGURE failed on %s: %d (%u Hz, %u channel(s))\n",
             dev_path, ret, sample_rate, channels);
      goto err;
    }

  if (ioctl(s->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info) < 0)
    {
      ret = -errno;
      printf("vs_audio: GETBUFFERINFO failed on %s: %d\n", dev_path, ret);
      goto err;
    }

  s->buffersize = buf_info.buffer_size;
  s->nbuffers = buf_info.nbuffers;
  if (s->nbuffers > VS_AUDIO_MAX_BUFFERS)
    {
      s->nbuffers = VS_AUDIO_MAX_BUFFERS;
    }

  if (s->nbuffers == 0 || s->buffersize < sizeof(int16_t))
    {
      printf("vs_audio: driver offered %u buffer(s) of %u byte(s)\n",
             s->nbuffers, s->buffersize);
      ret = -EIO;
      goto err;
    }

  for (i = 0; i < s->nbuffers; i++)
    {
      memset(&desc, 0, sizeof(desc));
      desc.numbytes = s->buffersize;
      desc.u.pbuffer = &s->buffers[i];

      /* A return of 0 is not success: the upper half uses it to report that
       * its buffer quota is already exhausted.
       */

      ret = ioctl(s->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
      if (ret <= 0 || s->buffers[i] == NULL)
        {
          printf("vs_audio: ALLOCBUFFER %u of %u returned %d\n", i,
                 s->nbuffers, ret);
          s->buffers[i] = NULL;
          s->nbuffers = i;
          ret = -ENOMEM;
          goto err;
        }
    }

  /* Drop any queue left behind by an earlier session so stale completions
   * cannot be mistaken for this session's buffers.
   */

  (void)mq_unlink(mqname);

  attr.mq_maxmsg = s->nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags = 0;

  s->mq = mq_open(mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (s->mq == (mqd_t)-1)
    {
      ret = -errno;
      printf("vs_audio: mq_open(%s) failed: %d\n", mqname, ret);
      goto err;
    }

  if (ioctl(s->fd, AUDIOIOC_REGISTERMQ, (unsigned long)s->mq) < 0)
    {
      ret = -errno;
      printf("vs_audio: REGISTERMQ failed on %s: %d\n", dev_path, ret);
      goto err;
    }

  return 0;

err:
  vs_audio_stream_close(s);
  return ret;
}

/****************************************************************************
 * Private Functions -- capture
 ****************************************************************************/

/* Append one driver buffer's samples to the ring.  Caller holds the lock.
 * Returns the number of bytes that did not fit.
 */

static size_t vs_audio_ring_put(struct vs_audio_cap_s *cap,
                                const unsigned char *src, size_t len)
{
  size_t room = cap->ring_size - cap->level;
  size_t take = len < room ? len : room;
  size_t first;

  if (take == 0)
    {
      return len;
    }

  first = cap->ring_size - cap->head;
  if (first > take)
    {
      first = take;
    }

  memcpy(cap->ring + cap->head, src, first);
  if (first < take)
    {
      memcpy(cap->ring, src + first, take - first);
    }

  cap->head = (cap->head + take) % cap->ring_size;
  cap->level += take;
  return len - take;
}

/* Keep the driver's queue serviced no matter how long the consumer spends in
 * the network, and stage what arrives in the ring.
 */

static void *vs_audio_cap_thread(void *arg)
{
  struct vs_audio_cap_s *cap = arg;

  for (; ; )
    {
      struct audio_msg_s msg;
      struct timespec ts;
      struct ap_buffer_s *apb;
      unsigned int prio;
      ssize_t got;
      bool done;

      if (vs_audio_realtime_deadline(&ts, VS_AUDIO_CAP_POLL_MS) < 0)
        {
          usleep(VS_AUDIO_CAP_POLL_MS * 1000);
        }
      else
        {
          got = mq_timedreceive(cap->s.mq, (char *)&msg, sizeof(msg), &prio,
                                &ts);
          if (got == (ssize_t)sizeof(msg))
            {
              if (msg.msg_id == AUDIO_MSG_IOERR)
                {
                  printf("vs_audio: capture reported an I/O error\n");
                }
              else if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
                {
                  apb = msg.u.ptr;

                  pthread_mutex_lock(&cap->lock);

                  /* Drop the saturated buffers the analog front end emits
                   * while the ADC settles.  Settling is detected rather than
                   * assumed: the first buffer that is not saturated ends the
                   * discard phase and is kept, so an utterance keeps its
                   * opening syllables even when the user starts talking
                   * immediately.  The configured count only bounds how long
                   * this can go on.
                   */

                  if (apb->nbytes >= sizeof(int16_t) && cap->settle > 0)
                    {
                      size_t total = apb->nbytes / sizeof(int16_t);
                      size_t clipped = 0;

                      (void)vs_audio_peak((const int16_t *)apb->samp, total,
                                          &clipped);

                      if (clipped * VS_AUDIO_SETTLE_CLIP_DEN >
                          total * VS_AUDIO_SETTLE_CLIP_NUM)
                        {
                          cap->settle--;
                          cap->settled += apb->nbytes;
                          apb->nbytes = 0;
                        }
                      else
                        {
                          cap->settle = 0;
                        }
                    }

                  if (apb->nbytes >= sizeof(int16_t) && !cap->aborted &&
                      (!cap->input_stopped ||
                       vs_audio_now_ms() < cap->input_deadline_ms))
                    {
                      size_t usable = apb->nbytes &
                                      ~(size_t)(sizeof(int16_t) - 1);

                      vs_audio_level_add(cap, (const int16_t *)apb->samp,
                                         usable / sizeof(int16_t));
                      cap->dropped += vs_audio_ring_put(cap, apb->samp,
                                                        usable);
                    }

                  done = cap->closing;
                  apb->nbytes = 0;
                  apb->curbyte = 0;
                  if (!done)
                    {
                      (void)vs_audio_enqueue(&cap->s, apb);
                    }

                  pthread_mutex_unlock(&cap->lock);
                }
            }
        }

      pthread_mutex_lock(&cap->lock);
      done = cap->closing;
      pthread_mutex_unlock(&cap->lock);
      if (done)
        {
          break;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions -- capture
 ****************************************************************************/

struct vs_audio_cap_s *vs_audio_capture_open(const char *dev_path,
                                             unsigned int sample_rate,
                                             unsigned int channels,
                                             unsigned int bits)
{
  struct vs_audio_cap_s *cap;
  int ret;

  if (dev_path == NULL ||
      !vs_audio_params_valid(sample_rate, channels, bits))
    {
      return NULL;
    }

  cap = calloc(1, sizeof(*cap));
  if (cap == NULL)
    {
      return NULL;
    }

  ret = pthread_mutex_init(&cap->lock, NULL);
  if (ret != 0)
    {
      free(cap);
      return NULL;
    }

  /* PSRAM first, for the same reason the playback ring uses it: this buffer
   * is touched at audio rate, not memory rate, and taking it out of the SRAM
   * heap would compete with the driver's own pipeline buffers.
   */

  /* Whole samples only.  Every quantity in the ring arithmetic is even --
   * the ISR advances nbytes by sizeof(int16_t), reads round down -- so an odd
   * size would, on the first overflow, leave level permanently odd: read()
   * would round its copy to zero while pending() still reported a byte, and
   * capture would stop dead from that point.  The Kconfig range does not
   * exclude odd values, so clamp here instead of trusting it.
   */

  cap->ring_size = CONFIG_VS_AUDIO_CAPTURE_RING_BYTES &
                   ~(size_t)(sizeof(int16_t) - 1);
  cap->ring = bk7258_psram_malloc(cap->ring_size);
  cap->ring_psram = cap->ring != NULL;
  if (cap->ring == NULL)
    {
      cap->ring = malloc(cap->ring_size);
    }

  if (cap->ring == NULL)
    {
      pthread_mutex_destroy(&cap->lock);
      free(cap);
      return NULL;
    }

  ret = vs_audio_stream_open(&cap->s, dev_path, "vs_aud_c", false,
                             sample_rate, channels);
  if (ret < 0)
    {
      if (cap->ring_psram)
        {
          bk7258_psram_free(cap->ring);
        }
      else
        {
          free(cap->ring);
        }

      pthread_mutex_destroy(&cap->lock);
      free(cap);
      return NULL;
    }

  cap->settle = CONFIG_VS_AUDIO_CAPTURE_SETTLE_BUFFERS;
  cap->bytes_per_sec = sample_rate * channels * (VS_AUDIO_BITS / 8);
  return cap;
}

int vs_audio_capture_start(struct vs_audio_cap_s *cap)
{
  pthread_attr_t attr;
  unsigned int i;
  int ret;

  if (cap == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&cap->lock);
  if (cap->s.started)
    {
      pthread_mutex_unlock(&cap->lock);
      return 0;
    }

  /* START before queueing anything, even though it costs the samples of the
   * first FIFO interrupt.
   *
   * A buffer must never sit in the driver's queue while the device is merely
   * PREPARED: audio_stop() in nuttx/audio/audio.c only descends to the lower
   * half from RUNNING or PAUSED, so in that state AUDIOIOC_STOP does not run
   * bk7258_audio_flush() and the buffers are never handed back.  Freeing them
   * anyway -- which is what teardown must then do -- leaves the driver's
   * pendq pointing at released memory, and close(fd) walks it.
   *
   * Nothing is lost by this order: the ISR masks its own interrupt when it
   * finds no buffer, and bk7258_audio_enqueuebuffer() re-arms it while
   * running, so the queue below picks the stream straight back up.
   */

  if (ioctl(cap->s.fd, AUDIOIOC_START, 0) < 0)
    {
      ret = -errno;
      printf("vs_audio: capture START failed: %d\n", ret);
      pthread_mutex_unlock(&cap->lock);
      return ret;
    }

  cap->s.started = true;

  for (i = 0; i < cap->s.nbuffers; i++)
    {
      cap->s.buffers[i]->nbytes = 0;
      cap->s.buffers[i]->curbyte = 0;
      ret = vs_audio_enqueue(&cap->s, cap->s.buffers[i]);
      if (ret < 0)
        {
          printf("vs_audio: capture ENQUEUEBUFFER failed: %d\n", ret);
          pthread_mutex_unlock(&cap->lock);
          return ret;
        }
    }

  pthread_mutex_unlock(&cap->lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  vs_audio_thread_priority(&attr);
  ret = pthread_create(&cap->thread, &attr, vs_audio_cap_thread, cap);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      printf("vs_audio: capture thread failed: %d\n", ret);
      return -ret;
    }

  cap->thread_valid = true;
  return 0;
}

int vs_audio_capture_read(struct vs_audio_cap_s *cap, void *buf, size_t len)
{
  size_t copy;
  size_t first;

  if (cap == NULL || buf == NULL || len < sizeof(int16_t))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&cap->lock);
  if (cap->aborted)
    {
      pthread_mutex_unlock(&cap->lock);
      return -ECANCELED;
    }

  copy = cap->level < len ? cap->level : len;

  /* Whole samples only: the caller casts this to int16_t. */

  copy &= ~(size_t)(sizeof(int16_t) - 1);
  if (copy == 0)
    {
      pthread_mutex_unlock(&cap->lock);
      return -EAGAIN;
    }

  first = cap->ring_size - cap->tail;
  if (first > copy)
    {
      first = copy;
    }

  memcpy(buf, cap->ring + cap->tail, first);
  if (first < copy)
    {
      memcpy((unsigned char *)buf + first, cap->ring, copy - first);
    }

  cap->tail = (cap->tail + copy) % cap->ring_size;
  cap->level -= copy;
  pthread_mutex_unlock(&cap->lock);
  return (int)copy;
}

size_t vs_audio_capture_pending(struct vs_audio_cap_s *cap)
{
  size_t pending;

  if (cap == NULL)
    {
      return 0;
    }

  pthread_mutex_lock(&cap->lock);
  pending = cap->level;
  pthread_mutex_unlock(&cap->lock);
  return pending;
}

void vs_audio_capture_stop(struct vs_audio_cap_s *cap)
{
  if (cap == NULL)
    {
      return;
    }

  pthread_mutex_lock(&cap->lock);
  if (!cap->input_stopped)
    {
      unsigned int rate = cap->bytes_per_sec > 0 ?
                          cap->bytes_per_sec : 32000u;

      cap->input_stopped = true;

      /* Two buffer periods of grace, computed from the geometry the driver
       * reported rather than assumed.  The ADC is part-way through a buffer
       * when this is called and the driver only hands one back once it is
       * full, so a single period races with the very arrival it is waiting
       * for; two lands it reliably and still costs only about 128 ms.  A
       * constant bound is what keeps a drain loop terminating.
       */

      cap->input_deadline_ms = vs_audio_now_ms() +
                               2u * (uint64_t)cap->s.buffersize * 1000u /
                               rate;
    }

  pthread_mutex_unlock(&cap->lock);
}

bool vs_audio_capture_input_pending(struct vs_audio_cap_s *cap)
{
  bool pending;

  if (cap == NULL)
    {
      return false;
    }

  pthread_mutex_lock(&cap->lock);
  pending = !cap->aborted &&
            (!cap->input_stopped ||
             vs_audio_now_ms() < cap->input_deadline_ms);
  pthread_mutex_unlock(&cap->lock);
  return pending;
}

void vs_audio_capture_level(struct vs_audio_cap_s *cap,
                            struct vs_audio_level_s *level)
{
  if (level == NULL)
    {
      return;
    }

  memset(level, 0, sizeof(*level));

  if (cap != NULL)
    {
      pthread_mutex_lock(&cap->lock);
      level->peak = cap->peak;
      level->dropped = cap->dropped;
      level->clipped = cap->clipped;
      level->samples = cap->samples;
      level->settled = cap->settled;
      if (cap->samples > 0)
        {
          uint64_t mean = cap->sumsq / cap->samples;
          unsigned int root = 0;
          unsigned int bit;

          /* Integer square root; this runs once per utterance, and pulling in
           * the floating-point library for it is not worth the code size.
           */

          for (bit = 1u << 15; bit > 0; bit >>= 1)
            {
              unsigned int trial = root | bit;

              if ((uint64_t)trial * trial <= mean)
                {
                  root = trial;
                }
            }

          level->rms = root;
        }

      pthread_mutex_unlock(&cap->lock);
    }
}

void vs_audio_capture_abort(struct vs_audio_cap_s *cap)
{
  if (cap == NULL)
    {
      return;
    }

  pthread_mutex_lock(&cap->lock);
  cap->aborted = true;
  cap->input_stopped = true;
  cap->level = 0;
  cap->head = 0;
  cap->tail = 0;
  pthread_mutex_unlock(&cap->lock);
}

void vs_audio_capture_close(struct vs_audio_cap_s *cap)
{
  if (cap == NULL)
    {
      return;
    }

  pthread_mutex_lock(&cap->lock);
  cap->closing = true;
  pthread_mutex_unlock(&cap->lock);

  if (cap->thread_valid)
    {
      pthread_join(cap->thread, NULL);
      cap->thread_valid = false;
    }

  vs_audio_stream_close(&cap->s);

  if (cap->ring_psram)
    {
      bk7258_psram_free(cap->ring);
    }
  else
    {
      free(cap->ring);
    }

  pthread_mutex_destroy(&cap->lock);
  free(cap);
}

/****************************************************************************
 * Private Functions -- playback
 ****************************************************************************/

/* Copy out of the ring into one driver buffer.  Caller holds the lock. */

static size_t vs_audio_ring_take(struct vs_audio_pb_s *pb,
                                 struct ap_buffer_s *apb)
{
  size_t want = pb->level;
  size_t first;

  if (want > apb->nmaxbytes)
    {
      want = apb->nmaxbytes;
    }

  /* Whole samples only, or the DAC would be handed half of one. */

  want &= ~(size_t)(sizeof(int16_t) - 1);
  if (want == 0)
    {
      return 0;
    }

  first = pb->ring_size - pb->tail;
  if (first > want)
    {
      first = want;
    }

  memcpy(apb->samp, pb->ring + pb->tail, first);
  if (first < want)
    {
      memcpy(apb->samp + first, pb->ring, want - first);
    }

  pb->tail = (pb->tail + want) % pb->ring_size;
  pb->level -= want;
  return want;
}

static void *vs_audio_pb_thread(void *arg)
{
  struct vs_audio_pb_s *pb = arg;

  for (; ; )
    {
      struct audio_msg_s msg;
      struct timespec ts;
      unsigned int prio;
      ssize_t got;
      bool done;

      /* One reclaim attempt.  Its timeout is also this loop's pacing, so a
       * silent stretch does not spin.
       */

      if (vs_audio_realtime_deadline(&ts, VS_AUDIO_PB_POLL_MS) == 0)
        {
          got = mq_timedreceive(pb->s.mq, (char *)&msg, sizeof(msg), &prio,
                                &ts);
          if (got == (ssize_t)sizeof(msg) &&
              msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
            {
              pthread_mutex_lock(&pb->lock);
              if (pb->inflight > 0)
                {
                  pb->inflight--;
                }

              if (pb->freecount < pb->s.nbuffers)
                {
                  pb->freebuf[pb->freecount++] = msg.u.ptr;
                }

              pthread_mutex_unlock(&pb->lock);
            }
        }
      else
        {
          usleep(VS_AUDIO_PB_POLL_MS * 1000);
        }

      pthread_mutex_lock(&pb->lock);

      /* Pre-roll: hold the DAC back until enough audio has accumulated, or
       * until the gate's own deadline passes.  Either release is final.
       *
       * The deadline is what makes a short reply work: it can be complete and
       * still be below the mark, in which case waiting for the mark means
       * waiting for audio that does not exist.
       */

      if (!pb->flowing &&
          (pb->level >= pb->prebuffer ||
           (pb->first_write_ms != 0 &&
            vs_audio_now_ms() - pb->first_write_ms >= pb->prebuffer_ms)))
        {
          pb->flowing = true;
        }

      /* Rebuffer: the pipeline emptied while the reply was still arriving.
       *
       * Muting and masking the transmit interrupt is not cosmetic.  Left
       * running, the DAC keeps clocking an empty FIFO and emits whatever was
       * last in it, so a gap is not silence but a held sample or a repeat.
       * Pausing makes the gap clean, and the driver still accepts buffers
       * while paused, so audio can build back up behind it.
       */

      if (pb->flowing && !pb->stopped && !pb->producer_done &&
          !pb->paused && pb->inflight == 0 && pb->level == 0)
        {
          if (ioctl(pb->s.fd, AUDIOIOC_PAUSE, 0) == 0)
            {
              pb->paused = true;
            }

          if (!pb->starved)
            {
              pb->starved = true;
              pb->underruns++;
            }
        }

      /* Resume once there is enough to keep going, or when nothing more is
       * coming -- otherwise the tail of a reply that starved right at its end
       * would never be played at all.
       */

      if (pb->paused &&
          (pb->level >= pb->resume_level || pb->producer_done ||
           pb->stopped))
        {
          if (ioctl(pb->s.fd, AUDIOIOC_RESUME, 0) == 0)
            {
              pb->paused = false;
              pb->starved = false;
            }
        }

      if (pb->level > 0 || pb->inflight > 0)
        {
          pb->starved = false;
        }

      /* Buffers are queued even while paused: they wait in the driver's queue
       * and start playing the moment it resumes, which is what makes the gap a
       * pause rather than a discontinuity.
       */

      while (pb->flowing && !pb->stopped && pb->freecount > 0 &&
             pb->level > 0)
        {
          struct ap_buffer_s *apb = pb->freebuf[pb->freecount - 1];
          size_t n;

          apb->nbytes = 0;
          apb->curbyte = 0;
          n = vs_audio_ring_take(pb, apb);
          if (n == 0)
            {
              break;
            }

          apb->nbytes = n;
          if (vs_audio_enqueue(&pb->s, apb) < 0)
            {
              /* Put the audio back so nothing is lost, and try again on the
               * next pass rather than dropping the buffer.
               */

              pb->tail = (pb->tail + pb->ring_size - n) % pb->ring_size;
              pb->level += n;
              break;
            }

          pb->freecount--;
          pb->inflight++;
        }

      done = pb->closing;
      pthread_mutex_unlock(&pb->lock);

      if (done)
        {
          break;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions -- output volume
 ****************************************************************************/

/* Open, set or read, close.  There is deliberately no state kept here.
 *
 * bk7258_audio_configure() stores what AUDIO_FU_VOLUME gives it in
 * priv->cfg.dac_dig_gain as well as writing the DAC gain register, precisely
 * so the setting outlives the stream that was running when it was made.  A
 * cached copy in this file could therefore only go stale, and asking the
 * driver costs one ioctl.
 */

int vs_audio_volume_set(const char *dev_path, unsigned int permille)
{
  struct audio_caps_desc_s cap_desc;
  int fd;
  int ret = 0;

  if (dev_path == NULL || permille > 1000)
    {
      return -EINVAL;
    }

  fd = open(dev_path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  /* No RESERVE: this configures a property of the device rather than claiming
   * it, and reserving would fail while a reply is being spoken -- which is one
   * of the times a user is most likely to reach for the volume.
   */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
  cap_desc.caps.ac_controls.hw[0] = (uint16_t)permille;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      ret = -errno;
      printf("vs_audio: volume %u rejected: %d\n", permille, ret);
    }

  close(fd);
  return ret;
}

int vs_audio_volume_get(const char *dev_path, unsigned int *permille)
{
  struct audio_caps_desc_s cap_desc;
  int fd;
  int ret = 0;

  if (dev_path == NULL || permille == NULL)
    {
      return -EINVAL;
    }

  fd = open(dev_path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;

  /* ac_channels == 0 is how the driver says it did not answer this feature,
   * which is not the same as answering zero volume.
   */

  if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&cap_desc.caps) < 0)
    {
      ret = -errno;
    }
  else if (cap_desc.caps.ac_channels == 0)
    {
      ret = -ENOTTY;
    }
  else
    {
      *permille = cap_desc.caps.ac_controls.hw[0];
    }

  close(fd);
  return ret;
}

/****************************************************************************
 * Public Functions -- playback
 ****************************************************************************/

struct vs_audio_pb_s *vs_audio_playback_open(const char *dev_path,
                                             unsigned int sample_rate,
                                             unsigned int channels,
                                             unsigned int bits)
{
  struct vs_audio_pb_s *pb;
  pthread_attr_t attr;
  int ret;

  if (dev_path == NULL ||
      !vs_audio_params_valid(sample_rate, channels, bits))
    {
      return NULL;
    }

  pb = calloc(1, sizeof(*pb));
  if (pb == NULL)
    {
      return NULL;
    }

  ret = pthread_mutex_init(&pb->lock, NULL);
  if (ret != 0)
    {
      free(pb);
      return NULL;
    }

  /* PSRAM first.  This ring is touched at audio rate, not memory rate, so
   * uncached PSRAM costs it nothing, while taking 128 KiB out of the SRAM
   * heap would starve the small pipeline buffers the driver needs -- the
   * failure app/audio_test records as "ALLOCBUFFER 0 of 4".
   */

  /* Halve the request until PSRAM can satisfy it rather than failing outright.
   * The full size is what keeps a whole answer buffered, and therefore what
   * keeps the TTS socket drained, but a smaller ring still speaks -- so a
   * momentarily busy PSRAM heap should degrade playback, not cancel the reply.
   * The SRAM heap is not a fallback at these sizes; it is 336 KiB in total and
   * the driver's own pipeline buffers come out of it.
   */

  pb->ring_size = CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES;
  while (pb->ring == NULL)
    {
      pb->ring = bk7258_psram_malloc(pb->ring_size);
      if (pb->ring != NULL || pb->ring_size <= VS_AUDIO_PB_RING_MIN)
        {
          break;
        }

      pb->ring_size /= 2;
    }

  pb->ring_psram = pb->ring != NULL;
  if (pb->ring == NULL)
    {
      pb->ring = malloc(pb->ring_size);
    }

  if (pb->ring == NULL)
    {
      pthread_mutex_destroy(&pb->lock);
      free(pb);
      return NULL;
    }

  if (pb->ring_size < CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES)
    {
      struct bk7258_psram_info info;

      /* Not an error, and no longer a risk to the stream: since the TTS client
       * answers the server's keep-alive pings, a ring that fills only applies
       * backpressure -- the reply arrives at the speed it is spoken instead of
       * the speed of the network.  It is still worth reporting with the heap
       * figures, because the gap between what was asked for and what PSRAM
       * could give is the only clue to what else is holding the heap.
       */

      if (bk7258_psram_info(&info) == 0)
        {
          printf("vs_audio: playback ring %zu of %d byte(s); psram heap "
                 "free %zu largest %zu of %zu\n",
                 pb->ring_size, CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES,
                 info.heap.free, info.heap.largest_free, info.heap.size);
        }
      else
        {
          printf("vs_audio: playback ring %zu of %d byte(s)\n",
                 pb->ring_size, CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES);
        }
    }

  ret = vs_audio_stream_open(&pb->s, dev_path, "vs_aud_p", true, sample_rate,
                             channels);
  if (ret < 0)
    {
      goto err_ring;
    }

  pb->freecount = pb->s.nbuffers;
  memcpy(pb->freebuf, pb->s.buffers,
         pb->s.nbuffers * sizeof(pb->freebuf[0]));
  pb->bytes_per_sec = sample_rate * channels * (VS_AUDIO_BITS / 8);

  /* Clamped to a fraction of the ring: a pre-buffer the ring cannot hold would
   * never be satisfied, and playback would depend entirely on drain() to
   * release it.  Whole samples only, for the same reason the ring size is.
   */

  pb->prebuffer = (size_t)pb->bytes_per_sec * g_pb_watermark_ms / 1000u;
  if (pb->prebuffer > pb->ring_size / 2)
    {
      pb->prebuffer = pb->ring_size / 2;
    }

  pb->prebuffer &= ~(size_t)(sizeof(int16_t) - 1);
  pb->prebuffer_ms = g_pb_watermark_ms;
  pb->flowing = pb->prebuffer == 0;

  pb->resume_level = (size_t)pb->bytes_per_sec * VS_AUDIO_PB_RESUME_MS / 1000u;
  if (pb->resume_level > pb->ring_size / 2)
    {
      pb->resume_level = pb->ring_size / 2;
    }

  pb->resume_level &= ~(size_t)(sizeof(int16_t) - 1);
  pb->iob_min = INT_MAX;

  /* Start the DAC now, before any audio exists.
   *
   * The alternative -- queue a buffer or two first, then start -- would put
   * buffers in the driver's queue while it is still only PREPARED, and
   * AUDIOIOC_STOP does not hand those back (see the comment in
   * vs_audio_capture_start()), so teardown would free memory the driver
   * still has linked.  Starting empty is safe instead: the TX ISR masks its
   * own interrupt when it finds nothing to play and enqueuebuffer() re-arms
   * it, and the driver's own start sequence keeps the amplifier muted until
   * the analog output has settled, so an idle DAC is silent rather than
   * noisy.
   */

  if (ioctl(pb->s.fd, AUDIOIOC_START, 0) < 0)
    {
      ret = -errno;
      printf("vs_audio: playback START failed: %d\n", ret);
      vs_audio_stream_close(&pb->s);
      goto err_ring;
    }

  pb->s.started = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  vs_audio_thread_priority(&attr);
  ret = pthread_create(&pb->thread, &attr, vs_audio_pb_thread, pb);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      printf("vs_audio: playback thread failed: %d\n", ret);
      vs_audio_stream_close(&pb->s);
      goto err_ring;
    }

  pb->thread_valid = true;
  return pb;

err_ring:
  if (pb->ring_psram)
    {
      bk7258_psram_free(pb->ring);
    }
  else
    {
      free(pb->ring);
    }

  pthread_mutex_destroy(&pb->lock);
  free(pb);
  return NULL;
}

int vs_audio_playback_write(struct vs_audio_pb_s *pb, const void *buf,
                            size_t len)
{
  const unsigned char *src = buf;
  size_t done = 0;

  if (pb == NULL || buf == NULL)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return 0;
    }

  while (done < len)
    {
      size_t room;
      size_t chunk;
      size_t first;

      pthread_mutex_lock(&pb->lock);
      if (pb->stopped)
        {
          pthread_mutex_unlock(&pb->lock);
          return -ECANCELED;
        }

      room = pb->ring_size - pb->level;
      if (room == 0)
        {
          /* Backpressure: the ring holds seconds of audio, so this only
           * happens when the network is well ahead of the speaker.
           */

          pthread_mutex_unlock(&pb->lock);
          usleep(VS_AUDIO_WAIT_MS * 1000);
          continue;
        }

      chunk = len - done;
      if (chunk > room)
        {
          chunk = room;
        }

      first = pb->ring_size - pb->head;
      if (first > chunk)
        {
          first = chunk;
        }

      /* Network buffer pressure, sampled where the producer actually is.
       * throttled=true asks the same question TCP read-ahead asks, so this is
       * the figure that decides whether more data may be accepted.
       */

      {
        int navail = iob_navail(true);

        if (navail < pb->iob_min)
          {
            pb->iob_min = navail;
          }
      }

      /* Start the gate's clock at the first byte, not at open(): the DAC is
       * opened before the network request goes out, and counting the TLS
       * handshake against the pre-buffer would release it before any audio had
       * arrived at all.
       */

      if (pb->first_write_ms == 0)
        {
          pb->first_write_ms = vs_audio_now_ms();
        }

      memcpy(pb->ring + pb->head, src + done, first);
      if (first < chunk)
        {
          memcpy(pb->ring, src + done + first, chunk - first);
        }

      pb->head = (pb->head + chunk) % pb->ring_size;
      pb->level += chunk;
      done += chunk;
      pthread_mutex_unlock(&pb->lock);
    }

  return (int)len;
}

void vs_audio_playback_drain(struct vs_audio_pb_s *pb)
{
  uint64_t deadline;
  size_t pending;

  if (pb == NULL)
    {
      return;
    }

  pthread_mutex_lock(&pb->lock);
  /* Whatever is in the ring at this point is all there will ever be, so the
   * gate has done its job and must not keep a short reply waiting for audio
   * that is not coming.
   */

  pb->flowing = true;
  pb->producer_done = true;

  /* Nothing further is coming, so a pause has nothing left to wait for.  The
   * thread resumes on producer_done, but doing it here as well means drain's
   * own deadline is not spent waiting for the next poll.
   */

  if (pb->paused && ioctl(pb->s.fd, AUDIOIOC_RESUME, 0) == 0)
    {
      pb->paused = false;
    }

  pending = pb->level + (size_t)pb->inflight * pb->s.buffersize;
  deadline = vs_audio_now_ms() + VS_AUDIO_DRAIN_SLACK_MS +
             (pb->bytes_per_sec > 0 ?
              (uint64_t)pending * 1000u / pb->bytes_per_sec : 0);

  /* A trailing odd byte can never become a whole sample, so it must not keep
   * this loop waiting; vs_audio_ring_take() rounds to whole samples too.
   */

  while (!pb->stopped &&
         (pb->level >= sizeof(int16_t) || pb->inflight > 0))
    {
      pthread_mutex_unlock(&pb->lock);

      if (vs_audio_now_ms() > deadline)
        {
          printf("vs_audio: playback did not drain in time\n");
          return;
        }

      usleep(VS_AUDIO_WAIT_MS * 1000);
      pthread_mutex_lock(&pb->lock);
    }

  pthread_mutex_unlock(&pb->lock);
}

void vs_audio_playback_stop(struct vs_audio_pb_s *pb)
{
  if (pb == NULL)
    {
      return;
    }

  pthread_mutex_lock(&pb->lock);
  if (!pb->stopped)
    {
      pb->stopped = true;

      /* Drop what has not reached the driver and let the driver flush what
       * has, so the speaker goes quiet now rather than after the tail.
       */

      pb->level = 0;
      pb->head = 0;
      pb->tail = 0;
      if (pb->s.started)
        {
          (void)ioctl(pb->s.fd, AUDIOIOC_STOP, 0);
          pb->s.started = false;
        }
    }

  pthread_mutex_unlock(&pb->lock);
}

unsigned int vs_audio_playback_underruns(struct vs_audio_pb_s *pb)
{
  unsigned int count;

  if (pb == NULL)
    {
      return 0;
    }

  pthread_mutex_lock(&pb->lock);
  count = pb->underruns;
  pthread_mutex_unlock(&pb->lock);
  return count;
}

/* Fold this session's outcome into the remembered watermark.
 *
 * Called from close() rather than from the thread so it happens once per
 * reply, with the final underrun count in hand.
 */

static void vs_audio_pb_adapt(struct vs_audio_pb_s *pb)
{
  unsigned int next = g_pb_watermark_ms;

  if (pb->iob_min != INT_MAX)
    {
      printf("vs_audio: network buffers low-water %d of %d\n",
             pb->iob_min, CONFIG_IOB_NBUFFERS);
    }

  if (pb->underruns != 0)
    {
      next *= VS_AUDIO_PB_WATERMARK_RISE;
      if (next > CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MAX_MS)
        {
          next = CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MAX_MS;
        }
    }
  else if (next > CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS)
    {
      next = next > VS_AUDIO_PB_WATERMARK_FALL_MS +
             CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS ?
             next - VS_AUDIO_PB_WATERMARK_FALL_MS :
             CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS;
    }

  if (next != g_pb_watermark_ms)
    {
      printf("vs_audio: playback watermark %u -> %u ms after %u underrun(s)\n",
             g_pb_watermark_ms, next, pb->underruns);
      g_pb_watermark_ms = next;
    }
}

void vs_audio_playback_close(struct vs_audio_pb_s *pb)
{
  if (pb == NULL)
    {
      return;
    }

  vs_audio_pb_adapt(pb);

  pthread_mutex_lock(&pb->lock);
  pb->closing = true;
  pthread_mutex_unlock(&pb->lock);

  if (pb->thread_valid)
    {
      pthread_join(pb->thread, NULL);
      pb->thread_valid = false;
    }

  vs_audio_stream_close(&pb->s);

  if (pb->ring_psram)
    {
      bk7258_psram_free(pb->ring);
    }
  else
    {
      free(pb->ring);
    }

  pthread_mutex_destroy(&pb->lock);
  free(pb);
}
