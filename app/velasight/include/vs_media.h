#ifndef __APP_VELASIGHT_INCLUDE_VS_MEDIA_H
#define __APP_VELASIGHT_INCLUDE_VS_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One still frame captured from /dev/video0.  data is heap or PSRAM
 * allocated depending on from_psram; callers must always release it through
 * vs_media_frame_release() rather than free()/bk7258_psram_free() directly,
 * so the allocator choice stays an implementation detail of vs_media.c.
 */

struct vs_media_frame_s
{
  unsigned char *data;
  size_t         len;
  uint16_t       width;
  uint16_t       height;
  bool           from_psram;
};

/* Capture exactly one JPEG frame and close /dev/video0 before returning.
 * Blocking, roughly 300 ms.  width/height must match one of the sensor's
 * fixed modes (480x480, 640x480, 864x480); anything else fails fast with
 * -EINVAL instead of reaching the driver.
 *
 * Returns 0 and fills *frame, or a negative errno:
 *   -EINVAL    unsupported geometry or bad argument
 *   -ENODEV    /dev/video0 could not be opened
 *   -ETIMEDOUT no frame within the capture window
 *   -EBADMSG   a frame arrived but is not a well-formed JPEG
 *   -ENOMEM    no buffer available for the frame
 *   (other)    the underlying ioctl's errno
 */

int  vs_media_capture_jpeg(struct vs_media_frame_s *frame,
                           uint16_t width, uint16_t height);

/* Release a frame produced by vs_media_capture_jpeg().  Safe to call with a
 * zeroed or already-released frame. */

void vs_media_frame_release(struct vs_media_frame_s *frame);

#endif /* __APP_VELASIGHT_INCLUDE_VS_MEDIA_H */
