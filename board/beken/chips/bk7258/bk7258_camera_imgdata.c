/****************************************************************************
 * board/beken/chips/bk7258/bk7258_camera_imgdata.c
 *
 * BK7258 GC2145 camera platform (imgdata) driver: NuttX V4L2
 * imgdata_ops_s implementation for the YUV_BUF whole-frame direct-capture
 * path.  This is the platform data interface half of the driver split
 * described in the openvela Camera Driver Framework guide; sensor-specific
 * I2C register programming lives in the imgsensor half
 * (board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_imgsensor.c).
 *
 * Capture model
 * -------------
 * YUV_BUF writes a complete YUV422 frame straight into the V4L2 frame
 * buffer (which imgdata_ops_s.alloc() takes from PSRAM) and raises the
 * YUV_ARV interrupt when the frame is done.  That is the reference
 * implementation's pure-YUV path: bk_avdk_smp
 * ap/components/bk_dvp/src/bk_dvp.c dvp_camera_yuv_mode() points
 * em_base_addr at the frame buffer, and dvp_camera_yuv_eof_handler()
 * (registered on YUV_BUF_YUV_ARV) hands the finished frame up and
 * re-points em_base_addr at the next one.  There is no line-batch
 * ping-pong buffer and no DMA copy in this path -- the previous revision
 * of this driver reconstructed frames from ~60 SM0_WR/SM1_WR line-done
 * interrupts per frame plus one DMA transfer each, which is what the
 * reference only does for the combined YUV+encode formats.
 *
 * Interrupt-context rules
 * -----------------------
 * v4l2_cap.c's complete_capture() runs in interrupt context and, from
 * there, calls back into IMGDATA_SET_BUF() (to re-arm the next buffer)
 * and IMGDATA_STOP_CAPTURE()/IMGSENSOR_STOP_CAPTURE() (when it runs out
 * of vacant containers).  So set_buf() and stop_capture(), not just the
 * frame callback, must be interrupt-safe: no printf(), no blocking, no
 * allocation.  Diagnostics in those functions are guarded by
 * up_interrupt_context().
 *
 * Capture watchdog
 * ----------------
 * VIDIOC_DQBUF blocks in nxsem_wait_uninterruptible() with no timeout
 * (v4l2_cap.c capture_dqbuf()), so if the hardware never completes a
 * frame the caller's dequeue thread never returns -- which is exactly how
 * `nxcamera stream` ended up unable to process its own `q` command
 * (nxcamera_stop() posts VIDEO_MSG_STOP and then blocks in pthread_join()
 * on that thread).  This driver therefore runs a watchdog while capturing
 * and reports an error frame if no YUV_ARV arrives within
 * BK7258_CAMERA_WATCHDOG_MS: complete_capture() completes the buffer with
 * V4L2_BUF_FLAG_ERROR and posts dqbuf_wait_flg, so DQBUF returns, the
 * application loop regains control and a stop request can be honoured.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>

#include <sys/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/video/imgdata.h>

#include "bk7258_yuv_buf.h"
#include "bk7258_jpeg_enc.h"
#include "bk7258_dma.h"
#include "bk7258_camera_imgdata.h"
#include "bk7258_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Geometry is not fixed here: whatever the imgsensor half can program, this
 * half must be able to receive.  The accepted geometry therefore comes from
 * the format the framework hands down at VIDIOC_S_FMT, and the only limits
 * this driver imposes are the ones the hardware really has:
 *
 *   - YUV_BUF counts in 8x8 blocks (x_pixel = width / 8), so both
 *     dimensions must be multiples of 8.
 *   - the frame must fit in PSRAM, which set_buf() checks against the
 *     buffer the framework actually allocated.
 *
 * The largest mode the sensor offers is 1600x1200, i.e. 3,840,000 bytes a
 * frame; the PSRAM display pool is 0x570000 (5,701,632) bytes, so that mode
 * admits a single buffer while 1280x720 (1,843,200) admits three.  Which of
 * those the application asks for is its business -- REQBUFS fails cleanly
 * through imgdata_ops_s.alloc() when the pool cannot satisfy it.
 */

#define BK7258_CAMERA_ALIGN       8u

/* JPEG capture.
 *
 * The encoded bitstream has no memory-mapped output on this SoC: it leaves
 * the JPEG block through a FIFO register, so a DMA channel has to drain it
 * into the V4L2 buffer (see bk7258_jpeg_enc.h).  The channel runs in REPEAT
 * mode with a looping destination, so one configuration absorbs a whole
 * frame regardless of how big it turns out to be.
 *
 * BK7258_CAMERA_JPEG_CHUNK is the DMA's transfer_len, i.e. how often the
 * finish interrupt fires.  10KB matches the reference's FRAME_BUFFER_CACHE
 * (dvp_private.h); at a typical 30KB frame that is a handful of interrupts
 * per frame, which is cheap enough while still bounding how much of a frame
 * is in flight.
 *
 * The encoder appends 5 CRC bytes after the bitstream (JPEG_CRC_SIZE in
 * hal_jpeg_types.h).  They travel through the FIFO like everything else but
 * are not part of the image, and byte_count_pfrm does not count them --
 * which is why this driver takes byte_count_pfrm as the length and does not
 * subtract anything.
 *
 * BK7258_CAMERA_JPEG_EOI_WINDOW is how far back from that length to look for
 * the FF D9 end-of-image marker.  The reference searches the last 10 bytes
 * and trims to the marker; the same check doubles as proof that the frame
 * arrived whole, so a frame without it is completed as an error rather than
 * handed to the application as a truncated file.
 */

/* Lines of the encoder's input staging area in PSRAM.
 *
 * In JPEG mode YUV_BUF does not write frames; it stages line batches for the
 * encoder to consume, and its frame-buffer register points at that staging
 * area (yuv_buf_hal_set_jpeg_mode_config() writes em_base_addr from
 * config->base_addr, which the reference fills with a buffer of
 * width * 32 * 2 bytes -- bk_camera_dvp_ctlr.c).
 *
 * Pointing that register at the V4L2 buffer instead, as a first attempt did,
 * makes the module write raw pixels straight over the bitstream the DMA is
 * depositing there: the captured file then contains recognisable UYVY data
 * and never starts with FF D8.
 */

