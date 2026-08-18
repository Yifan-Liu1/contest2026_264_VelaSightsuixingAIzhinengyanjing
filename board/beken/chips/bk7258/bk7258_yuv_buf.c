/****************************************************************************
 * board/beken/chips/bk7258/bk7258_yuv_buf.c
 *
 * BK7258 YUV_BUF controller, YUV direct-capture mode only.  Register
 * layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/yuv_buf_struct.h and
 * ap/middleware/soc/bk7258_ap/hal/yuv_buf_ll.h.
 *
 * Register map (byte offset from BK7258_YUV_BUF_BASE):
 *   dev_id        0x00 - read-only module identity
 *   dev_version   0x04 - read-only module version
 *   global_ctrl   0x08 - bit[0] soft_reset, bit[1] clk_gate_bypass
 *   dev_status    0x0c - read-only module status
 *   ctrl          0x10 - bit[0] yuv_mode, bit[2:3] yuv_fmt_sel,
 *                         bit[6] hsync_rev, bit[7] vsync_rev,
 *                         bit[9] sync_edge_dect_en, bit[10:11] mclk_div,
 *                         bit[12] h264_mode, bit[13] bps_cis,
 *                         bit[14] memrev, bit[19] soi_hsync
 *   pixel         0x14 - bit[0:7] x_pixel, bit[8:15] y_pixel,
 *                         bit[16:31] frame_blk
 *   em_base_addr  0x20 - frame writer destination
 *   int_en        0x24 - bit[0] vsync_nege, bit[1] yuv_arv,
 *                         bit[2] sm0_wr, bit[3] sm1_wr, bit[4] sen_full,
 *                         bit[5] enc_line, bit[6] sen_resl,
 *                         bit[7] h264_err, bit[8] enc_slow
 *   int_status    0x28 - same bit order as int_en (write-1-to-clear)
 *   int_masks     0x2c - bit[0:1] vsync_nege, bit[2] resl_err,
 *                         bit[3] sens_full, bit[4] enc_slow,
 *                         bit[5] h264_err
 *   emr_base_addr 0x30 - resize-path destination (same value as
 *                         em_base_addr for whole-frame capture)
 *   resize_pixel  0x34 - bit[1:8] x_pixel, bit[9:16] y_pixel
 *
 * x_pixel/y_pixel are width/height divided by 8 (matching bk_dvp.c's
 * "yuv_mode_config.x_pixel = config->width / 8"); frame_blk = x_pixel *
 * y_pixel / 2.
 *
 * Power/clock: enabling this module requires clearing the video
 * pipeline's shared power-down bit and setting its own clock gate, both
 * in the system controller's register space
 * (ap/middleware/driver/yuv_buf/yuv_buf_driver.c yuv_buf_init_common())
 * -- neither is part of YUV_BUF's own register block.  bk_avdk_smp
 * enables the H264 clock gate alongside YUV_BUF's even for pure YUV
 * direct-capture mode; both are set here to match.
 *
 * Capture model: whole-frame direct write into PSRAM.  em_base_addr /
 * emr_base_addr point at the caller's frame buffer and the hardware
 * raises YUV_ARV (int bit[1]) once a complete frame has been written --
 * exactly what the reference does for pure IMAGE_YUV
 * (dvp_camera_yuv_mode() + dvp_camera_yuv_eof_handler()).  The
 * SM0_WR/SM1_WR line-batch interrupts are not used here; see
 * include/bk7258_yuv_buf.h for why.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <stdint.h>
#include <stdio.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "irq.h"
#include "bk7258_yuv_buf.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_YUV_BUF_BASE        0x48020000u  /* SOC_YUV_BUF_REG_BASE */

#define YUV_BUF_REG(word_off)      (BK7258_YUV_BUF_BASE + (word_off) * 4u)

#define YUV_BUF_DEV_ID                   YUV_BUF_REG(0)
#define YUV_BUF_DEV_VERSION              YUV_BUF_REG(1)

#define YUV_BUF_GLOBAL_CTRL              YUV_BUF_REG(2)
#define YUV_BUF_GLOBAL_CTRL_SOFT_RESET   (1u << 0)
#define YUV_BUF_GLOBAL_CTRL_CLK_GATE_BYPASS (1u << 1)

