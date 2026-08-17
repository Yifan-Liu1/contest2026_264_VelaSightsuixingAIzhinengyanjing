/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_flash_client.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_FLASH_CLIENT_H
#define __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_FLASH_CLIENT_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/* The partition the table reserves for this core: 8KB, read/write, execute
 * disabled (bk_avdk_smp .../partitions/partitions.txt and partitions_gen.h,
 * CONFIG_EASYFLASH_AP_PARTITION_OFFSET/SIZE).  Nothing else on the board
 * writes it, which is why it is safe to keep the AP's own key-value store
 * here.  The CP's own environment is the neighbouring 8KB at 0x007FA000 and
 * must not be touched.
 */

#define BK7258_FLASH_AP_ENV_BASE   0x007fc000u
#define BK7258_FLASH_AP_ENV_SIZE   0x00002000u
#define BK7258_FLASH_SECTOR_SIZE   0x00001000u

/****************************************************************************
 * Name: bk7258_flash_client_init
 *
 * Description:
 *   Connect to the flash service the CP already runs, over the mailbox.  Must
 *   be called from a thread after the mailbox link is up, never from an
 *   interrupt handler.  Returns OK, or a negated errno if the CP did not
 *   answer -- in which case flash is simply unavailable and callers are
 *   expected to carry on without persistence.
 *
 ****************************************************************************/

int bk7258_flash_client_init(void);

/* Whether the connect above succeeded. */

bool bk7258_flash_client_ready(void);

/****************************************************************************
 * Name: bk7258_flash_read / bk7258_flash_write / bk7258_flash_erase_sector
 *
 * Description:
 *   Absolute flash addresses, as the service takes them.  Reads and writes
 *   are chunked internally to the 512 bytes the protocol allows per frame.  A
 *   sector is BK7258_FLASH_SECTOR_SIZE and must be erased before its bytes
 *   can be rewritten, exactly as on the part itself.
 *
 *   These block for up to ~600ms per chunk and must not be called from an
 *   interrupt handler.
 *
 ****************************************************************************/

int bk7258_flash_read(uint32_t address, FAR void *buffer, size_t len);
int bk7258_flash_write(uint32_t address, FAR const void *buffer, size_t len);
int bk7258_flash_erase_sector(uint32_t address);

#endif /* __ARCH_ARM_SRC_BK7258_INCLUDE_BK7258_FLASH_CLIENT_H */
