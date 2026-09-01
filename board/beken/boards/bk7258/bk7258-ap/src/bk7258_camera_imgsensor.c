/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_imgsensor.c
 *
 * GC2145 sensor-side (imgsensor) driver: NuttX V4L2 imgsensor_ops_s
 * implementation.  This is the sensor device layer half of the driver
 * split described in the openvela Camera Driver Framework guide -- it
 * owns I2C register programming, power/reset sequencing, and DVP
 * pinmux/MCLK setup; the platform (YUV_BUF/DMA/full-frame-assembly)
 * half lives in board/beken/chips/bk7258/bk7258_camera_imgdata.c.
 *
 * Register tables (g_gc2145_init_regs, g_gc2145_640_480_regs) and the
 * power/reset/pinmux/MCLK sequence below are ported from bk_avdk_smp
 * release/v3.1.1 ap/components/bk_peripheral/src/dvp/dvp_gc2145.c and
 * ap/components/bk_dvp/src/dvp_common.c, cross-checked against
 * AIDK_AI玩具开发板_原理图.pdf for this board's actual pin assignment
 * (GC2145's I2C bus is bit-banged on GPIO42/43, not the SoC's hardware
 * I2C1 peripheral -- see bk7258_gc2145_i2c_bitbang.h).
 *
 * MCLK note: GPIO27 requires pinmux function index 1
 * (GPIO_DEV_CLK_AUXS_CIS), not the index-0 function the other DVP pins
 * use, plus the system controller's AUXS_CIS clock-source-select +
 * divider + gate-enable sequence -- pinmux selection alone does not
 * drive a clock signal onto the pin.  GC2145's power-up sequence
 * requires MCLK to be applied before its I2C/SCCB interface responds.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/video/imgsensor.h>
#include <sys/videoio.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"
#include "bk7258_gc2145_i2c_bitbang.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GC2145_I2C_ADDR       0x3Cu

#define DVP_MCLK_PIN          27u
#define DVP_MCLK_PINMUX_FUNCTION 1u  /* GPIO_DEV_CLK_AUXS_CIS */

#define DVP_PCLK_PIN          29u
#define DVP_HSYNC_PIN         30u
#define DVP_VSYNC_PIN         31u
#define DVP_DATA_PIN(i)       (32u + (i))
#define DVP_PINMUX_FUNCTION   0u    /* GPIO_DEV_JPEG_{PCLK,HSYNC,VSYNC,PXDATAn} */

#define DVP_RESET_PIN         28u
#define DVP_POWER_PIN         49u

/* System clock controller (SOC_SYS_REG_BASE) fields for the AUXS_CIS
 * clock path that drives GC2145's MCLK input pin (GPIO27).
 */

#define BK7258_SYS_REG_BASE             0x44010000u

#define BK7258_SYS_REG_0X0A             (BK7258_SYS_REG_BASE + (0xau << 2))
#define BK7258_SYS_CKSEL_AUXS_CIS_SHIFT 15u
#define BK7258_SYS_CKSEL_AUXS_CIS_MASK  (0x3u << BK7258_SYS_CKSEL_AUXS_CIS_SHIFT)
#define BK7258_SYS_CKDIV_AUXS_CIS_SHIFT 17u
#define BK7258_SYS_CKDIV_AUXS_CIS_MASK  (0x1fu << BK7258_SYS_CKDIV_AUXS_CIS_SHIFT)

#define BK7258_SYS_REG_0X0D             (BK7258_SYS_REG_BASE + (0xdu << 2))
#define BK7258_SYS_CIS_AUXS_CKEN        (1u << 9)

/* MCLK_24M case: cksel=3, ckdiv=19. */

#define GC2145_MCLK_CKSEL               3u
#define GC2145_MCLK_CKDIV               19u

/* Index into g_gc2145_modes of the mode programmed at init: 640x480, the
 * geometry the whole capture path was brought up and measured on.  Kept as
 * an index rather than a width/height pair so there is exactly one place
 * that decides it.
 */

#define GC2145_DEFAULT_MODE    1u

/* GC2145 identity registers, per dvp_gc2145.c's CHIP_ID_ADDR_HB/LB and
 * CHIP_ID_VAL_HB/LB.  Readable only after power-on + MCLK, which is why
 * the check lives in init() rather than in is_available().
 */

#define GC2145_CHIP_ID_REG_HI  0xF0u
#define GC2145_CHIP_ID_REG_LO  0xF1u
#define GC2145_CHIP_ID_VAL_HI  0x21u
#define GC2145_CHIP_ID_VAL_LO  0x45u

/* Frame rates this driver can program at 640x480.  The reference's four
 * tables (sensor_gc2145_640_480_{30,25,20,15}fps_table in dvp_gc2145.c)
 * are byte-identical except for registers 0x07/0x08 -- the frame-length
 * (dummy line) high/low pair -- so they are represented here as one
 * shared table plus the per-rate 0x07/0x08 values, rather than four
 * near-duplicate copies.
 */

#define GC2145_FPS_DEFAULT     30u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gc2145_reg
{
  uint8_t reg;
  uint8_t value;
};

