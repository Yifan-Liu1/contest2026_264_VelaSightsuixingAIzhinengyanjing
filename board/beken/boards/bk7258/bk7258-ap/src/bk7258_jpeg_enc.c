/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-ap/src/bk7258_jpeg_enc.c
 *
 * V4L2 M2M JPEG encoder for the BK7258 AP core: raw YUV in, JPEG out.
 *
 * Why this is a *software* encoder
 * -------------------------------
 * BK7258 does have a hardware JPEG encoder, but it cannot be used here.  Its
 * configuration (bk_avdk_smp/ap/include/driver/hal/hal_jpeg_types.h,
 * jpeg_config_t) is entirely sensor timing -- vsync/hsync polarity, mclk,
 * sensor_fmt -- and its rx_buf field is where encoded JPEG is *received*,
 * not where pixels come from.  The API confirms it: bk_jpeg_enc_start()
 * takes a yuv_mode_t, not a source address, and bk_jpeg_enc_get_fifo_addr()
 * hands back an output FIFO.  The block is fed by the DVP camera interface
 * and never reads from memory, so it cannot serve a memory-to-memory codec.
 *
 * That hardware path is still the right answer for "camera straight to JPEG"
 * and belongs in the capture driver (/dev/video0).  This driver covers the
 * other case the M2M framework exists for: a YUV frame that is already in
 * memory, wherever it came from.
 *
 * Encoding uses libjpeg-turbo's classic API with jpeg_write_raw_data(),
 * which takes already-subsampled planar input and skips colour conversion
 * entirely.  The simpler TurboJPEG wrapper (tjCompressFromYUV) is not
 * available: the library's build file globs only j*.c, so turbojpeg.c is
 * never compiled, and external/ is an upstream repository this project does
 * not modify.
 *
 * Buffers come from PSRAM, not the main heap.  A 640x480 I420 frame is
 * 460 KB while the AP's malloc heap has roughly 290 KB free, so the
 * framework's default kumm_memalign() would fail for any useful resolution.
 * The alloc_buf and free_buf hooks redirect allocation to the PSRAM heap
 * (2.8 MB free) while keeping the framework's zero-copy buffer flow
 * unchanged.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>

#include <nuttx/kmalloc.h>
#include <nuttx/video/v4l2_m2m.h>

/* v4l2_m2m.h leaves NuttX's EXTERN defined and libjpeg's jmorecfg.h defines
 * its own; dropping it here keeps the two from colliding without touching
 * either header.
 */

#undef EXTERN
#include <jpeglib.h>

#include "bk7258_jpeg_enc.h"
#include "bk7258_camera_imgdata.h"
#include "bk7258_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_BK7258_JPEG_ENC_DEV_PATH
#  define CONFIG_BK7258_JPEG_ENC_DEV_PATH "/dev/video1"
#endif

#ifndef CONFIG_BK7258_JPEG_ENC_QUALITY
#  define CONFIG_BK7258_JPEG_ENC_QUALITY 80
#endif

#define BK7258_JPEG_DEF_WIDTH   320
#define BK7258_JPEG_DEF_HEIGHT  240

/* Bounds come from the 8x8 DCT block: dimensions are rounded up to whole
 * MCUs, and a frame smaller than one MCU has nothing to encode.
 */

/* Encoding runs on its own thread, below the CP heartbeat.
 *
 * The heartbeat is a kthread at priority 100 (bk7258_pm_pwc.c) and CP resets
 * the board if it goes unanswered for 2 s.  A 320x240 frame measures about
 * 766 ms of uninterruptible libjpeg work, so anything at or above 100
 * starves it: on HPWORK (priority 224) the third consecutive frame tripped
 * "IPC[1]heartbeat timeout" and an assert at mb_ipc_task:297.
 */

/* Largest buffer pool taken from the main heap.
 *
 * PSRAM is only needed because a 640x480 frame does not fit in the AP's
 * ~290 KB malloc heap.  Smaller pools do fit, and the main heap is both
 * faster and not shared with the display and encode pools, so prefer it
 * whenever the request plus a margin leaves the heap with room to work.
 */

#define BK7258_JPEG_SRAM_MAX       (128 * 1024)
#define BK7258_JPEG_SRAM_MARGIN    (64 * 1024)

/* Large pools come from psram-encode.
 *
 * PSRAM is divided into named pools (bk7258_psram.c).  psram-encode exists
 * for exactly this and holds 1.4 MB; the general heap it used to take from
 * is shared with the 2 MB ramdisk, and capture buffers come out of
 * psram-display.  Keeping each subsystem in its own pool means the camera
 * and the encoder cannot starve each other.
 */

#define BK7258_JPEG_POOL           BK7258_PSRAM_POOL_ENCODE

#define BK7258_JPEG_THREAD_PRIO    80
#define BK7258_JPEG_THREAD_STACK   8192

#define BK7258_JPEG_MIN_WIDTH   16
#define BK7258_JPEG_MIN_HEIGHT  16
#define BK7258_JPEG_MAX_WIDTH   1280
#define BK7258_JPEG_MAX_HEIGHT  720

/* Worst-case encoded size.  JPEG normally lands far below one byte per
 * pixel, but a pathological (noise) frame at high quality can exceed it, and
 * overrunning the destination is not recoverable -- libjpeg's memory
 * destination would try to realloc a buffer it does not own.  One byte per
 * pixel plus a floor for the headers of tiny frames is the compromise.
 */

#define BK7258_JPEG_HDR_SLACK   4096

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One planar frame handed to libjpeg, plus how it is subsampled.
 *
 * I420 arrives as 4:2:0 and can be described in place with no copying.  UYVY
 * is 4:2:2 and interleaved, so it is de-interleaved into scratch planes and
 * kept at 4:2:2 -- averaging the chroma down to 4:2:0 would throw away
 * resolution the source actually has.
 */

struct bk7258_jpeg_planes_s
{
  uint8_t *plane[3];
  int      stride[3];
  int      h_samp;              /* luma horizontal sampling factor */
  int      v_samp;              /* luma vertical sampling factor   */
};

