/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>
#include <stdint.h>

/* NOTE: board_app_finalinitialize() (BOARDIOC_FINALINIT) is only invoked by
 * apps/nshlib after the NSH start-up script, which requires
 * CONFIG_BOARDCTL_FINALINIT and the apps/ framework -- neither is enabled
 * in this minimal NSH configuration, so this hook is dead code here.  The
 * GC9D01 bring-up test is instead invoked from board_late_initialize() in
 * bk7258_bringup.c, which is unconditionally called via
 * CONFIG_BOARD_LATE_INITIALIZE.  See
 * docs/local/superpowers/plans/2026-07-29-gc9d01-lcd-bringup.md Task 3 Step 3.
 */

int board_app_finalinitialize(uintptr_t arg)
{
  return 0;
}
