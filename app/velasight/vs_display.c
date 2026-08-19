#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_fbdev.h>

#include "include/vs_display.h"

LV_FONT_DECLARE(velasight_font_16_ui);

#define VS_DISPLAY_COUNT 2

struct vs_panel_s
{
  lv_display_t *display;
  lv_obj_t *screen;
  lv_obj_t *heading;
  lv_obj_t *primary;
  lv_obj_t *secondary;
  lv_obj_t *progress;
};

struct vs_display_s
{
  struct vs_panel_s panel[VS_DISPLAY_COUNT];
  lv_nuttx_result_t nuttx;
};

static lv_color_t vs_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
  return lv_color_make(red, green, blue);
}

static void vs_label_style(lv_obj_t *label, lv_color_t color, int32_t y,
                           int32_t height)
{
  lv_obj_set_width(label, 140);
  lv_obj_set_height(label, height);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(label, &velasight_font_16_ui, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static int vs_panel_init(struct vs_panel_s *panel, lv_display_t *display)
{
  lv_obj_t *screen;

  if (panel == NULL || display == NULL)
    {
      return -EINVAL;
    }

  memset(panel, 0, sizeof(*panel));
  panel->display = display;
  screen = lv_display_get_screen_active(display);
  panel->screen = screen;
  lv_obj_set_style_bg_color(screen, vs_rgb(8, 12, 24), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  panel->heading = lv_label_create(screen);
  panel->primary = lv_label_create(screen);
  panel->secondary = lv_label_create(screen);
  if (panel->heading == NULL || panel->primary == NULL ||
      panel->secondary == NULL)
    {
      return -ENOMEM;
    }

  vs_label_style(panel->heading, vs_rgb(140, 180, 200), 12, 24);
  vs_label_style(panel->primary, vs_rgb(230, 240, 255), 42, 68);
  vs_label_style(panel->secondary, vs_rgb(40, 210, 170), 116, 35);

  panel->progress = lv_arc_create(screen);
  if (panel->progress == NULL)
    {
      return -ENOMEM;
    }

  lv_obj_set_size(panel->progress, 118, 118);
  lv_obj_center(panel->progress);
  lv_arc_set_range(panel->progress, 0, 100);
  lv_arc_set_angles(panel->progress, 135, 405);
  lv_arc_set_bg_angles(panel->progress, 135, 405);
  lv_arc_set_value(panel->progress, 0);
  lv_obj_set_style_arc_color(panel->progress, vs_rgb(25, 55, 75),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_width(panel->progress, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_color(panel->progress, vs_rgb(40, 210, 170),
                             LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(panel->progress, 5, LV_PART_INDICATOR);
  lv_obj_set_style_opa(panel->progress, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_add_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
  return 0;
}

static void vs_panel_set_text(struct vs_panel_s *panel, const char *heading,
                              const char *primary, const char *secondary)
{
  lv_label_set_text(panel->heading, heading != NULL ? heading : "");
  lv_label_set_text(panel->primary, primary != NULL ? primary : "");
  lv_label_set_text(panel->secondary, secondary != NULL ? secondary : "");
}

static void vs_panel_set_progress(struct vs_panel_s *panel, bool visible,
                                  uint8_t progress)
{
  if (visible)
    {
      lv_obj_remove_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
      lv_arc_set_value(panel->progress, progress);
    }
  else
    {
      lv_obj_add_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
    }
}

static void vs_history_date(const char *date, char *compact, size_t size)
{
  const char *separator = strchr(date, ' ');

  if (separator == NULL)
    {
      snprintf(compact, size, "%s", date);
    }
  else
    {
      snprintf(compact, size, "%.*s\n%s", (int)(separator - date), date,
               separator + 1);
    }
}

static void vs_render_left(struct vs_panel_s *panel,
                           const struct vs_ui_snapshot_s *snapshot)
{
  char index[16];
  bool progress = false;

  snprintf(index, sizeof(index), "%02u/%02u", snapshot->history_index + 1,
           snapshot->history_count);
  if (snapshot->page == VS_PAGE_SOCIAL_ENTER)
    {
      vs_panel_set_text(panel, "社交模式", index, "请按住");
      progress = true;
    }
  else if (snapshot->page == VS_PAGE_SOCIAL_EXITING)
    {
      vs_panel_set_text(panel, "退出社交", "退出", "请按住");
      progress = true;
    }
  else if (snapshot->page == VS_PAGE_NET_SWITCHING)
    {
      vs_panel_set_text(panel, "网络切换",
                        snapshot->network_target_ap ? "切换 AP" : "切换 STA",
                        "请按住");
      progress = true;
    }
  else if (snapshot->page == VS_PAGE_HISTORY)
    {
      vs_panel_set_text(panel, "历史记录", index, "选择");
    }
  else
    {
      vs_panel_set_text(panel, "状态", snapshot->primary,
                        snapshot->secondary);
      progress = snapshot->show_progress;
    }

  vs_panel_set_progress(panel, progress, snapshot->progress);
}

static void vs_render_right(struct vs_panel_s *panel,
                            const struct vs_ui_snapshot_s *snapshot)
{
  char date[32];

  if (snapshot->page == VS_PAGE_HISTORY && snapshot->history != NULL)
    {
      vs_history_date(snapshot->history->date, date, sizeof(date));
      vs_panel_set_text(panel, "记录", snapshot->history->title, date);
    }
  else
    {
      vs_panel_set_text(panel, "状态", snapshot->primary,
                        snapshot->secondary);
    }

  vs_panel_set_progress(panel, false, 0);
}

int vs_display_open(struct vs_display_s **display)
{
  struct vs_display_s *state;
  lv_nuttx_dsc_t descriptor;
  int ret;

  if (display == NULL)
    {
      return -EINVAL;
    }

  state = calloc(1, sizeof(*state));
  if (state == NULL)
    {
      return -ENOMEM;
    }

  lv_init();
  lv_nuttx_dsc_init(&descriptor);
  descriptor.fb_path = "/dev/fb0";
  lv_nuttx_init(&descriptor, &state->nuttx);
  if (state->nuttx.disp == NULL)
    {
      free(state);
      return -ENODEV;
    }

  state->panel[0].display = state->nuttx.disp;
  ret = vs_panel_init(&state->panel[0], state->nuttx.disp);
  if (ret < 0)
    {
      lv_nuttx_deinit(&state->nuttx);
      free(state);
      return ret;
    }

  state->panel[1].display = lv_nuttx_fbdev_create();
  if (state->panel[1].display == NULL ||
      lv_nuttx_fbdev_set_file(state->panel[1].display, "/dev/fb1") < 0)
    {
      if (state->panel[1].display != NULL)
        {
          lv_display_delete(state->panel[1].display);
        }
      lv_nuttx_deinit(&state->nuttx);
      free(state);
      return -ENODEV;
    }

  ret = vs_panel_init(&state->panel[1], state->panel[1].display);
  if (ret < 0)
    {
      lv_display_delete(state->panel[1].display);
      lv_nuttx_deinit(&state->nuttx);
      free(state);
      return ret;
    }

  *display = state;
  return 0;
}

int vs_display_render(struct vs_display_s *display,
                      const struct vs_ui_snapshot_s *snapshot)
{
  if (display == NULL || snapshot == NULL)
    {
      return -EINVAL;
    }

  vs_render_left(&display->panel[0], snapshot);
  vs_render_right(&display->panel[1], snapshot);
  lv_timer_handler();
  return 0;
}

void vs_display_tick(struct vs_display_s *display)
{
  if (display != NULL)
    {
      lv_timer_handler();
    }
}

void vs_display_close(struct vs_display_s *display)
{
  if (display == NULL)
    {
      return;
    }

  if (display->panel[1].display != NULL)
    {
      lv_display_delete(display->panel[1].display);
    }
  lv_nuttx_deinit(&display->nuttx);
  lv_deinit();
  free(display);
}
