/****************************************************************************
 * app/audio_test/audio_test_main.c
 *
 * BK7258 internal-DAC audio bring-up diagnostic command.
 *
 * This is primarily a *measurement* tool.  The board has no writable
 * filesystem (the AP config mounts only procfs) and the AP console is a
 * mailbox command-injection path rather than a transparent byte stream, so
 * there is nowhere to put a WAV file and no way to pull one back out.
 * Printing numbers over the console is therefore the only practical way to
 * answer most of the questions that matter on first bring-up:
 *
 *   play  Does the internal DAC actually drive the HT6872?  Synthesises a
 *         sine wave, so "did anything come out of the speaker" is decided
 *         by ear while the command reports underruns.
 *
 *   rec   Do the analog MIC channels deliver plausible samples?  Reports
 *         per-channel DC offset, RMS and peak.
 *
 *   loop  Is the AEC feedback network wired the way the schematic says?
 *         Plays a sine and captures at the same time, then reports the
 *         correlation between the two captured channels.  The schematic
 *         taps HT6872 OUT+/OUT- through R84/R83 into MIC2P/MIC2N, so the
 *         right channel must track the played tone while the left channel
 *         (the real microphone) does not.  A right-channel RMS that rises
 *         with playback is the only objective evidence that the R83/R84
 *         return path exists.
 *
 *   recplay
 *         Does the capture path preserve something a human recognises?
 *         Statistics prove the ADC is delivering plausible samples, but
 *         only listening proves the samples are actually the sound in the
 *         room.  Records into a heap buffer, then plays that buffer back
 *         out the speaker.  The two phases are sequential and each owns
 *         its device, so this works even though there is nowhere to store
 *         a file: the recording never leaves RAM.
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

#include <arch/chip/bk7258_psram.h>

#include "audio_test_ogg.h"
#include "audio_test_stream.h"
#include <math.h>
#include <mqueue.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Private diagnostic ioctl understood by the BK7258 audio lower half.
 *
 * The number is duplicated here on purpose: applications do not get the
 * board's private src directory on their include path, and exporting a
 * board header into apps/ just to share one constant would be worse
 * coupling than a documented duplicate.  The authoritative definition is
 * BK7258_AUDIOIOC_DIAG in
 * vendor/beken/boards/bk7258/bk7258-ap/src/bk7258_audio_bringup.h -- keep
 * the two in step.
 */

#define AUDIO_TEST_IOC_DIAG     _AUDIOIOC(200)

/* Mirrors BK7258_AUDIOIOC_SET_CAPGAIN and its argument in
 * board/.../src/bk7258_audio_bringup.h, which applications cannot include.
 * Keep the two in step.
 */

#define AUDIO_TEST_IOC_CAPGAIN  _AUDIOIOC(201)

struct audio_test_capgain_s
{
  uint8_t mic_gain;
  uint8_t adc_gain;
  bool    hpf;
};

/* Gain staging left as the driver has it until asked otherwise: -1 means
 * "do not send the ioctl", so a plain run still measures the current
 * baseline rather than silently a different one.
 */

#define AUDIO_TEST_GAIN_UNSET   (-1)

#define AUDIO_TEST_PLAY_DEV     "/dev/audio/pcm0p"
#define AUDIO_TEST_CAP_DEV      "/dev/audio/pcm0c"

#define AUDIO_TEST_MQ_PLAY      "audio_test_p"
#define AUDIO_TEST_MQ_CAP       "audio_test_c"

#define AUDIO_TEST_MAX_BUFFERS  8

/* Poll granularity while waiting for buffer-done messages. */

#define AUDIO_TEST_POLL_MS      50

/* Defaults.  16 kHz is the driver's default rate and is reachable from the
 * 26 MHz crystal; 44.1k/48k need the APLL sequence, which the chip driver
 * rejects with -ENOTSUP for now.
 */

#define AUDIO_TEST_DEF_SECONDS  3
#define AUDIO_TEST_DEF_RATE     16000
#define AUDIO_TEST_DEF_TONE_HZ  1000

/* Playback runs shorter than capture by default: capture needs a few
 * seconds to average a meaningful RMS, whereas playback only has to be
 * long enough to hear.
 */

#define AUDIO_TEST_DEF_PLAY_SECONDS 1

/* Default tone amplitude, in raw 16-bit sample units.
 *
 * Deliberately quiet.  The first hardware run used 16000 (half of full
 * scale) into the HT6872 and was uncomfortably loud in a shared room --
 * a diagnostic tone only has to be audible and measurable, not loud.  320
 * is 1/50 of that.  Raise it with -a when a louder tone is genuinely
 * needed, e.g. to lift the MIC2 echo reference clear of the noise floor.
 */

#define AUDIO_TEST_DEF_AMPLITUDE 320

/* Captured buffers discarded before statistics start.
 *
 * The analog front end needs time to settle after the ADC is
 * enabled, and the first samples come back at full scale.  Without
 * this, a quiet room still reported peak 32767 against an RMS of
 * about 1100 -- a peak-to-RMS ratio near 30, which is a startup
 * transient rather than anything acoustic.
 */

#define AUDIO_TEST_SETTLE_BUFFERS 8

/* Default duration of the record-then-listen test.
 *
 * Long enough to say a short sentence, short enough that the clip fits in
 * the main heap: the AP has 336 KiB of SRAM total and PSRAM is a separate
 * mm_initialize() heap that plain malloc() does not reach, so at 16 kHz
 * mono the practical ceiling is a handful of seconds.
 */

#define AUDIO_TEST_DEF_RECPLAY_SECONDS 5

/* Default length and bitrate of an 'opus' capture.
 *
 * Five seconds is a sentence, which is what a recogniser needs to be worth
 * asking.  24 kbps is above what wideband speech needs for intelligibility
 * and still leaves the base64 dump short enough to cross this console in a
 * couple of seconds.
 */

#define AUDIO_TEST_DEF_OPUS_SECONDS    5
#define AUDIO_TEST_DEF_OPUS_BITRATE    24000

/* Defaults for continuous chunked capture.
 *
 * Two seconds per chunk is the figure the upload plan settled on: long
 * enough that the per-chunk request overhead is noise next to the payload,
 * short enough that a recogniser sees a phrase soon after it is spoken.
 *
 * Thirty seconds is the default session length only because a command has to
 * stop by itself; -t sets whatever is wanted.  It is deliberately several
 * times the chunk length so a run exercises the ring rather than filling it
 * once.
 */

#define AUDIO_TEST_DEF_CHUNK_MS        2000
#define AUDIO_TEST_DEF_STREAM_SECONDS  30

/* How often the session prints its counters while running. */

#define AUDIO_TEST_STREAM_REPORT_S     5

