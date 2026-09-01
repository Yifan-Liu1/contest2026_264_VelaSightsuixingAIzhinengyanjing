/****************************************************************************
 * app/velasight/vs_media.c
 *
 * One-shot still capture for the idle voice assistant's photo-question path.
 * Opens /dev/video0, negotiates JPEG at a sensor-native geometry, waits for
 * exactly one frame, copies it out and closes the device again.  Modeled on
 * app/social_cue/social_cue_main.c's sc_capture() and the JPEG segment walk
 * from app/agent_camera/agent_camera_main.c's agent_camera_check_jpeg() --
 * this file only reuses their *logic*, not their translation units, so it
 * has no build dependency on either application.
 *
 * This module knows nothing about pages, requests or the app event queue.
 * It only does device I/O and hands back a self-contained buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/videoio.h>
#include <unistd.h>

#include <arch/chip/bk7258_psram.h>

#include "include/vs_media.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VS_MEDIA_VIDEO_DEV      "/dev/video0"
#define VS_MEDIA_NBUFFERS       2
#define VS_MEDIA_CAPTURE_TMO_MS 5000

/* The GC2145 driver only accepts these three exact geometries
 * (bk7258_gc2145_find_mode() in bk7258_camera_imgsensor.c does an exact
 * match, not nearest-fit).  sizeimage is sized for the largest of the three
 * so one constant covers all of them; 480x480 stays well under it.
 */

#define VS_MEDIA_SIZEIMAGE (160 * 1024)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: vs_media_geometry_supported
 *
 * Description:
 *   Reject unsupported geometry before it reaches the driver.  Keeping this
 *   list here (rather than trusting VIDIOC_S_FMT alone) means a caller gets
 *   -EINVAL immediately instead of a driver-specific failure several ioctls
 *   deep.
 *
 ****************************************************************************/

