/****************************************************************************
 * app/jpeg_test/jpeg_test_main.c
 *
 * Exercise the BK7258 V4L2 M2M JPEG encoder without a camera.
 *
 * The encoder takes raw YUV from memory, so it can be driven entirely from a
 * synthetic frame -- which is the point: this validates the codec while the
 * camera capture path is still being built, and keeps the two pieces of work
 * independent.
 *
 * The generated frame is deliberately not noise.  Colour bars plus a
 * vertical luma ramp compress the way a real photo does (flat regions, hard
 * edges) and, more importantly, are recognisable by eye: after 'dump' the
 * JPEG can be
 * rebuilt on a PC and a wrong plane order, a swapped Cb/Cr, or an off-by-one
 * stride is obvious at a glance rather than hidden behind a plausible
 * compression ratio.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <nuttx/video/fb.h>
#include <sys/videoio.h>

/* jmorecfg.h defines EXTERN for its own use and NuttX headers have already
 * defined it as something else.
 */

#undef EXTERN
#include <jpeglib.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_JPEG_TEST_DEV_PATH
#  define CONFIG_JPEG_TEST_DEV_PATH "/dev/video1"
#endif

#define JPEG_TEST_NBUFFERS      2

#define JPEG_TEST_DEF_WIDTH     320
#define JPEG_TEST_DEF_HEIGHT    240
#define JPEG_TEST_DEF_QUALITY   80
#define JPEG_TEST_DEF_FRAMES    1

/* How long to wait for an encoded frame before giving up.  Software JPEG at
 * 320x240 is milliseconds of work, so anything approaching this means the
 * work queue never ran.
 */

/* Kept under the CP heartbeat's 2 s window on purpose.  A 5 s wait here plus
 * one frame of encoding added up to the six seconds of silence that made CP
 * assert and reset the board -- a test tool must fail its own run, not take
 * the system down with it.  A 320x240 frame measures ~830 ms, so 1500 ms is
 * generous while still bailing out before the heartbeat is at risk.
 */

/* How long to wait for one encoded frame.
 *
 * Measured cost is roughly 11 us per pixel (820 ms for 320x240), so a fixed
 * budget either starves large frames or lets a wedged driver hang the shell.
 * Scale it, with a floor for tiny frames.
 */

#define JPEG_TEST_TIMEOUT_FLOOR 1000
#define JPEG_TEST_US_PER_PIXEL  40
#define JPEG_TEST_POLL_MS       20

/* Bytes of JPEG per line of base64 output.  57 encodes to exactly 76
 * characters, which stays clear of the console's line length.
 */

#define JPEG_TEST_B64_CHUNK     57

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Frames the results array can hold.  Beyond this the run still encodes,
 * it just stops recording per-frame detail.
 */

#define JPEG_TEST_MAX_RESULTS   32

/* `show` defaults to the panel's own size so nothing is scaled: a scaling
 * step would be one more thing to blame when the picture looks wrong.
 */

#define JPEG_TEST_SHOW_SIZE     160
#define JPEG_TEST_FBDEV         "/dev/fb0"
#define JPEG_TEST_CAMDEV        "/dev/video0"

/* What the camera delivers.  Fixed: it enumerates exactly one frame size. */

#define JPEG_TEST_CAM_WIDTH     640
#define JPEG_TEST_CAM_HEIGHT    480

/* A frame at 57 fps arrives in under 20 ms; this is only a backstop. */

#define JPEG_TEST_CAM_TIMEOUT   2000

/* Colour bars in the synthetic pattern, and how far from a bar edge a pixel
 * has to be to count towards the aggregate error.  Chroma is subsampled, so
 * pixels straddling an edge legitimately differ from the reference.
 */

#define JPEG_TEST_BARS          8
#define JPEG_TEST_EDGE_MARGIN   3

struct jpeg_test_buf_s
{
  void     *addr;
  size_t    length;

  /* What QUERYBUF reported.  Required on every subsequent QBUF: the driver
   * rebuilds its pointer from it, so dropping it corrupts the address.
   */

  uint32_t  offset;
};

/* One frame's outcome, recorded rather than printed.
 *
 * Printing inside the loop is what caused the resets: AP console output
 * travels over the mailbox UART0_TX channel, and while that channel is busy
 * bk7258_mailbox_send_wire() returns -EBUSY, which the heartbeat discards
 * without retrying.  Enough console traffic and CP stops seeing heartbeats.
 */

/* Longjmp-based libjpeg error handling.  The default error_exit calls
 * exit(), which would take the whole program down on a malformed stream
 * rather than letting this report what happened.
 */

struct jpeg_test_jerr_s
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

/* A capture held open.  The frame stays mapped while the encoder copies out
 * of it, which is why this is a handle rather than a returned buffer.
 */

struct jpeg_test_capture_s
{
  int       fd;
  uint8_t  *frame;
  size_t    length;
};

struct jpeg_test_result_s
{
  uint32_t bytes;
  uint32_t ms;
  bool     soi_ok;
  bool     eoi_ok;
  bool     driver_error;
};

/* Where a decoded frame goes.  Pixels are addressed by stride because the
 * destination is the panel's buffer, whose rows need not be tightly packed,
 * and w/h bound the picture so it can be centred.
 */

struct jpeg_test_target_s
{
  uint16_t *px;
  uint32_t  stride;   /* in pixels, not bytes */
  uint32_t  w;
  uint32_t  h;
};