/* Target peak the recording is scaled to before it is played back.
 *
 * A quiet room captures at a peak of about 127 out of 32767 with the analog
 * microphone gain left at the vendor default of 0.  Handing that straight
 * to the DAC would be inaudible -- the tone test needs an amplitude of 960
 * just to be heard -- so the stored clip is scaled up before playback.
 *
 * The scaling is applied after the statistics have been reported, so the
 * reported DC, RMS and peak still describe what the ADC actually delivered
 * rather than what came out of the speaker.
 */

#define AUDIO_TEST_DEF_PLAY_PEAK 6000

/* Ceiling on the normalisation factor.  The microphone noise floor is
 * amplified along with the wanted signal, so an unbounded gain would turn
 * a silent room into loud hiss.
 */

#define AUDIO_TEST_MAX_GAIN 64

/* Give up on playback after this many consecutive idle poll intervals.
 *
 * Without it a driver that stops returning buffers would leave the drain
 * loop spinning forever.
 */

#define AUDIO_TEST_PLAY_IDLE_LIMIT 200

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One direction of the test: a device, its message queue and its buffers. */

struct audio_test_stream_s
{
  int fd;
  mqd_t mq;
  const char *mqname;
  bool started;

  unsigned int nbuffers;
  unsigned int buffersize;
  struct ap_buffer_s *buffers[AUDIO_TEST_MAX_BUFFERS];

  unsigned int inflight;
};

/* Running per-channel statistics.  Sums are kept in 64-bit so a few
 * seconds at 16 kHz cannot overflow them.
 */

struct audio_test_chstat_s
{
  int64_t sum;
  uint64_t sumsq;
  int32_t peak;
  uint32_t count;
};

struct audio_test_stats_s
{
  struct audio_test_chstat_s ch[2];

  /* Cross term for the L/R correlation, accumulated after the fact from
   * the raw sums (see audio_test_report()).
   */

  int64_t cross;

  unsigned int frames;
  unsigned int skipped;
  unsigned int played;
  unsigned int underruns;
  unsigned int ioerrors;
};

struct audio_test_cfg_s
{
  unsigned int seconds;
  unsigned int samplerate;
  unsigned int tone_hz;
  unsigned int amplitude;           /* raw 16-bit sample peak */
  unsigned int play_peak;           /* recplay target peak, 0: no scaling */
  unsigned int bitrate;             /* opus target bits per second */
  bool ogg;                         /* encode to Ogg Opus and dump base64 */
  int dac_volume;                   /* -1: leave alone, else 0..1000 */
  int mic_gain;                     /* analog, 0..15, or _UNSET */
  int adc_gain;                     /* digital, 0..63, or _UNSET */
  int hpf;                          /* 0/1 high-pass stages, or _UNSET */
  const char *pcm_host;             /* non-NULL: send raw PCM there */
  int pcm_port;
  const char *stream_host;          /* non-NULL: chunked upload target */
  int stream_port;
  unsigned int chunk_ms;            /* chunk length for 'stream' */
};

/* A recording held in the heap.  Capture appends to it and playback drains
 * it, so one buffer serves both phases of the record-then-listen test.
 */

struct audio_test_clip_s
{
  uint8_t *data;
  size_t capacity;
  size_t used;                      /* bytes written by capture */
  size_t played;                    /* bytes handed back to playback */
  unsigned int settle;              /* capture buffers still to discard */
  bool psram;                       /* data came from the PSRAM heap */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Sine phase, carried across buffer refills so the tone is continuous. */

static float g_tone_phase;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void audio_test_usage(void)
{
  printf("Usage: audio_test <play|rec|loop|recplay|opus|diag> [options]\n"
         "  play     sine out the speaker\n"
         "  rec      capture and report signal statistics\n"
         "  loop     live microphone to speaker passthrough\n"
         "  recplay  record into RAM, then play the recording back\n"
         "  opus     record, encode to Ogg Opus, print it as base64\n"
         "  dump     print the last encoded file again\n"
         "  send <ip> <port>\n"
         "           send the last encoded file over TCP\n"
         "  pcm <ip> <port>\n"
         "           record, then send raw PCM over TCP (no encoder, so\n"
         "           the samples are what the ADC produced)\n"
         "  stream <ip> <port>\n"
         "           capture continuously and upload fixed-length chunks\n"
         "           while still recording; capture is never blocked by\n"
         "           the network, chunks are dropped instead\n"
         "  -c <ms>    stream: chunk length, default %d\n"
         "  diag     capture with a driver register dump\n"
         "  -b <bps>   opus bitrate, default %d\n"
         "  -t <sec>   duration, default %d\n"
         "  -r <rate>  sample rate (8000|16000|32000), default %d\n"
         "  -f <hz>    tone frequency for play/loop, default %d\n"
         "  -g <peak>  recplay: scale the recording to this peak before\n"
         "             playback, 0 to play it raw, default %d\n"
         "  -v <0..1000> DAC volume, default: leave as configured\n"
         "  -m <0..15>   analog MIC gain (ahead of the ADC; raising this\n"
         "               is the only way to improve SNR, and too much\n"
         "               clips transients irrecoverably)\n"
         "  -d <0..63>   ADC digital gain, 0x2d (45) is 0 dB\n"
         "  -p <0|1>     ADC high-pass stages, 0 bypasses them\n",
         AUDIO_TEST_DEF_CHUNK_MS, AUDIO_TEST_DEF_OPUS_BITRATE,
         AUDIO_TEST_DEF_SECONDS, AUDIO_TEST_DEF_RATE,
         AUDIO_TEST_DEF_TONE_HZ, AUDIO_TEST_DEF_PLAY_PEAK);
}

/****************************************************************************
 * Name: audio_test_stat_add
 ****************************************************************************/

static void audio_test_stat_add(struct audio_test_chstat_s *st,
                                int16_t sample)
{
  int32_t abs;

  st->sum += sample;
  st->sumsq += (uint64_t)((int32_t)sample * (int32_t)sample);

  abs = sample < 0 ? -(int32_t)sample : (int32_t)sample;
  if (abs > st->peak)
    {
      st->peak = abs;
    }

  st->count++;
}

/****************************************************************************
 * Name: audio_test_accumulate
 *
 * Description:
 *   Fold one captured buffer into the statistics.  With the MIC2 echo
 *   reference enabled the stream is L/R interleaved, so an even sample
 *   index is the microphone and an odd one the reference.
 *
 ****************************************************************************/

static void audio_test_accumulate(struct audio_test_stats_s *stats,
                                  const struct ap_buffer_s *apb,
                                  unsigned int nchannels)
{
  const int16_t *samples = (const int16_t *)apb->samp;
  unsigned int nsamples = apb->nbytes / sizeof(int16_t);
  unsigned int i;

  if (stats->skipped < AUDIO_TEST_SETTLE_BUFFERS)
    {
      stats->skipped++;
      return;
    }

  if (nchannels == 2)
    {
      for (i = 0; i + 1 < nsamples; i += 2)
        {
          audio_test_stat_add(&stats->ch[0], samples[i]);
          audio_test_stat_add(&stats->ch[1], samples[i + 1]);
          stats->cross += (int32_t)samples[i] * (int32_t)samples[i + 1];
        }
    }
  else
    {
      for (i = 0; i < nsamples; i++)
        {
          audio_test_stat_add(&stats->ch[0], samples[i]);
        }
    }

  stats->frames++;
}

/****************************************************************************
 * Name: audio_test_fill_tone
 *
 * Description:
 *   Fill a playback buffer with a mono sine wave.
 *
 ****************************************************************************/

static void audio_test_fill_tone(struct ap_buffer_s *apb,
                                 unsigned int samplerate,
                                 unsigned int tone_hz,
                                 unsigned int amplitude)
{
  int16_t *samples = (int16_t *)apb->samp;
  unsigned int nsamples = apb->nmaxbytes / sizeof(int16_t);
  float step = 2.0f * (float)M_PI * (float)tone_hz / (float)samplerate;
  float scale = (float)amplitude;
  unsigned int i;

  for (i = 0; i < nsamples; i++)
    {
      samples[i] = (int16_t)(scale * sinf(g_tone_phase));

      g_tone_phase += step;
      if (g_tone_phase >= 2.0f * (float)M_PI)
        {
          g_tone_phase -= 2.0f * (float)M_PI;
        }
    }

  apb->nbytes = nsamples * sizeof(int16_t);
  apb->curbyte = 0;
}

/****************************************************************************
 * Name: audio_test_clip_write
 *
 * Description:
 *   Append one captured buffer to the clip.  The opening buffers are
 *   dropped for the same reason the statistics drop them: the analog front
 *   end has not settled and storing them would put a burst of full-scale
 *   noise at the head of the playback.
 *
 ****************************************************************************/

static void audio_test_clip_write(struct audio_test_clip_s *clip,
                                  const struct ap_buffer_s *apb)
{
  size_t room;

  if (clip->settle > 0)
    {
      clip->settle--;
      return;
    }

  room = clip->capacity - clip->used;
  if (room == 0)
    {
      return;
    }

  if (apb->nbytes < room)
    {
      room = apb->nbytes;
    }

  memcpy(clip->data + clip->used, apb->samp, room);
  clip->used += room;
}

/****************************************************************************
 * Name: audio_test_clip_read
 *
 * Description:
 *   Fill a playback buffer from the clip.  Returns the number of bytes
 *   supplied, which is zero once the whole clip has been handed out.
 *
 ****************************************************************************/

static size_t audio_test_clip_read(struct audio_test_clip_s *clip,
                                   struct ap_buffer_s *apb)
{
  size_t copy = clip->used - clip->played;