#define BK7258_CAMERA_JPEG_STAGE_LINES 32u

#define BK7258_CAMERA_JPEG_DMA_CH     0u
#define BK7258_CAMERA_JPEG_CHUNK      (10u * 1024u)
#define BK7258_CAMERA_JPEG_DRAIN_SPINS 100000u
#define BK7258_CAMERA_JPEG_EOI_BACK   16u
#define BK7258_CAMERA_JPEG_EOI_FWD    1024u

/* Smallest V4L2 buffer this driver will drain an encoded frame into.
 *
 * For a compressed format the buffer size is the application's choice, not a
 * function of the geometry: v4l2_cap.c's get_bufsize() returns the format's
 * sizeimage when the application set one and only falls back to width*height
 * when it did not.  Both are legal, and they differ by a lot -- packages/
 * ai_agent asks for 160KB while width*height at 480x480 is 225KB -- so the
 * driver cannot derive the capacity and has to take what set_buf() is given.
 *
 * Two chunks is the floor.  The drain channel loops over the buffer in
 * transfer_len steps, so a capacity below one step cannot be armed at all,
 * and a capacity of exactly one step leaves no room to tell "wrapped" from
 * "filled".  Anything smaller would also be far below a real frame: the
 * reference sizes its own frame cache at one chunk (dvp_private.h).
 */

#define BK7258_CAMERA_JPEG_MIN_BUF    (BK7258_JPEG_ENC_PAD + \
                                       2u * BK7258_CAMERA_JPEG_CHUNK)

/* YUV_BUF writes frames over the PSRAM bus; a frame buffer outside PSRAM
 * would be silently dropped (or corrupt unrelated memory), so set_buf()
 * rejects one.  PSRAM is mapped at SOC_PSRAM_DATA_BASE, 16MB.
 */

#define BK7258_CAMERA_PSRAM_BASE  0x60000000u
#define BK7258_CAMERA_PSRAM_END   0x61000000u

/* Upper bound on how long VIDIOC_DQBUF may block before this driver
 * reports an error frame instead of letting the caller hang forever.
 * 500ms is ~15 frame periods at 30fps: long enough never to fire during
 * healthy streaming, short enough that a `q` keystroke feels responsive.
 */

#define BK7258_CAMERA_WATCHDOG_MS 500

/* Why the encoder is not started on a frame boundary.
 *
 * Every JPEG session begins with exactly err=3 resets=3 short=1 and then
 * runs clean, no matter how long it is (measured at 3, 13 and 33 frames:
 * the counts did not move).  So the errors are a start-up transient, not a
 * per-frame defect.  The reason looked obvious: the reference arms its
 * capture path before it initialises the sensor (bk_dvp.c dvp_camera_init(),
 * step 5 bk_yuv_buf_start(JPEG_MODE) then step 6 sensor->init()), so its
 * encoder always sees a stream that starts at a frame boundary, whereas this
 * driver leaves the sensor streaming from init (bk7258_camera_imgsensor.c)
 * and therefore enables the encoder at an arbitrary phase -- and a partial
 * frame is exactly what the encoder reports as frame_err (the reference
 * names that handler dvp_camera_sensor_ppi_err_handler, i.e. "the picture is
 * not the size I was told").
 *
 * Waiting for one VSYNC before bk7258_jpeg_enc_start() was tried and does
 * not work, because the premise is circular: in JPEG mode the VSYNC events
 * only start once the encoder is enabled.  yuv_mode is off in this mode
 * (see bk7258_yuv_buf_start_jpeg()), so until the JPEG block runs there is
 * no sync detection to report a boundary.  Measured: the wait timed out in
 * all three sessions, and the only effect was the wait itself -- a 30-frame
 * session went from 29.72 to 25.99 fps (elapsed 1110ms -> 1231ms).
 *
 * The transient is harmless where it matters: it is absorbed before the
 * application sees anything.  A frame without an EOI is completed with an
 * error and never handed up, so the first frame an application dequeues is
 * a whole one -- measured across three sessions, frame 0 came back with
 * bytesused 13079 / 13147 / 13111, SOI and EOI present, flags 0.  That is
 * what matters for packages/ai_agent, whose camera_capture tool takes
 * exactly one frame.
 *
 * Eliminating it for real means giving the sensor a stream on/off that
 * start_capture() can sequence against, which is a change to the imgsensor
 * side and to the already-validated UYVY path.  Not worth it for three
 * absorbed errors.
 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_camera_imgdata_s
{
  struct imgdata_s data;          /* Must be first: base-pointer cast. */
  FAR uint8_t *frame_buf;          /* Set by set_buf(); NULL until then. */
  uint32_t frame_buf_size;
  uint16_t width;                  /* Latched by validate_frame_setting(). */
  uint16_t height;
  uint32_t frame_bytes;            /* Buffer bytes one frame needs. */
  bool jpeg;                       /* Encoded output rather than raw YUV. */
  volatile uint32_t jpeg_chunks;   /* DMA chunk interrupts, diagnostics. */
  volatile uint32_t jpeg_frame_chunks; /* Chunks since the channel was armed. */
  volatile uint32_t jpeg_short;    /* Frames completed without an EOI. */
  volatile uint32_t jpeg_err_seen; /* err_count at the last VSYNC check. */
  volatile uint32_t jpeg_resets;   /* Recoveries performed. */
  volatile uint32_t jpeg_vsyncs;   /* VSYNC negedges seen this session. */
  FAR uint8_t *jpeg_stage;         /* Encoder input staging area, PSRAM. */
  uint32_t jpeg_stage_bytes;
  volatile int32_t jpeg_eoi_delta;  /* EOI offset minus byte_count_pfrm. */
  volatile uint32_t jpeg_hdr_fail;  /* Frames whose header could not be
                                     * rebuilt (bitstream not as expected). */
  volatile uint8_t jpeg_tail[8];    /* Bytes around a rejected frame's end. */
  volatile uint32_t jpeg_tail_at;   /* Where jpeg_tail was sampled. */
  volatile bool capturing;
  imgdata_capture_t capture_cb;
  FAR void *capture_cb_arg;
  struct wdog_s watchdog;
  volatile uint32_t frames_done;   /* Frames handed to the framework. */
  volatile uint32_t frames_at_arm; /* frames_done when watchdog last armed */
  volatile uint32_t timeouts;      /* Watchdog error reports. */
  volatile uint32_t set_buf_calls;
  volatile uint32_t rejected_bufs;
  clock_t start_ticks;             /* For the measured frame rate. */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_camera_imgdata_init(FAR struct imgdata_s *data);
