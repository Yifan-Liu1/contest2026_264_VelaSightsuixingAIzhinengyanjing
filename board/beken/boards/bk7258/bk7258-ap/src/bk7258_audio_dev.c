/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_audio_dev.c
 *
 * NuttX audio lower-half for the BK7258 internal analog audio block.
 *
 * Two independent instances are created: a playback instance driving the
 * mono DAC (which reaches the speaker through the HT6872 amplifier) and,
 * when CONFIG_BK7258_AUDIO_CAPTURE is set, a capture instance reading the
 * analog MIC ADC.
 *
 * Data movement is interrupt-driven FIFO copy.  The chip's DMA engine is
 * not used yet: bk7258_dma.c currently exposes a single-instance
 * memory-to-memory channel (bk7258_dma_configure/start/stop with no
 * channel argument), so a peripheral-destination circular channel has to
 * be added there before playback can be handed to DMA.  At 16 kHz mono
 * the FIFO path is affordable; it will not be at 48 kHz stereo.
 *
 * Buffer completion is reported from a high-priority work queue rather
 * than directly from the ISR, because the upper half's dequeue path ends
 * in a message-queue send.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/audio/audio.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/queue.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include "bk7258_aud.h"
#include "bk7258_audio_bringup.h"

#ifdef CONFIG_BK7258_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_BK7258_AUDIO_SAMPLERATE
#  define CONFIG_BK7258_AUDIO_SAMPLERATE 16000
#endif

#ifndef CONFIG_BK7258_AUDIO_PA_ON_DELAY_MS
#  define CONFIG_BK7258_AUDIO_PA_ON_DELAY_MS 20
#endif

#ifndef CONFIG_BK7258_AUDIO_PA_OFF_DELAY_MS
#  define CONFIG_BK7258_AUDIO_PA_OFF_DELAY_MS 10
#endif

/* Number of samples copied per ISR visit before giving the FIFO status
 * another look.  Bounds the time spent in interrupt context.
 */

#define BK7258_AUDIO_ISR_BUDGET   64

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_audio_s
{
  /* This field must appear first so the structure can be cast to
   * struct audio_lowerhalf_s.
   */

  struct audio_lowerhalf_s dev;

  bool playback;                     /* true: DAC, false: ADC          */
  bool configured;
  bool running;
  bool paused;

  struct bk7258_aud_config cfg;

  /* Buffers handed down by the upper half but not yet fully consumed or
   * filled, and buffers finished by the ISR and awaiting the completion
   * callback.
   */

  struct dq_queue_s pendq;
  struct dq_queue_s doneq;

  struct work_s work;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_audio_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                                FAR struct audio_caps_s *caps);
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                  FAR void *session,
                                  FAR const struct audio_caps_s *caps);
#else
static int bk7258_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                  FAR const struct audio_caps_s *caps);
#endif
static int bk7258_audio_shutdown(FAR struct audio_lowerhalf_s *dev);
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_start(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session);
#else
static int bk7258_audio_start(FAR struct audio_lowerhalf_s *dev);
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_stop(FAR struct audio_lowerhalf_s *dev,
                             FAR void *session);
#else
static int bk7258_audio_stop(FAR struct audio_lowerhalf_s *dev);
#endif
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_pause(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session);
static int bk7258_audio_resume(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session);
#else
static int bk7258_audio_pause(FAR struct audio_lowerhalf_s *dev);
static int bk7258_audio_resume(FAR struct audio_lowerhalf_s *dev);
#endif
#endif
static int bk7258_audio_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                      FAR struct ap_buffer_s *apb);
static int bk7258_audio_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                     FAR struct ap_buffer_s *apb);
static int bk7258_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                              unsigned long arg);
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_reserve(FAR struct audio_lowerhalf_s *dev,
                                FAR void **session);
static int bk7258_audio_release(FAR struct audio_lowerhalf_s *dev,
                                FAR void *session);
