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
#define VS_MAX_FPS             10
#define VS_FRAME_INTERVAL_MS   (1000 / VS_MAX_FPS)
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

/* The specification calls the y=110..128 row the wider chord region and gives
 * it "date, time or longer meta information".  The chord is 128 px across at
 * that row's lowest scanline, so this is as wide as it can be without the ends
 * of a fifteen-character address falling off the circle -- which 104 px would
 * have clipped.
 */

#define VS_META_WIDE_X       16
#define VS_META_WIDE_WIDTH   128

/* Both rings are 152x152 and centred, hugging the bezel as the display rules
 * require.  They differ only in how much of the circle they sweep.
 *
 * A hold ring may cross the y=108 divider, because a hold hides the key hints
 * and has the screen to itself.  A level ring shares the screen with them, so
 * its ends have to stop above the line -- and the way to do that is to draw
 * less of the circle, not to draw a smaller one.
 *
 * An end at angle a sits at y = cy + r sin(a), so clearing y=108 with cy=80
 * and r=76 needs sin(a) <= 28/76, i.e. a <= 21.6 degrees or a >= 158.4.  The
 * angles below are 160 and 380 (= 20), which puts both ends at y = 106 and
 * leaves a 220 degree sweep -- still most of the circle, and visually the same
 * ring as the hold one.
 */

#define VS_RING_SIZE         152
#define VS_HOLD_ANGLE_START  135
#define VS_HOLD_ANGLE_END    405
#define VS_LEVEL_ANGLE_START 160
#define VS_LEVEL_ANGLE_END   380

#define VS_STATUS_SHORT_X       2
#define VS_STATUS_SHORT_Y       44
#define VS_STATUS_SHORT_WIDTH   156
#define VS_STATUS_SHORT_HEIGHT  48
#define VS_STATUS_LONG_X        12

/* 136 px at x=12 is the same span as the lower divider below it, so it is
 * already known to clear the bezel; the chord is 142 px wide at y=43, the
 * highest and therefore narrowest row this box occupies.  The extra 8 px over
 * the original 128 exist so a line of eight full-width CJK glyphs (16.0 px
 * each in velasight_font_16_ui) is comfortably inside the box rather than
 * exactly equal to it.  Text is centred, so wider changes nothing for the
 * single-line values that also use this layout.
 */

#define VS_STATUS_LONG_Y        43
#define VS_STATUS_LONG_WIDTH    136
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

  /* The sweep currently programmed into the arc.  Cached so the background
   * angles are only rewritten when the ring changes kind: each write ends in
   * lv_arc's value_update(), which invalidates the ring's area, and doing that
   * on every repaint would undo the point of the snapshot dirty check.
   */

  int32_t ring_start;
  int32_t ring_end;
  lv_obj_t *divider[2];
  lv_obj_t *key[VS_KEY_COUNT];
};

