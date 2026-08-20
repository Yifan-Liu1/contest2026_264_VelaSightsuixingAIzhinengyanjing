/****************************************************************************
 * board/beken/chips/bk7258/bk7258_qspi.c
 *
 * 4-wire MCU-SPI LCD transport on the BK7258 QSPI controllers, one bus per
 * panel.  Register layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/qspi_struct.h and hal/qspi_ll.h.
 * Protocol source: ap/middleware/driver/lcd/lcd_spi_driver.c.
 *
 * ---------------------------------------------------------------------
 * Two panels, two controllers
 * ---------------------------------------------------------------------
 * This board has two GC9D01 footprints and can populate both (the "eyes"
 * of the toy/glasses form factor).  Sheet 5/6 of
 * AIDK_AI玩具开发板_原理图.pdf labels them 单屏 (LCD1) and 双屏 (LCD2);
 * combined with sheet 2/6's main-chip pin table:
 *
 *   LCD1 (bus 1, QSPI1)            LCD2 (bus 0, QSPI0)
 *   ------------------------       ------------------------
 *   SCL   LCD_QSPI_CLK  = P2       SCL   FL_QSPI_CLK   = P22
 *   CS    LCD_QSPI_CS   = P3       CS    FL_QSPI_CS    = P23
 *   SDA   LCD_QSPI_D0   = P4       SDA   FL_QSPI_D0    = P24
 *   D/C   LCD_QSPI_D1   = P5       D/C   LCD_QSPI_D3   = P7
 *   RESET LCD_RST       = P45      RESET LCD_QSPI_D2   = P6
 *
 * Independently corroborated by bk_solution_ai (the vendor's AIDK
 * solution), components/bk_dual_screen_avi_player/
 * bk_dual_screen_avi_player.c:
 *
 *   .lcd0_config = { .spi_id = 0, .dc_pin = GPIO_7, .reset_pin = GPIO_6 }
 *   .lcd1_config = { .spi_id = 1, .dc_pin = GPIO_5, .reset_pin = GPIO_45 }
 *
 * Note on the "FL_" prefix: it suggests Flash, and those three pads are
 * QSPI0's, which on other designs would carry a NOR flash.  On this board
 * they do not: `FL_QSPI_*` appears exactly six times in the schematic --
 * three times in the main-chip pin table and three times on LCD2 -- and no
 * memory device is attached.  The SoC's own flash uses a separate
 * interface.  So there is no bus arbitration to worry about; QSPI0 belongs
 * entirely to the second panel.
 *
 * Both panels share the LDO33_EN 3.3V rail (GPIO52) and the backlight
 * switch (GPIO25); neither is per-panel.
 *
 * Pads used as plain GPIO despite having a controller function:
 *   P5 = QSPI1_IO1, P6 = QSPI1_IO2, P7 = QSPI1_IO3.
 * Only CLK/CSN/IO0 are pinmuxed on each bus, so P5/P6/P7 stay free for
 * D/C and RESET duty.  Driving them from the controller is what left the
 * first panel unable to tell commands from data.
 *
 * ---------------------------------------------------------------------
 * Why MCU-SPI framing and not QSPI framing
 * ---------------------------------------------------------------------
 * GC9D01 sits in bk_avdk_smp's lcd/spi/ directory (note: spi, not qspi),
 * its device table declares .type = LCD_TYPE_SPI, and lcd_spi_driver.c
 * drives it as plain 4-wire MCU SPI:
 *
 *   bk_lcd_spi_send_cmd():  DC low,  then the bare opcode byte
 *   bk_lcd_spi_send_data(): DC high, then the bare parameter bytes
 *
 * with the bytes packed into cmd_c_h (not cmd_c_l) and cmd_c_cfg1 set to
 * 0x3 << (data_len * 2) to mark the end of the transfer.  There is no
 * header byte of any kind.  An earlier version of this file sent the
 * QSPI-panel header {0x02, 0x00, reg, 0x00}; every command "succeeded"
 * because cmd_start_done only reports that the controller finished
 * shifting out whatever it was told to.
 *
 * ---------------------------------------------------------------------
 * Clocking
 * ---------------------------------------------------------------------
 * The panel clock is NOT config.clk_rate.  bk_qspi_init()
 * (ap/middleware/driver/qspi/qspi_driver.c) programs the source mux and
 * pre-divider through sysctrl and leaves clk_rate at 0 for every LCD
 * path.  The two controllers use *different sysctrl registers* with the
 * same bit positions, which is easy to conflate; see
 * hardware/bk7258_sysctrl.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "hardware/bk7258_sysctrl.h"
#include "bk7258_gpio.h"
#include "bk7258_qspi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets, identical on both controller instances (qspi_struct.h
 * word index in parentheses).
 */

