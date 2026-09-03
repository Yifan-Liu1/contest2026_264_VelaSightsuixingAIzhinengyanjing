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
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>

#include <sys/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/video/imgdata.h>
#include <nuttx/wqueue.h>

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

/* Ring size in chunks.  24 chunks is 240KB, about eight 640x480 frames at the
 * measured 25KB: enough that a late copy cannot be overtaken by the encoder,
 * and small enough to sit in the display pool alongside everything else.
 */

#define BK7258_CAMERA_JPEG_RING_CHUNKS 24u
#define BK7258_CAMERA_JPEG_RING_SLOTS  3u
#define BK7258_CAMERA_JPEG_VALIDATE_BYTES (80u * 1024u)

/* How far into a span to look for the frame's SOI.  The leftovers of the
 * previous frame measured a few dozen bytes; 256 is comfortable slack and
 * still far short of the header, so a hit can only be the real frame start.
 */

#define BK7258_CAMERA_JPEG_SOI_SCAN   256u
#define BK7258_CAMERA_JPEG_EOI_BACK   16u
#define BK7258_CAMERA_JPEG_EOI_FWD    1024u

/* How long the EOF handler may wait for the encoder's output FIFO to empty.
 *
 * Configurable because it is a busy-wait in interrupt context and the vendor
 * does not have it at all: its EOF handler flushes and stops the channel
 * straight away, and catches whatever the drain left behind through the
 * byte-count reconciliation below.  Keeping the wait is cheaper than
 * discarding a frame, but only measurement can say how long it really runs,
 * which is what jpeg_drain_spins_max is for.
 */

#ifndef CONFIG_BK7258_CAMERA_JPEG_DRAIN_SPINS
#  define CONFIG_BK7258_CAMERA_JPEG_DRAIN_SPINS 100000
#endif

#define BK7258_CAMERA_JPEG_DRAIN_SPINS \
  ((uint32_t)CONFIG_BK7258_CAMERA_JPEG_DRAIN_SPINS)

/* How many macroblocks of a delivered frame are Huffman-validated.
 *
 * Zero means the whole frame, which is what this driver did until now and
 * what CONFIG_BK7258_CAMERA_JPEG_VALIDATE_MCUS=0 restores.  See
 * bk7258_jpeg_realign_entropy_prefix() for why a prefix settles the question
 * the walk is asked (bit alignment is a property of the whole scan) and what
 * it gives up (corruption past the prefix, which the reconciliation covers).
 */

#ifndef CONFIG_BK7258_CAMERA_JPEG_VALIDATE_MCUS
#  define CONFIG_BK7258_CAMERA_JPEG_VALIDATE_MCUS 0
#endif

#ifndef CONFIG_BK7258_CAMERA_JPEG_DMA_DEST_BURST
#  define CONFIG_BK7258_CAMERA_JPEG_DMA_DEST_BURST 0
#endif

/* Tolerance for the delivered-versus-counted comparison.
 *
 * The hardware appends JPEG_CRC_SIZE (5) bytes after the end-of-image marker
 * and the last DMA chunk is accounted in whole 32-bit words, so a healthy
 * frame does not match exactly; the vendor allows any difference short of a
 * whole chunk.  This window is deliberately much tighter, because its
 * purpose here is to notice a span that picked up a neighbour's bytes.
 */

#define BK7258_CAMERA_JPEG_RECON_SLACK 64

