/****************************************************************************
 * board/beken/chips/bk7258/bk7258_jpeg_enc.c
 *
 * BK7258 hardware JPEG encoder, register layer only.  Register layout
 * source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/jpeg_struct.h and
 * ap/middleware/soc/bk7258_ap/hal/jpeg_ll.h; initialisation order from
 * ap/middleware/soc/common/hal/jpeg_hal.c jpeg_hal_switch_mode()'s
 * JPEG_MODE branch plus ap/middleware/driver/jpeg_enc/jpeg_driver.c
 * jpeg_init_common().
 *
 * Register map (byte offset from BK7258_JPEG_ENC_BASE):
 *   dev_id           0x00 - read-only module identity
 *   dev_version      0x04 - read-only module version
 *   global_ctrl      0x08 - bit[0] soft_reset, bit[1] clk_gate_bypass
 *   dev_status       0x0c - read-only module status
 *   eof_offset       0x10 - bit[16:31] eof_offset
 *   rx_fifo_data     0x14 - encoded-stream FIFO read port (DMA source)
 *   int_status       0x18 - bit[0] sof, bit[1] eof, bit[2] head output,
 *                            bit[3] frame error, bit[5] line clear,
 *                            bit[6] fifo_rd_finish (write-1-to-clear)
 *   byte_count_pfrm  0x1c - encoded byte count of the last frame
 *   fifo_status      0x20 - bit[26] stream_fifo_empty,
 *                            bit[27] stream_fifo_full, bit[28] is_data_set
 *   y_count          0x24 - bit[24:31] y_count
 *   byte_cnt_line    0x28 - encoded byte count of the current line
 *   int_en           0x30 - bit[0] frame_err, bit[1] head, bit[2] sof,
 *                            bit[3] eof, bit[7] line_clear
 *   cfg              0x34 - bit[1] video_byte_reverse, bit[4] jpeg_enc_en,
 *                            bit[8:15] x_pixel, bit[16] jpeg_enc_size,
 *                            bit[17] bitrate_ctrl, bit[18:19] bitrate_step,
 *                            bit[20] auto_step, bit[23] bitrate_mode,
 *                            bit[24:31] y_pixel
 *   target_byte_h    0x38 - rate-control upper bound, bytes per frame
 *   target_byte_l    0x3c - rate-control lower bound, bytes per frame
 *   quant_table      0x80 - 32 words of quantisation table
 *
 * NOTE the int_en bit order is NOT the int_status bit order: eof is
 * int_en bit[3] but int_status bit[1], sof is int_en bit[2] but
 * int_status bit[0].  Evidence: jpeg_ll_enable_end_frame_int() sets
 * int_en.eof_int_en (struct bit[3]) while
 * jpeg_ll_is_frame_end_int_triggered() tests int_status BIT(1).
 *
 * x_pixel/y_pixel are width/height divided by 8, exactly as for YUV_BUF
 * (ap/components/bk_dvp/src/bk_dvp.c dvp_camera_jpeg_config_init():
 * "jpeg_config.x_pixel = config->width / 8").
 *
 * Power/clock: this module needs the shared video-pipeline power domain
 * on and its own clock gate open, both in the system controller's
 * register space, before any of its own registers have real effect
 * (jpeg_driver.c jpeg_init_common()).  Note the JPEG clock gate is in a
 * *different* sysctrl register than YUV_BUF's: sys_hal_set_jpeg_clk_en()
 * writes cpu_device_clk_enable.jpeg_cken (sysctrl word 0x0c bit[28]),
 * whereas YUV_BUF uses reserver_reg0xd.yuv_cken (word 0x0d bit[3]).
 * There is also a reserver_reg0xd.jpeg_cken at word 0x0d bit[8] which
 * the vendor code never touches -- do not confuse the two.
 *
 * Output path: unlike YUV_BUF this module has no destination-address
 * register; the encoded bytes must be pulled out of rx_fifo_data by DMA
 * (DMA_DEV_JPEG).  See include/bk7258_jpeg_enc.h for the citations.
 *
 * BK7258 deltas worth knowing when comparing against jpeg_hal.c: on this
 * chip jpeg_ll_enable_sync_edge_dect(), jpeg_ll_enable_vsync_reverse(),
 * jpeg_ll_enable_hsync_reverse(), jpeg_ll_yuv_fml_sel() and
 * jpeg_ll_set_mclk_div() are all empty "// not support" stubs
 * (ap/middleware/soc/bk7258_ap/hal/jpeg_ll.h).  Sync polarity, sensor
 * byte order and the sensor MCLK divider are therefore programmed in
 * YUV_BUF's ctrl register, not here, and jpeg_hal_switch_mode()'s
 * vsync/hsync/sensor_fmt handling compiles to nothing on BK7258.  This
 * driver writes only the fields that really exist.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "irq.h"
#include "bk7258_jpeg_enc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_JPEG_ENC_BASE       0x48000000u  /* SOC_JPEG_REG_BASE */

#define JPEG_REG(word_off)         (BK7258_JPEG_ENC_BASE + (word_off) * 4u)

#define JPEG_DEV_ID                      JPEG_REG(0)
#define JPEG_DEV_VERSION                 JPEG_REG(1)

#define JPEG_GLOBAL_CTRL                 JPEG_REG(2)
#define JPEG_GLOBAL_CTRL_SOFT_RESET      (1u << 0)
#define JPEG_GLOBAL_CTRL_CLK_GATE_BYPASS (1u << 1)

#define JPEG_DEV_STATUS                  JPEG_REG(3)
#define JPEG_EOF_OFFSET                  JPEG_REG(4)
#define JPEG_RX_FIFO_DATA                JPEG_REG(5)

