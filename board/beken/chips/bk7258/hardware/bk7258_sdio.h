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
#define BK7258_SDIO_DATA_WRITE_ENABLE (1u << 16)
#define BK7258_SDIO_DATA_BYTE_SELECT  (1u << 17)
#define BK7258_SDIO_DATA_TX_BYTE_SELECT (1u << 18)
#define BK7258_SDIO_DATA_BUS_4BIT     (1u << 2)

#define BK7258_SDIO_CMD_NO_RESPONSE   (1u << 0)
#define BK7258_SDIO_CMD_RESPONSE_END  (1u << 1)
#define BK7258_SDIO_CMD_TIMEOUT       (1u << 2)
#define BK7258_SDIO_DATA_RECEIVE_END  (1u << 3)
#define BK7258_SDIO_DATA_WRITE_END    (1u << 4)
#define BK7258_SDIO_DATA_TIMEOUT      (1u << 5)
#define BK7258_SDIO_RX_NEED_READ      (1u << 6)
#define BK7258_SDIO_TX_NEED_WRITE     (1u << 7)
#define BK7258_SDIO_FIFO_OVERFLOW     (1u << 8)
#define BK7258_SDIO_TX_FIFO_EMPTY     (1u << 9)
#define BK7258_SDIO_CMD_CRC_OK        (1u << 10)
#define BK7258_SDIO_CMD_CRC_FAIL      (1u << 11)
#define BK7258_SDIO_DATA_CRC_OK       (1u << 12)
#define BK7258_SDIO_DATA_CRC_FAIL     (1u << 13)
#define BK7258_SDIO_RESPONSE_INDEX_SHIFT 14
#define BK7258_SDIO_RESPONSE_INDEX_MASK (0x3fu << BK7258_SDIO_RESPONSE_INDEX_SHIFT)
#define BK7258_SDIO_WRITE_STATUS_SHIFT 20
#define BK7258_SDIO_WRITE_STATUS_MASK (0x7u << BK7258_SDIO_WRITE_STATUS_SHIFT)
#define BK7258_SDIO_WRITE_STATUS_ACCEPTED 2u
#define BK7258_SDIO_DATA_BUSY         (1u << 23)

#define BK7258_SDIO_FIFO_RX_RESET     (1u << 16)
#define BK7258_SDIO_FIFO_TX_RESET     (1u << 17)
#define BK7258_SDIO_RX_READY          (1u << 18)
#define BK7258_SDIO_TX_READY          (1u << 19)
#define BK7258_SDIO_STATE_RESET       (1u << 20)
#define BK7258_SDIO_WRITE_WAIT        (1u << 24)
#define BK7258_SDIO_CLOCK_RECOVERY    (1u << 25)
#define BK7258_SDIO_SAMPLE_POSITIVE   (1u << 26)
#define BK7258_SDIO_CLOCK_GATE        (1u << 27)
#define BK7258_SDIO_HOST_WRITE_BLOCK  (1u << 28)

#define BK7258_SDIO_TX_CLOCK_GATE     (1u << 13)
#define BK7258_SDIO_INT_MASK_DEFAULT  0x0000e03fu

/* SDIO module clock codes, written to SYS_CPU_CLK_DIV_MODE2[17:14] by
 * sdio_set_clock().  The field is the vendor's cksel_sdio[17] plus
 * ckdiv_sdio[16:14]: source 0 = XTAL 26 MHz, 1 = 320 MHz PLL, divider
 * 0:/1 1:/2 2:/4 3:/8 4:/16 5:/32 6:/64 7:/256.  SD_CLK is this module
 * clock divided again by the block's own SD_RATE_SELECT field
 * (FIFO register bits [22:21]), which this driver leaves at its reset value
 * of 0 (/1) -- so SD_CLK is the module clock.
 *
 * The XTAL source gives 26 / 13 / 6.5 / 3.25 / 1.625 / 0.8125 / 0.40625 /
 * 0.1016 MHz for codes 0..7.  It is preferred over the PLL for both settings
 * below because the XTAL is unconditionally running, while a PLL code would
 * make SD_CLK depend on the 320 MHz domain being up when this driver
 * initialises.
 */

/* Identification: 26 MHz / 256 = 101.6 kHz.  Deliberately well under the
 * 400 kHz ceiling the SD specification puts on card identification, and the
 * value the enumeration path has always been validated at.  The next code up
 * (6, /64) is 406.25 kHz, which is over that ceiling, so there is nothing to
 * gain here -- identification is a few dozen commands with no bulk data.
 */

