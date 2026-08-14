/****************************************************************************
 * apps/camera_preview/preview_jpeg.h
 *
 * Encode a frame that is already being previewed, using the board's hardware
 * JPEG encoder through its V4L2 M2M node (/dev/video1).
 *
 * Why this belongs to the preview application rather than to a tool of its
 * own: /dev/video0 has one owner.  While a preview is streaming, nothing else
 * can open the camera, so the process that holds the camera is the only one
 * that can produce the JPEG a cloud request needs.  `jpeg_test cam` does the
 * same encode but opens the camera itself, which is fine for a codec test and
 * useless while a preview is running.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_CAMERA_PREVIEW_PREVIEW_JPEG_H
#define __APPS_CAMERA_PREVIEW_PREVIEW_JPEG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct preview_jpeg_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Open the encoder for w x h frames in the camera's own byte order (VYUY).
 *
 * Returns NULL when /dev/video1 is absent, which is the normal case for a
 * configuration built without CONFIG_BK7258_JPEG_ENC; the caller should treat
 * that as "no JPEG available" rather than as a failure of the preview.
 */

struct preview_jpeg_s *preview_jpeg_open(int w, int h, int quality);

/* Encode one frame.
 *
 * frame/len describe the captured buffer, which is *not* handed to the
 * encoder directly: the driver only accepts input inside the pools it
 * allocated itself (bk7258_jpeg_addr_ok), so the frame is copied into the
 * codec's own output buffer first.  The copy cost is reported separately from
 * the encode cost by preview_jpeg_last_ms() precisely because it is the
 * expensive half.
 *
 * On success *out points into the codec's capture buffer (valid until the
 * next call) and *outlen is the JPEG length.
 */

int preview_jpeg_encode(struct preview_jpeg_s *ctx,
                        const uint8_t *frame, size_t len,
                        const uint8_t **out, size_t *outlen);

/* Milliseconds spent in the last encode, split into the copy and the codec. */

void preview_jpeg_last_ms(struct preview_jpeg_s *ctx,
                          uint32_t *copy_ms, uint32_t *codec_ms);

void preview_jpeg_close(struct preview_jpeg_s *ctx);

#endif /* __APPS_CAMERA_PREVIEW_PREVIEW_JPEG_H */