static int bk7258_camera_imgdata_uninit(FAR struct imgdata_s *data);
static int bk7258_camera_imgdata_set_buf(FAR struct imgdata_s *data,
                                          uint8_t nr_datafmts,
                                          FAR imgdata_format_t *datafmts,
                                          uint8_t *addr, uint32_t size);
static int bk7258_camera_imgdata_validate_frame_setting(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval);
static int bk7258_camera_imgdata_start_capture(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval,
    imgdata_capture_t callback, FAR void *arg);
static int bk7258_camera_imgdata_stop_capture(FAR struct imgdata_s *data);
static void *bk7258_camera_imgdata_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size,
                                          uint32_t size);
static void bk7258_camera_imgdata_free(FAR struct imgdata_s *data,
                                        void *addr);
static void bk7258_camera_watchdog_expiry(wdparm_t arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgdata_ops_s g_bk7258_camera_imgdata_ops =
{
  .init                   = bk7258_camera_imgdata_init,
  .uninit                 = bk7258_camera_imgdata_uninit,
  .set_buf                = bk7258_camera_imgdata_set_buf,
  .validate_frame_setting = bk7258_camera_imgdata_validate_frame_setting,
  .start_capture          = bk7258_camera_imgdata_start_capture,
  .stop_capture           = bk7258_camera_imgdata_stop_capture,
  .alloc                  = bk7258_camera_imgdata_alloc,
  .free                   = bk7258_camera_imgdata_free,
};

static struct bk7258_camera_imgdata_s g_bk7258_camera_imgdata =
{
  .data = { &g_bk7258_camera_imgdata_ops },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_camera_now
 *
 * Description:
 *   Interrupt-safe timestamp for the capture callback: derived from the
 *   system tick counter rather than from clock_gettime()/gettimeofday(),
 *   neither of which is meant to be called from an interrupt handler.
 *
 ****************************************************************************/

static void bk7258_camera_now(FAR struct timeval *ts)
{
  clock_t ticks = clock_systime_ticks();

  ts->tv_sec  = ticks / TICK_PER_SEC;
  ts->tv_usec = (ticks % TICK_PER_SEC) * (USEC_PER_SEC / TICK_PER_SEC);
}

/****************************************************************************
 * Name: bk7258_camera_watchdog_arm
 *
 * Description:
 *   (Re)starts the capture watchdog and records the frame count it is
 *   measuring against.  Interrupt-safe.
 *
 ****************************************************************************/

static void bk7258_camera_watchdog_arm(
    FAR struct bk7258_camera_imgdata_s *priv)
{
  priv->frames_at_arm = priv->frames_done;
  wd_start(&priv->watchdog, MSEC2TICK(BK7258_CAMERA_WATCHDOG_MS),
           bk7258_camera_watchdog_expiry, (wdparm_t)priv);
}

/****************************************************************************
 * Name: bk7258_camera_jpeg_dma_arm
 *
 * Description:
 *   Points the drain channel at the current V4L2 buffer and starts it.
 *
 *   Called both at stream start and from the frame-done path, so it must be
 *   safe in interrupt context: register writes only.
 *
 *   The destination loops over exactly the buffer the framework gave us.
 *   That bound is what keeps a runaway encoder from writing past the buffer:
 *   a frame larger than the buffer wraps and corrupts its own beginning,
 *   which the EOI check then rejects, instead of corrupting somebody else's
 *   memory.
 *
 ****************************************************************************/

static void bk7258_camera_jpeg_dma_arm(
    FAR struct bk7258_camera_imgdata_s *priv)
{
  struct bk7258_dma_cfg_s cfg;

  /* The bitstream lands BK7258_JPEG_ENC_PAD bytes into the buffer, leaving
   * room for the standards-conforming header that replaces the block's own
   * one when the frame completes (bk7258_jpeg_enc_write_header()).  The DMA
   * window shrinks by the same amount so it still cannot run past the buffer.
   */

  uint32_t dest = (uint32_t)(uintptr_t)priv->frame_buf +
                  BK7258_JPEG_ENC_PAD;

  cfg.channel         = BK7258_CAMERA_JPEG_DMA_CH;
  cfg.src_addr        = bk7258_jpeg_enc_get_fifo_addr();
  cfg.dest_addr       = dest;
  cfg.transfer_len    = BK7258_CAMERA_JPEG_CHUNK;
  cfg.src_dev         = BK7258_DMA_DEV_JPEG;
  cfg.dest_dev        = BK7258_DMA_DEV_MEM;
  cfg.src_inc         = false;   /* A FIFO: the address must not move. */
  cfg.dest_inc        = true;
  cfg.repeat          = true;
  cfg.dest_loop_start = dest;
  cfg.dest_loop_end   = dest + priv->frame_bytes - BK7258_JPEG_ENC_PAD;
  cfg.data_width      = BK7258_DMA_WIDTH_32BITS;

  /* Cleared here because the delivered length of the next frame is counted
   * from this point: arming resets the destination to the top of the buffer.
   */

  priv->jpeg_frame_chunks = 0;

  if (bk7258_dma_configure_ex(&cfg) == 0)
    {
      bk7258_dma_start_channel(BK7258_CAMERA_JPEG_DMA_CH);
    }
}

/****************************************************************************
 * Name: bk7258_camera_jpeg_chunk_done
 *
 * Description:
 *   One DMA chunk moved.  Counting only: in REPEAT mode this interrupt is a
 *   progress report, not an end of transfer.
 *
 *   A handler must be registered even though nothing needs doing, because
 *   the DMA driver only services -- and therefore only acknowledges the
 *   finish interrupt of -- channels that have one.  Without it the channel's
 *   finish bit would stay set and the level-sensitive line would never
 *   release.  Interrupt context: counters only.
 *
 ****************************************************************************/

static void bk7258_camera_jpeg_chunk_done(FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;

  priv->jpeg_chunks++;
  priv->jpeg_frame_chunks++;
}

/****************************************************************************
 * Name: bk7258_camera_jpeg_eof
 *
 * Description:
 *   One encoded frame finished.  Runs in interrupt context (the JPEG block's
 *   EOF interrupt) and must not print, block or allocate.
 *
 *   Sequence, and why it is this order (reference:
 *   dvp_camera_jpeg_eof_handler()):
 *
 *     1. Flush the DMA's source side.  The last bytes of a frame do not
 *        land on a 32-bit boundary, and without this they stay in the
 *        channel and the frame is short by up to three bytes.
 *     2. Stop the channel, so nothing more is written while the buffer is
 *        handed over.
 *     3. Take the length from byte_count_pfrm, which the hardware counted
 *        while encoding.
 *     4. Confirm the frame ends in FF D9 and trim to it.  This is the one
 *        cheap check that the bitstream really arrived whole; a frame
 *        without it is reported as an error rather than passed off as a
 *        JPEG that no decoder will open.
 *
 ****************************************************************************/

static void bk7258_camera_jpeg_eof(FAR void *arg, uint32_t bytes)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;

  /* The bitstream starts PAD bytes into the V4L2 buffer (see
   * bk7258_camera_jpeg_dma_arm), so every offset below -- delivered, the EOI
   * search, the recorded tail -- is relative to this, not to the buffer.
   */

  FAR const uint8_t *buf = priv->frame_buf == NULL ? NULL :
                           priv->frame_buf + BK7258_JPEG_ENC_PAD;
  uint32_t capacity;
  struct timeval ts;
  uint32_t len = 0;
  uint32_t delivered;
  uint32_t hdrlen;
  uint32_t i;

  if (!priv->capturing || priv->frame_buf == NULL)
    {
      return;
    }

  /* Wait for the tail of the frame to actually arrive.
   *
   * EOF means the encoder stopped producing, not that the bitstream reached
   * memory: the output FIFO still holds a few hundred bytes, and flushing
   * the DMA's own source buffer does not move them (that only pushes the
   * partial word held inside the channel).  Examined at EOF, the buffer ends
   * mid-entropy-data with no end-of-image marker -- measured: at the
   * reported 7515 bytes the memory read f7 5f fb e7 ff 00 af 46, i.e. the
   * stream carried on.
   *
   * The wait is bounded because this runs in interrupt context.  The drain
   * is a few hundred bytes at one 32-bit word per request, so it finishes in
   * microseconds; the bound only stops a stalled bus from wedging the
   * handler.
   */

  for (i = 0; i < BK7258_CAMERA_JPEG_DRAIN_SPINS; i++)
    {
      if (bk7258_jpeg_enc_fifo_empty())
        {
          break;
        }
    }

  bk7258_dma_flush_src_buffer(BK7258_CAMERA_JPEG_DMA_CH);
  bk7258_dma_stop_channel(BK7258_CAMERA_JPEG_DMA_CH);

  /* How many bytes the DMA actually put in the buffer since it was armed.
   *
   * This, not byte_count_pfrm, is where the frame ends in memory.  The two
   * disagree, and the buffer makes it obvious why: with the channel running
   * in REPEAT the bitstreams of successive frames sit end to end, so
   * whatever lies beyond the current frame is the tail of an older, longer
   * one.  Measured at a reported 6975 bytes, memory read
   * 28 fe d6 9f fe 81 f2 9f -- entropy data belonging to a previous frame.
   *
   * Counting delivery instead needs no assumption about what the hardware's
   * frame counter includes.
   */

  delivered = priv->jpeg_frame_chunks * BK7258_CAMERA_JPEG_CHUNK +
              (BK7258_CAMERA_JPEG_CHUNK -
               bk7258_dma_get_channel_remain_len(
                   BK7258_CAMERA_JPEG_DMA_CH));

  capacity = priv->frame_bytes - BK7258_JPEG_ENC_PAD;

  if (delivered > capacity)
    {
      delivered = capacity;
    }

  if (delivered >= 2u)
    {
      /* Search a window around the reported length rather than only below
       * it.  byte_count_pfrm is the hardware's count for the frame, but it
       * is not necessarily the file length: the marker turned out to sit
       * *past* it on this board, so a search that only looks backwards (as
       * the reference's does) rejects every frame.  The delta is recorded
       * below so the relationship is documented by measurement rather than
       * assumed.
       *
       * Scanning forward is safe: JPEG byte-stuffs every FF inside entropy
       * data as FF 00, so a bare FF D9 can only be the end-of-image marker.
       */

      /* Scan back from the end of what was delivered.  The encoder appends
       * 5 CRC bytes after the marker (JPEG_CRC_SIZE), and the last chunk is
       * accounted in whole words, so the marker sits a few bytes short of
       * the delivered count rather than exactly at it.  Scanning backwards
       * is safe: JPEG byte-stuffs every FF inside entropy data as FF 00, so
       * a bare FF D9 can only be the end-of-image marker.
       */

      uint32_t lo = delivered > BK7258_CAMERA_JPEG_EOI_BACK ?
                    delivered - BK7258_CAMERA_JPEG_EOI_BACK : 2u;

      for (i = delivered; i >= lo; i--)
        {
          if (buf[i - 1] == 0xd9u && buf[i - 2] == 0xffu)
            {
              len = i;
              priv->jpeg_eoi_delta = (int32_t)len - (int32_t)bytes;
              break;
            }
        }
    }

  bk7258_camera_now(&ts);

  if (len == 0)
    {
      priv->jpeg_short++;

      /* Record what is actually at the reported length, so the next run says
       * what the encoder produced instead of guessing at it.
       */

      if (delivered >= 4u && delivered + 4u <= capacity)
        {
          uint32_t k;

          priv->jpeg_tail_at = delivered;
          for (k = 0; k < 8u; k++)
            {
              priv->jpeg_tail[k] = buf[delivered - 4u + k];
            }
        }

      if (priv->capture_cb != NULL)
        {
          priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
        }
    }
  else
    {
      /* Replace the block's header with one standard decoders accept.  It is
       * longer than the block's, which is why the bitstream was placed PAD
       * bytes in; the returned offset is where the entropy data starts, so
       * the file the application sees runs from the buffer's first byte to
       * the end of what was delivered.
       */

      hdrlen = (uint32_t)bk7258_jpeg_enc_write_header(priv->frame_buf,
                                                      BK7258_JPEG_ENC_PAD);
      if (hdrlen == 0)
        {
          priv->jpeg_hdr_fail++;

          if (priv->capture_cb != NULL)
            {
              priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
            }
        }
      else
        {
          priv->frames_done++;

          if (priv->capture_cb != NULL)
            {
              priv->capture_cb(0, BK7258_JPEG_ENC_PAD + len, &ts,
                               priv->capture_cb_arg);
            }
        }
    }

  if (priv->capturing)
    {
      /* Re-arm the drain here rather than waiting for set_buf().
       *
       * set_buf() only comes back when the application re-queues the buffer,
       * and it does not do that for a frame completed with an error.  The
       * first version relied on it, so the channel died on the first bad
       * frame: both board runs stopped at exactly 7 chunks (~72KB) however
       * long they ran and whatever frame rate was programmed, which is what
       * gave it away.
       */

      bk7258_camera_jpeg_dma_arm(priv);
      bk7258_camera_watchdog_arm(priv);
    }
}

/****************************************************************************
 * Name: bk7258_camera_jpeg_vsync
 *
 * Description:
 *   Frame boundary, in interrupt context.  Recovers the encoder if it gave
 *   up on the frame that just went past.
 *
 *   Why this is needed at all: the sensor streams continuously, so the
 *   encoder is always enabled somewhere in the middle of a frame and its
 *   first frame is a fragment.  Left alone it stays wedged -- measured on
 *   this board, 28 frames started, 26 raised frame_err, exactly one reached
 *   EOF and that one was 3.5KB with no end-of-image marker.  The reference
 *   handles this the same way: its VSYNC handler calls
 *   dvp_camera_reset_hardware_modules_handler() whenever an error is
 *   flagged, which is where this sequence comes from.
 *
 *   The sequence matters: both modules are stopped before either is reset,
 *   the drain is restarted before the producers, and the encoder is enabled
 *   last, so it comes up on a frame boundary rather than mid-frame.
 *
 *   Every call here is register-only and print-free.  The encoder's
 *   configuration survives its soft reset (the reference does not
 *   reprogram it either), so recovery costs no I2C and no table writes.
 *
 ****************************************************************************/

static void bk7258_camera_jpeg_vsync(FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;
  struct bk7258_jpeg_enc_stats_s st;

  if (!priv->capturing || !priv->jpeg)
    {
      return;
    }

  priv->jpeg_vsyncs++;

  bk7258_jpeg_enc_get_stats(&st);

  if (st.err_count == priv->jpeg_err_seen)
    {
      return;
    }

  priv->jpeg_err_seen = st.err_count;
  priv->jpeg_resets++;

  bk7258_yuv_buf_stop();
  bk7258_jpeg_enc_stop();

  bk7258_jpeg_enc_soft_reset();
  bk7258_yuv_buf_soft_reset();

  bk7258_dma_stop_channel(BK7258_CAMERA_JPEG_DMA_CH);
  bk7258_camera_jpeg_dma_arm(priv);

  bk7258_yuv_buf_start_jpeg();
  bk7258_jpeg_enc_start();
}

/****************************************************************************
 * Name: bk7258_camera_frame_done
 *
 * Description:
 *   YUV_BUF frame-done (YUV_ARV) callback -- interrupt context, one call
 *   per completed frame already written into priv->frame_buf by the
 *   hardware.  Reports the frame to the V4L2 framework, which re-arms the
 *   next buffer through set_buf().  Must stay print-free.
 *
 ****************************************************************************/

static void bk7258_camera_frame_done(FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;
  struct timeval ts;

  if (!priv->capturing || priv->frame_buf == NULL)
    {
      return;
    }

  /* In JPEG mode this event still arrives -- YUV_BUF's VSYNC/YUV_ARV
   * interrupts stay enabled because the watchdog uses them as the "sensor is
   * alive" marker -- but it must not complete a buffer: the module's frame
   * writer is switched off, so the buffer holds no image, and the encoded
   * frame is reported by bk7258_camera_jpeg_eof() instead.  Completing here
   * is what handed the application a whole buffer of uninitialised PSRAM on
   * the first JPEG attempt.
   */

  if (priv->jpeg)
    {
      bk7258_camera_watchdog_arm(priv);
      return;
    }

  priv->frames_done++;

  if (priv->capture_cb != NULL)
    {
      bk7258_camera_now(&ts);
      priv->capture_cb(0, priv->frame_buf_size, &ts, priv->capture_cb_arg);
    }

  if (priv->capturing)
    {
      bk7258_camera_watchdog_arm(priv);
    }
}

/****************************************************************************
 * Name: bk7258_camera_watchdog_expiry
 *
 * Description:
 *   No frame arrived within BK7258_CAMERA_WATCHDOG_MS.  Report an error
 *   frame so a blocked VIDIOC_DQBUF returns (with V4L2_BUF_FLAG_ERROR set
 *   on the buffer) instead of hanging the caller forever, then keep
 *   watching -- the hardware is left running so a late first frame is
 *   still captured.  Interrupt context: no printf().
 *
 ****************************************************************************/

static void bk7258_camera_watchdog_expiry(wdparm_t arg)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)arg;
  struct timeval ts;

  if (!priv->capturing)
    {
      return;
    }

  if (priv->frames_done == priv->frames_at_arm)
    {
      priv->timeouts++;

      if (priv->capture_cb != NULL)
        {
          bk7258_camera_now(&ts);

          /* Non-zero result: complete_capture() marks the buffer
           * V4L2_BUF_FLAG_ERROR, completes it and posts dqbuf_wait_flg.
           */

          priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
        }
    }

  if (priv->capturing)
    {
      bk7258_camera_watchdog_arm(priv);
    }
}