struct bk7258_gc2145_dev_s
{
  struct imgsensor_s sensor;    /* Must be first: base-pointer cast. */
  bool initialized;
  uint32_t current_fps;         /* Rate programmed, 0 = fixed by the mode. */
  FAR const struct gc2145_mode *mode;   /* Resolution programmed. */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool bk7258_gc2145_is_available(FAR struct imgsensor_s *sensor);
static int  bk7258_gc2145_init(FAR struct imgsensor_s *sensor);
static int  bk7258_gc2145_uninit(FAR struct imgsensor_s *sensor);
static FAR const char *
bk7258_gc2145_get_driver_name(FAR struct imgsensor_s *sensor);
static int  bk7258_gc2145_validate_frame_setting(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval);
static int  bk7258_gc2145_start_capture(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval);
static int  bk7258_gc2145_stop_capture(FAR struct imgsensor_s *sensor,
                                        imgsensor_stream_type_t type);
static int  bk7258_gc2145_get_frame_interval(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    FAR imgsensor_interval_t *interval);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Full GC2145 initialization register table (585 entries), ported
 * verbatim from dvp_gc2145.c.
 */

static const struct gc2145_reg g_gc2145_init_regs[] =
{
  { 0xFE, 0xF0 },
  { 0xFE, 0xF0 },
  { 0xFE, 0xF0 },
  { 0xFC, 0x06 },
  { 0xF6, 0x00 },
  { 0xF7, 0x1D },
  { 0xF8, 0x84 },
  { 0xFA, 0x00 },
  { 0xF9, 0xFE },
  { 0xF2, 0x00 },
  { 0xFE, 0x00 },
  { 0x03, 0x04 },
  { 0x04, 0xE2 },
  { 0x09, 0x00 },
  { 0x0A, 0x00 },
  { 0x0B, 0x00 },
  { 0x0C, 0x00 },
  { 0x0D, 0x04 },
  { 0x0E, 0xC0 },
  { 0x0F, 0x06 },
  { 0x10, 0x52 },
  { 0x12, 0x2E },
  { 0x17, 0x14 },
  { 0x18, 0x22 },
  { 0x19, 0x0E },
  { 0x1A, 0x01 },
  { 0x1B, 0x4B },
  { 0x1C, 0x07 },
  { 0x1D, 0x10 },
  { 0x1E, 0x88 },
  { 0x1F, 0x78 },
  { 0x20, 0x03 },
  { 0x21, 0x40 },
  { 0x22, 0xA0 },
  { 0x24, 0x16 },
  { 0x25, 0x01 },
  { 0x26, 0x10 },
  { 0x2D, 0x60 },
  { 0x30, 0x01 },
  { 0x31, 0x90 },
  { 0x33, 0x06 },
  { 0x34, 0x01 },
  { 0xFE, 0x00 },
  { 0x80, 0x7F },
  { 0x81, 0x26 },
  { 0x82, 0xFA },
  { 0x83, 0x00 },
  { 0x84, 0x02 },
  { 0x86, 0x03 },
  { 0x88, 0x03 },
  { 0x89, 0x03 },
  { 0x85, 0x08 },
  { 0x8A, 0x00 },
  { 0x8B, 0x00 },
  { 0xB0, 0x55 },
  { 0xC3, 0x00 },
  { 0xC4, 0x80 },
  { 0xC5, 0x90 },
  { 0xC6, 0x3B },
  { 0xC7, 0x46 },
  { 0xEC, 0x06 },
  { 0xED, 0x04 },
  { 0xEE, 0x60 },
  { 0xEF, 0x90 },
  { 0xB6, 0x01 },
  { 0x90, 0x01 },
  { 0x91, 0x00 },
  { 0x92, 0x00 },
  { 0x93, 0x00 },
  { 0x94, 0x00 },
  { 0x95, 0x04 },
  { 0x96, 0xB0 },
  { 0x97, 0x06 },
  { 0x98, 0x40 },
  { 0xFE, 0x00 },
  { 0x40, 0x42 },
  { 0x41, 0x00 },
  { 0x43, 0x5B },
  { 0x5E, 0x00 },
  { 0x5F, 0x00 },
  { 0x60, 0x00 },
  { 0x61, 0x00 },
  { 0x62, 0x00 },
  { 0x63, 0x00 },
  { 0x64, 0x00 },
  { 0x65, 0x00 },
  { 0x66, 0x20 },
  { 0x67, 0x20 },
  { 0x68, 0x20 },
  { 0x69, 0x20 },
  { 0x76, 0x00 },
  { 0x6A, 0x08 },
  { 0x6B, 0x08 },
  { 0x6C, 0x08 },
  { 0x6D, 0x08 },
  { 0x6E, 0x08 },
  { 0x6F, 0x08 },
  { 0x70, 0x08 },
  { 0x71, 0x08 },
  { 0x76, 0x00 },
  { 0x72, 0xF0 },
  { 0x7E, 0x3C },
  { 0x7F, 0x00 },
  { 0xFE, 0x02 },
  { 0x48, 0x15 },
  { 0x49, 0x00 },
  { 0x4B, 0x0B },
  { 0xFE, 0x00 },
  { 0xFE, 0x01 },
  { 0x01, 0x04 },
  { 0x02, 0xC0 },
  { 0x03, 0x04 },
  { 0x04, 0x90 },
  { 0x05, 0x30 },
  { 0x06, 0x90 },
  { 0x07, 0x30 },
  { 0x08, 0x80 },
  { 0x09, 0x00 },
  { 0x0A, 0x82 },
  { 0x0B, 0x11 },
  { 0x0C, 0x10 },
  { 0x11, 0x10 },
  { 0x13, 0x7B },
  { 0x17, 0x00 },
  { 0x1C, 0x11 },
  { 0x1E, 0x61 },
  { 0x1F, 0x35 },
  { 0x20, 0x40 },
  { 0x22, 0x40 },
  { 0x23, 0x20 },
  { 0xFE, 0x02 },
  { 0x0F, 0x04 },
  { 0xFE, 0x01 },
  { 0x12, 0x35 },
  { 0x15, 0xB0 },
  { 0x10, 0x31 },
  { 0x3E, 0x28 },
  { 0x3F, 0xB0 },
  { 0x40, 0x90 },
  { 0x41, 0x0F },
  { 0xFE, 0x02 },
  { 0x90, 0x6C },
  { 0x91, 0x03 },
  { 0x92, 0xCB },
  { 0x94, 0x33 },
  { 0x95, 0x84 },
  { 0x97, 0x65 },
  { 0xA2, 0x11 },
  { 0xFE, 0x00 },
  { 0xFE, 0x02 },
  { 0x80, 0xC1 },
  { 0x81, 0x08 },
  { 0x82, 0x05 },
  { 0x83, 0x08 },
  { 0x84, 0x0A },
  { 0x86, 0xF0 },
  { 0x87, 0x50 },
  { 0x88, 0x15 },
  { 0x89, 0xB0 },
  { 0x8A, 0x30 },
  { 0x8B, 0x10 },
  { 0xFE, 0x01 },
  { 0x21, 0x04 },
  { 0xFE, 0x02 },
  { 0xA3, 0x50 },
  { 0xA4, 0x20 },
  { 0xA5, 0x40 },
  { 0xA6, 0x80 },
  { 0xAB, 0x40 },
  { 0xAE, 0x0C },
  { 0xB3, 0x46 },
  { 0xB4, 0x64 },
  { 0xB6, 0x38 },
  { 0xB7, 0x01 },
  { 0xB9, 0x2B },
  { 0x3C, 0x04 },
  { 0x3D, 0x15 },
  { 0x4B, 0x06 },
  { 0x4C, 0x20 },
  { 0xFE, 0x00 },
  { 0xFE, 0x02 },
  { 0x10, 0x09 },
  { 0x11, 0x0D },
  { 0x12, 0x13 },
  { 0x13, 0x19 },
  { 0x14, 0x27 },
  { 0x15, 0x37 },
  { 0x16, 0x45 },
  { 0x17, 0x53 },
  { 0x18, 0x69 },
  { 0x19, 0x7D },
  { 0x1A, 0x8F },
  { 0x1B, 0x9D },
  { 0x1C, 0xA9 },
  { 0x1D, 0xBD },
  { 0x1E, 0xCD },
  { 0x1F, 0xD9 },
  { 0x20, 0xE3 },
  { 0x21, 0xEA },
  { 0x22, 0xEF },
  { 0x23, 0xF5 },
  { 0x24, 0xF9 },
  { 0x25, 0xFF },
  { 0xFE, 0x00 },
  { 0xC6, 0x20 },
  { 0xC7, 0x2B },
  { 0xFE, 0x02 },
  { 0x26, 0x0F },
  { 0x27, 0x14 },
  { 0x28, 0x19 },
  { 0x29, 0x1E },
  { 0x2A, 0x27 },
  { 0x2B, 0x33 },
  { 0x2C, 0x3B },
  { 0x2D, 0x45 },
  { 0x2E, 0x59 },
  { 0x2F, 0x69 },
  { 0x30, 0x7C },
  { 0x31, 0x89 },
  { 0x32, 0x98 },
  { 0x33, 0xAE },
  { 0x34, 0xC0 },
  { 0x35, 0xCF },
  { 0x36, 0xDA },
  { 0x37, 0xE2 },
  { 0x38, 0xE9 },
  { 0x39, 0xF3 },
  { 0x3A, 0xF9 },
  { 0x3B, 0xFF },
  { 0xFE, 0x02 },
  { 0xD1, 0x32 },
  { 0xD2, 0x32 },
  { 0xD3, 0x40 },
  { 0xD6, 0xF0 },
  { 0xD7, 0x10 },
  { 0xD8, 0xDA },
  { 0xDD, 0x14 },
  { 0xDE, 0x86 },
  { 0xED, 0x80 },
  { 0xEE, 0x00 },
  { 0xEF, 0x3F },
  { 0xD8, 0xD8 },
  { 0xFE, 0x01 },
  { 0x9F, 0x40 },
  { 0xFE, 0x01 },
  { 0xC2, 0x14 },
  { 0xC3, 0x0D },
  { 0xC4, 0x0C },
  { 0xC8, 0x15 },
  { 0xC9, 0x0D },
  { 0xCA, 0x0A },
  { 0xBC, 0x24 },
  { 0xBD, 0x10 },
  { 0xBE, 0x0B },
  { 0xB6, 0x25 },
  { 0xB7, 0x16 },
  { 0xB8, 0x15 },
  { 0xC5, 0x00 },
  { 0xC6, 0x00 },
  { 0xC7, 0x00 },
  { 0xCB, 0x00 },
  { 0xCC, 0x00 },
  { 0xCD, 0x00 },
  { 0xBF, 0x07 },
  { 0xC0, 0x00 },
  { 0xC1, 0x00 },
  { 0xB9, 0x00 },
  { 0xBA, 0x00 },
  { 0xBB, 0x00 },
  { 0xAA, 0x01 },
  { 0xAB, 0x01 },
  { 0xAC, 0x00 },
  { 0xAD, 0x05 },
  { 0xAE, 0x06 },
  { 0xAF, 0x0E },
  { 0xB0, 0x0B },
  { 0xB1, 0x07 },
  { 0xB2, 0x06 },
  { 0xB3, 0x17 },
  { 0xB4, 0x0E },
  { 0xB5, 0x0E },
  { 0xD0, 0x09 },
  { 0xD1, 0x00 },
  { 0xD2, 0x00 },
  { 0xD6, 0x08 },
  { 0xD7, 0x00 },
  { 0xD8, 0x00 },
  { 0xD9, 0x00 },
  { 0xDA, 0x00 },
  { 0xDB, 0x00 },
  { 0xD3, 0x0A },
  { 0xD4, 0x00 },
  { 0xD5, 0x00 },
  { 0xA4, 0x00 },
  { 0xA5, 0x00 },
  { 0xA6, 0x77 },
  { 0xA7, 0x77 },
  { 0xA8, 0x77 },
  { 0xA9, 0x77 },
  { 0xA1, 0x80 },
  { 0xA2, 0x80 },
  { 0xFE, 0x01 },
  { 0xDF, 0x0D },
  { 0xDC, 0x25 },
  { 0xDD, 0x30 },
  { 0xE0, 0x77 },
  { 0xE1, 0x80 },
  { 0xE2, 0x77 },
  { 0xE3, 0x90 },
  { 0xE6, 0x90 },
  { 0xE7, 0xA0 },
  { 0xE8, 0x90 },
  { 0xE9, 0xA0 },
  { 0xFE, 0x00 },
  { 0xFE, 0x01 },
  { 0x4F, 0x00 },
  { 0x4F, 0x00 },
  { 0x4B, 0x01 },
  { 0x4F, 0x00 },
  { 0x4C, 0x01 },
  { 0x4D, 0x71 },
  { 0x4E, 0x01 },
  { 0x4C, 0x01 },
  { 0x4D, 0x91 },
  { 0x4E, 0x01 },
  { 0x4C, 0x01 },
  { 0x4D, 0x70 },
  { 0x4E, 0x01 },
  { 0x4C, 0x01 },
  { 0x4D, 0x90 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xB0 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8F },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6F },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAF },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xD0 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xF0 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCF },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xEF },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6E },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8E },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAE },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCE },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x4D },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6D },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8D },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAD },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCD },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x4C },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6C },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8C },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAC },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCC },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCB },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x4B },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6B },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8B },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAB },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8A },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAA },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCA },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCA },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xC9 },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8A },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0x89 },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xA9 },
  { 0x4E, 0x04 },
  { 0x4C, 0x02 },
  { 0x4D, 0x0B },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x0A },
  { 0x4E, 0x05 },
  { 0x4C, 0x01 },
  { 0x4D, 0xEB },
  { 0x4E, 0x05 },
  { 0x4C, 0x01 },
  { 0x4D, 0xEA },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x09 },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x29 },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x2A },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x4A },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x8A },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x49 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x69 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x89 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0xA9 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x48 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x68 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x69 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0xCA },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xC9 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xE9 },
  { 0x4E, 0x07 },
  { 0x4C, 0x03 },
  { 0x4D, 0x09 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xC8 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xE8 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xA7 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xC7 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xE7 },
  { 0x4E, 0x07 },
  { 0x4C, 0x03 },
  { 0x4D, 0x07 },
  { 0x4E, 0x07 },
  { 0x4F, 0x01 },
  { 0x50, 0x80 },
  { 0x51, 0xA8 },
  { 0x52, 0x47 },
  { 0x53, 0x38 },
  { 0x54, 0xC7 },
  { 0x56, 0x0E },
  { 0x58, 0x08 },
  { 0x5B, 0x00 },
  { 0x5C, 0x74 },
  { 0x5D, 0x8B },
  { 0x61, 0xDB },
  { 0x62, 0xB8 },
  { 0x63, 0x86 },
  { 0x64, 0xC0 },
  { 0x65, 0x04 },
  { 0x67, 0xA8 },
  { 0x68, 0xB0 },
  { 0x69, 0x00 },
  { 0x6A, 0xA8 },
  { 0x6B, 0xB0 },
  { 0x6C, 0xAF },
  { 0x6D, 0x8B },
  { 0x6E, 0x50 },
  { 0x6F, 0x18 },
  { 0x73, 0xF0 },
  { 0x70, 0x0D },
  { 0x71, 0x60 },
  { 0x72, 0x80 },
  { 0x74, 0x01 },
  { 0x75, 0x01 },
  { 0x7F, 0x0C },
  { 0x76, 0x70 },
  { 0x77, 0x58 },
  { 0x78, 0xA0 },
  { 0x79, 0x5E },
  { 0x7A, 0x54 },
  { 0x7B, 0x58 },
  { 0xFE, 0x00 },
  { 0xFE, 0x02 },
  { 0xC0, 0x01 },
  { 0xC1, 0x44 },
  { 0xC2, 0xFD },
  { 0xC3, 0x04 },
  { 0xC4, 0xF0 },
  { 0xC5, 0x48 },
  { 0xC6, 0xFD },
  { 0xC7, 0x46 },
  { 0xC8, 0xFD },
  { 0xC9, 0x02 },
  { 0xCA, 0xE0 },
  { 0xCB, 0x45 },
  { 0xCC, 0xEC },
  { 0xCD, 0x48 },
  { 0xCE, 0xF0 },
  { 0xCF, 0xF0 },
  { 0xE3, 0x0C },
  { 0xE4, 0x4B },
  { 0xE5, 0xE0 },
  { 0xFE, 0x01 },
  { 0x9F, 0x40 },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xF2, 0x0F },
  { 0xFE, 0x02 },
  { 0x40, 0xBF },
  { 0x46, 0xCF },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0x24, 0xFF },
  { 0xFE, 0x00 },
};

