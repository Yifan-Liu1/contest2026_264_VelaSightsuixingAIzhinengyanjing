/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_BK7258_SDIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_BK7258_SDIO_H

#include <nuttx/sdio.h>
#include <stdint.h>

FAR struct sdio_dev_s *bk7258_sdio_initialize(int slotno);
int bk7258_sdio_read_blocks(FAR uint8_t *buffer, uint32_t sector,
                            unsigned int nblocks);

#endif
