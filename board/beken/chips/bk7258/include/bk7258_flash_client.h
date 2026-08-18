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

/* The transfer size the protocol fixes (flash_ipc.h, FLASH_IPC_READ_SIZE and
 * FLASH_IPC_WRITE_SIZE).  Reads and writes are chunked to it internally, two
 * frames per chunk; a caller that wants one chunk's worth of exposure keeps
 * its transfer at or below this.
 */

#define BK7258_FLASH_CHUNK_SIZE    0x00000200u

/****************************************************************************
 * Name: bk7258_flash_notify_init
 *
 * Description:
 *   Take MB_CHNL_FLASH, the channel the CP uses to announce that it is about
 *   to touch flash and that it has finished.  Answering it is required of this
 *   core whatever it does with flash itself: the CP raises the same
 *   notification around its own writes and spins 5ms per edge waiting for the
 *   acknowledgement.  Call once from board bring-up; it only registers a
 *   callback and never blocks.
 *
 ****************************************************************************/

int bk7258_flash_notify_init(void);

/****************************************************************************
 * Name: bk7258_flash_op_notify_register
 *
 * Description:
 *   Subscribe to those announcements.  busy is true when a flash access is
 *   starting and false when it has finished; between the two the part is
 *   unavailable, which for this core means instruction fetch out of it stalls
 *   -- so anything with a deadline (the panel, the camera sampler) wants to be
 *   told.  Runs in interrupt context with the CP spinning on the answer: do
 *   nothing here that can block or take long.  One subscriber; registering
 *   again replaces it, NULL removes it.
 *
 ****************************************************************************/

typedef CODE void (*bk7258_flash_op_notify_t)(bool busy, FAR void *arg);

void bk7258_flash_op_notify_register(bk7258_flash_op_notify_t callback,
                                     FAR void *arg);

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
