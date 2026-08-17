/****************************************************************************
 * board/beken/chips/bk7258/bk7258_reset.c
 *
 * Chip reset for the AP core, i.e. what NSH's `reboot` needs.
 *
 * Why the AP needs its own reset path
 * ----------------------------------
 * Until this file existed the AP could not reset the board at all: NuttX had
 * no CONFIG_BOARDCTL_RESET here, so `reboot` was not even built, and every
 * reset in the development loop went through the CP shell's own `reboot`
 * instead (autoflash.sh and serial_cmd.sh both do that, because this board
 * has no auto-reset circuit on DTR/RTS -- see autoflash.sh's header).
 *
 * That CP path is slow, and measurably so.  Measured 2026-08-14, three runs,
 * identical to the millisecond: from the `reboot` echo on the CP console to
 * the CP's first boot line is 8.373s, of which the CP's own uptime counter
 * says only ~110ms is bootrom plus bootloader.  So the chip sits there for
 * 8.26s before it resets.  8000ms is CONFIG_INT_WDT_PERIOD_MS in the CP
 * build: the CP's `reboot` ends in bk_wdt_force_reboot(), which with
 * CONFIG_NMI_WDT_EN=1 takes wdt_hal_nmi_reboot() -- an NMI, not a reset --
 * and the NMI lands in the two-core dump/handover machinery in
 * cp/middleware/arch/cm33/trap_base.c, which our NuttX AP does not
 * participate in.  Nothing resets the part until the interrupt watchdog
 * expires.
 *
 * The AON watchdog does not have that problem.  It is the same register the
 * CP uses in its *non*-NMI branch (wdt_hal_force_reboot(), the #else side):
 * write the period with the two-stage key and the part resets, with no NMI,
 * no dump handover and nobody to wait for.
 *
 * Register: AON_WDT_R_CTRL at 0x44000600, one word --
 *   bits [15:0]  period
 *   bits [23:16] key: write 0x5A then 0xA5 to commit
 * (ap/middleware/soc/bk7258_ap/soc/aon_wdt_reg.h; the sequence and the
 * period value 0x0A are taken verbatim from wdt_hal_force_reboot() in
 * ap/middleware/soc/common/hal/wdt_hal.c.)
 *
 * The register map is shared and flat -- the AP build defines
 * SOC_ADDR_OFFSET as 0, and this driver already drives AON GPIO at
 * 0x44000400 -- so the AP can reach the AON watchdog directly and does not
 * need the CP's cooperation.  Asking the CP over the mailbox would not help
 * anyway: its IPC_CPU1_NEED_REBOOT handler calls the same
 * bk_wdt_force_reboot() that takes 8s (cp mb_ipc_cmd.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_AON_WDT_CTRL     0x44000600u

#define BK7258_AON_WDT_KEY_1ST  0x5au
#define BK7258_AON_WDT_KEY_2ND  0xa5u
#define BK7258_AON_WDT_KEY_S    16

/* Smallest period the vendor itself uses for a forced reboot.  The AON
 * watchdog counts on the always-on 32kHz domain, so ten ticks is a fraction
 * of a millisecond -- the reset is effectively immediate.
 */

#define BK7258_AON_WDT_PERIOD   0x000au

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_reset
 *
 * Description:
 *   Reset the whole chip.  Invoked by BOARDIOC_RESET, which is what NSH's
 *   `reboot` command issues.  status is the caller's reset reason; this part
 *   has nowhere to record it that survives an AON watchdog reset, so it is
 *   accepted and ignored rather than pretended about.
 *
 *   Does not return.
 *
 ****************************************************************************/

int board_reset(int status)
{
  UNUSED(status);

  /* Interrupts off first: once the period is committed the part is going
   * down within a fraction of a millisecond, and there is no useful work
   * left to do.  Leaving them on would only widen the window in which an
   * ISR runs against half-torn-down state.
   */

  up_irq_save();

  putreg32((BK7258_AON_WDT_KEY_1ST << BK7258_AON_WDT_KEY_S) |
           BK7258_AON_WDT_PERIOD, BK7258_AON_WDT_CTRL);
  putreg32((BK7258_AON_WDT_KEY_2ND << BK7258_AON_WDT_KEY_S) |
           BK7258_AON_WDT_PERIOD, BK7258_AON_WDT_CTRL);

  for (; ; )
    {
      /* The watchdog does the rest.  If it somehow does not, hanging here is
       * the honest outcome: returning would let the caller report a reset
       * that never happened.
       */
    }

  return 0;
}