#define YUV_BUF_DEV_STATUS               YUV_BUF_REG(3)

#define YUV_BUF_CTRL                     YUV_BUF_REG(4)
#define YUV_BUF_CTRL_YUV_MODE            (1u << 0)
#define YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT   2u
#define YUV_BUF_CTRL_YUV_FMT_SEL_MASK    (0x3u << YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT)
#define YUV_BUF_CTRL_HSYNC_REV           (1u << 6)
#define YUV_BUF_CTRL_VSYNC_REV           (1u << 7)
#define YUV_BUF_CTRL_SYNC_EDGE_DECT_EN   (1u << 9)
#define YUV_BUF_CTRL_MCLK_DIV_SHIFT      10u
#define YUV_BUF_CTRL_MCLK_DIV_MASK       (0x3u << YUV_BUF_CTRL_MCLK_DIV_SHIFT)
#define YUV_BUF_CTRL_H264_MODE           (1u << 12)
#define YUV_BUF_CTRL_BPS_CIS             (1u << 13)
#define YUV_BUF_CTRL_MEMREV              (1u << 14)
#define YUV_BUF_CTRL_SOI_HSYNC           (1u << 19)

#define YUV_BUF_FMT_YUYV                 0u
#define YUV_BUF_MCLK_DIV_3               3u

#define YUV_BUF_PIXEL                    YUV_BUF_REG(5)
#define YUV_BUF_PIXEL_X_SHIFT            0u
#define YUV_BUF_PIXEL_Y_SHIFT            8u
#define YUV_BUF_PIXEL_FRAME_BLK_SHIFT    16u

#define YUV_BUF_EM_BASE_ADDR             YUV_BUF_REG(8)

#define YUV_BUF_INT_EN                   YUV_BUF_REG(9)
#define YUV_BUF_INT_STATUS               YUV_BUF_REG(10)
#define YUV_BUF_INT_VSYNC_NEGE           (1u << 0)
#define YUV_BUF_INT_YUV_ARV              (1u << 1)
#define YUV_BUF_INT_SM0_WR               (1u << 2)
#define YUV_BUF_INT_SM1_WR               (1u << 3)
#define YUV_BUF_INT_SEN_FULL             (1u << 4)
#define YUV_BUF_INT_ENC_LINE             (1u << 5)
#define YUV_BUF_INT_SEN_RESL             (1u << 6)
#define YUV_BUF_INT_H264_ERR             (1u << 7)
#define YUV_BUF_INT_ENC_SLOW             (1u << 8)
#define YUV_BUF_INT_ALL                  0x1FFu

/* Error events the reference routes to dvp_camera_sensor_ppi_err_handler()
 * (bk_dvp.c dvp_camera_register_isr_function()): sensor FIFO full,
 * sensor resolution mismatch, encoder too slow, H264 error.
 */

#define YUV_BUF_INT_ERRORS \
  (YUV_BUF_INT_SEN_FULL | YUV_BUF_INT_SEN_RESL | \
   YUV_BUF_INT_H264_ERR | YUV_BUF_INT_ENC_SLOW)

/* What whole-frame YUV capture actually needs: frame done, frame start,
 * and the error reports.  The line-batch (SM0/SM1) and per-encoded-line
 * events belong to the encode path only -- enabling them here would cost
 * ~1800 interrupts/s at 640x480@30fps for nothing.
 */

#define YUV_BUF_INT_EN_YUV_MODE \
  (YUV_BUF_INT_VSYNC_NEGE | YUV_BUF_INT_YUV_ARV | YUV_BUF_INT_ERRORS)

#define YUV_BUF_INT_MASKS                YUV_BUF_REG(11)
#define YUV_BUF_INT_MASKS_VSYNC_NEGE     (0x3u << 0)
#define YUV_BUF_INT_MASKS_RESL_ERR       (1u << 2)
#define YUV_BUF_INT_MASKS_SENS_FULL      (1u << 3)
#define YUV_BUF_INT_MASKS_ENC_SLOW       (1u << 4)
#define YUV_BUF_INT_MASKS_H264_ERR       (1u << 5)