#else
static int bk7258_audio_reserve(FAR struct audio_lowerhalf_s *dev);
static int bk7258_audio_release(FAR struct audio_lowerhalf_s *dev);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct audio_ops_s g_bk7258_audio_ops =
{
  bk7258_audio_getcaps,       /* getcaps        */
  bk7258_audio_configure,     /* configure      */
  bk7258_audio_shutdown,      /* shutdown       */
  bk7258_audio_start,         /* start          */
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  bk7258_audio_stop,          /* stop           */
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
  bk7258_audio_pause,         /* pause          */
  bk7258_audio_resume,        /* resume         */
#endif
  NULL,                       /* allocbuffer    */
  NULL,                       /* freebuffer     */
  bk7258_audio_enqueuebuffer, /* enqueue_buffer */
  bk7258_audio_cancelbuffer,  /* cancel_buffer  */
  bk7258_audio_ioctl,         /* ioctl          */
  NULL,                       /* read           */
  NULL,                       /* write          */
  bk7258_audio_reserve,       /* reserve        */
  bk7258_audio_release        /* release        */
};

/* The chip layer supports one DAC and one ADC, so a static instance per
 * direction is sufficient and avoids an allocation on the bring-up path.
 */

static struct bk7258_audio_s g_bk7258_playback;
#ifdef CONFIG_BK7258_AUDIO_CAPTURE
static struct bk7258_audio_s g_bk7258_capture;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_audio_complete_work
 *
 * Description:
 *   Work-queue half of buffer completion.  Drains doneq and reports each
 *   buffer to the upper half.
 *
 ****************************************************************************/

static void bk7258_audio_complete_work(FAR void *arg)
{
  FAR struct bk7258_audio_s *priv = arg;
  FAR struct ap_buffer_s *apb;
  irqstate_t flags;

  for (; ; )
    {
      flags = enter_critical_section();
      apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->doneq);
      leave_critical_section(flags);

      if (apb == NULL)
        {
          break;
        }

#ifdef CONFIG_AUDIO_MULTI_SESSION
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK,
                      NULL);
#else
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
#endif
    }
}

/****************************************************************************
 * Name: bk7258_audio_finish
 *
 * Description:
 *   Move a finished buffer to doneq and schedule the completion work.
 *   Called from interrupt context.
 *
 ****************************************************************************/

static void bk7258_audio_finish(FAR struct bk7258_audio_s *priv,
                                FAR struct ap_buffer_s *apb)
{
  dq_addlast(&apb->dq_entry, &priv->doneq);

  if (work_available(&priv->work))
    {
      work_queue(HPWORK, &priv->work, bk7258_audio_complete_work, priv, 0);
    }
}

/****************************************************************************
 * Name: bk7258_audio_tx_callback
 *
 * Description:
 *   DAC FIFO service, invoked from the AUD ISR when the left FIFO has
 *   drained below its read threshold.
 *
 ****************************************************************************/

static void bk7258_audio_tx_callback(void *arg)
{
  FAR struct bk7258_audio_s *priv = arg;
  FAR struct ap_buffer_s *apb;
  unsigned int budget = BK7258_AUDIO_ISR_BUDGET;

  while (budget > 0)
    {
      apb = (FAR struct ap_buffer_s *)dq_peek(&priv->pendq);
      if (apb == NULL)
        {
          /* Nothing left to play.  Mask the interrupt so the ISR does not
           * spin on a permanently-empty FIFO; enqueuebuffer() re-arms it.
           */

          bk7258_aud_tx_int_enable(false);
          return;
        }

      if (apb->curbyte + sizeof(int16_t) <= apb->nbytes)
        {
          FAR const int16_t *samples =
            (FAR const int16_t *)(apb->samp + apb->curbyte);
          unsigned int avail = (apb->nbytes - apb->curbyte) /
                               sizeof(int16_t);
          unsigned int written;

          if (avail > budget)
            {
              avail = budget;
            }

          written = bk7258_aud_dac_write(samples, avail);
          apb->curbyte += written * sizeof(int16_t);
          budget -= written;

          if (written < avail)
            {
              /* FIFO is full: come back on the next interrupt. */

              return;
            }
        }
      else
        {
          /* Buffer exhausted (this also covers a zero-length buffer, which
           * the upper half uses to mark end of stream).
           */

          (void)dq_remfirst(&priv->pendq);
          bk7258_audio_finish(priv, apb);
        }
    }
}

