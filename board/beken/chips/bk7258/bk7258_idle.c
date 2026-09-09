/****************************************************************************
 * BK7258 P0 idle implementation.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "arm_internal.h"
#include "nvic.h"

void up_idle(void)
{
  modifyreg32(NVIC_SYSCON, NVIC_SYSCON_SLEEPDEEP, 0);
  __asm__ volatile("dsb sy\n"
                   "wfi\n"
                   "isb sy\n" ::: "memory");
}