struct jpeg_test_ctx_s
{
  int    fd;
  struct jpeg_test_buf_s out[JPEG_TEST_NBUFFERS];   /* raw YUV in  */
  struct jpeg_test_buf_s cap[JPEG_TEST_NBUFFERS];   /* JPEG out    */
  int    nout;
  int    ncap;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The most recent encoded frame, kept so 'dump' can run as a separate
 * command.  Sized on first use.
 */

static uint8_t *g_last_jpeg;
static size_t   g_last_len;
static uint32_t g_last_width;
static uint32_t g_last_height;

static const char g_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Used ahead of their definitions: the pattern generators share the bar
 * table, and the camera path reuses the display and decode steps.
 */

static void jpeg_test_pattern_bar_chroma(uint32_t bar, uint8_t *cb,
                                         uint8_t *cr);
static void jpeg_test_reference_row(uint16_t *dst, uint32_t w, uint32_t h,
                                    uint32_t row, bool swap);
static uint16_t *jpeg_test_decode(struct jpeg_test_target_s *dst,
                                  uint32_t *width, uint32_t *height);
static int jpeg_test_verify(const uint16_t *got, uint32_t stride,
                            uint32_t w, uint32_t h);
static int jpeg_test_to_panel(bool bswap, bool verdict);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void jpeg_test_usage(void)
{
  printf("Usage: jpeg_test <enc|show|cam|dump|info> [options]\n"
         "  enc   generate a synthetic YUV frame and encode it\n"
         "  show  encode, decode and display it on " JPEG_TEST_FBDEV "\n"
         "  cam   encode one frame from " JPEG_TEST_CAMDEV "\n"
         "  dump  base64 the last encoded JPEG to the console\n"
         "  info  report the encoder's capabilities and formats\n"
         "  -w <px>  width, default %d (show: %d)\n"
         "  -h <px>  height, default %d (show: %d)\n"
         "  -q <1..100> quality, default %d\n"
         "  -f <i420|uyvy|vyuy> input format, default i420\n"
         "        vyuy is the camera's order, Cr Y1 Cb Y0\n"
         "  -n <count>  frames to encode, default %d\n"
         "  -s <0|1>  byte-swap pixels going to the panel, default 0\n"
         "  -d <0|1>  cam: display the result, default 1\n",
         JPEG_TEST_DEF_WIDTH, JPEG_TEST_SHOW_SIZE,
         JPEG_TEST_DEF_HEIGHT, JPEG_TEST_SHOW_SIZE,
         JPEG_TEST_DEF_QUALITY, JPEG_TEST_DEF_FRAMES);
}

/****************************************************************************
 * Name: jpeg_test_now_ms
 ****************************************************************************/

static uint64_t jpeg_test_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: jpeg_test_fill_i420
 *
 * Description:
 *   Eight colour bars over a top-to-bottom luma ramp, in planar 4:2:0.
 *
 *   Chroma is written at half resolution in both directions, so a driver
 *   that mixes up the Cb and Cr planes produces visibly wrong colours rather
 *   than a subtle shift.
 *
 ****************************************************************************/

static void jpeg_test_fill_i420(uint8_t *dst, uint32_t w, uint32_t h)
{
  /* Rec.601 chroma for white, yellow, cyan, green, magenta, red, blue and
   * black -- the classic bar order, so the result is recognisable.
   */

  static const uint8_t cb[8] =
    {
      128, 16, 166, 54, 202, 90, 240, 128
    };

  static const uint8_t cr[8] =
    {
      128, 146, 16, 34, 222, 240, 110, 128
    };

  uint8_t *y = dst;
  uint8_t *u = dst + (size_t)w * h;
  uint8_t *v = u + (size_t)w * h / 4;
  uint32_t row;
  uint32_t col;

  for (row = 0; row < h; row++)
    {
      uint8_t ramp = 40 + (uint8_t)((row * 175) / h);

      for (col = 0; col < w; col++)
        {
          uint32_t bar = col * 8 / w;

          /* Bar 7 stays black so the ramp shows up unmodulated
           * somewhere.
           */

          y[(size_t)row * w + col] = bar == 7 ? 16 : ramp;
        }
    }

  for (row = 0; row < h / 2; row++)
    {
      for (col = 0; col < w / 2; col++)
        {
          uint32_t bar = (col * 2) * 8 / w;

          u[(size_t)row * (w / 2) + col] = cb[bar];
          v[(size_t)row * (w / 2) + col] = cr[bar];
        }
    }
}

/****************************************************************************
 * Name: jpeg_test_fill_uyvy
 *
 * Description:
 *   The same picture as UYVY, so the two input paths can be compared against
 *   each other rather than only against expectations.
 *
 ****************************************************************************/

static void jpeg_test_fill_uyvy(uint8_t *dst, uint32_t w, uint32_t h)
{
  static const uint8_t cb[8] =
    {
      128, 16, 166, 54, 202, 90, 240, 128
    };

  static const uint8_t cr[8] =
    {
      128, 146, 16, 34, 222, 240, 110, 128
    };

  uint32_t row;
  uint32_t col;

  for (row = 0; row < h; row++)
    {
      uint8_t *p = dst + (size_t)row * w * 2;
      uint8_t ramp = 40 + (uint8_t)((row * 175) / h);

      for (col = 0; col + 1 < w; col += 2)
        {
          uint32_t bar = col * 8 / w;
          uint8_t luma = bar == 7 ? 16 : ramp;

          *p++ = cb[bar];
          *p++ = luma;
          *p++ = cr[bar];
          *p++ = luma;
        }
    }
}

/****************************************************************************
 * Name: jpeg_test_fill_vyuy
 *
 * Description:
 *   The same picture again, in the byte order the camera actually writes:
 *   Cr Y1 Cb Y0, the four bus bytes of each pair reversed.  Note which luma
 *   goes where -- the pair's even column is the *last* byte.
 *
 *   Having this lets the VYUY path be checked before a camera is involved.
 *
 ****************************************************************************/

static void jpeg_test_fill_vyuy(uint8_t *dst, uint32_t w, uint32_t h)
{
  uint32_t row;
  uint32_t col;

  for (row = 0; row < h; row++)
    {
      uint8_t *p = dst + (size_t)row * w * 2;
      uint8_t ramp = 40 + (uint8_t)((row * 175) / h);

      for (col = 0; col + 1 < w; col += 2)
        {
          uint32_t bar = col * JPEG_TEST_BARS / w;
          uint8_t luma = bar == JPEG_TEST_BARS - 1 ? 16 : ramp;
          uint8_t cb;
          uint8_t cr;

          jpeg_test_pattern_bar_chroma(bar, &cb, &cr);

          *p++ = cr;
          *p++ = luma;
          *p++ = cb;
          *p++ = luma;
        }
    }
}

/****************************************************************************
 * Name: jpeg_test_setup_queue
 *
 * Description:
 *   REQBUFS plus QUERYBUF and mmap for one queue.  CAPTURE buffers are
 *   queued immediately so the driver always has somewhere to put a result;
 *   OUTPUT buffers stay with us until there is a frame to put in them.
 *
 ****************************************************************************/

static int jpeg_test_setup_queue(struct jpeg_test_ctx_s *ctx,
                                 enum v4l2_buf_type type,
                                 struct jpeg_test_buf_s *bufs,
                                 int *count, int want)
{
  struct v4l2_requestbuffers req;
  int i;

  memset(&req, 0, sizeof(req));
  req.count  = (uint32_t)want;
  req.memory = V4L2_MEMORY_MMAP;
  req.type   = type;

  if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0)
    {
      if (errno != ENOMEM || req.count <= 1)
        {
          printf("jpeg_test: REQBUFS(%s) failed: %d\n",
                 V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture", errno);
          return -errno;
        }

      /* Out of pool memory for the pipelined case.  One buffer per queue
       * still works, it just cannot overlap the next frame with the
       * application holding the previous one.
       */

      printf("jpeg_test: %s pool needs %lu buffers, retrying with 1\n",
             V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture",
             (unsigned long)req.count);

      memset(&req, 0, sizeof(req));
      req.type   = type;
      req.memory = V4L2_MEMORY_MMAP;
      req.count  = 1;

      if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0)
        {
          printf("jpeg_test: REQBUFS(%s) failed even with 1: %d\n",
                 V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture", errno);
          return -errno;
        }
    }

  *count = req.count;

  for (i = 0; i < req.count; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = type;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
          printf("jpeg_test: QUERYBUF %d failed: %d\n", i, errno);
          return -errno;
        }

      bufs[i].length = buf.length;
      bufs[i].offset = buf.m.offset;
      bufs[i].addr   = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, ctx->fd, buf.m.offset);
      if (bufs[i].addr == MAP_FAILED)
        {
          printf("jpeg_test: mmap %d failed: %d\n", i, errno);
          return -errno;
        }

