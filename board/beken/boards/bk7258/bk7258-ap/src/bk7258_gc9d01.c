/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c
 *
 * GC9D01 160x160 round LCD bring-up (4-wire MCU SPI), two panels.
 *
 * Init command table copied verbatim -- all 50 entries -- from
 * bk_avdk_smp release/v3.1.1
 * ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c
 * (gc9d01_init_cmds).  The bring-up sequence follows bk_lcd_spi_init() in
 * ap/middleware/driver/lcd/lcd_spi_driver.c.
 *
 * ---------------------------------------------------------------------
 * Dual panel
 * ---------------------------------------------------------------------
 * The board carries two GC9D01 footprints (sheet 5/6: LCD1 labelled 单屏,
 * LCD2 labelled 双屏) and both can be populated -- the two "eyes" of the
 * product.  Each has its own QSPI controller, D/C and RESET; they share the
 * 3.3V rail and the backlight.  Pin evidence and the independent
 * corroboration from bk_solution_ai are in bk7258_qspi.c's header.
 *
 *   display 0 -> bus 1 (QSPI1), D/C GPIO5, RESET GPIO45   [LCD1 footprint]
 *   display 1 -> bus 0 (QSPI0), D/C GPIO7, RESET GPIO6    [LCD2 footprint]
 *
 * display 0 is deliberately the QSPI1 panel: that is the one that was
 * brought up and verified first, so /dev/fb0 keeps pointing at the same
 * physical screen it always did.
 *
 * A missing second panel is not an error.  The panel has no readable ID
 * register, so an unpopulated footprint is indistinguishable from a working
 * one from the software side -- every command still "completes".  Failing
 * bring-up because display 1 is absent would therefore be wrong, and
 * bk7258_gc9d01_panel_init() cannot detect it either way.
 *
 * ---------------------------------------------------------------------
 * 2026-08-10: why the panel was dark even though the bus was busy
 * ---------------------------------------------------------------------
 * Four independent causes, each on its own sufficient to produce a black
 * screen:
 *
 *  1. Panel power was off.  The LCD connector's VDD and LEDA both come from
 *     LDO_3V3, whose enable is the LDO33_EN net = GPIO52.  Nothing asserted
 *     it -- worse, bk7258_pwm.c owned GPIO52 as "BK7258_MOTOR_LDO_GPIO" and
 *     drove it *low* in its setup path, which runs earlier in
 *     bk7258_bringup() than the display does.  GPIO52 is now a
 *     reference-counted shared rail (bk7258_ldo33.c).
 *
 *  2. Backlight was off.  LEDK comes from the LCD_BL net, switched by Q3
 *     (MMBT3904) from LCD_BL_PWM = GPIO25.  A GC9D01 module is a
 *     transmissive TFT: with the backlight off the GRAM contents are
 *     invisible no matter how correct they are.
 *
 *  3. No D/C line -- the pad was pinmuxed as a controller IO instead.
 *  4. Wrong wire protocol (QSPI-panel header instead of bare opcodes).
 *
 * Items 3 and 4 are documented in bk7258_qspi.c.  Two further gaps fixed
 * here: the previous table dropped 20 of the 50 entries, and CASET/RASET
 * was never programmed -- bk_lcd_spi_init() ends by setting the drawing
 * window to the full panel, and a GC9-series panel's window registers have
 * no guaranteed reset value.
 *
 * Success criterion note: GC9D01 exposes no readable ID register (there is
 * no RDID opcode anywhere in the vendor's table), so "the sequence was sent
 * without timing out" has never been evidence that the panel accepted it.
 * The only proof is pixels on the glass.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include "bk7258_gpio.h"
#include "bk7258_ldo33.h"
#include "bk7258_qspi.h"
#include "bk7258_gc9d01_fb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Shared across both panels.
 *
 *   P25 -> LCD_BL_PWM -> Q3 base -> LCD_BL -> both panels' LEDK
 *   P52 -> LDO33_EN   -> LDO_3V3 -> both panels' VDD + LEDA
 */

#define GC9D01_BACKLIGHT_PIN 25u

/* Time for LDO_3V3 to come up before any panel is touched.  No datasheet
 * citation; 20ms is generously above any small LDO's soft-start and costs
 * nothing once per boot.
 */

#define GC9D01_LDO_SETTLE_MS 20u

/* Delay-marker convention from the vendor table: data_len == 0xFF means
 * "this is not a command, delay data[0] milliseconds", handled by
 * bk_lcd_spi_init() rather than by the bus driver.
 */

#define GC9D01_DELAY_MARKER  0xFFu

struct gc9d01_init_cmd
{
  uint8_t cmd;
  uint8_t data[32];
  uint8_t data_len;
};

/* Per-panel wiring.  Indexed by display number. */

struct gc9d01_panel_s
{
  int bus;                 /* BK7258_LCD_BUS0 / BUS1 */
  unsigned int dc_pin;
  unsigned int reset_pin;
  const char *footprint;   /* schematic designator, for logs */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gc9d01_panel_s g_panels[GC9D01_NDISPLAYS] =
{
  {
    .bus       = BK7258_LCD_BUS1,
    .dc_pin    = 5u,
    .reset_pin = 45u,
    .footprint = "LCD1",
  },
  {
    .bus       = BK7258_LCD_BUS0,
    .dc_pin    = 7u,
    .reset_pin = 6u,
    .footprint = "LCD2",
  },
};

static bool g_rail_held;

/* Which panels have already been through the reset and command sequence.
 * fb_register() -> up_fbinitialize() asks for its own display again after
 * bring-up has initialised both together, and re-sending the sequence would
 * reset the glass mid-greeting.
 */

static bool g_panel_ready[GC9D01_NDISPLAYS];

/* All 50 entries of gc9d01_init_cmds, in order, including the delay marker
 * and the trailing Display On.  Earlier revisions of this file omitted 20
 * of them -- 11 because the old bus driver could not carry more than 4
 * payload bytes, and 9 with no stated reason -- which meant the panel's
 * gamma, porch and power-control registers were left at reset values.
 */

static const struct gc9d01_init_cmd g_gc9d01_init_cmds[] =
{
  { 0xFE, { 0x00 }, 0 },
  { 0xEF, { 0x00 }, 0 },
  { 0x80, { 0xFF }, 1 },
  { 0x81, { 0xFF }, 1 },
  { 0x82, { 0xFF }, 1 },
  { 0x83, { 0xFF }, 1 },
  { 0x84, { 0xFF }, 1 },
  { 0x85, { 0xFF }, 1 },
  { 0x86, { 0xFF }, 1 },
  { 0x87, { 0xFF }, 1 },
  { 0x88, { 0xFF }, 1 },
  { 0x89, { 0xFF }, 1 },
  { 0x8A, { 0xFF }, 1 },
  { 0x8B, { 0xFF }, 1 },
  { 0x8C, { 0xFF }, 1 },
  { 0x8D, { 0xFF }, 1 },
  { 0x8E, { 0xFF }, 1 },
  { 0x8F, { 0xFF }, 1 },
  { 0x3A, { 0x05 }, 1 },
  { 0xEC, { 0x01 }, 1 },
  { 0x74, { 0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00 }, 7 },
  { 0x98, { 0x3E, 0x99, 0x3E }, 3 },
  { 0xB5, { 0x0D, 0x0D }, 2 },
  { 0x60, { 0x38, 0x0F, 0x79, 0x67 }, 4 },
  { 0x61, { 0x38, 0x11, 0x79, 0x67 }, 4 },
  { 0x64, { 0x38, 0x17, 0x71, 0x5F, 0x79, 0x67 }, 6 },
  { 0x65, { 0x38, 0x13, 0x71, 0x5B, 0x79, 0x67 }, 6 },
  { 0x6A, { 0x00, 0x00 }, 2 },
  { 0x6C, { 0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50 }, 7 },
  { 0x6E,
      { 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F,
        0x0D, 0x0D, 0x0B, 0x0B, 0x09, 0x09, 0x00, 0x00,
        0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C, 0x0E, 0x0E,
        0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04 }, 32 },
  { 0xBF, { 0x01 }, 1 },
  { 0xF9, { 0x40 }, 1 },
  { 0x9B, { 0x3B }, 1 },
  { 0x93, { 0x33, 0x7F, 0x00 }, 3 },
  { 0x7E, { 0x30 }, 1 },
  { 0x70, { 0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08 }, 6 },
  { 0x71, { 0x0D, 0x02, 0x08 }, 3 },
  { 0x91, { 0x0E, 0x09 }, 2 },
  { 0xC3, { 0x18 }, 1 },
  { 0xC4, { 0x18 }, 1 },
  { 0xC9, { 0x3C }, 1 },
  { 0xF0, { 0x13, 0x15, 0x04, 0x05, 0x01, 0x38 }, 6 },
  { 0xF2, { 0x13, 0x15, 0x04, 0x05, 0x01, 0x34 }, 6 },
  { 0xF1, { 0x4B, 0xB8, 0x7B, 0x34, 0x35, 0xEF }, 6 },
  { 0xF3, { 0x47, 0xB4, 0x72, 0x34, 0x35, 0xDA }, 6 },
  { 0x36, { 0x00 }, 1 },
  { 0x34, { 0x00 }, 0 },                    /* Tearing Effect line off */
  { 0x11, { 0x00 }, 0 },                    /* Sleep Out */
  { 0x00, { 0x78 }, GC9D01_DELAY_MARKER },  /* delay 120ms */
  { 0x29, { 0x00 }, 0 },                    /* Display On */
};

#define GC9D01_INIT_CMD_COUNT \
  (sizeof(g_gc9d01_init_cmds) / sizeof(g_gc9d01_init_cmds[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gc9d01_hw_reset
 *
 * Description:
 *   High-low-high pulse on RESET, mirroring lcd_spi_device_gpio_init():
 *   high, low for 100ms, high, then 120ms settle before any command.  The
 *   120ms figure is the de-facto standard across every panel driver in the
 *   same bk_avdk_smp codebase (lcd_st7701s.c:48, lcd_nt35512.c:859,
 *   lcd_st7789v.c:167 all use rtos_delay_milliseconds(120) post-reset).
 *
 *   Every panel in the mask is pulsed together, because all three waits are
 *   the panel's own settling time and not bus occupancy: two panels on
 *   separate RESET pins and separate QSPI buses have no reason to wait one
 *   after the other.  Doing them in series cost 230ms per panel and was
 *   half of the delay between reset and the first pixel on the glass.
 *
 ****************************************************************************/

static void gc9d01_hw_reset(int displays)
{
  int display;

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((displays & (1 << display)) != 0)
        {
          bk7258_gpio_output(g_panels[display].reset_pin, true);
        }
    }

  up_mdelay(10);

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((displays & (1 << display)) != 0)
        {
          bk7258_gpio_write(g_panels[display].reset_pin, false);
        }
    }

  up_mdelay(100);

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((displays & (1 << display)) != 0)
        {
          bk7258_gpio_write(g_panels[display].reset_pin, true);
        }
    }

  up_mdelay(120);
}

/****************************************************************************
 * Name: gc9d01_set_window
 *
 * Description:
 *   CASET/RASET, 16-bit big-endian start and end.  bk_lcd_spi_init() ends
 *   with this for the full panel area; nothing in the command table sets
 *   it, so without this call the drawing window is whatever the panel's
 *   reset state happens to be.
 *
 ****************************************************************************/

static bool gc9d01_set_window(int bus, uint16_t x0, uint16_t y0,
                              uint16_t x1, uint16_t y1)
{
  uint8_t args[4];

  args[0] = (uint8_t)(x0 >> 8);
  args[1] = (uint8_t)(x0 & 0xff);
  args[2] = (uint8_t)(x1 >> 8);
  args[3] = (uint8_t)(x1 & 0xff);

  if (!bk7258_lcd_spi_write_cmd_data(bus, GC9D01_CMD_CASET, args, 4))
    {
      return false;
    }

  args[0] = (uint8_t)(y0 >> 8);
  args[1] = (uint8_t)(y0 & 0xff);
  args[2] = (uint8_t)(y1 >> 8);
  args[3] = (uint8_t)(y1 & 0xff);

  return bk7258_lcd_spi_write_cmd_data(bus, GC9D01_CMD_RASET, args, 4);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_gc9d01_bus(int display)
{
  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  return g_panels[display].bus;
}

bool bk7258_gc9d01_window_full(int display)
{
  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return false;
    }

  return gc9d01_set_window(g_panels[display].bus, 0, 0,
                           GC9D01_XRES - 1, GC9D01_YRES - 1);
}

void bk7258_gc9d01_backlight(bool on)
{
  bk7258_gpio_output(GC9D01_BACKLIGHT_PIN, on);
}

int bk7258_gc9d01_panels_init(int displays)
{
  int pending = 0;
  int display;
  size_t i;

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((displays & (1 << display)) != 0 && !g_panel_ready[display])
        {
          pending |= 1 << display;
        }
    }

  if (pending == 0)
    {
      /* Either nothing was asked for, or every panel asked for is already
       * up: up_fbinitialize() calls this again per display after bring-up
       * has done both, and re-running the sequence would blank the glass.
       */

      return (displays & ((1 << GC9D01_NDISPLAYS) - 1)) != 0 ? OK : -EINVAL;
    }

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  /* Do not rely on the reset value of the shared backlight GPIO.  Force the
   * glass dark before powering or resetting either controller; bring-up will
   * enable it only after both panel GRAMs have received a black frame. */

  bk7258_gc9d01_backlight(false);
#endif

  /* Power first, and only once: the rail is shared, so take a reference on
   * behalf of the display subsystem as a whole rather than per panel.
   */

  if (!g_rail_held)
    {
      printf("gc9d01: LDO33 rail on (refs=%u)\n", bk7258_ldo33_request());
      g_rail_held = true;
      up_mdelay(GC9D01_LDO_SETTLE_MS);
    }

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((pending & (1 << display)) != 0)
        {
          printf("gc9d01[%d/%s]: hardware reset (RST=GPIO%u)\n", display,
                 g_panels[display].footprint,
                 (unsigned int)g_panels[display].reset_pin);
        }
    }

  gc9d01_hw_reset(pending);

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((pending & (1 << display)) != 0)
        {
          bk7258_lcd_spi_init(g_panels[display].bus,
                              g_panels[display].dc_pin);
          printf("gc9d01[%d/%s]: sending %u init entries\n", display,
                 g_panels[display].footprint,
                 (unsigned int)GC9D01_INIT_CMD_COUNT);
        }
    }

  /* One pass over the command table, feeding every panel from it.  The table
   * is walked once rather than once per panel so that its delay marker --
   * the 120ms the controller needs after Sleep Out -- is waited out once for
   * all of them instead of once each.
   */

  for (i = 0; i < GC9D01_INIT_CMD_COUNT; i++)
    {
      const struct gc9d01_init_cmd *entry = &g_gc9d01_init_cmds[i];

      if (entry->data_len == GC9D01_DELAY_MARKER)
        {
          up_mdelay(entry->data[0]);
          continue;
        }

      for (display = 0; display < GC9D01_NDISPLAYS; display++)
        {
          if ((pending & (1 << display)) == 0)
            {
              continue;
            }

          if (!bk7258_lcd_spi_write_cmd_data(g_panels[display].bus,
                                             entry->cmd, entry->data,
                                             entry->data_len))
            {
              printf("gc9d01[%d]: cmd 0x%02x (entry %u) timed out, "
                     "aborting\n", display, entry->cmd, (unsigned int)i);

              /* Drop this panel and keep going: on a two-panel board one
               * dead bus must not cost the other panel its init sequence.
               */

              pending &= ~(1 << display);
            }
        }

      if (pending == 0)
        {
          return -EIO;
        }
    }

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((pending & (1 << display)) == 0)
        {
          continue;
        }

      if (!bk7258_gc9d01_window_full(display))
        {
          printf("gc9d01[%d]: CASET/RASET failed\n", display);
          pending &= ~(1 << display);
          continue;
        }

      g_panel_ready[display] = true;

      printf("gc9d01[%d/%s]: init sequence sent\n", display,
             g_panels[display].footprint);
    }

  if (pending == 0)
    {
      return -EIO;
    }

#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  /* Diagnostic configurations reveal the initialized panels immediately.
   * VelaSight keeps the shared backlight off until both framebuffer memories
   * have been pushed as black, so uninitialized panel GRAM is never visible.
   */

  bk7258_gc9d01_backlight(true);
#endif

  return OK;
}

int bk7258_gc9d01_panel_init(int display)
{
  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  return bk7258_gc9d01_panels_init(1 << display);
}

/****************************************************************************
 * Name: bk7258_gc9d01_test
 *
 * Description:
 *   Legacy standalone entry point, kept as a thin wrapper so panel 0 can be
 *   brought up without the framebuffer layer.  The caveat that has always
 *   applied to it still applies: "the sequence was sent without hanging" is
 *   not evidence that the panel accepted it.  Use
 *   bk7258_gc9d01_fb_test_pattern() for the only real check.
 *
 ****************************************************************************/

int bk7258_gc9d01_test(int argc, char **argv)
{
  UNUSED(argc);
  UNUSED(argv);

  return bk7258_gc9d01_panel_init(0);
}