static bool vs_media_geometry_supported(uint16_t width, uint16_t height)
{
  static const struct
  {
    uint16_t width;
    uint16_t height;
  } modes[] =
  {
    { 480, 480 },
    { 640, 480 },
    { 864, 480 },
  };
  size_t i;

  for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    {
      if (modes[i].width == width && modes[i].height == height)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: vs_media_check_jpeg
 *
 * Description:
 *   Walk the JPEG segment chain rather than only checking SOI/EOI.  This
 *   board's encoder has produced frames with SOI and EOI both present but
 *   no SOS (no scan header), which decodes as garbage in any standard
 *   decoder including the vision model's.  Marker bytes may be preceded by
 *   filler 0xFF bytes, which this walk skips.
 *
 ****************************************************************************/

static bool vs_media_check_jpeg(const unsigned char *data, size_t len)
{
  bool have_sos = false;
  size_t i;

  if (len < 4 || data[0] != 0xff || data[1] != 0xd8)
    {
      return false;
    }

  i = 2;
  while (i + 1 < len)
    {
      uint8_t marker;
      size_t seglen;

      if (data[i] != 0xff)
        {
          break;
        }

      while (i + 1 < len && data[i + 1] == 0xff)
        {
          i++;
        }

      if (i + 1 >= len)
        {
          break;
        }

      marker = data[i + 1];
      if (marker == 0xd9)
        {
          break;
        }

      if (marker == 0xda)
        {
          have_sos = true;
          break;
        }

      if (i + 3 >= len)
        {
          break;
        }

      seglen = ((size_t)data[i + 2] << 8) | data[i + 3];
      if (seglen < 2)
        {
          break;
        }

      i += 2 + seglen;
    }

  if (!have_sos)
    {
      return false;
    }

  return data[len - 2] == 0xff && data[len - 1] == 0xd9;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_media_capture_jpeg(struct vs_media_frame_s *frame,
                          uint16_t width, uint16_t height)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_format fmt;
  struct pollfd pfd;
  void *addr[VS_MEDIA_NBUFFERS];
  size_t buflen[VS_MEDIA_NBUFFERS];
  unsigned char *copy = NULL;
  bool from_psram = true;
  int fd = -1;
  int ret;
  int i;

  if (frame == NULL)
    {
      return -EINVAL;
    }

  memset(frame, 0, sizeof(*frame));

  if (!vs_media_geometry_supported(width, height))
    {
      printf("vs_media: unsupported geometry %ux%u\n", width, height);
      return -EINVAL;
    }

  for (i = 0; i < VS_MEDIA_NBUFFERS; i++)
    {
      addr[i] = NULL;
      buflen[i] = 0;
    }

  fd = open(VS_MEDIA_VIDEO_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("vs_media: open %s failed, errno=%d\n",
             VS_MEDIA_VIDEO_DEV, errno);
      return -ENODEV;
    }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;
  fmt.fmt.pix.sizeimage   = VS_MEDIA_SIZEIMAGE;

  if (ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      ret = -errno;
      printf("vs_media: S_FMT JPEG %ux%u failed, errno=%d\n",
             width, height, errno);
      goto errout_close;
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = VS_MEDIA_NBUFFERS;

  if (ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      ret = -errno;
      printf("vs_media: REQBUFS failed, errno=%d\n", errno);
      goto errout_close;
    }

  for (i = 0; i < VS_MEDIA_NBUFFERS; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = (uint32_t)i;

      if (ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
        {
          ret = -errno;
          printf("vs_media: QUERYBUF[%d] failed, errno=%d\n", i, errno);
          goto errout_unmap;
        }

      addr[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, (off_t)buf.m.offset);
      if (addr[i] == MAP_FAILED)
        {
          addr[i] = NULL;
          ret = -errno;
          printf("vs_media: mmap[%d] failed, errno=%d\n", i, errno);
          goto errout_unmap;
        }

      buflen[i] = buf.length;

      if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
        {
          ret = -errno;
          printf("vs_media: QBUF[%d] failed, errno=%d\n", i, errno);
          goto errout_unmap;
        }
    }

  if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)&req.type) < 0)
    {
      ret = -errno;
      printf("vs_media: STREAMON failed, errno=%d\n", errno);
      goto errout_unmap;
    }

  pfd.fd     = fd;
  pfd.events = POLLIN;

  if (poll(&pfd, 1, VS_MEDIA_CAPTURE_TMO_MS) <= 0)
    {
      printf("vs_media: no frame within %d ms\n", VS_MEDIA_CAPTURE_TMO_MS);
      ret = -ETIMEDOUT;
      goto errout_streamoff;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
    {
      ret = -errno;
      printf("vs_media: DQBUF failed, errno=%d\n", errno);
      goto errout_streamoff;
    }

  if (buf.index >= VS_MEDIA_NBUFFERS || buf.bytesused == 0)
    {
      ret = -EIO;
      goto errout_streamoff;
    }

  if (!vs_media_check_jpeg(addr[buf.index], buf.bytesused))
    {
      printf("vs_media: captured frame is not a well-formed JPEG "
             "(%lu bytes)\n", (unsigned long)buf.bytesused);
      ret = -EBADMSG;
      goto errout_streamoff;
    }

  /* Copy out of the mmap'd driver buffer before it is handed back with
   * REQBUFS count=0 below.  Prefer PSRAM: this buffer can be tens of KB and
   * must not compete with task stacks in the SRAM heap for the duration of
   * a network round trip.  Fall back to the regular heap if PSRAM is
   * offline so a transient PSRAM power state does not turn into a lost
   * photo.
   */

  copy = bk7258_psram_malloc(buf.bytesused);
  if (copy == NULL)
    {
      from_psram = false;
      copy = malloc(buf.bytesused);
    }

  if (copy == NULL)
    {
      ret = -ENOMEM;
      goto errout_streamoff;
    }

  memcpy(copy, addr[buf.index], buf.bytesused);

  frame->data       = copy;
  frame->len         = buf.bytesused;
  frame->width       = width;
  frame->height       = height;
  frame->from_psram = from_psram;
  ret = 0;

errout_streamoff:
  ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&req.type);

errout_unmap:
  for (i = 0; i < VS_MEDIA_NBUFFERS; i++)
    {
      if (addr[i] != NULL)
        {
          munmap(addr[i], buflen[i]);
        }
    }

  /* Release the driver's buffer allocation explicitly rather than relying
   * on close() to do it.  This device is opened and closed once per photo
   * question, and letting an internal buffer count drift across repeated
   * open/close cycles is exactly the kind of thing that only shows up after
   * dozens of runs.
   */

  req.count = 0;
  ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req);

errout_close:
  close(fd);
  return ret;
}