#define GC2145_INIT_REG_COUNT \
  (sizeof(g_gc2145_init_regs) / sizeof(g_gc2145_init_regs[0]))

/* Sensor modes.
 *
 * Every table below is the reference driver's own (dvp_gc2145.c): these are
 * the sensor vendor's numbers for window, sub-sampling and crop, and there
 * is no independent source for them.  They were extracted mechanically
 * rather than retyped -- the extractor was checked by having it reproduce
 * the 640x480 table this driver already carried, byte for byte.
 *
 * Which resolutions appear here is decided by what can be described
 * honestly.  The reference programs a frame rate by dispatching on the
 * output width it reads back from registers 0x97/0x98, and it only handles
 * widths 1600, 1280, 864 and 640.  For 480 and 800 wide it programs no rate
 * at all: those modes run at whatever their own window table fixes.  So
 * 480x320 and 800x480 are left out entirely (nothing true could be reported
 * for VIDIOC_ENUM_FRAMEINTERVALS), while 480x480 is kept -- it is the one
 * mode that maps 1:1 onto the round 160x160 panels with no cropping -- and
 * is marked as having no programmable rate.
 */

/* 480x480 window/subsample table (41 entries), transcribed from the
 * reference's sensor_gc2145_480_480_table.
 */

static const struct gc2145_reg g_gc2145_480_480_regs[] =
{
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xF8, 0x85 },
  { 0xFA, 0x11 },
  { 0xFE, 0x00 },
  { 0x09, 0x00 },
  { 0x0A, 0x78 },
  { 0x0B, 0x01 },
  { 0x0C, 0x40 },
  { 0x0D, 0x03 },
  { 0x0E, 0xD0 },
  { 0x0F, 0x03 },
  { 0x10, 0xD0 },
  { 0xFD, 0x00 },
  { 0x90, 0x01 },
  { 0x91, 0x00 },
  { 0x92, 0x00 },
  { 0x93, 0x00 },
  { 0x94, 0x00 },
  { 0x95, 0x01 },
  { 0x96, 0xE0 },
  { 0x97, 0x01 },
  { 0x98, 0xE0 },
  { 0x99, 0x22 },
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x00 },
  { 0x08, 0xA0 },
  { 0xFE, 0x01 },
  { 0x25, 0x01 },
  { 0x26, 0x63 },
  { 0x27, 0x04 },
  { 0x28, 0x29 },
  { 0x29, 0x04 },
  { 0x2A, 0x29 },
  { 0x2B, 0x04 },
  { 0x2C, 0x29 },
  { 0x2D, 0x04 },
  { 0x2E, 0x29 },
  { 0xFE, 0x00 },
};

