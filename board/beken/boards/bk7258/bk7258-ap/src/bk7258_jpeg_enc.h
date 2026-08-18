/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-ap/src/bk7258_jpeg_enc.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_JPEG_ENC_H
#define __VENDOR_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_JPEG_ENC_H

#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One software encode request.  Carries everything the encoder needs, so it
 * can serve both the M2M device and the capture driver's JPEG format without
 * either of them reaching into the other's state.
 */

struct bk7258_jpeg_sw_req_s
{
  FAR uint8_t *src;        /* Raw frame: I420, UYVY or VYUY */
  size_t       srclen;
  FAR uint8_t *dst;        /* Where the JPEG file goes */
  size_t       dstlen;
  FAR uint8_t *scratch;    /* De-interleave area, see below; NULL for I420 */
  size_t       scratch_size;
  uint32_t     width;
  uint32_t     height;
  uint32_t     pixelformat;
  int          quality;    /* 1..100 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: bk7258_jpeg_enc_initialize
 *
 * Description:
 *   Register the V4L2 M2M JPEG encoder at CONFIG_BK7258_JPEG_ENC_DEV_PATH.
 *
 *   Call this after PSRAM is up: the encoder's buffers and its de-interleave
 *   scratch both come from the PSRAM heap, because a single 640x480 I420
 *   frame is larger than the AP's entire malloc heap.  Registration itself
 *   allocates nothing, so an early call would not fail outright -- it would
 *   fail later at VIDIOC_REQBUFS, which is much harder to diagnose.
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

int bk7258_jpeg_enc_initialize(void);

/****************************************************************************
 * Name: bk7258_jpeg_sw_scratch_size
 *
 * Description:
 *   How much scratch bk7258_jpeg_sw_encode() needs to de-interleave one
 *   frame of this format and size into planes.  Zero for formats that are
 *   already planar (I420).
 *
 ****************************************************************************/

size_t bk7258_jpeg_sw_scratch_size(uint32_t pixelformat,
                                   uint32_t width, uint32_t height);

/****************************************************************************
 * Name: bk7258_jpeg_sw_encode
 *
 * Description:
 *   Encode one raw frame to JPEG with libjpeg-turbo.  Returns the encoded
 *   length, or a negated errno.
 *
 *   Measured 252-286ms for 640x480 at quality 80, so this must be called
 *   from a thread; an interrupt handler would stall the system for a third
 *   of a second.
 *
 ****************************************************************************/

int bk7258_jpeg_sw_encode(FAR const struct bk7258_jpeg_sw_req_s *req);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_JPEG_ENC_H */
