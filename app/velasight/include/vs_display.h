/****************************************************************************
 * app/velasight/include/vs_display.h
 *
 * Opaque handle for the pair of 160x160 round panels, and the calls that drive
 * them.
 *
 * Rendering is snapshot-driven: the UI thread hands over a complete
 * vs_ui_snapshot_s and this module decides what changed.  vs_display_flush()
 * exists because vs_display_tick() honours LVGL's refresh period, so a frame
 * rendered just before a blocking call would still be queued when the block
 * began -- and a progress page nobody sees is the same as no progress page.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
#ifndef __APP_VELASIGHT_INCLUDE_VS_DISPLAY_H
#define __APP_VELASIGHT_INCLUDE_VS_DISPLAY_H

#include "vs_types.h"

struct vs_display_s;

int vs_display_open(struct vs_display_s **display);
int vs_display_render(struct vs_display_s *display,
                      const struct vs_ui_snapshot_s *snapshot);
void vs_display_close(struct vs_display_s *display);
void vs_display_tick(struct vs_display_s *display);

/* Paint the pending frame synchronously, bypassing the refresh timer.
 *
 * vs_display_tick() only paints when LVGL's 100 ms period has elapsed, so a
 * frame rendered immediately before a blocking operation would still be queued
 * when the block began -- and a progress page nobody sees is the same as no
 * progress page.  Use this to put such a frame on the glass first.  Safe with
 * display == NULL.
 */

void vs_display_flush(struct vs_display_s *display);

#endif