static int bk7258_camera_imgdata_init(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  printf("bk7258_camera_imgdata: init: entry\n");

  priv->capturing      = false;
  priv->frames_done    = 0;
  priv->frames_at_arm  = 0;
  priv->timeouts       = 0;
  priv->set_buf_calls  = 0;
  priv->rejected_bufs  = 0;
  priv->jpeg           = false;
  priv->jpeg_chunks    = 0;
  priv->jpeg_short     = 0;
  priv->jpeg_err_seen  = 0;
  priv->jpeg_resets    = 0;
  priv->jpeg_stage     = NULL;
  priv->jpeg_stage_bytes = 0;
  priv->jpeg_hdr_fail  = 0;

  bk7258_yuv_buf_init();
  bk7258_yuv_buf_set_frame_callback(bk7258_camera_frame_done, priv);

  /* The JPEG path's two other pieces are brought up here rather than lazily
   * at the first JPEG stream, because both print and attach interrupts,
   * which is task-level work; start_capture() can be reached from a context
   * where that is not welcome.  Neither does anything until configured, and
   * a raw-YUV-only user pays one clock gate and one idle interrupt line.
   */

  bk7258_dma_init();

  if (bk7258_jpeg_enc_init() < 0)
    {
      printf("bk7258_camera_imgdata: init: JPEG encoder unavailable, "
             "raw capture only\n");
    }

  printf("bk7258_camera_imgdata: init: complete\n");

  return OK;
}