void vs_media_frame_release(struct vs_media_frame_s *frame)
{
  if (frame == NULL || frame->data == NULL)
    {
      return;
    }

  if (frame->from_psram)
    {
      bk7258_psram_free(frame->data);
    }
  else
    {
      free(frame->data);
    }

  memset(frame, 0, sizeof(*frame));
}

/****************************************************************************
 * Resident capture
 ****************************************************************************/

struct vs_media_stream_s
{
  int      fd;
  uint16_t width;
  uint16_t height;
  bool     streaming;

  void  *addr[VS_MEDIA_NBUFFERS];
  size_t buflen[VS_MEDIA_NBUFFERS];

  /* Written by vs_media_stream_wake() from another thread and read by the
   * grab loop.  volatile rather than locked: it is a one-way latch a single
   * writer sets and never clears, so the only race is a grab that reads it
   * one poll interval late, which is what the short poll slice below bounds.
   */

  volatile bool stopping;

  uint32_t delivered;
  uint32_t malformed;
};

/* How long one poll() inside a grab may block, regardless of the caller's
 * timeout.  The caller's timeout is honoured by looping.
 *
 * This exists because there is nothing to interrupt a poll() on a V4L2
 * descriptor: no eventfd, no pipe in the set.  Slicing the wait is what makes
 * vs_media_stream_wake() take effect within a bounded time instead of at the
 * end of a wait sized for a stalled sensor.  50 ms is well under the shortest
 * gesture the UI reacts to, so a session ending never feels delayed by it, and
 * at ~3 FPS it costs at most a handful of extra syscalls per frame.
 */

#define VS_MEDIA_POLL_SLICE_MS 50

