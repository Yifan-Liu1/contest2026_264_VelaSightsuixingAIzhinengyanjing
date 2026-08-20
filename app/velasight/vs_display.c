#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_fbdev.h>

#include <arch/board/board.h>

#include "include/vs_display.h"

LV_FONT_DECLARE(velasight_font_16_ui);

#define VS_DISPLAY_COUNT 2
#define VS_CONTENT_PANEL 0
#define VS_STATUS_PANEL  1
#define VS_STATUS_SCALE  384

#define VS_TOP_DIVIDER_Y     32
#define VS_BOTTOM_DIVIDER_Y  108
#define VS_TITLE_X           48
#define VS_TITLE_Y           8
#define VS_TITLE_WIDTH       64
#define VS_TITLE_HEIGHT      20
#define VS_MAIN_X            12
#define VS_MAIN_Y            35
#define VS_MAIN_WIDTH        136
#define VS_MAIN_HEIGHT       71
#define VS_LOWER_TOP_Y       110
#define VS_LOWER_BOTTOM_Y    130
#define VS_LOWER_ROW_HEIGHT  18
#define VS_META_X            28
#define VS_META_WIDTH        104

#define VS_STATUS_SHORT_X       28
#define VS_STATUS_SHORT_Y       52
#define VS_STATUS_SHORT_WIDTH   104
#define VS_STATUS_SHORT_HEIGHT  32
#define VS_STATUS_LONG_X        16
#define VS_STATUS_LONG_Y        43
#define VS_STATUS_LONG_WIDTH    128
#define VS_STATUS_LONG_HEIGHT   61

struct vs_panel_s
{
  lv_display_t *display;
  lv_obj_t *screen;
  lv_obj_t *title;
  lv_obj_t *body;
  lv_obj_t *meta;
  lv_obj_t *status_line[2];
  lv_obj_t *progress;
  lv_obj_t *divider[2];
  lv_obj_t *key[VS_KEY_COUNT];
};

struct vs_display_s
{
  struct vs_panel_s panel[VS_DISPLAY_COUNT];
  lv_nuttx_result_t nuttx;
  bool revealed;
  bool waiting;
  uint8_t wait_phase;
  uint32_t wait_next_ms;
  char wait_meta[VS_TEXT_SHORT];
};

static uint32_t vs_display_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static lv_color_t vs_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
  return lv_color_make(red, green, blue);
}