struct vs_display_s
{
  struct vs_panel_s panel[VS_DISPLAY_COUNT];
  lv_nuttx_result_t nuttx;
  struct vs_ui_snapshot_s previous;
  bool previous_valid;
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
                     28, 110 + line * 20, 104, VS_LOWER_ROW_HEIGHT);
      lv_label_set_long_mode(panel->status_line[line], LV_LABEL_LONG_CLIP);
      lv_obj_add_flag(panel->status_line[line], LV_OBJ_FLAG_HIDDEN);
    }
  lv_obj_set_pos(panel->status_line[1], 28, VS_LOWER_BOTTOM_Y);
  lv_obj_set_width(panel->status_line[1], 104);
  lv_obj_set_style_text_color(panel->status_line[1], vs_rgb(52, 201, 173), 0);
  vs_divider_style(panel->divider[0], 20, VS_TOP_DIVIDER_Y, 120);
  vs_divider_style(panel->divider[1], 12, VS_BOTTOM_DIVIDER_Y, 136);

  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      panel->key[key] = lv_label_create(screen);
      if (panel->key[key] == NULL)
        return -ENOMEM;

      vs_label_style(panel->key[key], vs_rgb(155, 175, 187),
                     key == VS_KEY_NEXT ? 80 : 16, VS_LOWER_TOP_Y, 64,
                     VS_LOWER_ROW_HEIGHT);
      lv_label_set_long_mode(panel->key[key], LV_LABEL_LONG_CLIP);
      lv_obj_set_style_bg_opa(panel->key[key], LV_OPA_TRANSP, 0);
    }

  panel->progress = lv_arc_create(screen);
  if (panel->progress == NULL)
    return -ENOMEM;

  lv_obj_set_size(panel->progress, VS_RING_SIZE, VS_RING_SIZE);
  lv_obj_center(panel->progress);
  lv_arc_set_range(panel->progress, 0, 100);
  lv_arc_set_bg_angles(panel->progress, VS_HOLD_ANGLE_START,
                       VS_HOLD_ANGLE_END);
  lv_arc_set_value(panel->progress, 0);
  panel->ring_start = VS_HOLD_ANGLE_START;
  panel->ring_end = VS_HOLD_ANGLE_END;
  lv_obj_set_style_arc_color(panel->progress, vs_rgb(27, 55, 72), LV_PART_MAIN);
  lv_obj_set_style_arc_width(panel->progress, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(panel->progress, vs_rgb(53, 199, 174), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(panel->progress, 4, LV_PART_INDICATOR);
  lv_obj_set_style_opa(panel->progress, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_add_flag(panel->progress, LV_OBJ_FLAG_HIDDEN);
  lv_timer_set_period(lv_display_get_refr_timer(display),
                      VS_FRAME_INTERVAL_MS);
  return 0;
}

static void vs_set_label(lv_obj_t *label, const char *text)
{
  const char *value = text != NULL ? text : "";

  if (strcmp(lv_label_get_text(label), value) != 0)
    lv_label_set_text(label, value);
}

static void vs_set_hidden(lv_obj_t *obj, bool hidden)
{
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden)
    return;

  if (hidden)
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void vs_set_text_color(lv_obj_t *obj, lv_color_t color)
{
  if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), color))
    lv_obj_set_style_text_color(obj, color, 0);
}

static void vs_set_bg_color(lv_obj_t *obj, lv_color_t color)
{
  if (!lv_color_eq(lv_obj_get_style_bg_color(obj, 0), color))
    lv_obj_set_style_bg_color(obj, color, 0);
}

static void vs_set_bg_opa(lv_obj_t *obj, lv_opa_t opa)
{
  if (lv_obj_get_style_bg_opa(obj, 0) != opa)
    lv_obj_set_style_bg_opa(obj, opa, 0);
}

static bool vs_key_changed(const struct vs_softkey_s *current,
                           const struct vs_softkey_s *previous)
{
  return current->visible != previous->visible ||
         current->highlighted != previous->highlighted ||
         strcmp(current->text, previous->text) != 0;
}

static bool vs_content_changed(const struct vs_ui_snapshot_s *current,
                               const struct vs_ui_snapshot_s *previous)
{
  return current->page != previous->page ||
         current->history_is_blank != previous->history_is_blank ||
         current->wifi_ready != previous->wifi_ready ||
         current->network.wifi_issue != previous->network.wifi_issue ||
         current->network.ap_client_count !=
         previous->network.ap_client_count ||
         strcmp(current->content_title, previous->content_title) != 0 ||
         strcmp(current->content_body, previous->content_body) != 0 ||
         strcmp(current->content_meta, previous->content_meta) != 0 ||
         strcmp(current->status_meta, previous->status_meta) != 0;
}

static bool vs_status_changed(const struct vs_ui_snapshot_s *current,
                              const struct vs_ui_snapshot_s *previous)
{
  return current->page != previous->page ||
         current->progress != previous->progress ||
         current->progress_kind != previous->progress_kind ||
         current->emotion_color != previous->emotion_color ||
         strcmp(current->status_title, previous->status_title) != 0 ||
         strcmp(current->status_value, previous->status_value) != 0 ||
         vs_key_changed(&current->softkey[VS_KEY_BACK],
                        &previous->softkey[VS_KEY_BACK]) ||
         vs_key_changed(&current->softkey[VS_KEY_CONFIRM],
                        &previous->softkey[VS_KEY_CONFIRM]) ||
         vs_key_changed(&current->softkey[VS_KEY_NEXT],
                        &previous->softkey[VS_KEY_NEXT]);
}