/****************************************************************************
 * Name: bk7258_audio_rx_callback
 *
 * Description:
 *   ADC FIFO service, invoked from the AUD ISR when the FIFO has risen
 *   above its write threshold.
 *
 ****************************************************************************/

#ifdef CONFIG_BK7258_AUDIO_CAPTURE
static void bk7258_audio_rx_callback(void *arg)
{
  FAR struct bk7258_audio_s *priv = arg;
  FAR struct ap_buffer_s *apb;
  unsigned int budget = BK7258_AUDIO_ISR_BUDGET;

  while (budget > 0)
    {
      apb = (FAR struct ap_buffer_s *)dq_peek(&priv->pendq);
      if (apb == NULL)
        {
          /* No buffer to fill.  Drop the interrupt rather than spinning;
           * the samples still in the FIFO are lost, which is the correct
           * behaviour for a capture overrun.
           */

          bk7258_aud_rx_int_enable(false);
          return;
        }

      if (apb->nbytes + sizeof(int16_t) <= apb->nmaxbytes)
        {
          FAR int16_t *samples = (FAR int16_t *)(apb->samp + apb->nbytes);
          unsigned int room = (apb->nmaxbytes - apb->nbytes) /
                              sizeof(int16_t);
          unsigned int read;

          if (room > budget)
            {
              room = budget;
            }

          read = bk7258_aud_adc_read(samples, room);
          apb->nbytes += read * sizeof(int16_t);
          budget -= read;

          if (read < room)
            {
              /* FIFO drained: wait for the next interrupt. */

              return;
            }
        }
      else
        {
          (void)dq_remfirst(&priv->pendq);
          bk7258_audio_finish(priv, apb);
        }
    }
}
#endif

/****************************************************************************
 * Name: bk7258_audio_flush
 *
 * Description:
 *   Return every queued buffer to the upper half.  Used by shutdown and
 *   stop so no buffer is stranded in the lower half.
 *
 ****************************************************************************/

static void bk7258_audio_flush(FAR struct bk7258_audio_s *priv)
{
  FAR struct ap_buffer_s *apb;
  irqstate_t flags;

  for (; ; )
    {
      flags = enter_critical_section();
      apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->pendq);
      leave_critical_section(flags);

      if (apb == NULL)
        {
          break;
        }

#ifdef CONFIG_AUDIO_MULTI_SESSION
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK,
                      NULL);
#else
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
#endif
    }

  bk7258_audio_complete_work(priv);
}

/****************************************************************************
 * Name: bk7258_audio_poll_adc
 *
 * Description:
 *   Diagnostic helper: drain the ADC FIFO by polling for roughly 200 ms,
 *   deliberately ignoring the interrupt path.  Returns the sample count and
 *   accumulates the sum of squares so the caller can report a mean square.
 *
 ****************************************************************************/

static unsigned int bk7258_audio_poll_adc(FAR uint64_t *sumsq)
{
  int16_t samples[32];
  unsigned int total = 0;
  unsigned int i;

  for (i = 0; i < 200; i++)
    {
      unsigned int got = bk7258_aud_adc_read(samples, 32);
      unsigned int j;

      for (j = 0; j < got; j++)
        {
          *sumsq += (uint64_t)((int32_t)samples[j] * (int32_t)samples[j]);
        }

      total += got;
      up_udelay(1000);
    }

  return total;
}

/****************************************************************************
 * Name: bk7258_audio_getcaps
 ****************************************************************************/