int vs_media_stream_open(struct vs_media_stream_s **stream,
                         uint16_t width, uint16_t height, uint32_t fps)
{
  struct vs_media_stream_s *s;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_format fmt;
  int ret;
  int i;

  if (stream == NULL)
    {
      return -EINVAL;
    }

  *stream = NULL;

  if (!vs_media_geometry_supported(width, height))
    {
      printf("vs_media: unsupported stream geometry %ux%u\n", width, height);
      return -EINVAL;
    }

  s = calloc(1, sizeof(*s));
  if (s == NULL)
    {
      return -ENOMEM;
    }

  s->fd     = -1;
  s->width  = width;
  s->height = height;

  s->fd = open(VS_MEDIA_VIDEO_DEV, O_RDWR);
  if (s->fd < 0)
    {
      /* EBUSY is worth separating from "no such device": the two mean
       * "something else owns the camera" and "there is no camera", and only
       * the first is something a user can resolve by leaving the other mode.
       */

      ret = errno == EBUSY ? -EBUSY : -ENODEV;
      printf("vs_media: stream open %s failed, errno=%d\n",
             VS_MEDIA_VIDEO_DEV, errno);
      free(s);
      return ret;
    }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;
  fmt.fmt.pix.sizeimage   = VS_MEDIA_SIZEIMAGE;

  if (ioctl(s->fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      ret = -errno;
      printf("vs_media: stream S_FMT JPEG %ux%u failed, errno=%d\n",
             width, height, errno);
      goto errout;
    }

  /* The delivery rate, here rather than just before STREAMON.  It only needs
   * the format, which is now set, and the driver only accepts it while the
   * stream is off -- and VIDIOC_QBUF below runs the framework's capture-state
   * machine, which is not somewhere to be relying on the state having stayed
   * put.
   *
   * Advisory by design: a driver that will not take the rate still streams, so
   * this logs and carries on at whatever rate the driver chose.  Losing the
   * request costs efficiency; failing the open would cost the session.
   */

  if (fps != 0)
    {
      struct v4l2_streamparm parm;

      memset(&parm, 0, sizeof(parm));
      parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      parm.parm.capture.timeperframe.numerator   = 1;
      parm.parm.capture.timeperframe.denominator = fps;

      if (ioctl(s->fd, VIDIOC_S_PARM, (unsigned long)&parm) < 0)
        {
          printf("vs_media: stream %lu fps not accepted (errno=%d), "
                 "continuing at the driver's rate\n",
                 (unsigned long)fps, errno);
          fps = 0;
        }
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = VS_MEDIA_NBUFFERS;

  if (ioctl(s->fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      ret = -errno;
      printf("vs_media: stream REQBUFS failed, errno=%d\n", errno);
      goto errout;
    }

  for (i = 0; i < VS_MEDIA_NBUFFERS; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = (uint32_t)i;

      if (ioctl(s->fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
        {
          ret = -errno;
          printf("vs_media: stream QUERYBUF[%d] failed, errno=%d\n", i,
                 errno);
          goto errout;
        }

      s->addr[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        s->fd, (off_t)buf.m.offset);
      if (s->addr[i] == MAP_FAILED)
        {
          s->addr[i] = NULL;
          ret = -errno;
          printf("vs_media: stream mmap[%d] failed, errno=%d\n", i, errno);
          goto errout;
        }

      s->buflen[i] = buf.length;

      if (ioctl(s->fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
        {
          ret = -errno;
          printf("vs_media: stream QBUF[%d] failed, errno=%d\n", i, errno);
          goto errout;
        }
    }

  if (ioctl(s->fd, VIDIOC_STREAMON, (unsigned long)&req.type) < 0)
    {
      ret = -errno;
      printf("vs_media: stream STREAMON failed, errno=%d\n", errno);
      goto errout;
    }

  s->streaming = true;
  *stream = s;

  if (fps != 0)
    {
      printf("vs_media: stream open %ux%u at up to %lu fps\n", width, height,
             (unsigned long)fps);
    }
  else
    {
      printf("vs_media: stream open %ux%u\n", width, height);
    }

  return 0;

errout:
  vs_media_stream_close(s);
  return ret;
}

int vs_media_stream_grab(struct vs_media_stream_s *stream,
                         struct vs_media_frame_s *frame,
                         unsigned int timeout_ms)
{
  struct v4l2_buffer buf;
  struct pollfd pfd;
  unsigned char *copy;
  bool from_psram = true;
  unsigned int waited = 0;
  int ret;

  if (stream == NULL || frame == NULL || !stream->streaming)
    {
      return -EINVAL;
    }

  memset(frame, 0, sizeof(*frame));

  pfd.fd     = stream->fd;
  pfd.events = POLLIN;

  /* Poll in slices so wake() is observed promptly.  The <= lets a caller pass
   * a timeout that is an exact multiple of the slice and still get that many
   * milliseconds of waiting rather than one slice less.
   */

  for (; ; )
    {
      unsigned int slice;

      if (stream->stopping)
        {
          return -ECANCELED;
        }

      if (waited >= timeout_ms)
        {
          return -ETIMEDOUT;
        }

      slice = timeout_ms - waited;
      if (slice > VS_MEDIA_POLL_SLICE_MS)
        {
          slice = VS_MEDIA_POLL_SLICE_MS;
        }

      pfd.revents = 0;
      ret = poll(&pfd, 1, (int)slice);
      if (ret > 0)
        {
          break;
        }

      if (ret < 0 && errno != EINTR)
        {
          return -errno;
        }

      waited += slice;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if (ioctl(stream->fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
    {
      return -errno;
    }

  if (buf.index >= VS_MEDIA_NBUFFERS || buf.bytesused == 0)
    {
      /* Requeue before returning.  A buffer dequeued and not given back is
       * gone for the life of the stream, and with only two of them a couple of
       * these would stall the sensor permanently.
       */

      ioctl(stream->fd, VIDIOC_QBUF, (unsigned long)&buf);
      return -EIO;
    }

  if (!vs_media_check_jpeg(stream->addr[buf.index], buf.bytesused))
    {
      stream->malformed++;
      ioctl(stream->fd, VIDIOC_QBUF, (unsigned long)&buf);
      return -EBADMSG;
    }

  /* PSRAM first, same reasoning as the one-shot path: a session holds one of
   * these across a network round trip, and tens of KB in the SRAM heap would
   * compete with the pthread stacks that can only come from there.
   */

  copy = bk7258_psram_malloc(buf.bytesused);
  if (copy == NULL)
    {
      from_psram = false;
      copy = malloc(buf.bytesused);
    }

  if (copy == NULL)
    {
      ioctl(stream->fd, VIDIOC_QBUF, (unsigned long)&buf);
      return -ENOMEM;
    }

  memcpy(copy, stream->addr[buf.index], buf.bytesused);

  /* Back to the driver immediately, before the caller does anything with the
   * frame.  This is the whole point of copying: the sensor keeps filling while
   * the upload blocks.
   */

  ioctl(stream->fd, VIDIOC_QBUF, (unsigned long)&buf);

  frame->data       = copy;
  frame->len        = buf.bytesused;
  frame->width      = stream->width;
  frame->height     = stream->height;
  frame->from_psram = from_psram;
  stream->delivered++;
  return 0;
}

void vs_media_stream_wake(struct vs_media_stream_s *stream, bool stop)
{
  if (stream == NULL)
    {
      return;
    }

  if (stop)
    {
      stream->stopping = true;
    }
}

void vs_media_stream_stats(struct vs_media_stream_s *stream,
                           uint32_t *delivered, uint32_t *malformed)
{
  if (delivered != NULL)
    {
      *delivered = stream != NULL ? stream->delivered : 0;
    }

  if (malformed != NULL)
    {
      *malformed = stream != NULL ? stream->malformed : 0;
    }
}

void vs_media_stream_close(struct vs_media_stream_s *stream)
{
  int i;

  if (stream == NULL)
    {
      return;
    }

  if (stream->fd >= 0)
    {
      if (stream->streaming)
        {
          int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          ioctl(stream->fd, VIDIOC_STREAMOFF, (unsigned long)&type);
          stream->streaming = false;
        }

      for (i = 0; i < VS_MEDIA_NBUFFERS; i++)
        {
          if (stream->addr[i] != NULL)
            {
              munmap(stream->addr[i], stream->buflen[i]);
              stream->addr[i] = NULL;
            }
        }

      /* Hand the driver's allocation back explicitly rather than relying on
       * close(), for the same reason the one-shot path does: an internal
       * buffer count that drifts across open/close cycles only shows up after
       * many sessions.
       */

      {
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof(req));
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        req.count  = 0;
        ioctl(stream->fd, VIDIOC_REQBUFS, (unsigned long)&req);
      }

      close(stream->fd);
      stream->fd = -1;
    }

  if (stream->delivered != 0 || stream->malformed != 0)
    {
      printf("vs_media: stream closed, %lu frames, %lu malformed\n",
             (unsigned long)stream->delivered,
             (unsigned long)stream->malformed);
    }

  free(stream);
}