static void vs_panel_set_progress(struct vs_panel_s *panel,
                                  const struct vs_ui_snapshot_s *snapshot,
                                  bool visible)
{
  if (visible)
    {
      int32_t start = snapshot->progress_kind == VS_PROGRESS_LEVEL ?
                      VS_LEVEL_ANGLE_START : VS_HOLD_ANGLE_START;
      int32_t end = snapshot->progress_kind == VS_PROGRESS_LEVEL ?
                    VS_LEVEL_ANGLE_END : VS_HOLD_ANGLE_END;

      /* Only the background sweep is programmed here.  The value is the single
       * source of truth for how far the ring is filled, and
       * lv_arc_set_bg_angles() ends in lv_arc's value_update(), so changing the
       * sweep re-derives the indicator from the value on its own.
       *
       * lv_arc_set_angles() must not be used alongside it.  That call writes
       * the indicator angles directly -- passing the full sweep pegs the ring
       * at 100% -- and lv_arc_set_value() cannot put it back, because it
       * returns early when the value has not changed (lv_arc.c:237).  A level
       * being redrawn has an unchanged value by definition, which is why the
       * volume ring read full on every repaint after the first.
       */

      if (panel->ring_start != start || panel->ring_end != end)
        {
          lv_arc_set_bg_angles(panel->progress, start, end);
          panel->ring_start = start;
          panel->ring_end = end;
        }

      vs_set_hidden(panel->progress, false);
      lv_arc_set_value(panel->progress, snapshot->progress);
    }
  else
    vs_set_hidden(panel->progress, true);
}

static void vs_panel_set_keys(struct vs_panel_s *panel,
                              const struct vs_ui_snapshot_s *snapshot,
                              bool content_panel)
{
  int key;

  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      bool visible = snapshot->softkey[key].visible;

      /* The left footer carries metadata and short status only.  The right
       * footer is fixed: back/next on the first row, confirm on the second. */
      if (content_panel)
        visible = false;
      else
        {
          if (key == VS_KEY_BACK)
            {
              lv_obj_set_pos(panel->key[key], 8, VS_LOWER_TOP_Y);
              lv_obj_set_width(panel->key[key], 64);
            }
          else if (key == VS_KEY_NEXT)
            {
              lv_obj_set_pos(panel->key[key], 88, VS_LOWER_TOP_Y);
              lv_obj_set_width(panel->key[key], 64);
            }
          else
            {
              lv_obj_set_pos(panel->key[key], 28, VS_LOWER_BOTTOM_Y);
              lv_obj_set_width(panel->key[key], 104);
            }
        }

      /* A hold hides every hint but the one being acknowledged, because the
       * gesture in progress is the only thing the keys can do.  A level does
       * the opposite: the user is pressing keys to move it, so the hints are
       * what makes it operable.
       */

      if (snapshot->progress_kind != VS_PROGRESS_NONE &&
          snapshot->progress_kind != VS_PROGRESS_LEVEL &&
          !(snapshot->response_active && snapshot->response_key == key))
        visible = false;

      vs_set_label(panel->key[key], visible ? snapshot->softkey[key].text : "");
      vs_set_text_color(panel->key[key],
          snapshot->softkey[key].highlighted ? vs_rgb(7, 12, 21) :
          vs_rgb(155, 175, 187));
      vs_set_bg_color(panel->key[key], vs_rgb(53, 199, 174));
      vs_set_bg_opa(panel->key[key],
          snapshot->softkey[key].highlighted && visible ? LV_OPA_COVER :
          LV_OPA_TRANSP);
    }
}