static int bk7258_camera_imgdata_uninit(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  FAR const uint8_t *p = priv->frame_buf;

  priv->capturing = false;
  wd_cancel(&priv->watchdog);

  if (priv->jpeg)
    {
      /* Encoder first, then the channel that drains it: stopping the drain
       * while the encoder still produced output would leave the FIFO
       * filling with nobody reading it.
       */

      bk7258_yuv_buf_set_vsync_callback(NULL, NULL);
      bk7258_jpeg_enc_stop();
      bk7258_dma_stop_channel(BK7258_CAMERA_JPEG_DMA_CH);
    }

  bk7258_yuv_buf_stop();
  bk7258_yuv_buf_set_frame_callback(NULL, NULL);

  /* Task context (VIDIOC close path): safe to print, and this is the one
   * place that reports the whole session's outcome.  The first bytes of
   * the last frame buffer are dumped here because this board has no
   * filesystem big enough to save a 614400-byte frame (tmpfs lives in the
   * 336KB kernel heap), so a hexdump is the only way to confirm real
   * pixel data was captured rather than an all-zero buffer.
   */

  printf("bk7258_camera_imgdata: uninit: frames=%u timeouts=%u "
         "set_buf=%u rejected=%u\n",
         (unsigned int)priv->frames_done, (unsigned int)priv->timeouts,
         (unsigned int)priv->set_buf_calls,
         (unsigned int)priv->rejected_bufs);

  bk7258_yuv_buf_dump_status("uninit");

  if (p != NULL && priv->frames_done > 0)
    {
      printf("bk7258_camera_imgdata: uninit: frame[0..31] = "
             "%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x\n",
             p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
             p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
             p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
             p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
    }

  return OK;
}

static int bk7258_camera_imgdata_set_buf(FAR struct imgdata_s *data,
                                          uint8_t nr_datafmts,
                                          FAR imgdata_format_t *datafmts,
                                          uint8_t *addr, uint32_t size)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  uint32_t phys = (uint32_t)(uintptr_t)addr;
  uint32_t need;
  uint32_t span;

  /* How much of the buffer the hardware is allowed to use, and the least it
   * has to be given.
   *
   * Raw capture writes whole frames, so the buffer must hold exactly one and
   * the geometry fixes both numbers.  Encoded capture is the other way
   * round: the length is whatever the encoder produces, the buffer is
   * capacity, and the application chose it (see BK7258_CAMERA_JPEG_MIN_BUF).
   * The span keeps BK7258_JPEG_ENC_PAD for the header that gets written in
   * front of the bitstream, and what is left is rounded down to the drain
   * channel's transfer_len: the channel loops in whole steps, so a window
   * that is not a multiple of one would let the last step write past its
   * end, which is the one thing this bound exists to prevent.
   */

  if (priv->jpeg)
    {
      /* The drain window is the buffer minus the header reservation, and it
       * is that window -- not the buffer -- which has to be a whole number
       * of transfer_len steps, because the channel loops over it.
       */

      need = BK7258_CAMERA_JPEG_MIN_BUF;
      span = size > BK7258_JPEG_ENC_PAD ?
             BK7258_JPEG_ENC_PAD +
             ((size - BK7258_JPEG_ENC_PAD) / BK7258_CAMERA_JPEG_CHUNK) *
             BK7258_CAMERA_JPEG_CHUNK : 0;
    }
  else
    {
      need = priv->frame_bytes;
      span = priv->frame_bytes;
    }

  /* Interrupt context is possible here (v4l2_cap.c complete_capture()
   * re-arms the next buffer), so only report through counters unless
   * this is a task-level call.
   */

  if (addr == NULL || priv->width == 0 || span == 0 ||
      size < need ||
      phys < BK7258_CAMERA_PSRAM_BASE ||
      phys + span > BK7258_CAMERA_PSRAM_END)
    {
      priv->rejected_bufs++;

      if (!up_interrupt_context())
        {
          printf("bk7258_camera_imgdata: set_buf: rejected addr=%p "
                 "size=%u (need >=%u bytes inside PSRAM "
                 "0x%08x-0x%08x)\n",
                 addr, (unsigned int)size,
                 (unsigned int)need,
                 (unsigned int)BK7258_CAMERA_PSRAM_BASE,
                 (unsigned int)BK7258_CAMERA_PSRAM_END);
        }

      return -EINVAL;
    }

  priv->frame_bytes = span;
  priv->frame_buf = addr;
  priv->frame_buf_size = priv->frame_bytes;
  priv->set_buf_calls++;

  /* Point the hardware's frame writer at this buffer.  Done on every
   * call, which is also how the next frame gets re-armed from the
   * frame-done path (reference: dvp_camera_yuv_eof_handler()'s
   * bk_yuv_buf_set_em_base_addr()).
   *
   * In JPEG mode the module writes no frames at all -- the encoder's output
   * is drained by DMA -- so the equivalent re-arm is pointing that channel
   * at the new buffer and restarting it.
   */

  if (priv->jpeg)
    {
      if (priv->capturing)
        {
          bk7258_camera_jpeg_dma_arm(priv);
        }
    }
  else
    {
      bk7258_yuv_buf_set_frame_buffer(phys);
    }

  if (!up_interrupt_context())
    {
      printf("bk7258_camera_imgdata: set_buf: addr=%p frame_bytes=%u "
             "(buffer size given=%u)\n",
             addr, (unsigned int)priv->frame_buf_size, (unsigned int)size);
    }

  return OK;
}