      if (!V4L2_TYPE_IS_OUTPUT(type))
        {
          if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0)
            {
              printf("jpeg_test: QBUF(capture) %d failed: %d\n", i, errno);
              return -errno;
            }
        }
    }

  return 0;
}

/****************************************************************************
 * Name: jpeg_test_encode
 ****************************************************************************/

static int jpeg_test_encode(uint32_t width, uint32_t height, int quality,
                            uint32_t pixfmt, int nframes,
                            const uint8_t *source)
{
  struct jpeg_test_ctx_s ctx;
  struct v4l2_format fmt;
  struct v4l2_ext_controls ctrls;
  struct v4l2_ext_control ctrl;
  struct v4l2_capability cap;
  struct jpeg_test_result_s results[JPEG_TEST_MAX_RESULTS];
  uint32_t timeout_ms = JPEG_TEST_TIMEOUT_FLOOR +
                        (width * height * JPEG_TEST_US_PER_PIXEL) / 1000;
  size_t rawsize;
  int want;
  int recorded = 0;
  int type;
  int frame;
  int ret = 0;
  int i;

  memset(results, 0, sizeof(results));

  memset(&ctx, 0, sizeof(ctx));

  ctx.fd = open(CONFIG_JPEG_TEST_DEV_PATH, O_RDWR);
  if (ctx.fd < 0)
    {
      printf("jpeg_test: cannot open %s: %d\n",
             CONFIG_JPEG_TEST_DEV_PATH, errno);
      return -errno;
    }

  memset(&cap, 0, sizeof(cap));
  if (ioctl(ctx.fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
      printf("jpeg_test: %s [%s]\n", cap.driver, cap.card);
    }

  /* Input format first: the encoder derives the encoded geometry from it, so
   * setting CAPTURE first would just be overwritten.
   */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = pixfmt;

  if (ioctl(ctx.fd, VIDIOC_S_FMT, &fmt) < 0)
    {
      printf("jpeg_test: S_FMT(output) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  /* The driver may have adjusted the geometry; everything below uses what it
   * actually accepted, not what was asked for.
   */

  width   = fmt.fmt.pix.width;
  height  = fmt.fmt.pix.height;
  rawsize = fmt.fmt.pix.sizeimage;

  printf("jpeg_test: input %lux%lu %c%c%c%c, %zu bytes/frame\n",
         (unsigned long)width, (unsigned long)height,
         (int)(pixfmt & 0xff), (int)((pixfmt >> 8) & 0xff),
         (int)((pixfmt >> 16) & 0xff), (int)((pixfmt >> 24) & 0xff),
         rawsize);

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;

  if (ioctl(ctx.fd, VIDIOC_S_FMT, &fmt) < 0)
    {
      printf("jpeg_test: S_FMT(capture) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  memset(&ctrl, 0, sizeof(ctrl));
  memset(&ctrls, 0, sizeof(ctrls));
  ctrl.id       = V4L2_CID_JPEG_COMPRESSION_QUALITY;
  ctrl.value    = quality;
  ctrls.count    = 1;
  ctrls.controls = &ctrl;

  if (ioctl(ctx.fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0)
    {
      printf("jpeg_test: quality request rejected: %d\n", errno);
    }

  /* Only what the run can use: a second buffer per queue overlaps the next
   * frame with the previous one, which a single-frame run never does, and at
   * 640x480 asking for it leaves the JPEG pool with nothing.
   */

  want = nframes > 1 ? JPEG_TEST_NBUFFERS : 1;

  ret = jpeg_test_setup_queue(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                              ctx.out, &ctx.nout, want);
  if (ret < 0)
    {
      goto out;
    }

  ret = jpeg_test_setup_queue(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                              ctx.cap, &ctx.ncap, want);
  if (ret < 0)
    {
      goto out;
    }

  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(ctx.fd, VIDIOC_STREAMON, &type) < 0)
    {
      printf("jpeg_test: STREAMON(output) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(ctx.fd, VIDIOC_STREAMON, &type) < 0)
    {
      printf("jpeg_test: STREAMON(capture) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  for (frame = 0; frame < nframes; frame++)
    {
      struct v4l2_buffer buf;
      const uint8_t *jpeg;
      uint8_t *keep;
      uint32_t dequeued;
      uint64_t start;
      uint64_t waited;
      int slot = frame % ctx.nout;

      if (source != NULL)
        {
          memcpy(ctx.out[slot].addr, source, rawsize);
        }
      else if (pixfmt == V4L2_PIX_FMT_UYVY)
        {
          jpeg_test_fill_uyvy(ctx.out[slot].addr, width, height);
        }
      else if (pixfmt == V4L2_PIX_FMT_VYUY)
        {
          jpeg_test_fill_vyuy(ctx.out[slot].addr, width, height);
        }
      else
        {
          jpeg_test_fill_i420(ctx.out[slot].addr, width, height);
        }

      memset(&buf, 0, sizeof(buf));
      buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      buf.memory    = V4L2_MEMORY_MMAP;
      buf.index     = slot;
      buf.m.offset  = ctx.out[slot].offset;
      buf.bytesused = rawsize;

      start = jpeg_test_now_ms();

      if (ioctl(ctx.fd, VIDIOC_QBUF, &buf) < 0)
        {
          printf("jpeg_test: QBUF(output) failed: %d\n", errno);
          ret = -errno;
          goto stop;
        }

      /* Wait for the encoded frame.  DQBUF here is non-blocking in this
       * framework, so poll rather than assume it sleeps.
       */

      for (waited = 0; waited < (int)timeout_ms;
           waited += JPEG_TEST_POLL_MS)
        {
          memset(&buf, 0, sizeof(buf));
          buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          buf.memory = V4L2_MEMORY_MMAP;

          if (ioctl(ctx.fd, VIDIOC_DQBUF, &buf) == 0)
            {
              break;
            }

          if (errno != EAGAIN)
            {
              printf("jpeg_test: DQBUF(capture) failed: %d\n", errno);
              ret = -errno;
              goto stop;
            }

          usleep(JPEG_TEST_POLL_MS * 1000);
        }

      if (waited >= (int)timeout_ms)
        {
          ret = -ETIMEDOUT;
          goto stop;
        }

      if (frame < JPEG_TEST_MAX_RESULTS)
        {
          recorded = frame + 1;
          results[frame].bytes = buf.bytesused;
          results[frame].ms    = (uint32_t)(jpeg_test_now_ms() - start);
          results[frame].driver_error =
            (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
        }

      if (buf.flags & V4L2_BUF_FLAG_ERROR)
        {
          ret = -EIO;
          goto stop;
        }

      /* Keep the last frame so 'dump' can emit it after this command has
       * released the device.
       */

      if (buf.bytesused > 0)
        {
          keep = realloc(g_last_jpeg, buf.bytesused);

          if (keep != NULL)
            {
              g_last_jpeg   = keep;
              memcpy(g_last_jpeg, ctx.cap[buf.index].addr, buf.bytesused);
              g_last_len    = buf.bytesused;
              g_last_width  = width;
              g_last_height = height;
            }

          /* A JPEG must start with SOI and end with EOI.  Recorded, not
           * printed, so the console stays quiet until the run is over.
           */

          jpeg = ctx.cap[buf.index].addr;

          if (frame < JPEG_TEST_MAX_RESULTS)
            {
              results[frame].soi_ok = jpeg[0] == 0xff && jpeg[1] == 0xd8;
              results[frame].eoi_ok =
                jpeg[buf.bytesused - 2] == 0xff &&
                jpeg[buf.bytesused - 1] == 0xd9;
            }
        }

      /* Requeue the buffer that was actually dequeued.
       *
       * Not "frame % ncap": the driver picks which buffer to fill, and
       * assuming a rotation instead of using the index DQBUF reported queued
       * one buffer twice and lost the other, after which the driver had
       * nowhere to write and stopped producing frames entirely.
       */

      dequeued = buf.index;

      memset(&buf, 0, sizeof(buf));
      buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory   = V4L2_MEMORY_MMAP;
      buf.index    = dequeued;
      buf.m.offset = ctx.cap[dequeued].offset;

      if (ioctl(ctx.fd, VIDIOC_QBUF, &buf) < 0)
        {
          printf("jpeg_test: QBUF(capture %lu) failed: %d\n",
                 (unsigned long)dequeued, errno);
          ret = -errno;
          goto stop;
        }

      /* Reclaim the consumed input buffers.
       *
       * Bounded on purpose: an unbounded "while (DQBUF == 0);" spins at
       * application priority with no yield, which starved the CP heartbeat
       * for six seconds and got the board reset.  There can never be more
       * than the queue depth outstanding, so that is the limit.
       */

      for (i = 0; i < ctx.nout; i++)
        {
          memset(&buf, 0, sizeof(buf));
          buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
          buf.memory = V4L2_MEMORY_MMAP;

          if (ioctl(ctx.fd, VIDIOC_DQBUF, &buf) < 0)
            {
              break;
            }
        }
    }

stop:
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  ioctl(ctx.fd, VIDIOC_STREAMOFF, &type);
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(ctx.fd, VIDIOC_STREAMOFF, &type);

  /* Safe to talk now: streaming has stopped, so competing for the mailbox
   * console channel can no longer cost a heartbeat.
   */

  for (i = 0; i < recorded; i++)
    {
      printf("jpeg_test: frame %d -> %lu bytes in %lu ms, ratio %lu:1  %s\n",
             i, (unsigned long)results[i].bytes,
             (unsigned long)results[i].ms,
             (unsigned long)(results[i].bytes ?
                             rawsize / results[i].bytes : 0),
             results[i].driver_error ? "DRIVER ERROR" :
             (results[i].soi_ok && results[i].eoi_ok) ? "valid JPEG" :
             !results[i].soi_ok ? "NO SOI" : "NO EOI");
    }

  if (ret == -ETIMEDOUT)
    {
      printf("jpeg_test: timed out after %d frame(s), budget was %lu ms\n",
             recorded, (unsigned long)timeout_ms);
    }

out:
  for (i = 0; i < ctx.nout; i++)
    {
      if (ctx.out[i].addr != NULL && ctx.out[i].addr != MAP_FAILED)
        {
          munmap(ctx.out[i].addr, ctx.out[i].length);
        }
    }

  for (i = 0; i < ctx.ncap; i++)
    {
      if (ctx.cap[i].addr != NULL && ctx.cap[i].addr != MAP_FAILED)
        {
          munmap(ctx.cap[i].addr, ctx.cap[i].length);
        }
    }

  close(ctx.fd);

  if (ret == 0 && g_last_len > 0)
    {
      printf("jpeg_test: run 'jpeg_test dump' to get the JPEG off "
             "the board\n");
    }

  return ret;
}

/****************************************************************************
 * Name: jpeg_test_pattern_bar_chroma
 *
 * Description:
 *   The Cb/Cr the pattern generators write for a given bar.  Shared with the
 *   reference builder so the comparison cannot drift from the source.
 *
 ****************************************************************************/

static void jpeg_test_pattern_bar_chroma(uint32_t bar, uint8_t *cb,
                                         uint8_t *cr)
{
  static const uint8_t cbtab[JPEG_TEST_BARS] =
    {
      128, 16, 166, 54, 202, 90, 240, 128
    };

  static const uint8_t crtab[JPEG_TEST_BARS] =
    {
      128, 146, 16, 34, 222, 240, 110, 128
    };

  *cb = cbtab[bar];
  *cr = crtab[bar];
}

/****************************************************************************
 * Name: jpeg_test_ycc_to_rgb565
 *
 * Description:
 *   JFIF full-range YCbCr to RGB565.  JPEG is JFIF by definition, so this is
 *   the conversion the decoder performs; using the same one here keeps the
 *   difference between them down to compression loss.
 *
 ****************************************************************************/

static uint16_t jpeg_test_ycc_to_rgb565(int y, int cb, int cr)
{
  int r = y + (1402 * (cr - 128)) / 1000;
  int g = y - (344 * (cb - 128)) / 1000 - (714 * (cr - 128)) / 1000;
  int b = y + (1772 * (cb - 128)) / 1000;

  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  b = b < 0 ? 0 : (b > 255 ? 255 : b);

  return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/****************************************************************************
 * Name: jpeg_test_reference
 *
 * Description:
 *   Build what the decoded frame should look like, from the definition of
 *   the test pattern rather than by running the generator again.  With swap
 *   Cb and Cr are exchanged, which is what a driver that mixes up the chroma
 *   planes would produce.
 *
 ****************************************************************************/

static void jpeg_test_reference_row(uint16_t *dst, uint32_t w, uint32_t h,
                                   uint32_t row, bool swap)
{
  int ramp = 40 + (int)((row * 175) / h);
  uint32_t col;

  for (col = 0; col < w; col++)
    {
      uint32_t bar = col * JPEG_TEST_BARS / w;
      uint8_t cb;
      uint8_t cr;
      int y;

      jpeg_test_pattern_bar_chroma(bar, &cb, &cr);
      y = bar == JPEG_TEST_BARS - 1 ? 16 : ramp;

      dst[col] = swap ? jpeg_test_ycc_to_rgb565(y, cr, cb) :
                        jpeg_test_ycc_to_rgb565(y, cb, cr);
    }
}

/****************************************************************************
 * Name: jpeg_test_capture
 *
 * Description:
 *   Grab one frame from the camera into a malloc'd buffer the caller owns.
 *
 *   The frame is copied out rather than handed over mapped, so the camera
 *   can be closed before the encoder allocates: they draw on different PSRAM
 *   pools, but holding a 614 KB capture buffer open for no reason is still
 *   worth avoiding.
 *
 ****************************************************************************/

static int jpeg_test_capture(struct jpeg_test_capture_s *cap,
                             uint32_t *width, uint32_t *height)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_format fmt;
  uint8_t *mapped = NULL;
  uint32_t offset = 0;
  size_t length = 0;
  uint32_t waited;
  int type;
  int fd;

  memset(cap, 0, sizeof(*cap));
  cap->fd = -1;

  fd = open(JPEG_TEST_CAMDEV, O_RDWR);
  if (fd < 0)
    {
      printf("jpeg_test: open %s failed: %d\n", JPEG_TEST_CAMDEV, errno);
      return -errno;
    }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                 = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width        = JPEG_TEST_CAM_WIDTH;
  fmt.fmt.pix.height       = JPEG_TEST_CAM_HEIGHT;

  /* The capture side advertises UYVY, which is the tag rather than the
   * layout; asking for anything else would simply be refused.
   */

  fmt.fmt.pix.pixelformat  = V4L2_PIX_FMT_UYVY;
  fmt.fmt.pix.field        = V4L2_FIELD_ANY;

  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0)
    {
      printf("jpeg_test: camera S_FMT failed: %d\n", errno);
      close(fd);
      return -errno;
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = 1;

  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      printf("jpeg_test: camera REQBUFS failed: %d\n", errno);
      close(fd);
      return -errno;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index  = 0;

  if (ioctl(fd, VIDIOC_QUERYBUF, (uintptr_t)&buf) < 0)
    {
      printf("jpeg_test: camera QUERYBUF failed: %d\n", errno);
      close(fd);
      return -errno;
    }

  length = buf.length;
  offset = buf.m.offset;
  mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                offset);

  if (mapped == MAP_FAILED)
    {
      printf("jpeg_test: camera mmap failed: %d\n", errno);
      close(fd);
      return -errno;
    }

  if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
    {
      printf("jpeg_test: camera QBUF failed: %d\n", errno);
      goto unmap;
    }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("jpeg_test: camera STREAMON failed: %d\n", errno);
      goto unmap;
    }

  for (waited = 0; waited < JPEG_TEST_CAM_TIMEOUT;
       waited += JPEG_TEST_POLL_MS)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buf) == 0)
        {
          break;
        }

      if (errno != EAGAIN)
        {
          printf("jpeg_test: camera DQBUF failed: %d\n", errno);
          goto streamoff;
        }

      usleep(JPEG_TEST_POLL_MS * 1000);
    }

  if (waited >= JPEG_TEST_CAM_TIMEOUT)
    {
      printf("jpeg_test: no frame from the camera in %d ms\n",
             JPEG_TEST_CAM_TIMEOUT);
      goto streamoff;
    }

  printf("jpeg_test: captured %dx%d, %zu bytes (%lu reported used)\n",
         JPEG_TEST_CAM_WIDTH, JPEG_TEST_CAM_HEIGHT, length,
         (unsigned long)buf.bytesused);

  /* Left streaming and mapped on purpose: the encoder copies out of this
   * buffer, and staging it anywhere else needs memory the main heap has not
   * got.
   */

  cap->fd     = fd;
  cap->frame  = mapped;
  cap->length = length;

  *width  = JPEG_TEST_CAM_WIDTH;
  *height = JPEG_TEST_CAM_HEIGHT;

  return 0;

streamoff:
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type);

unmap:
  munmap(mapped, length);
  close(fd);
  return -EIO;
}

/****************************************************************************
 * Name: jpeg_test_capture_release
 ****************************************************************************/

static void jpeg_test_capture_release(struct jpeg_test_capture_s *cap)
{
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (cap->fd < 0)
    {
      return;
    }

  ioctl(cap->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
  munmap(cap->frame, cap->length);
  close(cap->fd);

  cap->fd    = -1;
  cap->frame = NULL;
}

/****************************************************************************
 * Name: jpeg_test_check_order
 *
 * Description:
 *   Confirm which byte of each pair holds which luma, from the frame itself.
 *
 *   Reported as two smoothness figures.  Whichever assignment makes the
 *   within-pair difference match the across-pair difference is the correct
 *   one; the wrong one roughly doubles the across-pair figure because it
 *   transposes every adjacent column.
 *
 ****************************************************************************/

static void jpeg_test_check_order(const uint8_t *frame, uint32_t w,
                                  uint32_t h)
{
  uint32_t rows = h > 64 ? 64 : h;
  uint32_t within[2] = {
    0, 0
  };

  uint32_t across[2] = {
    0, 0
  };

  uint32_t n = 0;
  uint32_t row;
  uint32_t col;
  int which;

  /* Two candidate luma placements: bytes 3 and 1 (the camera's, even column
   * last) against bytes 1 and 3 (textbook).
   */

  static const uint8_t y0[2] = {
    3, 1
  };

  static const uint8_t y1[2] = {
    1, 3
  };

  for (row = 0; row < rows; row++)
    {
      const uint8_t *p = frame + (size_t)row * w * 2;

      for (col = 0; col + 3 < w; col += 2)
        {
          for (which = 0; which < 2; which++)
            {
              int a = p[y0[which]];
              int b = p[y1[which]];
              int c = p[4 + y0[which]];
              int d;

              d = b - a;
              within[which] += (uint32_t)(d < 0 ? -d : d);

              d = c - b;
              across[which] += (uint32_t)(d < 0 ? -d : d);
            }

          n++;
          p += 4;
        }
    }

  if (n == 0)
    {
      return;
    }

  /* Scaled by 100 so the ratio survives integer division. */

  for (which = 0; which < 2; which++)
    {
      uint32_t wi = within[which] * 100 / n;
      uint32_t ac = across[which] * 100 / n;

      printf("jpeg_test: luma %u/%u: within %lu.%02lu across %lu.%02lu%s\n",
             y0[which], y1[which],
             (unsigned long)(wi / 100), (unsigned long)(wi % 100),
             (unsigned long)(ac / 100), (unsigned long)(ac % 100),
             which == 0 ? "   <- used" : "");
    }

  printf("jpeg_test: the assignment whose two figures agree is the right "
         "one\n");
}

/****************************************************************************
 * Name: jpeg_test_cam
 *
 * Description:
 *   Capture one frame and encode it, which is the path the image recognition
 *   work needs.  Optionally show the result.
 *
 ****************************************************************************/

static int jpeg_test_cam(int quality, bool display, bool bswap)
{
  struct jpeg_test_capture_s cap;
  uint32_t width = 0;
  uint32_t height = 0;
  int ret;

  ret = jpeg_test_capture(&cap, &width, &height);
  if (ret < 0)
    {
      return ret;
    }

  jpeg_test_check_order(cap.frame, width, height);

  /* VYUY, not UYVY: see jpeg_test_fill_vyuy() and the encoder's format
   * description.  This is the whole point of the exercise.
   */

  ret = jpeg_test_encode(width, height, quality, V4L2_PIX_FMT_VYUY, 1,
                         cap.frame);
  jpeg_test_capture_release(&cap);

  if (ret < 0)
    {
      return ret;
    }

  if (display)
    {
      /* No verdict here: a camera frame has no reference to compare with.
       * `show` is where the encoder's correctness is established.
       */

      ret = jpeg_test_to_panel(bswap, false);
    }

  return ret;
}

/****************************************************************************
 * Name: jpeg_test_to_panel
 *
 * Description:
 *   Decode the last encoded frame into the panel's buffer and show it,
 *   optionally reporting whether the round trip preserved the picture.
 *
 *   The frame buffer is the decode destination rather than a staging copy:
 *   it is already RGB565 at the panel's size and lives in PSRAM, so the
 *   frame stays out of the main heap, which libjpeg needs all of.
 *
 ****************************************************************************/

static int jpeg_test_to_panel(bool bswap, bool verdict)
{
  struct jpeg_test_target_s dst;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct fb_area_s area;
  uint16_t *fbmem;
  uint16_t *px;
  uint32_t dw = 0;
  uint32_t dh = 0;
  int ret = 0;
  int fd;

  fd = open(JPEG_TEST_FBDEV, O_RDWR);
  if (fd < 0)
    {
      printf("jpeg_test: open %s failed: %d\n", JPEG_TEST_FBDEV, errno);
      return -errno;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (uintptr_t)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (uintptr_t)&pinfo) < 0)
    {
      printf("jpeg_test: framebuffer query failed: %d\n", errno);
      close(fd);
      return -errno;
    }

  if (vinfo.fmt != FB_FMT_RGB16_565)
    {
      printf("jpeg_test: panel is not RGB565 (fmt %u)\n",
             (unsigned int)vinfo.fmt);
      close(fd);
      return -ENOTSUP;
    }

  fbmem = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (fbmem == MAP_FAILED)
    {
      printf("jpeg_test: framebuffer mmap failed: %d\n", errno);
      close(fd);
      return -errno;
    }

  memset(fbmem, 0, pinfo.fblen);

  dst.px     = fbmem;
  dst.stride = pinfo.stride / 2;
  dst.w      = vinfo.xres;
  dst.h      = vinfo.yres;

  px = jpeg_test_decode(&dst, &dw, &dh);
  if (px == NULL)
    {
      munmap(fbmem, pinfo.fblen);
      close(fd);
      return -EIO;
    }

  printf("jpeg_test: decoded %lux%lu from %zu bytes\n",
         (unsigned long)dw, (unsigned long)dh, g_last_len);

  if (verdict)
    {
      ret = jpeg_test_verify(px, dst.stride, dw, dh);
    }

  /* Swap after the verdict, not before: the wire order is a property of the
   * QSPI burst, while the verdict is about the pixels.  The whole buffer can
   * be swapped because the border memset to 0 is unchanged by it.
   */

  if (bswap)
    {
      size_t i;

      for (i = 0; i < pinfo.fblen / 2; i++)
        {
          uint16_t c = fbmem[i];

          fbmem[i] = (uint16_t)((c >> 8) | (c << 8));
        }
    }

  area.x = 0;
  area.y = 0;
  area.w = (fb_coord_t)vinfo.xres;
  area.h = (fb_coord_t)vinfo.yres;

  if (ioctl(fd, FBIO_UPDATE, (uintptr_t)&area) < 0)
    {
      printf("jpeg_test: FBIO_UPDATE failed: %d\n", errno);
      ret = ret == 0 ? -errno : ret;
    }
  else
    {
      printf("jpeg_test: %lux%lu shown on a %ux%u panel%s\n",
             (unsigned long)dw, (unsigned long)dh,
             (unsigned int)vinfo.xres, (unsigned int)vinfo.yres,
             bswap ? ", byte-swapped" : "");
    }

  munmap(fbmem, pinfo.fblen);
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: jpeg_test_jerr_exit
 ****************************************************************************/

static void jpeg_test_jerr_exit(j_common_ptr cinfo)
{
  struct jpeg_test_jerr_s *err = (struct jpeg_test_jerr_s *)cinfo->err;
  char msg[JMSG_LENGTH_MAX];

  (*cinfo->err->format_message)(cinfo, msg);
  printf("jpeg_test: libjpeg: %s\n", msg);

  longjmp(err->setjmp_buffer, 1);
}

/****************************************************************************
 * Name: jpeg_test_decode
 *
 * Description:
 *   Decode the last encoded frame straight to RGB565, the panel's format.
 *   Returns a malloc'd buffer of w*h pixels, or NULL.
 *
 ****************************************************************************/

static uint16_t *jpeg_test_decode(struct jpeg_test_target_s *dst,
                                  uint32_t *width, uint32_t *height)
{
  struct jpeg_decompress_struct dinfo;
  struct jpeg_test_jerr_s jerr;
  uint32_t maxw = dst->w;
  uint32_t maxh = dst->h;
  uint16_t *out = NULL;
  uint32_t row = 0;
  unsigned int num;

  if (g_last_jpeg == NULL || g_last_len == 0)
    {
      printf("jpeg_test: nothing encoded yet\n");
      return NULL;
    }

  memset(&dinfo, 0, sizeof(dinfo));
  dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_test_jerr_exit;

  if (setjmp(jerr.setjmp_buffer))
    {
      jpeg_destroy_decompress(&dinfo);
      return NULL;
    }

  jpeg_create_decompress(&dinfo);
  jpeg_mem_src(&dinfo, g_last_jpeg, (unsigned long)g_last_len);

  if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK)
    {
      printf("jpeg_test: not a complete JPEG header\n");
      jpeg_destroy_decompress(&dinfo);
      return NULL;
    }

  dinfo.out_color_space = JCS_RGB565;

  /* Largest N/8 reduction that fits.  libjpeg does this inside the IDCT, so
   * it costs less than decoding full size, and it keeps the buffers down to
   * what the panel can actually show.
   */

  for (num = 8; num >= 1; num--)
    {
      dinfo.scale_num   = num;
      dinfo.scale_denom = 8;
      jpeg_calc_output_dimensions(&dinfo);

      if (dinfo.output_width <= maxw && dinfo.output_height <= maxh)
        {
          break;
        }
    }

  if (dinfo.output_width > maxw || dinfo.output_height > maxh)
    {
      printf("jpeg_test: %lux%lu will not reduce to fit %lux%lu\n",
             (unsigned long)dinfo.image_width,
             (unsigned long)dinfo.image_height,
             (unsigned long)maxw, (unsigned long)maxh);
      jpeg_destroy_decompress(&dinfo);
      return NULL;
    }

  if (num != 8)
    {
      printf("jpeg_test: decoding at %u/8 to fit the panel\n", num);
    }

  /* Centre the picture in the target; rows are written straight into it.
   *
   * RGB565 reports output_components as 3 but writes two bytes per pixel
   * (jdcol565.c advances by 2), so the row stride follows the format, not
   * that field.
   *
   * The target is the panel's buffer in PSRAM, so a whole frame no longer
   * competes with libjpeg's working pools over the 88 KB left in the main
   * heap.  That competition is what produced "Insufficient memory (case 4)"
   * regardless of which of the two asked first.  Output dimensions are final
   * here: jpeg_calc_output_dimensions set them in the scale loop above, and
   * start_decompress only recomputes the same values.
   */

  out = dst->px + (size_t)((maxh - dinfo.output_height) / 2) * dst->stride +
        (maxw - dinfo.output_width) / 2;

  if (!jpeg_start_decompress(&dinfo))
    {
      printf("jpeg_test: start_decompress refused\n");
      jpeg_destroy_decompress(&dinfo);
      return NULL;
    }

  while (dinfo.output_scanline < dinfo.output_height)
    {
      JSAMPROW rows[1];

      rows[0] = (JSAMPROW)(out + (size_t)row * dst->stride);

      if (jpeg_read_scanlines(&dinfo, rows, 1) != 1)
        {
          printf("jpeg_test: decode stalled at row %lu\n",
                 (unsigned long)row);
          jpeg_abort_decompress(&dinfo);
          jpeg_destroy_decompress(&dinfo);
          return NULL;
        }

      row++;
    }

  *width  = dinfo.output_width;
  *height = dinfo.output_height;

  jpeg_finish_decompress(&dinfo);
  jpeg_destroy_decompress(&dinfo);

  return out;
}

/****************************************************************************
 * Name: jpeg_test_channel_error
 *
 * Description:
 *   Mean absolute per-channel error between two RGB565 images, in 8-bit
 *   terms, ignoring pixels close to a bar edge where chroma subsampling
 *   makes a difference legitimate.
 *
 ****************************************************************************/

static void jpeg_test_channel_error(const uint16_t *got, uint32_t stride,
                                    uint16_t *rowbuf,
                                    uint32_t w, uint32_t h, bool swap,
                                    uint32_t *er, uint32_t *eg,
                                    uint32_t *eb)
{
  uint32_t sumr = 0;
  uint32_t sumg = 0;
  uint32_t sumb = 0;
  uint32_t n = 0;
  uint32_t row;
  uint32_t col;

  for (row = 0; row < h; row++)
    {
      /* Regenerate this row of the reference and drop it again.  The pattern
       * is defined per pixel, so keeping the whole image was only ever
       * convenience, and it is convenience the heap can no longer afford.
       */

      jpeg_test_reference_row(rowbuf, w, h, row, swap);

      for (col = 0; col < w; col++)
        {
          uint32_t barw = w / JPEG_TEST_BARS;
          uint32_t off = barw ? col % barw : 0;
          size_t i = (size_t)row * stride + col;
          int d;

          if (barw > 2 * JPEG_TEST_EDGE_MARGIN &&
              (off < JPEG_TEST_EDGE_MARGIN ||
               off >= barw - JPEG_TEST_EDGE_MARGIN))
            {
              continue;
            }

          d = (int)((got[i] >> 11) & 0x1f) -
              (int)((rowbuf[col] >> 11) & 0x1f);
          sumr += (uint32_t)(d < 0 ? -d : d) * 8;

          d = (int)((got[i] >> 5) & 0x3f) - (int)((rowbuf[col] >> 5) & 0x3f);
          sumg += (uint32_t)(d < 0 ? -d : d) * 4;

          d = (int)(got[i] & 0x1f) - (int)(rowbuf[col] & 0x1f);
          sumb += (uint32_t)(d < 0 ? -d : d) * 8;

          n++;
        }
    }

  if (n == 0)
    {
      n = 1;
    }

  *er = sumr / n;
  *eg = sumg / n;
  *eb = sumb / n;
}

/****************************************************************************
 * Name: jpeg_test_unpack565
 *
 * Description:
 *   RGB565 back to 8-bit channels, so differences can be read in the units
 *   the source was written in.
 *
 ****************************************************************************/

static void jpeg_test_unpack565(uint16_t px, int *r, int *g, int *b)
{
  *r = (int)((px >> 11) & 0x1f) * 8;
  *g = (int)((px >> 5) & 0x3f) * 4;
  *b = (int)(px & 0x1f) * 8;
}

/****************************************************************************
 * Name: jpeg_test_verify
 *
 * Description:
 *   Decide whether the encoder produced the picture it was given, without
 *   involving the panel.  The decoded frame is scored against the pattern's
 *   definition and against the same reference with Cb and Cr exchanged;
 *   whichever scores lower is the chroma order that actually came out, so no
 *   absolute threshold has to be chosen.
 *
 ****************************************************************************/

static int jpeg_test_verify(const uint16_t *got, uint32_t stride,
                            uint32_t w, uint32_t h)
{
  uint16_t *rowbuf;
  uint32_t nr;
  uint32_t ng;
  uint32_t nb;
  uint32_t sr;
  uint32_t sg;
  uint32_t sb;
  uint32_t normal;
  uint32_t swap;
  uint32_t bar;
  int ret = 0;

  rowbuf = malloc((size_t)w * 2);

  if (rowbuf == NULL)
    {
      printf("jpeg_test: no memory for a %lu pixel reference row\n",
             (unsigned long)w);
      return -ENOMEM;
    }

  jpeg_test_channel_error(got, stride, rowbuf, w, h, false,
                          &nr, &ng, &nb);
  jpeg_test_channel_error(got, stride, rowbuf, w, h, true, &sr, &sg, &sb);

  normal = nr + ng + nb;
  swap   = sr + sg + sb;

  printf("jpeg_test: mean |error| vs reference   R%lu G%lu B%lu (sum %lu)\n",
         (unsigned long)nr, (unsigned long)ng, (unsigned long)nb,
         (unsigned long)normal);
  printf("jpeg_test: mean |error| vs Cb/Cr swap  R%lu G%lu B%lu (sum %lu)\n",
         (unsigned long)sr, (unsigned long)sg, (unsigned long)sb,
         (unsigned long)swap);

  /* Bar centres are far from every subsampling edge, so a mismatch here is
   * the encoder's and not the format's.
   */

  printf("jpeg_test: bar  expected RGB   decoded RGB   worst channel\n");

  for (bar = 0; bar < JPEG_TEST_BARS; bar++)
    {
      uint32_t col = (bar * w) / JPEG_TEST_BARS + w / (2 * JPEG_TEST_BARS);
      size_t i = (size_t)(h / 2) * stride + col;
      int wr;
      int wg;
      int wb;
      int gr;
      int gg;
      int gb;
      int worst;
      int d;

      jpeg_test_reference_row(rowbuf, w, h, h / 2, false);
      jpeg_test_unpack565(rowbuf[col], &wr, &wg, &wb);
      jpeg_test_unpack565(got[i], &gr, &gg, &gb);

      worst = gr - wr;
      worst = worst < 0 ? -worst : worst;
      d = gg - wg;
      d = d < 0 ? -d : d;
      worst = d > worst ? d : worst;
      d = gb - wb;
      d = d < 0 ? -d : d;
      worst = d > worst ? d : worst;

      printf("jpeg_test:  %lu   %3d,%3d,%3d   %3d,%3d,%3d   %d%s\n",
             (unsigned long)bar, wr, wg, wb, gr, gg, gb, worst,
             worst > 40 ? "  <-- wrong" : "");
    }

  if (swap < normal)
    {
      printf("jpeg_test: VERDICT chroma planes are exchanged -- the swapped "
             "reference fits better\n");
      ret = -EIO;
    }
  else if (normal > 40)
    {
      printf("jpeg_test: VERDICT chroma order is right but the error is "
             "large; check luma stride and sampling factors\n");
      ret = -EIO;
    }
  else
    {
      printf("jpeg_test: VERDICT picture matches the source, chroma order "
             "and strides are correct\n");
    }

  free(rowbuf);
  return ret;
}

/****************************************************************************
 * Name: jpeg_test_show
 *
 * Description:
 *   Encode a frame, decode it back and put it on the panel, reporting
 *   whether the round trip preserved the picture.
 *
 ****************************************************************************/

static int jpeg_test_show(uint32_t width, uint32_t height, int quality,
                          uint32_t pixfmt, bool bswap)
{
  int ret;

  ret = jpeg_test_encode(width, height, quality, pixfmt, 1, NULL);
  if (ret < 0)
    {
      return ret;
    }

  /* to_panel shows the picture whether or not the verdict likes it: a wrong
   * picture on the glass is worth seeing, and the verdict says whether to
   * trust it.
   */

  return jpeg_test_to_panel(bswap, true);
}

/****************************************************************************
 * Name: jpeg_test_dump
 *
 * Description:
 *   Print the last encoded frame as base64.
 *
 *   The board has no writable filesystem and the AP console is a mailbox
 *   command path, so text over the console is the only way out.  base64
 *   survives that path; raw bytes would not.
 *
 ****************************************************************************/

static int jpeg_test_dump(void)
{
  size_t i;

  if (g_last_jpeg == NULL || g_last_len == 0)
    {
      printf("jpeg_test: nothing encoded yet, run 'jpeg_test enc' first\n");
      return -ENODATA;
    }

  printf("jpeg_test: ---BEGIN JPEG BASE64--- %lux%lu %zu bytes\n",
         (unsigned long)g_last_width, (unsigned long)g_last_height,
         g_last_len);

  for (i = 0; i < g_last_len; i += JPEG_TEST_B64_CHUNK)
    {
      size_t n = g_last_len - i;
      size_t j;
      char line[80];
      int p = 0;

      if (n > JPEG_TEST_B64_CHUNK)
        {
          n = JPEG_TEST_B64_CHUNK;
        }

      for (j = 0; j < n; j += 3)
        {
          uint32_t v = (uint32_t)g_last_jpeg[i + j] << 16;
          size_t rem = n - j;

          if (rem > 1)
            {
              v |= (uint32_t)g_last_jpeg[i + j + 1] << 8;
            }

          if (rem > 2)
            {
              v |= g_last_jpeg[i + j + 2];
            }

          line[p++] = g_b64[(v >> 18) & 0x3f];
          line[p++] = g_b64[(v >> 12) & 0x3f];
          line[p++] = rem > 1 ? g_b64[(v >> 6) & 0x3f] : '=';
          line[p++] = rem > 2 ? g_b64[v & 0x3f] : '=';
        }

      line[p] = '\0';
      printf("%s\n", line);
    }

  printf("jpeg_test: ---END JPEG BASE64---\n");
  return 0;
}

/****************************************************************************
 * Name: jpeg_test_info
 ****************************************************************************/

static int jpeg_test_info(void)
{
  struct v4l2_capability cap;
  struct v4l2_fmtdesc fmt;
  int fd;
  int i;

  fd = open(CONFIG_JPEG_TEST_DEV_PATH, O_RDWR);
  if (fd < 0)
    {
      printf("jpeg_test: cannot open %s: %d\n",
             CONFIG_JPEG_TEST_DEV_PATH, errno);
      return -errno;
    }

  memset(&cap, 0, sizeof(cap));
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
      printf("jpeg_test: driver=%s card=%s caps=0x%08lx\n",
             cap.driver, cap.card, (unsigned long)cap.capabilities);
    }

  for (i = 0; i < 8; i++)
    {
      memset(&fmt, 0, sizeof(fmt));
      fmt.index = i;
      fmt.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT;

      if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
        {
          break;
        }

      printf("  input  %d: %c%c%c%c  %s\n", i,
             (int)(fmt.pixelformat & 0xff),
             (int)((fmt.pixelformat >> 8) & 0xff),
             (int)((fmt.pixelformat >> 16) & 0xff),
             (int)((fmt.pixelformat >> 24) & 0xff),
             fmt.description);
    }

  for (i = 0; i < 8; i++)
    {
      memset(&fmt, 0, sizeof(fmt));
      fmt.index = i;
      fmt.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
        {
          break;
        }

      printf("  output %d: %c%c%c%c  %s\n", i,
             (int)(fmt.pixelformat & 0xff),
             (int)((fmt.pixelformat >> 8) & 0xff),
             (int)((fmt.pixelformat >> 16) & 0xff),
             (int)((fmt.pixelformat >> 24) & 0xff),
             fmt.description);
    }

  close(fd);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  bool show = argc > 1 && strcmp(argv[1], "show") == 0;
  uint32_t width = show ? JPEG_TEST_SHOW_SIZE : JPEG_TEST_DEF_WIDTH;
  uint32_t height = show ? JPEG_TEST_SHOW_SIZE : JPEG_TEST_DEF_HEIGHT;
  uint32_t pixfmt = V4L2_PIX_FMT_YUV420;
  int quality = JPEG_TEST_DEF_QUALITY;
  int nframes = JPEG_TEST_DEF_FRAMES;
  bool bswap = false;
  bool display = true;
  int i;

  if (argc < 2)
    {
      jpeg_test_usage();
      return EXIT_FAILURE;
    }

  for (i = 2; i + 1 < argc; i += 2)
    {
      unsigned long value = strtoul(argv[i + 1], NULL, 0);

      if (strcmp(argv[i], "-w") == 0)
        {
          width = (uint32_t)value;
        }
      else if (strcmp(argv[i], "-h") == 0)
        {
          height = (uint32_t)value;
        }
      else if (strcmp(argv[i], "-q") == 0)
        {
          quality = (int)value;
        }
      else if (strcmp(argv[i], "-n") == 0)
        {
          nframes = (int)value;
        }
      else if (strcmp(argv[i], "-s") == 0)
        {
          bswap = value != 0;
        }
      else if (strcmp(argv[i], "-d") == 0)
        {
          display = value != 0;
        }
      else if (strcmp(argv[i], "-f") == 0)
        {
          if (strcmp(argv[i + 1], "uyvy") == 0)
            {
              pixfmt = V4L2_PIX_FMT_UYVY;
            }
          else if (strcmp(argv[i + 1], "vyuy") == 0)
            {
              pixfmt = V4L2_PIX_FMT_VYUY;
            }
          else if (strcmp(argv[i + 1], "i420") == 0)
            {
              pixfmt = V4L2_PIX_FMT_YUV420;
            }
          else
            {
              printf("jpeg_test: unknown format %s\n", argv[i + 1]);
              return EXIT_FAILURE;
            }
        }
      else
        {
          jpeg_test_usage();
          return EXIT_FAILURE;
        }
    }

  if (show)
    {
      return jpeg_test_show(width, height, quality, pixfmt, bswap) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "cam") == 0)
    {
      return jpeg_test_cam(quality, display, bswap) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "enc") == 0)
    {
      return jpeg_test_encode(width, height, quality, pixfmt, nframes,
                              NULL) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "dump") == 0)
    {
      return jpeg_test_dump() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "info") == 0)
    {
      return jpeg_test_info() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  jpeg_test_usage();
  return EXIT_FAILURE;
}
