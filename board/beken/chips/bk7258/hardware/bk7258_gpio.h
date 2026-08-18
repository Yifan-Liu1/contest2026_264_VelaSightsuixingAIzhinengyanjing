/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_GPIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_GPIO_H

#include <stdint.h>

#define BK7258_GPIO_BASE              0x44000400u
#define BK7258_GPIO_CFG(pin)          (BK7258_GPIO_BASE + ((pin) << 2))

#define BK7258_GPIO_INPUT             (1u << 0)
#define BK7258_GPIO_OUTPUT            (1u << 1)
#define BK7258_GPIO_INPUT_ENABLE      (1u << 2)

/* bit[3]: gpio_struct.h names this field "gpio_output_en", but per
 * gpio_ll.h's gpio_ll_output_enable() ("GPIO output enbale low active"
 * comment, and the function body's explicit `if (enable) enable = 0;
 * else enable = 1;` inversion before writing the bit), this register
 * bit is *active-low*: writing 0 means "output enabled", writing 1
 * means "output disabled" -- the opposite of the field's own name.
 * BK7258_GPIO_OUTPUT_DISABLE below reflects that hardware-level (post-
 * inversion) meaning, which is what this driver's bk7258_gpio.c
 * historically used it for. (Investigated and confirmed correct as
 * originally written after a since-reverted attempt to "fix" this as
 * BK7258_GPIO_OUTPUT_ENABLE with normal-polarity semantics turned out
 * to be based on gpio_struct.h's field name alone, without checking
 * gpio_ll.h's inverting wrapper -- see
 * docs/superpowers/plans/2026-07-31-gc9d01-qspi1-camera-v4l2-verification.md
 * section 5.0 for that dead-end and how it was caught before being
 * committed as a real fix.) */
#define BK7258_GPIO_OUTPUT_DISABLE    (1u << 3)

#define BK7258_GPIO_PULL_UP           (1u << 4)
#define BK7258_GPIO_PULL_ENABLE       (1u << 5)
#define BK7258_GPIO_SECOND_FUNCTION  (1u << 6)
#define BK7258_GPIO_CAPACITY_SHIFT   8
#define BK7258_GPIO_CAPACITY_MASK    (3u << BK7258_GPIO_CAPACITY_SHIFT)
#define BK7258_GPIO_CAPACITY_3       (3u << BK7258_GPIO_CAPACITY_SHIFT)

#define BK7258_GPIO_SYS_BASE          0x440100c0u
#define BK7258_GPIO_SYS_CFG(pin)      (BK7258_GPIO_SYS_BASE + \
                                      (((pin) >> 3) << 2))
#define BK7258_GPIO_SYS_SHIFT(pin)    (((pin) & 7u) << 2)
#define BK7258_GPIO_SYS_MASK(pin)     (0xfu << BK7258_GPIO_SYS_SHIFT(pin))

#endif
