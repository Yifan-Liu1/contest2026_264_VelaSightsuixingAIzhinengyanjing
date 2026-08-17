/****************************************************************************
 * app/audio_test/audio_test_ogg.h
 *
 * Encode captured PCM as Ogg Opus and print it as base64.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_AUDIO_TEST_AUDIO_TEST_OGG_H
#define __APP_AUDIO_TEST_AUDIO_TEST_OGG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: audio_test_ogg_encoder_create
 *
 * Description:
 *   Allocate an Opus encoder and the page scratch that goes with it.
 *
 *   Continuous capture encodes a chunk every couple of seconds, and the
 *   encoder state is 38 KiB plus a 62 KiB packet scratch: allocating and
 *   freeing both per chunk would churn the heaps for the life of a session
 *   and put a large allocation on the critical path between one chunk being
 *   ready and the next one arriving.  Creating the encoder once and resetting
 *   it per chunk costs one call instead.
 *
 * Input Parameters:
 *   rate         - sample rate; must be one Opus accepts natively
 *   bitrate      - target bits per second
 *   prefer_psram - put the 38 KiB encoder state in PSRAM even though SRAM
 *                  would be faster.
 *
 *                  Set by callers that need the SRAM for something SRAM is
 *                  the only option for.  The continuous path needs a 32 KiB
 *                  thread stack, and pthread stacks can only come from the
 *                  SRAM heap; with the encoder state taking SRAM first,
 *                  pthread_create() failed with ENOMEM.  Leaving the choice
 *                  to whichever allocation ran first made that a race, so it
 *                  is a parameter instead.
 *
 * Returned Value:
 *   An opaque handle, or NULL if there was no memory for it.
 *
 ****************************************************************************/

void *audio_test_ogg_encoder_create(unsigned int rate, unsigned int bitrate,
                                    bool prefer_psram);

/****************************************************************************
 * Name: audio_test_ogg_encoder_destroy
 ****************************************************************************/

void audio_test_ogg_encoder_destroy(void *handle);

/****************************************************************************
 * Name: audio_test_ogg_encode
 *
 * Description:
 *   Encode PCM into a complete, self-contained Ogg Opus stream in the
 *   caller's buffer.
 *
 *   The encoder state is reset first, and the identification and comment
 *   headers are emitted again for every call, so each result decodes on its
 *   own with no reference to the ones before it.  That is what makes a chunk
 *   individually retryable: the upload plan allows a sequence number to be
 *   missing and allows one retry per chunk, neither of which is possible if
 *   chunk N only decodes after chunk N-1 has arrived.
 *
 * Input Parameters:
 *   handle   - from audio_test_ogg_encoder_create()
 *   pcm      - mono 16-bit samples
 *   nsamples - number of samples in pcm
 *   serial   - Ogg stream serial; must differ between concurrent streams,
 *              so the caller passes something derived from the sequence
 *   out      - destination buffer
 *   outcap   - bytes available at out
 *   outlen   - receives the encoded length
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

int audio_test_ogg_encode(void *handle, const int16_t *pcm, size_t nsamples,
                          uint32_t serial, uint8_t *out, size_t outcap,
                          size_t *outlen);

/****************************************************************************
 * Name: audio_test_ogg_opus_dump
 *
 * Description:
 *   Encode mono 16-bit PCM as Ogg Opus and print the file as base64.
 *
 *   The board has no writable filesystem and the AP console is a mailbox
 *   command path rather than a byte stream, so base64 over the console is
 *   the only way a file leaves this board.  A length and a CRC32 are printed
 *   with it so the receiving side can tell a truncated capture from a
 *   complete one instead of discovering it while decoding.
 *
 * Input Parameters:
 *   pcm      - mono 16-bit samples
 *   nsamples - number of samples in pcm
 *   rate     - sample rate; must be one Opus accepts natively
 *   bitrate  - target bits per second for the encoder
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

int audio_test_ogg_opus_dump(const int16_t *pcm, size_t nsamples,
                             unsigned int rate, unsigned int bitrate);

/****************************************************************************
 * Name: audio_test_ogg_redump
 *
 * Description:
 *   Print the file the last encode produced, again.  The console drops
 *   output under load, so a retry has to be possible without asking someone
 *   to speak into the microphone a second time.
 *
 * Returned Value:
 *   Zero on success, -ENODATA if nothing has been encoded yet.
 *
 ****************************************************************************/

int audio_test_ogg_redump(void);

/****************************************************************************
 * Name: audio_test_ogg_send
 *
 * Description:
 *   Send the file the last encode produced over TCP.
 *
 *   The console dump exists because there was no network; now that there is
 *   one, a socket carries the same bytes in a fraction of the time, without
 *   base64's third of overhead and without anyone having to capture a
 *   console while it scrolls.  The console path stays as the fallback for
 *   when the board has no address.
 *
 * Input Parameters:
 *   host - IPv4 address of the listener, in dotted-quad form
 *   port - TCP port it is listening on
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure, -ENODATA if nothing has
 *   been encoded yet.
 *
 ****************************************************************************/

int audio_test_ogg_send(const char *host, int port);

/****************************************************************************
 * Name: audio_test_send_raw
 *
 * Description:
 *   Send a buffer over TCP unchanged.
 *
 *   Used for raw PCM: a lossy encoder is built to discard noise-like content,
 *   so a noise measurement taken after one describes the encoder as much as
 *   the microphone.  Raw PCM is also the format the upload protocol
 *   specifies, so this is the shape the real path will take.
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

int audio_test_send_raw(const char *host, int port, const void *data,
                        size_t len);

#endif /* __APP_AUDIO_TEST_AUDIO_TEST_OGG_H */