/* 640x480 window/subsample table (40 entries), transcribed from the
 * reference's sensor_gc2145_640_480_table.
 */

static const struct gc2145_reg g_gc2145_640_480_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x00 },
  { 0x08, 0x32 },
  { 0xFE, 0x01 },
  { 0x25, 0x00 },
  { 0x26, 0xFA },
  { 0x27, 0x04 },
  { 0x28, 0xE2 },
  { 0x29, 0x04 },
  { 0x2A, 0xE2 },
  { 0x2B, 0x04 },
  { 0x2C, 0xE2 },
  { 0x2D, 0x04 },
  { 0x2E, 0xE2 },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xF8, 0x85 },
  { 0xFA, 0x00 },
  { 0xFE, 0x00 },
  { 0x09, 0x00 },
  { 0x0A, 0x78 },
  { 0x0B, 0x00 },
  { 0x0C, 0xA0 },
  { 0x0D, 0x03 },
  { 0x0E, 0xD0 },
  { 0x0F, 0x05 },
  { 0x10, 0x10 },
  { 0xFD, 0x01 },
  { 0x90, 0x01 },
  { 0x91, 0x00 },
  { 0x92, 0x00 },
  { 0x93, 0x00 },
  { 0x94, 0x00 },
  { 0x95, 0x01 },
  { 0x96, 0xE0 },
  { 0x97, 0x02 },
  { 0x98, 0x80 },
};

/* 864x480 window/subsample table (45 entries), transcribed from the
 * reference's sensor_gc2145_864_480_table.
 */

static const struct gc2145_reg g_gc2145_864_480_regs[] =
{
  { 0xFE, 0x00 },
  { 0xFD, 0x00 },
  { 0xFA, 0x11 },
  { 0xFE, 0x00 },
  { 0x09, 0x01 },
  { 0x0A, 0x70 },
  { 0x0B, 0x01 },
  { 0x0C, 0x68 },
  { 0x0D, 0x02 },
  { 0x0E, 0x40 },
  { 0x0F, 0x03 },
  { 0x10, 0xE0 },
  { 0x90, 0x01 },
  { 0x91, 0x00 },
  { 0x92, 0x00 },
  { 0x93, 0x00 },
  { 0x94, 0x00 },
  { 0x95, 0x01 },
  { 0x96, 0xE0 },
  { 0x97, 0x03 },
  { 0x98, 0x60 },
  { 0x99, 0x11 },
  { 0x9A, 0x06 },
  { 0xFE, 0x00 },
  { 0xEC, 0x06 },
  { 0xED, 0x04 },
  { 0xEE, 0x60 },
  { 0xEF, 0x90 },
  { 0xFE, 0x01 },
  { 0x74, 0x01 },
  { 0xFE, 0x01 },
  { 0x01, 0x04 },
  { 0x02, 0xC0 },
  { 0x03, 0x04 },
  { 0x04, 0x90 },
  { 0x05, 0x30 },
  { 0x06, 0x90 },
  { 0x07, 0x30 },
  { 0x08, 0x80 },
  { 0x0A, 0x82 },
  { 0xFE, 0x01 },
  { 0x21, 0x15 },
  { 0xFE, 0x00 },
  { 0x20, 0x15 },
  { 0xFE, 0x00 },
};

/* 640x480 at 30 fps (17 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_640_480_30fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x00 },
  { 0x08, 0xA0 },
  { 0xFE, 0x01 },
  { 0x25, 0x01 },
  { 0x26, 0x63 },
  { 0x27, 0x04 },
  { 0x28, 0x29 },
  { 0x29, 0x04 },
  { 0x2A, 0x29 },
  { 0x2B, 0x04 },
  { 0x2C, 0x29 },
  { 0x2D, 0x04 },
  { 0x2E, 0x29 },
  { 0xFE, 0x00 },
};

/* 640x480 at 25 fps (17 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_640_480_25fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x01 },
  { 0x08, 0x7A },
  { 0xFE, 0x01 },
  { 0x25, 0x01 },
  { 0x26, 0x63 },
  { 0x27, 0x04 },
  { 0x28, 0x29 },
  { 0x29, 0x04 },
  { 0x2A, 0x29 },
  { 0x2B, 0x04 },
  { 0x2C, 0x29 },
  { 0x2D, 0x04 },
  { 0x2E, 0x29 },
  { 0xFE, 0x00 },
};

/* 640x480 at 20 fps (17 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_640_480_20fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x03 },
  { 0x08, 0x02 },
  { 0xFE, 0x01 },
  { 0x25, 0x01 },
  { 0x26, 0x63 },
  { 0x27, 0x04 },
  { 0x28, 0x29 },
  { 0x29, 0x04 },
  { 0x2A, 0x29 },
  { 0x2B, 0x04 },
  { 0x2C, 0x29 },
  { 0x2D, 0x04 },
  { 0x2E, 0x29 },
  { 0xFE, 0x00 },
};

/* 640x480 at 15 fps (17 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_640_480_15fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x05 },
  { 0x08, 0x50 },
  { 0xFE, 0x01 },
  { 0x25, 0x01 },
  { 0x26, 0x63 },
  { 0x27, 0x04 },
  { 0x28, 0x29 },
  { 0x29, 0x04 },
  { 0x2A, 0x29 },
  { 0x2B, 0x04 },
  { 0x2C, 0x29 },
  { 0x2D, 0x04 },
  { 0x2E, 0x29 },
  { 0xFE, 0x00 },
};

/* 864x480 at 25 fps (16 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_864_480_25fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x00 },
  { 0x08, 0x50 },
  { 0xFE, 0x01 },
  { 0x25, 0x00 },
  { 0x26, 0xFA },
  { 0x27, 0x04 },
  { 0x28, 0xE2 },
  { 0x29, 0x04 },
  { 0x2A, 0xE2 },
  { 0x2B, 0x04 },
  { 0x2C, 0xE2 },
  { 0x2D, 0x04 },
  { 0x2E, 0xE2 },
};

/* 864x480 at 20 fps (16 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_864_480_20fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x01 },
  { 0x08, 0x00 },
  { 0xFE, 0x01 },
  { 0x25, 0x00 },
  { 0x26, 0xFA },
  { 0x27, 0x04 },
  { 0x28, 0xE2 },
  { 0x29, 0x04 },
  { 0x2A, 0xE2 },
  { 0x2B, 0x04 },
  { 0x2C, 0xE2 },
  { 0x2D, 0x04 },
  { 0x2E, 0xE2 },
};

/* 864x480 at 15 fps (16 entries): frame length (page 0 0x07/0x08) and the
 * AEC step table (page 1 0x25..0x2E) that belongs with it.  Writing only the
 * frame length, as an earlier revision did, leaves the exposure ladder set
 * for a different rate.
 */