#define QSPI_OFF_GLB_CTRL          0x08u   /* REG_0x02 */
#define QSPI_OFF_CMD_A_CFG2        0x2cu   /* REG_0x0b */
#define QSPI_OFF_CMD_C_L           0x40u   /* REG_0x10 */
#define QSPI_OFF_CMD_C_H           0x44u   /* REG_0x11 */
#define QSPI_OFF_CMD_C_CFG1        0x48u   /* REG_0x12 */
#define QSPI_OFF_CMD_C_CFG2        0x4cu   /* REG_0x13 */
#define QSPI_OFF_CONFIG            0x60u   /* REG_0x18 */
#define QSPI_OFF_STATUS_CLR        0x6cu   /* REG_0x1b */
#define QSPI_OFF_STATUS            0x70u   /* REG_0x1c */
#define QSPI_OFF_FIFO_DATA         0x100u  /* REG_0x40.. */

#define QSPI_GLB_CTRL_SOFT_RESET   (1u << 0)

#define QSPI_STATUS_CMD_START_DONE      (1u << 2)
#define QSPI_STATUS_CLR_CMD_START_DONE  (1u << 2)
#define QSPI_CMD_START_BIT              (1u << 0)

#define QSPI_CONFIG_QSPI_EN             (1u << 0)
#define QSPI_CONFIG_IO2_IO3_MODE        (1u << 3)
#define QSPI_CONFIG_FORCE_SPI_CS_LOW    (1u << 6)
#define QSPI_CONFIG_CLK_RATE_SHIFT      8u
#define QSPI_CONFIG_CLK_RATE_MASK       (0xffu << QSPI_CONFIG_CLK_RATE_SHIFT)
#define QSPI_CONFIG_DISABLE_CMD_SCK     (1u << 16)
#define QSPI_CONFIG_IO_CPU_MEM_SEL      (1u << 22)

/* cmd_a_cfg2 value installed once at init, copied verbatim from
 * lcd_spi_driver_init_with_qspi()'s
 * qspi_hal_set_cmd_a_cfg2(hal, 0x80000000) under
 * CONFIG_LCD_SPI_REFRESH_WITH_QSPI_MAPPING_MODE.  Decoded against
 * qspi_struct.h's cmd_a_cfg2 bitfields that is cmd_mode = 2 (bits 31:30)
 * with data_line = 0 (bits 15:14), i.e. single-line data -- which is what
 * a 4-wire SPI panel needs, since only SDA is wired.  The real QSPI panel
 * driver uses 0x80008000 here (data_line = 2, four lines), which is the
 * comparison that establishes what the field means.
 */

#define QSPI_CMD_A_CFG2_MAPPING_MODE    0x80000000u

/* Source clock 480MHz / (1 + 7) = 60MHz, matching LCD_QSPI_60M as
 * requested by lcd_spi_gc9d01_config.clk.
 */

#define QSPI_SRC_CLK_DIV_60M            7u