#define JPEG_INT_STATUS                  JPEG_REG(6)
#define JPEG_INT_STATUS_SOF              (1u << 0)
#define JPEG_INT_STATUS_EOF              (1u << 1)
#define JPEG_INT_STATUS_HEAD             (1u << 2)
#define JPEG_INT_STATUS_FRAME_ERR        (1u << 3)
#define JPEG_INT_STATUS_LINE_CLEAR       (1u << 5)

/* jpeg_ll_get_interrupt_status() returns only int_status.int_status,
 * i.e. bit[0:5]; bit[6] (fifo_rd_finish) is a FIFO state flag, not an
 * event, and is deliberately left out of the acknowledge write-back.
 */

#define JPEG_INT_STATUS_MASK             0x3fu

#define JPEG_BYTE_COUNT_PFRM             JPEG_REG(7)

#define JPEG_FIFO_STATUS                 JPEG_REG(8)
#define JPEG_FIFO_STATUS_STREAM_EMPTY    (1u << 26)
#define JPEG_Y_COUNT                     JPEG_REG(9)
#define JPEG_BYTE_COUNT_LINE             JPEG_REG(10)

#define JPEG_INT_EN                      JPEG_REG(12)
#define JPEG_INT_EN_FRAME_ERR            (1u << 0)
#define JPEG_INT_EN_HEAD                 (1u << 1)
#define JPEG_INT_EN_SOF                  (1u << 2)
#define JPEG_INT_EN_EOF                  (1u << 3)
#define JPEG_INT_EN_LINE_CLEAR           (1u << 7)

/* What the reference enables for JPEG_MODE: end-of-frame only
 * (jpeg_hal_switch_mode(): "jpeg_ll_enable_end_frame_int(hal->hw)" is
 * the single int_en call in the JPEG_MODE branch).  head/sof/line_clear
 * belong to paths this driver does not implement, and frame_err is left
 * off to stay faithful to the reference -- one frame's worth of encoded
 * data is reported by EOF alone.
 */

/* Interrupts armed in JPEG mode.
 *
 * EOF is the one the driver acts on -- it is what "a frame is encoded"
 * means.  SOF and frame-error are enabled purely to be counted: without
 * them, an encoder that produces nothing is indistinguishable from one that
 * starts frames and fails to finish them, which is exactly the ambiguity the
 * first board attempt ran into (isr=0 frame=0 sof=0 could not say whether
 * the sensor stream was reaching the encoder at all).
 */

#define JPEG_INT_EN_JPEG_MODE \
  (JPEG_INT_EN_EOF | JPEG_INT_EN_SOF | JPEG_INT_EN_FRAME_ERR)

#define JPEG_CFG                         JPEG_REG(13)
#define JPEG_CFG_VIDEO_BYTE_REVERSE      (1u << 1)
#define JPEG_CFG_ENC_EN                  (1u << 4)
#define JPEG_CFG_X_PIXEL_SHIFT           8u
#define JPEG_CFG_X_PIXEL_MASK            (0xffu << JPEG_CFG_X_PIXEL_SHIFT)
#define JPEG_CFG_ENC_SIZE                (1u << 16)
#define JPEG_CFG_BITRATE_CTRL            (1u << 17)
#define JPEG_CFG_BITRATE_STEP_SHIFT      18u
#define JPEG_CFG_BITRATE_STEP_MASK       (0x3u << JPEG_CFG_BITRATE_STEP_SHIFT)
#define JPEG_CFG_AUTO_STEP               (1u << 20)
#define JPEG_CFG_BITRATE_MODE            (1u << 23)
#define JPEG_CFG_Y_PIXEL_SHIFT           24u
#define JPEG_CFG_Y_PIXEL_MASK            (0xffu << JPEG_CFG_Y_PIXEL_SHIFT)

/* jpeg_ll_set_default_bitrate_step(): "hw->cfg.bitrate_step = 3". */

#define JPEG_BITRATE_STEP_DEFAULT        3u

#define JPEG_TARGET_BYTE_H               JPEG_REG(14)
#define JPEG_TARGET_BYTE_L               JPEG_REG(15)

#define JPEG_QUANT_TABLE                 JPEG_REG(0x20)
#define JPEG_QUANT_TABLE_LEN             32u

/* Rate-control byte window per resolution band, verbatim from
 * jpeg_hal.c's JPEG_BITRATE_{MAX,MIN}_SIZE_* macros and the x_pixel
 * switch in jpeg_hal_set_target_bitrate().  x_pixel is width/8, so the
 * band boundaries are 40/80/160/200 == 320/640/1280/1600 pixels wide
 * (X_PIXEL_* in ap/include/driver/jpeg_enc_types.h).
 */

#define JPEG_BITRATE_MAX_320             (20u * 1024u)
#define JPEG_BITRATE_MIN_320             (5u * 1024u)
#define JPEG_BITRATE_MAX_640             (35u * 1024u)
#define JPEG_BITRATE_MIN_640             (20u * 1024u)
#define JPEG_BITRATE_MAX_1280            (50u * 1024u)
#define JPEG_BITRATE_MIN_1280            (30u * 1024u)

#define JPEG_X_PIXEL_320                 40u
#define JPEG_X_PIXEL_640                 80u
#define JPEG_X_PIXEL_1280                160u
#define JPEG_X_PIXEL_1600                200u

/* Geometry limits: x_pixel/y_pixel are 8-bit cfg fields holding
 * width/8 and height/8.
 */

#define JPEG_PIXEL_BLOCK                 8u
#define JPEG_MAX_DIMENSION               (255u * JPEG_PIXEL_BLOCK)