static const struct gc2145_reg g_gc2145_864_480_15fps_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x02 },
  { 0x08, 0x08 },
  { 0xFE, 0x01 },
  { 0x25, 0x00 },
  { 0x26, 0xFA },
  { 0x27, 0x04 },
  { 0x28, 0xE2 },
  { 0x29, 0x04 },
  { 0x2A, 0xE2 },
  { 0x2B, 0x04 },
  { 0x2C, 0xE2 },
  { 0x2D, 0x04 },
  { 0x2E, 0xE2 },
};

/* One selectable frame rate: the register writes that produce it. */

struct gc2145_fps_mode
{
  uint32_t fps;
  FAR const struct gc2145_reg *regs;
  size_t nregs;
};

/* One selectable resolution, with the rates programmable at it.
 *
 * nfps == 0 means the rate is whatever the window table fixes; the driver
 * then programs no rate and reports no interval, rather than claiming one it
 * cannot deliver.
 */

struct gc2145_mode
{
  uint16_t width;
  uint16_t height;
  FAR const struct gc2145_reg *regs;
  size_t nregs;
  FAR const struct gc2145_fps_mode *fps;
  size_t nfps;
};

#define GC2145_REGS(t)  (t), (sizeof(t) / sizeof((t)[0]))

static const struct gc2145_fps_mode g_gc2145_640_480_fps[] =
{
  { 30, GC2145_REGS(g_gc2145_640_480_30fps_regs) },
  { 25, GC2145_REGS(g_gc2145_640_480_25fps_regs) },
  { 20, GC2145_REGS(g_gc2145_640_480_20fps_regs) },
  { 15, GC2145_REGS(g_gc2145_640_480_15fps_regs) },
};

static const struct gc2145_fps_mode g_gc2145_864_480_fps[] =
{
  { 25, GC2145_REGS(g_gc2145_864_480_25fps_regs) },
  { 20, GC2145_REGS(g_gc2145_864_480_20fps_regs) },
  { 15, GC2145_REGS(g_gc2145_864_480_15fps_regs) },
};

/* Ordered smallest first so index 0 of VIDIOC_ENUM_FRAMESIZES is the
 * cheapest mode.  640x480 stays the driver's default because it is the mode
 * the whole capture path was brought up and measured on.
 *
 * 1280x720 and 1600x1200 are deliberately absent even though their register
 * tables are above.  1280x720 was tried on hardware and wedges the board:
 * capture starts, then the AP stops responding and the CP's IPC watchdog
 * fires ("AP link down" followed by "IPC[1]heartbeat timeout" and an assert
 * in mb_ipc_task), taking the whole system down.  It is not an allocation
 * failure -- three 1280x720 buffers are 5,529,600 bytes against a 5,701,632
 * byte display pool, and a pool that cannot serve a request returns NULL,
 * which REQBUFS reports cleanly as -ENOMEM.
 *
 * The suspect is sustained PSRAM write bandwidth: 640x480 works out at about
 * 37MB/s (614400 bytes x the ~60 frames/s the hardware actually delivers),
 * and 1280x720 at the same frame rate would be ~105MB/s, with the CP sharing
 * that bus.  Which points at the other open question -- the frame rate the
 * sensor delivers is about twice what these tables program (30fps programmed,
 * 59.8 frames/s counted, and the frame callback fires once per frame on
 * YUV_ARV only).  Until that is understood, a mode that can hang the board
 * has no business being advertised as a capability.
 */

static const struct gc2145_mode g_gc2145_modes[] =
{
  {
    480, 480, GC2145_REGS(g_gc2145_480_480_regs), NULL, 0
  },
  {
    640, 480, GC2145_REGS(g_gc2145_640_480_regs),
    g_gc2145_640_480_fps,
    sizeof(g_gc2145_640_480_fps) / sizeof(g_gc2145_640_480_fps[0])
  },
  {
    864, 480, GC2145_REGS(g_gc2145_864_480_regs),
    g_gc2145_864_480_fps,
    sizeof(g_gc2145_864_480_fps) / sizeof(g_gc2145_864_480_fps[0])
  },
};

#define GC2145_MODE_COUNT \
  (sizeof(g_gc2145_modes) / sizeof(g_gc2145_modes[0]))

static const struct imgsensor_ops_s g_bk7258_gc2145_ops =
{
  .is_available           = bk7258_gc2145_is_available,
  .init                   = bk7258_gc2145_init,
  .uninit                 = bk7258_gc2145_uninit,
  .get_driver_name        = bk7258_gc2145_get_driver_name,
  .validate_frame_setting = bk7258_gc2145_validate_frame_setting,
  .start_capture          = bk7258_gc2145_start_capture,
  .stop_capture           = bk7258_gc2145_stop_capture,
  .get_frame_interval     = bk7258_gc2145_get_frame_interval,
};

/* Static capability descriptors: single discrete resolution/format,
 * matching this driver's single register table (no runtime resolution
 * switching).
 */

/* V4L2_PIX_FMT_UYVY is a label of convenience, not the exact layout.
 *
 * What YUV_BUF actually writes into the frame buffer is the four bus bytes
 * of each pixel pair stored *backwards*:
 *
 *   DVP bus:  Y0 Cb Y1 Cr      (GC2145 register 0x84 = 0x02, "YCbYCr")
 *   memory:   Cr Y1 Cb Y0
 *
 * so byte0 is Cr, byte2 is Cb, byte3 is the left pixel's luma and byte1 the
 * right pixel's.  Three separate measurements pin that down; keep them
 * distinct, because the first one alone was mistaken for all three:
 *
 *  1. Luma is at bytes 1 and 3.  Decoding with luma at bytes 0/2 was run on
 *     hardware and photographed: geometrically correct, clearly
 *     recognisable, but every pixel saturated green or magenta -- the
 *     arithmetic signature of feeding chroma into the luma term (derivation
 *     in camera_preview_main.c's preview_convert()).  A captured frame
 *     agrees: bytes 1/3 have sd 39 and span 18..153, bytes 0/2 have sd 6.2
 *     and 2.3 about 128.
 *
 *  2. The two luma samples are stored in reverse column order.  Natural
 *     images are equally smooth across every pixel boundary, so mean |dY|
 *     within a group must equal mean |dY| across the group boundary.  On a
 *     captured frame it was 1.752 within versus 3.787 across (2.16x);
 *     un-swapping the two luma bytes gives 1.752 versus 1.760, matching the
 *     1.621 the same measurement gives vertically, where no byte order is
 *     involved.  The asymmetry is what a pair read out backwards looks like:
 *     the "across" difference then spans three columns instead of one.
 *
 *  3. Therefore byte0 is Cr.  Given the bus order above (Beken's own table
 *     comments 0x84 = 0x02 as "yuyv"; Zephyr's GC2145 driver maps
 *     VIDEO_PIX_FMT_YUYV to GC2145_REG_OUTPUT_FMT_YCBYCR = 0x02), a group
 *     whose chroma sits at bytes 0/2 *and* whose luma is reversed is exactly
 *     the bus bytes reversed, which puts Cr first.
 *
 * Confirmed on the glass, 2026-08-11, by running all three decodes: the order
 * above gives correct colour, textbook UYVY gives cyan skin tones with
 * otherwise correct structure and brightness (the Cb/Cr swap), and textbook
 * YUYV gives fluorescent green/magenta (chroma in the luma term).  Each
 * observation falsifies a different candidate, which is why all three were
 * run rather than just the default.
 *
 * The fourcc stays UYVY because the imgsensor and imgdata layers define only
 * UYVY and YUYV (include/nuttx/video/imgsensor.h): VYUY, the closest true
 * name, has no IMGSENSOR_PIX_FMT_* and would need changes in
 * drivers/video/v4l2_cap.c, i.e. outside this board's adaptation.  UYVY is
 * read as "packed 4:2:2, chroma byte first" and the exact order lives here
 * and in the application's decoder.
 *
 * Two traps to avoid repeating:
 *
 *  1. A command recorded in a document is not a measurement.  An earlier
 *     round flipped this to YUYV citing `ffmpeg -pix_fmt yuyv422` from
 *     docs/main/2026-08-10-capture-success-and-prior-doc-limitations.md
 *     §4.1, while §4.2 of that same document lists "YUYV 字节序" as NOT yet
 *     verified.
 *  2. This descriptor and the application must agree or VIDIOC_S_FMT fails,
 *     but agreement is not correctness.  Both said UYVY, then both YUYV,
 *     then both UYVY, and V4L2 accepted every time; meanwhile Cb and Cr
 *     stayed swapped, which shows as slightly wrong hues rather than an
 *     obviously broken picture.  The only authority is the hardware.
 *
 * Note ctrl.yuv_fmt_sel stays at YUV_FORMAT_YUYV (0) in bk7258_yuv_buf.c:
 * that field describes the order the sensor puts on the DVP bus, which is a
 * separate thing from the order the module writes to memory.  If the memory
 * order itself ever needs to be normalised, the candidates are the module's
 * ctrl.bus_dat_byte_reve (bit18) and ctrl.memrev (bit14) -- untested here,
 * and testable only on hardware.
 */

