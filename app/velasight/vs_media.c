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
