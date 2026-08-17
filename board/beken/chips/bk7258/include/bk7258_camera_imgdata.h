/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_camera_imgdata.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_CAMERA_IMGDATA_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_CAMERA_IMGDATA_H

#include <nuttx/video/imgdata.h>

/****************************************************************************
 * Name: bk7258_camera_imgdata_initialize
 *
 * Description:
 *   Returns the singleton imgdata_s instance implementing the BK7258
 *   YUV_BUF/DMA capture platform driver (imgdata_ops_s), for use with
 *   capture_register().  Does not itself touch hardware; the returned
 *   instance's ops->init() is what performs YUV_BUF/DMA controller
 *   initialization, invoked by the V4L2 core when the /dev/videoN node
 *   this is registered under is opened.
 *
 ****************************************************************************/

FAR struct imgdata_s *bk7258_camera_imgdata_initialize(void);


/****************************************************************************
 * Name: bk7258_camera_sw_jpeg_t
 *
 * Description:
 *   Software JPEG encoder used to serve V4L2_PIX_FMT_JPEG on this capture
 *   device.  src holds one raw frame in the sensor's own byte order (V Y1 U
 *   Y0, i.e. V4L2_PIX_FMT_VYUY); the encoder writes a complete JPEG file to
 *   dst and returns its length, or a negated errno.
 *
 *   Called from the low-priority work queue, never from an interrupt: the
 *   encode is ~270ms for 640x480.
 *
 ****************************************************************************/

typedef CODE int (*bk7258_camera_sw_jpeg_t)(FAR uint8_t *src, size_t srclen,
                                            FAR uint8_t *dst, size_t dstlen,
                                            uint32_t width, uint32_t height);

/****************************************************************************
 * Name: bk7258_camera_set_sw_jpeg
 *
 * Description:
 *   Register the encoder above, or NULL to leave JPEG capture to the
 *   hardware block.  The board registers one at bring-up because the
 *   hardware block mis-assembles its bitstream; see the implementation.
 *
 ****************************************************************************/

void bk7258_camera_set_sw_jpeg(bk7258_camera_sw_jpeg_t encoder);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_CAMERA_IMGDATA_H */