static const struct v4l2_fmtdesc g_bk7258_gc2145_fmtdescs[] =
{
  {
    .index = 0,
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixelformat = V4L2_PIX_FMT_UYVY,
  },
  {
    .index = 1,
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixelformat = V4L2_PIX_FMT_JPEG,
    .description = "JPEG",
    .flags = V4L2_FMT_FLAG_COMPRESSED,
  },
};

static const struct v4l2_frmsizeenum g_bk7258_gc2145_frmsizes[] =
{
  {
    .index = 0,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = 480,
        .height = 480,
      },
  },
  {
    .index = 1,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = 640,
        .height = 480,
      },
  },
  {
    .index = 2,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = 864,
        .height = 480,
      },
  },
  {
    .index = 3,
    .pixel_format = V4L2_PIX_FMT_JPEG,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = 480,
        .height = 480,
      },
  },
  {
    .index = 4,
    .pixel_format = V4L2_PIX_FMT_JPEG,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = 640,
        .height = 480,
      },
  },
  {
    .index = 5,
    .pixel_format = V4L2_PIX_FMT_JPEG,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = 864,
        .height = 480,
      },
  },
};

/* Frame intervals this driver can actually program, per resolution.
 *
 * 480x480 contributes nothing on purpose: the reference programs no rate at
 * that width (see the comment above the mode tables), so the rate is
 * whatever its window table fixes and there is no honest value to report
 * here.  An empty enumeration says "unknown" -- a made-up number would not.
 */

static const struct v4l2_frmivalenum g_bk7258_gc2145_frmintervals[] =
{
  {
    .index = 0, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 640, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 30 },
  },
  {
    .index = 1, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 640, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 25 },
  },
  {
    .index = 2, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 640, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 20 },
  },
  {
    .index = 3, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 640, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 15 },
  },
  {
    .index = 4, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 864, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 25 },
  },
  {
    .index = 5, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 864, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 20 },
  },
  {
    .index = 6, .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_UYVY,
    .width = 864, .height = 480,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete = { .numerator = 1, .denominator = 15 },
  },
};

#define GC2145_FMTDESCS_COUNT \
  (sizeof(g_bk7258_gc2145_fmtdescs) / sizeof(g_bk7258_gc2145_fmtdescs[0]))

#define GC2145_FRMSIZES_COUNT \
  (sizeof(g_bk7258_gc2145_frmsizes) / sizeof(g_bk7258_gc2145_frmsizes[0]))

#define GC2145_FRMINTERVALS_COUNT \
  (sizeof(g_bk7258_gc2145_frmintervals) / \
   sizeof(g_bk7258_gc2145_frmintervals[0]))

static struct bk7258_gc2145_dev_s g_bk7258_gc2145 =
{
  .sensor =
    {
      .ops = &g_bk7258_gc2145_ops,
      .fmtdescs_num = GC2145_FMTDESCS_COUNT,
      .fmtdescs = g_bk7258_gc2145_fmtdescs,
      .frmsizes_num = GC2145_FRMSIZES_COUNT,
      .frmsizes = g_bk7258_gc2145_frmsizes,
      .frmintervals_num = GC2145_FRMINTERVALS_COUNT,
      .frmintervals = g_bk7258_gc2145_frmintervals,
    },
  .current_fps = GC2145_FPS_DEFAULT,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bk7258_gc2145_power_on(void)
{
  bk7258_gpio_output(DVP_POWER_PIN, true);

  /* LDO settle delay before releasing reset. */

  up_udelay(120000);
}

static void bk7258_gc2145_reset(void)
{
  bk7258_gpio_output(DVP_RESET_PIN, false);
  up_udelay(120000);

  bk7258_gpio_output(DVP_RESET_PIN, true);
  up_udelay(120000);
}

static void bk7258_gc2145_dvp_pinmux(void)
{
  uint32_t i;

  bk7258_gpio_set_function(DVP_MCLK_PIN, DVP_MCLK_PINMUX_FUNCTION);
  bk7258_gpio_set_function(DVP_PCLK_PIN, DVP_PINMUX_FUNCTION);
  bk7258_gpio_set_function(DVP_HSYNC_PIN, DVP_PINMUX_FUNCTION);
  bk7258_gpio_set_function(DVP_VSYNC_PIN, DVP_PINMUX_FUNCTION);

  for (i = 0; i < 8; i++)
    {
      bk7258_gpio_set_function(DVP_DATA_PIN(i), DVP_PINMUX_FUNCTION);
    }
}

/* Enables the AUXS_CIS system clock path that supplies GC2145's MCLK
 * input (GPIO27).  Must be called after bk7258_gc2145_dvp_pinmux() has
 * already selected GPIO27's AUXS_CIS pinmux function, and before any
 * I2C traffic.
 */

static void bk7258_gc2145_mclk_enable(void)
{
  uint32_t reg;

  reg = getreg32(BK7258_SYS_REG_0X0A);
  reg &= ~(BK7258_SYS_CKSEL_AUXS_CIS_MASK | BK7258_SYS_CKDIV_AUXS_CIS_MASK);
  reg |= (GC2145_MCLK_CKSEL << BK7258_SYS_CKSEL_AUXS_CIS_SHIFT) &
         BK7258_SYS_CKSEL_AUXS_CIS_MASK;
  reg |= (GC2145_MCLK_CKDIV << BK7258_SYS_CKDIV_AUXS_CIS_SHIFT) &
         BK7258_SYS_CKDIV_AUXS_CIS_MASK;
  putreg32(reg, BK7258_SYS_REG_0X0A);

  reg = getreg32(BK7258_SYS_REG_0X0D);
  reg |= BK7258_SYS_CIS_AUXS_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0D);
}

static bool bk7258_gc2145_write_reg_table(const struct gc2145_reg *table,
                                           size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      if (!bk7258_i2c1_write_reg(GC2145_I2C_ADDR, table[i].reg,
                                  table[i].value))
        {
          return false;
        }
    }

  return true;
}

/* Symmetric counterpart of power_on()/reset()/mclk_enable(): assert
 * reset, cut the sensor's supply and close the MCLK clock gate.  Without
 * this, closing /dev/video0 left GPIO49 driving both DVP LDOs and the
 * AUXS_CIS clock running, i.e. the camera kept burning power with nobody
 * using it -- which matters on a battery-powered product.
 */

static void bk7258_gc2145_power_off(void)
{
  uint32_t reg;

  bk7258_gpio_output(DVP_RESET_PIN, false);

  reg = getreg32(BK7258_SYS_REG_0X0D);
  reg &= ~BK7258_SYS_CIS_AUXS_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0D);

  bk7258_gpio_output(DVP_POWER_PIN, false);
}

/* Finds the mode entry for a requested geometry, or NULL. */