/* yuv_buf_hal_set_err_mask(): vsync_nege 0, resl_err 1, sens_full 1,
 * enc_slow 1 (h264_err left at its reset value 0).
 */

#define YUV_BUF_INT_MASKS_DEFAULT \
  (YUV_BUF_INT_MASKS_RESL_ERR | YUV_BUF_INT_MASKS_SENS_FULL | \
   YUV_BUF_INT_MASKS_ENC_SLOW)

#define YUV_BUF_EMR_BASE_ADDR            YUV_BUF_REG(12)

#define YUV_BUF_RESIZE_PIXEL             YUV_BUF_REG(13)
#define YUV_BUF_RESIZE_PIXEL_X_SHIFT     1u
#define YUV_BUF_RESIZE_PIXEL_Y_SHIFT     9u

/* System controller registers (SOC_SYS_REG_BASE) that gate the video
 * pipeline's power and this module's clock -- required before any of
 * YUV_BUF's own registers have real effect.  Ported from
 * yuv_buf_init_common(); see file header.
 */

#define BK7258_SYS_REG_BASE              0x44010000u
#define BK7258_SYS_REG_0X10               (BK7258_SYS_REG_BASE + (0x10u << 2))
#define BK7258_SYS_PWD_VIDP                (1u << 7)

#define BK7258_SYS_REG_0X0D               (BK7258_SYS_REG_BASE + (0xdu << 2))
#define BK7258_SYS_YUV_CKEN                 (1u << 3)
#define BK7258_SYS_H264_CKEN                (1u << 0)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bk7258_yuv_buf_frame_cb_t g_frame_cb;
static bk7258_yuv_buf_frame_cb_t g_vsync_cb;
static FAR void *g_vsync_cb_arg;
static FAR void *g_frame_cb_arg;
static uint32_t g_frame_buf_addr;

/* Written by the ISR, read by task-level diagnostics. */

static volatile uint32_t g_isr_count;
static volatile uint32_t g_frame_count;
static volatile uint32_t g_vsync_count;
static volatile uint32_t g_err_count;
static volatile uint32_t g_last_status;
static volatile uint32_t g_err_status;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_yuv_buf_isr
 *
 * Description:
 *   YUV_BUF interrupt handler.  Runs in interrupt context, therefore it
 *   MUST NOT print: the previous revision of this driver called printf()
 *   here (and again in its line-batch callback), which is what actually
 *   produced the whole-system hangs that three earlier sessions
 *   misattributed to the global_ctrl.soft_reset polarity -- see
 *   bk7258_yuv_buf_init() for the full evidence trail.  Observability is
 *   provided by the counters below plus bk7258_yuv_buf_dump_status(),
 *   which task-level code calls.
 *
 ****************************************************************************/

