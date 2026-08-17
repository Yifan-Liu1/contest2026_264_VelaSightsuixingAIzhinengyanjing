/****************************************************************************
 * app/audio_test/audio_test_stream.h
 *
 * Continuous capture split into fixed-length chunks and uploaded while the
 * next chunk is still being recorded.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_AUDIO_TEST_AUDIO_TEST_STREAM_H
#define __APP_AUDIO_TEST_AUDIO_TEST_STREAM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Opaque: everything in here is shared between the capture thread and the
 * uploader, and the locking that makes that safe is this module's business
 * alone.
 */

struct audio_test_stream_ctx_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: audio_test_stream_open
 *
 * Description:
 *   Allocate the chunk ring and start the uploader thread.
 *
 *   The ring is what decouples the two halves.  Capture runs on a deadline
 *   it does not control -- the ADC delivers a buffer every few milliseconds
 *   whether or not anyone is ready for it -- while an upload takes as long
 *   as the network feels like taking.  Handing a chunk straight to a socket
 *   from the capture path would make every network stall a capture stall,
 *   and the plan is explicit that congestion must not block audio, the
 *   mailbox heartbeat or the keys.  So capture only ever copies into a free
 *   slot and signals; if the uploader has fallen behind, capture discards
 *   the oldest queued chunk and keeps recording rather than waiting for it.
 *
 *   Dropping a chunk is a deliberate choice and not a silent one: the
 *   upload protocol tolerates a gap in the sequence numbers, and a counted
 *   drop reported at the end is far better than audio that stutters
 *   whenever Wi-Fi does.
 *
 * Input Parameters:
 *   host     - IPv4 address of the receiver, in dotted-quad form
 *   port     - TCP port it is listening on
 *   rate     - sample rate of the audio that will be fed in
 *   chunk_ms - chunk length in milliseconds
 *   bitrate  - Opus target bits per second
 *
 * Returned Value:
 *   A context, or NULL on failure (a message has been printed).
 *
 ****************************************************************************/

struct audio_test_stream_ctx_s *
audio_test_stream_open(const char *host, int port, unsigned int rate,
                       unsigned int chunk_ms, unsigned int bitrate);

/****************************************************************************
 * Name: audio_test_stream_feed
 *
 * Description:
 *   Hand captured samples to the chunker.  Called from the capture thread
 *   once per buffer the driver returns.
 *
 *   Never blocks and never fails: if there is nowhere to put the samples it
 *   frees a slot by dropping the oldest chunk that has not started
 *   uploading yet.
 *
 * Input Parameters:
 *   ctx      - from audio_test_stream_open()
 *   samples  - interleaved 16-bit samples as they came from the driver
 *   nsamples - number of samples available at samples, all channels
 *   stride   - 1 for mono, 2 to take the left channel of an interleaved
 *              pair.  With the AEC echo reference compiled in the capture
 *              stream is L/R interleaved and only the left channel is the
 *              microphone, so the de-interleave happens here rather than
 *              forcing the caller to stage a copy of every buffer.
 *
 ****************************************************************************/

void audio_test_stream_feed(struct audio_test_stream_ctx_s *ctx,
                            const int16_t *samples, size_t nsamples,
                            unsigned int stride);

/****************************************************************************
 * Name: audio_test_stream_close
 *
 * Description:
 *   Flush whatever is queued, stop the uploader and release everything.
 *
 *   A partially filled chunk is sent too: the last couple of seconds of a
 *   session are usually the reason it was started.
 *
 ****************************************************************************/

void audio_test_stream_close(struct audio_test_stream_ctx_s *ctx);

/****************************************************************************
 * Name: audio_test_stream_report
 *
 * Description:
 *   Print the running counters.  Safe to call while capture is running.
 *
 ****************************************************************************/

void audio_test_stream_report(struct audio_test_stream_ctx_s *ctx);

#endif /* __APP_AUDIO_TEST_AUDIO_TEST_STREAM_H */
