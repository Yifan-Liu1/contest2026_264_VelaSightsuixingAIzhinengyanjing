#ifndef __APP_VELASIGHT_INCLUDE_VS_DISPLAY_H
#define __APP_VELASIGHT_INCLUDE_VS_DISPLAY_H

#include "vs_types.h"

struct vs_display_s;

int vs_display_open(struct vs_display_s **display);
int vs_display_render(struct vs_display_s *display,
                      const struct vs_ui_snapshot_s *snapshot);
void vs_display_close(struct vs_display_s *display);
void vs_display_tick(struct vs_display_s *display);

#endif