static int bk7258_yuv_buf_isr(int irq, FAR void *context, FAR void *arg)
{
  uint32_t status = getreg32(YUV_BUF_INT_STATUS);

  /* Write-1-to-clear: write back exactly the bits read as set, including
   * any that are not consumed below, so an enabled-but-unhandled event
   * can never latch the (level-sensitive) interrupt line permanently.
   */

  putreg32(status, YUV_BUF_INT_STATUS);

  g_isr_count++;
  g_last_status = status;

  if ((status & YUV_BUF_INT_VSYNC_NEGE) != 0)
    {
      g_vsync_count++;

      /* A frame boundary.  The JPEG path uses this to recover from an
       * encoder that gave up mid-frame, which is why the callback exists
       * separately from the frame-done one: in JPEG mode no frame-done
       * event ever arrives.
       */

      if (g_vsync_cb != NULL)
        {
          g_vsync_cb(g_vsync_cb_arg);
        }
    }

  if ((status & YUV_BUF_INT_ERRORS) != 0)
    {
      g_err_count++;
      g_err_status |= status & YUV_BUF_INT_ERRORS;
    }

  if ((status & YUV_BUF_INT_YUV_ARV) != 0)
    {
      g_frame_count++;

      if (g_frame_cb != NULL)
        {
          g_frame_cb(g_frame_cb_arg);
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void bk7258_yuv_buf_init(void)
{
  uint32_t reg;
  uint32_t id_in_reset;
  uint32_t version_in_reset;
  uint32_t status_in_reset;

  /* Power on the video pipeline and enable YUV_BUF's (and H264's,
   * required alongside it even for direct-capture mode) clock gate
   * before touching this module's own registers.
   */

  reg = getreg32(BK7258_SYS_REG_0X10);
  reg &= ~BK7258_SYS_PWD_VIDP;
  putreg32(reg, BK7258_SYS_REG_0X10);

  reg = getreg32(BK7258_SYS_REG_0X0D);
  reg |= BK7258_SYS_H264_CKEN | BK7258_SYS_YUV_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0D);

  printf("bk7258_yuv_buf: init: power/clock enabled, reg0x10=0x%08x "
         "reg0x0d=0x%08x\n",
         (unsigned int)getreg32(BK7258_SYS_REG_0X10),
         (unsigned int)getreg32(BK7258_SYS_REG_0X0D));

  /* Global control, per the reference's yuv_buf_ll_init():
   *
   *   hw->global_ctrl.soft_reset = 0;
   *   hw->global_ctrl.soft_reset = 1;
   *   hw->global_ctrl.clk_gate_bypass = 1;
   *
   * i.e. soft_reset is active-low (asserting reset means writing 0) and
   * the module is left with soft_reset=1 (released) AND
   * clk_gate_bypass=1, final global_ctrl == 0x3.
   *
   * History (docs/main/2026-08-07-global-ctrl-polarity-hang-and-further-
   * elimination.md): three earlier attempts at exactly this write hung
   * the AP core and were recorded as "polarity disproven on this
   * hardware", leaving the module parked at global_ctrl == 0 -- held in
   * reset -- which is why every one of the nine interrupt sources stayed
   * silent and int_status read 0x00000000 forever.  Those hangs are now
   * attributed to this driver's own ISR (and the imgdata line-batch
   * callback it invoked) calling printf() from interrupt context:
   * releasing the reset made the interrupt fire for the first time, each
   * invocation then spent ~8ms of interrupt time pushing ~100 bytes out
   * of a 115200-baud UART -- with SM0/SM1 enabled that is ~1800
   * interrupts/s -- starving all task-level work until the CP-side IPC
   * heartbeat assert fired.  The truncated-mid-printf serial log is the
   * signature of a print from an ISR racing a task-level print, not of a
   * bus fault.  The ISR is now print-free and only the frame-rate
   * interrupts are enabled, so this sequence is safe to restore.
   *
   * If this ever hangs again, the identity/status snapshots printed
   * below are the evidence to look at: they read the module's read-only
   * ID registers with the reset asserted and again with it released,
   * which distinguishes "module is dead/unclocked" from "module is
   * alive" without needing an oscilloscope or JTAG.
   */

  putreg32(0, YUV_BUF_GLOBAL_CTRL);
  up_udelay(10);

  id_in_reset      = getreg32(YUV_BUF_DEV_ID);
  version_in_reset = getreg32(YUV_BUF_DEV_VERSION);
  status_in_reset  = getreg32(YUV_BUF_DEV_STATUS);

  putreg32(YUV_BUF_GLOBAL_CTRL_SOFT_RESET, YUV_BUF_GLOBAL_CTRL);
  up_udelay(10);
  putreg32(YUV_BUF_GLOBAL_CTRL_SOFT_RESET |
           YUV_BUF_GLOBAL_CTRL_CLK_GATE_BYPASS, YUV_BUF_GLOBAL_CTRL);
  up_udelay(10);

  printf("bk7258_yuv_buf: init: global_ctrl=0x%08x (soft_reset released, "
         "clk_gate_bypass set, per yuv_buf_ll_init())\n",
         (unsigned int)getreg32(YUV_BUF_GLOBAL_CTRL));
  printf("bk7258_yuv_buf: init: dev_id=0x%08x->0x%08x "
         "dev_version=0x%08x->0x%08x dev_status=0x%08x->0x%08x "
         "(in-reset -> released)\n",
         (unsigned int)id_in_reset,
         (unsigned int)getreg32(YUV_BUF_DEV_ID),
         (unsigned int)version_in_reset,
         (unsigned int)getreg32(YUV_BUF_DEV_VERSION),
         (unsigned int)status_in_reset,
         (unsigned int)getreg32(YUV_BUF_DEV_STATUS));

  /* Leave the module quiet until bk7258_yuv_buf_configure(): no
   * interrupt sources enabled, no stale status latched.
   */

  putreg32(0, YUV_BUF_INT_EN);
  putreg32(YUV_BUF_INT_ALL, YUV_BUF_INT_STATUS);

  irq_attach(BK7258_IRQ_YUVB, bk7258_yuv_buf_isr, NULL);
  up_enable_irq(BK7258_IRQ_YUVB);

  printf("bk7258_yuv_buf: init: complete, irq=%d attached and enabled\n",
         (int)BK7258_IRQ_YUVB);
}

void bk7258_yuv_buf_configure(uint16_t width, uint16_t height)
{
  uint32_t x_pixel = width / 8u;
  uint32_t y_pixel = height / 8u;
  uint32_t frame_blk = (x_pixel * y_pixel) / 2u;
  uint32_t ctrl;
  irqstate_t flags;

  flags = enter_critical_section();
  g_isr_count   = 0;
  g_frame_count = 0;
  g_vsync_count = 0;
  g_err_count   = 0;
  g_last_status = 0;
  g_err_status  = 0;
  leave_critical_section(flags);

  putreg32((x_pixel << YUV_BUF_PIXEL_X_SHIFT) |
           (y_pixel << YUV_BUF_PIXEL_Y_SHIFT) |
           (frame_blk << YUV_BUF_PIXEL_FRAME_BLK_SHIFT),
           YUV_BUF_PIXEL);

  /* GC2145 is YUYV with active-high HSYNC/VSYNC (PIXEL_FMT_YUYV,
   * SYNC_HIGH_LEVEL), so yuv_fmt_sel/hsync_rev/vsync_rev/bps_cis/memrev
   * all resolve to 0; mclk_div and sync_edge_dect_en/soi_hsync are the
   * fields that must be written explicitly
   * (yuv_buf_hal_set_config_common()).
   */

  ctrl = getreg32(YUV_BUF_CTRL);
  ctrl &= ~(YUV_BUF_CTRL_YUV_FMT_SEL_MASK | YUV_BUF_CTRL_HSYNC_REV |
            YUV_BUF_CTRL_VSYNC_REV | YUV_BUF_CTRL_MCLK_DIV_MASK |
            YUV_BUF_CTRL_BPS_CIS | YUV_BUF_CTRL_MEMREV |
            YUV_BUF_CTRL_H264_MODE);
  /* yuv_fmt_sel describes the byte order the SENSOR puts on the DVP
   * bus, which GC2145's register tables define as Y Cb Y Cr
   * (dvp_gc2145.c's .fmt = PIXEL_FMT_YUYV, i.e. YUV_FORMAT_YUYV = 0,
   * from register 0x84 = 0x02).
   * It is NOT the order this module writes to memory: the frame buffer
   * comes out with those four bytes reversed, Cr Y1 Cb Y0.  The V4L2
   * layer still advertises V4L2_PIX_FMT_UYVY because no fourcc names that
   * layout and the imgsensor/imgdata layers define only UYVY and YUYV.
   * See bk7258_camera_imgsensor.c's g_bk7258_gc2145_fmtdescs for the three
   * measurements behind this (a panel photograph for which bytes are luma,
   * a smoothness test for the reversal, the bus order for which chroma byte
   * is Cb -- the hexdump alone is consistent with several readings).
   *
   * If the memory order ever has to be normalised to a standard fourcc, the
   * fields to try are ctrl.bus_dat_byte_reve (bit18) and ctrl.memrev
   * (bit14); both are left at 0 here, as the vendor driver does, and
   * neither has been tested on this board.
   */

  ctrl |= (YUV_BUF_FMT_YUYV << YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT);
  ctrl |= YUV_BUF_CTRL_SYNC_EDGE_DECT_EN;
  ctrl |= (YUV_BUF_MCLK_DIV_3 << YUV_BUF_CTRL_MCLK_DIV_SHIFT);
  ctrl |= YUV_BUF_CTRL_SOI_HSYNC;
  putreg32(ctrl, YUV_BUF_CTRL);

  putreg32((x_pixel << YUV_BUF_RESIZE_PIXEL_X_SHIFT) |
           (y_pixel << YUV_BUF_RESIZE_PIXEL_Y_SHIFT),
           YUV_BUF_RESIZE_PIXEL);

  putreg32(YUV_BUF_INT_MASKS_DEFAULT, YUV_BUF_INT_MASKS);

  /* Drop anything latched while the module was being programmed, then
   * enable exactly the events whole-frame YUV capture needs.
   */

  putreg32(YUV_BUF_INT_ALL, YUV_BUF_INT_STATUS);
  putreg32(YUV_BUF_INT_EN_YUV_MODE, YUV_BUF_INT_EN);

  printf("bk7258_yuv_buf: configure(%ux%u): x_pixel=%u y_pixel=%u "
         "frame_blk=%u\n",
         (unsigned int)width, (unsigned int)height,
         (unsigned int)x_pixel, (unsigned int)y_pixel,
         (unsigned int)frame_blk);
  printf("bk7258_yuv_buf: configure: global_ctrl=0x%08x ctrl=0x%08x "
         "pixel=0x%08x resize_pixel=0x%08x int_masks=0x%08x "
         "int_en=0x%08x em_base_addr=0x%08x emr_base_addr=0x%08x\n",
         (unsigned int)getreg32(YUV_BUF_GLOBAL_CTRL),
         (unsigned int)getreg32(YUV_BUF_CTRL),
         (unsigned int)getreg32(YUV_BUF_PIXEL),
         (unsigned int)getreg32(YUV_BUF_RESIZE_PIXEL),
         (unsigned int)getreg32(YUV_BUF_INT_MASKS),
         (unsigned int)getreg32(YUV_BUF_INT_EN),
         (unsigned int)getreg32(YUV_BUF_EM_BASE_ADDR),
         (unsigned int)getreg32(YUV_BUF_EMR_BASE_ADDR));
}

void bk7258_yuv_buf_set_frame_buffer(uint32_t addr)
{
  g_frame_buf_addr = addr;

  putreg32(addr, YUV_BUF_EM_BASE_ADDR);
  putreg32(addr, YUV_BUF_EMR_BASE_ADDR);
}

uint32_t bk7258_yuv_buf_get_frame_buffer(void)
{
  return g_frame_buf_addr;
}

void bk7258_yuv_buf_set_frame_callback(bk7258_yuv_buf_frame_cb_t cb,
                                       FAR void *arg)
{
  irqstate_t flags = enter_critical_section();

  g_frame_cb = cb;
  g_frame_cb_arg = arg;

  leave_critical_section(flags);
}

void bk7258_yuv_buf_start(void)
{
  /* Drop anything latched while idle, then arm the frame events and
   * switch the module into YUV direct-capture mode
   * (yuv_buf_hal_start_yuv_mode(): enable yuv_mode, disable h264_mode).
   */

  putreg32(YUV_BUF_INT_ALL, YUV_BUF_INT_STATUS);
  putreg32(YUV_BUF_INT_EN_YUV_MODE, YUV_BUF_INT_EN);
  modifyreg32(YUV_BUF_CTRL, YUV_BUF_CTRL_H264_MODE, YUV_BUF_CTRL_YUV_MODE);
}

/****************************************************************************
 * Name: bk7258_yuv_buf_start_jpeg
 *
 * Description:
 *   Hands the sensor stream to the JPEG encoder instead of writing frames
 *   to memory.
 *
 *   The difference from bk7258_yuv_buf_start() is that yuv_mode must be
 *   *off*: this module's own frame writer and the encoder are two consumers
 *   of the same pixel stream, and the reference selects between them rather
 *   than running both (yuv_buf_hal_start_jpeg_mode() disables yuv_mode and
 *   h264_mode, then enables the JPEG block).  Leaving yuv_mode on is the
 *   obvious mistake here, and it would not look like a mode error: the
 *   module would keep filling the frame buffer while the encoder also ran.
 *
 *   The frame interrupts stay enabled: VSYNC negedge is still the marker for
 *   "sensor is alive", which the capture watchdog needs.  Completion of an
 *   encoded frame is reported by the JPEG block's own EOF interrupt, not
 *   from here.
 *
 ****************************************************************************/

void bk7258_yuv_buf_set_vsync_callback(bk7258_yuv_buf_frame_cb_t cb,
                                      FAR void *arg)
{
  g_vsync_cb_arg = arg;
  g_vsync_cb = cb;
}

/****************************************************************************
 * Name: bk7258_yuv_buf_soft_reset
 *
 * Description:
 *   Pulses the module's global soft reset, leaving it released with the
 *   clock gate still bypassed.  soft_reset is low-active: 0 holds the
 *   module in reset, so the pulse is 0 then 1 (see bk7258_yuv_buf_init()
 *   for the evidence trail on that polarity -- getting it backwards is what
 *   three earlier sessions misdiagnosed).
 *
 *   Print-free: the JPEG error recovery calls this from interrupt context
 *   (reference: bk_yuv_buf_soft_reset() inside
 *   dvp_camera_reset_hardware_modules_handler()).
 *
 ****************************************************************************/

void bk7258_yuv_buf_soft_reset(void)
{
  putreg32(0, YUV_BUF_GLOBAL_CTRL);
  putreg32(YUV_BUF_GLOBAL_CTRL_SOFT_RESET |
           YUV_BUF_GLOBAL_CTRL_CLK_GATE_BYPASS, YUV_BUF_GLOBAL_CTRL);
}

void bk7258_yuv_buf_start_jpeg(void)
{
  putreg32(YUV_BUF_INT_ALL, YUV_BUF_INT_STATUS);
  putreg32(YUV_BUF_INT_EN_YUV_MODE, YUV_BUF_INT_EN);
  modifyreg32(YUV_BUF_CTRL,
              YUV_BUF_CTRL_YUV_MODE | YUV_BUF_CTRL_H264_MODE, 0);
}

void bk7258_yuv_buf_stop(void)
{
  modifyreg32(YUV_BUF_CTRL, YUV_BUF_CTRL_YUV_MODE, 0);

  /* Leave the module silent: sync detection can keep producing vsync
   * events after yuv_mode is cleared, and there is nothing left to
   * report them to.
   */

  putreg32(0, YUV_BUF_INT_EN);
  putreg32(YUV_BUF_INT_ALL, YUV_BUF_INT_STATUS);
}

void bk7258_yuv_buf_get_stats(FAR struct bk7258_yuv_buf_stats_s *stats)
{
  irqstate_t flags;

  if (stats == NULL)
    {
      return;
    }

  flags = enter_critical_section();

  stats->isr_count   = g_isr_count;
  stats->frame_count = g_frame_count;
  stats->vsync_count = g_vsync_count;
  stats->err_count   = g_err_count;
  stats->last_status = g_last_status;
  stats->err_status  = g_err_status;

  leave_critical_section(flags);
}

void bk7258_yuv_buf_dump_status(FAR const char *tag)
{
  struct bk7258_yuv_buf_stats_s stats;

  bk7258_yuv_buf_get_stats(&stats);

  printf("bk7258_yuv_buf: [%s] global_ctrl=0x%08x ctrl=0x%08x "
         "int_en=0x%08x int_status=0x%08x em_base_addr=0x%08x "
         "dev_id=0x%08x dev_status=0x%08x\n",
         tag,
         (unsigned int)getreg32(YUV_BUF_GLOBAL_CTRL),
         (unsigned int)getreg32(YUV_BUF_CTRL),
         (unsigned int)getreg32(YUV_BUF_INT_EN),
         (unsigned int)getreg32(YUV_BUF_INT_STATUS),
         (unsigned int)getreg32(YUV_BUF_EM_BASE_ADDR),
         (unsigned int)getreg32(YUV_BUF_DEV_ID),
         (unsigned int)getreg32(YUV_BUF_DEV_STATUS));
  printf("bk7258_yuv_buf: [%s] isr=%u frame=%u vsync=%u err=%u "
         "last_status=0x%08x err_status=0x%08x\n",
         tag,
         (unsigned int)stats.isr_count,
         (unsigned int)stats.frame_count,
         (unsigned int)stats.vsync_count,
         (unsigned int)stats.err_count,
         (unsigned int)stats.last_status,
         (unsigned int)stats.err_status);
}
