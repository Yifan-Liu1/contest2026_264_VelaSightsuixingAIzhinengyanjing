/****************************************************************************
 * BK7258 AP driver shared serialization.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_DRIVER_H
#define __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_DRIVER_H

#include <nuttx/spinlock.h>

extern rspinlock_t g_bk7258_driver_lock;

#endif
