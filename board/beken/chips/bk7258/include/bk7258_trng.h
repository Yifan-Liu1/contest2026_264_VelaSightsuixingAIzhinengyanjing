/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_trng.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_TRNG_H
#define __BOARD_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_TRNG_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_trng_initialize
 *
 * Description:
 *   Probe the hardware TRNG and seed the kernel entropy pool from it.  Call
 *   from board bring-up.  /dev/random itself is registered independently by
 *   devrandom_register(), which NuttX calls during driver initialisation.
 *
 * Returned Value:
 *   OK on success; -ENODEV if the block returned a constant, in which case
 *   the pool is left with interrupt timing only.
 *
 ****************************************************************************/

int bk7258_trng_initialize(void);

#endif /* __BOARD_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_TRNG_H */
