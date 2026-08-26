/****************************************************************************
 * app/velasight/include/vs_audio.h
 *
 * PCM capture and playback over the NuttX audio character devices
 * (/dev/audio/pcm0c, /dev/audio/pcm0p).
 *
 * Why this exists instead of packages/ai_agent's audio_capture/audio_playback
 * ---------------------------------------------------------------------------
 * Those two files offer exactly two backends and neither one exists on this
 * board:
 *
 *   - a direct ALSA path behind CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT, which
 *     needs the Allwinner R528 rtos-hal include tree; and
 *   - media_recorder()/media_player(), which need the media framework.
 *
 * configs/ai_agent enables neither (there is no CONFIG_MEDIA* at all), so on
 * hardware audio_capture_open() fell through to media_recorder_open() and
 * failed with errno 0 and then EINVAL, which vs_voice.c reported as -EIO and
 * the display showed as "设备读写失败（-5）" on both voice pages.
 *
 * What this board does have is the NuttX audio upper half over BK7258's
 * internal ADC/DAC, registered by bk7258_audio_initialize() and already
 * proven on hardware by app/audio_test (16 kHz mono 16-bit capture,
 * rms=28.2 peak=145 underruns=0 -- see docs/8.16基础适配门禁验收记录.md).
 * The ioctl sequence below is that app's, which is also nxrecorder's:
 * RESERVE, CONFIGURE, GETBUFFERINFO, ALLOCBUFFER, mq_open, REGISTERMQ,
 * START, then prime the queue.  That last pair is deliberately the reverse of
 * audio_test's order: a buffer queued to a device that is only PREPARED can
 * never be handed back, because the audio upper half's stop path does not
 * reach the driver from that state, and freeing it anyway would leave the
 * driver's queue pointing at released memory.
 *
 * Hardware limits this API does not try to hide
 * ---------------------------------------------
 * Sample width is 16 bits only, and channel count is a property of the
 * wiring (playback mono, capture mono) rather than a request -- the driver
 * answers -EINVAL for anything else.  Only 8000/16000/32000 Hz are reachable
 * while the audio block runs from the 26 MHz crystal: 24000 is accepted by
 * aud_set_dac_samplerate() but is derived from the 48 kHz family, which needs
 * the APLL sequence that aud_clk_config() rejects with -ENOTSUP.  Asking for
 * a rate the DAC cannot really produce is therefore not a loud failure but
 * wrong-pitch audio, so callers must pass one of the three.
 *
 * Threading contract
 * ------------------
 * open/start/read/write/drain/close belong to one owning thread.  Only
 * vs_audio_capture_abort() and vs_audio_playback_stop() may be called from
 * another thread, and only while the owner still holds the handle -- the
 * owner must stop publishing the pointer before it calls close().  That is
 * exactly how vs_voice.c uses them: the voice worker owns both handles and
 * the UI thread reaches them through g_voice under its lock.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_AUDIO_H
#define __APP_VELASIGHT_INCLUDE_VS_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* For VS_PRIORITY_AUDIO: the threads created here have to sit above the UI,
 * and the priority layout is shared with the rest of the application.
 */

#include "vs_types.h"

struct vs_audio_cap_s;
struct vs_audio_pb_s;

/****************************************************************************
 * Capture
 ****************************************************************************/

/* Open and configure one capture session.  bits must be 16 and channels must
 * match the hardware (1 on this board); sample_rate must be 8000, 16000 or
 * 32000.  Returns NULL on failure.
 */

struct vs_audio_cap_s *vs_audio_capture_open(const char *dev_path,
                                             unsigned int sample_rate,
                                             unsigned int channels,
                                             unsigned int bits);

/* Start the ADC and then fill the driver's queue.  Returns 0 or a negative
 * errno.
 */

int vs_audio_capture_start(struct vs_audio_cap_s *cap);

/* What one capture session delivered, for tuning the analog gain.
 *
 * An utterance the recognizer returns no text for looks identical to a broken
 * microphone unless the level is on record, and the two have opposite fixes.
 * peak and rms are on the samples' own 0..32767 scale.
 */

struct vs_audio_level_s
{
  unsigned int peak;      /* Loudest sample delivered. */
  unsigned int rms;       /* Root mean square over every delivered sample. */
  size_t dropped;         /* Bytes lost to the staging ring overflowing. */
  size_t settled;         /* Bytes discarded as analog front-end settling. */
  uint64_t clipped;       /* Samples pinned at full scale by the analog stage. */
  uint64_t samples;       /* Samples delivered, for reading clipped as a rate. */
};

/* Read the level counters.  Safe with cap == NULL, which reports zeroes.
 *
 * peak saturates at 32767 and so cannot distinguish loud from overdriven;
 * clipped is what separates them.  A few clipped samples per thousand is
 * ordinary speech at a healthy gain, while a percent or more means
 * CONFIG_VS_AUDIO_MIC_GAIN is high enough that the distortion itself is what
 * the recognizer is failing on.
 */

void vs_audio_capture_level(struct vs_audio_cap_s *cap,
                            struct vs_audio_level_s *level);

/* Bytes staged and not yet read.  Lets a caller finishing an utterance drain
 * exactly what is buffered instead of guessing at a duration.
 */

size_t vs_audio_capture_pending(struct vs_audio_cap_s *cap);

