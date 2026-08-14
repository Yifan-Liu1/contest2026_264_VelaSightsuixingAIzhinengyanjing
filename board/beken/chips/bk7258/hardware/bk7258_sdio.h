/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SDIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SDIO_H

#include <stdint.h>

#define BK7258_SDIO_BASE              0x458d0000u
#define BK7258_SDIO_REG(n)            (BK7258_SDIO_BASE + ((n) << 2))

#define BK7258_SDIO_CMD_CTRL          BK7258_SDIO_REG(4)
#define BK7258_SDIO_CMD_ARGUMENT      BK7258_SDIO_REG(5)
#define BK7258_SDIO_CMD_TIMER         BK7258_SDIO_REG(6)
#define BK7258_SDIO_DATA_CTRL         BK7258_SDIO_REG(7)
#define BK7258_SDIO_DATA_TIMER        BK7258_SDIO_REG(8)
#define BK7258_SDIO_RESPONSE(n)       BK7258_SDIO_REG(9 + (n))
#define BK7258_SDIO_INT_STATUS        BK7258_SDIO_REG(0xd)
#define BK7258_SDIO_INT_MASK          BK7258_SDIO_REG(0xe)
#define BK7258_SDIO_TX_FIFO           BK7258_SDIO_REG(0xf)
#define BK7258_SDIO_RX_FIFO           BK7258_SDIO_REG(0x10)
#define BK7258_SDIO_FIFO              BK7258_SDIO_REG(0x11)

#define BK7258_SDIO_CMD_START         (1u << 0)
#define BK7258_SDIO_CMD_RESPONSE      (1u << 1)
#define BK7258_SDIO_CMD_LONG          (1u << 2)
#define BK7258_SDIO_CMD_CRC_CHECK     (1u << 3)
#define BK7258_SDIO_CMD_INDEX_SHIFT   4
#define BK7258_SDIO_CMD_INDEX_MASK    (0x3fu << BK7258_SDIO_CMD_INDEX_SHIFT)

#define BK7258_SDIO_DATA_ENABLE       (1u << 0)
#define BK7258_SDIO_DATA_MULTIBLOCK   (1u << 3)
#define BK7258_SDIO_DATA_BLOCK_SHIFT  4
#define BK7258_SDIO_DATA_BLOCK_MASK   (0xfffu << BK7258_SDIO_DATA_BLOCK_SHIFT)
#define BK7258_SDIO_DATA_BYTE_SELECT  (1u << 17)
#define BK7258_SDIO_DATA_BUS_4BIT    (1u << 2)

#define BK7258_SDIO_CMD_NO_RESPONSE   (1u << 0)
#define BK7258_SDIO_CMD_RESPONSE_END  (1u << 1)
#define BK7258_SDIO_CMD_TIMEOUT       (1u << 2)
#define BK7258_SDIO_DATA_RECEIVE_END  (1u << 3)
#define BK7258_SDIO_DATA_WRITE_END    (1u << 4)
#define BK7258_SDIO_DATA_TIMEOUT      (1u << 5)
#define BK7258_SDIO_RX_NEED_READ      (1u << 6)
#define BK7258_SDIO_TX_NEED_WRITE     (1u << 7)
#define BK7258_SDIO_FIFO_OVERFLOW     (1u << 8)
#define BK7258_SDIO_CMD_CRC_OK        (1u << 10)
#define BK7258_SDIO_CMD_CRC_FAIL      (1u << 11)
#define BK7258_SDIO_DATA_CRC_OK       (1u << 12)
#define BK7258_SDIO_DATA_CRC_FAIL     (1u << 13)
#define BK7258_SDIO_RESPONSE_INDEX_SHIFT 14
#define BK7258_SDIO_RESPONSE_INDEX_MASK (0x3fu << BK7258_SDIO_RESPONSE_INDEX_SHIFT)

#define BK7258_SDIO_FIFO_RX_RESET     (1u << 16)
#define BK7258_SDIO_FIFO_TX_RESET     (1u << 17)
#define BK7258_SDIO_STATE_RESET       (1u << 20)
#define BK7258_SDIO_RX_READY          (1u << 18)
#define BK7258_SDIO_SAMPLE_POSITIVE   (1u << 26)
#define BK7258_SDIO_CLOCK_RECOVERY    (1u << 25)
#define BK7258_SDIO_CLOCK_GATE        (1u << 27)
#define BK7258_SDIO_INT_MASK_DEFAULT  0x0000e03fu

#define BK7258_SDIO_ID_CLOCK          7u
#define BK7258_SDIO_TRANSFER_CLOCK    7u
#define BK7258_SDIO_TIMEOUT            0xffffffffu

#endif