static int bk7258_audio_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                                FAR struct audio_caps_s *caps)
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

  (void)type;

  DEBUGASSERT(caps != NULL &&
              caps->ac_len >= sizeof(struct audio_caps_s));

  caps->ac_format.hw  = 0;
  caps->ac_controls.w = 0;

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_QUERY:

        /* Playback is mono: the chip has no right DAC and the board has a
         * single speaker.  Capture reports two channels when the MIC2 echo
         * reference is enabled, because frames really are L/R interleaved
         * (L = voice, R = reference) even though only one of the two is a
         * microphone.
         */

        caps->ac_channels = priv->playback ? 1 :
                            (priv->cfg.capture_reference ? 2 : 1);

        switch (caps->ac_subtype)
          {
            case AUDIO_TYPE_QUERY:
              caps->ac_controls.b[0] = priv->playback ? AUDIO_TYPE_OUTPUT :
                                                        AUDIO_TYPE_INPUT;
              caps->ac_format.hw = 1 << (AUDIO_FMT_PCM - 1);
              break;

            default:
              caps->ac_controls.b[0] = AUDIO_SUBFMT_END;
              break;
          }
        break;

      case AUDIO_TYPE_OUTPUT:
      case AUDIO_TYPE_INPUT:
        caps->ac_channels = priv->playback ? 1 :
                            (priv->cfg.capture_reference ? 2 : 1);

        switch (caps->ac_subtype)
          {
            case AUDIO_TYPE_QUERY:

              /* Only the rates reachable from the 26 MHz crystal are
               * advertised.  The 44.1k/48k families additionally need the
               * APLL calibration sequence, which aud_clk_config() in
               * bk7258_aud.c deliberately rejects with -ENOTSUP until it
               * has been transcribed from the vendor tree.
               */

              caps->ac_controls.hw[0] = AUDIO_SAMP_RATE_8K |
                                        AUDIO_SAMP_RATE_16K |
                                        AUDIO_SAMP_RATE_32K;
              caps->ac_controls.b[2] = 16;   /* Bits per sample */
              break;

            default:
              break;
          }
        break;

      default:
        caps->ac_subtype = 0;
        caps->ac_channels = 0;
        break;
    }

  return caps->ac_len;
}

/****************************************************************************
 * Name: bk7258_audio_configure
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                  FAR void *session,
                                  FAR const struct audio_caps_s *caps)
#else
static int bk7258_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                  FAR const struct audio_caps_s *caps)
#endif
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;
  uint32_t samplerate;
  int ret;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  DEBUGASSERT(caps != NULL);

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_OUTPUT:
      case AUDIO_TYPE_INPUT:
        if ((caps->ac_type == AUDIO_TYPE_OUTPUT) != priv->playback)
          {
            return -EINVAL;
          }

        /* Sample width.  The FIFO ports are 16 bits per sample and the
         * copy loops below step by sizeof(int16_t), so anything else would
         * be silently misinterpreted rather than merely sounding wrong.
         */

        if (caps->ac_controls.b[2] != 0 && caps->ac_controls.b[2] != 16)
          {
            auderr("unsupported sample width %u\n",
                   caps->ac_controls.b[2]);
            return -EINVAL;
          }

        /* Channel count.  Playback is mono because the chip has no right
         * DAC.  Capture is two channels when the MIC2 echo reference is
         * enabled and one otherwise; the count is a property of the
         * hardware wiring, not something the caller can choose.
         */

        if (caps->ac_channels != 0)
          {
            uint8_t expected = priv->playback ? 1 :
                               (priv->cfg.capture_reference ? 2 : 1);

            if (caps->ac_channels != expected)
              {
                auderr("unsupported channel count %u, expected %u\n",
                       caps->ac_channels, expected);
                return -EINVAL;
              }
          }

        /* nxplayer splits the sample rate across hw[0] and b[3] (see
         * nxplayer.c:1935-1936), so both halves have to be recombined.
         */

        samplerate = caps->ac_controls.hw[0] |
                     ((uint32_t)caps->ac_controls.b[3] << 16);
        if (samplerate != 0)
          {
            priv->cfg.samplerate = samplerate;
          }

        if (priv->playback)
          {
            ret = bk7258_aud_dac_setup(&priv->cfg);
          }
        else
          {
            ret = bk7258_aud_adc_setup(&priv->cfg);
          }

        if (ret < 0)
          {
            auderr("%s setup failed: %d\n",
                   priv->playback ? "DAC" : "ADC", ret);
            return ret;
          }

        priv->configured = true;
        break;

      case AUDIO_TYPE_FEATURE:

        /* Volume maps onto the DAC's digital gain.  The upper half passes
         * 0..1000; scale that onto 0..0x3f, whose 0 dB point is 0x2d.
         */

