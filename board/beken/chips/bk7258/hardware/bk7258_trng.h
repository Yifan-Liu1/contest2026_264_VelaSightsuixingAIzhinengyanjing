/****************************************************************************
 * board/beken/chips/bk7258/hardware/bk7258_trng.h
 *
 * BK7258 true random number generator register map.
 *
 * Register layout and the enable sequence come from the vendor SDK:
 * bk_avdk_smp/cp/middleware/soc/bk7258/soc/trng_struct.h (the register
 * struct), .../hal/trng_ll.h (trng_ll_enable / trng_ll_get_random_data) and
 * cp/include/soc/bk7258/reg_base.h (SOC_TRNG_REG_BASE).
 *
 * The base address here is the secure alias.  The AP builds with
 * CONFIG_ARCH_TRUSTZONE_SECURE, i.e. the vendor's CONFIG_SPE case, where
 * SOC_ADDR_OFFSET is 0 -- the same convention every other peripheral in this
 * port already uses (UART1 0x45830000, I2C1 0x45860000, AUD 0x47800000).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_TRNG_H
#define __BOARD_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_TRNG_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_TRNG_BASE            0x458c0000u

#define BK7258_TRNG_DEV_ID          (BK7258_TRNG_BASE + 0x00u)  /* REG_0x00 */
#define BK7258_TRNG_DEV_VERSION     (BK7258_TRNG_BASE + 0x04u)  /* REG_0x01 */
#define BK7258_TRNG_GLOBAL_CTRL     (BK7258_TRNG_BASE + 0x08u)  /* REG_0x02 */
#define BK7258_TRNG_DEV_STATUS      (BK7258_TRNG_BASE + 0x0cu)  /* REG_0x03 */
#define BK7258_TRNG_CTRL            (BK7258_TRNG_BASE + 0x10u)  /* REG_0x04 */
#define BK7258_TRNG_DATA            (BK7258_TRNG_BASE + 0x14u)  /* REG_0x05 */

/* GLOBAL_CTRL */

#define BK7258_TRNG_SOFT_RESET      (1u << 0)
#define BK7258_TRNG_CLK_GATE_BYPASS (1u << 1)

/* CTRL */

#define BK7258_TRNG_EN              (1u << 0)

#endif /* __BOARD_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_TRNG_H */
