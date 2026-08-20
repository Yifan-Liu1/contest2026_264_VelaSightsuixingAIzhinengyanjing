/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include <arch/board/board.h>

#include "bk7258_gc9d01_fb.h"

void board_early_initialize(void)
{
#if defined(CONFIG_BK7258_GC9D01_FB) && \
    defined(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  /* GPIO is already available here (the LEDs below use the same helper).
   * Clamp the shared LCD backlight before late board or panel initialization
   * so controller GRAM can never be exposed during the early boot window. */

  bk7258_gc9d01_backlight(false);
#endif
  bk7258_led_initialize();
}

int board_app_initialize(uintptr_t arg)
{
  return 0;
}