static lv_color_t vs_color(uint32_t value)
{
  return vs_rgb((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

static void vs_label_style(lv_obj_t *label, lv_color_t color, int32_t x,
                           int32_t y, int32_t width, int32_t height)
{
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(label, &velasight_font_16_ui, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void vs_divider_style(lv_obj_t *divider, int32_t x, int32_t y,
                             int32_t width)
{
  lv_obj_set_pos(divider, x, y);
  lv_obj_set_size(divider, width, 1);
  lv_obj_set_style_bg_color(divider, vs_rgb(27, 55, 72), 0);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(divider, 0, 0);
  lv_obj_set_style_radius(divider, 0, 0);
}

static int vs_panel_init(struct vs_panel_s *panel, lv_display_t *display)
{
  lv_obj_t *screen;
  int key;

  if (panel == NULL || display == NULL)
    return -EINVAL;

  memset(panel, 0, sizeof(*panel));
  panel->display = display;
  screen = lv_display_get_screen_active(display);
  panel->screen = screen;
  lv_obj_set_style_bg_color(screen, vs_rgb(7, 12, 21), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  panel->title = lv_label_create(screen);
  panel->body = lv_label_create(screen);
  panel->meta = lv_label_create(screen);
  panel->status_line[0] = lv_label_create(screen);
  panel->status_line[1] = lv_label_create(screen);
  panel->divider[0] = lv_obj_create(screen);
  panel->divider[1] = lv_obj_create(screen);
  if (panel->title == NULL || panel->body == NULL || panel->meta == NULL ||
      panel->status_line[0] == NULL || panel->status_line[1] == NULL ||
      panel->divider[0] == NULL || panel->divider[1] == NULL)
    return -ENOMEM;

  vs_label_style(panel->title, vs_rgb(128, 170, 185), VS_TITLE_X,
                 VS_TITLE_Y, VS_TITLE_WIDTH, VS_TITLE_HEIGHT);
  vs_label_style(panel->body, vs_rgb(235, 242, 246), VS_MAIN_X, VS_MAIN_Y,
                 VS_MAIN_WIDTH, VS_MAIN_HEIGHT);
  vs_label_style(panel->meta, vs_rgb(52, 201, 173), VS_META_X,
                 VS_LOWER_BOTTOM_Y, VS_META_WIDTH, VS_LOWER_ROW_HEIGHT);
  lv_label_set_long_mode(panel->title, LV_LABEL_LONG_CLIP);
  lv_label_set_long_mode(panel->meta, LV_LABEL_LONG_CLIP);
  for (int line = 0; line < 2; line++)
    {
      vs_label_style(panel->status_line[line], vs_rgb(155, 175, 187),
                     8, 110 + line * 20, 144, VS_LOWER_ROW_HEIGHT);
      lv_label_set_long_mode(panel->status_line[line], LV_LABEL_LONG_CLIP);
      lv_obj_add_flag(panel->status_line[line], LV_OBJ_FLAG_HIDDEN);
    }
  vs_divider_style(panel->divider[0], 20, VS_TOP_DIVIDER_Y, 120);
  vs_divider_style(panel->divider[1], 12, VS_BOTTOM_DIVIDER_Y, 136);

  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      panel->key[key] = lv_label_create(screen);
      if (panel->key[key] == NULL)
        return -ENOMEM;

      if (key == VS_KEY_CONFIRM)
        vs_label_style(panel->key[key], vs_rgb(155, 175, 187),
                     28, VS_LOWER_TOP_Y, 104, VS_LOWER_ROW_HEIGHT);
      else
        vs_label_style(panel->key[key], vs_rgb(155, 175, 187),
                       key == VS_KEY_BACK ? 16 : 80, VS_LOWER_TOP_Y, 64,
                       VS_LOWER_ROW_HEIGHT);
      lv_label_set_long_mode(panel->key[key], LV_LABEL_LONG_CLIP);
      lv_obj_set_style_bg_opa(panel->key[key], LV_OPA_TRANSP, 0);
    }

  panel->progress = lv_arc_create(screen);
  if (panel->progress == NULL)
    return -ENOMEM;

  lv_obj_set_size(panel->progress, 152, 152);
  lv_obj_center(panel->progress);
  lv_arc_set_range(panel->progress, 0, 100);
  lv_arc_set_angles(panel->progress, 135, 405);
  lv_arc_set_bg_angles(panel->progress, 135, 405);
  lv_arc_set_value(panel->progress, 0);
  lv_obj_set_style_arc_color(panel->progress, vs_rgb(27, 55, 72), LV_PART_MAIN);
  lv_obj_set_style_arc_width(panel->progress, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(panel->progress, vs_rgb(53, 199, 174), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(panel->progress, 4, LV_PART_INDICATOR);
  lv_obj_set_style_opa(panel->progress, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_add_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
  return 0;
}

static void vs_set_label(lv_obj_t *label, const char *text)
{
  lv_label_set_text(label, text != NULL ? text : "");
}

static void vs_panel_set_progress(struct vs_panel_s *panel,
                                  const struct vs_ui_snapshot_s *snapshot,
                                  bool visible)
{
  if (visible)
    {
      lv_obj_remove_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
      lv_arc_set_value(panel->progress, snapshot->progress);
    }
  else
    lv_obj_add_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
}

static void vs_panel_set_keys(struct vs_panel_s *panel,
                              const struct vs_ui_snapshot_s *snapshot,
                              bool content_panel)
{
  int key;

  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      bool visible = snapshot->softkey[key].visible;

      /* The left physical screen carries the confirm action.  The right
       * physical screen carries back and next, so the shared key row does not
       * duplicate a core action. */
      if (content_panel)
        {
          visible = key == VS_KEY_CONFIRM && visible;
          if (snapshot->page == VS_PAGE_HISTORY_BLANK)
            visible = false;
          if (key == VS_KEY_CONFIRM)
            lv_obj_set_y(panel->key[key], VS_LOWER_BOTTOM_Y);
        }
      else
        {
          visible = key != VS_KEY_CONFIRM && visible;
          lv_obj_set_y(panel->key[key], VS_LOWER_TOP_Y);
        }

      if (snapshot->progress_kind != VS_PROGRESS_NONE)
        visible = false;

      vs_set_label(panel->key[key], visible ? snapshot->softkey[key].text : "");
      lv_obj_set_style_text_color(panel->key[key],
          snapshot->softkey[key].highlighted ? vs_rgb(7, 12, 21) :
          vs_rgb(155, 175, 187), 0);
      lv_obj_set_style_bg_color(panel->key[key], vs_rgb(53, 199, 174), 0);
      lv_obj_set_style_bg_opa(panel->key[key],
          snapshot->softkey[key].highlighted && visible ? LV_OPA_COVER :
          LV_OPA_TRANSP, 0);
    }
}

static void vs_render_content(struct vs_panel_s *panel,
                              const struct vs_ui_snapshot_s *snapshot)
{
  /* The wider upper row of the circular footer carries dates and other long
   * metadata.  The confirm action uses the shorter bottom chord. */

  lv_obj_set_y(panel->meta, VS_LOWER_TOP_Y);
  vs_set_label(panel->title, snapshot->content_title);
  vs_set_label(panel->body, snapshot->content_body);
  vs_set_label(panel->meta, snapshot->content_meta);
  for (int line = 0; line < 2; line++)
    lv_obj_add_flag(panel->status_line[line], LV_OBJ_FLAG_HIDDEN);
  if (snapshot->page == VS_PAGE_HISTORY_BLANK)
    {
      char line[VS_TEXT_LONG];

      snprintf(line, sizeof(line), "wifi:%s api:%s",
               snapshot->wifi_ready ? "已连接" : "未连接",
               snapshot->api_ready ? "可用" : "错误");
      vs_set_label(panel->status_line[0], line);
      /* Battery ADC is not initialized on this board yet; never show a
       * fabricated percentage while the measurement source is unavailable. */
      vs_set_label(panel->status_line[1], "电量:错误");
      for (int status_line = 0; status_line < 2; status_line++)
        lv_obj_remove_flag(panel->status_line[status_line], LV_OBJ_FLAG_HIDDEN);
    }
  vs_panel_set_progress(panel, snapshot, false);
  vs_panel_set_keys(panel, snapshot, true);
}

static void vs_render_status(struct vs_panel_s *panel,
                             const struct vs_ui_snapshot_s *snapshot)
{
  char value[VS_TEXT_LONG];
  bool progress = snapshot->progress_kind != VS_PROGRESS_NONE &&
                  snapshot->page != VS_PAGE_ERROR;
  bool short_value;

  snprintf(value, sizeof(value), "%s", snapshot->status_value);
  short_value = strlen(value) <= 12;
  if (short_value)
    {
      lv_obj_set_pos(panel->body, VS_STATUS_SHORT_X, VS_STATUS_SHORT_Y);
      lv_obj_set_size(panel->body, VS_STATUS_SHORT_WIDTH,
                      VS_STATUS_SHORT_HEIGHT);
      lv_obj_set_style_transform_scale(panel->body, VS_STATUS_SCALE, 0);
      lv_obj_set_style_transform_pivot_x(panel->body,
                                         VS_STATUS_SHORT_WIDTH / 2, 0);
      lv_obj_set_style_transform_pivot_y(panel->body,
                                         VS_STATUS_SHORT_HEIGHT / 2, 0);
    }
  else
    {
      lv_obj_set_pos(panel->body, VS_STATUS_LONG_X, VS_STATUS_LONG_Y);
      lv_obj_set_size(panel->body, VS_STATUS_LONG_WIDTH,
                      VS_STATUS_LONG_HEIGHT);
      lv_obj_set_style_transform_scale(panel->body, LV_SCALE_NONE, 0);
    }

  lv_obj_set_y(panel->meta, VS_LOWER_BOTTOM_Y);
  vs_set_label(panel->title, snapshot->status_title);
  vs_set_label(panel->body, value);
  vs_set_label(panel->meta, snapshot->status_meta);
  lv_obj_set_style_text_color(panel->body, vs_color(snapshot->emotion_color), 0);
  vs_panel_set_progress(panel, snapshot, progress &&
                        snapshot->progress_kind == VS_PROGRESS_HOLD);
  vs_panel_set_keys(panel, snapshot, false);
}

int vs_display_open(struct vs_display_s **display)
{
  struct vs_display_s *state;
  lv_nuttx_dsc_t descriptor;
  int ret;

  if (display == NULL)
    return -EINVAL;

  state = calloc(1, sizeof(*state));
  if (state == NULL)
    return -ENOMEM;

  lv_init();
  lv_nuttx_dsc_init(&descriptor);
  descriptor.fb_path = "/dev/fb0";
  lv_nuttx_init(&descriptor, &state->nuttx);
  if (state->nuttx.disp == NULL)
    {
      free(state);
      return -ENODEV;
    }

  ret = vs_panel_init(&state->panel[VS_STATUS_PANEL], state->nuttx.disp);
  if (ret < 0)
    goto fail_status;

  state->panel[VS_CONTENT_PANEL].display = lv_nuttx_fbdev_create();
  if (state->panel[VS_CONTENT_PANEL].display == NULL ||
      lv_nuttx_fbdev_set_file(state->panel[VS_CONTENT_PANEL].display,
                              "/dev/fb1") < 0)
    {
      ret = -ENODEV;
      goto fail_content;
    }

  ret = vs_panel_init(&state->panel[VS_CONTENT_PANEL],
                      state->panel[VS_CONTENT_PANEL].display);
  if (ret < 0)
    goto fail_content;

  *display = state;
  return 0;

fail_content:
  if (state->panel[VS_CONTENT_PANEL].display != NULL)
    lv_display_delete(state->panel[VS_CONTENT_PANEL].display);
fail_status:
  lv_nuttx_deinit(&state->nuttx);
  free(state);
  return ret;
}

int vs_display_render(struct vs_display_s *display,
                      const struct vs_ui_snapshot_s *snapshot)
{
  if (display == NULL || snapshot == NULL)
    return -EINVAL;

  /* fb1 is the physical left content display; fb0 is the physical right
   * status display. */
  vs_render_content(&display->panel[VS_CONTENT_PANEL], snapshot);
  vs_render_status(&display->panel[VS_STATUS_PANEL], snapshot);

  if (!display->revealed)
    {
      /* Match the old synchronous boot animation's hand-off: both complete
       * frames must reach panel GRAM before the shared backlight is enabled.
       * lv_refr_now() executes each framebuffer's FBIO_UPDATE synchronously. */

      lv_obj_invalidate(display->panel[VS_STATUS_PANEL].screen);
      lv_obj_invalidate(display->panel[VS_CONTENT_PANEL].screen);
      lv_refr_now(display->panel[VS_STATUS_PANEL].display);
      lv_refr_now(display->panel[VS_CONTENT_PANEL].display);
#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
      bk7258_display_reveal();
#endif
      display->revealed = true;
    }
  display->waiting = snapshot->progress_kind == VS_PROGRESS_WAIT;
  snprintf(display->wait_meta, sizeof(display->wait_meta), "%s",
           snapshot->status_meta);
  if (!display->waiting)
    {
      display->wait_phase = 0;
      display->wait_next_ms = 0;
    }
  if (display->revealed)
    lv_timer_handler();
  return 0;
}

void vs_display_tick(struct vs_display_s *display)
{
  if (display != NULL)
    {
      uint32_t now = vs_display_now_ms();

      if (display->waiting &&
          (display->wait_next_ms == 0 ||
           (int32_t)(now - display->wait_next_ms) >= 0))
        {
          static const char *const phases[] = {".", "..", "...", ""};
          char meta[VS_TEXT_LONG];

          snprintf(meta, sizeof(meta), "%s%s", display->wait_meta,
                   phases[display->wait_phase]);
          vs_set_label(display->panel[VS_STATUS_PANEL].meta,
                       meta);
          display->wait_phase = (display->wait_phase + 1) % 4;
          display->wait_next_ms = now + 350;
        }
      lv_timer_handler();
    }
}

void vs_display_close(struct vs_display_s *display)
{
  if (display == NULL)
    return;

  if (display->panel[VS_CONTENT_PANEL].display != NULL)
    lv_display_delete(display->panel[VS_CONTENT_PANEL].display);
  lv_nuttx_deinit(&display->nuttx);
  lv_deinit();
  free(display);
}