#ifdef CONFIG_BK7258_CAMERA_JPEG_RECONCILE
#  define BK7258_CAMERA_JPEG_RECON_ENFORCED 1
#else
#  define BK7258_CAMERA_JPEG_RECON_ENFORCED 0
#endif

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
  bool jpeg_software;              /* This geometry uses software fallback. */
  volatile uint32_t jpeg_chunks;   /* DMA chunk interrupts, diagnostics. */
  volatile uint32_t jpeg_frame_chunks; /* Chunks since the channel was armed. */
  volatile uint32_t jpeg_short;    /* Frames completed without an EOI. */
  volatile uint32_t jpeg_err_seen; /* err_count at the last VSYNC check. */
  volatile uint32_t jpeg_resets;   /* Recoveries performed. */
  volatile uint32_t jpeg_vsyncs;   /* VSYNC negedges seen this session. */

  /* Software JPEG: raw frames land in jpeg_raw and are encoded by
   * g_sw_jpeg() on the low-priority work queue, because the encode takes
   * ~270ms and this driver is otherwise entirely interrupt-driven.
   */

  struct work_s jpeg_work;

  /* Two staging frames, filled alternately by YUV_BUF.  One is being encoded
   * while the other takes the next frame, and a frame that arrives while the
   * encoder is busy overwrites the *unprocessed* one -- which is the
   * "drop the oldest unprocessed frame" rule the product spec asks for, and
   * the opposite of what a single buffer would do (it would either tear or
   * make the newest frame wait behind an old one).
   */

  FAR uint8_t *jpeg_raw[2];        /* UYVY staging, PSRAM */
  uint8_t jpeg_raw_fill;           /* Which one the hardware is filling */
  int8_t jpeg_raw_ready;           /* Newest complete one, -1 if none */
  uint32_t jpeg_raw_bytes;
  clock_t jpeg_next_sample;        /* Earliest tick for the next encode */

  /* The delivery rate in force for this stream, and the period derived from
   * it.  CONFIG_BK7258_CAMERA_JPEG_FPS is the ceiling; an application may ask
   * for less through VIDIOC_S_PARM.  See bk7258_camera_jpeg_sample_period().
   */

  uint32_t jpeg_fps;
  clock_t  jpeg_sample_period;
  volatile bool jpeg_encoding;     /* An encode is in flight */
  volatile bool jpeg_buf_armed;    /* The framework queued a buffer that has
                                    * not been completed yet */
  volatile uint32_t set_buf_live;   /* set_buf calls made while streaming */
  volatile uint32_t jpeg_sw_frames;
  volatile uint32_t jpeg_sw_drops;    /* Unprocessed frames replaced */
  volatile uint32_t jpeg_sw_skipped;  /* Frames the 5fps sampler passed on */

  /* jpeg_sw_drops used to count both "the encoder was still busy" and "the
   * application had not queued a buffer", which made a stall impossible to
   * attribute: the two have opposite causes and opposite fixes.  Split.
   */

  volatile uint32_t jpeg_sw_busy;      /* Skipped: an encode was in flight   */
  volatile uint32_t jpeg_sw_nobuf;     /* Skipped: no queued V4L2 buffer     */
  volatile uint32_t jpeg_hw_busy;      /* HW frames skipped while validating */
  volatile clock_t  jpeg_last_now;     /* Ticks at the last sampler decision */
  uint32_t jpeg_work_len;              /* Raw length ending after EOI */
  bool jpeg_work_finish_pending;       /* DMA diagnostic for queued frame */
  struct timeval jpeg_work_ts;         /* EOF time reported on completion */
  FAR uint8_t *jpeg_stage;         /* Encoder input staging area, PSRAM. */
  uint32_t jpeg_stage_bytes;
  FAR uint8_t *jpeg_validate_scratch; /* Entropy parser input, internal RAM */
  uint32_t jpeg_validate_scratch_bytes;
  volatile uint32_t jpeg_validate_ticks;
  volatile uint32_t jpeg_validate_runs;
  volatile int32_t jpeg_eoi_delta;  /* EOI offset minus byte_count_pfrm. */
  volatile uint32_t jpeg_hdr_fail;  /* Frames whose header could not be
                                     * rebuilt (bitstream not as expected). */
  volatile uint32_t jpeg_bit_fixed; /* Frames repaired by dropping leading
                                     * entropy bits. */
  volatile uint32_t jpeg_bit_fail;  /* Frames with no valid bit alignment. */
  volatile uint8_t jpeg_tail[8];    /* Bytes around a rejected frame's end. */
  volatile uint32_t jpeg_tail_at;   /* Where jpeg_tail was sampled. */

  /* Where the time in a delivered frame actually goes.
   *
   * Cycle counts, not ticks: at CONFIG_USEC_PER_TICK=1000 a tick cannot see
   * the copy at all, and "0 ms" is indistinguishable from "not measured".
   * up_perf_gettime() is the DWT cycle counter on this core (armv8-m
   * arm_perf.c), so these are directly comparable with each other and, once
   * divided by the elapsed wall time, they also say what the core clock
   * really is -- which is the one number the driver has so far had to assume
   * (CONFIG_BK7258_CPU_FREQ_HZ only ever reached systick).
   */

  volatile uint32_t jpeg_copy_cycles;    /* Ring span -> V4L2 buffer */
  volatile uint32_t jpeg_hdr_cycles;     /* write_header() */
  volatile uint32_t jpeg_scratch_cycles; /* Entropy in/out of scratch */
  volatile uint32_t jpeg_realign_cycles; /* Huffman/MCU validation */
  clock_t jpeg_start_cycles;             /* Cycle counter at stream start */

  /* Worst and total FIFO drain waits, and how often the wait ran out. */

  volatile uint32_t jpeg_drain_spins_max;
  volatile uint32_t jpeg_drain_spins_sum;
  volatile uint32_t jpeg_drain_timeouts;

  /* The vendor's frame-integrity check: what the hardware says it encoded
   * against what the DMA actually delivered (dvp_camera_jpeg_eof_handler()).
   * Recorded always; enforced only with CONFIG_BK7258_CAMERA_JPEG_RECONCILE,
   * because the tolerance window is a property of this board's drain
   * behaviour and has to be measured before it can be trusted to reject.
   */

  volatile int32_t jpeg_recon_delta;    /* Last delivered-minus-counted */
  volatile int32_t jpeg_recon_worst;    /* Largest magnitude seen */
  volatile uint32_t jpeg_recon_bad;     /* Frames outside the window */

  /* The drain ring.  Three slots: the DMA owns one at a time and is switched
   * to the next at every EOF -- flush, stop, re-arm, exactly as the vendor
   * does at its own frame boundary (dvp_camera_jpeg_eof_handler()).  The
   * difference is only where it re-arms to: the vendor points the channel at
   * the next frame buffer it will hand up, while this driver cannot (the
   * framework supplies one buffer at a time, inside the completion
   * callback), so a frame is a span of the slot that was just closed,
   * copied out from there.
   */

  FAR uint8_t *jpeg_ring;
  uint32_t jpeg_ring_bytes;
  uint32_t jpeg_ring_read;            /* Offset within the current slot */
  uint8_t jpeg_ring_slot;             /* Slot currently owned by DMA */
  volatile uint32_t jpeg_ring_chunks; /* Cumulative, never reset */
  volatile uint32_t jpeg_ring_over;   /* Spans longer than the ring */
  volatile uint32_t jpeg_resync;      /* Spans that needed SOI realignment */
  volatile uint32_t jpeg_no_soi;      /* Spans dropped for having no SOI */
  volatile uint32_t jpeg_finish_pending; /* EOF snapshots with FINISH pending */
  volatile uint32_t jpeg_pending_mask;   /* First 32 delivered frame indices */
  volatile int32_t jpeg_pending_delta;   /* raw delivered - hw byte_count */

  /* Set by any path that decided the pipeline is out of step, cleared by the
   * VSYNC handler once it has reset it.  The vendor keeps exactly one such
   * flag (dvp_driver_handle_t::error) and every producer of an error sets it
   * rather than recovering on the spot, so recovery always lands on a frame
   * boundary.  Before this, only the encoder's own err_count could trigger a
   * reset, and a frame lost to a missing EOI or an unrecognisable span left
   * the hardware running with whatever phase had caused it.
   */

  volatile bool jpeg_error;
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
 * Name: bk7258_camera_jpeg_sample_period
 *
 * Description:
 *   Work out how often this stream should deliver a frame, from the interval
 *   the application asked for and the build's ceiling.
 *
 *   CONFIG_BK7258_CAMERA_JPEG_FPS stays what its help text says it is -- an
 *   upper bound, defaulting to the rate the encoder can actually reach, so a
 *   build never throttles itself below the hardware.  What was missing is the
 *   other half of that argument: it says sampling "belongs to whoever is
 *   uploading", but the interval such a caller passes was accepted by the
 *   framework and then discarded here, so the only way to sample slower was
 *   to lower the ceiling for every application in the image.
 *
 *   That mattered once social mode existed.  It wants a few frames a second
 *   and paces its own dequeues accordingly, but the pacing was on the wrong
 *   side of the driver: every frame this module delivers costs a copy out of
 *   the drain ring plus, on the hardware path, a full 4:2:2 Huffman
 *   validation over non-cacheable PSRAM, and it was paying that at the
 *   ceiling rate while reading a fraction of the result.  Lowering the
 *   ceiling instead would have taken web_tool's live preview down with it,
 *   because both are the same /dev/video0 and the ceiling is one build-wide
 *   constant.
 *
 *   Zero or nonsense means "no request": the framework seeds the interval
 *   from the sensor's default and a caller that never issues VIDIOC_S_PARM
 *   should keep the behaviour it had.  A request above the ceiling is capped
 *   rather than refused, because the ceiling is a statement about the
 *   hardware and no caller can argue with it.
 *
 ****************************************************************************/