static FAR const struct gc2145_mode *
bk7258_gc2145_find_mode(uint16_t width, uint16_t height)
{
  size_t i;

  for (i = 0; i < GC2145_MODE_COUNT; i++)
    {
      if (g_gc2145_modes[i].width == width &&
          g_gc2145_modes[i].height == height)
        {
          return &g_gc2145_modes[i];
        }
    }

  return NULL;
}

/* Finds a rate within a mode, or NULL.  A mode with no rate table has
 * nothing to find: nfps == 0 makes the loop fall straight through.
 */

static FAR const struct gc2145_fps_mode *
bk7258_gc2145_find_fps(FAR const struct gc2145_mode *mode, uint32_t fps)
{
  size_t i;

  for (i = 0; i < mode->nfps; i++)
    {
      if (mode->fps[i].fps == fps)
        {
          return &mode->fps[i];
        }
    }

  return NULL;
}

/* Programs one resolution, and the rate to go with it.
 *
 * Order matters and is the reference's: the window table first, then the
 * rate table.  Both halves are needed, and they are not independent -- the
 * rate tables carry the AEC step ladder as well as the frame length, and
 * each resolution has its own.  Programming a window table on its own
 * leaves whatever rate the previous mode had; programming a rate table from
 * a different resolution sets an exposure ladder for the wrong line time.
 *
 * fps == 0 means "no preference": the mode's first rate is used, which is
 * the fastest one the reference lists for it.
 *
 * GC2145 has no frame-rate register.  The rate follows from the frame length
 * (dummy lines) in page-0 0x07/0x08, which is why the reference ships one
 * register table per rate rather than a divider.
 */

static bool bk7258_gc2145_apply_mode(
    FAR struct bk7258_gc2145_dev_s *priv,
    FAR const struct gc2145_mode *mode, uint32_t fps)
{
  FAR const struct gc2145_fps_mode *rate = NULL;

  if (mode->nfps > 0)
    {
      rate = (fps == 0) ? &mode->fps[0] :
                          bk7258_gc2145_find_fps(mode, fps);
      if (rate == NULL)
        {
          return false;
        }
    }

  if (!bk7258_gc2145_write_reg_table(mode->regs, mode->nregs))
    {
      return false;
    }

  if (rate != NULL &&
      !bk7258_gc2145_write_reg_table(rate->regs, rate->nregs))
    {
      return false;
    }

  priv->mode = mode;
  priv->current_fps = (rate != NULL) ? rate->fps : 0;
  return true;
}

/* Reads GC2145's identity registers.  This is the only real proof that
 * the sensor is present, powered and talking -- the 585-entry init table
 * is write-only, so before this the driver could not distinguish "sensor
 * ACKed and configured" from "something on the bus ACKed".
 */

static bool bk7258_gc2145_check_chip_id(void)
{
  uint8_t hi = 0;
  uint8_t lo = 0;

  if (!bk7258_i2c1_read_reg(GC2145_I2C_ADDR, GC2145_CHIP_ID_REG_HI, &hi) ||
      !bk7258_i2c1_read_reg(GC2145_I2C_ADDR, GC2145_CHIP_ID_REG_LO, &lo))
    {
      printf("bk7258_camera_imgsensor: chip ID read failed\n");
      return false;
    }

  printf("bk7258_camera_imgsensor: chip ID = 0x%02x%02x (expected "
         "0x%02x%02x)\n", hi, lo,
         GC2145_CHIP_ID_VAL_HI, GC2145_CHIP_ID_VAL_LO);

  return hi == GC2145_CHIP_ID_VAL_HI && lo == GC2145_CHIP_ID_VAL_LO;
}

static bool bk7258_gc2145_is_available(FAR struct imgsensor_s *sensor)
{
  /* Deliberately unconditional.  A real probe would have to power the
   * sensor, start MCLK and run an I2C transaction, and this is called
   * from capture_register() during board bring-up -- doing all that here
   * would move the camera's power sequencing into boot, and a bus
   * glitch at that moment would make /dev/video0 disappear entirely
   * with no way to retry.  The identity check therefore runs inside
   * init() (bk7258_gc2145_check_chip_id()), where it reports a mismatch
   * without taking the device node away.
   */

  return true;
}

static int bk7258_gc2145_init(FAR struct imgsensor_s *sensor)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;

  printf("bk7258_camera_imgsensor: init: entry\n");

  bk7258_gc2145_power_on();
  printf("bk7258_camera_imgsensor: init: power_on done\n");

  bk7258_gc2145_reset();
  printf("bk7258_camera_imgsensor: init: reset done\n");

  bk7258_gc2145_dvp_pinmux();
  printf("bk7258_camera_imgsensor: init: dvp_pinmux done\n");

  bk7258_gc2145_mclk_enable();
  printf("bk7258_camera_imgsensor: init: mclk_enable done, "
         "reg0x0a=0x%08x reg0x0d=0x%08x\n",
         (unsigned int)getreg32(BK7258_SYS_REG_0X0A),
         (unsigned int)getreg32(BK7258_SYS_REG_0X0D));

  bk7258_i2c1_init();
  printf("bk7258_camera_imgsensor: init: i2c1_init done\n");

  /* Identity check before the 585-entry blind write, so a wiring or bus
   * problem is reported as such instead of surfacing later as a
   * mid-table NACK.  A mismatch is reported but not fatal: the register
   * tables are known to work on this board, and refusing to continue
   * would turn any future read-timing regression into "the camera is
   * gone".
   */

  if (!bk7258_gc2145_check_chip_id())
    {
      printf("bk7258_camera_imgsensor: init: chip ID mismatch or read "
             "failure, continuing anyway\n");
    }

  printf("bk7258_camera_imgsensor: init: writing %u init registers\n",
         (unsigned int)GC2145_INIT_REG_COUNT);

  if (!bk7258_gc2145_write_reg_table(g_gc2145_init_regs,
                                      GC2145_INIT_REG_COUNT))
    {
      printf("bk7258_camera_imgsensor: init: init register table write "
             "FAILED\n");
      bk7258_gc2145_power_off();
      return -EIO;
    }

  printf("bk7258_camera_imgsensor: init: init register table write OK, "
         "writing the %ux%u mode\n",
         (unsigned int)g_gc2145_modes[GC2145_DEFAULT_MODE].width,
         (unsigned int)g_gc2145_modes[GC2145_DEFAULT_MODE].height);

  /* A mode is programmed here, not left until start_capture(), because the
   * sensor streams continuously once initialised: without a window table it
   * would be driving the DVP bus with whatever geometry the init table
   * leaves behind.  start_capture() reprograms it if the application asked
   * for something else.
   */

  if (!bk7258_gc2145_apply_mode(priv,
                                &g_gc2145_modes[GC2145_DEFAULT_MODE],
                                GC2145_FPS_DEFAULT))
    {
      printf("bk7258_camera_imgsensor: init: mode program FAILED\n");
      bk7258_gc2145_power_off();
      return -EIO;
    }

  printf("bk7258_camera_imgsensor: init: complete at %ux%u %ufps, sensor "
         "now continuously outputting DVP signal\n",
         (unsigned int)priv->mode->width, (unsigned int)priv->mode->height,
         (unsigned int)priv->current_fps);

  priv->initialized = true;
  return OK;
}

static int bk7258_gc2145_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;

  bk7258_gc2145_power_off();
  priv->initialized = false;

  printf("bk7258_camera_imgsensor: uninit: sensor powered off, MCLK "
         "gate closed\n");

  return OK;
}

static FAR const char *
bk7258_gc2145_get_driver_name(FAR struct imgsensor_s *sensor)
{
  return "GC2145";
}