static int bk7258_camera_imgdata_validate_frame_setting(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  uint16_t w;
  uint16_t h;

  if (nr_datafmts < 1)
    {
      return -EINVAL;
    }

  if (datafmts[IMGDATA_FMT_MAIN].pixelformat != IMGDATA_PIX_FMT_UYVY &&
      datafmts[IMGDATA_FMT_MAIN].pixelformat != IMGDATA_PIX_FMT_JPEG)
    {
      return -EINVAL;
    }

  w = datafmts[IMGDATA_FMT_MAIN].width;
  h = datafmts[IMGDATA_FMT_MAIN].height;

  /* YUV_BUF's pixel register counts 8x8 blocks, so a geometry that is not a
   * multiple of 8 cannot be expressed at all -- it would silently capture a
   * truncated frame.
   */

  if (w == 0 || h == 0 ||
      (w % BK7258_CAMERA_ALIGN) != 0 || (h % BK7258_CAMERA_ALIGN) != 0)
    {
      return -EINVAL;
    }

  if ((uint32_t)w * h * 2u >
      BK7258_CAMERA_PSRAM_END - BK7258_CAMERA_PSRAM_BASE)
    {
      return -EINVAL;
    }

  /* Latched here rather than in start_capture() because set_buf() needs it
   * first: the framework calls this on VIDIOC_S_FMT, then allocates and
   * queues buffers, and only then starts streaming.  A "validate" that also
   * records is not pretty, but the alternative -- deriving the geometry from
   * the buffer size in set_buf() -- cannot tell 640x480 from 800x384.
   */

  priv->width = w;
  priv->height = h;
  priv->jpeg = datafmts[IMGDATA_FMT_MAIN].pixelformat ==
               IMGDATA_PIX_FMT_JPEG;

  /* The buffer size the framework will hand to set_buf().
   *
   * For UYVY it is fixed by the geometry: the module writes width*height*2
   * bytes and nothing else will do, so record it here and reject a smaller
   * buffer later.
   *
   * For JPEG there is nothing to record.  v4l2_cap.c's get_bufsize() honours
   * the format's sizeimage first and only computes width*height when the
   * application left it at zero, so the capacity is the application's to
   * choose and is not knowable from the geometry -- an earlier version
   * assumed width*height here and rejected every buffer from an application
   * that set a smaller sizeimage (packages/ai_agent asks for 160KB, which is
   * below 480*480).  set_buf() takes the real capacity from its own
   * argument.
   */

  priv->frame_bytes = priv->jpeg ? 0u : (uint32_t)w * h * 2u;

  return OK;
}

