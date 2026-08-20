/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_GC9D01_FB_H
#define __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_GC9D01_FB_H

#include <stdbool.h>
#include <stdint.h>

/* Number of panels this board can drive.  Both GC9D01 footprints (sheet
 * 5/6's LCD1 "单屏" and LCD2 "双屏") get a framebuffer, exposed as
 * /dev/fb0 and /dev/fb1.  display 0 is the QSPI1 panel, which is the one
 * that was brought up first -- keeping it at index 0 means /dev/fb0 still
 * refers to the same physical screen as before dual-panel support.
 */

#define GC9D01_NDISPLAYS 2

/* Panel geometry, from the vendor's own device table
 * (bk_avdk_smp/ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c:
 * lcd_device_gc9d01 = { .width = 160, .height = 160 }).  One RGB565 frame
 * is 160*160*2 = 51200 bytes.
 */

#define GC9D01_XRES   160
#define GC9D01_YRES   160
#define GC9D01_BPP    16
#define GC9D01_STRIDE (GC9D01_XRES * 2)
#define GC9D01_FBLEN  (GC9D01_STRIDE * GC9D01_YRES)

/* Column/row address set, standard MIPI DCS opcodes the GC9 series
 * follows.  RAMWR (0x2C) is issued by the bus driver as part of the frame
 * push, so it is not needed here.
 */

#define GC9D01_CMD_CASET 0x2A
#define GC9D01_CMD_RASET 0x2B

/* Rail power-up (once, shared), panel reset, MCU-SPI bring-up and the
 * GC9D01 initialization command sequence for one display, ending with the
 * full-panel drawing window and the backlight on (also shared).
 * Implemented in bk7258_gc9d01.c, which owns the command table.  Returns OK
 * or a negated errno.
 *
 * Note that a missing panel cannot be detected: GC9D01 has no readable ID
 * register, so an unpopulated footprint accepts the whole sequence exactly
 * as a working one does.
 */

int bk7258_gc9d01_panel_init(int display);

/* Same, for a bitmask of panels at once.  Worth having because the sequence
 * is almost entirely waiting -- 20ms rail, 230ms reset pulse, 120ms after
 * Sleep Out -- and those waits belong to the panels, not to the bus: done
 * per panel they cost 350ms each, done together they cost 350ms in total.
 * Panels already initialised are skipped, so calling this for both panels
 * from bring-up and then letting up_fbinitialize() ask again per display is
 * safe.
 */

int bk7258_gc9d01_panels_init(int displays);

/* Which LCD bus a display is wired to; negative on a bad index. */

int bk7258_gc9d01_bus(int display);

/* Set the drawing window to the whole panel.  Exposed because the
 * framebuffer's update path re-asserts it before every frame, and the
 * pin/opcode knowledge belongs to the panel driver.
 */

bool bk7258_gc9d01_window_full(int display);

/* Backlight (LCD_BL_PWM -> Q3 -> LEDK) on/off.  Shared by both panels --
 * there is one switch for the whole board.
 */

void bk7258_gc9d01_backlight(bool on);

/* Bring-up helpers: fill one display's framebuffer with a solid colour, or
 * draw a four-quadrant test pattern, and push it.  VelaSight uses a black
 * fill before enabling the shared backlight, then hands all updates to LVGL.
 * These also exist because GC9D01 has no readable ID register -- until
 * something is pushed there is no evidence the init sequence took effect.
 */

int bk7258_gc9d01_fb_fill(int display, uint16_t rgb565);
int bk7258_gc9d01_fb_test_pattern(int display);

/* Boot greeting: the word "hello" centred on the panel.  This is what
 * bk7258_bringup() pushes now; the quadrant pattern above stays because it is
 * the diagnostic that makes a byte-order or stride error visible, and it is
 * still reachable from the shell as 'camera_preview pattern'.
 */

int bk7258_gc9d01_fb_hello(int display);

/* The boot animation: writes the greeting one pen stroke at a time, using the
 * same renderer and stroke font as the 'hello' shell command.  displays is a
 * bitmask of panels; every step pushes a full frame to each of them, so the
 * push sets the pace (see the implementation for the measured cost).
 */

int bk7258_gc9d01_fb_hello_animate(int displays, int steps);

/* Draw one word (with a colour) or two lines, and push once.  Used by the
 * event-driven status screen; see bk7258_status_screen.c for why the product
 * path draws these instead of a live preview.  Returns OK, -EINVAL for text
 * the stroke font cannot draw, or -ENODEV if the panel is not up.
 */

int bk7258_gc9d01_fb_text(int display, FAR const char *text,
                          uint16_t colour);
int bk7258_gc9d01_fb_two_lines(int display, FAR const char *line1,
                               FAR const char *line2);

#endif /* __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_GC9D01_FB_H */