  if (copy > apb->nmaxbytes)
    {
      copy = apb->nmaxbytes;
    }

  if (copy > 0)
    {
      memcpy(apb->samp, clip->data + clip->played, copy);
      clip->played += copy;
    }

  apb->nbytes = copy;
  apb->curbyte = 0;
  return copy;
}

/****************************************************************************
 * Name: audio_test_clip_ms
 *
 * Description:
 *   Duration of the captured audio in milliseconds.
 *
 ****************************************************************************/

static unsigned int audio_test_clip_ms(const struct audio_test_clip_s *clip,
                                       unsigned int samplerate,
                                       unsigned int nchannels)
{
  unsigned int bps = samplerate * nchannels * (unsigned int)sizeof(int16_t);

  if (bps == 0)
    {
      return 0;
    }

  return (unsigned int)(((uint64_t)clip->used * 1000ull) / bps);
}

/****************************************************************************
 * Name: audio_test_clip_normalise
 *
 * Description:
 *   Scale the clip so that its loudest sample lands near target_peak.
 *
 *   Uses an integer factor: this is a bring-up aid, and a coarse gain that
 *   is obviously just a multiply is easier to reason about than a resampled
 *   or dithered one when the question being answered is only "does the
 *   captured audio sound like the room".
 *
 ****************************************************************************/

static void audio_test_clip_normalise(struct audio_test_clip_s *clip,
                                      unsigned int target_peak)
{
  int16_t *samples = (int16_t *)clip->data;
  size_t nsamples = clip->used / sizeof(int16_t);
  int32_t peak = 0;
  unsigned int gain;
  size_t i;

  if (target_peak == 0 || nsamples == 0)
    {
      return;
    }

  for (i = 0; i < nsamples; i++)
    {
      int32_t abs = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];

      if (abs > peak)
        {
          peak = abs;
        }
    }

  if (peak == 0)
    {
      printf("audio_test: clip is digital silence, not scaling\n");
      return;
    }

  gain = (unsigned int)((int32_t)target_peak / peak);
  if (gain > AUDIO_TEST_MAX_GAIN)
    {
      gain = AUDIO_TEST_MAX_GAIN;
    }

  if (gain <= 1)
    {
      printf("audio_test: clip peak %ld already loud enough, not scaling\n",
             (long)peak);
      return;
    }

  for (i = 0; i < nsamples; i++)
    {
      int32_t value = (int32_t)samples[i] * (int32_t)gain;

      if (value > 32767)
        {
          value = 32767;
        }
      else if (value < -32768)
        {
          value = -32768;
        }

      samples[i] = (int16_t)value;
    }

  printf("audio_test: scaled clip %ux for playback (peak %ld -> %ld); the "
         "microphone noise floor is amplified too\n", gain, (long)peak,
         (long)(peak * (int32_t)gain));
}

/****************************************************************************
 * Name: audio_test_enqueue
 ****************************************************************************/

