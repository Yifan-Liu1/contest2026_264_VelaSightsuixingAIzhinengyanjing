/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_status_screen.c
 *
 * Event-driven status and summary screen.
 *
 * Why this exists instead of the live preview
 * ------------------------------------------
 * The product spec is explicit: the panels must not run a real-time camera
 * preview, and they must refresh only when a key, a state or a result
 * changes -- "屏幕仅事件触发刷新，不运行实时 Camera Preview", with the
 * display carrying "状态" and "一至两行摘要".  A 160x160 round panel cannot
 * usefully show a 640x480 scene anyway, and a 30fps preview costs 24-26ms of
 * QSPI per panel per frame, which is exactly the budget the spec wants left
 * for audio.
 *
 * camera_preview stays in the tree as a *diagnostic*: it is how a byte-order
 * or geometry fault is made visible (docs/reference/display.md).  It is no
 * longer what the product path draws.
 *
 * What it draws
 * -------------
 *   physical right panel (fb0): the state -- one word plus a state colour, so the
 *                      state is readable across the room.
 *   physical left panel (fb1): up to two lines of summary text.
 *
 * Text uses the stroke font this board already has (hello_font.c).  That font
 * covers ASCII only, so Chinese summaries -- which is what the cloud actually
 * returns -- cannot be drawn yet; bk7258_status_screen_summary() reports that
 * by returning -ENOSYS for non-ASCII input rather than drawing a row of
 * blanks and pretending.  Wiring in a CJK glyph source is tracked separately.
 *
 * Refresh is one full-frame push per panel, only when something changed: the
 * panel takes no partial update (see gc9d01_updatearea()), and repainting an
 * unchanged screen would spend 25ms for nothing.  Nothing here holds a camera,
 * audio or network lock, and a failed push is reported but never propagated --
 * the spec requires display failure not to break the data path.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arch/board/hello_paint.h>

#include "bk7258_gc9d01_fb.h"
#include "bk7258_status_screen.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define STATUS_PANEL   0        /* fb0 shows the state */
#define SUMMARY_PANEL  1        /* fb1 shows the summary */

#define SUMMARY_LINES  2
#define SUMMARY_CHARS  24

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* What is on the glass right now, so an unchanged update costs nothing. */

static enum bk7258_status_e g_shown_state = BK7258_STATUS_MAX;
static char g_shown_summary[SUMMARY_LINES][SUMMARY_CHARS + 1];

/* State names and colours.  The words are the spec's own state names for the
 * two modes, kept short enough to read on a 160px round panel.
 */

static const struct
{
  FAR const char *text;
  uint16_t colour;              /* RGB565 */
}
g_states[BK7258_STATUS_MAX] =
{
  [BK7258_STATUS_IDLE]         = { "idle",    0x4208u },  /* grey  */
  [BK7258_STATUS_CONSENT]      = { "ready?",  0xfd20u },  /* amber */
  [BK7258_STATUS_RECORDING]    = { "rec",     0xf800u },  /* red   */
  [BK7258_STATUS_FINALIZING]   = { "wait",    0xfd20u },
  [BK7258_STATUS_RESULT_READY] = { "done",    0x07e0u },  /* green */
  [BK7258_STATUS_LISTENING]    = { "listen",  0x001fu },  /* blue  */
  [BK7258_STATUS_TRANSCRIBING] = { "...",     0x001fu },
  [BK7258_STATUS_THINKING]     = { "think",   0x001fu },
  [BK7258_STATUS_SPEAKING]     = { "speak",   0x07e0u },
  [BK7258_STATUS_CANCELLED]    = { "cancel",  0x4208u },
  [BK7258_STATUS_ERROR]        = { "error",   0xf800u },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool status_is_ascii(FAR const char *text)
{
  while (*text != '\0')
    {
      if ((unsigned char)*text > 0x7fu)
        {
          return false;
        }

      text++;
    }

  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_status_screen_state(enum bk7258_status_e state)
{
  int ret;

  if ((unsigned int)state >= BK7258_STATUS_MAX)
    {
      return -EINVAL;
    }

  if (state == g_shown_state)
    {
      return OK;                /* Already on the glass */
    }

  ret = bk7258_gc9d01_fb_text(STATUS_PANEL, g_states[state].text,
                              g_states[state].colour);
  if (ret < 0)
    {
      /* Display trouble must not stop the caller: the spec puts the data path
       * ahead of the screen.
       */

      printf("status_screen: state %s not drawn: %d\n",
             g_states[state].text, ret);
      return ret;
    }

  g_shown_state = state;

  /* Entering idle also clears the summary panel.
   *
   * Without this the two panels disagree after boot: the greeting is drawn on
   * both, then only the state panel is repainted, so one round display says
   * "idle" while the other still says "hello vela" -- which reads as a broken
   * panel, and was reported as one.  It matters beyond boot too: a summary
   * from a finished session must not sit next to an idle state, or the screen
   * claims something the device is no longer doing.
   */

  if (state == BK7258_STATUS_IDLE)
    {
      int cleared = bk7258_gc9d01_fb_two_lines(SUMMARY_PANEL, "", "");

      if (cleared < 0)
        {
          printf("status_screen: summary panel not cleared: %d\n", cleared);
        }
      else
        {
          g_shown_summary[0][0] = '\0';
          g_shown_summary[1][0] = '\0';
        }
    }

  /* One line per actual repaint.  Kept because it is the only outside
   * evidence that the panel was painted -- there is no way to read the glass
   * back -- and because it makes the "only on change" rule checkable: a
   * screen that repaints on a timer would show up here as a stream.
   */

  printf("status_screen: state %s drawn\n", g_states[state].text);
  return OK;
}

int bk7258_status_screen_summary(FAR const char *line1, FAR const char *line2)
{
  char l1[SUMMARY_CHARS + 1];
  char l2[SUMMARY_CHARS + 1];
  int ret;

  strlcpy(l1, line1 != NULL ? line1 : "", sizeof(l1));
  strlcpy(l2, line2 != NULL ? line2 : "", sizeof(l2));

  if (!status_is_ascii(l1) || !status_is_ascii(l2))
    {
      /* The stroke font is ASCII-only.  Say so instead of drawing blanks:
       * a caller that gets -ENOSYS can fall back to TTS, which is the spec's
       * primary output channel anyway.
       */

      return -ENOSYS;
    }

  if (strcmp(l1, g_shown_summary[0]) == 0 &&
      strcmp(l2, g_shown_summary[1]) == 0)
    {
      return OK;
    }

  ret = bk7258_gc9d01_fb_two_lines(SUMMARY_PANEL, l1, l2);
  if (ret < 0)
    {
      printf("status_screen: summary not drawn: %d\n", ret);
      return ret;
    }

  strlcpy(g_shown_summary[0], l1, sizeof(g_shown_summary[0]));
  strlcpy(g_shown_summary[1], l2, sizeof(g_shown_summary[1]));
  return OK;
}

void bk7258_status_screen_invalidate(void)
{
  g_shown_state = BK7258_STATUS_MAX;
  g_shown_summary[0][0] = '\0';
  g_shown_summary[1][0] = '\0';
}