static int bk7258_gc2145_validate_frame_setting(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;
  FAR const struct gc2145_mode *mode;

  if (nr_datafmts < 1)
    {
      return -EINVAL;
    }

  /* The geometry must be one of the modes this driver carries a register
   * table for, and the format must be the byte order the capture path
   * actually produces (see g_bk7258_gc2145_fmtdescs).
   */

  /* Both formats come off the same sensor at the same geometry: the encoder
   * sits behind the capture module, so the sensor side does not care which
   * of the two the application asked for.
   */

  if (datafmts[IMGSENSOR_FMT_MAIN].pixelformat != IMGSENSOR_PIX_FMT_UYVY &&
      datafmts[IMGSENSOR_FMT_MAIN].pixelformat != IMGSENSOR_PIX_FMT_JPEG)
    {
      return -EINVAL;
    }

  mode = bk7258_gc2145_find_mode(datafmts[IMGSENSOR_FMT_MAIN].width,
                                 datafmts[IMGSENSOR_FMT_MAIN].height);
  if (mode == NULL)
    {
      return -EINVAL;
    }

  /* Frame rate.  This needs care, because the same op serves two ioctls
   * that mean different things by the interval argument:
   *
   *   VIDIOC_S_PARM validates the *requested* rate against the format that
   *   is already set.  Here the rate is the application's request and a rate
   *   this driver cannot program must be rejected -- silently accepting 30
   *   and running at 20 is what made the `30` in `stream 640 480 30` a lie.
   *
   *   VIDIOC_S_FMT validates the *new* format against the rate left over
   *   from before.  Here the interval is not a request at all; the
   *   application is changing geometry and has said nothing about the rate.
   *   Rejecting then makes a legal switch impossible: after streaming
   *   640x480 at 30, no application could select 864x480 (25/20/15) without
   *   knowing to set the rate first, and nxcamera issues S_FMT before
   *   S_PARM.
   *
   * The two are told apart by whether the geometry is the one already
   * programmed.  When it is not, the rate is stale baggage: the format is
   * accepted and start_capture() programs the mode's fastest rate, printing
   * what it chose.  VIDIOC_G_PARM then reports the real value, so nothing
   * is claimed that the hardware is not doing.
   */

  /* nfps == 0 is the third case, and leaving it out of the two above was a
   * bug.  At such a mode there is no sensor rate to program -- the window
   * table fixes it, which is why 480x480 enumerates none -- so there is no
   * request here to reject and nothing that could be claimed falsely: what
   * the application receives is paced by the capture module's JPEG sampler,
   * which does honour the interval (bk7258_camera_jpeg_sample_period()).
   *
   * Rejecting instead made the answer depend on history rather than on the
   * request.  priv->mode is the mode last *started*, so the first
   * VIDIOC_S_PARM after boot at 480x480 was compared against the 640x480 the
   * init sequence leaves programmed, took the mode != priv->mode path and
   * succeeded; every later one compared 480x480 against itself and returned
   * -EINVAL.  Same call, same arguments, different result depending on what
   * had streamed before.
   *
   * start_capture() and apply_mode() both already guard on nfps > 0 for the
   * same reason; this was the one place that did not.
   */

  if (interval != NULL && interval->denominator != 0 &&
      mode == priv->mode && mode->nfps > 0)
    {
      uint32_t fps = interval->denominator / (interval->numerator ?
                                             interval->numerator : 1);

      if (bk7258_gc2145_find_fps(mode, fps) == NULL)
        {
          return -EINVAL;
        }
    }

  return OK;
}

static int bk7258_gc2145_start_capture(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;
  FAR const struct gc2145_mode *mode;
  uint32_t fps = 0;
  int ret;

  printf("bk7258_camera_imgsensor: start_capture: entry\n");

  ret = bk7258_gc2145_validate_frame_setting(sensor, type, nr_datafmts,
                                              datafmts, interval);
  if (ret < 0)
    {
      printf("bk7258_camera_imgsensor: start_capture: validate failed, "
             "ret=%d\n", ret);
      return ret;
    }

  /* Apply the requested mode.  validate_frame_setting() above has already
   * rejected geometries and rates this driver cannot produce, so a failure
   * here is an I2C fault rather than a bad request.
   *
   * Reprogrammed only when something actually changed: the window and rate
   * tables together are 50-60 I2C register writes at ~100kHz bitbang, and
   * the common case (streaming the mode that is already programmed) should
   * not pay for them.
   */

  mode = bk7258_gc2145_find_mode(datafmts[IMGSENSOR_FMT_MAIN].width,
                                 datafmts[IMGSENSOR_FMT_MAIN].height);
  if (mode == NULL)
    {
      return -EINVAL;
    }

  if (interval != NULL && interval->denominator != 0)
    {
      fps = interval->denominator / (interval->numerator ?
                                     interval->numerator : 1);
    }

  /* At a mode with no programmable rate the request is not addressed to this
   * half at all, so drop it here rather than carrying it into the comparison
   * below.  current_fps stays 0 at such a mode -- apply_mode() has no rate to
   * record -- so a request left in place would compare unequal on every
   * stream start and rewrite the whole window table each time, which is
   * exactly what the "only reprogram when something changed" test above the
   * mode lookup exists to avoid.
   */

  if (mode->nfps == 0)
    {
      fps = 0;
    }

  /* A rate carried over from a different resolution may not exist at this
   * one (see validate_frame_setting()).  Drop to the mode's fastest rather
   * than refusing to stream, and print it, so the log always shows the rate
   * that is really programmed.
   */

  if (fps != 0 && mode->nfps > 0 &&
      bk7258_gc2145_find_fps(mode, fps) == NULL)
    {
      printf("bk7258_camera_imgsensor: start_capture: %ufps not available "
             "at %ux%u, using %ufps\n", (unsigned int)fps,
             (unsigned int)mode->width, (unsigned int)mode->height,
             (unsigned int)mode->fps[0].fps);
      fps = 0;
    }

  if (mode != priv->mode || (fps != 0 && fps != priv->current_fps))
    {
      if (!bk7258_gc2145_apply_mode(priv, mode, fps))
        {
          printf("bk7258_camera_imgsensor: start_capture: %ux%u @ %ufps "
                 "program FAILED\n", (unsigned int)mode->width,
                 (unsigned int)mode->height, (unsigned int)fps);
          return -EIO;
        }

      printf("bk7258_camera_imgsensor: start_capture: mode set to %ux%u\n",
             (unsigned int)mode->width, (unsigned int)mode->height);
    }

  /* GC2145 has no separate streaming-enable register in this driver's
   * ported tables -- the sensor outputs DVP data continuously once its
   * init sequence completes in bk7258_gc2145_init().  Capture
   * start/stop is entirely a matter of whether the imgdata half
   * (YUV_BUF) is listening.
   */

  printf("bk7258_camera_imgsensor: start_capture: OK at %ux%u %ufps (sensor "
         "streaming continuously since init)\n",
         (unsigned int)priv->mode->width, (unsigned int)priv->mode->height,
         (unsigned int)priv->current_fps);

  return OK;
}

static int bk7258_gc2145_get_frame_interval(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    FAR imgsensor_interval_t *interval)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;

  if (interval == NULL)
    {
      return -EINVAL;
    }

  /* Report the rate that is actually programmed, so VIDIOC_G_PARM
   * reflects hardware state rather than whatever the caller last asked
   * for.
   */

  interval->numerator = 1;
  interval->denominator = priv->current_fps;

  return OK;
}

static int bk7258_gc2145_stop_capture(FAR struct imgsensor_s *sensor,
                                       imgsensor_stream_type_t type)
{
  /* v4l2_cap.c's complete_capture() calls IMGSENSOR_STOP_CAPTURE() from
   * interrupt context when it runs out of vacant buffer containers, so
   * this must not printf().  GC2145 keeps streaming regardless (no
   * streaming-enable bit in this driver's register tables), so there is
   * nothing to do here anyway.
   */

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct imgsensor_s *bk7258_camera_imgsensor_initialize(void)
{
  return &g_bk7258_gc2145.sensor;
}
