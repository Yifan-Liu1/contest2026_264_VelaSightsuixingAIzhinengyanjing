/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_BOARDS_BK7258_AP_INCLUDE_BOARD_H
#define __VENDOR_BEKEN_BOARDS_BK7258_AP_INCLUDE_BOARD_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOARD_NAME "bk7258-ap"
#define BOARD_AP_RAM_START 0x28010000u
#define BOARD_AP_RAM_END   0x28064000u
#define BOARD_KEY_VOLUME_UP_GPIO   13
#define BOARD_KEY_POWER_GPIO       12
#define BOARD_KEY_VOLUME_DOWN_GPIO 8

#define BUTTON_VOLUME_UP           0
#define BUTTON_POWER               1
#define BUTTON_VOLUME_DOWN         2
#define NUM_BUTTONS                3

#define BUTTON_VOLUME_UP_BIT       (1u << BUTTON_VOLUME_UP)
#define BUTTON_POWER_BIT           (1u << BUTTON_POWER)
#define BUTTON_VOLUME_DOWN_BIT     (1u << BUTTON_VOLUME_DOWN)

#define BOARD_LED_RED_GPIO         40
#define BOARD_LED_GREEN_GPIO       41

#define LED_INIRQ                  0
#define LED_SIGNAL                 1
#define LED_ASSERTION              2
#define LED_PANIC                  3
#define LED_IDLE                   4
#define LED_HEAPALLOCATE           5
#define LED_IRQSENABLED            6
#define LED_STACKCREATED           7

int bk7258_bringup(void);
uint32_t board_button_initialize(void);
void board_autoled_on(int led);
void board_autoled_off(int led);
void bk7258_led_initialize(void);

/* Unconditional because both configurations call it, from opposite sides of
 * the same switch: with VELASIGHT the SD-NAND completion in bk7258_mmcsd.c
 * does, and without it the nand_config_loader task in bk7258_bringup.c does.
 * Declaring it under #ifdef VELASIGHT therefore broke exactly the build that
 * needs it most -- configs/nsh compiles with -Werror, so the missing
 * declaration was an error rather than a warning, and that configuration has
 * not built since.  bk7258_agent_config.c is compiled unconditionally, so
 * there is one definition to link against either way.
 */

void bk7258_nand_seed_agent_config(void);

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
int bk7258_wifi_wait_ready(void);
void bk7258_display_reveal(void);
bool bk7258_ai_config_ready(void);
#endif

#ifdef CONFIG_BK7258_SDIO
int bk7258_mmcsd_status(bool *mounted, char *source, size_t source_len);
#endif

#endif
