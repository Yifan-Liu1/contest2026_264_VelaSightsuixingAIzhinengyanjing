/****************************************************************************
 * vendor/beken/chips/bk7258/bk7258_timerisr.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>
#include <nuttx/timers/arch_timer.h>

#include "systick.h"

void up_timer_initialize(void)
{
  /* The DWT cycle counter, which up_perf_gettime() reads on armv8-m, is the
   * only clock on this core fine enough to see where a frame's time goes:
   * CONFIG_USEC_PER_TICK is 1000, so a tick cannot resolve a 25 KB copy.
   * The camera driver uses it to attribute per-frame cost and, by dividing
   * cycles by elapsed milliseconds, to report what the core clock actually
   * is -- CONFIG_BK7258_CPU_FREQ_HZ is an assumption until something
   * measures it.
   */

  up_perf_init((void *)CONFIG_BK7258_CPU_FREQ_HZ);

  up_timer_set_lowerhalf(
    systick_initialize(true, CONFIG_BK7258_CPU_FREQ_HZ, -1));
}