/* System controller registers that gate the video pipeline's power and
 * this module's clock -- required before any JPEG register has real
 * effect.  Ported from jpeg_driver.c jpeg_init_common():
 *
 *   bk_pm_module_vote_power_ctrl(PM_POWER_SUB_MODULE_NAME_VIDP_JPEG_EN,
 *                               PM_POWER_MODULE_STATE_ON)
 *     -> sys_hal_video_power_en(0)
 *     -> cpu_power_sleep_wakeup.pwd_vidp = 0  (sysctrl word 0x10 bit[7],
 *        active-high power-DOWN, so clearing it powers the domain up)
 *   sys_drv_set_jpeg_clk_en(1)
 *     -> sys_hal_set_jpeg_clk_en(1)
 *     -> cpu_device_clk_enable.jpeg_cken = 1 (sysctrl word 0x0c bit[28])
 *
 * The remaining two steps of jpeg_init_common() need nothing here:
 * sys_drv_int_enable(JPEGENC_INTERRUPT_CTRL_BIT) is what this port's
 * up_enable_irq() already does (bk7258_irq.c bk7258_route_irq() writes
 * the same CPU interrupt-enable register), and sys_drv_set_jpeg_disckg()
 * is an empty stub on BK7258 (sys_hal.c sys_hal_set_jpeg_disckg() has
 * its only line commented out).
 */

#define BK7258_SYS_REG_BASE              0x44010000u
#define BK7258_SYS_REG_0X0C              (BK7258_SYS_REG_BASE + (0x0cu << 2))
#define BK7258_SYS_JPEG_CKEN             (1u << 28)

#define BK7258_SYS_REG_0X10              (BK7258_SYS_REG_BASE + (0x10u << 2))
#define BK7258_SYS_PWD_VIDP              (1u << 7)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Quantisation table, copied verbatim (values and order) from
 * ap/middleware/soc/bk7258_ap/hal/jpeg_ll.c's static
 * jpeg_quant_table[JPEG_QUANT_TABLE_LEN], which
 * jpeg_ll_init_quant_table() writes word by word starting at
 * JPEG_R_QUANT_TABLE (jpeg_reg.h: JPEG_R_BASE + 4 * 0x20).  Words 0..15
 * are the luma table, words 16..31 the chroma table; there is no
 * quality-scaling step in the reference, the hardware gets exactly these
 * 32 words.  (jpeg_struct.h declares the array as quat_table[0x1d] = 29
 * words, which contradicts the 32 words jpeg_ll_init_quant_table()
 * actually writes; the .c is authoritative here since it is the code
 * that runs.)
 */

static const uint32_t g_jpeg_quant_table[JPEG_QUANT_TABLE_LEN] =
{
  0x07060608, 0x07080506, 0x09090707, 0x140c0a08,
  0x0b0b0c0d, 0x1312190c, 0x1a1d140f, 0x1a1d1e1f,
  0x24201c1c, 0x2220272e, 0x1c1c232c, 0x2c293728,
  0x34343130, 0x39271f34, 0x3c32383d, 0x3234332e,
  0x0c090909, 0x0d180c0b, 0x2132180d, 0x3232211c,
  0x32323232, 0x32323232, 0x32323232, 0x32323232,
  0x32323232, 0x32323232, 0x32323232, 0x32323232,
  0x32323232, 0x32323232, 0x32323232, 0x32323232
};

static bk7258_jpeg_enc_eof_cb_t g_eof_cb;
static FAR void *g_eof_cb_arg;
static uint32_t g_output_buf_addr;

/* Written by the ISR, read by task-level diagnostics. */

static volatile uint32_t g_isr_count;
static volatile uint32_t g_frame_count;
static volatile uint32_t g_sof_count;
static volatile uint32_t g_err_count;
static volatile uint32_t g_last_status;
static volatile uint32_t g_last_bytes;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_jpeg_enc_isr
 *
 * Description:
 *   JPEG encoder interrupt handler.  Runs in interrupt context, so it
 *   MUST NOT print, block or allocate -- the sibling YUV_BUF driver's
 *   history (a printf() from its ISR starving all task-level work until
 *   the CP-side IPC heartbeat asserted; see bk7258_yuv_buf.c
 *   bk7258_yuv_buf_init()) is the reason that rule is spelled out here.
 *   Observability comes from the counters below plus
 *   bk7258_jpeg_enc_dump_status(), which task-level code calls.
 *
 *   On EOF the frame's encoded length is read from byte_count_pfrm
 *   (REG_0x7, jpeg_ll_get_frame_byte_number()) and handed to the
 *   registered callback, which therefore also runs in interrupt context.
 *
 ****************************************************************************/

