/****************************************************************************
 * apps/camera_preview/preview_jpeg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <sys/videoio.h>

#include "preview_jpeg.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PJ_DEV        "/dev/video1"
#define PJ_TIMEOUT_MS 2000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct preview_jpeg_s
{
  int      fd;
  uint8_t *in;          /* mmap'd output-queue (i.e. encoder input) buffer */
  size_t   in_len;
  uint8_t *out;         /* mmap'd capture-queue (i.e. JPEG) buffer */
  size_t   out_len;
  size_t   frame_bytes; /* what the driver says one input frame is */
  uint32_t copy_ms;
  uint32_t codec_ms;
  bool     streaming;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t pj_now_ms(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/****************************************************************************
 * Name: pj_copy32
 *
 * Description:
 *   Word-at-a-time copy.  This libc's memcpy is byte-wise and measured at
 *   ~1.46us per byte on this board (see the access-cost table in
 *   camera_preview's preview_bench), which would put a 460800-byte frame at
 *   two thirds of a second.  Both ends are 4-byte aligned here: the codec
 *   pool is 32-byte aligned and the capture buffer comes from a PSRAM pool.
 *
 ****************************************************************************/

static void pj_copy32(uint8_t *dst, const uint8_t *src, size_t len)
{
  uint32_t       *d = (uint32_t *)dst;
  const uint32_t *s = (const uint32_t *)src;
  size_t          words = len >> 2;
  size_t          i;

  for (i = 0; i < words; i++)
    {
      d[i] = s[i];
    }

  for (i = words << 2; i < len; i++)
    {
      dst[i] = src[i];
    }
}

static int pj_setup_queue(struct preview_jpeg_s *ctx, enum v4l2_buf_type type,
                          uint8_t **addr, size_t *length)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  void *mem;

  memset(&req, 0, sizeof(req));
  req.count  = 1;               /* one frame in flight is all this needs */
  req.memory = V4L2_MEMORY_MMAP;
  req.type   = type;

  if (ioctl(ctx->fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      printf("preview_jpeg: REQBUFS(%s) failed, errno=%d\n",
             V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture", errno);
      return -errno;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = type;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index  = 0;

  if (ioctl(ctx->fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
    {
      printf("preview_jpeg: QUERYBUF(%s) failed, errno=%d\n",
             V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture", errno);
      return -errno;
    }

  mem = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
             ctx->fd, (off_t)buf.m.offset);
  if (mem == MAP_FAILED)
    {
      printf("preview_jpeg: mmap(%s) failed, errno=%d\n",
             V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture", errno);
      return -errno;
    }

  *addr   = (uint8_t *)mem;
  *length = buf.length;

  if (!V4L2_TYPE_IS_OUTPUT(type))
    {
      /* The JPEG buffer has to be queued before STREAMON or the encoder has
       * nowhere to put the result.
       */

      if (ioctl(ctx->fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
        {
          printf("preview_jpeg: QBUF(capture) failed, errno=%d\n", errno);
          return -errno;
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct preview_jpeg_s *preview_jpeg_open(int w, int h, int quality)
{
  struct preview_jpeg_s *ctx;
  struct v4l2_ext_controls ctrls;
  struct v4l2_ext_control ctrl;
  struct v4l2_format fmt;
  int type;

  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    {
      return NULL;
    }

  ctx->fd = open(PJ_DEV, O_RDWR);
  if (ctx->fd < 0)
    {
      printf("preview_jpeg: %s unavailable (errno=%d); build with "
             "CONFIG_BK7258_JPEG_ENC to get it\n", PJ_DEV, errno);
      free(ctx);
      return NULL;
    }

  /* Input: the camera's own byte order, so nothing has to be re-ordered
   * between preview and encode.  The driver accepts I420 / UYVY / VYUY.
   */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width       = (uint32_t)w;
  fmt.fmt.pix.height      = (uint32_t)h;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_VYUY;

  if (ioctl(ctx->fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      printf("preview_jpeg: S_FMT(output) %dx%d VYUY failed, errno=%d\n",
             w, h, errno);
      goto err;
    }

  ctx->frame_bytes = fmt.fmt.pix.sizeimage;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;

  if (ioctl(ctx->fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      printf("preview_jpeg: S_FMT(capture) JPEG failed, errno=%d\n", errno);
      goto err;
    }

  memset(&ctrl, 0, sizeof(ctrl));
  memset(&ctrls, 0, sizeof(ctrls));
  ctrl.id        = V4L2_CID_JPEG_COMPRESSION_QUALITY;
  ctrl.value     = quality;
  ctrls.count    = 1;
  ctrls.controls = &ctrl;

  if (ioctl(ctx->fd, VIDIOC_S_EXT_CTRLS, (unsigned long)&ctrls) < 0)
    {
      printf("preview_jpeg: quality %d rejected, errno=%d (using default)\n",
             quality, errno);
    }

  if (pj_setup_queue(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                     &ctx->in, &ctx->in_len) < 0 ||
      pj_setup_queue(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                     &ctx->out, &ctx->out_len) < 0)
    {
      goto err;
    }

  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(ctx->fd, VIDIOC_STREAMON, (unsigned long)&type) < 0)
    {
      printf("preview_jpeg: STREAMON(output) failed, errno=%d\n", errno);
      goto err;
    }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(ctx->fd, VIDIOC_STREAMON, (unsigned long)&type) < 0)
    {
      printf("preview_jpeg: STREAMON(capture) failed, errno=%d\n", errno);
      goto err;
    }

  ctx->streaming = true;

  printf("preview_jpeg: %s ready, input %dx%d VYUY %zu B/frame, "
         "JPEG buffer %zu B, quality %d\n",
         PJ_DEV, w, h, ctx->frame_bytes, ctx->out_len, quality);

  return ctx;

err:
  preview_jpeg_close(ctx);
  return NULL;
}

int preview_jpeg_encode(struct preview_jpeg_s *ctx,
                        const uint8_t *frame, size_t len,
                        const uint8_t **out, size_t *outlen)
{
  struct v4l2_buffer buf;
  struct pollfd pfd;
  uint32_t t0;
  size_t n;

  if (ctx == NULL || frame == NULL || out == NULL || outlen == NULL)
    {
      return -EINVAL;
    }

  /* The encoder reads only from its own pools, so the frame has to be copied
   * in.  This is the expensive half and it is measured on its own; passing
   * the capture buffer straight through as a USERPTR would remove it, but
   * bk7258_jpeg_addr_ok() rejects any address outside the codec's pools --
   * deliberately, because a wrong offset would point a DMA at arbitrary
   * memory.  Relaxing that to the PSRAM media window is a driver change with
   * its own review, not something to smuggle in here.
   */

  n = len < ctx->in_len ? len : ctx->in_len;
  if (n < ctx->frame_bytes)
    {
      printf("preview_jpeg: frame is %zu B, encoder wants %zu\n",
             len, ctx->frame_bytes);
      return -EINVAL;
    }

  t0 = pj_now_ms();
  pj_copy32(ctx->in, frame, ctx->frame_bytes);
  ctx->copy_ms = pj_now_ms() - t0;

  t0 = pj_now_ms();

  memset(&buf, 0, sizeof(buf));
  buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  buf.memory    = V4L2_MEMORY_MMAP;
  buf.index     = 0;
  buf.bytesused = (uint32_t)ctx->frame_bytes;

  if (ioctl(ctx->fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
    {
      printf("preview_jpeg: QBUF(output) failed, errno=%d\n", errno);
      return -errno;
    }

  pfd.fd     = ctx->fd;
  pfd.events = POLLIN;

  if (poll(&pfd, 1, PJ_TIMEOUT_MS) <= 0)
    {
      printf("preview_jpeg: no JPEG within %d ms\n", PJ_TIMEOUT_MS);
      return -ETIMEDOUT;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if (ioctl(ctx->fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
    {
      printf("preview_jpeg: DQBUF(capture) failed, errno=%d\n", errno);
      return -errno;
    }

  ctx->codec_ms = pj_now_ms() - t0;
  *out          = ctx->out;
  *outlen       = buf.bytesused;

  /* Hand both buffers back so the next call can reuse them. */

  if (ioctl(ctx->fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
    {
      printf("preview_jpeg: re-QBUF(capture) failed, errno=%d\n", errno);
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  buf.memory = V4L2_MEMORY_MMAP;
  ioctl(ctx->fd, VIDIOC_DQBUF, (unsigned long)&buf);

  return 0;
}

void preview_jpeg_last_ms(struct preview_jpeg_s *ctx,
                          uint32_t *copy_ms, uint32_t *codec_ms)
{
  if (ctx == NULL)
    {
      return;
    }

  if (copy_ms != NULL)
    {
      *copy_ms = ctx->copy_ms;
    }

  if (codec_ms != NULL)
    {
      *codec_ms = ctx->codec_ms;
    }
}

void preview_jpeg_close(struct preview_jpeg_s *ctx)
{
  int type;

  if (ctx == NULL)
    {
      return;
    }

  if (ctx->streaming)
    {
      type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      ioctl(ctx->fd, VIDIOC_STREAMOFF, (unsigned long)&type);
      type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      ioctl(ctx->fd, VIDIOC_STREAMOFF, (unsigned long)&type);
    }

  if (ctx->in != NULL)
    {
      munmap(ctx->in, ctx->in_len);
    }

  if (ctx->out != NULL)
    {
      munmap(ctx->out, ctx->out_len);
    }

  if (ctx->fd >= 0)
    {
      close(ctx->fd);
    }

  free(ctx);
}