static void bk7258_camera_jpeg_sample_period(
    FAR struct bk7258_camera_imgdata_s *priv,
    FAR const imgdata_interval_t *interval)
{
  uint32_t fps = CONFIG_BK7258_CAMERA_JPEG_FPS;

  if (interval != NULL && interval->denominator != 0)
    {
      uint32_t requested = interval->denominator /
                           (interval->numerator != 0 ?
                            interval->numerator : 1u);

      if (requested != 0 && requested < fps)
        {
          fps = requested;
        }
    }

  priv->jpeg_fps = fps;

  /* At least one tick, so a rate the tick resolution cannot express degrades
   * to "every frame" instead of to a period of zero, which would compare as
   * already due and defeat the gate entirely.
   */

  priv->jpeg_sample_period = MSEC2TICK(1000u / fps);
  if (priv->jpeg_sample_period == 0)
    {
      priv->jpeg_sample_period = 1;
    }
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
 *   Points the drain channel at the slot the DMA is to own next and starts
 *   it.
 *
 *   Called at stream start, from the frame-done path and from the VSYNC
 *   recovery, so it must be safe in interrupt context: register writes only.
 *
 *   The destination loops over exactly one slot of the ring.  That bound is
 *   what keeps a runaway encoder from writing past it: a frame larger than a
 *   slot wraps and corrupts its own beginning, which the SOI/EOI checks then
 *   reject, instead of corrupting somebody else's memory.
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

  uint32_t slot_bytes = priv->jpeg_ring_bytes /
                        BK7258_CAMERA_JPEG_RING_SLOTS;
  uint32_t dest = (uint32_t)(uintptr_t)(
    priv->jpeg_ring + (uint32_t)priv->jpeg_ring_slot * slot_bytes);

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
  cfg.dest_loop_end   = dest + slot_bytes;
  cfg.data_width      = BK7258_DMA_WIDTH_32BITS;

  /* Burst lengths.  The vendor uses SINGLE on the FIFO side and INC16 into
   * memory (dvp_camera_dma_config(), CONFIG_SPE branch); a FIFO source must
   * stay SINGLE because its address does not advance, so only the
   * destination is a knob.
   */

  cfg.src_burst       = BK7258_DMA_BURST_SINGLE;
  cfg.dest_burst      = CONFIG_BK7258_CAMERA_JPEG_DMA_DEST_BURST;

  /* Cleared here because the delivered length of the next frame is counted
   * from this point: arming resets the destination to the top of the buffer.
   */

  priv->jpeg_frame_chunks = 0;
  priv->jpeg_ring_chunks = 0;
  priv->jpeg_ring_read = 0;

  if (bk7258_dma_configure_ex(&cfg) == 0)
    {
      bk7258_dma_start_channel(BK7258_CAMERA_JPEG_DMA_CH);
    }
}

/****************************************************************************
 * Name: bk7258_camera_jpeg_ring_write_pos
 *
 * Description:
 *   Where the DMA is currently writing, as an offset into the ring.
 *
 *   The destination address register cannot be used for this: it holds the
 *   programmed start and does not advance while the channel runs (measured --
 *   over six frames it read back exactly the base address every time).  So
 *   the position is reconstructed from the chunk interrupts, which is sound
 *   only because the channel is never restarted: the counter is cumulative
 *   for the whole session.
 *
 *   Interrupts are masked over the two reads because a chunk interrupt
 *   landing between them would mismatch the counter with remain_len and put
 *   the position out by a whole chunk.
 *
 ****************************************************************************/

static uint32_t bk7258_camera_jpeg_ring_write_pos(
    FAR struct bk7258_camera_imgdata_s *priv, FAR bool *finish_pending)
{
  irqstate_t flags = up_irq_save();
  uint32_t chunks;
  uint32_t remain;
  bool pending;
  uint64_t total;

  bk7258_dma_get_channel_progress(BK7258_CAMERA_JPEG_DMA_CH, &remain,
                                  &pending);
  chunks = priv->jpeg_ring_chunks;

  up_irq_restore(flags);

  if (finish_pending != NULL)
    {
      *finish_pending = pending;
    }

  if (remain > BK7258_CAMERA_JPEG_CHUNK)
    {
      remain = BK7258_CAMERA_JPEG_CHUNK;
    }

  /* Deliberately do not compensate for pending here in the diagnostic build.
   * The pending mask and delta printed at stop_capture prove or falsify the
   * one-chunk ownership hypothesis before the behavior is changed.
   */

  total = (uint64_t)chunks * BK7258_CAMERA_JPEG_CHUNK +
          (BK7258_CAMERA_JPEG_CHUNK - remain);

  return (uint32_t)(total % (priv->jpeg_ring_bytes /
                             BK7258_CAMERA_JPEG_RING_SLOTS));
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
  priv->jpeg_ring_chunks++;
}

/****************************************************************************
 * Name: bk7258_camera_jpeg_validate_work
 *
 * Description:
 *   Finish one hardware JPEG after the EOF handler copied it out of the DMA
 *   ring.  Exact Huffman/MCU validation walks the complete entropy stream;
 *   doing that in the JPEG ISR delayed DMA chunk accounting long enough to
 *   lose the next ring position (measured as one delivered frame followed by
 *   no_soi=151).  LPWORK keeps the ring's interrupt-side bookkeeping live.
 *
 *   The V4L2 buffer remains owned by the driver until capture_cb() runs, and
 *   EOF drops newer frames while jpeg_encoding is true, so this task has
 *   exclusive access to frame_buf without another allocation or copy.
 ****************************************************************************/