/* TX FIFO window is fifo_data[61] = 244 bytes.  The vendor driver chunks
 * at 0x100, which overruns it; capping at the real size avoids writing
 * past fifo_data[] into the registers that follow.  Nothing in the GC9D01
 * command table comes close (longest payload is 32 bytes).
 */

#define QSPI_FIFO_MAX_BYTES             (61u * 4u)

#define QSPI_CFG1_FIFO_PAYLOAD          0x300u
#define QSPI_CFG2_DATA_LEN_SHIFT        2u
#define QSPI_CMD_LINE_MARK              0x3u

/* Matches qspi_ll_wait_cmd_done()'s 10000-iteration bound instead of
 * looping forever, so a pinmux or wiring problem produces a diagnosable
 * timeout rather than an unrecoverable hang.
 */

#define QSPI_WAIT_DONE_MAX_ITER    10000

/* Settle time between the last window write and releasing CS, copied from
 * bk_lcd_spi_wait_display_complete()'s bk_delay_us(15).
 */

#define QSPI_FRAME_TAIL_DELAY_US   15

#define GC9D01_CMD_RAMWR           0x2Cu

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct lcd_bus_s
{
  /* Fixed hardware description */

  uintptr_t base;             /* controller register base */
  uintptr_t window;           /* memory-mapped data window */
  uint32_t  clk_gate;         /* bit in BK7258_SYS_DEVCLK_EN */
  uintptr_t clkdiv_reg;       /* sysctrl register holding ckdiv/cksel */
  uint32_t  ckdiv_mask;
  uint32_t  ckdiv_shift;
  uint32_t  cksel_480m;
  uint8_t   clk_pin;
  uint8_t   csn_pin;
  uint8_t   io0_pin;
  uint8_t   pinmux;           /* function-select index for this bus */
  const char *name;

  /* Runtime state */

  unsigned int dc_pin;
  bool initialized;
  bool cmd_traced;
  uint32_t frames;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Bus 0 = QSPI0 on GPIO22/23/24, function-select index 3.
 * Bus 1 = QSPI1 on GPIO2/3/4,    function-select index 6.
 * Indices from gpio_map.h's per-pin function arrays.
 */

static struct lcd_bus_s g_bus[BK7258_LCD_NBUSES] =
{
  {
    .base        = BK7258_QSPI0_BASE,
    .window      = 0x64000000u,          /* LCD_QSPI0_DATA_ADDR */
    .clk_gate    = BK7258_QSPI0_MODULE_CLK_EN,
    .clkdiv_reg  = BK7258_SYS_CPU_CLKDIV_MODE2,
    .ckdiv_mask  = BK7258_QSPI0_CKDIV_MASK,
    .ckdiv_shift = BK7258_QSPI0_CKDIV_SHIFT,
    .cksel_480m  = BK7258_QSPI0_CKSEL_480M,
    .clk_pin     = 22u,
    .csn_pin     = 23u,
    .io0_pin     = 24u,
    .pinmux      = 3u,
    .name        = "qspi0",
  },
  {
    .base        = BK7258_QSPI1_BASE,
    .window      = 0x68000000u,          /* LCD_QSPI1_DATA_ADDR */
    .clk_gate    = BK7258_QSPI1_MODULE_CLK_EN,
    .clkdiv_reg  = BK7258_SYS_CPU26M_WDT_CLKDIV,
    .ckdiv_mask  = BK7258_QSPI1_CKDIV_MASK,
    .ckdiv_shift = BK7258_QSPI1_CKDIV_SHIFT,
    .cksel_480m  = BK7258_QSPI1_CKSEL_480M,
    .clk_pin     = 2u,
    .csn_pin     = 3u,
    .io0_pin     = 4u,
    .pinmux      = 6u,
    .name        = "qspi1",
  },
};

/* RGB565 byte order on the wire.  Shared: both panels are the same part.
 *
 * The panel takes the high byte of each 16-bit pixel first.  The vendor's
 * own fill helper (lcd_spi_display_fill_pure_color() in
 * bk_avdk_smp/projects/spi_lcd_example/ap/ap_main.c) stores
 * `data[0] = color >> 8; data[1] = color;` into the frame buffer and then
 * DMAs it verbatim into the data window, which establishes that wire order
 * == memory byte order for this window.
 *
 * NuttX's FB_FMT_RGB16_565 is host-endian, so a little-endian uint16 store
 * puts the low byte first and the driver has to swap each halfword on its
 * way out.  Doing it here rather than asking applications to store
 * big-endian keeps /dev/fbN conformant.
 *
 * Confirmed on hardware: with the swap active a saturated-green frame
 * (0x06E0) shows as green; had the swap been wrong it would have appeared
 * as 0xE006, a dark red.
 */

static bool g_swap_bytes = true;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Swap the two bytes of each 16-bit pixel inside one 32-bit word:
 * [b0 b1 b2 b3] -> [b1 b0 b3 b2] in memory order.
 */

static inline uint32_t gc9_swap16x2(uint32_t w)
{
  return ((w & 0x00ff00ffu) << 8) | ((w & 0xff00ff00u) >> 8);
}

static inline struct lcd_bus_s *lcd_bus(int bus)
{
  if (bus < 0 || bus >= BK7258_LCD_NBUSES)
    {
      return NULL;
    }

  return &g_bus[bus];
}

static bool lcd_wait_done(FAR struct lcd_bus_s *b)
{
  int i;

  for (i = 0; i < QSPI_WAIT_DONE_MAX_ITER; i++)
    {
      if ((getreg32(b->base + QSPI_OFF_STATUS) &
           QSPI_STATUS_CMD_START_DONE) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  /* Clear cmd_start_done (write 1 then 0), matching qspi_ll_wait_cmd_done()
   * -- required so a stale "done" flag from this command does not make the
   * next command's wait loop return immediately.
   */

  modifyreg32(b->base + QSPI_OFF_STATUS_CLR, 0,
              QSPI_STATUS_CLR_CMD_START_DONE);
  modifyreg32(b->base + QSPI_OFF_STATUS_CLR,
              QSPI_STATUS_CLR_CMD_START_DONE, 0);

  return i < QSPI_WAIT_DONE_MAX_ITER;
}

/****************************************************************************
 * Name: lcd_xfer_short
 *
 * Description:
 *   Shifts out 1..4 bytes through the cmd_c channel, byte 0 first.  This is
 *   lcd_spi_send_data_with_qspi_cmd_c(): all four registers are cleared
 *   first (which also drops any stale data_len left in cfg2 by a previous
 *   FIFO transfer), the bytes are packed little-endian into cmd_c_h so that
 *   cmd1 == data[0], and cfg1 marks the first *unused* byte slot with
 *   wire-mode 3.
 *
 ****************************************************************************/

static bool lcd_xfer_short(FAR struct lcd_bus_s *b, const uint8_t *data,
                           uint32_t len)
{
  uint32_t value = 0;
  uint32_t i;

  putreg32(0, b->base + QSPI_OFF_CMD_C_L);
  putreg32(0, b->base + QSPI_OFF_CMD_C_H);
  putreg32(0, b->base + QSPI_OFF_CMD_C_CFG1);
  putreg32(0, b->base + QSPI_OFF_CMD_C_CFG2);

  for (i = 0; i < len; i++)
    {
      value |= ((uint32_t)data[i]) << (i * 8);
    }

  putreg32(value, b->base + QSPI_OFF_CMD_C_H);
  putreg32(QSPI_CMD_LINE_MARK << (len * 2u),
           b->base + QSPI_OFF_CMD_C_CFG1);

  if (!b->cmd_traced)
    {
      /* One-shot trace per bus: the whole framing fix lives in these
       * registers, so their read-back values are the evidence that a bare
       * opcode -- not a QSPI header -- is going out.
       */

      b->cmd_traced = true;
      printf("bk7258_lcd_spi[%s]: first xfer: cmd_c_h=0x%08x "
             "cmd_c_l=0x%08x cfg1=0x%08x cfg2=0x%08x len=%u\n", b->name,
             (unsigned int)getreg32(b->base + QSPI_OFF_CMD_C_H),
             (unsigned int)getreg32(b->base + QSPI_OFF_CMD_C_L),
             (unsigned int)getreg32(b->base + QSPI_OFF_CMD_C_CFG1),
             (unsigned int)getreg32(b->base + QSPI_OFF_CMD_C_CFG2),
             (unsigned int)len);
    }

  modifyreg32(b->base + QSPI_OFF_CMD_C_CFG2, 0, QSPI_CMD_START_BIT);

  return lcd_wait_done(b);
}

/****************************************************************************
 * Name: lcd_xfer_fifo
 *
 * Description:
 *   Shifts out the first 4 bytes through cmd_c_h and the next 'fifo_len'
 *   bytes through the TX FIFO, as
 *   lcd_spi_send_data_with_qspi_indirect_mode()'s long branch does.  Bytes
 *   are assembled one at a time so an unaligned source pointer is safe (a
 *   command table entry's payload generally is not word aligned).
 *
 ****************************************************************************/

static bool lcd_xfer_fifo(FAR struct lcd_bus_s *b, const uint8_t *data,
                          uint32_t fifo_len)
{
  uint32_t words = (fifo_len + 3u) / 4u;
  uint32_t value;
  uint32_t i;

  value = ((uint32_t)data[0]) |
          ((uint32_t)data[1] << 8) |
          ((uint32_t)data[2] << 16) |
          ((uint32_t)data[3] << 24);

  putreg32(value, b->base + QSPI_OFF_CMD_C_H);
  putreg32(QSPI_CFG1_FIFO_PAYLOAD, b->base + QSPI_OFF_CMD_C_CFG1);
  putreg32(fifo_len << QSPI_CFG2_DATA_LEN_SHIFT,
           b->base + QSPI_OFF_CMD_C_CFG2);

  for (i = 0; i < words; i++)
    {
      uint32_t word = 0;
      uint32_t k;

      for (k = 0; k < 4u; k++)
        {
          uint32_t idx = i * 4u + k;

          if (idx < fifo_len)
            {
              word |= ((uint32_t)data[4u + idx]) << (k * 8);
            }
        }

      putreg32(word, b->base + QSPI_OFF_FIFO_DATA + i * 4u);
    }

  modifyreg32(b->base + QSPI_OFF_CMD_C_CFG2, 0, QSPI_CMD_START_BIT);

  return lcd_wait_done(b);
}

static bool lcd_xfer(FAR struct lcd_bus_s *b, const uint8_t *data,
                     uint32_t len)
{
  uint32_t remain = len;
  const uint8_t *p = data;

  while (remain > 0)
    {
      uint32_t fifo_len;

      if (remain <= 4u)
        {
          return lcd_xfer_short(b, p, remain);
        }

      fifo_len = remain - 4u;
      if (fifo_len > QSPI_FIFO_MAX_BYTES)
        {
          fifo_len = QSPI_FIFO_MAX_BYTES;
        }

      if (!lcd_xfer_fifo(b, p, fifo_len))
        {
          return false;
        }

      p += 4u + fifo_len;
      remain -= 4u + fifo_len;
    }

  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void bk7258_lcd_spi_init(int bus, unsigned int dc_pin)
{
  FAR struct lcd_bus_s *b = lcd_bus(bus);

  if (b == NULL)
    {
      return;
    }

  b->dc_pin = dc_pin;

  /* 1. Module clock gate.  Without this the controller's command state
   * machine has no clock and cmd_start_done never asserts.
   */

  modifyreg32(BK7258_SYS_DEVCLK_EN, 0, b->clk_gate);

  /* 2. Source clock: 480MHz / 8 = 60MHz, the divider path bk_qspi_init()
   * uses.  config.clk_rate stays 0 (cleared in step 4).  Note the two
   * controllers use different sysctrl registers.
   */

  modifyreg32(b->clkdiv_reg, b->ckdiv_mask,
              (QSPI_SRC_CLK_DIV_60M << b->ckdiv_shift) | b->cksel_480m);

  /* 3. Pinmux CLK/CSN/IO0 only.  The D/C pad must stay a plain GPIO --
   * handing it to the controller is what left the panel unable to tell
   * commands from data.  Idle high = "data", matching
   * lcd_spi_device_gpio_init()'s initial state.
   */

  bk7258_gpio_set_function(b->clk_pin, b->pinmux);
  bk7258_gpio_set_function(b->csn_pin, b->pinmux);
  bk7258_gpio_set_function(b->io0_pin, b->pinmux);
  bk7258_gpio_output(b->dc_pin, true);

  /* 4. Controller enable, qspi_ll_init_common(): qspi_en + io2_io3_mode.
   * io2_io3_mode makes IO2/IO3 driven from config.io2/io3 instead of by
   * the shifter, which is correct when only one data line is wired.
   */

  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_CLK_RATE_MASK,
              QSPI_CONFIG_QSPI_EN | QSPI_CONFIG_IO2_IO3_MODE);

  /* 5. Soft-reset pulse.  Order and final state copied from
   * lcd_spi_driver_init_with_qspi(): disable (0), delay, enable (1).
   */

  modifyreg32(b->base + QSPI_OFF_GLB_CTRL, QSPI_GLB_CTRL_SOFT_RESET, 0);
  up_udelay(10);
  modifyreg32(b->base + QSPI_OFF_GLB_CTRL, 0, QSPI_GLB_CTRL_SOFT_RESET);

  /* 6. Data-window transfer shape for the pixel path. */

  putreg32(QSPI_CMD_A_CFG2_MAPPING_MODE, b->base + QSPI_OFF_CMD_A_CFG2);

  b->initialized = true;

  printf("bk7258_lcd_spi[%s]: init: devclk_en=0x%08x clkdiv=0x%08x "
         "config=0x%08x glb_ctrl=0x%08x cmd_a_cfg2=0x%08x "
         "pins=CLK%u/CS%u/SDA%u dc=GPIO%u\n", b->name,
         (unsigned int)getreg32(BK7258_SYS_DEVCLK_EN),
         (unsigned int)getreg32(b->clkdiv_reg),
         (unsigned int)getreg32(b->base + QSPI_OFF_CONFIG),
         (unsigned int)getreg32(b->base + QSPI_OFF_GLB_CTRL),
         (unsigned int)getreg32(b->base + QSPI_OFF_CMD_A_CFG2),
         (unsigned int)b->clk_pin, (unsigned int)b->csn_pin,
         (unsigned int)b->io0_pin, (unsigned int)b->dc_pin);
}

bool bk7258_lcd_spi_ready(int bus)
{
  FAR struct lcd_bus_s *b = lcd_bus(bus);

  return b != NULL && b->initialized;
}

bool bk7258_lcd_spi_write_cmd(int bus, uint8_t cmd)
{
  FAR struct lcd_bus_s *b = lcd_bus(bus);

  if (b == NULL || !b->initialized)
    {
      return false;
    }

  bk7258_gpio_write(b->dc_pin, false);

  return lcd_xfer(b, &cmd, 1u);
}

bool bk7258_lcd_spi_write_data(int bus, const uint8_t *data, uint32_t len)
{
  FAR struct lcd_bus_s *b = lcd_bus(bus);

  if (b == NULL || !b->initialized || data == NULL || len == 0)
    {
      return false;
    }

  bk7258_gpio_write(b->dc_pin, true);

  return lcd_xfer(b, data, len);
}

bool bk7258_lcd_spi_write_cmd_data(int bus, uint8_t cmd,
                                   const uint8_t *data, uint32_t len)
{
  if (!bk7258_lcd_spi_write_cmd(bus, cmd))
    {
      return false;
    }

  if (len == 0)
    {
      return true;
    }

  return bk7258_lcd_spi_write_data(bus, data, len);
}

bool bk7258_lcd_spi_write_frame(int bus, const void *frame, size_t len)
{
  FAR struct lcd_bus_s *b = lcd_bus(bus);
  const uint32_t *src = frame;
  size_t words = len / 4u;
  size_t i;

  if (b == NULL || !b->initialized || frame == NULL || len == 0 ||
      (len & 3u) != 0)
    {
      return false;
    }

  /* RAMWR goes out as an ordinary DC-low command; CS is allowed to
   * deassert afterwards, exactly as bk_lcd_spi_frame_display() does.
   */

  if (!bk7258_lcd_spi_write_cmd(bus, GC9D01_CMD_RAMWR))
    {
      return false;
    }

  bk7258_gpio_write(b->dc_pin, true);

  /* lcd_spi_quad_write_start(): park the command channel, hold CS low for
   * the whole burst, hand the data line to the memory-mapped window and
   * stop the command path from generating SCK.
   */

  putreg32(0, b->base + QSPI_OFF_CMD_C_L);
  putreg32(0, b->base + QSPI_OFF_CMD_C_H);
  putreg32(0, b->base + QSPI_OFF_CMD_C_CFG1);
  putreg32(0, b->base + QSPI_OFF_CMD_C_CFG2);

  modifyreg32(b->base + QSPI_OFF_CONFIG, 0, QSPI_CONFIG_FORCE_SPI_CS_LOW);
  modifyreg32(b->base + QSPI_OFF_CONFIG, 0, QSPI_CONFIG_IO_CPU_MEM_SEL);
  modifyreg32(b->base + QSPI_OFF_CONFIG, 0, QSPI_CONFIG_DISABLE_CMD_SCK);

  if (b->frames < 2)
    {
      printf("bk7258_lcd_spi[%s]: frame %u: config=0x%08x words=%u "
             "src=%p first_px=0x%04x swap=%u\n", b->name,
             (unsigned int)b->frames,
             (unsigned int)getreg32(b->base + QSPI_OFF_CONFIG),
             (unsigned int)words, frame,
              (unsigned int)(src[0] & 0xffff),
             (unsigned int)g_swap_bytes);
    }

  /* CPU-driven copy.  The controller back-pressures the AHB write when its
   * FIFO is full, so this self-throttles to the wire rate; 160x160 RGB565
   * measured ~27ms on the bus.
   *
   * The destination address increments: lcd_spi_dma_single_mode_config()
   * sets dst.start_addr = qspi_data, dst.end_addr = qspi_data + data_len
   * and dst.addr_inc_en = DMA_ADDR_INC_ENABLE, so the window is a range,
   * not a single FIFO register.
   */

  if (g_swap_bytes)
    {
      for (i = 0; i < words; i++)
        {
          putreg32(gc9_swap16x2(src[i]), b->window + i * 4u);
        }
    }
  else
    {
      for (i = 0; i < words; i++)
        {
          putreg32(src[i], b->window + i * 4u);
        }
    }

  up_udelay(QSPI_FRAME_TAIL_DELAY_US);

  /* lcd_spi_quad_write_stop(), same order as the reference. */

  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_DISABLE_CMD_SCK, 0);
  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_FORCE_SPI_CS_LOW, 0);
  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_IO_CPU_MEM_SEL, 0);

  b->frames++;
  return true;
}

uint32_t bk7258_lcd_spi_frame_count(int bus)
{
  FAR struct lcd_bus_s *b = lcd_bus(bus);

  return b != NULL ? b->frames : 0;
}

void bk7258_lcd_spi_set_byteswap(bool enable)
{
  g_swap_bytes = enable;
}

bool bk7258_lcd_spi_get_byteswap(void)
{
  return g_swap_bytes;
}
