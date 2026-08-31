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

/* Release a frame produced by either capture path below.  Safe to call with a
 * zeroed or already-released frame. */

void vs_media_frame_release(struct vs_media_frame_s *frame);

/****************************************************************************
 * Resident capture
 *
 * The social session's camera path.  Deliberately separate from the one-shot
 * function above rather than a loop around it: /dev/video0 has exactly one
 * owner, and re-running the whole open / S_FMT / REQBUFS / mmap / STREAMON /
 * STREAMOFF / munmap / close sequence per frame costs most of the ~300 ms a
 * frame takes.  A session sampling several times a second cannot afford that,
 * and the integration plan names holding the device open for the session as a
 * requirement rather than an optimisation.
 *
 * Threading: one handle belongs to one thread.  Nothing here is locked, and
 * the only concurrent call that is safe is vs_media_stream_wake(), which
 * exists so a session ending on the UI thread can shorten the poll a capture
 * thread is sitting in.
 ****************************************************************************/

struct vs_media_stream_s;

/****************************************************************************
 * Name: vs_media_stream_open
 *
 * Description:
 *   Open /dev/video0, negotiate JPEG at the given geometry, map the buffers
 *   and start streaming.  Blocking, and the expensive half of the capture
 *   cost: everything after this is a poll and a DQBUF.
 *
 *   Same geometry rule as vs_media_capture_jpeg() -- 480x480, 640x480 or
 *   864x480 -- rejected here rather than several ioctls deep.
 *
 * Returned Value:
 *   0 with *stream filled, or a negative errno.  -EBUSY when the device is
 *   already owned, which is the case worth naming: the idle assistant's photo
 *   path and this one are mutually exclusive by design, so a session starting
 *   while a photo question is mid-capture is a real conflict rather than a
 *   transient.
 *
 ****************************************************************************/

int vs_media_stream_open(struct vs_media_stream_s **stream,
                         uint16_t width, uint16_t height);

/****************************************************************************
 * Name: vs_media_stream_grab
 *
 * Description:
 *   Wait up to timeout_ms for a frame and copy it out.  The copy is what lets
 *   the buffer go straight back to the driver, so the sensor keeps running
 *   while the caller does whatever it does with the bytes.
 *
 *   Frames that arrive while the caller is away are overwritten in the
 *   driver's two-deep queue rather than queued, which is the intended
 *   behaviour: the session wants the most recent face, not a backlog of stale
 *   ones.
 *
 * Returned Value:
 *   0 with *frame filled -- the caller owns it and must release it.
 *   -ETIMEDOUT  no frame within timeout_ms; the stream is still healthy
 *   -EBADMSG    a frame arrived but is not a well-formed JPEG.  Recoverable:
 *               the buffer has been requeued and the next grab may succeed.
 *               The encoder on this board has produced frames with SOI and
 *               EOI but no scan header, so this is a real case, not defensive
 *               coding.
 *   -ECANCELED  vs_media_stream_wake() was called with stop set
 *   -ENOMEM     no buffer for the copy
 *
 ****************************************************************************/

int vs_media_stream_grab(struct vs_media_stream_s *stream,
                         struct vs_media_frame_s *frame,
                         unsigned int timeout_ms);

/****************************************************************************
 * Name: vs_media_stream_wake
 *
 * Description:
 *   Interrupt a grab that is waiting.  Safe from another thread and safe with
 *   stream == NULL.
 *
 *   With stop true, this grab and every later one answer -ECANCELED.  That is
 *   how a session ends promptly: without it, ending would have to wait out
 *   the current poll, and the poll is sized for a sensor that has stalled
 *   rather than for a user who has just let go of a button.
 *
 ****************************************************************************/

void vs_media_stream_wake(struct vs_media_stream_s *stream, bool stop);

/* Frames delivered and frames rejected by the JPEG check, for one stream.
 * The ratio is the only way to tell a camera producing nothing from one
 * producing garbage after the fact.  Safe with stream == NULL.
 */

void vs_media_stream_stats(struct vs_media_stream_s *stream,
                           uint32_t *delivered, uint32_t *malformed);

/* STREAMOFF, unmap, release the driver's buffers and close the device.  Safe
 * with stream == NULL.  Any frame already handed out stays valid: it is a
 * copy.
 */

void vs_media_stream_close(struct vs_media_stream_s *stream);

#endif /* __APP_VELASIGHT_INCLUDE_VS_MEDIA_H */
