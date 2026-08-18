/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.c
 *
 * openvela framebuffer driver for the GC9D01 panels (160x160 round, RGB565,
 * one per QSPI controller).  Implements the three hooks fb_register() needs
 * -- up_fbinitialize(), up_fbgetvplane(), up_fbuninitialize() -- plus the
 * fb_vtable_s entries required for a panel that has no autonomous refresh:
 * getvideoinfo, getplaneinfo and updatearea.
 *
 * Why updatearea is mandatory here: a RAM-less RGB parallel panel is
 * scanned out continuously by an LCD controller, so writing the
 * framebuffer is enough.  GC9D01 has its own GRAM and is written over a
 * serial bus, so nothing reaches the glass until the driver explicitly
 * pushes it.  Without CONFIG_FB_UPDATE + this callback the screen would
 * simply never change.
 *
 * ---------------------------------------------------------------------
 * Multiple displays
 * ---------------------------------------------------------------------
 * fb_register(display, plane) (include/nuttx/video/fb.h) calls
 * up_fbinitialize(display), then up_fbgetvplane(display, plane), then
 * fb_register_device(), which names the node /dev/fb<display>.  So exposing
 * two panels is just a matter of calling fb_register() twice.
 *
 * The constraint that shapes this file: fb_vtable_s's callbacks receive
 * only the vtable pointer, with no display argument.  A single shared
 * vtable therefore cannot tell which panel it is being asked about.  Each
 * display consequently gets its own state structure whose *first* member is
 * the vtable, so a callback can recover its own state by casting the vtable
 * pointer back (the usual container-of trick, valid here because the vtable
 * sits at offset 0).
 *
 * Framebuffers come from the kernel heap: 2 x 51200 = 100KB out of roughly
 * 300KB, which is affordable and much faster for the CPU to write than
 * PSRAM (which is mapped non-cacheable).  The camera's own frame buffers do
 * live in PSRAM -- they are 614400 bytes each and have no alternative.
 *
 * The panels are round: the corners of the 160x160 square are not visible.
 * That is a composition concern for whatever draws into the framebuffer,
 * not something this driver compensates for.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>
#include <nuttx/video/fb.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <arch/board/hello_paint.h>

#include "bk7258_qspi.h"
#include "bk7258_gc9d01_fb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* vtable MUST stay first: the callbacks recover this structure by casting
 * their vtable argument, since fb_vtable_s carries no display index.
 */