static int bk7258_jpeg_enc_isr(int irq, FAR void *context, FAR void *arg)
{
  uint32_t status = getreg32(JPEG_INT_STATUS) & JPEG_INT_STATUS_MASK;
  uint32_t bytes;

  /* Write-1-to-clear: write back exactly the event bits read as set,
   * including any that are not consumed below, so an
   * enabled-but-unhandled event can never latch the interrupt line
   * permanently (jpeg_ll_clear_interrupt_status() writes the value that
   * jpeg_ll_get_interrupt_status() returned).
   */

  putreg32(status, JPEG_INT_STATUS);

  g_isr_count++;
  g_last_status = status;

  if ((status & JPEG_INT_STATUS_SOF) != 0)
    {
      g_sof_count++;
    }

  if ((status & JPEG_INT_STATUS_FRAME_ERR) != 0)
    {
      g_err_count++;
    }

  if ((status & JPEG_INT_STATUS_EOF) != 0)
    {
      bytes = getreg32(JPEG_BYTE_COUNT_PFRM);

      g_frame_count++;
      g_last_bytes = bytes;

      if (g_eof_cb != NULL)
        {
          g_eof_cb(g_eof_cb_arg, bytes);
        }
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_jpeg_enc_write_quant_table
 *
 * Description:
 *   Loads the 32-word quantisation table, mirroring
 *   jpeg_ll_init_quant_table()'s simple ascending word writes.
 *
 ****************************************************************************/

static void bk7258_jpeg_enc_write_quant_table(void)
{
  uint32_t i;

  for (i = 0; i < JPEG_QUANT_TABLE_LEN; i++)
    {
      putreg32(g_jpeg_quant_table[i], JPEG_QUANT_TABLE + i * 4u);
    }
}

/****************************************************************************
 * Name: bk7258_jpeg_enc_set_target_bitrate
 *
 * Description:
 *   Programs the rate-control byte window for the given x_pixel
 *   (= width / 8), reproducing jpeg_hal_set_target_bitrate()'s switch
 *   exactly -- including its behaviour for widths it does not name: the
 *   vendor switch matches x_pixel only against 40/80/160/200, and every
 *   other value (e.g. 1920/8 = 240, or 864/8 = 108) falls into the
 *   default case, which uses the 640-wide window.  That is intentional
 *   here: a "wider than 1280 gets the 1280 window" generalisation would
 *   be a behaviour change relative to the reference, and the byte
 *   windows have not been characterised on this board.
 *
 ****************************************************************************/

static void bk7258_jpeg_enc_set_target_bitrate(uint32_t x_pixel)
{
  uint32_t up_size;
  uint32_t low_size;

  switch (x_pixel)
    {
      case JPEG_X_PIXEL_320:
        up_size  = JPEG_BITRATE_MAX_320;
        low_size = JPEG_BITRATE_MIN_320;
        break;

      case JPEG_X_PIXEL_1280:
      case JPEG_X_PIXEL_1600:
        up_size  = JPEG_BITRATE_MAX_1280;
        low_size = JPEG_BITRATE_MIN_1280;
        break;

      case JPEG_X_PIXEL_640:
      default:
        up_size  = JPEG_BITRATE_MAX_640;
        low_size = JPEG_BITRATE_MIN_640;
        break;
    }

  putreg32(up_size, JPEG_TARGET_BYTE_H);
  putreg32(low_size, JPEG_TARGET_BYTE_L);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_jpeg_enc_init(void)
{
  uint32_t reg;
  uint32_t id_in_reset;
  uint32_t version_in_reset;
  uint32_t status_in_reset;

  /* Power on the video pipeline and open this module's clock gate before
   * touching any JPEG register (jpeg_init_common(); see the file
   * header for the register/bit derivation).
   */

  reg = getreg32(BK7258_SYS_REG_0X10);
  reg &= ~BK7258_SYS_PWD_VIDP;
  putreg32(reg, BK7258_SYS_REG_0X10);

  reg = getreg32(BK7258_SYS_REG_0X0C);
  reg |= BK7258_SYS_JPEG_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0C);

  printf("bk7258_jpeg_enc: init: power/clock enabled, reg0x0c=0x%08x "
         "reg0x10=0x%08x\n",
         (unsigned int)getreg32(BK7258_SYS_REG_0X0C),
         (unsigned int)getreg32(BK7258_SYS_REG_0X10));

  /* Soft reset, per jpeg_ll_init():
   *
   *   hw->global_ctrl.soft_reset = 0;
   *   hw->global_ctrl.soft_reset = 1;
   *   jpeg_ll_reset_config_to_default(hw);   // int_en = 0, ack status
   *
   * i.e. soft_reset is active-low exactly as in YUV_BUF's global_ctrl
   * (asserting reset means writing 0), and the module is left released
   * with clk_gate_bypass still 0 -- final global_ctrl == 0x1.  Note the
   * asymmetry with YUV_BUF: there clk_gate_bypass is set during init,
   * whereas the JPEG block only gets it when encoding actually starts
   * (jpeg_ll_enable_jpeg_mode() sets clk_gate_bypass and jpeg_enc_en
   * together), so it is set in bk7258_jpeg_enc_start() instead.
   *
   * The identity/status snapshots printed below are taken with the
   * reset asserted and again with it released; that pair distinguishes
   * "module dead/unclocked" from "module alive" on the board without
   * needing an oscilloscope or JTAG, which is exactly the evidence that
   * was missing during the YUV_BUF bring-up.
   */

  putreg32(0, JPEG_GLOBAL_CTRL);
  up_udelay(10);

  id_in_reset      = getreg32(JPEG_DEV_ID);
  version_in_reset = getreg32(JPEG_DEV_VERSION);
  status_in_reset  = getreg32(JPEG_DEV_STATUS);

  putreg32(JPEG_GLOBAL_CTRL_SOFT_RESET, JPEG_GLOBAL_CTRL);
  up_udelay(10);

  /* Leave the module quiet until bk7258_jpeg_enc_configure(): no
   * interrupt sources enabled, no stale status latched.
   */

  putreg32(0, JPEG_INT_EN);
  putreg32(getreg32(JPEG_INT_STATUS) & JPEG_INT_STATUS_MASK,
           JPEG_INT_STATUS);

  printf("bk7258_jpeg_enc: init: global_ctrl=0x%08x (soft_reset "
         "released, per jpeg_ll_init())\n",
         (unsigned int)getreg32(JPEG_GLOBAL_CTRL));
  printf("bk7258_jpeg_enc: init: dev_id=0x%08x->0x%08x "
         "dev_version=0x%08x->0x%08x dev_status=0x%08x->0x%08x "
         "(in-reset -> released)\n",
         (unsigned int)id_in_reset,
         (unsigned int)getreg32(JPEG_DEV_ID),
         (unsigned int)version_in_reset,
         (unsigned int)getreg32(JPEG_DEV_VERSION),
         (unsigned int)status_in_reset,
         (unsigned int)getreg32(JPEG_DEV_STATUS));

  irq_attach(BK7258_IRQ_JPEG_ENC, bk7258_jpeg_enc_isr, NULL);
  up_enable_irq(BK7258_IRQ_JPEG_ENC);

  printf("bk7258_jpeg_enc: init: complete, irq=%d attached and enabled\n",
         (int)BK7258_IRQ_JPEG_ENC);

  return 0;
}

void bk7258_jpeg_enc_uninit(void)
{
  uint32_t reg;

  bk7258_jpeg_enc_stop();

  /* jpeg_deinit_common()'s order: stop, soft-reset pulse, clear the
   * interrupt configuration, then drop the clocks.
   */

  putreg32(0, JPEG_GLOBAL_CTRL);
  up_udelay(10);
  putreg32(JPEG_GLOBAL_CTRL_SOFT_RESET, JPEG_GLOBAL_CTRL);

  putreg32(0, JPEG_INT_EN);
  putreg32(getreg32(JPEG_INT_STATUS) & JPEG_INT_STATUS_MASK,
           JPEG_INT_STATUS);

  up_disable_irq(BK7258_IRQ_JPEG_ENC);
  irq_detach(BK7258_IRQ_JPEG_ENC);

  reg = getreg32(BK7258_SYS_REG_0X0C);
  reg &= ~BK7258_SYS_JPEG_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0C);

  /* pwd_vidp is deliberately left as it is: the video-pipeline power
   * domain is shared with YUV_BUF and H264 (the vendor driver
   * reference-counts it in sys_drv_video_power_handle()), so powering it
   * down from here would silently kill a capture path this driver does
   * not own.
   */

  g_eof_cb     = NULL;
  g_eof_cb_arg = NULL;
}

int bk7258_jpeg_enc_configure(uint16_t width, uint16_t height)
{
  uint32_t x_pixel;
  uint32_t y_pixel;
  uint32_t cfg;
  irqstate_t flags;

  if (width == 0 || height == 0 ||
      (width % JPEG_PIXEL_BLOCK) != 0 || (height % JPEG_PIXEL_BLOCK) != 0 ||
      width > JPEG_MAX_DIMENSION || height > JPEG_MAX_DIMENSION)
    {
      printf("bk7258_jpeg_enc: configure: rejected %ux%u (both "
             "dimensions must be non-zero multiples of 8 and <= %u, "
             "since x_pixel/y_pixel are 8-bit fields holding size/8)\n",
             (unsigned int)width, (unsigned int)height,
             (unsigned int)JPEG_MAX_DIMENSION);
      return -EINVAL;
    }

  x_pixel = width / JPEG_PIXEL_BLOCK;
  y_pixel = height / JPEG_PIXEL_BLOCK;

  flags = enter_critical_section();
  g_isr_count   = 0;
  g_frame_count = 0;
  g_sof_count   = 0;
  g_err_count   = 0;
  g_last_status = 0;
  g_last_bytes  = 0;
  leave_critical_section(flags);

  /* jpeg_hal_switch_mode(), JPEG_MODE branch, in order:
   *
   *   jpeg_ll_clear_config()            cfg.v = 0
   *   [vsync/hsync/sensor_fmt handling: all no-ops on BK7258, see the
   *    file header]
   *   jpeg_ll_enable_end_frame_int()    int_en.eof_int_en = 1
   *   jpeg_ll_set_x_pixel()             cfg.x_pixel
   *   jpeg_ll_set_y_pixel()             cfg.y_pixel
   *   jpeg_ll_init_quant_table()        32 words at REG_0x20
   *   jpeg_hal_set_target_bitrate()     target_byte_h / target_byte_l
   *   jpeg_ll_enable_bitrate_ctrl()     cfg.bitrate_ctrl = 1
   *   jpeg_ll_set_default_bitrate_step() cfg.bitrate_step = 3
   *   jpeg_ll_enable_video_byte_reverse() cfg.video_byte_reverse = 1
   *   jpeg_ll_enable_enc_size()         cfg.jpeg_enc_size = 1
   *
   * The cfg fields are composed into one value and written once, which
   * is equivalent to the vendor's clear-then-set-each-field sequence
   * (it starts from cfg.v = 0) but does not expose the hardware to a
   * string of half-configured intermediate states.  jpeg_enc_en stays
   * 0 here: encoding is armed by bk7258_jpeg_enc_start().
   *
   * auto_step and bitrate_mode are left at 0 because the JPEG_MODE
   * branch never writes them.
   */

  putreg32(0, JPEG_CFG);

  cfg = ((x_pixel << JPEG_CFG_X_PIXEL_SHIFT) & JPEG_CFG_X_PIXEL_MASK) |
        ((y_pixel << JPEG_CFG_Y_PIXEL_SHIFT) & JPEG_CFG_Y_PIXEL_MASK) |
        JPEG_CFG_BITRATE_CTRL |
        (JPEG_BITRATE_STEP_DEFAULT << JPEG_CFG_BITRATE_STEP_SHIFT) |
        JPEG_CFG_ENC_SIZE;

  /* video_byte_reverse: carried over from the reference's JPEG_MODE
   * branch as-is.  Its semantics are the same family as the 4-byte
   * bus-to-memory reversal already measured on this board's YUV path
   * (bk7258_yuv_buf.c's yuv_fmt_sel comment: the frame buffer comes out
   * as Cr Y1 Cb Y0 for a Y Cb Y Cr bus order), so it plausibly
   * compensates for the same wiring -- but that connection is inference,
   * not measurement: this bit has NOT been independently verified on
   * this board, and if the encoder produces colour-swapped or garbage
   * output this is the first bit to toggle.
   */

  cfg |= JPEG_CFG_VIDEO_BYTE_REVERSE;

  bk7258_jpeg_enc_write_quant_table();
  bk7258_jpeg_enc_set_target_bitrate(x_pixel);

  putreg32(cfg, JPEG_CFG);

  /* Drop anything latched while the module was being programmed, then
   * enable exactly the event the encode path needs.
   */

  putreg32(getreg32(JPEG_INT_STATUS) & JPEG_INT_STATUS_MASK,
           JPEG_INT_STATUS);
  putreg32(JPEG_INT_EN_JPEG_MODE, JPEG_INT_EN);

  printf("bk7258_jpeg_enc: configure(%ux%u): x_pixel=%u y_pixel=%u\n",
         (unsigned int)width, (unsigned int)height,
         (unsigned int)x_pixel, (unsigned int)y_pixel);
  printf("bk7258_jpeg_enc: configure: global_ctrl=0x%08x cfg=0x%08x "
         "int_en=0x%08x int_status=0x%08x target_byte_h=%u "
         "target_byte_l=%u quant[0]=0x%08x quant[31]=0x%08x\n",
         (unsigned int)getreg32(JPEG_GLOBAL_CTRL),
         (unsigned int)getreg32(JPEG_CFG),
         (unsigned int)getreg32(JPEG_INT_EN),
         (unsigned int)getreg32(JPEG_INT_STATUS),
         (unsigned int)getreg32(JPEG_TARGET_BYTE_H),
         (unsigned int)getreg32(JPEG_TARGET_BYTE_L),
         (unsigned int)getreg32(JPEG_QUANT_TABLE),
         (unsigned int)getreg32(JPEG_QUANT_TABLE +
                                (JPEG_QUANT_TABLE_LEN - 1u) * 4u));

  return 0;
}

void bk7258_jpeg_enc_set_buffer(uint32_t addr)
{
  /* Records only -- there is no destination-address register on this
   * module.  The vendor's jpeg_ll_set_em_base_addr() looks like one but
   * is not: on BK7258 its whole body is
   *
   *     hw->eof_offset.v |= (0x20 << 16);
   *
   * which discards the address argument entirely and ORs a constant into
   * eof_offset[16:31] (jpeg_hal_debug.c even prints that register under
   * the label "em_base_addr", which is where the name confusion comes
   * from).  jpeg_hal_switch_mode() only calls it under "#if
   * (!CONFIG_YUV_BUF)", i.e. never in this board's configuration, where
   * YUV_BUF feeds the encoder -- so this driver does not write
   * eof_offset at all, and the meaning of that 0x20 remains unverified.
   *
   * The encoded bitstream leaves the chip's JPEG block through
   * rx_fifo_data; see bk7258_jpeg_enc_get_fifo_addr().
   */

  g_output_buf_addr = addr;
}

uint32_t bk7258_jpeg_enc_get_buffer(void)
{
  return g_output_buf_addr;
}

uint32_t bk7258_jpeg_enc_get_fifo_addr(void)
{
  return JPEG_RX_FIFO_DATA;
}

void bk7258_jpeg_enc_register_callback(bk7258_jpeg_enc_eof_cb_t cb,
                                       FAR void *arg)
{
  irqstate_t flags = enter_critical_section();

  g_eof_cb = cb;
  g_eof_cb_arg = arg;

  leave_critical_section(flags);
}

void bk7258_jpeg_enc_start(void)
{
  /* Drop anything latched while idle, then enable encoding the way
   * jpeg_ll_enable_jpeg_mode() does: clock-gate bypass first, then
   * cfg.jpeg_enc_en.  Read-modify-write on both registers, so the
   * geometry/rate-control fields configure() wrote survive untouched.
   */

  putreg32(getreg32(JPEG_INT_STATUS) & JPEG_INT_STATUS_MASK,
           JPEG_INT_STATUS);

  modifyreg32(JPEG_GLOBAL_CTRL, 0, JPEG_GLOBAL_CTRL_CLK_GATE_BYPASS);
  modifyreg32(JPEG_CFG, 0, JPEG_CFG_ENC_EN);
}

/****************************************************************************
 * Name: bk7258_jpeg_enc_soft_reset
 *
 * Description:
 *   Pulses the module's global soft reset (0 = held, 1 = released, so the
 *   pulse is 0 then 1) without touching anything else.
 *
 *   This is the recovery step for a frame the encoder gave up on.  The
 *   reference does exactly this and does NOT reprogram the encoder
 *   afterwards (bk_jpeg_enc_soft_reset() called from
 *   dvp_camera_reset_hardware_modules_handler()), so the configuration
 *   registers are expected to survive the pulse.
 *
 *   Print-free and register-only, because recovery runs from the capture
 *   path's interrupt handler.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_jpeg_enc_fifo_empty
 *
 * Description:
 *   True when the output FIFO holds no more bitstream.
 *
 *   The EOF interrupt says the encoder finished producing, not that the
 *   bitstream has been delivered: whatever is still in this FIFO has yet to
 *   be moved by the DMA.  Measured on this board, a frame examined at EOF is
 *   short by a few hundred bytes and its end-of-image marker has not arrived
 *   yet.  Callers therefore wait on this before deciding a frame's length.
 *
 *   Read-only and print-free: called from interrupt context.
 *
 ****************************************************************************/

bool bk7258_jpeg_enc_fifo_empty(void)
{
  return (getreg32(JPEG_FIFO_STATUS) &
          JPEG_FIFO_STATUS_STREAM_EMPTY) != 0;
}

void bk7258_jpeg_enc_soft_reset(void)
{
  modifyreg32(JPEG_GLOBAL_CTRL, JPEG_GLOBAL_CTRL_SOFT_RESET, 0);
  modifyreg32(JPEG_GLOBAL_CTRL, 0, JPEG_GLOBAL_CTRL_SOFT_RESET);
}

void bk7258_jpeg_enc_stop(void)
{
  /* jpeg_ll_disable_jpeg_mode(): clk_gate_bypass = 0, jpeg_enc_en = 0.
   * Nothing else is touched -- in particular int_en keeps the EOF enable
   * and cfg keeps the geometry, so a later _start() resumes without
   * reconfiguring (and no interrupt can arrive in the meantime, because
   * a stopped encoder produces no events).
   */

  modifyreg32(JPEG_CFG, JPEG_CFG_ENC_EN, 0);
  modifyreg32(JPEG_GLOBAL_CTRL, JPEG_GLOBAL_CTRL_CLK_GATE_BYPASS, 0);
}

/****************************************************************************
 * Name: bk7258_jpeg_enc_write_header
 *
 * Description:
 *   Replaces the header this block puts in front of its bitstream with a
 *   standards-conforming one, in place, without moving the entropy data.
 *
 *   Why it is needed: the block emits SOI / APP0 / SOF0 / DQT x2 / DHT x3 and
 *   then entropy data, and the AC Huffman table it writes into the stream is
 *   NOT the table it encoded with.  Measured on this board (2026-08-12): its
 *   two DC tables are byte-identical to the JPEG Annex K standard tables, its
 *   AC table is not, and it emits neither the chroma AC table nor an SOS
 *   segment at all.  libjpeg's verdict on the raw stream is "Invalid JPEG
 *   file structure: missing SOS marker"; substituting the four standard
 *   tables plus an SOS decodes to a correct picture.  Evidence and the
 *   table-by-table comparison are in docs/reference/camera.md 14.7.
 *
 *   Layout.  The caller has the block write its bitstream at buf + pad rather
 *   than at buf, which buys the room the longer header needs:
 *
 *     hardware header  SOI + APP0 + SOF0 + DQT x2 + DHT x3 + fill  ~429 B
 *     this header      SOI + SOF0 + DQT x2 + DHT x4 + SOS           605 B
 *
 *   605 > 429, so overwriting in place from buf would run into the entropy
 *   data.  With pad bytes in front there is room, and the leftover gap is
 *   absorbed by a COM (comment) segment, whose length is free.  That also
 *   makes the number of 0xFF fill bytes the block emits irrelevant -- it
 *   varies between frames, and the COM segment soaks up the difference:
 *
 *     buf[0 .. ]           SOI + SOF0 + DQT x2 + DHT x4   (fixed length)
 *     buf[.. E-15]         COM segment (>= 4 B), covering the old header
 *     buf[E-14 .. E-1]     SOS                            (14 B)
 *     buf[E ..]            entropy data, not touched
 *
 *   SOF0 and DQT are copied from what the block produced: both are correct
 *   (the geometry is right and the quantisation tables are the ones this
 *   driver programmed), and copying them saves this function from having to
 *   know them.  They are staged on the stack because the destination range
 *   overlaps the source.
 *
 *   Interrupt context safe: no printf, no allocation, no blocking.  Reads a
 *   few hundred bytes and writes ~605.
 *
 * Input Parameters:
 *   buf - the V4L2 buffer, i.e. the DMA destination minus pad
 *   pad - offset the bitstream was written at
 *
 * Returned Value:
 *   Offset of the entropy data within buf, which is also the length of the
 *   header just written.  Zero if the stream was not shaped as expected, in
 *   which case buf is left alone.
 *
 ****************************************************************************/

size_t bk7258_jpeg_enc_write_header(FAR uint8_t *buf, size_t pad)
{
  /* The four Annex K tables in DC0 / AC0 / DC1 / AC1 order, each a complete
   * segment including its FF C4 marker.  Taken from a reference encoder's
   * output rather than typed in, so the bit counts and symbol lists cannot
   * have been transcribed wrong.
   */

  static const uint8_t g_std_dht[] =
  {
    0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00,
    0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05,
    0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11,
    0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
    0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00,
    0xb5, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05,
    0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04,
    0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25,
    0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66,
    0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
  };

  /* Scan header: DC0/AC0 for luma, DC1/AC1 for chroma, baseline spectral
   * range.  Ns=3 matches the SOF0 this block emits.
   */

  static const uint8_t g_sos[] =
  {
    0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11,
    0x00, 0x3f, 0x00
  };

  uint8_t stage[BK7258_JPEG_ENC_STAGE_MAX];
  size_t stagelen = 0;
  size_t entropy = 0;
  size_t fixed;
  size_t comlen;
  size_t pos;
  size_t i;

  if (buf == NULL)
    {
      return 0;
    }

  i = pad;

  if (buf[i] != 0xff || buf[i + 1] != 0xd8)
    {
      return 0;                    /* No SOI: not a bitstream we recognise. */
    }

  i += 2;

  /* Walk the block's own header, staging SOF0 and DQT, and stop at the first
   * thing that is not a segment -- that is where entropy data begins.
   */

  while (i + 3 < pad + BK7258_JPEG_ENC_HDR_SCAN_MAX)
    {
      uint8_t marker;
      size_t seglen;

      if (buf[i] != 0xff)
        {
          break;
        }

      while (buf[i + 1] == 0xff)
        {
          i++;
        }

      marker = buf[i + 1];

      if (marker == 0xd9)
        {
          break;                     /* End of image: no entropy data. */
        }

      seglen = ((size_t)buf[i + 2] << 8) | buf[i + 3];

      if (marker == 0xda)
        {
          /* The block does emit an SOS, and the entropy data begins after
           * it -- not at it.  Consume the segment and stop.
           *
           * Treating SOS as "not a segment" and breaking here reported the
           * entropy data as starting 16 bytes early, so the header the
           * application received declared its scan to begin inside the
           * block's own SOS.  Every frame then decoded to noise while
           * carrying a perfectly valid marker structure, which is why the
           * fault survived so long: SOF/DQT/DHT/SOS/EOI all check out.
           *
           * Measured on this board: the block's SOS sits at buf+870 with
           * length 12, entropy at 884, and this function returned 868.
           * Re-assembling a captured frame on the host with the payload
           * shifted by those 16 bytes was the confirmation -- three frames
           * of a static scene then agreed to within 2.8 grey levels, in the
           * same range as the software encoder (1.5-11), while every other
           * shift gave 3.6 or worse with wildly inconsistent pairs.
           */

          if (seglen >= 2)
            {
              i += 2 + seglen;
              entropy = i;
            }

          break;
        }

      if (seglen < 2)
        {
          break;
        }

      if (marker == 0xc0 || marker == 0xdb)
        {
          if (stagelen + seglen + 2 > sizeof(stage))
            {
              return 0;
            }

          memcpy(&stage[stagelen], &buf[i], seglen + 2);
          stagelen += seglen + 2;
        }

      i += 2 + seglen;
      entropy = i;
    }

  if (entropy == 0 || stagelen == 0)
    {
      return 0;
    }

  fixed = 2 + stagelen + sizeof(g_std_dht);

  if (entropy < fixed + sizeof(g_sos) + 4)
    {
      return 0;                    /* pad too small for this stream. */
    }

  comlen = entropy - sizeof(g_sos) - fixed;

  buf[0] = 0xff;
  buf[1] = 0xd8;
  pos = 2;

  memcpy(&buf[pos], stage, stagelen);
  pos += stagelen;

  memcpy(&buf[pos], g_std_dht, sizeof(g_std_dht));
  pos += sizeof(g_std_dht);

  buf[pos]     = 0xff;
  buf[pos + 1] = 0xfe;
  buf[pos + 2] = (uint8_t)((comlen - 2) >> 8);
  buf[pos + 3] = (uint8_t)((comlen - 2) & 0xff);
  pos += comlen;

  memcpy(&buf[pos], g_sos, sizeof(g_sos));

  return entropy;
}

void bk7258_jpeg_enc_get_stats(FAR struct bk7258_jpeg_enc_stats_s *stats)
{
  irqstate_t flags;

  if (stats == NULL)
    {
      return;
    }

  flags = enter_critical_section();

  stats->isr_count   = g_isr_count;
  stats->frame_count = g_frame_count;
  stats->sof_count   = g_sof_count;
  stats->err_count   = g_err_count;
  stats->last_status = g_last_status;
  stats->last_bytes  = g_last_bytes;

  leave_critical_section(flags);
}

void bk7258_jpeg_enc_dump_status(FAR const char *tag)
{
  struct bk7258_jpeg_enc_stats_s stats;

  bk7258_jpeg_enc_get_stats(&stats);

  printf("bk7258_jpeg_enc: [%s] global_ctrl=0x%08x cfg=0x%08x "
         "int_en=0x%08x int_status=0x%08x byte_count_pfrm=%u "
         "fifo_status=0x%08x y_count=0x%08x dev_id=0x%08x "
         "dev_status=0x%08x\n",
         tag,
         (unsigned int)getreg32(JPEG_GLOBAL_CTRL),
         (unsigned int)getreg32(JPEG_CFG),
         (unsigned int)getreg32(JPEG_INT_EN),
         (unsigned int)getreg32(JPEG_INT_STATUS),
         (unsigned int)getreg32(JPEG_BYTE_COUNT_PFRM),
         (unsigned int)getreg32(JPEG_FIFO_STATUS),
         (unsigned int)getreg32(JPEG_Y_COUNT),
         (unsigned int)getreg32(JPEG_DEV_ID),
         (unsigned int)getreg32(JPEG_DEV_STATUS));
  printf("bk7258_jpeg_enc: [%s] target_byte_h=%u target_byte_l=%u "
         "out_buf=0x%08x fifo_addr=0x%08x\n",
         tag,
         (unsigned int)getreg32(JPEG_TARGET_BYTE_H),
         (unsigned int)getreg32(JPEG_TARGET_BYTE_L),
         (unsigned int)g_output_buf_addr,
         (unsigned int)bk7258_jpeg_enc_get_fifo_addr());
  printf("bk7258_jpeg_enc: [%s] isr=%u frame=%u sof=%u err=%u "
         "last_status=0x%08x last_bytes=%u\n",
         tag,
         (unsigned int)stats.isr_count,
         (unsigned int)stats.frame_count,
         (unsigned int)stats.sof_count,
         (unsigned int)stats.err_count,
         (unsigned int)stats.last_status,
         (unsigned int)stats.last_bytes);
}