#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
        if (caps->ac_format.hw == AUDIO_FU_VOLUME && priv->playback)
          {
            uint16_t volume = caps->ac_controls.hw[0];
            uint8_t gain;

            if (volume > 1000)
              {
                volume = 1000;
              }

            gain = (uint8_t)((volume * BK7258_AUD_DAC_DIG_GAIN_MAX) / 1000);
            return bk7258_aud_dac_set_dig_gain(gain);
          }

#endif
        return -ENOTTY;

      default:
        return -ENOTTY;
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_shutdown
 ****************************************************************************/

static int bk7258_audio_shutdown(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

  if (priv->playback)
    {
      /* Amplifier first, DAC second: see bk7258_audio_stop(). */

      bk7258_aud_dac_mute(true);
      bk7258_audio_pa_enable(false);
      bk7258_aud_dac_shutdown();
    }
  else
    {
      bk7258_aud_adc_shutdown();
    }

  bk7258_audio_flush(priv);

  priv->configured = false;
  priv->running = false;
  priv->paused = false;

  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_start
 *
 * Description:
 *   Playback start-up order follows the vendor's onboard_speaker_stream
 *   implementation (aud_dac start, pa_on_delay, amplifier on, un-mute):
 *   the amplifier stays muted while the DAC's analog output settles, so
 *   the speaker does not pop.  R71's 10K pull-down keeps CTRL low until
 *   bk7258_audio_pa_enable(true) drives it, which means the hardware is
 *   already silent before software gets here.
 *
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_start(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session)
#else
static int bk7258_audio_start(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  if (!priv->configured)
    {
      return -EPERM;
    }

  if (priv->running)
    {
      return OK;
    }

  if (priv->playback)
    {
      bk7258_aud_set_tx_callback(bk7258_audio_tx_callback, priv);
      bk7258_aud_dac_start();

      nxsig_usleep(CONFIG_BK7258_AUDIO_PA_ON_DELAY_MS * 1000);

      bk7258_audio_pa_enable(true);
      bk7258_aud_dac_mute(false);

      bk7258_aud_tx_int_enable(true);
    }
#ifdef CONFIG_BK7258_AUDIO_CAPTURE
  else
    {
      bk7258_aud_set_rx_callback(bk7258_audio_rx_callback, priv);
      bk7258_aud_adc_start();
      bk7258_aud_rx_int_enable(true);
    }
#endif

  priv->running = true;
  priv->paused = false;

  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_stop
 *
 * Description:
 *   Teardown order is the inverse and is deliberately mute-first.  Note
 *   the vendor's own header and code disagree about where pa_off_delay
 *   sits: onboard_speaker_stream.h:51 describes it as the delay *after*
 *   turning the amplifier off, while onboard_speaker_stream.c:341-345
 *   sleeps *before* pulling the GPIO low.  The code order is the one that
 *   actually lets the analog output settle while still muted, so that is
 *   what is implemented here.
 *
 ****************************************************************************/

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_stop(FAR struct audio_lowerhalf_s *dev,
                             FAR void *session)
#else
static int bk7258_audio_stop(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  if (priv->playback)
    {
      bk7258_aud_tx_int_enable(false);

      bk7258_aud_dac_mute(true);
      nxsig_usleep(CONFIG_BK7258_AUDIO_PA_OFF_DELAY_MS * 1000);
      bk7258_audio_pa_enable(false);

      bk7258_aud_dac_stop();
    }
  else
    {
      bk7258_aud_rx_int_enable(false);
      bk7258_aud_adc_stop();
    }

  if (priv->playback)
    {
      bk7258_aud_set_tx_callback(NULL, NULL);
    }
  else
    {
      bk7258_aud_set_rx_callback(NULL, NULL);
    }

  priv->running = false;
  priv->paused = false;

  bk7258_audio_flush(priv);

  return OK;
}
#endif

/****************************************************************************
 * Name: bk7258_audio_pause / bk7258_audio_resume
 *
 * Description:
 *   Pause only masks the FIFO interrupt and mutes; the DAC stays enabled
 *   and the amplifier stays on so resume is click-free.
 *
 ****************************************************************************/

#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_pause(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session)
#else
static int bk7258_audio_pause(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  if (!priv->running || priv->paused)
    {
      return OK;
    }

  if (priv->playback)
    {
      bk7258_aud_tx_int_enable(false);
      bk7258_aud_dac_mute(true);
    }
  else
    {
      bk7258_aud_rx_int_enable(false);
    }

  priv->paused = true;
  return OK;
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_resume(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session)
#else
static int bk7258_audio_resume(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  if (!priv->running || !priv->paused)
    {
      return OK;
    }

  if (priv->playback)
    {
      bk7258_aud_dac_mute(false);
      bk7258_aud_tx_int_enable(true);
    }
  else
    {
      bk7258_aud_rx_int_enable(true);
    }

  priv->paused = false;
  return OK;
}
#endif

/****************************************************************************
 * Name: bk7258_audio_enqueuebuffer
 ****************************************************************************/

static int bk7258_audio_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                      FAR struct ap_buffer_s *apb)
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;
  irqstate_t flags;

  DEBUGASSERT(apb != NULL);

  if (priv->playback)
    {
      apb->curbyte = 0;
    }
  else
    {
      apb->nbytes = 0;
    }

  flags = enter_critical_section();
  dq_addlast(&apb->dq_entry, &priv->pendq);
  leave_critical_section(flags);

  /* The ISR masks its own interrupt when it runs out of buffers, so
   * re-arm here rather than relying on it staying enabled.
   */

  if (priv->running && !priv->paused)
    {
      if (priv->playback)
        {
          bk7258_aud_tx_int_enable(true);
        }
      else
        {
          bk7258_aud_rx_int_enable(true);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_cancelbuffer
 ****************************************************************************/

static int bk7258_audio_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                     FAR struct ap_buffer_s *apb)
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;
  irqstate_t flags;

  flags = enter_critical_section();
  dq_rem(&apb->dq_entry, &priv->pendq);
  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_ioctl
 ****************************************************************************/

static int bk7258_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                              unsigned long arg)
{
  FAR struct bk7258_audio_s *priv = (FAR struct bk7258_audio_s *)dev;

  (void)priv;

  switch (cmd)
    {
      /* This must be answered unconditionally, and NOT hidden behind
       * CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS.
       *
       * The upper half latches its buffer quota from this call:
       * audio.c:1267-1279 forwards GETBUFFERINFO to the lower half and only
       * assigns upper->nbuffers when the lower half returns >= 0.  If the
       * lower half answers -ENOTTY, upper->nbuffers stays 0, and then
       * audio_allocbuffer() (audio.c:786-789) short-circuits with
       * "return 0" on the very first AUDIOIOC_ALLOCBUFFER because
       * periods >= nbuffers already holds.  The application then cannot
       * obtain a single buffer, which is what "ALLOCBUFFER 0 failed: 0"
       * looked like on hardware.
       */

      case AUDIOIOC_GETBUFFERINFO:
        {
          FAR struct ap_buffer_info_s *bufinfo =
            (FAR struct ap_buffer_info_s *)arg;

          if (bufinfo == NULL)
            {
              return -EINVAL;
            }

          bufinfo->buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
          bufinfo->nbuffers    = CONFIG_AUDIO_NUM_BUFFERS;
        }
        break;

      /* Gain staging for the capture path.  Takes effect at the next
       * AUDIOIOC_CONFIGURE, which is where adc_setup() runs; applying it to a
       * stream already running would change the level mid-recording and make
       * the measurement it exists for meaningless.
       */

      case BK7258_AUDIOIOC_SET_CAPGAIN:
        {
          FAR const struct bk7258_audio_capgain_s *g =
            (FAR const struct bk7258_audio_capgain_s *)arg;

          if (g == NULL)
            {
              return -EINVAL;
            }

          if (priv->playback)
            {
              return -ENOTTY;
            }

          if (g->mic_gain > BK7258_AUD_MIC_GAIN_MAX ||
              g->adc_gain > BK7258_AUD_ADC_GAIN_MAX)
            {
              return -EINVAL;
            }

          priv->cfg.mic_gain = g->mic_gain;
          priv->cfg.adc_gain = g->adc_gain;
          priv->cfg.adc_hpf  = g->hpf;

          audinfo("capture gain: mic=%u adc=%u hpf=%d\n",
                  g->mic_gain, g->adc_gain, g->hpf);
        }
        break;

      /* Diagnostic: dump registers, then read the FIFO by polling so the
       * interrupt path is taken out of the picture entirely.
       */

      case BK7258_AUDIOIOC_DIAG:
        {
          unsigned int total = 0;
          uint64_t sumsq = 0;
          unsigned int i;

          syslog(LOG_INFO, "diag: %s, configured=%d running=%d "
                 "paused=%d rate=%" PRIu32 " ref=%d\n",
                 priv->playback ? "playback" : "capture",
                 priv->configured, priv->running, priv->paused,
                 priv->cfg.samplerate, priv->cfg.capture_reference);

          bk7258_aud_dump();

          if (!priv->playback)
            {
              bool forced;

              forced = bk7258_aud_force_module_clock();
              syslog(LOG_INFO, "diag: aud_cken was %s\n",
                     forced ? "CLEAR -> now set by AP" : "already set");

              /* Which MIC front end actually reaches the ADC?
               *
               * The ADC has been measured to deliver a single channel, so
               * the right half of every FIFO word is always zero and MIC2
               * never shows up in a normal capture.  Looking at MIC2 on
               * its own is the only way to tell whether the board's
               * post-amplifier echo tap (HT6872 OUT+/OUT- -> R84/R83 ->
               * C61/C60 -> MIC2P/MIC2N) carries signal at all, which is a
               * board-level question rather than a driver one.
               */

              bk7258_aud_select_mic(true, false);
              up_udelay(5000);
              sumsq = 0;
              total = bk7258_audio_poll_adc(&sumsq);
              syslog(LOG_INFO,
                     "diag: MIC1 only -> %u samples, meansq=%llu\n", total,
                     total ? (unsigned long long)(sumsq / total) : 0ull);

              bk7258_aud_select_mic(false, true);
              up_udelay(5000);
              sumsq = 0;
              total = bk7258_audio_poll_adc(&sumsq);
              syslog(LOG_INFO,
                     "diag: MIC2 only -> %u samples, meansq=%llu\n", total,
                     total ? (unsigned long long)(sumsq / total) : 0ull);

              /* Leave MIC1 selected again. */

              bk7258_aud_select_mic(true, priv->cfg.capture_reference);

              /* AUD_CLK_CONTROL sweep.
               *
               * Only bit0 (ADC_SOFT_RESET) and bit1 (ADC_CLK_GATE) exist,
               * so all four combinations can simply be tried in order,
               * polling after each.  This settles by experiment what the
               * vendor sources describe inconsistently: whether bit0 is a
               * pulse that must be cleared (this driver's current
               * behaviour) or a release that must stay asserted (what
               * bk_aud_driver_init() actually leaves behind).
               */

              for (i = 0; i < 4; i++)
                {
                  bk7258_aud_set_clk_control(i);
                  up_udelay(2000);

                  sumsq = 0;
                  total = bk7258_audio_poll_adc(&sumsq);

                  syslog(LOG_INFO,
                         "diag: clkctl=0x%x (rst=%d gate=%d) -> %u samples,"
                         " meansq=%llu fifosta=0x%08" PRIx32 "\n",
                         i, i & 1, (i >> 1) & 1, total,
                         total ? (unsigned long long)(sumsq / total) : 0ull,
                         bk7258_aud_fifo_status());
                }
            }

          syslog(LOG_INFO, "diag: fifo status after poll=0x%08" PRIx32 "\n",
                 bk7258_aud_fifo_status());
        }
        break;

      default:
        (void)arg;
        return -ENOTTY;
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_reserve / bk7258_audio_release
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int bk7258_audio_reserve(FAR struct audio_lowerhalf_s *dev,
                                FAR void **session)
{
  (void)dev;

  if (session != NULL)
    {
      *session = NULL;
    }

  return OK;
}

static int bk7258_audio_release(FAR struct audio_lowerhalf_s *dev,
                                FAR void *session)
{
  (void)dev;
  (void)session;
  return OK;
}
#else
static int bk7258_audio_reserve(FAR struct audio_lowerhalf_s *dev)
{
  (void)dev;
  return OK;
}

static int bk7258_audio_release(FAR struct audio_lowerhalf_s *dev)
{
  (void)dev;
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_audio_dev_initialize
 *
 * Description:
 *   Create one audio lower-half instance.  playback selects the DAC path;
 *   otherwise the ADC path is used.
 *
 ****************************************************************************/

FAR struct audio_lowerhalf_s *bk7258_audio_dev_initialize(bool playback)
{
  FAR struct bk7258_audio_s *priv;

  if (playback)
    {
      priv = &g_bk7258_playback;
    }
  else
    {
#ifdef CONFIG_BK7258_AUDIO_CAPTURE
      priv = &g_bk7258_capture;
#else
      return NULL;
#endif
    }

  memset(priv, 0, sizeof(*priv));

  priv->dev.ops = &g_bk7258_audio_ops;
  priv->playback = playback;

  dq_init(&priv->pendq);
  dq_init(&priv->doneq);

  priv->cfg.samplerate   = CONFIG_BK7258_AUDIO_SAMPLERATE;
  priv->cfg.clksrc       = BK7258_AUD_CLK_XTAL;
  priv->cfg.dac_dig_gain = BK7258_AUD_DAC_DIG_GAIN_0DB;
  priv->cfg.dac_ana_gain = BK7258_AUD_DAC_ANA_GAIN_DEF;
  priv->cfg.adc_gain     = BK7258_AUD_ADC_GAIN_0DB;
  priv->cfg.mic_gain     = BK7258_AUD_MIC_GAIN_DEF;

  /* Bypassed, matching the vendor and everything measured so far.  The
   * high-pass stages are almost certainly the better setting for voice, but
   * flipping the default before the difference has been measured would mean
   * every later comparison starts from an unverified baseline.
   */

  priv->cfg.adc_hpf      = false;

#if defined(CONFIG_BK7258_AUDIO_CAPTURE) && \
    defined(CONFIG_BK7258_AUDIO_AEC_REFERENCE)
  priv->cfg.capture_reference = !playback;
#else
  priv->cfg.capture_reference = false;
#endif

  return &priv->dev;
}

#endif /* CONFIG_BK7258_AUDIO */
