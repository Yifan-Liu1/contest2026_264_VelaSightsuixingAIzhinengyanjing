/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_BK7258_SDIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_BK7258_SDIO_H

#include <nuttx/sdio.h>

FAR struct sdio_dev_s *bk7258_sdio_initialize(int slotno);

#endif