struct bk7258_jpeg_enc_s
{
  void                 *cookie;

  /* Where the framework's buffer pools came from, so free_buf returns them
   * to the same allocator.  Two pools at most: OUTPUT and CAPTURE.
   */

  void                 *pool[2];
  size_t                pool_size[2];

  /* Encoder thread and its wakeup.  The semaphore is posted by the framework
   * callbacks (which run on the caller's thread) and waited on here, so no
   * encoding ever happens on a caller's stack.
   */

  pthread_t             thread;
  sem_t                 wake;
  bool                  thread_running;
  bool                  stop;

  struct v4l2_format    output_fmt;   /* raw YUV coming in    */
  struct v4l2_format    capture_fmt;  /* JPEG going out       */

  int                   quality;      /* 1..100               */
  bool                  output_on;
  bool                  capture_on;
  bool                  flushing;

  /* Scratch planes for interleaved input, allocated on first use and sized
   * for the current format.  I420 never needs them.
   */

  uint8_t              *scratch;
  size_t                scratch_size;

  /* Statistics, cheap to keep and the only way to see what happened on a
   * board with no filesystem.
   */

  uint32_t              frames;
  uint32_t              errors;
  uint64_t              bytes_in;
  uint64_t              bytes_out;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_jpeg_open(void *cookie, void **priv);
static int bk7258_jpeg_close(void *priv);
static int bk7258_jpeg_capture_streamon(void *priv);
static int bk7258_jpeg_output_streamon(void *priv);
static int bk7258_jpeg_capture_streamoff(void *priv);
static int bk7258_jpeg_output_streamoff(void *priv);
static int bk7258_jpeg_capture_available(void *priv);
static int bk7258_jpeg_output_available(void *priv);
static int bk7258_jpeg_querycap(void *priv,
                                struct v4l2_capability *cap);
static int bk7258_jpeg_capture_enum_fmt(void *priv,
                                        struct v4l2_fmtdesc *fmt);
static int bk7258_jpeg_output_enum_fmt(void *priv,
                                       struct v4l2_fmtdesc *fmt);
static int bk7258_jpeg_capture_g_fmt(void *priv,
                                     struct v4l2_format *fmt);
static int bk7258_jpeg_output_g_fmt(void *priv,
                                    struct v4l2_format *fmt);
static int bk7258_jpeg_capture_s_fmt(void *priv,
                                     struct v4l2_format *fmt);
static int bk7258_jpeg_output_s_fmt(void *priv,
                                    struct v4l2_format *fmt);
static int bk7258_jpeg_capture_try_fmt(void *priv,
                                       struct v4l2_format *fmt);
static int bk7258_jpeg_output_try_fmt(void *priv,
                                      struct v4l2_format *fmt);
static size_t bk7258_jpeg_capture_g_bufsize(void *priv);
static size_t bk7258_jpeg_output_g_bufsize(void *priv);
static int bk7258_jpeg_g_ext_ctrls(void *priv,
                                   struct v4l2_ext_controls *ctrls);
static int bk7258_jpeg_s_ext_ctrls(void *priv,
                                   struct v4l2_ext_controls *ctrls);
static int bk7258_jpeg_subscribe_event(void *priv,
                                       struct v4l2_event_subscription *sub);
static void *bk7258_jpeg_alloc_buf(void *priv, size_t size);
static void bk7258_jpeg_free_buf(void *priv, void *addr);
static void bk7258_jpeg_process(struct bk7258_jpeg_enc_s *enc);
static void *bk7258_jpeg_thread(void *arg);
static void bk7258_jpeg_kick(struct bk7258_jpeg_enc_s *enc);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct codec_ops_s g_bk7258_jpeg_ops =
{
  .open              = bk7258_jpeg_open,
  .close             = bk7258_jpeg_close,
  .capture_streamon  = bk7258_jpeg_capture_streamon,
  .output_streamon   = bk7258_jpeg_output_streamon,
  .capture_streamoff = bk7258_jpeg_capture_streamoff,
  .output_streamoff  = bk7258_jpeg_output_streamoff,
  .capture_available = bk7258_jpeg_capture_available,
  .output_available  = bk7258_jpeg_output_available,
  .querycap          = bk7258_jpeg_querycap,
  .capture_enum_fmt  = bk7258_jpeg_capture_enum_fmt,
  .output_enum_fmt   = bk7258_jpeg_output_enum_fmt,
  .capture_g_fmt     = bk7258_jpeg_capture_g_fmt,
  .output_g_fmt      = bk7258_jpeg_output_g_fmt,
  .capture_s_fmt     = bk7258_jpeg_capture_s_fmt,
  .output_s_fmt      = bk7258_jpeg_output_s_fmt,
  .capture_try_fmt   = bk7258_jpeg_capture_try_fmt,
  .output_try_fmt    = bk7258_jpeg_output_try_fmt,
  .capture_g_bufsize = bk7258_jpeg_capture_g_bufsize,
  .output_g_bufsize  = bk7258_jpeg_output_g_bufsize,
  .g_ext_ctrls       = bk7258_jpeg_g_ext_ctrls,
  .s_ext_ctrls       = bk7258_jpeg_s_ext_ctrls,
  .subscribe_event   = bk7258_jpeg_subscribe_event,
  .alloc_buf         = bk7258_jpeg_alloc_buf,
  .free_buf          = bk7258_jpeg_free_buf,
};

static struct codec_s g_bk7258_jpeg_codec =
{
  .ops = &g_bk7258_jpeg_ops,
};

/* Input formats, in the order VIDIOC_ENUM_FMT reports them. */

static const uint32_t g_output_formats[] =
{
  V4L2_PIX_FMT_YUV420,          /* I420, planar 4:2:0 */
  V4L2_PIX_FMT_UYVY,            /* interleaved 4:2:2, textbook Cb Y0 Cr Y1 */
  V4L2_PIX_FMT_VYUY,            /* interleaved 4:2:2, this board's camera */
};

#define BK7258_JPEG_NOUTPUT_FMTS \
  (sizeof(g_output_formats) / sizeof(g_output_formats[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Where each component sits within a 4:2:2 pixel pair.
 *
 * off_y0 and off_y1 are the luma of the pair's even and odd column.  Keeping
 * them separate is what makes the camera's layout expressible: its chroma
 * bytes are in VYUY positions but its two luma samples are stored in reverse
 * column order, so no fourcc names it exactly.
 */

struct bk7258_jpeg_order_s
{
  uint8_t off_cb;
  uint8_t off_cr;
  uint8_t off_y0;
  uint8_t off_y1;
};

/* Textbook UYVY, and what YUV_BUF actually produces.
 *
 * The second entry matches camera_preview's "VYUY-R" and the measurements
 * behind it: the four bus bytes Y0 Cb Y1 Cr arrive reversed as Cr Y1 Cb Y0.
 */

static const struct bk7258_jpeg_order_s g_uyvy_order =
{
  0, 2, 1, 3
};

static const struct bk7258_jpeg_order_s g_vyuy_order =
{
  2, 0, 3, 1
};

/****************************************************************************
 * Name: bk7258_jpeg_raw_size
 *
 * Description:
 *   Bytes occupied by one raw frame of the given format.
 *
 ****************************************************************************/

static size_t bk7258_jpeg_raw_size(uint32_t pixelformat,
                                   uint32_t width, uint32_t height)
{
  switch (pixelformat)
    {
      case V4L2_PIX_FMT_YUV420:
        return (size_t)width * height * 3 / 2;

      case V4L2_PIX_FMT_UYVY:
      case V4L2_PIX_FMT_VYUY:
        return (size_t)width * height * 2;

      default:
        return 0;
    }
}

/****************************************************************************
 * Name: bk7258_jpeg_sw_scratch_size
 *
 * Description:
 *   Scratch needed to de-interleave one frame into planes.  Zero for formats
 *   that are already planar.
 *
 ****************************************************************************/

size_t bk7258_jpeg_sw_scratch_size(uint32_t pixelformat,
                                   uint32_t width, uint32_t height)
{
  if (pixelformat == V4L2_PIX_FMT_UYVY ||
      pixelformat == V4L2_PIX_FMT_VYUY)
    {
      /* 4:2:2 planar: full luma plus two half-width chroma planes. */

      return (size_t)width * height * 2;
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_describe_i420
 *
 * Description:
 *   Point the plane descriptor straight at an I420 source.  No copy: the
 *   three planes are already contiguous in the order libjpeg wants.
 *
 ****************************************************************************/

static void bk7258_jpeg_describe_i420(struct bk7258_jpeg_planes_s *planes,
                                      uint8_t *src,
                                      uint32_t width, uint32_t height)
{
  size_t ysize = (size_t)width * height;
  size_t csize = ysize / 4;

  planes->plane[0]  = src;
  planes->plane[1]  = src + ysize;
  planes->plane[2]  = src + ysize + csize;
  planes->stride[0] = width;
  planes->stride[1] = width / 2;
  planes->stride[2] = width / 2;
  planes->h_samp    = 2;
  planes->v_samp    = 2;
}

/****************************************************************************
 * Name: bk7258_jpeg_deinterleave
 *
 * Description:
 *   Split an interleaved 4:2:2 frame into Y, Cb and Cr planes, staying at
 *   4:2:2.  One chroma sample per pixel pair per plane and two luma samples.
 *
 *   Vertical resolution is left alone -- dropping to 4:2:0 here would
 *   discard chroma rows the source really has.
 *
 *   The layout comes in as a table so the camera's reversed byte order costs
 *   no second copy of this loop.
 *
 ****************************************************************************/

static void bk7258_jpeg_deinterleave(
                                struct bk7258_jpeg_planes_s *planes,
                                const struct bk7258_jpeg_order_s *order,
                                const uint8_t *src, uint8_t *scratch,
                                uint32_t width, uint32_t height)
{
  size_t ysize = (size_t)width * height;
  size_t csize = ysize / 2;
  uint8_t *y = scratch;
  uint8_t *cb = scratch + ysize;
  uint8_t *cr = scratch + ysize + csize;
  uint32_t row;
  uint32_t col;

  for (row = 0; row < height; row++)
    {
      const uint8_t *s = src + (size_t)row * width * 2;
      uint8_t *yd = y + (size_t)row * width;
      uint8_t *ud = cb + (size_t)row * (width / 2);
      uint8_t *vd = cr + (size_t)row * (width / 2);

      for (col = 0; col + 1 < width; col += 2)
        {
          *ud++ = s[order->off_cb];
          *vd++ = s[order->off_cr];
          *yd++ = s[order->off_y0];
          *yd++ = s[order->off_y1];
          s += 4;
        }
    }

  planes->plane[0]  = y;
  planes->plane[1]  = cb;
  planes->plane[2]  = cr;
  planes->stride[0] = width;
  planes->stride[1] = width / 2;
  planes->stride[2] = width / 2;
  planes->h_samp    = 2;
  planes->v_samp    = 1;
}

/****************************************************************************
 * Name: bk7258_jpeg_sw_encode
 *
 * Description:
 *   Encode one frame.  Returns the encoded length, or a negated errno.
 *
 *   Public, and deliberately free of this driver's device context, because
 *   the capture path needs the same encoder: the hardware JPEG block on
 *   /dev/video0 mis-assembles its bitstream (see the capture driver), so
 *   camera-to-JPEG is served by capturing UYVY and encoding it here.  Both
 *   users pass their own geometry, quality and de-interleave scratch.
 *
 *   Runs for a long time by driver standards -- 252 to 286ms for 640x480 at
 *   quality 80, measured -- so callers must be in a thread, never in an
 *   interrupt handler.
 *
 ****************************************************************************/

int bk7258_jpeg_sw_encode(FAR const struct bk7258_jpeg_sw_req_s *req)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  struct bk7258_jpeg_planes_s planes;
  JSAMPROW yrows[2 * DCTSIZE];
  JSAMPROW crows[2 * DCTSIZE];
  JSAMPROW vrows[2 * DCTSIZE];
  JSAMPARRAY data[3];
  FAR uint8_t *src = req->src;
  size_t srclen = req->srclen;
  FAR uint8_t *dst = req->dst;
  size_t dstlen = req->dstlen;
  uint32_t width = req->width;
  uint32_t height = req->height;
  uint32_t format = req->pixelformat;
  unsigned long outlen;
  uint8_t *outptr = dst;
  int mcu_rows;
  int i;

  if (srclen < bk7258_jpeg_raw_size(format, width, height))
    {
      return -EINVAL;
    }

  switch (format)
    {
      case V4L2_PIX_FMT_YUV420:
        bk7258_jpeg_describe_i420(&planes, src, width, height);
        break;

      case V4L2_PIX_FMT_UYVY:
      case V4L2_PIX_FMT_VYUY:
        if (req->scratch == NULL ||
            req->scratch_size < bk7258_jpeg_sw_scratch_size(format, width,
                                                         height))
          {
            return -ENOMEM;
          }

        bk7258_jpeg_deinterleave(&planes,
                                 format == V4L2_PIX_FMT_UYVY ?
                                 &g_uyvy_order : &g_vyuy_order,
                                 src, req->scratch, width, height);
        break;

      default:
        return -EINVAL;
    }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  /* Fixed destination: outptr/outlen are pre-allocated by the framework, and
   * passing a non-NULL buffer keeps libjpeg from trying to grow it.  A frame
   * that would not fit trips the error handler rather than reallocating.
   */

  outlen = dstlen;
  jpeg_mem_dest(&cinfo, &outptr, &outlen);

  cinfo.image_width      = width;
  cinfo.image_height     = height;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_YCbCr;

  jpeg_set_defaults(&cinfo);
  jpeg_set_colorspace(&cinfo, JCS_YCbCr);

  cinfo.comp_info[0].h_samp_factor = planes.h_samp;
  cinfo.comp_info[0].v_samp_factor = planes.v_samp;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;

  cinfo.raw_data_in = TRUE;
  jpeg_set_quality(&cinfo, req->quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  /* One pass feeds a whole MCU row: DCTSIZE scanlines per vertical sampling
   * step, so 16 luma rows at 4:2:0 and 8 at 4:2:2.
   */

  mcu_rows = DCTSIZE * planes.v_samp;

  data[0] = yrows;
  data[1] = crows;
  data[2] = vrows;

  while (cinfo.next_scanline < cinfo.image_height)
    {
      uint32_t yline = cinfo.next_scanline;
      uint32_t cline = yline / planes.v_samp;
      uint32_t cheight = height / planes.v_samp;

      /* Rows past the bottom edge repeat the last one.  libjpeg always reads
       * a full MCU row even when the image does not divide evenly, and
       * clamping is cheaper than padding the source.
       */

      for (i = 0; i < mcu_rows; i++)
        {
          uint32_t r = yline + i;

          if (r >= height)
            {
              r = height - 1;
            }

          yrows[i] = planes.plane[0] + (size_t)r * planes.stride[0];
        }

      for (i = 0; i < DCTSIZE; i++)
        {
          uint32_t r = cline + i;

          if (r >= cheight)
            {
              r = cheight - 1;
            }

          crows[i] = planes.plane[1] + (size_t)r * planes.stride[1];
          vrows[i] = planes.plane[2] + (size_t)r * planes.stride[2];
        }

      if (jpeg_write_raw_data(&cinfo, data, mcu_rows) == 0)
        {
          jpeg_destroy_compress(&cinfo);
          return -EIO;
        }

      if (cinfo.next_scanline == yline)
        {
          /* libjpeg accepted the rows without consuming any.  Continuing
           * would spin this loop forever, so fail the frame instead.
           */

          jpeg_destroy_compress(&cinfo);
          return -EIO;
        }
    }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  /* jpeg_mem_dest() would have replaced outptr if it had grown the buffer;
   * it must still point at the caller's memory or the encode silently went
   * somewhere the framework will not read.
   */

  if (outptr != dst)
    {
      return -ENOSPC;
    }

  return (int)outlen;
}

/****************************************************************************
 * Name: bk7258_jpeg_open
 ****************************************************************************/

static int bk7258_jpeg_open(void *cookie, void **priv)
{
  struct bk7258_jpeg_enc_s *enc;

  enc = kmm_zalloc(sizeof(struct bk7258_jpeg_enc_s));
  if (enc == NULL)
    {
      return -ENOMEM;
    }

  enc->cookie  = cookie;
  enc->quality = CONFIG_BK7258_JPEG_ENC_QUALITY;

  if (sem_init(&enc->wake, 0, 0) < 0)
    {
      kmm_free(enc);
      return -errno;
    }

  enc->output_fmt.type                 = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  enc->output_fmt.fmt.pix.width        = BK7258_JPEG_DEF_WIDTH;
  enc->output_fmt.fmt.pix.height       = BK7258_JPEG_DEF_HEIGHT;
  enc->output_fmt.fmt.pix.pixelformat  = V4L2_PIX_FMT_YUV420;

  enc->capture_fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  enc->capture_fmt.fmt.pix.width       = BK7258_JPEG_DEF_WIDTH;
  enc->capture_fmt.fmt.pix.height      = BK7258_JPEG_DEF_HEIGHT;
  enc->capture_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;

  *priv = enc;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_close
 ****************************************************************************/

static int bk7258_jpeg_close(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  /* Stop the thread before releasing anything it touches. */

  if (enc->thread_running)
    {
      enc->stop = true;
      sem_post(&enc->wake);
      pthread_join(enc->thread, NULL);
      enc->thread_running = false;
    }

  sem_destroy(&enc->wake);

  if (enc->scratch != NULL)
    {
      bk7258_psram_free(enc->scratch);
      enc->scratch = NULL;
    }

  kmm_free(enc);
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_streamon
 *
 * Description:
 *   The input format is final by now, so this is where the de-interleave
 *   scratch is sized and allocated -- deferring it keeps a driver that is
 *   merely registered from holding PSRAM it may never use.
 *
 ****************************************************************************/

static int bk7258_jpeg_output_streamon(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  size_t need;

  need = bk7258_jpeg_sw_scratch_size(enc->output_fmt.fmt.pix.pixelformat,
                                  enc->output_fmt.fmt.pix.width,
                                  enc->output_fmt.fmt.pix.height);

  if (need > enc->scratch_size)
    {
      if (enc->scratch != NULL)
        {
          bk7258_psram_free(enc->scratch);
        }

      enc->scratch = bk7258_psram_malloc(need);
      if (enc->scratch == NULL)
        {
          enc->scratch_size = 0;
          return -ENOMEM;
        }

      enc->scratch_size = need;
    }

  enc->output_on = true;
  enc->flushing  = false;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_streamon
 ****************************************************************************/

static int bk7258_jpeg_capture_streamon(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  enc->capture_on = true;

  /* Start the encoder thread here rather than at open(): by now the formats
   * are settled and there is actually work coming.
   */

  if (!enc->thread_running)
    {
      struct sched_param param;
      pthread_attr_t attr;
      int ret;

      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, BK7258_JPEG_THREAD_STACK);
      param.sched_priority = BK7258_JPEG_THREAD_PRIO;
      pthread_attr_setschedparam(&attr, &param);

      /* Without this the priority above is ignored: NuttX pthreads default
       * to PTHREAD_INHERIT_SCHED and would run at the caller's priority,
       * which is the application's -- the same band as the heartbeat.
       */

      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

      enc->stop = false;
      ret = pthread_create(&enc->thread, &attr, bk7258_jpeg_thread, enc);
      pthread_attr_destroy(&attr);

      if (ret != 0)
        {
          printf("bk7258_jpeg: cannot start encoder thread: %d\n", ret);
          return -ret;
        }

      enc->thread_running = true;
    }

  /* Buffers are queued by now, so anything already waiting can be encoded. */

  bk7258_jpeg_kick(enc);
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_streamoff
 *
 * Description:
 *   Stop taking new input and drain.  JPEG holds no frames back, so the only
 *   thing left is to mark the stream ended so the next pass can emit EOS.
 *
 ****************************************************************************/

static int bk7258_jpeg_output_streamoff(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  enc->output_on = false;
  enc->flushing  = true;

  bk7258_jpeg_kick(enc);
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_streamoff
 ****************************************************************************/

static int bk7258_jpeg_capture_streamoff(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  enc->capture_on = false;
  enc->flushing   = false;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_available
 ****************************************************************************/

static int bk7258_jpeg_output_available(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  bk7258_jpeg_kick(enc);
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_available
 ****************************************************************************/

static int bk7258_jpeg_capture_available(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  bk7258_jpeg_kick(enc);
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_querycap
 ****************************************************************************/

static int bk7258_jpeg_querycap(void *priv, struct v4l2_capability *cap)
{
  memset(cap, 0, sizeof(*cap));
  strlcpy((char *)cap->driver, "bk7258-jpeg", sizeof(cap->driver));
  strlcpy((char *)cap->card, "BK7258 JPEG encoder", sizeof(cap->card));
  cap->capabilities = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_enum_fmt
 ****************************************************************************/

static int bk7258_jpeg_capture_enum_fmt(void *priv,
                                        struct v4l2_fmtdesc *fmt)
{
  if (fmt->index > 0)
    {
      return -EINVAL;
    }

  fmt->pixelformat = V4L2_PIX_FMT_JPEG;
  fmt->flags       = V4L2_FMT_FLAG_COMPRESSED;
  strlcpy((char *)fmt->description, "JPEG", sizeof(fmt->description));
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_enum_fmt
 ****************************************************************************/

static int bk7258_jpeg_output_enum_fmt(void *priv,
                                       struct v4l2_fmtdesc *fmt)
{
  if (fmt->index >= BK7258_JPEG_NOUTPUT_FMTS)
    {
      return -EINVAL;
    }

  fmt->pixelformat = g_output_formats[fmt->index];
  fmt->flags       = 0;

  /* Spell the byte order out: VYUY here means what the camera produces, and
   * the description is the only place ENUM_FMT can say so.
   */

  switch (fmt->pixelformat)
    {
      case V4L2_PIX_FMT_YUV420:
        strlcpy((char *)fmt->description, "YUV 4:2:0 (I420)",
                sizeof(fmt->description));
        break;

      case V4L2_PIX_FMT_VYUY:
        strlcpy((char *)fmt->description, "4:2:2 Cr Y1 Cb Y0 (camera)",
                sizeof(fmt->description));
        break;

      default:
        strlcpy((char *)fmt->description, "4:2:2 Cb Y0 Cr Y1 (UYVY)",
                sizeof(fmt->description));
        break;
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_try_fmt
 ****************************************************************************/

static int bk7258_jpeg_output_try_fmt(void *priv, struct v4l2_format *fmt)
{
  uint32_t width = fmt->fmt.pix.width;
  uint32_t height = fmt->fmt.pix.height;
  uint32_t format = fmt->fmt.pix.pixelformat;
  unsigned int i;
  bool found = false;

  for (i = 0; i < BK7258_JPEG_NOUTPUT_FMTS; i++)
    {
      if (g_output_formats[i] == format)
        {
          found = true;
          break;
        }
    }

  if (!found)
    {
      format = V4L2_PIX_FMT_YUV420;
    }

  /* Both supported formats subsample chroma horizontally, and 4:2:0 does so
   * vertically too, so even dimensions are a hard requirement rather than a
   * preference.
   */

  width  &= ~1u;
  height &= ~1u;

  if (width < BK7258_JPEG_MIN_WIDTH)
    {
      width = BK7258_JPEG_MIN_WIDTH;
    }
  else if (width > BK7258_JPEG_MAX_WIDTH)
    {
      width = BK7258_JPEG_MAX_WIDTH;
    }

  if (height < BK7258_JPEG_MIN_HEIGHT)
    {
      height = BK7258_JPEG_MIN_HEIGHT;
    }
  else if (height > BK7258_JPEG_MAX_HEIGHT)
    {
      height = BK7258_JPEG_MAX_HEIGHT;
    }

  fmt->fmt.pix.width        = width;
  fmt->fmt.pix.height       = height;
  fmt->fmt.pix.pixelformat  = format;
  fmt->fmt.pix.bytesperline = format == V4L2_PIX_FMT_UYVY ?
                              width * 2 : width;
  fmt->fmt.pix.sizeimage    = bk7258_jpeg_raw_size(format, width, height);
  fmt->fmt.pix.field        = V4L2_FIELD_NONE;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_try_fmt
 ****************************************************************************/

static int bk7258_jpeg_capture_try_fmt(void *priv, struct v4l2_format *fmt)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  /* The encoded frame always has the geometry of the raw one; only the
   * OUTPUT side negotiates size.
   */

  fmt->fmt.pix.width        = enc->output_fmt.fmt.pix.width;
  fmt->fmt.pix.height       = enc->output_fmt.fmt.pix.height;
  fmt->fmt.pix.pixelformat  = V4L2_PIX_FMT_JPEG;
  fmt->fmt.pix.bytesperline = 0;
  fmt->fmt.pix.sizeimage    = bk7258_jpeg_capture_g_bufsize(priv);
  fmt->fmt.pix.field        = V4L2_FIELD_NONE;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_s_fmt
 ****************************************************************************/

static int bk7258_jpeg_output_s_fmt(void *priv, struct v4l2_format *fmt)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  int ret;

  ret = bk7258_jpeg_output_try_fmt(priv, fmt);
  if (ret < 0)
    {
      return ret;
    }

  enc->output_fmt = *fmt;

  /* Keep the encoded side's geometry in step: an application that sets only
   * the input format still gets a consistent answer from CAPTURE G_FMT.
   */

  enc->capture_fmt.fmt.pix.width  = fmt->fmt.pix.width;
  enc->capture_fmt.fmt.pix.height = fmt->fmt.pix.height;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_s_fmt
 ****************************************************************************/

static int bk7258_jpeg_capture_s_fmt(void *priv, struct v4l2_format *fmt)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  int ret;

  ret = bk7258_jpeg_capture_try_fmt(priv, fmt);
  if (ret < 0)
    {
      return ret;
    }

  enc->capture_fmt = *fmt;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_g_fmt
 ****************************************************************************/

static int bk7258_jpeg_output_g_fmt(void *priv, struct v4l2_format *fmt)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  *fmt = enc->output_fmt;
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_g_fmt
 ****************************************************************************/

static int bk7258_jpeg_capture_g_fmt(void *priv, struct v4l2_format *fmt)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  *fmt = enc->capture_fmt;
  fmt->fmt.pix.sizeimage = bk7258_jpeg_capture_g_bufsize(priv);
  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_output_g_bufsize
 ****************************************************************************/

static size_t bk7258_jpeg_output_g_bufsize(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;

  return bk7258_jpeg_raw_size(enc->output_fmt.fmt.pix.pixelformat,
                              enc->output_fmt.fmt.pix.width,
                              enc->output_fmt.fmt.pix.height);
}

/****************************************************************************
 * Name: bk7258_jpeg_capture_g_bufsize
 ****************************************************************************/

static size_t bk7258_jpeg_capture_g_bufsize(void *priv)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  uint32_t width = enc->output_fmt.fmt.pix.width;
  uint32_t height = enc->output_fmt.fmt.pix.height;

  return (size_t)width * height + BK7258_JPEG_HDR_SLACK;
}

/****************************************************************************
 * Name: bk7258_jpeg_g_ext_ctrls
 ****************************************************************************/

static int bk7258_jpeg_g_ext_ctrls(void *priv,
                                  struct v4l2_ext_controls *ctrls)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  int i;

  for (i = 0; i < ctrls->count; i++)
    {
      switch (ctrls->controls[i].id)
        {
          case V4L2_CID_JPEG_COMPRESSION_QUALITY:
            ctrls->controls[i].value = enc->quality;
            break;

          default:
            ctrls->error_idx = i;
            return -EINVAL;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_s_ext_ctrls
 ****************************************************************************/

static int bk7258_jpeg_s_ext_ctrls(void *priv,
                                  struct v4l2_ext_controls *ctrls)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  int i;

  for (i = 0; i < ctrls->count; i++)
    {
      switch (ctrls->controls[i].id)
        {
          case V4L2_CID_JPEG_COMPRESSION_QUALITY:
            {
              int q = ctrls->controls[i].value;

              if (q < 1 || q > 100)
                {
                  ctrls->error_idx = i;
                  return -ERANGE;
                }

              enc->quality = q;
            }
            break;

          default:
            ctrls->error_idx = i;
            return -EINVAL;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_subscribe_event
 ****************************************************************************/

static int bk7258_jpeg_subscribe_event(void *priv,
                                       struct v4l2_event_subscription *sub)
{
  return sub->type == V4L2_EVENT_EOS ? 0 : -EINVAL;
}

/****************************************************************************
 * Name: bk7258_jpeg_alloc_buf
 *
 * Description:
 *   Buffers come from PSRAM.  The framework would otherwise use
 *   kumm_memalign() on the main heap, which has around 290 KB free -- less
 *   than a single 640x480 I420 frame.
 *
 ****************************************************************************/

static void *bk7258_jpeg_alloc_buf(void *priv, size_t size)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  struct mallinfo mem;
  bool use_psram = true;
  void *addr = NULL;
  int slot;

  /* Same 32-byte alignment the framework's own allocation uses. */

  mem = mallinfo();

  if (size <= BK7258_JPEG_SRAM_MAX &&
      mem.fordblks > (int)(size + BK7258_JPEG_SRAM_MARGIN))
    {
      addr = kumm_memalign(32, size);
      if (addr != NULL)
        {
          use_psram = false;
        }
    }

  if (addr == NULL)
    {
      if (!bk7258_psram_is_online())
        {
          return NULL;
        }

      /* Only ever this pool, so free_buf has one answer.  A frame too big
       * for it fails here, REQBUFS reports ENOMEM, and the caller retries
       * with one buffer per queue.
       */

      addr = bk7258_media_pool_alloc(BK7258_JPEG_POOL, 32, size);

      if (addr == NULL)
        {
          return NULL;
        }
    }

  /* Remember the origin.  Called from REQBUFS, outside streaming, so saying
   * so on the console costs nothing here.
   */

  for (slot = 0; slot < 2; slot++)
    {
      if (enc->pool[slot] == NULL)
        {
          enc->pool[slot] = addr;
          enc->pool_size[slot] = size;
          break;
        }
    }

  printf("bk7258_jpeg: %zu byte pool from %s\n",
         size, use_psram ? "psram-encode" : "main heap");

  return addr;
}

/****************************************************************************
 * Name: bk7258_jpeg_free_buf
 ****************************************************************************/

static void bk7258_jpeg_free_buf(void *priv, void *addr)
{
  struct bk7258_jpeg_enc_s *enc = priv;
  int slot;

  if (addr == NULL)
    {
      return;
    }

  /* Which allocator owns this is a property of the address, so ask rather
   * than remember.  Bookkeeping that can drift out of step is how this
   * leaked in the first place.
   */

  if (bk7258_psram_contains(addr, 1))
    {
      bk7258_media_pool_free(BK7258_JPEG_POOL, addr);
    }
  else
    {
      kumm_free(addr);
    }

  /* Drop it from the bounds-check table too, so a stale extent cannot make a
   * freed pool look like a valid encode target.
   */

  for (slot = 0; slot < 2; slot++)
    {
      if (enc->pool[slot] == addr)
        {
          enc->pool[slot] = NULL;
          enc->pool_size[slot] = 0;
          break;
        }
    }
}

/****************************************************************************
 * Name: bk7258_jpeg_addr_ok
 *
 * Description:
 *   True if len bytes at addr sit inside a pool this driver allocated.
 *
 *   The framework rebuilds these pointers on every QBUF out of an offset the
 *   application supplies (v4l2_m2m.c:508), so a caller that gets that offset
 *   wrong points the encoder at arbitrary memory.  Writing there takes the
 *   AP down and CP resets the board, which is far too harsh a penalty for a
 *   bad ioctl argument.
 *
 ****************************************************************************/

static bool bk7258_jpeg_addr_ok(struct bk7258_jpeg_enc_s *enc,
                               const void *addr, size_t len)
{
  int slot;

  if (addr == NULL || len == 0)
    {
      return false;
    }

  for (slot = 0; slot < 2; slot++)
    {
      const uint8_t *base = enc->pool[slot];

      if (base == NULL)
        {
          continue;
        }

      if ((const uint8_t *)addr >= base &&
          (const uint8_t *)addr + len <= base + enc->pool_size[slot])
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: bk7258_jpeg_kick
 *
 * Description:
 *   Wake the encoder thread.  Called from framework callbacks, which run on
 *   whichever thread issued the ioctl -- posting a semaphore keeps the
 *   several-hundred-millisecond encode off that thread.
 *
 ****************************************************************************/

static void bk7258_jpeg_kick(struct bk7258_jpeg_enc_s *enc)
{
  if (enc->thread_running)
    {
      sem_post(&enc->wake);
    }
}

/****************************************************************************
 * Name: bk7258_jpeg_thread
 *
 * Description:
 *   Encode until told to stop.  Deliberately below the CP heartbeat thread's
 *   priority: a frame is hundreds of milliseconds of uninterruptible work,
 *   and outranking the heartbeat makes CP reset the board.
 *
 ****************************************************************************/

static void *bk7258_jpeg_thread(void *arg)
{
  struct bk7258_jpeg_enc_s *enc = arg;

  /* Nothing is printed from here on purpose.
   *
   * AP console output travels over the mailbox UART0_TX channel, and while
   * that channel is busy bk7258_mailbox_send_wire() returns -EBUSY, which
   * the heartbeat at bk7258_pm_pwc.c:79 discards without retrying.  Printing
   * from the encode path therefore costs heartbeats, and enough of them
   * makes CP assert and reset the board.  Statistics live in the instance
   * and are reported by whoever asks, outside of streaming.
   */

  while (!enc->stop)
    {
      if (sem_wait(&enc->wake) < 0)
        {
          continue;
        }

      if (enc->stop)
        {
          break;
        }

      bk7258_jpeg_process(enc);
    }

  return NULL;
}

/****************************************************************************
 * Name: bk7258_jpeg_process
 *
 * Description:
 *   Encode whatever is ready.  One input frame produces exactly one output
 *   frame, so unlike a predictive codec there is never a frame queued but
 *   not yet emitted to account for.
 *
 ****************************************************************************/

static void bk7258_jpeg_process(struct bk7258_jpeg_enc_s *enc)
{
  struct v4l2_buffer *src;
  struct v4l2_buffer *dst;
  struct v4l2_event event;
  int ret;

  src = codec_output_get_buf(enc->cookie);
  if (src == NULL)
    {
      /* Nothing to encode.  If the input side has been stopped this is the
       * end of the stream, which the application learns from an empty buffer
       * flagged LAST plus an EOS event.
       */

      if (!enc->flushing)
        {
          return;
        }

      dst = codec_capture_get_buf(enc->cookie);
      if (dst == NULL)
        {
          return;
        }

      enc->flushing    = false;
      dst->bytesused   = 0;
      dst->flags      &= ~(V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_KEYFRAME);
      dst->flags      |= V4L2_BUF_FLAG_LAST;
      codec_capture_put_buf(enc->cookie, dst);

      memset(&event, 0, sizeof(event));
      event.type = V4L2_EVENT_EOS;
      codec_queue_event(enc->cookie, &event);

      return;
    }

  dst = codec_capture_get_buf(enc->cookie);
  if (dst == NULL)
    {
      /* No room for the result.  Leave the input queued; capture_available
       * will run this again once the application returns a buffer.
       */

      return;
    }

  /* Buffers are recycled, so clear what this driver sets before setting it
   * again: otherwise the previous frame's verdict sticks.
   */

  dst->flags &= ~(V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_KEYFRAME |
                  V4L2_BUF_FLAG_LAST);

  if (!bk7258_jpeg_addr_ok(enc, src->m.vaddr, src->bytesused) ||
      !bk7258_jpeg_addr_ok(enc, dst->m.vaddr,
                           bk7258_jpeg_capture_g_bufsize(enc)))
    {
      ret = -EFAULT;
    }
  else
    {
      struct bk7258_jpeg_sw_req_s req;

      req.src          = (FAR uint8_t *)src->m.vaddr;
      req.srclen       = src->bytesused;
      req.dst          = (FAR uint8_t *)dst->m.vaddr;
      req.dstlen       = bk7258_jpeg_capture_g_bufsize(enc);
      req.scratch      = enc->scratch;
      req.scratch_size = enc->scratch_size;
      req.width        = enc->output_fmt.fmt.pix.width;
      req.height       = enc->output_fmt.fmt.pix.height;
      req.pixelformat  = enc->output_fmt.fmt.pix.pixelformat;
      req.quality      = enc->quality;

      ret = bk7258_jpeg_sw_encode(&req);
    }

  if (ret < 0)
    {
      enc->errors++;
      dst->bytesused = 0;
      dst->flags    |= V4L2_BUF_FLAG_ERROR;
    }
  else
    {
      enc->frames++;
      enc->bytes_in  += src->bytesused;
      enc->bytes_out += ret;
      dst->bytesused  = ret;
      dst->flags     |= V4L2_BUF_FLAG_KEYFRAME;
    }

  dst->timestamp = src->timestamp;

  codec_output_put_buf(enc->cookie, src);
  codec_capture_put_buf(enc->cookie, dst);

  /* More may already be queued: post to ourselves so the loop comes round
   * again without recursing.
   */

  sem_post(&enc->wake);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_BK7258_CAMERA_JPEG_SW
/****************************************************************************
 * Name: bk7258_camera_sw_jpeg_encode
 *
 * Description:
 *   The capture driver's software JPEG hook.  Encodes one raw camera frame
 *   (V Y1 U Y0, the sensor's own order) into the application's V4L2 buffer.
 *
 *   The de-interleave scratch is allocated once, on first use, and kept: it
 *   is 614400 bytes for 640x480, which the AP's malloc heap cannot hold.
 *
 *   It comes from the *display* pool, not the encode pool this M2M driver
 *   uses.  The encode pool is 1433600 bytes and the M2M device needs
 *   614400 + 311296 of it for one session; adding a persistent 614400 here
 *   pushed the total over and /dev/video1 started failing REQBUFS with
 *   ENOMEM -- measured as "preview_jpeg: REQBUFS(capture) failed, errno=12"
 *   the first time /dev/video0 had encoded anything.  The display pool is
 *   5.7MB and already holds this pipeline's capture buffers.
 *
 ****************************************************************************/

static FAR uint8_t *g_sw_scratch;
static size_t g_sw_scratch_size;

static int bk7258_camera_sw_jpeg_encode(FAR uint8_t *src, size_t srclen,
                                        FAR uint8_t *dst, size_t dstlen,
                                        uint32_t width, uint32_t height)
{
  struct bk7258_jpeg_sw_req_s req;
  size_t need = bk7258_jpeg_sw_scratch_size(V4L2_PIX_FMT_VYUY,
                                            width, height);

  if (need > g_sw_scratch_size)
    {
      if (g_sw_scratch != NULL)
        {
          bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY, g_sw_scratch);
          g_sw_scratch = NULL;
          g_sw_scratch_size = 0;
        }

      g_sw_scratch = bk7258_media_pool_alloc(BK7258_PSRAM_POOL_DISPLAY,
                                             32, need);
      if (g_sw_scratch == NULL)
        {
          return -ENOMEM;
        }

      g_sw_scratch_size = need;
    }

  req.src          = src;
  req.srclen       = srclen;
  req.dst          = dst;
  req.dstlen       = dstlen;
  req.scratch      = g_sw_scratch;
  req.scratch_size = g_sw_scratch_size;
  req.width        = width;
  req.height       = height;
  req.pixelformat  = V4L2_PIX_FMT_VYUY;
  req.quality      = CONFIG_BK7258_JPEG_ENC_QUALITY;

  return bk7258_jpeg_sw_encode(&req);
}
#endif

/****************************************************************************
 * Name: bk7258_jpeg_enc_initialize
 ****************************************************************************/

int bk7258_jpeg_enc_initialize(void)
{
  int ret;

  ret = codec_register(CONFIG_BK7258_JPEG_ENC_DEV_PATH,
                       &g_bk7258_jpeg_codec);
  if (ret < 0)
    {
      printf("bk7258_jpeg: failed to register %s: %d\n",
             CONFIG_BK7258_JPEG_ENC_DEV_PATH, ret);
      return ret;
    }

  printf("bk7258_jpeg: %s registered (YUV420/UYVY -> JPEG, q=%d, "
         "buffers from PSRAM)\n",
         CONFIG_BK7258_JPEG_ENC_DEV_PATH, CONFIG_BK7258_JPEG_ENC_QUALITY);

  /* Serve /dev/video0's JPEG format from here too.  The hardware JPEG block
   * delivers at 30fps but mis-assembles its bitstream, so a camera-to-JPEG
   * capture comes back as parts of two frames; encoding a UYVY capture is
   * slower and correct.  See bk7258_camera_set_sw_jpeg().
   */

#ifdef CONFIG_BK7258_CAMERA_JPEG_SW
  bk7258_camera_set_sw_jpeg(bk7258_camera_sw_jpeg_encode);
#endif

#ifdef CONFIG_BK7258_CAMERA_JPEG_SW
  printf("bk7258_jpeg: /dev/video0 JPEG served by the software encoder\n");
#else
  printf("bk7258_jpeg: /dev/video0 JPEG served by the hardware encoder\n");
#endif

  return 0;
}