static int bk7258_camera_imgdata_start_capture(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval,
    imgdata_capture_t callback, FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  struct bk7258_jpeg_enc_stats_s st;
  int ret;

  printf("bk7258_camera_imgdata: start_capture: entry, frame_buf=%p "
         "frame_buf_size=%u\n", priv->frame_buf,
         (unsigned int)priv->frame_buf_size);

  ret = bk7258_camera_imgdata_validate_frame_setting(data, nr_datafmts,
                                                      datafmts, interval);
  if (ret < 0)
    {
      printf("bk7258_camera_imgdata: start_capture: validate failed, "
             "ret=%d\n", ret);
      return ret;
    }

  if (priv->frame_buf == NULL)
    {
      printf("bk7258_camera_imgdata: start_capture: frame_buf is NULL, "
             "-EINVAL\n");
      return -EINVAL;
    }

  priv->capture_cb = callback;
  priv->capture_cb_arg = arg;
  priv->frames_done = 0;
  priv->timeouts = 0;
  priv->jpeg_chunks = 0;
  priv->jpeg_short = 0;

  bk7258_yuv_buf_configure(priv->width, priv->height);

  if (priv->jpeg)
    {
      /* Order matters: the drain channel must be running before the encoder
       * is, or the first bytes of the first frame are produced with nothing
       * listening on the FIFO.
       */

      /* The encoder's input staging area, which is what YUV_BUF's
       * frame-buffer register means in JPEG mode.  It must not be the V4L2
       * buffer: see BK7258_CAMERA_JPEG_STAGE_LINES.
       */

      priv->jpeg_stage_bytes = (uint32_t)priv->width *
                               BK7258_CAMERA_JPEG_STAGE_LINES * 2u;

      if (priv->jpeg_stage == NULL)
        {
          priv->jpeg_stage = bk7258_media_pool_alloc(
              BK7258_PSRAM_POOL_DISPLAY, 32, priv->jpeg_stage_bytes);
        }

      if (priv->jpeg_stage == NULL)
        {
          printf("bk7258_camera_imgdata: start_capture: no PSRAM for the "
                 "%u-byte encoder staging area\n",
                 (unsigned int)priv->jpeg_stage_bytes);
          return -ENOMEM;
        }

      bk7258_yuv_buf_set_frame_buffer((uint32_t)(uintptr_t)priv->jpeg_stage);

      bk7258_jpeg_enc_configure(priv->width, priv->height);
      bk7258_jpeg_enc_set_buffer((uint32_t)(uintptr_t)priv->frame_buf);
      bk7258_jpeg_enc_register_callback(bk7258_camera_jpeg_eof, priv);

      bk7258_dma_set_channel_callback(BK7258_CAMERA_JPEG_DMA_CH,
                                      bk7258_camera_jpeg_chunk_done, priv);
      bk7258_jpeg_enc_get_stats(&st);
      priv->jpeg_err_seen = st.err_count;
      priv->jpeg_resets = 0;
      priv->jpeg_vsyncs = 0;
      bk7258_yuv_buf_set_vsync_callback(bk7258_camera_jpeg_vsync, priv);
      priv->capturing = true;
      bk7258_camera_jpeg_dma_arm(priv);

      priv->start_ticks = clock_systime_ticks();
      bk7258_yuv_buf_start_jpeg();
      bk7258_jpeg_enc_start();
    }
  else
    {
      bk7258_yuv_buf_set_frame_buffer((uint32_t)(uintptr_t)priv->frame_buf);

      priv->capturing = true;
      priv->start_ticks = clock_systime_ticks();
      bk7258_yuv_buf_start();
    }

  bk7258_camera_watchdog_arm(priv);

  bk7258_yuv_buf_dump_status("start_capture: started");

  if (priv->jpeg)
    {
      bk7258_jpeg_enc_dump_status("start_capture: started");
    }

  printf("bk7258_camera_imgdata: start_capture: complete at %ux%u "
         "(%s, %u buffer bytes), watchdog=%dms\n",
         (unsigned int)priv->width, (unsigned int)priv->height,
         priv->jpeg ? "JPEG" : "UYVY",
         (unsigned int)priv->frame_bytes, BK7258_CAMERA_WATCHDOG_MS);

  return OK;
}