static void bk7258_camera_jpeg_validate_work(FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;
  uint32_t hdrlen;
  uint32_t frame_len;
  size_t entropy_len;
  clock_t validate_start;
  clock_t mark;
  int bitshift;
  int result = EIO;

  if (!priv->capturing || priv->frame_buf == NULL)
    {
      priv->jpeg_encoding = false;
      return;
    }

  mark = up_perf_gettime();
  hdrlen = (uint32_t)bk7258_jpeg_enc_write_header(priv->frame_buf,
                                                   BK7258_JPEG_ENC_PAD);
  priv->jpeg_hdr_cycles += (uint32_t)(up_perf_gettime() - mark);

  if (hdrlen == 0)
    {
      priv->jpeg_hdr_fail++;
      priv->jpeg_error = true;
    }
  else
    {
      entropy_len = (size_t)BK7258_JPEG_ENC_PAD + priv->jpeg_work_len -
                    2u - hdrlen;
      validate_start = clock_systime_ticks();

      if (entropy_len > priv->jpeg_validate_scratch_bytes)
        {
          bitshift = -ENOSPC;
        }
      else
        {
          mark = up_perf_gettime();
          memcpy(priv->jpeg_validate_scratch,
                 priv->frame_buf + hdrlen, entropy_len);
          priv->jpeg_scratch_cycles += (uint32_t)(up_perf_gettime() - mark);

          mark = up_perf_gettime();
          bitshift = bk7258_jpeg_realign_entropy_prefix(
            priv->jpeg_validate_scratch, &entropy_len,
            priv->jpeg_validate_scratch_bytes, priv->width, priv->height,
            CONFIG_BK7258_CAMERA_JPEG_VALIDATE_MCUS);
          priv->jpeg_realign_cycles += (uint32_t)(up_perf_gettime() - mark);

          if (bitshift > 0)
            {
              mark = up_perf_gettime();
              memcpy(priv->frame_buf + hdrlen,
                     priv->jpeg_validate_scratch, entropy_len);
              priv->jpeg_scratch_cycles +=
                (uint32_t)(up_perf_gettime() - mark);
            }
        }

      priv->jpeg_validate_ticks +=
        (uint32_t)(clock_systime_ticks() - validate_start);
      priv->jpeg_validate_runs++;

      if (bitshift < 0)
        {
          priv->jpeg_bit_fail++;
          priv->jpeg_error = true;
        }
      else
        {
          frame_len = hdrlen + (uint32_t)entropy_len + 2u;
          priv->frame_buf[frame_len - 2u] = 0xff;
          priv->frame_buf[frame_len - 1u] = 0xd9;

          if (bitshift > 0)
            {
              priv->jpeg_bit_fixed++;
            }

          if (priv->jpeg_work_finish_pending && priv->frames_done < 32u)
            {
              priv->jpeg_pending_mask |= 1u << priv->frames_done;
            }

          priv->frames_done++;
          result = 0;
        }
    }

  if (priv->capturing && priv->capture_cb != NULL)
    {
      priv->capture_cb(result, result == 0 ? frame_len : 0,
                       &priv->jpeg_work_ts, priv->capture_cb_arg);
    }

  /* Keep the guard set through capture_cb(): complete_capture() synchronously
   * supplies the next V4L2 buffer, and an EOF interrupt inside that handoff
   * must still drop rather than write into the buffer being returned.
   */

  priv->jpeg_encoding = false;

  if (priv->capturing)
    {
      bk7258_camera_watchdog_arm(priv);
    }
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

  /* The bitstream is copied to PAD bytes into the V4L2 buffer, leaving room
   * for the standards-conforming header, so every offset below -- delivered,
   * the EOI search, the recorded tail -- is relative to that, not to the
   * buffer.
   */

  FAR uint8_t *buf = priv->frame_buf == NULL ? NULL :
                     priv->frame_buf + BK7258_JPEG_ENC_PAD;
  FAR uint8_t *raw;
  uint32_t slot_bytes;
  uint32_t capacity;
  uint32_t write_pos;
  struct timeval ts;
  uint32_t len = 0;
  uint32_t delivered;
  uint32_t i;
  bool finish_pending;

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

  if (i > priv->jpeg_drain_spins_max)
    {
      priv->jpeg_drain_spins_max = i;
    }

  priv->jpeg_drain_spins_sum += i;

  if (i >= BK7258_CAMERA_JPEG_DRAIN_SPINS)
    {
      /* The FIFO never reported empty.  The frame is very likely short, so
       * let the VSYNC handler reset rather than trusting the span.
       */

      priv->jpeg_drain_timeouts++;
      priv->jpeg_error = true;
    }

  /* Match the vendor frame boundary: flush the partial DMA word, stop the
   * channel, snapshot its completed slot, and restart into a fresh slot
   * before doing any copy or validation.  Earlier stop/re-arm attempts used
   * the same destination and waited for V4L2, losing bytes produced in that
   * gap; slot switching has neither failure mode.
   */

  bk7258_dma_flush_src_buffer(BK7258_CAMERA_JPEG_DMA_CH);
  bk7258_dma_stop_channel(BK7258_CAMERA_JPEG_DMA_CH);

  write_pos = bk7258_camera_jpeg_ring_write_pos(priv, &finish_pending);
  slot_bytes = priv->jpeg_ring_bytes / BK7258_CAMERA_JPEG_RING_SLOTS;
  raw = priv->jpeg_ring + (uint32_t)priv->jpeg_ring_slot * slot_bytes;
  delivered = write_pos;
  priv->jpeg_ring_read = 0;

  priv->jpeg_ring_slot = (uint8_t)(
    ((unsigned int)priv->jpeg_ring_slot + 1u) %
    BK7258_CAMERA_JPEG_RING_SLOTS);
  bk7258_camera_jpeg_dma_arm(priv);

  if (finish_pending)
    {
      priv->jpeg_finish_pending++;
      priv->jpeg_pending_delta = (int32_t)delivered - (int32_t)bytes;
    }

  /* Resynchronise on the frame's own SOI.
   *
   * The read pointer is left at the write position when a frame is delivered
   * or skipped, but the encoder's last bytes for that frame reach memory
   * *after* that -- the FIFO drain is not instantaneous however long the
   * handler spins.  Those leftovers become the head of the next span, so the
   * next frame's entropy data starts a few bytes late.
   *
   * The effect is nasty precisely because it still decodes: every marker is
   * present (the header is synthesised) and the picture looks plausible, but
   * its content is displaced -- measured as a best match at 32 and 56 rows
   * of vertical offset with correlation 0.5-0.6, against 1.00 and zero offset
   * for a good frame -- and its DC and chroma come out wrong, which shows up
   * as an over-bright, colour-shifted frame roughly one in three.
   *
   * Every frame this block emits begins SOI + APP0, so skipping to the first
   * FF D8 in the span is an exact resynchronisation.
   */

  if (delivered >= 4u)
    {
      uint32_t scan;
      uint32_t limit = delivered - 1u < BK7258_CAMERA_JPEG_SOI_SCAN ?
                       delivered - 1u : BK7258_CAMERA_JPEG_SOI_SCAN;

      for (scan = 0; scan < limit; scan++)
        {
          if (raw[scan] == 0xffu && raw[scan + 1u] == 0xd8u)
            {
              break;
            }
        }

      if (scan < limit)
        {
          priv->jpeg_ring_read = scan;
          delivered -= scan;
          priv->jpeg_resync += scan != 0 ? 1u : 0u;
        }
      else
        {
          /* No frame start in the window: drop the span rather than deliver
           * a displaced picture.
           */

          priv->jpeg_no_soi++;
          priv->jpeg_error = true;
          priv->jpeg_ring_read = write_pos;

          if (priv->capturing)
            {
              bk7258_camera_watchdog_arm(priv);
            }

          return;
        }
    }

  /* The vendor's integrity check, on the span that is about to be delivered.
   *
   * dvp_camera_jpeg_eof_handler() compares what the DMA moved against the
   * hardware's own byte_count_pfrm and, when they disagree by anything short
   * of a whole chunk, flags the frame and lets the next VSYNC reset the
   * pipeline.  It costs one subtraction and catches the class of fault a
   * Huffman walk cannot see: a span that lost bytes, or picked up a
   * neighbour's.
   */

  priv->jpeg_recon_delta = (int32_t)delivered - (int32_t)bytes -
                           (int32_t)BK7258_JPEG_ENC_CRC_SIZE;

  if (priv->jpeg_recon_delta > priv->jpeg_recon_worst ||
      -priv->jpeg_recon_delta > priv->jpeg_recon_worst)
    {
      priv->jpeg_recon_worst = priv->jpeg_recon_delta < 0 ?
                               -priv->jpeg_recon_delta :
                               priv->jpeg_recon_delta;
    }

  if (priv->jpeg_recon_delta > BK7258_CAMERA_JPEG_RECON_SLACK ||
      priv->jpeg_recon_delta < -BK7258_CAMERA_JPEG_RECON_SLACK)
    {
      priv->jpeg_recon_bad++;

#ifdef CONFIG_BK7258_CAMERA_JPEG_RECONCILE
      priv->jpeg_error = true;
      priv->jpeg_ring_read = write_pos;

      if (priv->capture_cb != NULL)
        {
          bk7258_camera_now(&ts);
          priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
        }

      if (priv->capturing)
        {
          bk7258_camera_watchdog_arm(priv);
        }

      return;
#endif
    }

  capacity = priv->frame_buf_size > BK7258_JPEG_ENC_PAD ?
             priv->frame_buf_size - BK7258_JPEG_ENC_PAD : 0;

  if (delivered > capacity)
    {
      /* A frame bigger than the V4L2 buffer, or a span the ring wrapped
       * over.  Either way the data is not trustworthy; resynchronise on the
       * current position rather than deliver part of a frame.
       */

      priv->jpeg_ring_over++;
      priv->jpeg_error = true;
      priv->jpeg_ring_read = write_pos;


      if (priv->capture_cb != NULL)
        {
          bk7258_camera_now(&ts);
          priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
        }

      if (priv->capturing)
        {
          bk7258_camera_watchdog_arm(priv);
        }

      return;
    }

  if (priv->jpeg_encoding)
    {
      /* Exact entropy validation owns frame_buf until its LPWORK completion.
       * The DMA ring is independent and must keep advancing; dropping this
       * newest frame is cheaper and safer than queuing work behind an old
       * V4L2 buffer.
       */

      priv->jpeg_ring_read = write_pos;
      priv->jpeg_hw_busy++;

      if (priv->capturing)
        {
          bk7258_camera_watchdog_arm(priv);
        }

      return;
    }

  /* Fixed sampling rate, applied before the copy.
   *
   * The encoder runs at the sensor rate whatever we do, so pacing happens by
   * choosing which completed frames to deliver.  Skipping here rather than
   * after the copy also keeps the interrupt handler cheap: at 5 FPS five out
   * of six frames cost only a pointer update instead of a 25KB memcpy.
   *
   * Advancing the read pointer past a skipped frame is what makes the next
   * delivered frame the newest one rather than a queued old one -- the
   * spec's drop-the-oldest rule.
   */

  if ((sclock_t)(clock_systime_ticks() - priv->jpeg_next_sample) < 0)
    {
      /* Discard the frame by moving the read pointer past it.  Leaving the
       * pointer where it was instead makes the skipped frames pile up in the
       * ring, and the next delivered span then covers all of them: measured
       * as a 150611-byte "frame" and ring_over=3 at 5 FPS before this line
       * existed.
       */

      priv->jpeg_ring_read = write_pos;
      priv->jpeg_sw_skipped++;

      if (priv->capturing)
        {
          bk7258_camera_watchdog_arm(priv);
        }

      return;
    }

  priv->jpeg_next_sample = clock_systime_ticks() + priv->jpeg_sample_period;

  /* The completed slot is frozen and one frame is smaller than a slot, so
   * this copy cannot wrap or race the DMA now writing the next slot.
   */

  if (delivered > 0)
    {
      clock_t mark = up_perf_gettime();

      memcpy(buf, raw + priv->jpeg_ring_read, delivered);
      priv->jpeg_copy_cycles += (uint32_t)(up_perf_gettime() - mark);
    }

  priv->jpeg_ring_read = write_pos;


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
      priv->jpeg_error = true;

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
      /* Keep the V4L2 buffer owned and hand the complete raw frame to
       * task context.  All ring state was advanced before this point, so
       * subsequent DMA/EOF interrupts can continue independently.
       */

      priv->jpeg_work_len = len;
      priv->jpeg_work_finish_pending = finish_pending;
      priv->jpeg_work_ts = ts;
      priv->jpeg_encoding = true;

      if (work_queue(LPWORK, &priv->jpeg_work,
                     bk7258_camera_jpeg_validate_work, priv, 0) < 0)
        {
          priv->jpeg_encoding = false;
          priv->jpeg_bit_fail++;

          if (priv->capture_cb != NULL)
            {
              priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
            }
        }
    }

  if (priv->capturing)
    {
      /* Nothing to re-arm: the channel runs for the whole session.  An
       * earlier version re-armed here, and an earlier one still waited for
       * set_buf() to do it -- which never came for a frame completed with an
       * error, so the channel died on the first bad frame (both runs stopped
       * at exactly 7 chunks, ~72KB, whatever the frame rate).
       */

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

  /* Two sources, one recovery.  The encoder's own frame_err counter is what
   * this handler was built on; jpeg_error is everything the drain side
   * decided was out of step -- a span with no SOI, a length the hardware
   * disagrees with, a frame that never showed an end-of-image marker.  The
   * vendor funnels all of them into a single flag for exactly this reason
   * (dvp_driver_handle_t::error, cleared in the vendor's own
   * dvp_camera_vsync_negedge_handler()): recovering anywhere other than a
   * frame boundary reproduces the phase problem it is trying to clear.
   */

  if (st.err_count == priv->jpeg_err_seen && !priv->jpeg_error)
    {
      return;
    }

  priv->jpeg_err_seen = st.err_count;
  priv->jpeg_error = false;
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
 * Name: bk7258_camera_set_sw_jpeg
 *
 * Description:
 *   Register the software JPEG encoder used as the V4L2_PIX_FMT_JPEG
 *   correctness fallback.  With CONFIG_BK7258_CAMERA_HW_JPEG, formats up to
 *   640 pixels wide use validated hardware output and wider formats use this
 *   encoder; without it, all JPEG formats use software.
 *
 *   Why a hook rather than a direct call: the encoder is libjpeg-turbo, which
 *   lives in the apps tree and is linked by the board layer
 *   (boards/.../src/bk7258_jpeg_enc.c).  Calling it from here would make this
 *   chip driver depend on both, inverting the direction every other file in
 *   this tree follows (board calls chip).  The board registers itself at
 *   bring-up instead.
 *
 *   Why software remains registered: the repaired hardware path is decoded-
 *   pixel verified at 480x480 and 640x480, and rejects structurally invalid
 *   entropy fail-closed.  At 864x480 its syntax validated but decoded pixels
 *   were unstable, so that width deliberately uses the slower, correct
 *   software encoder.
 *
 ****************************************************************************/

static bk7258_camera_sw_jpeg_t g_sw_jpeg;

void bk7258_camera_set_sw_jpeg(bk7258_camera_sw_jpeg_t encoder)
{
  g_sw_jpeg = encoder;
}

/****************************************************************************
 * Name: bk7258_camera_sw_jpeg_work
 *
 * Description:
 *   Encode the staged raw frame into the application's buffer and complete
 *   it.  Work-queue context, so printf and long work are both allowed.
 *
 ****************************************************************************/

static void bk7258_camera_sw_jpeg_work(FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;
  struct timeval ts;
  int len;

  if (!priv->capturing || priv->frame_buf == NULL || g_sw_jpeg == NULL)
    {
      priv->jpeg_encoding = false;
      priv->jpeg_buf_armed = priv->capturing && priv->frame_buf != NULL;
      return;
    }

  /* Capacity comes from set_buf(), not from frame_bytes: start_capture()
   * deliberately zeroes frame_bytes for encoded formats (the length is
   * whatever the encoder produces, so there is no per-frame size), and the
   * first frame of a session used to be encoded with dstlen == 0 and
   * completed with V4L2_BUF_FLAG_ERROR -- visible to the application as
   * "frame 0: bytesused=0 flags=0x00000040".
   */

  /* Every exit from here that does not complete a buffer has to give the
   * arm back.
   *
   * bk7258_camera_frame_done() clears jpeg_buf_armed when it queues this
   * work, on the assumption that the work will consume the buffer -- either
   * by delivering a frame or by completing it with an error.  An exit that
   * does neither leaves the driver with armed == false and the framework
   * with a buffer it believes is still being filled, and nothing ever
   * re-arms: the framework only calls set_buf() after a completion.  The
   * session then delivers exactly one frame and stalls, which is what
   * "no frame within 5000 ms" was, with set_buf_live=1 armed=0 encoding=0.
   */

  if (priv->frame_buf_size == 0)
    {
      priv->jpeg_encoding = false;
      priv->jpeg_buf_armed = priv->capturing;
      return;
    }

  if (priv->jpeg_raw_ready < 0 ||
      priv->jpeg_raw[priv->jpeg_raw_ready] == NULL)
    {
      priv->jpeg_encoding = false;
      priv->jpeg_buf_armed = priv->capturing;
      return;
    }

  len = g_sw_jpeg(priv->jpeg_raw[priv->jpeg_raw_ready], priv->jpeg_raw_bytes,
                  priv->frame_buf, priv->frame_buf_size,
                  priv->width, priv->height);

  bk7258_camera_now(&ts);

  if (len > 0 && priv->jpeg_sw_frames == 0)
    {
      /* First frame of the session only: prove the encoded file is in the
       * buffer the application will read, not merely reported.
       */

      printf("bk7258_camera_imgdata: sw_jpeg first frame len=%d "
             "dst[0..3]=%02x %02x %02x %02x tail=%02x %02x\n",
             len, priv->frame_buf[0], priv->frame_buf[1],
             priv->frame_buf[2], priv->frame_buf[3],
             priv->frame_buf[len - 2], priv->frame_buf[len - 1]);
    }

  if (len > 0)
    {
      priv->jpeg_sw_frames++;
      priv->frames_done++;

      if (priv->capture_cb != NULL)
        {
          priv->capture_cb(0, (uint32_t)len, &ts, priv->capture_cb_arg);
        }
    }
  else
    {
      if (priv->capture_cb != NULL)
        {
          priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
        }
    }

  priv->jpeg_encoding = false;
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
      /* Software JPEG: the frame really is in memory (YUV_BUF's own writer is
       * on in this mode), so hand it to the work queue.  Frames that arrive
       * while an encode is still running are dropped rather than queued: at
       * ~270ms per encode and 33ms per frame there would always be a backlog,
       * and the newest frame is the one worth having.
       */

      if (priv->jpeg_software)
        {
          /* Encode only when the framework is actually waiting for a frame.
           * Encoding on every frame instead wasted most of the work: the
           * first version encoded 141 frames in a session where the
           * application received 6, because a completion with no queued
           * buffer is discarded -- and each of those is ~270ms of CPU.
           */

          clock_t now = clock_systime_ticks();

          /* The frame that just completed is the newest one; hand the
           * hardware the other buffer and remember this one.  If an older
           * unprocessed frame was sitting here it is simply forgotten, which
           * is the drop-the-oldest rule.
           */

          if (priv->jpeg_raw_ready == (int8_t)priv->jpeg_raw_fill)
            {
              priv->jpeg_sw_drops++;
            }

          priv->jpeg_raw_ready = (int8_t)priv->jpeg_raw_fill;
          priv->jpeg_raw_fill ^= 1u;

          if (priv->jpeg_raw[priv->jpeg_raw_fill] != NULL)
            {
              bk7258_yuv_buf_set_frame_buffer(
                (uint32_t)(uintptr_t)priv->jpeg_raw[priv->jpeg_raw_fill]);
            }

          /* Fixed sampling rate, not "as fast as the encoder manages": the
           * product spec fixes the rate so cloud-side timestamps line up, and
           * a free-running encoder would also hold the low-priority work
           * queue continuously.
           */

          priv->jpeg_last_now = now;

          if (priv->jpeg_encoding)
            {
              priv->jpeg_sw_busy++;
            }
          else if (!priv->jpeg_buf_armed)
            {
              priv->jpeg_sw_nobuf++;
            }
          else if ((sclock_t)(now - priv->jpeg_next_sample) < 0)
            {
              priv->jpeg_sw_skipped++;
            }
          else
            {
              /* Fixed period from the start of the encode, so the wait overlaps
               * the encode rather than following it.  Measuring from the
               * completion instead was tried and measured worse: 480x480 fell
               * from 4.39 to 2.26 fps, because encode and wait then add up.
               */

              priv->jpeg_next_sample = now + priv->jpeg_sample_period;
              priv->jpeg_encoding = true;
              priv->jpeg_buf_armed = false;
              work_queue(LPWORK, &priv->jpeg_work,
                         bk7258_camera_sw_jpeg_work, priv, 0);
            }
        }

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
  priv->jpeg_software  = false;
  priv->jpeg_chunks    = 0;
  priv->jpeg_short     = 0;
  priv->jpeg_err_seen  = 0;
  priv->jpeg_resets    = 0;
  priv->jpeg_stage     = NULL;
  priv->jpeg_stage_bytes = 0;
  priv->jpeg_validate_scratch = NULL;
  priv->jpeg_validate_scratch_bytes = 0;
  priv->jpeg_validate_ticks = 0;
  priv->jpeg_validate_runs = 0;
  priv->jpeg_hdr_fail  = 0;
  priv->jpeg_bit_fixed = 0;
  priv->jpeg_bit_fail  = 0;
  priv->jpeg_hw_busy   = 0;
  priv->jpeg_work_len  = 0;

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
      /* Both JPEG paths use jpeg_work and retain frame_buf until it exits.
       * Cancel before the framework can unmap that buffer.
       */

      work_cancel(LPWORK, &priv->jpeg_work);
      priv->jpeg_encoding = false;
    }

  if (priv->jpeg && !priv->jpeg_software)
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

  /* Separately count the calls that arrive while streaming.  set_buf_calls
   * alone cannot answer the question that matters when a session delivers
   * one frame and then stalls: whether the framework ever re-armed the
   * driver after a completion, or whether it did and the sampling gate
   * dropped the frame anyway.  The pre-STREAMON queueing is indis-
   * tinguishable from a re-arm in the total.
   */

  if (priv->capturing)
    {
      priv->set_buf_live++;
    }

  /* Point the hardware's frame writer at this buffer.  Done on every
   * call, which is also how the next frame gets re-armed from the
   * frame-done path (reference: dvp_camera_yuv_eof_handler()'s
   * bk_yuv_buf_set_em_base_addr()).
   *
   * In JPEG mode the module writes no frames at all -- the encoder's output
   * is drained by DMA -- so the equivalent re-arm is pointing that channel
   * at the new buffer and restarting it.
   */

  if (priv->jpeg && priv->jpeg_software)
    {
      /* Software JPEG: the module's frame writer stays on the staging frame,
       * and the application's buffer only ever receives the encoded file
       * from the work queue.  Arming the hardware drain channel here -- what
       * the branch below does -- pointed a DMA from the (idle) encoder FIFO
       * straight at the application's buffer, which is how the first
       * attempt came back as a valid JPEG header followed by 0xff filler.
       */

      if (priv->jpeg_raw[priv->jpeg_raw_fill] != NULL)
        {
          bk7258_yuv_buf_set_frame_buffer(
            (uint32_t)(uintptr_t)priv->jpeg_raw[priv->jpeg_raw_fill]);
        }

      /* The software encoder writes the JPEG from byte 0 and has the whole
       * buffer, so give it the real capacity rather than the drain window
       * computed above: that window reserves BK7258_JPEG_ENC_PAD for the
       * header the hardware path has to graft on, and rounds down to the
       * drain channel's transfer_len -- neither applies here.
       */

      priv->frame_buf_size = size;
      priv->jpeg_buf_armed = true;
    }
  else if (priv->jpeg)
    {
      /* Nothing to do: the drain writes into the ring, not into the
       * application's buffer, so a new buffer does not touch the DMA.  This
       * used to re-arm the channel here, which is exactly what broke the
       * bitstream.
       */
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

#ifdef CONFIG_BK7258_CAMERA_HW_JPEG
  priv->jpeg_software = priv->jpeg && g_sw_jpeg != NULL && w > 640u;
#else
  priv->jpeg_software = priv->jpeg && g_sw_jpeg != NULL;
#endif

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

  /* Ahead of the software/hardware split below, because both pace themselves
   * from this and only one of them is taken.
   */

  bk7258_camera_jpeg_sample_period(priv, interval);

  bk7258_yuv_buf_configure(priv->width, priv->height);

  if (priv->jpeg && priv->jpeg_software)
    {
      /* Software JPEG: capture UYVY exactly as the raw path does, into a
       * staging frame of our own -- the application's buffer is sized for a
       * JPEG file, roughly a tenth of a raw frame -- and let the work queue
       * encode it.  The hardware JPEG block stays out of the way entirely.
       */

      priv->jpeg_raw_bytes = (uint32_t)priv->width * priv->height * 2u;

      if (priv->jpeg_raw[0] == NULL)
        {
          priv->jpeg_raw[0] = bk7258_media_pool_alloc(
              BK7258_PSRAM_POOL_DISPLAY, 32, priv->jpeg_raw_bytes);
        }

      if (priv->jpeg_raw[0] == NULL)
        {
          printf("bk7258_camera_imgdata: start_capture: no PSRAM for the "
                 "%u-byte software-JPEG staging frame\n",
                 (unsigned int)priv->jpeg_raw_bytes);
          return -ENOMEM;
        }

      priv->jpeg_raw_fill = 0;
      priv->jpeg_raw_ready = -1;
      priv->jpeg_next_sample = clock_systime_ticks();

      priv->jpeg_encoding = false;
      priv->jpeg_sw_frames = 0;
      priv->jpeg_sw_drops = 0;
      priv->jpeg_sw_skipped = 0;
      priv->jpeg_sw_busy = 0;
      priv->jpeg_sw_nobuf = 0;
      priv->set_buf_live = 0;
      priv->jpeg_buf_armed = priv->frame_buf != NULL &&
                             priv->frame_buf_size != 0;

      bk7258_yuv_buf_set_frame_buffer((uint32_t)(uintptr_t)priv->jpeg_raw[0]);

      priv->capturing = true;
      priv->start_ticks = clock_systime_ticks();
      bk7258_yuv_buf_start();
    }
  else if (priv->jpeg)
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

      /* The drain ring.  Sized to several frames so a frame is never
       * overwritten before the EOF handler has copied it out; the copy
       * happens immediately, so this is slack, not a requirement.
       */

      priv->jpeg_ring_bytes = BK7258_CAMERA_JPEG_RING_CHUNKS *
                              BK7258_CAMERA_JPEG_CHUNK;

      if (priv->jpeg_ring == NULL)
        {
          priv->jpeg_ring = bk7258_media_pool_alloc(
              BK7258_PSRAM_POOL_DISPLAY, 32, priv->jpeg_ring_bytes);
        }

      if (priv->jpeg_ring == NULL)
        {
          printf("bk7258_camera_imgdata: start_capture: no PSRAM for the "
                 "%u-byte JPEG drain ring\n",
                 (unsigned int)priv->jpeg_ring_bytes);
          return -ENOMEM;
        }

      if (priv->jpeg_validate_scratch == NULL)
        {
          priv->jpeg_validate_scratch =
            kmm_malloc(BK7258_CAMERA_JPEG_VALIDATE_BYTES);
          priv->jpeg_validate_scratch_bytes =
            priv->jpeg_validate_scratch == NULL ? 0 :
            BK7258_CAMERA_JPEG_VALIDATE_BYTES;
        }

      if (priv->jpeg_validate_scratch == NULL)
        {
          printf("bk7258_camera_imgdata: start_capture: no internal RAM for "
                 "the %u-byte JPEG validator scratch\n",
                 BK7258_CAMERA_JPEG_VALIDATE_BYTES);
          return -ENOMEM;
        }

      priv->jpeg_validate_ticks = 0;
      priv->jpeg_validate_runs = 0;
      priv->jpeg_ring_over = 0;
      priv->jpeg_ring_slot = 0;
      priv->jpeg_resync = 0;
      priv->jpeg_no_soi = 0;
      priv->jpeg_finish_pending = 0;
      priv->jpeg_pending_mask = 0;
      priv->jpeg_pending_delta = 0;
      priv->jpeg_bit_fixed = 0;
      priv->jpeg_bit_fail = 0;
      priv->jpeg_hw_busy = 0;
      priv->jpeg_sw_skipped = 0;
      priv->jpeg_error = false;
      priv->jpeg_copy_cycles = 0;
      priv->jpeg_hdr_cycles = 0;
      priv->jpeg_scratch_cycles = 0;
      priv->jpeg_realign_cycles = 0;
      priv->jpeg_drain_spins_max = 0;
      priv->jpeg_drain_spins_sum = 0;
      priv->jpeg_drain_timeouts = 0;
      priv->jpeg_recon_delta = 0;
      priv->jpeg_recon_worst = 0;
      priv->jpeg_recon_bad = 0;

      /* Seeded to "due now" so the first frame of a session is never delayed
       * by the pacing; the period itself was computed before the split above.
       */

      priv->jpeg_next_sample = clock_systime_ticks();

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
      priv->jpeg_start_cycles = up_perf_gettime();
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

  if (priv->jpeg_ring != NULL && !up_interrupt_context())
    {
      bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY, priv->jpeg_ring);
      priv->jpeg_ring = NULL;
    }

  if (priv->jpeg_validate_scratch != NULL && !up_interrupt_context())
    {
      kmm_free(priv->jpeg_validate_scratch);
      priv->jpeg_validate_scratch = NULL;
      priv->jpeg_validate_scratch_bytes = 0;
    }

  if (!up_interrupt_context())
    {
      int slot;

      for (slot = 0; slot < 2; slot++)
        {
          if (priv->jpeg_raw[slot] != NULL)
            {
              bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY,
                                     priv->jpeg_raw[slot]);
              priv->jpeg_raw[slot] = NULL;
            }
        }
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

      printf("bk7258_camera_imgdata: stop_capture: ring_over=%u resync=%u "
             "no_soi=%u sampler_skipped=%u hw_busy=%u (target %u fps)\n",
             (unsigned int)priv->jpeg_ring_over,
             (unsigned int)priv->jpeg_resync,
             (unsigned int)priv->jpeg_no_soi,
             (unsigned int)priv->jpeg_sw_skipped,
             (unsigned int)priv->jpeg_hw_busy,
             (unsigned int)priv->jpeg_fps);

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

          if (priv->jpeg_software)
            {
              printf("bk7258_camera_imgdata: stop_capture: sw_jpeg "
                     "frames=%u dropped_oldest=%u sampler_skipped=%u "
                     "busy=%u nobuf=%u due_in=%ld "
                     "set_buf_live=%u armed=%d encoding=%d "
                     "(target %u fps)\n",
                     (unsigned int)priv->jpeg_sw_frames,
                     (unsigned int)priv->jpeg_sw_drops,
                     (unsigned int)priv->jpeg_sw_skipped,
                     (unsigned int)priv->jpeg_sw_busy,
                     (unsigned int)priv->jpeg_sw_nobuf,
                     (long)((sclock_t)(priv->jpeg_next_sample -
                                       priv->jpeg_last_now)),
                     (unsigned int)priv->set_buf_live,
                     (int)priv->jpeg_buf_armed,
                     (int)priv->jpeg_encoding,
                     (unsigned int)priv->jpeg_fps);
            }

          printf("bk7258_camera_imgdata: stop_capture: jpeg chunks=%u "
                 "short=%u resets=%u vsyncs=%u hdr_fail=%u "
                 "bit_fixed=%u bit_fail=%u eoi_delta=%d "
                 "dma_remain=%u finish_pending=%u pending_mask=0x%08x "
                 "pending_delta=%d\n",
                 (unsigned int)priv->jpeg_chunks,
                 (unsigned int)priv->jpeg_short,
                 (unsigned int)priv->jpeg_resets,
                 (unsigned int)priv->jpeg_vsyncs,
                 (unsigned int)priv->jpeg_hdr_fail,
                 (unsigned int)priv->jpeg_bit_fixed,
                 (unsigned int)priv->jpeg_bit_fail,
                 (int)priv->jpeg_eoi_delta,
                 (unsigned int)bk7258_dma_get_channel_remain_len(
                     BK7258_CAMERA_JPEG_DMA_CH),
                 (unsigned int)priv->jpeg_finish_pending,
                 (unsigned int)priv->jpeg_pending_mask,
                 (int)priv->jpeg_pending_delta);
          printf("bk7258_camera_imgdata: stop_capture: validator runs=%u "
                 "total_ticks=%u avg=%u ms\n",
                 (unsigned int)priv->jpeg_validate_runs,
                 (unsigned int)priv->jpeg_validate_ticks,
                 (unsigned int)(priv->jpeg_validate_runs ?
                   TICK2MSEC(priv->jpeg_validate_ticks) /
                   priv->jpeg_validate_runs : 0u));

          /* Cycle-resolution breakdown, and the core clock it implies.
           *
           * Reported per delivered frame so the four numbers add up to
           * something comparable with the validator average above, and as a
           * derived frequency so that CONFIG_BK7258_CPU_FREQ_HZ -- which
           * only ever reached systick -- can be checked against the
           * hardware rather than believed.
           *
           * Only for the hardware path: everything below is maintained by
           * the drain and validation code, so on a software session it would
           * print whatever the last hardware session left behind.
           */

          if (!priv->jpeg_software && priv->jpeg_validate_runs != 0)
            {
              uint32_t runs = priv->jpeg_validate_runs;

              printf("bk7258_camera_imgdata: stop_capture: cycles/frame "
                     "copy=%u hdr=%u scratch=%u realign=%u "
                     "(validate_mcus=%d)\n",
                     (unsigned int)(priv->jpeg_copy_cycles / runs),
                     (unsigned int)(priv->jpeg_hdr_cycles / runs),
                     (unsigned int)(priv->jpeg_scratch_cycles / runs),
                     (unsigned int)(priv->jpeg_realign_cycles / runs),
                     CONFIG_BK7258_CAMERA_JPEG_VALIDATE_MCUS);
            }

          if (!priv->jpeg_software && ms != 0)
            {
              uint32_t elapsed_cycles =
                (uint32_t)(up_perf_gettime() - priv->jpeg_start_cycles);

              printf("bk7258_camera_imgdata: stop_capture: core clock "
                     "measured=%u kHz configured=%u kHz\n",
                     (unsigned int)(elapsed_cycles / ms),
                     (unsigned int)(CONFIG_BK7258_CPU_FREQ_HZ / 1000));
            }

          if (!priv->jpeg_software)
            {
              printf("bk7258_camera_imgdata: stop_capture: drain spins "
                     "max=%u avg=%u timeouts=%u limit=%u\n",
                     (unsigned int)priv->jpeg_drain_spins_max,
                     (unsigned int)(priv->jpeg_vsyncs ?
                       priv->jpeg_drain_spins_sum / priv->jpeg_vsyncs : 0u),
                     (unsigned int)priv->jpeg_drain_timeouts,
                     (unsigned int)BK7258_CAMERA_JPEG_DRAIN_SPINS);

              printf("bk7258_camera_imgdata: stop_capture: reconcile "
                     "last=%d worst=%d outside_window=%u slack=%d "
                     "enforced=%d\n",
                     (int)priv->jpeg_recon_delta,
                     (int)priv->jpeg_recon_worst,
                     (unsigned int)priv->jpeg_recon_bad,
                     BK7258_CAMERA_JPEG_RECON_SLACK,
                     BK7258_CAMERA_JPEG_RECON_ENFORCED);
            }

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