#define BK7258_SDIO_ID_CLOCK          7u

/* Data transfer: 26 MHz / 1 = 26 MHz, the fastest SD_CLK this part offers
 * and the vendor's own top SD clock for it (CLK_26M in
 * middleware/driver/sdcard/sdio_driver.h).
 *
 * This used to be 7u, the same code as identification, which left the whole
 * filesystem running at the 101.6 kHz enumeration clock: 512 bytes on a
 * 1-bit bus took 40.3 ms of wire time, so one small-file rewrite -- a dozen
 * single-block CMD17/CMD24 round trips, because CONFIG_MMCSD_MULTIBLOCK_LIMIT
 * is 1 and FAT caches one sector -- cost about 400 ms.
 *
 * Dial back down this ladder if a higher clock costs data integrity: 1u is
 * 13 MHz and 2u is 6.5 MHz, the vendor's conservative rate.  Sampling phase
 * is the thing that decides it -- see BK7258_SDIO_CLOCK_RECOVERY and
 * BK7258_SDIO_SAMPLE_POSITIVE, which stay at the vendor's negative-edge
 * default.  The symptom to watch for is BK7258_SDIO_DATA_CRC_FAIL from
 * sdio_interrupt(), reported as "SDIO data error=-5".
 */

#define BK7258_SDIO_TRANSFER_CLOCK    0u

/* Command and data engine timeout counters, in SD clock cycles.  Full scale
 * on purpose: at 26 MHz this is still 165 s, so the software deadlines in
 * sdio_wait_command() and sdio_eventwait() stay the ones that decide, at
 * every clock on the ladder above.
 */

#define BK7258_SDIO_TIMEOUT            0xffffffffu

/* How long sdio_wait_command() polls without yielding before it falls back to
 * sleeping.  A command and its response are 96 bit-times plus NCR, which is
 * under 4 us at 26 MHz and about 1.4 ms at the identification clock, so this
 * covers the normal case at every clock and the fallback only runs when a
 * card is genuinely not answering.
 */

#define BK7258_SDIO_CMD_SPIN_US        2000u

/* Settling interval between arming the receive engine and letting the caller
 * issue CMD17, in microseconds.
 *
 * A delay here is needed -- without one BK7258_SDIO_DATA_RECEIVE_END goes
 * missing -- but it was nxsig_usleep(2000), which on a 1 ms tick actually
 * sleeps about 3 ms, and that would have become 95% of the cost of a sector
 * read once the transfer clock went up.  The vendor's own
 * sdcard_read_single_block() has no delay at all in this position, so there
 * is no documented figure to honour; what there is, is evidence that ~200
 * SD clocks was enough, because that is what 2 ms bought at 101.6 kHz.
 *
 * 200 us of up_udelay() is 5200 SD clocks at 26 MHz -- 26 times the settling
 * the old value ever actually provided -- and being a busy wait rather than a
 * sleep it is not rounded up to a tick boundary.  Raise it first if reads
 * start failing after a clock change.
 */

#define BK7258_SDIO_RX_SETTLE_US       200u

/* Whole-transfer budget for filling the 512-byte TX FIFO, in milliseconds.
 *
 * This replaces a per-word bound of 100 000 iterations of up_udelay(1), which
 * allowed 100 ms per four bytes and so 12.8 s for one sector -- against
 * CONFIG_MMCSD_BLOCK_WDATADELAY of 260 ms for the transfer that follows it.
 * The fill runs before BK7258_SDIO_DATA_WRITE_ENABLE, so the FIFO cannot
 * drain while it runs and TX_READY only ever blocks if the FIFO is shallower
 * than a block; a bound on the whole fill is the useful shape.
 */

#define BK7258_SDIO_FILL_TIMEOUT_MS    100u

/* How long sdio_write_accepted() waits in interrupt context for the card's
 * CRC status token, in microseconds.  Sized to stay generous across the whole
 * clock ladder above -- 5200 SD clocks at 26 MHz, 1300 at 6.5 MHz -- against
 * the eight clocks the token actually takes, while putting a figure on how
 * long a block write can hold the CPU in the ISR.
 */

#define BK7258_SDIO_WRITE_STATUS_US    200u

#endif