static void vs_render_content(struct vs_panel_s *panel,
                              const struct vs_ui_snapshot_s *snapshot)
{
  /* The wider upper row carries metadata; the lower row carries the short
   * status moved from the right screen. */

  if (snapshot->page == VS_PAGE_VOLUME)
    {
      lv_obj_set_pos(panel->meta, VS_META_WIDE_X, VS_LOWER_TOP_Y);
      lv_obj_set_width(panel->meta, VS_META_WIDE_WIDTH);
    }
  else
    {
      lv_obj_set_pos(panel->meta, VS_META_X, VS_LOWER_TOP_Y);
      lv_obj_set_width(panel->meta, VS_META_WIDTH);
    }

  vs_set_label(panel->title, snapshot->content_title);
  vs_set_label(panel->body, snapshot->content_body);
  vs_set_label(panel->meta, snapshot->content_meta);
  for (int line = 0; line < 2; line++)
    vs_set_hidden(panel->status_line[line], true);
  if (snapshot->page == VS_PAGE_HISTORY_BLANK)
    {
      char line[VS_TEXT_LONG];

      if (snapshot->network.mode == VS_NET_AP)
        {
          snprintf(line, sizeof(line), "WiFi %s",
                   snapshot->network.ap_client_count != 0 ?
                   "已连接" : "待连接");
        }
      else if (snapshot->network.wifi_issue == VS_WIFI_ISSUE_SSID_NOT_FOUND)
        {
          snprintf(line, sizeof(line), "SSID未扫描到");
        }
      else if (snapshot->network.wifi_issue == VS_WIFI_ISSUE_PASSWORD)
        {
          snprintf(line, sizeof(line), "WiFi密码错误");
        }
      else if (snapshot->network.wifi_issue == VS_WIFI_ISSUE_DISCONNECTED)
        {
          snprintf(line, sizeof(line), "WiFi已断开");
        }
      else
        {
          snprintf(line, sizeof(line), "WiFi %s",
                   snapshot->wifi_ready ? "已连接" : "未连接");
        }
      vs_set_label(panel->status_line[0], line);
      vs_set_hidden(panel->status_line[0], false);
    }
  vs_set_label(panel->status_line[1], snapshot->status_meta);
  vs_set_hidden(panel->status_line[1], snapshot->status_meta[0] == '\0');
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
    }
  else
    {
      lv_obj_set_pos(panel->body, VS_STATUS_LONG_X, VS_STATUS_LONG_Y);
      lv_obj_set_size(panel->body, VS_STATUS_LONG_WIDTH,
                      VS_STATUS_LONG_HEIGHT);
    }

  vs_set_label(panel->title, snapshot->status_title);
  vs_set_label(panel->body, value);
  vs_set_label(panel->meta, "");
  vs_set_text_color(panel->body, vs_color(snapshot->emotion_color));
  vs_panel_set_progress(panel, snapshot, progress &&
                        (snapshot->progress_kind == VS_PROGRESS_HOLD ||
                         snapshot->progress_kind == VS_PROGRESS_LEVEL));
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
  bool content_dirty;
  bool status_dirty;

  if (display == NULL || snapshot == NULL)
    return -EINVAL;

  /* fb1 is the physical left content display; fb0 is the physical right
   * status display. */
  content_dirty = !display->previous_valid ||
                  vs_content_changed(snapshot, &display->previous);
  status_dirty = !display->previous_valid ||
                 vs_status_changed(snapshot, &display->previous);
  if (content_dirty)
    vs_render_content(&display->panel[VS_CONTENT_PANEL], snapshot);
  if (status_dirty)
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
  /* After reveal, LVGL's 100 ms display timers coalesce invalidations and cap
   * each panel at 10 FPS.  Only the hidden boot hand-off bypasses that cap. */
  display->previous = *snapshot;
  display->previous_valid = true;
  display->waiting = snapshot->progress_kind == VS_PROGRESS_WAIT;
  snprintf(display->wait_meta, sizeof(display->wait_meta), "%s",
           snapshot->status_meta);
  if (!display->waiting)
    {
      display->wait_phase = 0;
      display->wait_next_ms = 0;
    }
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
          vs_set_label(display->panel[VS_CONTENT_PANEL].status_line[1],
                       meta);
          display->wait_phase = (display->wait_phase + 1) % 4;
          display->wait_next_ms = now + 350;
        }
      lv_timer_handler();
    }
}

void vs_display_flush(struct vs_display_s *display)
{
  if (display == NULL)
    {
      return;
    }

  /* Both panels, and both synchronously: the two screens carry halves of one
   * message, and lv_refr_now() runs each framebuffer's FBIO_UPDATE before it
   * returns.  This is the same hand-off the boot reveal uses.
   */

  lv_refr_now(display->panel[VS_STATUS_PANEL].display);
  lv_refr_now(display->panel[VS_CONTENT_PANEL].display);
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