struct gc9d01_fb_s
{
  struct fb_vtable_s vtable;
  int display;
  int bus;
  FAR uint8_t *fbmem;
  uint32_t updates;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gc9d01_getvideoinfo(FAR struct fb_vtable_s *vtable,
                               FAR struct fb_videoinfo_s *vinfo);
static int gc9d01_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                               FAR struct fb_planeinfo_s *pinfo);
#ifdef CONFIG_FB_UPDATE
static int gc9d01_updatearea(FAR struct fb_vtable_s *vtable,
                             FAR const struct fb_area_s *area);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct gc9d01_fb_s g_gc9d01_fb[GC9D01_NDISPLAYS] =
{
  {
    .vtable =
      {
        .getvideoinfo = gc9d01_getvideoinfo,
        .getplaneinfo = gc9d01_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
        .updatearea   = gc9d01_updatearea,
#endif
      },
    .display = 0,
    .bus     = -1,
  },
  {
    .vtable =
      {
        .getvideoinfo = gc9d01_getvideoinfo,
        .getplaneinfo = gc9d01_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
        .updatearea   = gc9d01_updatearea,
#endif
      },
    .display = 1,
    .bus     = -1,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct gc9d01_fb_s *
gc9d01_from_vtable(FAR struct fb_vtable_s *vtable)
{
  /* Safe because .vtable is the first member of struct gc9d01_fb_s. */

  return (FAR struct gc9d01_fb_s *)vtable;
}

static int gc9d01_getvideoinfo(FAR struct fb_vtable_s *vtable,
                               FAR struct fb_videoinfo_s *vinfo)
{
  if (vtable == NULL || vinfo == NULL)
    {
      return -EINVAL;
    }

  vinfo->fmt     = FB_FMT_RGB16_565;
  vinfo->xres    = GC9D01_XRES;
  vinfo->yres    = GC9D01_YRES;
  vinfo->nplanes = 1;

  return OK;
}

static int gc9d01_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                               FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct gc9d01_fb_s *priv;

  if (vtable == NULL || pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  priv = gc9d01_from_vtable(vtable);

  /* Single buffer: fbcount is 1, so yres_virtual == yres and fblen is one
   * frame.  If a second buffer is added later, fblen/yres_virtual must grow
   * with it.
   */

  pinfo->fbmem        = priv->fbmem;
  pinfo->fblen        = GC9D01_FBLEN;
  pinfo->stride       = GC9D01_STRIDE;
  pinfo->display      = priv->display;
  pinfo->bpp          = GC9D01_BPP;
  pinfo->xres_virtual = GC9D01_XRES;
  pinfo->yres_virtual = GC9D01_YRES;
  pinfo->xoffset      = 0;
  pinfo->yoffset      = 0;

  return OK;
}

/****************************************************************************
 * Name: gc9d01_push
 *
 * Description:
 *   Re-assert the full-panel window and stream the framebuffer out.
 *
 ****************************************************************************/

static int gc9d01_push(FAR struct gc9d01_fb_s *priv)
{
  clock_t start = clock_systime_ticks();

  if (priv->fbmem == NULL || priv->bus < 0)
    {
      return -ENODEV;
    }

  if (!bk7258_gc9d01_window_full(priv->display))
    {
      return -EIO;
    }

  if (!bk7258_lcd_spi_write_frame(priv->bus, priv->fbmem, GC9D01_FBLEN))
    {
      printf("gc9d01_fb[%d]: pixel burst failed\n", priv->display);
      return -EIO;
    }

  priv->updates++;

  /* First few pushes get a timing report: it is the only way to know the
   * real cost of a frame on this bus, which decides whether the preview
   * path needs DMA.  After that stay quiet -- this runs per displayed
   * frame.
   */

  if (priv->updates <= 3)
    {
      uint32_t ms = TICK2MSEC(clock_systime_ticks() - start);

      printf("gc9d01_fb[%d]: update %u: %d bytes in %ums "
             "(%u frames on bus)\n", priv->display,
             (unsigned int)priv->updates, GC9D01_FBLEN, (unsigned int)ms,
             (unsigned int)bk7258_lcd_spi_frame_count(priv->bus));
    }

  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int gc9d01_updatearea(FAR struct fb_vtable_s *vtable,
                             FAR const struct fb_area_s *area)
{
  /* Partial updates would need a per-row copy out of the framebuffer,
   * because the panel expects a densely packed rectangle while the
   * framebuffer rows are GC9D01_STRIDE apart.  At 51200 bytes a full frame
   * is cheap enough that this always pushes everything and ignores the
   * requested rectangle.
   */

  UNUSED(area);

  if (vtable == NULL)
    {
      return -EINVAL;
    }

  return gc9d01_push(gc9d01_from_vtable(vtable));
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  FAR struct gc9d01_fb_s *priv;
  int ret;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];

  if (priv->fbmem == NULL)
    {
      priv->fbmem = kmm_zalloc(GC9D01_FBLEN);
      if (priv->fbmem == NULL)
        {
          printf("gc9d01_fb[%d]: cannot allocate %d-byte framebuffer\n",
                 display, GC9D01_FBLEN);
          return -ENOMEM;
        }
    }

  /* Rail, panel reset, bus bring-up and the init command sequence.  Kept in
   * bk7258_gc9d01.c so the command table and the pin map have a single
   * owner.
   */

  ret = bk7258_gc9d01_panel_init(display);
  if (ret < 0)
    {
      printf("gc9d01_fb[%d]: panel init failed: %d\n", display, ret);
      kmm_free(priv->fbmem);
      priv->fbmem = NULL;
      return ret;
    }

  priv->bus = bk7258_gc9d01_bus(display);

  printf("gc9d01_fb[%d]: %dx%d RGB565, framebuffer %d bytes at %p, bus %d\n",
         display, GC9D01_XRES, GC9D01_YRES, GC9D01_FBLEN, priv->fbmem,
         priv->bus);

  return OK;
}

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display < 0 || display >= GC9D01_NDISPLAYS || vplane != 0)
    {
      return NULL;
    }

  return &g_gc9d01_fb[display].vtable;
}