static int bk7258_camera_imgdata_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  priv->capturing = false;
  wd_cancel(&priv->watchdog);

  if (priv->jpeg)
    {
      /* Encoder first, then the channel that drains it: stopping the drain
       * while the encoder still produced output would leave the FIFO
       * filling with nobody reading it.
       */

      bk7258_yuv_buf_set_vsync_callback(NULL, NULL);
      bk7258_jpeg_enc_stop();
      bk7258_dma_stop_channel(BK7258_CAMERA_JPEG_DMA_CH);
    }

  bk7258_yuv_buf_stop();

  if (priv->jpeg_stage != NULL && !up_interrupt_context())
    {
      bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY, priv->jpeg_stage);
      priv->jpeg_stage = NULL;
    }

  /* Interrupt context is possible here: complete_capture() calls
   * IMGDATA_STOP_CAPTURE() when no vacant container is left.
   */

  if (!up_interrupt_context())
    {
      uint32_t ms = TICK2MSEC(clock_systime_ticks() - priv->start_ticks);

      /* Measured frame rate, not the requested one: the only way to see
       * whether the sensor's programmed rate is what the hardware
       * actually delivers.
       */

      printf("bk7258_camera_imgdata: stop_capture: frames=%u timeouts=%u "
             "elapsed=%ums measured=%u.%02u fps\n",
             (unsigned int)priv->frames_done,
             (unsigned int)priv->timeouts, (unsigned int)ms,
             (unsigned int)(ms ? priv->frames_done * 1000u / ms : 0u),
             (unsigned int)(ms ? priv->frames_done * 100000u / ms % 100u
                               : 0u));

      if (priv->jpeg)
        {
          /* The drain path's own numbers.  chunks == 0 together with a
           * non-zero SOF count is the signature of a channel that is
           * configured but moving nothing: the encoder starts frames, its
           * FIFO fills, YUV_BUF reports sen_full, and no EOF ever arrives.
           */

          printf("bk7258_camera_imgdata: stop_capture: jpeg chunks=%u "
                 "short=%u resets=%u vsyncs=%u hdr_fail=%u eoi_delta=%d "
                 "dma_remain=%u\n",
                 (unsigned int)priv->jpeg_chunks,
                 (unsigned int)priv->jpeg_short,
                 (unsigned int)priv->jpeg_resets,
                 (unsigned int)priv->jpeg_vsyncs,
                 (unsigned int)priv->jpeg_hdr_fail,
                 (int)priv->jpeg_eoi_delta,
                 (unsigned int)bk7258_dma_get_channel_remain_len(
                     BK7258_CAMERA_JPEG_DMA_CH));
          printf("bk7258_camera_imgdata: stop_capture: bytes[%u-4..+4] = "
                 "%02x %02x %02x %02x | %02x %02x %02x %02x\n",
                 (unsigned int)priv->jpeg_tail_at,
                 priv->jpeg_tail[0], priv->jpeg_tail[1],
                 priv->jpeg_tail[2], priv->jpeg_tail[3],
                 priv->jpeg_tail[4], priv->jpeg_tail[5],
                 priv->jpeg_tail[6], priv->jpeg_tail[7]);
          bk7258_jpeg_enc_dump_status("stop_capture");
        }
    }

  return OK;
}

/* V4L2_REQBUFS_COUNT_MAX (3) capture buffers at one 640x480 YUYV frame
 * (614400 bytes) exceed this board's internal SRAM heap
 * (CONFIG_RAM_SIZE=344064 bytes), so the framework's default
 * kumm_memalign()-backed allocation cannot hold the buffer pool.  This
 * board has a separate 16MB PSRAM region not mapped into the kernel's
 * malloc arena; imgdata_ops_s.alloc/free (the framework's documented
 * mechanism for drivers whose buffers must come from a non-default
 * memory pool) routes allocation to bk7258_psram.c's
 * bk7258_media_pool_alloc()/_free() instead.  YUV_BUF also requires the
 * frame buffer to be in PSRAM (see set_buf()).
 */

static void *bk7258_camera_imgdata_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size,
                                          uint32_t size)
{
  return bk7258_media_pool_alloc(BK7258_PSRAM_POOL_DISPLAY,
                                  align_size, size);
}

static void bk7258_camera_imgdata_free(FAR struct imgdata_s *data,
                                        void *addr)
{
  bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY, addr);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct imgdata_s *bk7258_camera_imgdata_initialize(void)
{
  return &g_bk7258_camera_imgdata.data;
}