/* Stop accepting new audio while leaving what is already staged readable.
 *
 * This is what "the user finished speaking" means: the ring stops growing, so
 * a drain loop terminates, but nothing already captured is thrown away.  Use
 * vs_audio_capture_abort() instead when the round is being abandoned.
 *
 * A short grace period survives this call, because the ADC is part-way
 * through a buffer when it is made and the driver only hands a buffer back
 * once it is full.  That last buffer is the end of the sentence the user just
 * spoke, so it is admitted rather than cut off.
 */

void vs_audio_capture_stop(struct vs_audio_cap_s *cap);

/* True while more audio may still arrive: either input has not been stopped,
 * or its grace period has not expired yet.
 *
 * A drain loop needs this to tell "everything has been read" from "the ring
 * happens to be empty at this instant".  Without it, a consumer that was
 * keeping up would see pending() == 0 immediately after stop() and finish the
 * utterance without the buffer the ADC was still filling.
 */

bool vs_audio_capture_input_pending(struct vs_audio_cap_s *cap);

/* Copy up to len bytes of signed 16-bit little-endian PCM into buf.
 *
 * Returns the byte count (always even, and possibly less than len), or:
 *   -EAGAIN     nothing staged yet
 *   -ECANCELED  vs_audio_capture_abort() was called
 *
 * Never blocks: a reader thread owns the device and this only copies out of
 * the staging ring, so however long the caller spends between reads -- a
 * blocking network write, for instance -- the driver keeps being serviced and
 * no samples are lost.
 *
 * The first buffers after the ADC is enabled are discarded rather than
 * returned: the analog front end delivers full-scale samples until it
 * settles.  Saturation is what identifies them, so only the buffers that are
 * actually unusable are dropped.
 */

int vs_audio_capture_read(struct vs_audio_cap_s *cap, void *buf, size_t len);

/* Make the current and all later reads fail with -ECANCELED.  Safe from any
 * thread and safe with cap == NULL.  Does not free the handle.
 */

void vs_audio_capture_abort(struct vs_audio_cap_s *cap);

/* Stop the ADC and release everything.  Safe with cap == NULL. */

void vs_audio_capture_close(struct vs_audio_cap_s *cap);

/****************************************************************************
 * Playback
 ****************************************************************************/

/* Open and configure one playback session and start its drain thread.
 * Same argument constraints as capture.  Returns NULL on failure.
 *
 * Writes land in a staging ring rather than going straight to the driver's
 * four small buffers, because the producer here is a streaming TTS
 * WebSocket: blocking it for the realtime length of the audio would stall
 * recv() and the server would be declared dead long before the speech ended.
 *
 * The DAC is started here, while there is still nothing to play; it stays
 * silent until the first write arrives.  See vs_audio.c for why queueing
 * audio before starting would make teardown unsafe.
 */

struct vs_audio_pb_s *vs_audio_playback_open(const char *dev_path,
                                             unsigned int sample_rate,
                                             unsigned int channels,
                                             unsigned int bits);

/* Queue len bytes of signed 16-bit little-endian PCM.
 *
 * Returns len once all of it is queued, or -ECANCELED if playback was
 * stopped.  Blocks only while the ring is full, which is backpressure rather
 * than realtime pacing.
 */

int vs_audio_playback_write(struct vs_audio_pb_s *pb, const void *buf,
                            size_t len);

/* Wait until everything queued has actually been played, so a caller can
 * treat "TTS finished" as "the user has heard it".  Returns early if
 * vs_audio_playback_stop() is called, and gives up if the driver stops making
 * progress, so it cannot hang a conversation.
 *
 * Deliberately separate from close(): the handle stays valid and registered
 * while this blocks, which is what lets the UI thread cut playback short
 * with stop() during the tail.
 */

void vs_audio_playback_drain(struct vs_audio_pb_s *pb);

/* Silence the speaker now and make later writes return -ECANCELED.  Safe
 * from any thread and safe with pb == NULL.  Does not free the handle.
 */

void vs_audio_playback_stop(struct vs_audio_pb_s *pb);

/* How many times the DAC ran out of audio while playing.
 *
 * Non-zero means the producer could not keep up and the listener heard gaps.
 * It is the measurement CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS is tuned
 * against, and the only way to tell a chopped reply from a short one after the
 * fact.  Safe with pb == NULL, which reports zero.
 */

unsigned int vs_audio_playback_underruns(struct vs_audio_pb_s *pb);

/* Join the drain thread, stop the DAC and release everything.  Whatever is
 * still queued is discarded, so call vs_audio_playback_drain() first when the
 * tail matters.  Safe with pb == NULL.
 */

void vs_audio_playback_close(struct vs_audio_pb_s *pb);

/****************************************************************************
 * Output volume
 ****************************************************************************/

/* Set and read the speaker volume, in thousandths of full scale.
 *
 * Independent of the handles above on purpose: the volume belongs to the
 * device, not to a playback session.  The driver keeps what it is given in its
 * own configuration as well as in the DAC gain register, so it survives the
 * next configure and therefore the next utterance -- which means these can be
 * called with nothing playing, and a change made between two replies still
 * applies to the second one.
 *
 * Both return 0 or a negative errno.  Read back rather than remember what was
 * written: the driver quantises to six bits of digital gain, so neighbouring
 * requests are the same setting, and 0 dB is 714 rather than 1000.
 */

int vs_audio_volume_set(const char *dev_path, unsigned int permille);

int vs_audio_volume_get(const char *dev_path, unsigned int *permille);

#endif /* __APP_VELASIGHT_INCLUDE_VS_AUDIO_H */
