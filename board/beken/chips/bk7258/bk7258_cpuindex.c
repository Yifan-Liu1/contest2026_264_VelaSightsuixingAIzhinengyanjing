/****************************************************************************
 * BK7258 logical CPU index.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/compiler.h>

#include "hardware/bk7258_memorymap.h"

int noinstrument_function up_cpu_index(void)
{
  return *(volatile uint32_t *)BK7258_DTCM_CPU_ID;
}