void up_fbuninitialize(int display)
{
  UNUSED(display);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_fill
 *
 * Description:
 *   Bring-up helper: fills one display's framebuffer with a solid RGB565
 *   colour and pushes it.  This is the first thing that can prove the whole
 *   chain (bus framing, init sequence, CASET/RASET, pixel burst) actually
 *   reaches the glass -- the panel has no readable ID register, so until
 *   something is visible there is no evidence the init sequence worked.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_fill(int display, uint16_t rgb565)
{
  FAR struct gc9d01_fb_s *priv;
  FAR uint16_t *px;
  int i;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  px = (FAR uint16_t *)priv->fbmem;

  if (px == NULL)
    {
      return -ENODEV;
    }

  for (i = 0; i < GC9D01_XRES * GC9D01_YRES; i++)
    {
      px[i] = rgb565;
    }

  printf("gc9d01_fb[%d]: fill 0x%04x pushed\n", display, rgb565);

  return gc9d01_push(priv);
}

/****************************************************************************
 * Name: greeting_style
 *
 * Description:
 *   The pen and layout the boot greeting is drawn with.  These are the same
 *   numbers as 'hello em=52 thick=4', so the boot animation and the shell
 *   command produce the same strokes -- they now share one renderer
 *   (hello_paint.c) and one stroke font (hello_font.c) instead of the panel
 *   driver carrying its own 5x7 bitmap copy of the word.
 *
 ****************************************************************************/

#define GC9D01_GREETING_TEXT   "hello vela"
#define GC9D01_GREETING_EM     52
#define GC9D01_GREETING_THICK  4
#define GC9D01_GREETING_STEPS  20

static int32_t greeting_style(FAR struct hs_style_s *st)
{
  int32_t budget = (GC9D01_XRES * 16 * 85) / 100;
  int32_t width = 0;
  int32_t total;

  memset(st, 0, sizeof(*st));
  st->fg      = 0xffff;
  st->bg      = 0x0000;
  st->em16    = GC9D01_GREETING_EM * 16;
  st->thick16 = GC9D01_GREETING_THICK * 16;

  /* Baseline is nudged below the centre by the same fraction the command
   * uses, so the word sits optically centred rather than mathematically.
   */

  st->baseline16 = (GC9D01_YRES * 16) / 2 + (st->em16 * 10) / 64;
  total = hs_measure(GC9D01_GREETING_TEXT, st, &width);

  /* Auto-fit, same 85% chord budget as the shell command.  The glass is
   * round, so the usable width is a chord rather than the full 160 pixels,
   * and GC9D01_GREETING_EM is the requested size rather than a promise: at
   * em 52 "hello vela" is about 180 pixels wide and would lose its first
   * letter off the left edge, which looks like a font bug rather than an
   * overflow.
   */

  if (width > budget && width > 0)
    {
      st->em16 = (st->em16 * budget) / width;
      st->thick16 = (st->thick16 * budget) / width;

      if (st->thick16 < 2 * 16)
        {
          st->thick16 = 2 * 16;
        }

      st->baseline16 = (GC9D01_YRES * 16) / 2 + (st->em16 * 10) / 64;
      total = hs_measure(GC9D01_GREETING_TEXT, st, &width);
    }

  st->cx16 = (GC9D01_XRES * 16 - width) / 2;
  return total;
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_hello
 *
 * Description:
 *   The greeting, complete, in one shot.  Kept for callers that cannot
 *   animate; the boot path uses bk7258_gc9d01_fb_hello_animate().
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_hello(int display)
{
  FAR struct gc9d01_fb_s *priv;
  struct hs_style_s st;
  int32_t total;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  if (priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  total = greeting_style(&st);

  hs_clear(priv->fbmem, GC9D01_STRIDE, GC9D01_XRES, GC9D01_YRES, &st);
  hs_stroke(priv->fbmem, GC9D01_STRIDE, GC9D01_XRES, GC9D01_YRES,
            GC9D01_GREETING_TEXT, &st, 0, total);

  return gc9d01_push(priv);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_hello_animate
 *
 * Description:
 *   Writes the greeting one pen stroke at a time, all selected panels
 *   advancing together, which is what the 'hello' shell command does.
 *
 *   displays is a bitmask of panels.  Only the newly written stroke is drawn
 *   each step -- the pen adds ink and never repaints -- so a step costs one
 *   short stroke plus a full-frame push per panel.  The panel takes no
 *   partial update (see gc9d01_updatearea()), so the push dominates and sets
 *   the pace; the elapsed time is reported to keep that cost visible in the
 *   boot log.
 *
 *   Reusing the command's renderer was not possible while it lived with the
 *   application: bring-up cannot start an application in this configuration
 *   (neither exec_builtin() nor posix_spawn() is built), which showed up as
 *   an undefined reference at link time.  Moving the renderer to the board is
 *   what made one implementation serve both.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_hello_animate(int displays, int steps)
{
  struct hs_style_s st;
  clock_t start = clock_systime_ticks();
  int32_t total;
  int32_t drawn = 0;
  int display;
  int step;
  int drew = 0;

  if (steps < 1)
    {
      steps = 1;
    }

  total = greeting_style(&st);
  if (total <= 0)
    {
      return -EINVAL;
    }

  for (display = 0; display < GC9D01_NDISPLAYS; display++)
    {
      if ((displays & (1 << display)) != 0 &&
          g_gc9d01_fb[display].fbmem != NULL)
        {
          hs_clear(g_gc9d01_fb[display].fbmem, GC9D01_STRIDE,
                   GC9D01_XRES, GC9D01_YRES, &st);
          (void)gc9d01_push(&g_gc9d01_fb[display]);
          drew = 1;
        }
    }

  if (drew == 0)
    {
      return -ENODEV;
    }

  for (step = 1; step <= steps; step++)
    {
      /* Same smoothstep the command uses, so the pen accelerates into the
       * stroke and settles out of it rather than moving at a constant rate.
       */

      int32_t target = (int32_t)(((int64_t)total *
                        hs_ease((step * 1000) / steps)) / 1000);
      FAR uint8_t *master = NULL;

      if (step >= steps)
        {
          target = total;
        }

      if (target <= drawn)
        {
          continue;
        }

      for (display = 0; display < GC9D01_NDISPLAYS; display++)
        {
          if ((displays & (1 << display)) == 0 ||
              g_gc9d01_fb[display].fbmem == NULL)
            {
              continue;
            }

          /* Rasterize once, then copy.  Both panels show the same word and
           * have identical geometry and stride, so stroking each of them
           * separately would pay for the same pixels twice: measured at
           * ~1.8ms per path pixel, the second pass cost about as much as a
           * full frame push.  memcpy of 51200 bytes between two kernel-RAM
           * framebuffers is far cheaper.
           */

          if (master == NULL)
            {
              hs_stroke(g_gc9d01_fb[display].fbmem, GC9D01_STRIDE,
                        GC9D01_XRES, GC9D01_YRES, GC9D01_GREETING_TEXT, &st,
                        drawn, target);
              master = g_gc9d01_fb[display].fbmem;
            }
          else
            {
              memcpy(g_gc9d01_fb[display].fbmem, master, GC9D01_FBLEN);
            }

          (void)gc9d01_push(&g_gc9d01_fb[display]);
        }

      drawn = target;
    }

  printf("gc9d01_fb: greeting written in %lu ms (%d steps, %d px path, "
         "em %d, pen %d)\n",
         (unsigned long)TICK2MSEC(clock_systime_ticks() - start), steps,
         (int)(total / 16), (int)(st.em16 / 16), (int)(st.thick16 / 16));

  return OK;
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_text / bk7258_gc9d01_fb_two_lines
 *
 * Description:
 *   Draw one word, or two lines, and push once.  These are what the
 *   event-driven status screen uses (bk7258_status_screen.c): the product
 *   spec forbids a live preview and asks for a state plus a one-or-two-line
 *   summary, refreshed only when it changes.
 *
 *   Sized to the panel rather than to the text: a 160x160 round display shows
 *   roughly six characters per line at a legible stroke width, so callers
 *   are expected to have shortened the text already.  Text the font cannot
 *   draw returns -EINVAL instead of an empty screen.
 *
 ****************************************************************************/

static int gc9d01_fb_lines(int display, FAR const char *const *lines,
                           int nlines, uint16_t fg)
{
  FAR struct gc9d01_fb_s *priv;
  struct hs_style_s st;
  int i;

  if (display < 0 || display >= GC9D01_NDISPLAYS || nlines < 1)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  if (priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  memset(&st, 0, sizeof(st));
  st.fg = fg;
  st.bg = 0x0000u;
  st.em16 = (nlines > 1 ? 26 : 44) * 16;
  st.thick16 = (nlines > 1 ? 2 : 3) * 16;
  st.cx16 = (GC9D01_XRES / 2) * 16;

  hs_clear(priv->fbmem, GC9D01_STRIDE, GC9D01_XRES, GC9D01_YRES, &st);

  for (i = 0; i < nlines; i++)
    {
      int32_t total;

      if (lines[i] == NULL || lines[i][0] == '\0')
        {
          continue;   /* An empty line just leaves the cleared background. */
        }

      /* Baselines: one line centred, two lines split around the centre. */

      st.baseline16 = nlines > 1 ?
                      ((GC9D01_YRES / 2) + (i == 0 ? -6 : 30)) * 16 :
                      ((GC9D01_YRES / 2) + 16) * 16;

      total = hs_measure(lines[i], &st, NULL);
      if (total <= 0)
        {
          return -EINVAL;
        }

      hs_stroke(priv->fbmem, GC9D01_STRIDE, GC9D01_XRES, GC9D01_YRES,
                lines[i], &st, 0, total);
    }

  return gc9d01_push(priv);
}

int bk7258_gc9d01_fb_text(int display, FAR const char *text, uint16_t colour)
{
  FAR const char *lines[1];

  lines[0] = text;
  return gc9d01_fb_lines(display, lines, 1, colour);
}

int bk7258_gc9d01_fb_two_lines(int display, FAR const char *line1,
                               FAR const char *line2)
{
  FAR const char *lines[2];

  lines[0] = line1;
  lines[1] = line2;
  return gc9d01_fb_lines(display, lines, 2, 0xffffu);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_test_pattern
 *
 * Description:
 *   Draws four quadrants (red / green / blue / white) with a black border.
 *   Colour order and geometry are both wrong-detectable by eye: a swapped
 *   byte order turns red into something else, and a stride mistake shears
 *   the quadrant boundaries.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_test_pattern(int display)
{
  FAR struct gc9d01_fb_s *priv;
  FAR uint16_t *px;
  int x;
  int y;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  px = (FAR uint16_t *)priv->fbmem;

  if (px == NULL)
    {
      return -ENODEV;
    }

  for (y = 0; y < GC9D01_YRES; y++)
    {
      for (x = 0; x < GC9D01_XRES; x++)
        {
          uint16_t c;

          if (x < 4 || y < 4 || x >= GC9D01_XRES - 4 ||
              y >= GC9D01_YRES - 4)
            {
              c = 0x0000;                                  /* black edge */
            }
          else if (y < GC9D01_YRES / 2)
            {
              c = (x < GC9D01_XRES / 2) ? 0xf800 : 0x07e0;  /* red green */
            }
          else
            {
              c = (x < GC9D01_XRES / 2) ? 0x001f : 0xffff;  /* blue white */
            }

          px[y * GC9D01_XRES + x] = c;
        }
    }

  printf("gc9d01_fb[%d]: test pattern drawn (TL red, TR green, BL blue, "
         "BR white, 4px black border)\n", display);

  return gc9d01_push(priv);
}
