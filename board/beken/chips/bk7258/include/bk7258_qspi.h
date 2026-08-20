/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_qspi.h
 *
 * 4-wire MCU-SPI LCD transport implemented on top of the BK7258 QSPI
 * controllers.  Two independent buses, one per panel.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* This is a *4-wire MCU SPI* transport (SCL / SDA / CS / DC), not a QSPI
 * one, even though it drives the QSPI controllers.  The distinction is the
 * whole reason the panel used to stay dark, so it is worth stating
 * explicitly here:
 *
 *   - GC9D01 lives under bk_avdk_smp's ap/components/bk_peripheral/src/
 *     lcd/spi/ (note: spi, not qspi) and is driven by lcd_spi_driver.c,
 *     which selects command vs. data with a dedicated DC GPIO and puts the
 *     raw opcode byte on the wire.
 *   - The genuinely-QSPI panels (GC9C01, ST77903, ...) live under
 *     lcd/qspi/ and are driven by lcd_qspi_driver.c, which has no DC
 *     pin and instead prefixes every transfer with a 4-byte
 *     {0x02|0x32, 0x00, reg, 0x00} header.
 *
 * Mixing the two produces exactly the observed symptom: every transfer
 * "completes" (the controller is happy), the bus is busy for the expected
 * number of microseconds, and nothing appears on the glass.
 */

/* Bus index.  This board wires one panel to each QSPI controller; see
 * bk7258_qspi.c's header for the schematic evidence.
 */

#define BK7258_LCD_BUS0   0   /* QSPI0 -- GPIO22/23/24, LCD2 footprint */
#define BK7258_LCD_BUS1   1   /* QSPI1 -- GPIO2/3/4,    LCD1 footprint */
#define BK7258_LCD_NBUSES 2

/* dc_pin is the panel's D/C line as a plain GPIO number.  init()
 * deliberately does NOT pinmux it -- on both buses the D/C net happens to
 * land on a pad that also carries one of the controller's own IO lines, so
 * it must be left as a GPIO output rather than handed to the controller.
 */

void bk7258_lcd_spi_init(int bus, unsigned int dc_pin);

/* True once the given bus has been initialized. */

bool bk7258_lcd_spi_ready(int bus);

/* One command byte, sent with DC low. */

bool bk7258_lcd_spi_write_cmd(int bus, uint8_t cmd);

/* Command parameter / pixel bytes, sent with DC high.  Any length; the
 * transfer is split across the controller's cmd_c register and TX FIFO the
 * same way lcd_spi_send_data_with_qspi_indirect_mode() does.
 */

bool bk7258_lcd_spi_write_data(int bus, const uint8_t *data, uint32_t len);

/* Convenience wrapper: one command byte followed by len parameter bytes,
 * with the DC transitions in between.  len == 0 sends only the command.
 */

bool bk7258_lcd_spi_write_cmd_data(int bus, uint8_t cmd,
                                   const uint8_t *data, uint32_t len);

/* Streams packed RGB565 pixels to the panel: RAMWR with DC low, then the
 * pixel bytes through that controller's memory-mapped data window with CS
 * held low for the whole burst. 'len' must be a non-zero multiple of 4. The
 * caller is responsible for having set the drawing window (CASET/RASET)
 * first.
 */

bool bk7258_lcd_spi_write_frame(int bus, const void *frame, size_t len);

/* Number of frames pushed on this bus since boot, for diagnostics. */

uint32_t bk7258_lcd_spi_frame_count(int bus);

/* RGB565 halfword byte-swap on the way to the panel.  Default is on: the
 * panel wants the high byte of each pixel first, while NuttX's
 * FB_FMT_RGB16_565 framebuffer is host-endian (low byte first).  Exposed as
 * a runtime switch so the assumption can be flipped on the board instead of
 * by re-flashing.  Applies to every bus -- both panels are the same part.
 */

void bk7258_lcd_spi_set_byteswap(bool enable);
bool bk7258_lcd_spi_get_byteswap(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H */