static int audio_test_enqueue(struct audio_test_stream_s *stream,
                              struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s desc;
  int ret;

  desc.numbytes = apb->nbytes;
  desc.u.buffer = apb;

  ret = ioctl(stream->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
  if (ret < 0)
    {
      return -errno;
    }

  stream->inflight++;
  return OK;
}

/****************************************************************************
 * Name: audio_test_open
 *
 * Description:
 *   Open one direction and run the standard reserve/configure/allocate/
 *   register sequence, mirroring apps/system/nxrecorder.
 *
 ****************************************************************************/

static int audio_test_open(struct audio_test_stream_s *stream,
                           const char *devpath, const char *mqname,
                           bool playback, unsigned int nchannels,
                           const struct audio_test_cfg_s *cfg)
{
  struct audio_caps_desc_s cap_desc;
  struct ap_buffer_info_s buf_info;
  struct audio_buf_desc_s desc;
  struct mq_attr attr;
  unsigned int i;
  int ret;

  memset(stream, 0, sizeof(*stream));
  stream->fd = -1;
  stream->mq = (mqd_t)-1;
  stream->mqname = mqname;

  stream->fd = open(devpath, O_RDWR | O_CLOEXEC);
  if (stream->fd < 0)
    {
      printf("audio_test: cannot open %s: %d\n", devpath, errno);
      return -errno;
    }

  if (ioctl(stream->fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      printf("audio_test: RESERVE failed on %s: %d\n", devpath, errno);
      ret = -errno;
      goto err;
    }

  /* Gain staging goes in before CONFIGURE, which is where the driver runs
   * adc_setup() and latches these into the analog front end.
   */

  if (!playback && (cfg->mic_gain != AUDIO_TEST_GAIN_UNSET ||
                    cfg->adc_gain != AUDIO_TEST_GAIN_UNSET ||
                    cfg->hpf != AUDIO_TEST_GAIN_UNSET))
    {
      struct audio_test_capgain_s gain;

      gain.mic_gain = cfg->mic_gain == AUDIO_TEST_GAIN_UNSET ?
                      0 : (uint8_t)cfg->mic_gain;
      gain.adc_gain = cfg->adc_gain == AUDIO_TEST_GAIN_UNSET ?
                      0x2d : (uint8_t)cfg->adc_gain;
      gain.hpf      = cfg->hpf == AUDIO_TEST_GAIN_UNSET ?
                      false : cfg->hpf != 0;

      if (ioctl(stream->fd, AUDIO_TEST_IOC_CAPGAIN,
                (unsigned long)&gain) < 0)
        {
          printf("audio_test: CAPGAIN failed: %d\n", errno);
          ret = -errno;
          goto err;
        }

      printf("audio_test: mic_gain=%u adc_gain=0x%02x hpf=%d\n",
             gain.mic_gain, gain.adc_gain, gain.hpf);
    }

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels = nchannels;
  cap_desc.caps.ac_controls.hw[0] = cfg->samplerate & 0xffff;
  cap_desc.caps.ac_controls.b[3] = cfg->samplerate >> 16;
  cap_desc.caps.ac_controls.b[2] = 16;
  cap_desc.caps.ac_subtype = AUDIO_FMT_PCM;

  if (ioctl(stream->fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      printf("audio_test: CONFIGURE failed on %s: %d "
             "(rate %u, %u channel(s))\n",
             devpath, errno, cfg->samplerate, nchannels);
      ret = -errno;
      goto err;
    }

  if (playback && cfg->dac_volume >= 0)
    {
      memset(&cap_desc, 0, sizeof(cap_desc));
      cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
      cap_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
      cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
      cap_desc.caps.ac_controls.hw[0] = cfg->dac_volume;

      if (ioctl(stream->fd, AUDIOIOC_CONFIGURE,
                (unsigned long)&cap_desc) < 0)
        {
          printf("audio_test: volume request rejected: %d\n", errno);
        }
    }

  /* Buffer geometry.  This call is not merely informational: the upper
   * half latches its own buffer quota (upper->nbuffers) from the value the
   * lower half returns here, and without it every AUDIOIOC_ALLOCBUFFER
   * returns 0 immediately.  A local fallback cannot substitute for it, so
   * treat a failure as fatal rather than papering over it.
   */

  if (ioctl(stream->fd, AUDIOIOC_GETBUFFERINFO,
            (unsigned long)&buf_info) < 0)
    {
      printf("audio_test: GETBUFFERINFO failed on %s: %d "
             "(driver must answer this or no buffer can be allocated)\n",
             devpath, errno);
      ret = -errno;
      goto err;
    }

  stream->buffersize = buf_info.buffer_size;
  stream->nbuffers = buf_info.nbuffers;
  if (stream->nbuffers > AUDIO_TEST_MAX_BUFFERS)
    {
      stream->nbuffers = AUDIO_TEST_MAX_BUFFERS;
    }

  for (i = 0; i < stream->nbuffers; i++)
    {
      desc.numbytes = stream->buffersize;
      desc.u.pbuffer = &stream->buffers[i];

      /* A return of 0 is not an error code: audio.c:786-789 uses it to say
       * the upper half's buffer quota is already exhausted.  Anything <= 0
       * means we did not get a buffer.
       */

      ret = ioctl(stream->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
      if (ret <= 0 || stream->buffers[i] == NULL)
        {
          printf("audio_test: ALLOCBUFFER %u of %u returned %d%s\n",
                 i, stream->nbuffers, ret,
                 ret == 0 ? " (upper-half buffer quota exhausted)" : "");
          stream->nbuffers = i;
          ret = -ENOMEM;
          goto err;
        }
    }

  attr.mq_maxmsg = stream->nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags = 0;

  stream->mq = mq_open(mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (stream->mq == (mqd_t)-1)
    {
      printf("audio_test: mq_open(%s) failed: %d\n", mqname, errno);
      ret = -errno;
      goto err;
    }

  if (ioctl(stream->fd, AUDIOIOC_REGISTERMQ, (unsigned long)stream->mq) < 0)
    {
      printf("audio_test: REGISTERMQ failed: %d\n", errno);
      ret = -errno;
      goto err;
    }

  return OK;

err:
  if (stream->mq != (mqd_t)-1)
    {
      mq_close(stream->mq);
      mq_unlink(mqname);
      stream->mq = (mqd_t)-1;
    }

  if (stream->fd >= 0)
    {
      close(stream->fd);
      stream->fd = -1;
    }

  return ret;
}

/****************************************************************************
 * Name: audio_test_close
 ****************************************************************************/

static void audio_test_close(struct audio_test_stream_s *stream)
{
  struct audio_buf_desc_s desc;
  unsigned int i;

  if (stream->fd < 0)
    {
      return;
    }

  if (stream->started)
    {
      ioctl(stream->fd, AUDIOIOC_STOP, 0);
      stream->started = false;
    }

  if (stream->mq != (mqd_t)-1)
    {
      ioctl(stream->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)stream->mq);
    }

  for (i = 0; i < stream->nbuffers; i++)
    {
      if (stream->buffers[i] != NULL)
        {
          desc.u.buffer = stream->buffers[i];
          ioctl(stream->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          stream->buffers[i] = NULL;
        }
    }

  ioctl(stream->fd, AUDIOIOC_RELEASE, 0);

  if (stream->mq != (mqd_t)-1)
    {
      mq_close(stream->mq);
      mq_unlink(stream->mqname);
      stream->mq = (mqd_t)-1;
    }

  close(stream->fd);
  stream->fd = -1;
}

/****************************************************************************
 * Name: audio_test_poll
 *
 * Description:
 *   Drain whatever the driver has posted for one stream, without blocking
 *   for longer than AUDIO_TEST_POLL_MS.
 *
 *   For capture, each returned buffer is folded into stats and re-enqueued
 *   so the recording is continuous.  For playback, each returned buffer is
 *   refilled with the next slice of the tone and re-enqueued.
 *
 ****************************************************************************/

static void audio_test_poll(struct audio_test_stream_s *stream,
                            struct audio_test_stats_s *stats,
                            const struct audio_test_cfg_s *cfg,
                            bool playback, unsigned int nchannels,
                            bool keep_going,
                            struct audio_test_clip_s *clip)
{
  struct audio_msg_s msg;
  struct timespec ts;
  unsigned int prio;
  ssize_t got;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += AUDIO_TEST_POLL_MS * 1000000;
  if (ts.tv_nsec >= 1000000000)
    {
      ts.tv_nsec -= 1000000000;
      ts.tv_sec++;
    }

  got = mq_timedreceive(stream->mq, (char *)&msg, sizeof(msg), &prio, &ts);
  if (got != sizeof(msg))
    {
      return;
    }

  switch (msg.msg_id)
    {
      case AUDIO_MSG_DEQUEUE:
        {
          struct ap_buffer_s *apb = msg.u.ptr;

          if (apb == NULL)
            {
              break;
            }

          if (stream->inflight > 0)
            {
              stream->inflight--;
            }

          if (playback)
            {
              stats->played++;

              if (keep_going)
                {
                  if (clip == NULL)
                    {
                      audio_test_fill_tone(apb, cfg->samplerate,
                                           cfg->tone_hz, cfg->amplitude);
                    }
                  else if (audio_test_clip_read(clip, apb) == 0)
                    {
                      /* Clip drained: let this buffer stay out of the
                       * queue so the stream winds down on its own.
                       */

                      break;
                    }

                  audio_test_enqueue(stream, apb);
                }
            }
          else
            {
              audio_test_accumulate(stats, apb, nchannels);

              if (clip != NULL)
                {
                  audio_test_clip_write(clip, apb);
                }

              if (keep_going)
                {
                  apb->nbytes = 0;
                  audio_test_enqueue(stream, apb);
                }
            }
        }
        break;

      case AUDIO_MSG_UNDERRUN:
        stats->underruns++;
        break;

      case AUDIO_MSG_IOERR:
        stats->ioerrors++;
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: audio_test_report
 ****************************************************************************/

static void audio_test_report(const struct audio_test_stats_s *stats,
                              unsigned int nchannels)
{
  double mean[2];
  double rms[2];
  unsigned int i;

  printf("audio_test: %u buffer(s) measured, %u discarded while settling,"
         " underruns=%u ioerrors=%u\n",
         stats->frames, stats->skipped, stats->underruns, stats->ioerrors);

  for (i = 0; i < nchannels; i++)
    {
      const struct audio_test_chstat_s *st = &stats->ch[i];
      double var;

      if (st->count == 0)
        {
          printf("  ch%u (%s): no samples\n", i,
                 i == 0 ? "MIC1 voice" : "MIC2 echo ref");
          mean[i] = 0.0;
          rms[i] = 0.0;
          continue;
        }

      mean[i] = (double)st->sum / (double)st->count;
      var = (double)st->sumsq / (double)st->count - mean[i] * mean[i];
      rms[i] = var > 0.0 ? sqrt(var) : 0.0;

      printf("  ch%u (%-14s): n=%-7lu dc=%-8.1f rms=%-8.1f peak=%ld\n",
             i, i == 0 ? "MIC1 voice" : "MIC2 echo ref",
             (unsigned long)st->count, mean[i], rms[i],
             (long)st->peak);
    }

  /* Correlation between the microphone and the echo reference, computed
   * from the accumulated sums so no second pass over the data is needed.
   * Near zero means the two channels are unrelated; a large magnitude
   * means the reference really is carrying the played signal.
   */

  if (nchannels == 2 && stats->ch[0].count > 0 &&
      rms[0] > 1.0 && rms[1] > 1.0)
    {
      double n = (double)stats->ch[0].count;
      double cov = (double)stats->cross / n - mean[0] * mean[1];

      printf("  corr(MIC1, MIC2ref) = %+.3f\n", cov / (rms[0] * rms[1]));
    }
}

/****************************************************************************
 * Name: audio_test_run
 ****************************************************************************/

static int audio_test_run(bool playback, bool capture, bool diag,
                          const struct audio_test_cfg_s *cfg)
{
  struct audio_test_stream_s play;
  struct audio_test_stream_s cap;
  struct audio_test_stats_s stats;
  unsigned int cap_channels;
  unsigned int i;
  time_t deadline;
  int ret = OK;

  memset(&stats, 0, sizeof(stats));
  memset(&play, 0, sizeof(play));
  memset(&cap, 0, sizeof(cap));
  play.fd = -1;
  cap.fd = -1;

  /* The capture channel count is a property of the wiring, not a choice:
   * with the MIC2 echo reference compiled in, frames are L/R interleaved.
   */

#ifdef CONFIG_BK7258_AUDIO_AEC_REFERENCE
  cap_channels = 2;
#else
  cap_channels = 1;
#endif

  if (playback)
    {
      ret = audio_test_open(&play, AUDIO_TEST_PLAY_DEV, AUDIO_TEST_MQ_PLAY,
                            true, 1, cfg);
      if (ret < 0)
        {
          goto done;
        }
    }

  if (capture)
    {
      ret = audio_test_open(&cap, AUDIO_TEST_CAP_DEV, AUDIO_TEST_MQ_CAP,
                            false, cap_channels, cfg);
      if (ret < 0)
        {
          goto done;
        }
    }

  /* Prime the queues before starting so the DAC does not run dry between
   * START and the first refill.
   */

  if (playback)
    {
      for (i = 0; i < play.nbuffers; i++)
        {
          audio_test_fill_tone(play.buffers[i], cfg->samplerate,
                               cfg->tone_hz, cfg->amplitude);
          audio_test_enqueue(&play, play.buffers[i]);
        }
    }

  if (capture)
    {
      for (i = 0; i < cap.nbuffers; i++)
        {
          cap.buffers[i]->nbytes = 0;
          audio_test_enqueue(&cap, cap.buffers[i]);
        }
    }

  if (capture)
    {
      if (ioctl(cap.fd, AUDIOIOC_START, 0) < 0)
        {
          printf("audio_test: capture START failed: %d\n", errno);
          ret = -errno;
          goto done;
        }

      cap.started = true;

      if (diag)
        {
          /* Issued with the ADC already enabled and streaming, which is
           * the only state in which the driver's polled read means
           * anything.
           */

          if (ioctl(cap.fd, AUDIO_TEST_IOC_DIAG, 0) < 0)
            {
              printf("audio_test: diag ioctl failed: %d\n", errno);
            }
        }
    }

  if (playback)
    {
      if (ioctl(play.fd, AUDIOIOC_START, 0) < 0)
        {
          printf("audio_test: playback START failed: %d\n", errno);
          ret = -errno;
          goto done;
        }

      play.started = true;
    }

  printf("audio_test: running %u s at %u Hz%s%s\n", cfg->seconds,
         cfg->samplerate,
         playback ? ", tone on" : "",
         capture ? ", capturing" : "");

  deadline = time(NULL) + (time_t)cfg->seconds;

  while (time(NULL) < deadline)
    {
      if (playback)
        {
          audio_test_poll(&play, &stats, cfg, true, 1, true, NULL);
        }

      if (capture)
        {
          audio_test_poll(&cap, &stats, cfg, false, cap_channels, true,
                          NULL);
        }
    }

  /* Let the buffers already handed to the driver come back so the report
   * covers them, but do not re-enqueue any more.
   */

  for (i = 0; i < 20; i++)
    {
      if (playback && play.inflight > 0)
        {
          audio_test_poll(&play, &stats, cfg, true, 1, false, NULL);
        }

      if (capture && cap.inflight > 0)
        {
          audio_test_poll(&cap, &stats, cfg, false, cap_channels, false,
                          NULL);
        }

      if ((!playback || play.inflight == 0) &&
          (!capture || cap.inflight == 0))
        {
          break;
        }
    }

  if (capture)
    {
      audio_test_report(&stats, cap_channels);
    }
  else
    {
      printf("audio_test: %u buffer(s) played, underruns=%u\n",
             stats.played, stats.underruns);
    }

done:
  audio_test_close(&play);
  audio_test_close(&cap);

  return ret;
}

/****************************************************************************
 * Name: audio_test_recplay
 *
 * Description:
 *   Record into a heap buffer, then play that buffer back out the speaker.
 *
 *   The two phases run one after the other and each opens and closes its
 *   own device, so nothing here depends on capture and playback being able
 *   to run at the same time (they can -- 'loop' proves it -- but keeping
 *   them apart makes the failure of one phase unambiguous).
 *
 ****************************************************************************/

static int audio_test_recplay(const struct audio_test_cfg_s *cfg)
{
  struct audio_test_stream_s stream;
  struct audio_test_stats_s stats;
  struct audio_test_clip_s clip;
  struct mallinfo mem;
  unsigned int cap_channels;
  unsigned int settle_ms;
  unsigned int idle;
  unsigned int i;
  size_t want;
  time_t deadline;
  int ret;

  /* Same reasoning as audio_test_run(): the capture channel count follows
   * the wiring, not a runtime choice.
   */

#ifdef CONFIG_BK7258_AUDIO_AEC_REFERENCE
  cap_channels = 2;
#else
  cap_channels = 1;
#endif

  memset(&clip, 0, sizeof(clip));

  /* Ask for the settling buffers on top of the requested duration, so the
   * caller still ends up with the audio they asked for after the discard.
   */

  want = (size_t)cfg->seconds * cfg->samplerate * cap_channels *
         sizeof(int16_t);
  want += (size_t)AUDIO_TEST_SETTLE_BUFFERS * CONFIG_AUDIO_BUFFER_NUMBYTES;

  mem = mallinfo();
  printf("audio_test: clip wants %zu KiB, heap has %zu KiB free\n",
         want / 1024, (size_t)mem.fordblks / 1024);

  /* PSRAM first, SRAM only as a fallback.
   *
   * The clip is the one large allocation here and it is touched at audio
   * rate, not at memory speed, so non-cacheable PSRAM costs it nothing.  Out
   * of SRAM it costs a great deal: this heap holds both frame buffers and
   * everything else, and a clip large enough to be useful starves the four
   * 2 KiB pipeline buffers the driver needs.  That is what "ALLOCBUFFER 0 of
   * 4 returned -1" was -- a 2 s clip fitting, and then leaving nothing for
   * the buffers that carry the audio into it.
   */

  clip.data = bk7258_psram_malloc(want);
  clip.psram = clip.data != NULL;
  if (clip.data == NULL)
    {
      clip.data = malloc(want);
    }

  if (clip.data == NULL)
    {
      printf("audio_test: %u s at %u Hz fits in neither PSRAM nor the "
             "heap; retry with a shorter -t\n", cfg->seconds,
             cfg->samplerate);
      return -ENOMEM;
    }

  printf("audio_test: clip of %zu KiB taken from %s\n", want / 1024,
         clip.psram ? "PSRAM" : "the SRAM heap");

  clip.capacity = want;
  clip.settle = AUDIO_TEST_SETTLE_BUFFERS;

  /* Phase 1: capture into the clip. */

  memset(&stats, 0, sizeof(stats));
  memset(&stream, 0, sizeof(stream));
  stream.fd = -1;

  ret = audio_test_open(&stream, AUDIO_TEST_CAP_DEV, AUDIO_TEST_MQ_CAP,
                        false, cap_channels, cfg);
  if (ret < 0)
    {
      goto free_clip;
    }

  for (i = 0; i < stream.nbuffers; i++)
    {
      stream.buffers[i]->nbytes = 0;
      audio_test_enqueue(&stream, stream.buffers[i]);
    }

  if (ioctl(stream.fd, AUDIOIOC_START, 0) < 0)
    {
      printf("audio_test: capture START failed: %d\n", errno);
      ret = -errno;
      audio_test_close(&stream);
      goto free_clip;
    }

  stream.started = true;

  settle_ms = (AUDIO_TEST_SETTLE_BUFFERS * CONFIG_AUDIO_BUFFER_NUMBYTES *
               1000u) /
              (cfg->samplerate * cap_channels *
               (unsigned int)sizeof(int16_t));

  printf("audio_test: recording %u s -- speak now (the first %u ms are "
         "dropped while the ADC settles)\n", cfg->seconds, settle_ms);

  /* One extra second of grace so the settling discard does not eat into
   * the requested duration; the capacity check ends the loop either way.
   */

  deadline = time(NULL) + (time_t)cfg->seconds + 1;

  while (time(NULL) < deadline && clip.used < clip.capacity)
    {
      audio_test_poll(&stream, &stats, cfg, false, cap_channels, true,
                      &clip);
    }

  for (i = 0; i < 20 && stream.inflight > 0; i++)
    {
      audio_test_poll(&stream, &stats, cfg, false, cap_channels, false,
                      &clip);
    }

  audio_test_close(&stream);

  printf("audio_test: captured %zu KiB = %u.%03u s\n", clip.used / 1024,
         audio_test_clip_ms(&clip, cfg->samplerate, cap_channels) / 1000,
         audio_test_clip_ms(&clip, cfg->samplerate, cap_channels) % 1000);

  audio_test_report(&stats, cap_channels);

  if (clip.used == 0)
    {
      printf("audio_test: nothing was captured, skipping playback\n");
      ret = -EIO;
      goto free_clip;
    }

  /* The DAC is mono (sys_hal_aud_dacr_en() is a vendor no-op stub), so if
   * the echo reference is compiled in the interleaved clip has to be
   * reduced to the microphone channel or it would play back at double
   * speed.
   */

  if (cap_channels == 2)
    {
      int16_t *samples = (int16_t *)clip.data;
      size_t nsamples = clip.used / sizeof(int16_t);
      size_t j;

      for (j = 0; 2 * j + 1 < nsamples; j++)
        {
          samples[j] = samples[2 * j];
        }

      clip.used = j * sizeof(int16_t);
      printf("audio_test: de-interleaved to %zu KiB of mono\n",
             clip.used / 1024);
    }

  audio_test_clip_normalise(&clip, cfg->play_peak);

  /* 'pcm' ships the capture untouched.
   *
   * Opus would be the wrong carrier for anything that examines the noise
   * itself: a 16 kbps encoder is built to spend its bits on what a listener
   * notices and to discard noise-like content, so a spectrum measured after
   * it describes the encoder as much as the microphone.  Raw PCM is also what
   * the upload protocol specifies, so this path is not only a diagnostic.
   */

  if (cfg->pcm_host != NULL)
    {
      ret = audio_test_send_raw(cfg->pcm_host, cfg->pcm_port,
                                clip.data, clip.used);
      goto free_clip;
    }

  /* 'opus' stops here: the point of that mode is to get a file off the
   * board, and playing the clip afterwards would only add a minute of
   * waiting to a command whose output is being captured by a script.
   */

  if (cfg->ogg)
    {
      ret = audio_test_ogg_opus_dump((const int16_t *)clip.data,
                                     clip.used / sizeof(int16_t),
                                     cfg->samplerate, cfg->bitrate);
      goto free_clip;
    }

  /* Phase 2: play the clip back. */

  memset(&stream, 0, sizeof(stream));
  stream.fd = -1;
  clip.played = 0;

  ret = audio_test_open(&stream, AUDIO_TEST_PLAY_DEV, AUDIO_TEST_MQ_PLAY,
                        true, 1, cfg);
  if (ret < 0)
    {
      goto free_clip;
    }

  for (i = 0; i < stream.nbuffers; i++)
    {
      if (audio_test_clip_read(&clip, stream.buffers[i]) == 0)
        {
          break;
        }

      audio_test_enqueue(&stream, stream.buffers[i]);
    }

  if (ioctl(stream.fd, AUDIOIOC_START, 0) < 0)
    {
      printf("audio_test: playback START failed: %d\n", errno);
      ret = -errno;
      audio_test_close(&stream);
      goto free_clip;
    }

  stream.started = true;

  printf("audio_test: playing it back\n");

  stats.played = 0;
  stats.underruns = 0;
  idle = 0;

  while ((clip.played < clip.used || stream.inflight > 0) &&
         idle < AUDIO_TEST_PLAY_IDLE_LIMIT)
    {
      unsigned int before = stats.played;

      audio_test_poll(&stream, &stats, cfg, true, 1, true, &clip);

      if (stats.played == before)
        {
          idle++;
        }
      else
        {
          idle = 0;
        }
    }

  if (idle >= AUDIO_TEST_PLAY_IDLE_LIMIT)
    {
      printf("audio_test: playback stalled with %zu byte(s) left\n",
             clip.used - clip.played);
    }

  printf("audio_test: played %u buffer(s), underruns=%u\n",
         stats.played, stats.underruns);

  audio_test_close(&stream);
  ret = OK;

free_clip:
  if (clip.psram)
    {
      bk7258_psram_free(clip.data);
    }
  else
    {
      free(clip.data);
    }

  return ret;
}

/****************************************************************************
 * Name: audio_test_stream_run
 *
 * Description:
 *   Capture continuously, cut the stream into fixed-length chunks and upload
 *   each one while the next is still being recorded.
 *
 *   This loop is deliberately not audio_test_poll(): that function folds
 *   every buffer into statistics and optionally into a clip, both of which
 *   are the wrong shape here.  What matters on this path is that the only
 *   work between receiving a buffer and re-enqueueing it is one strided copy
 *   into PSRAM -- no encode, no socket, nothing that can block -- because a
 *   buffer that is not returned promptly is an ADC overrun.  The encode and
 *   the upload happen on the other side of the ring, on a thread that is
 *   free to stall.
 *
 ****************************************************************************/

static int audio_test_stream_run(const struct audio_test_cfg_s *cfg)
{
  struct audio_test_stream_ctx_s *ctx;
  struct audio_test_stream_s cap;
  unsigned int cap_channels;
  unsigned int settle;
  unsigned int i;
  time_t deadline;
  time_t nextreport;
  int ret;

  /* As elsewhere, the channel count follows the wiring: with the AEC echo
   * reference compiled in the frames are L/R interleaved and only the left
   * channel is the microphone.
   */

#ifdef CONFIG_BK7258_AUDIO_AEC_REFERENCE
  cap_channels = 2;
#else
  cap_channels = 1;
#endif

  ret = audio_test_open(&cap, AUDIO_TEST_CAP_DEV, AUDIO_TEST_MQ_CAP,
                        false, cap_channels, cfg);
  if (ret < 0)
    {
      return ret;
    }

  ctx = audio_test_stream_open(cfg->stream_host, cfg->stream_port,
                               cfg->samplerate, cfg->chunk_ms,
                               cfg->bitrate);
  if (ctx == NULL)
    {
      audio_test_close(&cap);
      return -ENOMEM;
    }

  for (i = 0; i < cap.nbuffers; i++)
    {
      cap.buffers[i]->nbytes = 0;
      audio_test_enqueue(&cap, cap.buffers[i]);
    }

  if (ioctl(cap.fd, AUDIOIOC_START, 0) < 0)
    {
      printf("audio_test: capture START failed: %d\n", errno);
      ret = -errno;
      audio_test_stream_close(ctx);
      audio_test_close(&cap);
      return ret;
    }

  cap.started = true;
  settle = AUDIO_TEST_SETTLE_BUFFERS;

  printf("audio_test: capturing for %u s\n", cfg->seconds);

  deadline = time(NULL) + (time_t)cfg->seconds;
  nextreport = time(NULL) + AUDIO_TEST_STREAM_REPORT_S;

  while (time(NULL) < deadline)
    {
      struct audio_msg_s msg;
      struct timespec ts;
      unsigned int prio;
      ssize_t got;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += AUDIO_TEST_POLL_MS * 1000000;
      if (ts.tv_nsec >= 1000000000)
        {
          ts.tv_nsec -= 1000000000;
          ts.tv_sec++;
        }

      got = mq_timedreceive(cap.mq, (char *)&msg, sizeof(msg), &prio, &ts);
      if (got == sizeof(msg) && msg.msg_id == AUDIO_MSG_DEQUEUE &&
          msg.u.ptr != NULL)
        {
          struct ap_buffer_s *apb = msg.u.ptr;

          if (cap.inflight > 0)
            {
              cap.inflight--;
            }

          /* The analog front end returns full-scale samples for the first
           * few buffers after the ADC is enabled, so they are discarded
           * here for the same reason the statistics discard them.
           */

          if (settle > 0)
            {
              settle--;
            }
          else
            {
              audio_test_stream_feed(ctx, (const int16_t *)apb->samp,
                                     apb->nbytes / sizeof(int16_t),
                                     cap_channels);
            }

          apb->nbytes = 0;
          audio_test_enqueue(&cap, apb);
        }

      if (time(NULL) >= nextreport)
        {
          audio_test_stream_report(ctx);
          nextreport = time(NULL) + AUDIO_TEST_STREAM_REPORT_S;
        }
    }

  ioctl(cap.fd, AUDIOIOC_STOP, 0);
  cap.started = false;

  printf("audio_test: capture stopped, draining the upload queue\n");

  /* Closing flushes the tail of the recording, waits for the uploader and
   * prints the final counters.
   */

  audio_test_stream_close(ctx);
  audio_test_close(&cap);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct audio_test_cfg_s cfg;
  bool playback = false;
  bool capture = false;
  bool diag = false;
  bool recplay = false;

  /* Where the -x options begin.  'pcm' takes an address and a port as
   * positional arguments, so its options start two words later than
   * everything else's.
   */

  int argstart = 2;
  int i;

  cfg.seconds = AUDIO_TEST_DEF_SECONDS;
  cfg.samplerate = AUDIO_TEST_DEF_RATE;
  cfg.tone_hz = AUDIO_TEST_DEF_TONE_HZ;
  cfg.amplitude = AUDIO_TEST_DEF_AMPLITUDE;
  cfg.play_peak = AUDIO_TEST_DEF_PLAY_PEAK;
  cfg.bitrate = AUDIO_TEST_DEF_OPUS_BITRATE;
  cfg.ogg = false;
  cfg.dac_volume = -1;
  cfg.mic_gain = AUDIO_TEST_GAIN_UNSET;
  cfg.adc_gain = AUDIO_TEST_GAIN_UNSET;
  cfg.hpf = AUDIO_TEST_GAIN_UNSET;
  cfg.pcm_host = NULL;
  cfg.pcm_port = 0;
  cfg.stream_host = NULL;
  cfg.stream_port = 0;
  cfg.chunk_ms = AUDIO_TEST_DEF_CHUNK_MS;

  if (argc < 2)
    {
      audio_test_usage();
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "play") == 0)
    {
      playback = true;

      /* Keep the default audible but brief; -t still overrides. */

      cfg.seconds = AUDIO_TEST_DEF_PLAY_SECONDS;
    }
  else if (strcmp(argv[1], "rec") == 0)
    {
      capture = true;
    }
  else if (strcmp(argv[1], "loop") == 0)
    {
      playback = true;
      capture = true;
    }
  else if (strcmp(argv[1], "recplay") == 0)
    {
      recplay = true;
      cfg.seconds = AUDIO_TEST_DEF_RECPLAY_SECONDS;
    }
  else if (strcmp(argv[1], "dump") == 0)
    {
      return audio_test_ogg_redump() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "send") == 0)
    {
      if (argc < 4)
        {
          printf("Usage: audio_test send <ip> <port>\n");
          return EXIT_FAILURE;
        }

      return audio_test_ogg_send(argv[2], atoi(argv[3])) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "pcm") == 0)
    {
      /* Same capture path, no encoder: record then ship the samples as they
       * came out of the ADC.
       */

      if (argc < 4)
        {
          printf("Usage: audio_test pcm <ip> <port> [options]\n");
          return EXIT_FAILURE;
        }

      recplay = true;
      cfg.seconds = AUDIO_TEST_DEF_OPUS_SECONDS;
      cfg.pcm_host = argv[2];
      cfg.pcm_port = atoi(argv[3]);
      argstart = 4;
    }
  else if (strcmp(argv[1], "stream") == 0)
    {
      if (argc < 4)
        {
          printf("Usage: audio_test stream <ip> <port> [options]\n");
          return EXIT_FAILURE;
        }

      cfg.stream_host = argv[2];
      cfg.stream_port = atoi(argv[3]);
      cfg.seconds = AUDIO_TEST_DEF_STREAM_SECONDS;
      cfg.bitrate = AUDIO_TEST_DEF_OPUS_BITRATE;
      argstart = 4;
    }
  else if (strcmp(argv[1], "opus") == 0)
    {
      /* Same capture path as recplay, ending in an encoder instead of the
       * speaker.
       */

      recplay = true;
      cfg.ogg = true;
      cfg.seconds = AUDIO_TEST_DEF_OPUS_SECONDS;
    }
  else if (strcmp(argv[1], "diag") == 0)
    {
      diag = true;
      capture = true;
    }
  else
    {
      audio_test_usage();
      return EXIT_FAILURE;
    }

  for (i = argstart; i + 1 < argc; i += 2)
    {
      unsigned long value = strtoul(argv[i + 1], NULL, 0);

      if (strcmp(argv[i], "-t") == 0)
        {
          cfg.seconds = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-r") == 0)
        {
          cfg.samplerate = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-f") == 0)
        {
          cfg.tone_hz = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-a") == 0)
        {
          if (value > 32767)
            {
              value = 32767;
            }

          cfg.amplitude = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-g") == 0)
        {
          if (value > 32767)
            {
              value = 32767;
            }

          cfg.play_peak = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-v") == 0)
        {
          cfg.dac_volume = (int)value;
        }
      else if (strcmp(argv[i], "-b") == 0)
        {
          cfg.bitrate = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-c") == 0)
        {
          cfg.chunk_ms = (unsigned int)value;
        }
      else if (strcmp(argv[i], "-m") == 0)
        {
          cfg.mic_gain = (int)value;
        }
      else if (strcmp(argv[i], "-d") == 0)
        {
          cfg.adc_gain = (int)value;
        }
      else if (strcmp(argv[i], "-p") == 0)
        {
          cfg.hpf = (int)value;
        }
      else
        {
          audio_test_usage();
          return EXIT_FAILURE;
        }
    }

  if (cfg.seconds == 0 || cfg.samplerate == 0)
    {
      audio_test_usage();
      return EXIT_FAILURE;
    }

  if (cfg.samplerate != 8000 && cfg.samplerate != 16000 &&
      cfg.samplerate != 32000)
    {
      printf("audio_test: note: %u Hz needs the APLL path, which the chip "
             "driver rejects for now; expect CONFIGURE to fail\n",
             cfg.samplerate);
    }

  if (cfg.stream_host != NULL)
    {
      if (cfg.chunk_ms < 100)
        {
          printf("audio_test: a chunk shorter than 100 ms is mostly "
                 "request overhead\n");
          return EXIT_FAILURE;
        }

      return audio_test_stream_run(&cfg) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (recplay)
    {
      return audio_test_recplay(&cfg) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  return audio_test_run(playback, capture, diag, &cfg) < 0 ?
         EXIT_FAILURE : EXIT_SUCCESS;
}
