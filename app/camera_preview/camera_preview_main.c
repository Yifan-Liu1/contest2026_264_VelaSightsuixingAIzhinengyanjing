/****************************************************************************
 * app/camera_preview/camera_preview_main.c
 *
 * Live camera preview: GC2145 (640x480 packed 4:2:2, V Y1 U Y0 in memory)
 * -> GC9D01 (160x160 RGB565), pushed to every registered panel.
 *
 * Why this exists instead of `nxcamera; output /dev/fb0`: nxcamera's
 * show_image() passes the camera's own width and height as both source and
 * crop rectangle and gives libyuv only the framebuffer's stride, so it
 * would write a 640x480 image into a 160x160 framebuffer and run off the
 * end of it; and for a non-I420 source on an RGB565 panel it allocates
 * width*height*3/2 (460800 bytes) from the kernel heap, which on this board
 * holds roughly 300KB.  nxcamera also lives in the shared apps repository,
 * outside this project's scope, so it is left alone.
 *
 * Conversion: a single pass that samples the source directly into the
 * framebuffer, with no intermediate buffer at all.
 *
 *   centre-crop 480x480 out of 640x480, then take every 3rd pixel
 *   (480 / 160 == 3 exactly, so nearest-neighbour needs no arithmetic
 *   beyond an integer stride) and convert that chroma/luma group to RGB565.
 *
 * Going through libyuv would have meant staging I420 at crop size
 * (480*480*3/2 = 345600 bytes) and again at panel size, i.e. exactly the
 * allocation that defeats nxcamera on this board.  Sampling directly also
 * reads only ~100KB of the 614400-byte source frame per displayed frame,
 * which matters because PSRAM bandwidth is already the tightest resource
 * here (the capture path alone runs at ~37MB/s).
 *
 * The centre crop is what keeps the picture from being stretched: the panel
 * is a 160x160 round display, so a 4:3 frame is cropped to its central
 * square first.  The corners of that square are not visible on a round
 * panel -- that is a composition matter, not something this code hides.
 *
 * Dual panel: the board drives two GC9D01 screens (/dev/fb0 and /dev/fb1).
 * Conversion runs once into the first framebuffer and the result is copied
 * to the others, since both eyes show the same image.  The copy is 51200
 * bytes of kernel RAM, far cheaper than converting twice.  Each panel is
 * then pushed independently; at ~27ms per panel per frame the bus, not the
 * conversion, is the frame-rate limit.
 *
 * Usage:
 *   camera_preview [N] [options]        live preview, N frames (0 = forever)
 *   camera_preview live+face [N] [expr=<name>] [cycle=<sec>]
 *                                       fb0 live at the camera's full rate,
 *                                       fb1 an expression, repainted only
 *                                       when it changes
 *   camera_preview fill <hex> [options] static patterns, no camera involved
 *   camera_preview pattern|bars|grid [options]
 *   camera_preview face [name] [frames] [fps=N]
 *                                       draw an expression; no name lists them
 *
 * Options:
 *   fb=0 | fb=1 | fb=both   which panel(s) to drive (default both)
 *   jpeg | jpeg=N           also encode one frame in N through the hardware
 *                           JPEG encoder (/dev/video1); default N=60, about
 *                           two seconds at the measured rate
 *   jpegout=<path>          save the first encoded frame (needs a filesystem)
 *   q=N                     JPEG quality, default 80
 *   sat=N                   chroma gain in percent (default 100)
 *   gain=N                  luma gain in percent (default 100)
 *   full                    treat Y as full-range instead of 16..235
 *   uvswap                  exchange the Cb and Cr byte positions
 *   vyuy | uyvy | yuyv      YUV byte order (default vyuy, the hardware's)
 *   swap                    pre-byte-swap pattern pixels (see below)
 *
 * Why live+face and the encoder live in this one application: /dev/video0 has
 * a single owner, so while a preview streams nothing else can open the camera.
 * The process holding it is therefore the only one that can both drive the
 * panels and produce the JPEG an upload needs.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>

#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include <nuttx/video/fb.h>
#include <sys/videoio.h>

#include "preview_face.h"
#include "preview_jpeg.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAM_DEV        "/dev/video0"

/* One JPEG every 60 displayed frames, i.e. about every two seconds at the
 * measured 29 fps -- the cadence the project plan specifies for the vision
 * request ("按固定/自适应周期（默认 2s）抓取一帧").
 */

#define JPEG_DEFAULT_EVERY  60

/* How often live+face re-reads its expression source file.  Every 15 displayed
 * frames is about twice a second at the measured rate -- far more often than a
 * verdict can arrive (recognition measured at 3.6s for 240x240, 14.4s for
 * 480x480), and the read costs one open/read/close of a one-word file.
 */

#define SRC_POLL_FRAMES     15

#define CAM_WIDTH      640
#define CAM_HEIGHT     480

/* Centre square of the 4:3 sensor frame; 480x480 keeps the full height. */

#define CROP_SIZE      480
#define CROP_X         ((CAM_WIDTH - CROP_SIZE) / 2)

#define NBUFFERS       3

/* Panels this program can drive. */

#define MAX_FB         2

/* Panel pixel count, for bounding benchmark writes. */

#define GC9D01_PIXELS  (160 * 160)

/* Report progress this often so the console does not drown in output. */

#define REPORT_FRAMES  30

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Byte order inside one 4-byte group of the capture buffer.
 *
 * The hardware order is V Y1 U Y0 -- see the comment above preview_convert()
 * for how that was measured.  The other two are the textbook orders, kept
 * selectable so a claim about byte order can be checked on the glass in one
 * flash instead of being argued about.
 */

enum preview_yuv_order_e
{
  PREVIEW_HW = 0,     /* V Y1 U Y0 -- what YUV_BUF actually writes */
  PREVIEW_UYVY,       /* U Y0 V Y1 -- textbook UYVY, for A/B */
  PREVIEW_YUYV        /* Y0 U Y1 V -- textbook YUYV, for A/B */
};

struct preview_order_s
{
  FAR const char *name;
  uint8_t off_u;      /* byte holding Cb */
  uint8_t off_v;      /* byte holding Cr */
  uint8_t off_y0;     /* byte holding the luma of the pair's even column */
  uint8_t off_y1;     /* byte holding the luma of the pair's odd column */
};

struct preview_s
{
  int camfd;

  FAR uint8_t *bufs[NBUFFERS];
  uint32_t buf_sizes[NBUFFERS];
  int nbuffers;

  int fbfd[MAX_FB];
  FAR uint8_t *fbmem[MAX_FB];
  int fbidx[MAX_FB];            /* display number behind each slot */
  int nfb;

  /* How many of those panels the live loop writes and pushes.  Normally all
   * of them; the live+face mode sets it to 1 so that panel 0 shows the camera
   * while panel 1 keeps an expression that is only repainted when it changes.
   *
   * Why it matters, measured: one panel push is 25 ms, so driving both with
   * camera frames halves the preview rate -- 28.7 fps on one panel against
   * 16.8 fps on two, with the camera delivering 29 fps either way.  Keeping
   * the second panel out of the per-frame path is what buys the full rate.
   */

  int npreview;

  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct preview_order_s g_orders[] =
{
  /*  name      U  V  Y0 Y1 */

  { "VYUY-R",   2, 0, 3, 1 },   /* hardware: V Y1 U Y0 */
  { "UYVY",     0, 2, 1, 3 },
  { "YUYV",     1, 3, 0, 2 }
};

static enum preview_yuv_order_e g_yuv_order = PREVIEW_HW;

/* Colour pipeline tuning.
 *
 * The BT.601 limited-range coefficients below are correct as such
 * (298/409/100/208/516 == 1.164/1.596/0.391/0.813/2.018 in 8-bit fixed
 * point).
 *
 * On g_sat's value -- settled by measurement, after two wrong guesses.
 *
 * An earlier revision defaulted it to 150% claiming the chroma "barely leaves
 * neutral", citing the first 32 bytes of a frame.  That was 16 pixels from
 * the top-left corner, most likely a flat wall.  `camera_preview stats` now
 * measures whole frames, and a controlled pair settles it:
 *
 *   lens at a grey scene:        mean|U-128|=1  mean|V-128|=1  coloured=0%
 *   lens at a saturated object:  mean|U-128|=11 mean|V-128|=7  coloured=23%
 *
 * The chroma responds properly, so the sensor's colour path is fine and the
 * earlier "washed out" reading was just the scene.  White balance is neutral
 * too (U dev -1, V dev +1 on the grey scene), and exposure covers 11..254 of
 * the available range.  The GC2145 saturation registers are at their normal
 * defaults as well (page 2, 0xD1/0xD2 = 0x32; the 0xD1/0xD2 = 0x00 later in
 * the table is page 1 and unrelated).
 *
 * Conclusion: there is no colour defect to correct, so the default is 100 --
 * faithful reproduction.  g_sat and the per-channel gains stay as knobs for
 * subjective preference or for a future panel/sensor batch that does need
 * correcting, but nothing in the measurements justifies a non-unity default.
 *
 * These are run-time options rather than constants because colour is
 * subjective and needs iterating on the actual glass; baking a guess into
 * the firmware would cost a flash cycle per attempt.
 */

static int g_sat = 100;         /* chroma gain, percent (100 = faithful) */
static int g_gain = 100;        /* luma gain, percent */
static int g_gamma = 100;       /* tone curve exponent, percent (100 = off) */
static bool g_full_range;       /* Y spans 0..255 rather than 16..235 */
static bool g_uv_swap;          /* exchange U and V byte positions */

/* Cyclic shift applied when writing into the framebuffer.
 *
 * Diagnosing and correcting a horizontal offset between what the driver
 * thinks is column 0 and what the panel shows at its left edge.  The symptom
 * that motivated this is a vertical seam splitting the picture into two
 * halves: the row length is right (a wrong row length would shear the image
 * diagonally instead), but each row starts at the wrong column, so the two
 * parts swap sides and the wrap point shows up as a line.
 *
 * Shifting in the application is a *diagnostic*, not the final fix: once the
 * true offset is known it belongs in the panel driver's CASET/RASET window so
 * every framebuffer user benefits, not just this program.  Kept cyclic
 * (modulo the panel width) because that exactly inverts a wrap.
 */

static int g_xoff;
static int g_yoff;

/* Mirroring, for identifying the panel's scan direction.
 *
 * The ruler pattern showed green (drawn at y=0..3) appearing at the *bottom*
 * edge and blue (y=156..159) at the *top*, with red (x=156..159) on the left
 * -- i.e. both axes mirrored -- while the yellow centre line at x=80 stayed
 * centred, which rules out a cyclic shift (a shift of 80 would have moved
 * that line to the edge).
 *
 * The real fix for a mirrored panel is MADCTL (command 0x36) in the panel
 * driver's init table: bit7 MY = row order, bit6 MX = column order, so 0xC0
 * is a 180-degree rotation.  Doing it there costs nothing at run time and
 * fixes every /dev/fbN user.  These flags exist only to establish *which*
 * value is correct before committing it, since the vendor device table ships
 * 0x36 = 0x00 and whether that is right depends on how the module vendor
 * mounted the glass.
 */

static bool g_mirror_x;
static bool g_mirror_y;

/* Per-channel gain (white balance), percent.
 *
 * Motivation: the `bars` pattern showed the first six bars (black, blue,
 * green, cyan, red, magenta) reproduced correctly while yellow leaned green
 * and white leaned cyan.  Yellow is R+G and white is R+G+B, so both leaning
 * away from red while saturated red itself still looks red is the signature
 * of a red channel that is *low in gain* rather than missing.
 *
 * GC9D01 has no per-channel gain register -- its 0xF0/0xF1/0xF2/0xF3 entries
 * are a shared gamma curve -- so the correction belongs in the colour matrix.
 * Folding the gain into the coefficients rather than applying it afterwards
 * keeps it free of extra table lookups, which matter here: a lookup costs
 * ~1.4us on this platform (see preview_bench), so three per pixel would add
 * over 100ms to a frame.  The only added cost is two extra multiplies per
 * pixel for the luma term, and arithmetic is nearly free (25600 iterations
 * measured at 3ms).
 */

static int g_rgain = 100;
static int g_ggain = 100;
static int g_bgain = 100;

/* Tone curve, applied after the colour matrix.
 *
 * gamma=100 leaves it an identity table so the default path is bit-identical
 * to having no curve at all.  Below 100 lifts the mid-tones (makes a dark
 * picture brighter without clipping highlights), above 100 deepens them.
 * A table costs one SRAM read per channel; computing a power per pixel would
 * not fit the frame budget.
 *
 * Note the panel already applies its own gamma (the 0xF0/0xF1/0xF2/0xF3
 * entries in the GC9D01 init table), so this is a correction on top of that
 * rather than the primary gamma -- which is why the default is off.
 */

static uint8_t g_tone[256];

static void preview_build_tone(void)
{
  int i;

  for (i = 0; i < 256; i++)
    {
      if (g_gamma == 100)
        {
          g_tone[i] = (uint8_t)i;
        }
      else
        {
          /* out = 255 * (i/255)^(gamma/100), evaluated in double once per
           * run rather than per pixel.
           */

          double n = (double)i / 255.0;
          double e = (double)g_gamma / 100.0;
          int v = (int)(pow(n, e) * 255.0 + 0.5);

          g_tone[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Set from the SIGINT handler; the capture loop polls it.
 *
 * Live preview has no frame limit (`camera_preview` with no count runs until
 * stopped), so Ctrl-C is the only way out, and simply being killed is not
 * good enough: the exit path has to reach preview_cleanup(), which issues
 * VIDIOC_STREAMOFF, munmaps the buffers and releases them with REQBUFS(0).
 * Skipping that leaves the sensor streaming and the PSRAM buffer heap
 * allocated -- and that heap is larger than the AP's whole kernel heap.
 *
 * Two things are needed for Ctrl-C to arrive at all, and both were missing
 * before: CONFIG_TTY_SIGINT (which was set) delivers SIGINT to the pid NSH
 * registers with TIOCSCTTY, and CONFIG_SIG_DEFAULT (which was NOT set)
 * supplies the default action.  Without the latter a process that installs
 * no handler simply ignores the signal, which is why an earlier Ctrl-C probe
 * (app/ctrlc_test) appeared to work -- it installs a handler --
 * while this loop did not.  Installing a handler here means the exit no
 * longer depends on that config at all.
 */

static volatile sig_atomic_t g_stop;
static bool g_ctty_claimed;

static void preview_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

/****************************************************************************
 * Name: preview_claim_ctty
 *
 * Description:
 *   Registers this task as the console's controlling terminal, so that a
 *   0x03 on the wire turns into a SIGINT delivered here.
 *
 *   This is not something an application would normally have to do -- NSH is
 *   supposed to do it for the foreground task (nsh_builtin.c calls
 *   TIOCSCTTY with the child pid before waitpid).  On this board it does not
 *   happen: measured with app/ctrlc_test's probe, TIOCSCTTY from inside a
 *   freshly started builtin returns 0, i.e. the terminal was still
 *   *unclaimed*.  uart_check_special() requires dev->pid > 0, so without
 *   this call no amount of Ctrl-C produces a signal, which is exactly why
 *   the preview could not be stopped.  Why NSH skips it is still open; the
 *   probe reports it in one line and is worth re-running after any NSH or
 *   serial change.
 *
 *   Claiming it also has to be undone (preview_release_ctty), because
 *   drivers/serial/serial.c refuses to move an already-set owner: a second
 *   TIOCSCTTY returns -EINVAL.  Leaving it set would point SIGINT at a dead
 *   pid and break the *next* run.
 *
 ****************************************************************************/

static void preview_claim_ctty(void)
{
  if (ioctl(STDIN_FILENO, TIOCSCTTY, (unsigned long)getpid()) == 0)
    {
      g_ctty_claimed = true;
    }
  else if (errno != EINVAL)
    {
      printf("preview: TIOCSCTTY failed: %d (Ctrl-C may not work)\n", errno);
    }
}

static void preview_release_ctty(void)
{
  if (g_ctty_claimed)
    {
      ioctl(STDIN_FILENO, TIOCNOTTY, 0);
      g_ctty_claimed = false;
    }
}

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int preview_open_fb(FAR struct preview_s *p, int want)
{
  int display;

  for (display = 0; display < MAX_FB; display++)
    {
      char path[16];
      int fd;

      if (want >= 0 && want != display)
        {
          continue;
        }

      snprintf(path, sizeof(path), "/dev/fb%d", display);

      fd = open(path, O_RDWR);
      if (fd < 0)
        {
          /* A panel that is not registered is not an error when asking for
           * "both" -- a single-panel board is a legitimate configuration.
           */

          if (want < 0)
            {
              continue;
            }

          printf("preview: open %s failed: %d\n", path, errno);
          return -errno;
        }

      if (p->nfb == 0)
        {
          if (ioctl(fd, FBIOGET_VIDEOINFO, (uintptr_t)&p->vinfo) < 0 ||
              ioctl(fd, FBIOGET_PLANEINFO, (uintptr_t)&p->pinfo) < 0)
            {
              printf("preview: fb%d info ioctl failed: %d\n", display,
                     errno);
              close(fd);
              return -errno;
            }

          if (p->vinfo.fmt != FB_FMT_RGB16_565)
            {
              printf("preview: only RGB565 panels are supported (fmt=%u)\n",
                     p->vinfo.fmt);
              close(fd);
              return -ENOTSUP;
            }
        }

      p->fbmem[p->nfb] = mmap(NULL, p->pinfo.fblen,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FILE, fd, 0);
      if (p->fbmem[p->nfb] == MAP_FAILED)
        {
          printf("preview: fb%d mmap failed: %d\n", display, errno);
          close(fd);
          return -errno;
        }

      p->fbfd[p->nfb] = fd;
      p->fbidx[p->nfb] = display;
      p->nfb++;
    }

  if (p->nfb == 0)
    {
      printf("preview: no framebuffer could be opened\n");
      return -ENODEV;
    }

  /* Default: the live loop owns every panel it opened.  live+face narrows it
   * afterwards.
   */

  p->npreview = p->nfb;

  printf("preview: %d panel(s), %ux%u fmt=%u bpp=%u stride=%u fblen=%u\n",
         p->nfb, p->vinfo.xres, p->vinfo.yres, p->vinfo.fmt,
         p->pinfo.bpp, (unsigned int)p->pinfo.stride,
         (unsigned int)p->pinfo.fblen);

  return OK;
}

static int preview_open_camera(FAR struct preview_s *p)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_format fmt;
  int i;

  p->camfd = open(CAM_DEV, O_RDWR);
  if (p->camfd < 0)
    {
      printf("preview: open %s failed: %d\n", CAM_DEV, errno);
      return -errno;
    }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = CAM_WIDTH;
  fmt.fmt.pix.height      = CAM_HEIGHT;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;

  /* The real memory layout is V Y1 U Y0 (see preview_convert()), which no
   * V4L2 fourcc names: VYUY gets the chroma right but not the reversed luma
   * pair, and the imgsensor/imgdata layers this driver sits on define only
   * UYVY and YUYV anyway.  UYVY is therefore what both sides agree to call
   * it -- a label for "packed 4:2:2, chroma first", with the exact order
   * documented in the driver and decoded by preview_convert().  Agreeing
   * with the driver is necessary (VIDIOC_S_FMT fails otherwise) but not
   * sufficient: both sides can be wrong in the same direction without any
   * layer complaining, which is how the chroma swap survived this long.
   */

  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;

  if (ioctl(p->camfd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0)
    {
      printf("preview: VIDIOC_S_FMT failed: %d\n", errno);
      return -errno;
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = NBUFFERS;

  if (ioctl(p->camfd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      printf("preview: VIDIOC_REQBUFS failed: %d\n", errno);
      return -errno;
    }

  p->nbuffers = req.count < NBUFFERS ? req.count : NBUFFERS;

  for (i = 0; i < p->nbuffers; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(p->camfd, VIDIOC_QUERYBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_QUERYBUF(%d) failed: %d\n", i, errno);
          return -errno;
        }

      p->bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, p->camfd, buf.m.offset);
      if (p->bufs[i] == MAP_FAILED)
        {
          printf("preview: buffer %d mmap failed: %d\n", i, errno);
          return -errno;
        }

      p->buf_sizes[i] = buf.length;

      if (ioctl(p->camfd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_QBUF(%d) failed: %d\n", i, errno);
          return -errno;
        }
    }

  printf("preview: camera %dx%d 4:2:2 (V4L2 tag UYVY, memory V Y1 U Y0), "
         "%d buffers of %u bytes\n",
         CAM_WIDTH, CAM_HEIGHT, p->nbuffers,
         (unsigned int)p->buf_sizes[0]);

  return OK;
}

/****************************************************************************
 * Name: preview_draw_pattern
 *
 * Description:
 *   Static self-test patterns, drawn straight into the framebuffer and
 *   pushed once.  These exist to split "the panel shows nothing
 *   recognisable" into independent questions without a re-flash per
 *   hypothesis:
 *
 *     fill <hex>  uniform colour.  If this is uniform on the glass, the
 *                 transport is sound and any remaining fault is format or
 *                 geometry.  If it is uniform but the WRONG colour, it is
 *                 byte order.  If it is noise, the transport itself drops
 *                 or reorders bytes.
 *     pattern     four quadrants + black border.  Colour error, stride
 *                 error and window error each look different here, so one
 *                 look tells you which of the three it is.
 *     bars        8 vertical colour bars.  A single dropped byte shears
 *                 every bar below the drop, which a uniform fill hides.
 *     grid        1-pixel checkerboard, the worst case for the bus: any
 *                 dropped or duplicated word turns the regular lattice
 *                 into visible diagonal banding.
 *
 *   Appending "swap" writes each pixel pre-byte-swapped, which cancels the
 *   driver's own swap.  That makes the wire byte order testable from user
 *   space: exactly one of `fill f800` and `fill f800 swap` shows red.
 *
 ****************************************************************************/

static uint16_t preview_pat_bar(int i)
{
  static const uint16_t bars[8] =
  {
    0x0000, 0x001f, 0x07e0, 0x07ff, 0xf800, 0xf81f, 0xffe0, 0xffff
  };

  return bars[i & 7];
}

static int preview_draw_pattern(FAR struct preview_s *p, int argc,
                                FAR char *argv[])
{
  FAR const char *what = argv[1];
  int dw = p->vinfo.xres;
  int dh = p->vinfo.yres;
  bool swap = false;
  uint16_t colour = 0xf800;
  int ox = ((g_xoff % dw) + dw) % dw;
  int oy = ((g_yoff % dh) + dh) % dh;

  int x;
  int y;
  int i;

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "swap") == 0)
        {
          swap = true;
        }
      else if (strncmp(argv[i], "fb=", 3) != 0 &&
               strncmp(argv[i], "sat=", 4) != 0 &&
               strncmp(argv[i], "gain=", 5) != 0 &&
               strncmp(argv[i], "gamma=", 6) != 0 &&
               strncmp(argv[i], "xoff=", 5) != 0 &&
               strncmp(argv[i], "yoff=", 5) != 0 &&
               strcmp(argv[i], "mx") != 0 &&
               strcmp(argv[i], "my") != 0 &&
               strcmp(argv[i], "rot180") != 0 &&
               strncmp(argv[i], "rgain=", 6) != 0 &&
               strncmp(argv[i], "ggain=", 6) != 0 &&
               strncmp(argv[i], "bgain=", 6) != 0)
        {
          colour = (uint16_t)strtoul(argv[i], NULL, 16);
        }
    }

  for (y = 0; y < dh; y++)
    {
      int ty = y + oy;
      FAR uint16_t *drow;

      if (ty >= dh)
        {
          ty -= dh;
        }

      if (g_mirror_y)
        {
          ty = dh - 1 - ty;
        }

      drow = (FAR uint16_t *)(p->fbmem[0] + (size_t)ty * p->pinfo.stride);

      for (x = 0; x < dw; x++)
        {
          int tx = x + ox;
          uint16_t c;

          if (tx >= dw)
            {
              tx -= dw;
            }

          if (g_mirror_x)
            {
              tx = dw - 1 - tx;
            }

          if (strcmp(what, "fill") == 0)
            {
              c = colour;
            }
          else if (strcmp(what, "bars") == 0)
            {
              c = preview_pat_bar(x * 8 / dw);
            }
          else if (strcmp(what, "grid") == 0)
            {
              c = ((x ^ y) & 1) ? 0xffff : 0x0000;
            }
          else if (strcmp(what, "ruler") == 0)
            {
              /* Edge markers that make a shift readable by eye.  Each edge
               * gets its own colour, so wherever the white band appears is
               * where the driver's column 0 actually lands on the glass, and
               * the yellow centre line says whether the shift is half a
               * screen (the "seam down the middle" case) or something else.
               *
               *   white  x = 0..3      left edge
               *   red    x = 156..159  right edge
               *   green  y = 0..3      top edge
               *   blue   y = 156..159  bottom edge
               *   yellow x = 80        centre
               */

              if (x < 4)
                {
                  c = 0xffff;                    /* white  */
                }
              else if (x >= dw - 4)
                {
                  c = 0xf800;                    /* red    */
                }
              else if (y < 4)
                {
                  c = 0x07e0;                    /* green  */
                }
              else if (y >= dh - 4)
                {
                  c = 0x001f;                    /* blue   */
                }
              else if (x == dw / 2)
                {
                  c = 0xffe0;                    /* yellow */
                }
              else
                {
                  c = 0x0000;
                }
            }
          else if (strcmp(what, "wb") == 0)
            {
              /* White-balance ladder: five horizontal bands, each showing
               * white (left half) and yellow (right half) with G and B
               * progressively attenuated.  Pick the band that looks most
               * neutral / most yellow and use its percentage as ggain and
               * bgain for the live path.
               *
               * Attenuating G and B rather than boosting R is deliberate:
               * white is 0xFFFF, so R is already at its maximum 31 and
               * cannot be raised.  A multiplicative correction can only
               * bring the other channels down.
               *
               * Bands top to bottom: 100%, 92%, 84%, 76%, 68%.
               */

              static const int gb[5] =
              {
                100, 92, 84, 76, 68
              };

              int band = y * 5 / dh;
              int k = gb[band > 4 ? 4 : band];
              int rr = 31;
              int gg = 63 * k / 100;
              int bb = (x < dw / 2) ? (31 * k / 100) : 0;

              c = (uint16_t)((rr << 11) | (gg << 5) | bb);
            }
          else /* "pattern" */
            {
              if (x < 4 || y < 4 || x >= dw - 4 || y >= dh - 4)
                {
                  c = 0x0000;
                }
              else if (y < dh / 2)
                {
                  c = (x < dw / 2) ? 0xf800 : 0x07e0;
                }
              else
                {
                  c = (x < dw / 2) ? 0x001f : 0xffff;
                }
            }

          drow[tx] = swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
        }
    }

  for (i = 1; i < p->nfb; i++)
    {
      memcpy(p->fbmem[i], p->fbmem[0], p->pinfo.fblen);
    }

  printf("preview: drew '%s'%s on %dx%d, %d panel(s), xoff=%d yoff=%d "
         "mirror=%s%s\n", what, swap ? " (pre-swapped)" : "", dw, dh,
         p->nfb, ox, oy, g_mirror_x ? "X" : "-", g_mirror_y ? "Y" : "-");

  return OK;
}

/****************************************************************************
 * Name: preview_convert
 *
 * Description:
 *   One frame: centre-crop, 3:1 nearest-neighbour downscale and packed
 *   4:2:2 YUV to RGB565, straight from the capture buffer into the first
 *   framebuffer.  BT.601 coefficients in 8-bit fixed point; the shifts are
 *   chosen so the whole conversion stays in 32-bit integer math.
 *
 *   Byte order is V Y1 U Y0: each 4-byte group carries two pixels, with the
 *   chroma pair shared between them, but *reversed* -- byte 0 is Cr, byte 2
 *   is Cb, and byte 3 is the luma of the left (even) column while byte 1 is
 *   the luma of the right (odd) one.  A source x therefore maps to group
 *   (x >> 1) and picks byte 3 or byte 1 by x's parity.
 *
 *   How that was settled.  Three claims have to be separated, and each has
 *   its own evidence:
 *
 *   (1) Which bytes are luma.  Bytes 1 and 3.  Decoding with luma at bytes
 *       0/2 was photographed on the panel and gave a geometrically correct,
 *       clearly recognisable picture in which every pixel was either
 *       saturated green or magenta.  That is the arithmetic signature of
 *       feeding chroma into the luma term: with y_code == (U-16)*298 ≈ 33376
 *       constant and u_code == v_code == Y-128 == t,
 *
 *           r = (33376 + 409t) >> 8 = 130 + 1.6t
 *           g = (33376 - 308t) >> 8 = 130 - 1.2t
 *           b = (33376 + 516t) >> 8 = 130 + 2.0t
 *
 *       so t < 0 (dark) drives green and t > 0 (bright) drives magenta while
 *       luma structure survives, because t still carries it.  A captured
 *       frame agrees: over /tmp/frame.yuv, bytes 1 and 3 have sd 39 and span
 *       18..153, bytes 0 and 2 have sd 6.2 and 2.3 around 128.
 *
 *   (2) In which order the two luma samples sit.  Reversed: byte 3 is the
 *       left pixel.  Natural images are equally smooth across every pixel
 *       boundary, so mean |dY| between the two samples *inside* a group must
 *       match mean |dY| *across* the group boundary.  On the captured frame
 *       it does not: 1.752 inside versus 3.787 across, a factor of 2.16.
 *       Un-swapping the two luma bytes makes it 1.752 versus 1.760 -- and
 *       1.76 is also what the same measurement gives vertically (1.621),
 *       where no byte ordering is involved.  The asymmetry is the fingerprint
 *       of a pair read out backwards: the "across" difference then spans
 *       three columns instead of one.
 *
 *   (3) Which chroma byte is Cb.  Byte 2.  This follows from (2) once the
 *       sensor's bus order is known.  The GC2145 is configured with register
 *       0x84 = 0x02 (dvp_gc2145.c, commented "yuyv"), which the part's
 *       output-format table defines as Y Cb Y Cr on the wire -- corroborated
 *       by Zephyr's driver, where VIDEO_PIX_FMT_YUYV maps to
 *       GC2145_REG_OUTPUT_FMT_YCBYCR = 0x02.  A group in memory whose luma
 *       samples are reversed *and* whose chroma sits at bytes 0/2 is exactly
 *       the four bus bytes stored backwards: Y0 Cb Y1 Cr -> Cr Y1 Cb Y0.
 *       So byte 0 is Cr and byte 2 is Cb.  Scene evidence agrees: on
 *       frame.yuv (red text on a white sign) the extreme chroma deviations
 *       are byte0 +57 and byte2 -18, a ratio of -3.17, against -2.96
 *       predicted for pure red read as (Cr, Cb) -- and a poor match for blue
 *       read the other way round (-6.2 predicted).
 *
 *   What this fixed.  The decode used to assume plain UYVY (Cb at byte 0),
 *   which swaps Cb and Cr and so rotates every hue about the red/blue axis:
 *   structure, brightness and grey balance all stay right, which is why the
 *   symptom was only "colours a bit off" rather than an obviously broken
 *   image.  Reds came out blue.  The luma pair order was wrong at the same
 *   time, but that only misplaces detail by one source column and is
 *   invisible after the 3:1 downscale.
 *
 *   Note the layout is not any V4L2 fourcc: VYUY would describe the chroma
 *   correctly but not the reversed luma pair.  The driver still advertises
 *   V4L2_PIX_FMT_UYVY because that is the closest order the imgsensor and
 *   imgdata layers define; see bk7258_camera_imgsensor.c.
 *
 *   Saturation: with the chroma decoded correctly `camera_preview stats`
 *   measures a normal chroma swing (mean |U-128| = 11 on a saturated scene,
 *   1 on a grey one), so no correction is applied by default.  g_sat,
 *   g_gain, g_full_range and g_uv_swap remain as knobs because colour is
 *   subjective and needs the real panel in the loop.
 *
 ****************************************************************************/

/* One source row's worth of the cropped region, staged in SRAM.
 *
 * Why this exists: the capture buffer is in PSRAM, mapped non-cacheable, and
 * a single CPU read of it costs on the order of microseconds.  Measured:
 * converting one 160x160 frame with per-pixel reads straight out of PSRAM
 * took 194ms, i.e. ~7.6us per output pixel -- about 2400 CPU cycles at
 * 320MHz for what is a load plus a dozen arithmetic instructions.  Reducing
 * the number of reads per pixel from three to one changed nothing (180ms ->
 * 194ms, inside the noise), which is what ruled out access *count* and
 * pointed at per-access *latency*.
 *
 * A bulk memcpy, by contrast, is sequential and lets the PSRAM controller
 * burst.  So each needed row is copied once and then read repeatedly out of
 * SRAM.
 *
 * Only the cropped span is staged: the centre 480 of 640 columns is quad
 * indices CROP_X/2 .. (CROP_X+CROP_SIZE)/2, i.e. 240 quads = 960 bytes,
 * instead of the full 1280-byte row.  Static rather than on the stack
 * because this app's STACKSIZE is 4096.
 */

#define ROW_QUADS   (CROP_SIZE / 2)            /* 240 */
#define ROW_BYTES   (ROW_QUADS * 4)            /* 960 */
#define ROW_Q0      (CROP_X / 2)               /* first quad index staged */

static uint32_t g_rowbuf[ROW_QUADS];

/****************************************************************************
 * Name: preview_bench
 *
 * Description:
 *   Micro-benchmarks of the accesses the conversion loop actually makes.
 *
 *   This exists because two rounds of "obvious" optimisation both failed:
 *   collapsing three per-pixel PSRAM byte reads into one 32-bit read changed
 *   convert time from 180ms to 194ms (nothing), and staging each row in SRAM
 *   with memcpy made it 456ms (worse).  Both were reasoning from a guess
 *   about where the time went.  The numbers below separate the candidates so
 *   the next change can be aimed rather than tried.
 *
 *   What each line isolates:
 *     arith      the arithmetic alone, no frame memory touched at all
 *     sram rnd   same access pattern as the real loop, but out of SRAM
 *     psram seq  consecutive 32-bit reads from the capture buffer
 *     psram rnd  the real loop's stride (one quad per 1.5 quads)
 *     memcpy     bulk sequential PSRAM -> SRAM, the failed optimisation
 *
 *   Interpretation: if `psram rnd` dominates and `sram rnd` is small, the
 *   cost is per-access PSRAM latency, and the fix has to reduce accesses or
 *   make them cacheable -- not reorder them.
 *
 ****************************************************************************/

#define BENCH_N  25600      /* one frame's worth of output pixels */

static void preview_bench(FAR struct preview_s *p)
{
  FAR const volatile uint32_t *ps;
  uint32_t t;
  uint32_t acc = 0;
  int i;

  if (p->camfd < 0 || p->bufs[0] == NULL)
    {
      printf("bench: no capture buffer\n");
      return;
    }

  ps = (FAR const volatile uint32_t *)p->bufs[0];

  t = now_ms();
  for (i = 0; i < BENCH_N; i++)
    {
      acc += (uint32_t)(i * 409 + 128) >> 8;
    }

  printf("bench: arith     %u iters in %ums\n", BENCH_N,
         (unsigned int)(now_ms() - t));

  t = now_ms();
  for (i = 0; i < BENCH_N; i++)
    {
      acc += g_rowbuf[i % ROW_QUADS];
    }

  printf("bench: sram rnd  %u reads in %ums\n", BENCH_N,
         (unsigned int)(now_ms() - t));

  t = now_ms();
  for (i = 0; i < BENCH_N; i++)
    {
      acc += ps[i];
    }

  printf("bench: psram seq %u reads in %ums\n", BENCH_N,
         (unsigned int)(now_ms() - t));

  t = now_ms();
  for (i = 0; i < BENCH_N; i++)
    {
      /* Same stride the converter uses: 3 source pixels per output pixel,
       * two source pixels per quad.
       */

      acc += ps[(i * 3) >> 1];
    }

  printf("bench: psram rnd %u reads in %ums\n", BENCH_N,
         (unsigned int)(now_ms() - t));

  t = now_ms();
  for (i = 0; i < 160; i++)
    {
      memcpy(g_rowbuf, (FAR const void *)(p->bufs[0] + i * CAM_WIDTH * 2),
             ROW_BYTES);
    }

  printf("bench: memcpy    160 x %u bytes in %ums\n", ROW_BYTES,
         (unsigned int)(now_ms() - t));

  /* The converter's exact address sequence: 160 sampled rows, each 3 source
   * rows apart, 160 quads per row.  The plain "psram rnd" case above stays
   * inside the first 153600 bytes, while this one walks all 614400 -- if
   * PSRAM has row/page miss penalties, only this case will show them, and
   * that is the difference that the flat 14ms of read+arith+write fails to
   * explain against a measured 194ms convert.
   */

  t = now_ms();
  for (i = 0; i < 160; i++)
    {
      FAR const volatile uint32_t *row =
        (FAR const volatile uint32_t *)(p->bufs[0] +
                                        (size_t)(i * 3) * CAM_WIDTH * 2);
      int k;

      for (k = 0; k < 160; k++)
        {
          acc += row[(CROP_X + k * 3) >> 1];
        }
    }

  printf("bench: psram rows 160x160 strided reads in %ums\n",
         (unsigned int)(now_ms() - t));

  /* Framebuffer is kernel SRAM; included to confirm the write side is not a
   * factor.
   */

  t = now_ms();
  for (i = 0; i < BENCH_N; i++)
    {
      ((FAR uint16_t *)p->fbmem[0])[i % (GC9D01_PIXELS)] = (uint16_t)i;
    }

  printf("bench: fb write  %u writes in %ums (acc=%u)\n", BENCH_N,
         (unsigned int)(now_ms() - t), (unsigned int)acc);
}

/****************************************************************************
 * Name: preview_stats
 *
 * Description:
 *   Objective statistics of one captured frame.
 *
 *   Why this exists: judging the *panel* needs a human eye or a colorimeter,
 *   but judging the *source* does not.  Colour flows camera -> YUV -> RGB ->
 *   panel, and everything except the last hop is measurable from software.
 *   Earlier rounds asked for photographs to diagnose things that were
 *   actually decidable here.
 *
 *   How to read the output:
 *
 *     U/V mean    A neutral grey or white subject should give both near 128.
 *                 A consistent offset is the camera's white balance, and the
 *                 offset directly yields the per-channel gain needed:
 *                 V > 128 means the source leans red, U > 128 leans blue.
 *     Y min/max   The usable dynamic range.  A washed-out ("greyish")
 *                 picture with correct decoding usually means Y occupies far
 *                 less than 16..235, which no amount of chroma gain fixes --
 *                 that needs exposure or a contrast stretch.
 *     Y histogram Where the tones actually sit; a single tall bucket means a
 *                 flat, low-contrast frame.
 *
 *   Sampling every 4th quad keeps this to ~77k PSRAM reads (~18ms at the
 *   measured 0.23us each) while still covering the whole frame.
 *
 ****************************************************************************/

static void preview_stats(FAR struct preview_s *p, int index)
{
  FAR const uint32_t *q = (FAR const uint32_t *)p->bufs[index];
  FAR const struct preview_order_s *ord = &g_orders[g_yuv_order];
  uint32_t nquads = (CAM_WIDTH * CAM_HEIGHT) / 2;
  uint32_t hist[16];
  uint32_t ysum = 0;
  uint32_t usum = 0;
  uint32_t vsum = 0;
  uint32_t uabs = 0;      /* sum |U-128|, quantifies saturation */
  uint32_t vabs = 0;
  uint32_t csat = 0;      /* samples with |U-128|>16 or |V-128|>16 */
  int ymin = 255;
  int ymax = 0;
  int umin = 255;
  int umax = 0;
  int vmin = 255;
  int vmax = 0;
  uint32_t n = 0;
  uint32_t i;

  memset(hist, 0, sizeof(hist));

  for (i = 0; i < nquads; i += 4)
    {
      uint32_t quad = q[i];

      /* Same byte offsets the decoder uses (default V Y1 U Y0), so the
       * numbers below label the same channels the picture is built from.
       */

      int u  = (int)((quad >> (ord->off_u * 8)) & 0xff);
      int y0 = (int)((quad >> (ord->off_y0 * 8)) & 0xff);
      int v  = (int)((quad >> (ord->off_v * 8)) & 0xff);
      int y1 = (int)((quad >> (ord->off_y1 * 8)) & 0xff);
      int ya = (y0 + y1) / 2;

      usum += (uint32_t)u;
      vsum += (uint32_t)v;
      ysum += (uint32_t)ya;

      uabs += (uint32_t)(u > 128 ? u - 128 : 128 - u);
      vabs += (uint32_t)(v > 128 ? v - 128 : 128 - v);

      if ((u > 144 || u < 112) || (v > 144 || v < 112))
        {
          csat++;
        }

      if (u < umin)
        {
          umin = u;
        }

      if (u > umax)
        {
          umax = u;
        }

      if (v < vmin)
        {
          vmin = v;
        }

      if (v > vmax)
        {
          vmax = v;
        }

      if (ya < ymin)
        {
          ymin = ya;
        }

      if (ya > ymax)
        {
          ymax = ya;
        }

      hist[ya >> 4]++;
      n++;
    }

  if (n == 0)
    {
      printf("stats: no samples\n");
      return;
    }

  printf("stats: samples=%u  Y mean=%u min=%d max=%d span=%d\n",
         (unsigned int)n, (unsigned int)(ysum / n), ymin, ymax,
         ymax - ymin);
  printf("stats: U mean=%u (dev %+d) range=%d..%d   "
         "V mean=%u (dev %+d) range=%d..%d\n",
         (unsigned int)(usum / n), (int)(usum / n) - 128, umin, umax,
         (unsigned int)(vsum / n), (int)(vsum / n) - 128, vmin, vmax);

  /* Saturation, measured rather than assumed.  mean|U-128| / mean|V-128| is
   * how far the chroma actually swings from neutral; a colourful scene gives
   * something like 15..30, a nearly monochrome one only a few.  The last
   * figure is the share of samples that are meaningfully coloured at all.
   */

  printf("stats: chroma swing mean|U-128|=%u mean|V-128|=%u, "
         "coloured samples=%u%%\n",
         (unsigned int)(uabs / n), (unsigned int)(vabs / n),
         (unsigned int)(csat * 100 / n));

  printf("stats: Y hist");
  for (i = 0; i < 16; i++)
    {
      printf(" %u", (unsigned int)(hist[i] * 100 / n));
    }

  printf("  (%% per 16-level bucket)\n");
}

static int preview_convert(FAR struct preview_s *p, int index)
{
  FAR const uint8_t *src = p->bufs[index];
  int dw = p->vinfo.xres;
  int dh = p->vinfo.yres;
  int step = CROP_SIZE / dw;          /* 480 / 160 = 3 */
  int ycoef = g_full_range ? 256 : 298;
  int yoff  = g_full_range ? 0 : 16;
  int dx;
  int dy;

  /* Byte offsets within the 4-byte group, by decode order. */

  FAR const struct preview_order_s *ord = &g_orders[g_yuv_order];
  int off_u  = ord->off_u;
  int off_y0 = ord->off_y0;
  int off_v  = ord->off_v;
  int off_y1 = ord->off_y1;

  /* Shift amounts for extracting those bytes from one little-endian 32-bit
   * word.  Hoisted because the decode order cannot change within a frame.
   */

  int sh_u  = off_u * 8;
  int sh_v  = off_v * 8;
  int sh_y0 = off_y0 * 8;
  int sh_y1 = off_y1 * 8;

  /* Chroma gain folded into the BT.601 coefficients: g_sat is constant for
   * the frame, so this removes two integer divisions per pixel.
   */

  int c_rv;
  int c_gu;
  int c_gv;
  int c_bu;
  int yc_r;
  int yc_g;
  int yc_b;

  /* Cyclic shift, normalised once per frame.  Applied with a compare and a
   * subtract rather than a modulo: `%` costs ~1.3us per use on this platform
   * (see preview_bench), which per pixel would be 33ms a frame.
   */

  int ox = ((g_xoff % dw) + dw) % dw;
  int oy = ((g_yoff % dh) + dh) % dh;

  /* Any optional transform active?  Decided once per frame so the inner
   * loop never tests it.
   */

  bool xform = (ox != 0 || oy != 0 || g_mirror_x || g_mirror_y ||
                g_gamma != 100 || g_rgain != 100 || g_ggain != 100 ||
                g_bgain != 100);

  if (g_uv_swap)
    {
      int t = sh_u;

      sh_u = sh_v;
      sh_v = t;
    }

  ycoef = ycoef * g_gain / 100;

  /* Chroma gain (g_sat) and per-channel gain both folded in here, so the
   * inner loop has no divisions and no lookups.  The luma term needs one
   * coefficient per channel because each channel is scaled differently.
   */

  yc_r = ycoef * g_rgain / 100;
  yc_g = ycoef * g_ggain / 100;
  yc_b = ycoef * g_bgain / 100;

  c_rv = 409 * g_sat / 100 * g_rgain / 100;
  c_gu = 100 * g_sat / 100 * g_ggain / 100;
  c_gv = 208 * g_sat / 100 * g_ggain / 100;
  c_bu = 516 * g_sat / 100 * g_bgain / 100;

  if (step < 1 || dh * step > CAM_HEIGHT)
    {
      printf("preview: %dx%d panel does not divide the %dx%d crop\n",
             dw, dh, CROP_SIZE, CROP_SIZE);
      return -EINVAL;
    }

  for (dy = 0; dy < dh; dy++)
    {
      FAR const uint8_t *srow = src + (size_t)(dy * step) * CAM_WIDTH * 2;
      int ty = dy + oy;
      FAR uint16_t *drow;

      if (ty >= dh)
        {
          ty -= dh;
        }

      if (g_mirror_y)
        {
          ty = dh - 1 - ty;
        }

      drow = (FAR uint16_t *)(p->fbmem[0] + (size_t)ty * p->pinfo.stride);

      /* Second panel written in the same pass rather than copied afterwards.
       * Measured cost of each option per frame (see the access-cost table in
       * preview_bench()):
       *
       *     bulk memcpy of 51200 bytes   75 ms
       *     one extra 16-bit store/pixel 33 ms
       *
       * so the in-loop store wins by 42ms.  An earlier revision concluded the
       * opposite, having attributed a regression to this change when it was
       * actually caused by the tone table added in the same commit.  One
       * variable per experiment.
       */

      FAR uint16_t *drow1 = (p->npreview > 1) ?
        (FAR uint16_t *)(p->fbmem[1] + (size_t)ty * p->pinfo.stride) : NULL;

      /* Read the source quads straight out of PSRAM.  Staging the row in
       * SRAM with memcpy first was tried and was slower: strided PSRAM reads
       * cost 0.23us each while memcpy runs at ~1.46us per byte.
       *
       * Alignment holds: CAM_WIDTH*2 == 1280 is a multiple of 4, the quad
       * offset is (sx >> 1) * 4, and the capture buffer is PSRAM-pool
       * allocated and word aligned.
       */

      FAR const uint32_t *squad = (FAR const uint32_t *)srow;

      /* Two loop bodies: a fast path with no optional transform at all, and
       * a general one that applies every knob.
       *
       * Why split rather than always run the general form: each knob costs
       * real time in a loop that runs 25600 times a frame, and measurements
       * showed the accumulated cost of features that turned out to be
       * unnecessary:
       *
       *   plain conversion                      126 ms
       *   + per-channel gain (3 luma multiplies) 19 ms
       *   + shift/mirror (add, compare, branch)  16 ms
       *                                         ------
       *                                         161 ms
       *
       * All three knobs were added to diagnose suspected defects, and
       * `camera_preview stats` then showed there is nothing to correct:
       * white balance is neutral, the chroma responds properly, and the
       * ruler/bars patterns showed no shift and no mirroring.  So the
       * default has to be free of them, while the knobs stay available for
       * subjective preference or a future panel/sensor batch.
       */

      if (!xform)
        {
          for (dx = 0; dx < dw; dx++)
            {
              int sx = CROP_X + dx * step;
              uint32_t quad = squad[sx >> 1];
              int u = (int)((quad >> sh_u) & 0xff) - 128;
              int v = (int)((quad >> sh_v) & 0xff) - 128;
              int y = (int)((quad >> ((sx & 1) ? sh_y1 : sh_y0)) & 0xff) -
                      yoff;
              int r;
              int g;
              int b;
              uint16_t px;

              if (y < 0)
                {
                  y = 0;
                }

              y *= ycoef;

              r = (y + c_rv * v + 128) >> 8;
              g = (y - c_gu * u - c_gv * v + 128) >> 8;
              b = (y + c_bu * u + 128) >> 8;

              r = r < 0 ? 0 : (r > 255 ? 255 : r);
              g = g < 0 ? 0 : (g > 255 ? 255 : g);
              b = b < 0 ? 0 : (b > 255 ? 255 : b);

              px = (uint16_t)(((r & 0xf8) << 8) |
                              ((g & 0xfc) << 3) |
                               (b >> 3));

              drow[dx] = px;

              if (drow1 != NULL)
                {
                  drow1[dx] = px;
                }
            }
        }
      else
        {
          for (dx = 0; dx < dw; dx++)
            {
              int sx = CROP_X + dx * step;
              uint32_t quad = squad[sx >> 1];
              int u = (int)((quad >> sh_u) & 0xff) - 128;
              int v = (int)((quad >> sh_v) & 0xff) - 128;
              int y = (int)((quad >> ((sx & 1) ? sh_y1 : sh_y0)) & 0xff) -
                      yoff;
              int r;
              int g;
              int b;
              int tx;
              uint16_t px;

              if (y < 0)
                {
                  y = 0;
                }

              r = (y * yc_r + c_rv * v + 128) >> 8;
              g = (y * yc_g - c_gu * u - c_gv * v + 128) >> 8;
              b = (y * yc_b + c_bu * u + 128) >> 8;

              r = r < 0 ? 0 : (r > 255 ? 255 : r);
              g = g < 0 ? 0 : (g > 255 ? 255 : g);
              b = b < 0 ? 0 : (b > 255 ? 255 : b);

              if (g_gamma != 100)
                {
                  r = g_tone[r];
                  g = g_tone[g];
                  b = g_tone[b];
                }

              px = (uint16_t)(((r & 0xf8) << 8) |
                              ((g & 0xfc) << 3) |
                               (b >> 3));

              tx = dx + ox;
              if (tx >= dw)
                {
                  tx -= dw;
                }

              if (g_mirror_x)
                {
                  tx = dw - 1 - tx;
                }

              drow[tx] = px;

              if (drow1 != NULL)
                {
                  drow1[tx] = px;
                }
            }
        }
    }

  return OK;
}

/****************************************************************************
 * Name: preview_paint_face_on
 *
 * Description:
 *   Render expression idx onto one panel and push it.  Used by the live+face
 *   mode for the panel the camera does not own, so it is repainted only when
 *   the expression changes rather than once per frame -- which is the whole
 *   reason that mode can hold the camera's full rate.
 *
 ****************************************************************************/

static int preview_paint_face_on(FAR struct preview_s *p, int slot, int idx,
                                 uint32_t phase_ms)
{
  struct fb_area_s area;

  if (slot < 0 || slot >= p->nfb)
    {
      return -EINVAL;
    }

  preview_face_render(p->fbmem[slot], p->pinfo.stride,
                      (int)p->vinfo.xres, (int)p->vinfo.yres,
                      idx, phase_ms);

  area.x = 0;
  area.y = 0;
  area.w = p->vinfo.xres;
  area.h = p->vinfo.yres;

  if (ioctl(p->fbfd[slot], FBIO_UPDATE, (uintptr_t)&area) < 0)
    {
      printf("preview: fb%d FBIO_UPDATE failed: %d\n", p->fbidx[slot],
             errno);
      return -errno;
    }

  return OK;
}

static int preview_push_all(FAR struct preview_s *p,
                            FAR struct fb_area_s *area)
{
  int ret = OK;
  int i;

  for (i = 0; i < p->npreview; i++)
    {
      if (ioctl(p->fbfd[i], FBIO_UPDATE, (uintptr_t)area) < 0)
        {
          printf("preview: fb%d FBIO_UPDATE failed: %d\n", p->fbidx[i],
                 errno);
          ret = -errno;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: preview_run_face
 *
 * Description:
 *   Draws an expression from preview_face.c.  Like the pattern modes this
 *   never opens the camera, so it works when the capture path is broken or
 *   absent.
 *
 *   Both panels show the same face, so the frame is rendered once into
 *   fbmem[0] and memcpy'd to the others: a 51200-byte copy costs far less
 *   than a second pass of the renderer.  The frame rate is then set by the
 *   panel bus (~27-32ms per panel per frame), which is why the default is
 *   10fps rather than something the renderer could sustain.
 *
 ****************************************************************************/

static int preview_run_face(FAR struct preview_s *p, int argc,
                            FAR char *argv[], int want_fb)
{
  struct fb_area_s area;
  FAR const char *name = NULL;
  uint32_t max_frames = 0;
  uint32_t fps = 5;
  uint32_t period;
  uint32_t frames = 0;
  uint32_t render_ms = 0;
  uint32_t push_ms = 0;
  uint32_t start;
  uint32_t dur;
  int idx;
  int i;
  int ret;

  /* argv[1] is "face".  The first remaining argument that is neither a
   * number nor an option is the expression name; the shared options
   * (fb=, mx, ...) have already been consumed by main().
   */

  for (i = 2; i < argc; i++)
    {
      if (strncmp(argv[i], "fps=", 4) == 0)
        {
          int v = atoi(argv[i] + 4);

          if (v > 0 && v <= 60)
            {
              fps = (uint32_t)v;
            }
        }
      else if (argv[i][0] >= '0' && argv[i][0] <= '9')
        {
          max_frames = (uint32_t)atoi(argv[i]);
        }
      else if (name == NULL && strchr(argv[i], '=') == NULL &&
               strcmp(argv[i], "mx") != 0 && strcmp(argv[i], "my") != 0 &&
               strcmp(argv[i], "rot180") != 0 &&
               strcmp(argv[i], "swap") != 0)
        {
          name = argv[i];
        }
    }

  if (name == NULL)
    {
      printf("camera_preview face <name> [frames] [fps=N] "
             "(default 5fps)\n");

      for (i = 0; i < preview_face_count(); i++)
        {
          uint32_t d = preview_face_duration_ms(i);

          printf("  %-9s %s", preview_face_name(i),
                 d ? "animated" : "static  ");
          if (d)
            {
              printf(" %ums loop", (unsigned int)d);
            }

          printf("\n");
        }

      return OK;
    }

  idx = preview_face_lookup(name);
  if (idx < 0)
    {
      printf("preview: no expression called '%s' "
             "(run 'camera_preview face' for the list)\n", name);
      return -EINVAL;
    }

  ret = preview_open_fb(p, want_fb);
  if (ret < 0)
    {
      return ret;
    }

  area.x = 0;
  area.y = 0;
  area.w = p->vinfo.xres;
  area.h = p->vinfo.yres;

  dur    = preview_face_duration_ms(idx);
  period = 1000 / fps;

  printf("preview: face '%s' on %d panel(s), %s",
         preview_face_name(idx), p->nfb, dur ? "animated" : "static");
  if (dur)
    {
      printf(", %ums loop at %ufps, Ctrl-C to stop",
             (unsigned int)dur, (unsigned int)fps);
    }

  printf("\n");

  if (dur)
    {
      struct sigaction act;

      memset(&act, 0, sizeof(act));
      act.sa_handler = preview_sigint;
      sigemptyset(&act.sa_mask);

      if (sigaction(SIGINT, &act, NULL) < 0)
        {
          printf("preview: sigaction failed: %d (Ctrl-C will not be "
                 "clean)\n", errno);
        }

      preview_claim_ctty();
    }

  start = now_ms();

  for (; ; )
    {
      uint32_t t0 = now_ms();
      uint32_t t1;
      uint32_t spent;

      preview_face_render(p->fbmem[0], p->pinfo.stride,
                          (int)p->vinfo.xres, (int)p->vinfo.yres,
                          idx, dur ? t0 - start : 0);

      for (i = 1; i < p->nfb; i++)
        {
          /* Word-at-a-time rather than memcpy(): a 51200-byte memcpy of this
           * framebuffer measured 65ms on the board, which is slower than the
           * QSPI push of the same bytes.  The copy is the only place where
           * the renderer reads the framebuffer back, so it is worth doing by
           * hand.
           */

          FAR uint32_t *dst = (FAR uint32_t *)(void *)p->fbmem[i];
          FAR const uint32_t *src = (FAR const uint32_t *)
                                    (const void *)p->fbmem[0];
          size_t words = p->pinfo.fblen / 4;
          size_t w;

          for (w = 0; w < words; w++)
            {
              dst[w] = src[w];
            }
        }

      t1 = now_ms();
      render_ms += t1 - t0;
      preview_push_all(p, &area);
      push_ms += now_ms() - t1;
      frames++;

      if (dur == 0 || g_stop ||
          (max_frames != 0 && frames >= max_frames))
        {
          break;
        }

      spent = now_ms() - t0;
      if (spent < period)
        {
          usleep((period - spent) * 1000);
        }
    }

  printf("preview: face done, %u frame(s), render=%ums/f push=%ums/f\n",
         (unsigned int)frames, (unsigned int)(render_ms / frames),
         (unsigned int)(push_ms / frames));

  return OK;
}

static void preview_cleanup(FAR struct preview_s *p)
{
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int i;

  /* Hand the terminal back before anything else: it must not stay pointed at
   * this pid once the task is gone (see preview_claim_ctty).
   */

  preview_release_ctty();

  if (p->camfd >= 0)
    {
      ioctl(p->camfd, VIDIOC_STREAMOFF, (uintptr_t)&type);

      for (i = 0; i < p->nbuffers; i++)
        {
          if (p->bufs[i] != NULL && p->bufs[i] != MAP_FAILED)
            {
              munmap(p->bufs[i], p->buf_sizes[i]);
            }
        }

      close(p->camfd);
    }

  for (i = 0; i < p->nfb; i++)
    {
      if (p->fbmem[i] != NULL && p->fbmem[i] != MAP_FAILED)
        {
          munmap(p->fbmem[i], p->pinfo.fblen);
        }

      close(p->fbfd[i]);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct preview_s p;
  struct fb_area_s area;
  struct v4l2_buffer buf;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  uint32_t max_frames = 0;
  uint32_t frames = 0;
  uint32_t errors = 0;
  uint32_t conv_ms = 0;
  uint32_t push_ms = 0;
  uint32_t t_start;
  uint32_t t0;
  bool pattern_mode = false;
  bool face_mode = false;
  bool bench_mode = false;
  bool stats_mode = false;
  bool live_face = false;
  FAR const char *face_name = NULL;
  FAR const char *face_src = NULL;
  FAR const char *jpeg_out = NULL;
  FAR struct preview_jpeg_s *enc = NULL;
  int jpeg_every = 0;
  int jpeg_quality = 80;
  int cycle_s = 0;
  int face_idx = 0;
  uint32_t jpeg_frames = 0;
  uint32_t next_cycle_ms = 0;
  int want_fb = -1;               /* -1 = every registered panel */
  int i;
  int ret;

  /* NuttX's flat build keeps a builtin application's static storage across
   * invocations, so every one of these survives from one 'camera_preview' to
   * the next.  Leaving g_stop set is what made every animated expression
   * report "1 frame(s)" after the first Ctrl-C of the boot: the render loop
   * broke on a stop flag left over from a previous run.
   *
   * The decode and colour options have exactly the same problem, and it is
   * worse because it is silent: 'camera_preview 30 yuyv' followed by
   * 'camera_preview 30 sat=200' ran the second one in YUYV as well, and
   * printed so in its own banner without anybody asking for it.  Any A/B
   * comparison done that way compares two things that both changed.  So the
   * whole option set is returned to its default here, and the defaults live
   * in one place rather than being repeated.
   */

  g_stop       = 0;
  g_yuv_order  = PREVIEW_HW;
  g_full_range = false;
  g_uv_swap    = false;
  g_sat        = 100;
  g_gain       = 100;
  g_gamma      = 100;
  g_xoff       = 0;
  g_yoff       = 0;
  g_mirror_x   = false;
  g_mirror_y   = false;
  g_rgain      = 100;
  g_ggain      = 100;
  g_bgain      = 100;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "yuyv") == 0)
        {
          g_yuv_order = PREVIEW_YUYV;
        }
      else if (strcmp(argv[i], "uyvy") == 0)
        {
          g_yuv_order = PREVIEW_UYVY;
        }
      else if (strcmp(argv[i], "vyuy") == 0)
        {
          g_yuv_order = PREVIEW_HW;
        }
      else if (strcmp(argv[i], "full") == 0)
        {
          g_full_range = true;
        }
      else if (strcmp(argv[i], "uvswap") == 0)
        {
          g_uv_swap = true;
        }
      else if (strncmp(argv[i], "sat=", 4) == 0)
        {
          g_sat = atoi(argv[i] + 4);
        }
      else if (strncmp(argv[i], "gain=", 5) == 0)
        {
          g_gain = atoi(argv[i] + 5);
        }
      else if (strncmp(argv[i], "gamma=", 6) == 0)
        {
          g_gamma = atoi(argv[i] + 6);
        }
      else if (strncmp(argv[i], "xoff=", 5) == 0)
        {
          g_xoff = atoi(argv[i] + 5);
        }
      else if (strncmp(argv[i], "yoff=", 5) == 0)
        {
          g_yoff = atoi(argv[i] + 5);
        }
      else if (strcmp(argv[i], "mx") == 0)
        {
          g_mirror_x = true;
        }
      else if (strcmp(argv[i], "my") == 0)
        {
          g_mirror_y = true;
        }
      else if (strcmp(argv[i], "rot180") == 0)
        {
          g_mirror_x = true;
          g_mirror_y = true;
        }
      else if (strncmp(argv[i], "rgain=", 6) == 0)
        {
          g_rgain = atoi(argv[i] + 6);
        }
      else if (strncmp(argv[i], "ggain=", 6) == 0)
        {
          g_ggain = atoi(argv[i] + 6);
        }
      else if (strncmp(argv[i], "bgain=", 6) == 0)
        {
          g_bgain = atoi(argv[i] + 6);
        }
      else if (strcmp(argv[i], "fb=both") == 0)
        {
          want_fb = -1;
        }
      else if (strncmp(argv[i], "fb=", 3) == 0)
        {
          want_fb = atoi(argv[i] + 3);
        }
      else if (strncmp(argv[i], "expr=", 5) == 0)
        {
          face_name = argv[i] + 5;
        }
      else if (strncmp(argv[i], "src=", 4) == 0)
        {
          face_src = argv[i] + 4;
        }
      else if (strncmp(argv[i], "cycle=", 6) == 0)
        {
          cycle_s = atoi(argv[i] + 6);
        }
      else if (strcmp(argv[i], "jpeg") == 0)
        {
          jpeg_every = JPEG_DEFAULT_EVERY;
        }
      else if (strncmp(argv[i], "jpeg=", 5) == 0)
        {
          jpeg_every = atoi(argv[i] + 5);
        }
      else if (strncmp(argv[i], "jpegout=", 8) == 0)
        {
          jpeg_out = argv[i] + 8;
          if (jpeg_every == 0)
            {
              jpeg_every = JPEG_DEFAULT_EVERY;
            }
        }
      else if (strncmp(argv[i], "q=", 2) == 0)
        {
          jpeg_quality = atoi(argv[i] + 2);
        }
    }

  if (argc > 1)
    {
      if (strcmp(argv[1], "fill") == 0 || strcmp(argv[1], "bars") == 0 ||
          strcmp(argv[1], "grid") == 0 || strcmp(argv[1], "pattern") == 0 ||
          strcmp(argv[1], "ruler") == 0 || strcmp(argv[1], "wb") == 0)
        {
          pattern_mode = true;
        }
      else if (strcmp(argv[1], "face") == 0)
        {
          face_mode = true;
        }
      else if (strcmp(argv[1], "live+face") == 0 ||
               strcmp(argv[1], "liveface") == 0)
        {
          live_face = true;

          /* "live+face 200" -- same meaning as the bare frame count. */

          if (argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '9')
            {
              max_frames = (uint32_t)atoi(argv[2]);
            }
        }
      else if (strcmp(argv[1], "bench") == 0)
        {
          bench_mode = true;
        }
      else if (strcmp(argv[1], "stats") == 0)
        {
          stats_mode = true;
        }
      else
        {
          max_frames = (uint32_t)atoi(argv[1]);
        }
    }

  memset(&p, 0, sizeof(p));
  p.camfd = -1;

  /* Pattern mode never touches the camera: it is meant to be usable when
   * the capture path is the thing under suspicion.
   */

  if (face_mode)
    {
      ret = preview_run_face(&p, argc, argv, want_fb);
      goto out;
    }

  if (pattern_mode)
    {
      ret = preview_open_fb(&p, want_fb);
      if (ret < 0)
        {
          goto out;
        }

      area.x = 0;
      area.y = 0;
      area.w = p.vinfo.xres;
      area.h = p.vinfo.yres;

      preview_build_tone();
      ret = preview_draw_pattern(&p, argc, argv);
      if (ret == OK)
        {
          ret = preview_push_all(&p, &area);
        }

      goto out;
    }

  preview_build_tone();

  printf("preview: starting (%s -> fb%s), frames=%u, decode=%s, "
         "sat=%d%% gain=%d%% gamma=%d%% range=%s%s\n",
         CAM_DEV, want_fb < 0 ? "*" : (want_fb ? "1" : "0"),
         (unsigned int)max_frames,
         g_orders[g_yuv_order].name,
         g_sat, g_gain, g_gamma, g_full_range ? "full" : "limited",
         g_uv_swap ? " uvswap" : "");
  printf("preview: shift xoff=%d yoff=%d mirror=%s%s, "
         "gain R=%d%% G=%d%% B=%d%%\n", g_xoff, g_yoff,
         g_mirror_x ? "X" : "-", g_mirror_y ? "Y" : "-",
         g_rgain, g_ggain, g_bgain);

  ret = preview_open_fb(&p, want_fb);
  if (ret < 0)
    {
      goto out;
    }

  ret = preview_open_camera(&p);
  if (ret < 0)
    {
      goto out;
    }

  if (live_face)
    {
      /* Panel 0 gets the camera, panel 1 gets the expression.  Both are
       * opened; only the first is in the per-frame path.
       */

      if (p.nfb < 2)
        {
          printf("preview: live+face needs two panels, found %d\n", p.nfb);
          ret = -ENODEV;
          goto out;
        }

      p.npreview = 1;

      if (face_name != NULL)
        {
          face_idx = preview_face_lookup(face_name);
          if (face_idx < 0)
            {
              printf("preview: no expression named '%s'; run "
                     "'camera_preview face' for the list\n", face_name);
              ret = -EINVAL;
              goto out;
            }
        }

      (void)preview_paint_face_on(&p, 1, face_idx, 0);

      printf("preview: live+face -- fb%d live, fb%d expression '%s'%s%s%s\n",
             p.fbidx[0], p.fbidx[1], preview_face_name(face_idx),
             cycle_s > 0 ? ", cycling" : "",
             face_src ? ", source " : "", face_src ? face_src : "");
    }

  if (jpeg_every > 0)
    {
      enc = preview_jpeg_open(CAM_WIDTH, CAM_HEIGHT, jpeg_quality);
      if (enc == NULL)
        {
          printf("preview: continuing without JPEG\n");
        }
      else
        {
          printf("preview: encoding one frame every %d displayed\n",
                 jpeg_every);
        }
    }

  /* From here on the hardware is streaming, so an exit must go through
   * preview_cleanup().  SA_RESTART is deliberately not set: the loop only
   * needs the flag, and DQBUF is not interruptible anyway (it waits in
   * nxsem_wait_uninterruptible), so the flag is noticed when the next frame
   * or the driver's 500ms watchdog completes a buffer.
   */

  {
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = preview_sigint;
    sigemptyset(&act.sa_mask);

    if (sigaction(SIGINT, &act, NULL) < 0)
      {
        printf("preview: sigaction failed: %d (Ctrl-C will not be clean)\n",
               errno);
      }
  }

  preview_claim_ctty();

  if (ioctl(p.camfd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("preview: VIDIOC_STREAMON failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  area.x = 0;
  area.y = 0;
  area.w = p.vinfo.xres;
  area.h = p.vinfo.yres;

  if (stats_mode)
    {
      /* Several frames: the sensor's auto-exposure/AWB need a few frames to
       * settle, and a single frame could be mid-convergence.
       */

      int k;

      for (k = 0; k < 5; k++)
        {
          memset(&buf, 0, sizeof(buf));
          buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          buf.memory = V4L2_MEMORY_MMAP;

          if (ioctl(p.camfd, VIDIOC_DQBUF, (uintptr_t)&buf) < 0)
            {
              printf("stats: DQBUF failed: %d\n", errno);
              break;
            }

          if ((buf.flags & V4L2_BUF_FLAG_ERROR) == 0)
            {
              printf("stats: --- frame %d ---\n", k);
              preview_stats(&p, buf.index);
            }

          ioctl(p.camfd, VIDIOC_QBUF, (uintptr_t)&buf);
        }

      ret = OK;
      goto out;
    }

  if (bench_mode)
    {
      /* One DQBUF first so the buffer holds real captured data rather than
       * whatever the pool was left with -- a benchmark on untouched memory
       * could read differently.
       */

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      if (ioctl(p.camfd, VIDIOC_DQBUF, (uintptr_t)&buf) >= 0)
        {
          preview_bench(&p);
          ioctl(p.camfd, VIDIOC_QBUF, (uintptr_t)&buf);
        }
      else
        {
          printf("bench: DQBUF failed: %d\n", errno);
        }

      ret = OK;
      goto out;
    }

  t_start = now_ms();

  for (; ; )
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(p.camfd, VIDIOC_DQBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_DQBUF failed: %d\n", errno);
          break;
        }

      /* The imgdata driver reports timed-out frames with
       * V4L2_BUF_FLAG_ERROR rather than blocking forever; skip those
       * instead of showing stale pixels.
       */

      if ((buf.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          errors++;
        }
      else
        {
          t0 = now_ms();
          if (preview_convert(&p, buf.index) == OK)
            {
              conv_ms += now_ms() - t0;

              t0 = now_ms();
              (void)preview_push_all(&p, &area);
              push_ms += now_ms() - t0;
              frames++;

              /* One frame in every jpeg_every goes through the hardware
               * encoder as well.  This is the eyeglasses' upload path: the
               * process that owns the camera is the only one that can produce
               * a JPEG while a preview is running, so it produces both.
               */

              if (enc != NULL && (frames % (uint32_t)jpeg_every) == 0)
                {
                  FAR const uint8_t *jpg;
                  size_t jlen;

                  if (preview_jpeg_encode(enc, p.bufs[buf.index],
                                          p.buf_sizes[buf.index],
                                          &jpg, &jlen) == OK)
                    {
                      uint32_t cp_ms = 0;
                      uint32_t cd_ms = 0;

                      preview_jpeg_last_ms(enc, &cp_ms, &cd_ms);
                      jpeg_frames++;

                      printf("preview: jpeg #%u %zu bytes "
                             "(copy %ums + codec %ums), SOI=%02x%02x "
                             "EOI=%02x%02x\n",
                             (unsigned int)jpeg_frames, jlen,
                             (unsigned int)cp_ms, (unsigned int)cd_ms,
                             jpg[0], jpg[1],
                             jlen >= 2 ? jpg[jlen - 2] : 0,
                             jlen >= 1 ? jpg[jlen - 1] : 0);

                      if (jpeg_out != NULL)
                        {
                          FILE *f = fopen(jpeg_out, "w");

                          if (f == NULL)
                            {
                              printf("preview: cannot write %s (errno=%d); "
                                     "is /mnt mounted?\n",
                                     jpeg_out, errno);
                              jpeg_out = NULL;
                            }
                          else
                            {
                              size_t n = fwrite(jpg, 1, jlen, f);

                              fclose(f);
                              printf("preview: wrote %zu bytes to %s\n",
                                     n, jpeg_out);
                              jpeg_out = NULL;   /* first frame only */
                            }
                        }
                    }
                }

              /* Expression rotation, for demonstrating the second panel
               * without a model behind it yet.  It repaints only when the
               * expression actually changes.
               */

              /* Expression source.  Two ways to drive the second panel:
               *
               *   cycle=<sec>   rotate through the table, for showing the
               *                 panel works without a model behind it
               *   src=<path>    read the expression name from a file, which
               *                 is how a real verdict gets in: whoever does
               *                 the recognition (the agent's write_file tool,
               *                 social_cue, or a host script) writes one word
               *                 there and this loop picks it up.
               *
               * A file is used rather than an IPC channel because /dev/video0
               * has one owner: the recogniser cannot open the camera while
               * this preview holds it, so the two halves have to meet
               * somewhere, and a one-word file is the cheapest meeting point
               * that both the agent's tool sandbox and a shell can write.
               */

              if (live_face && face_src != NULL &&
                  (frames % SRC_POLL_FRAMES) == 0)
                {
                  FILE *sf = fopen(face_src, "r");

                  if (sf != NULL)
                    {
                      char word[32];

                      if (fgets(word, sizeof(word), sf) != NULL)
                        {
                          char *nl = strchr(word, '\n');
                          int idx;

                          if (nl != NULL)
                            {
                              *nl = '\0';
                            }

                          idx = preview_face_lookup(word);
                          if (idx >= 0 && idx != face_idx)
                            {
                              face_idx = idx;
                              (void)preview_paint_face_on(&p, 1, face_idx, 0);
                              printf("preview: expression <- %s (%s)\n",
                                     word, face_src);
                            }
                          else if (idx < 0)
                            {
                              printf("preview: %s names '%s', which is not an "
                                     "expression\n", face_src, word);
                            }
                        }

                      fclose(sf);
                    }
                }

              if (live_face && cycle_s > 0)
                {
                  uint32_t nowms = now_ms();

                  if (next_cycle_ms == 0)
                    {
                      next_cycle_ms = nowms + (uint32_t)cycle_s * 1000;
                    }
                  else if (nowms >= next_cycle_ms)
                    {
                      face_idx = (face_idx + 1) % preview_face_count();
                      (void)preview_paint_face_on(&p, 1, face_idx, 0);
                      printf("preview: expression -> %s\n",
                             preview_face_name(face_idx));
                      next_cycle_ms = nowms + (uint32_t)cycle_s * 1000;
                    }
                }
            }
          else
            {
              errors++;
            }
        }

      if (ioctl(p.camfd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_QBUF failed: %d\n", errno);
          break;
        }

      if (g_stop)
        {
          printf("preview: interrupted\n");
          break;
        }

      if (frames > 0 && (frames % REPORT_FRAMES) == 0)
        {
          uint32_t el = now_ms() - t_start;

          printf("preview: %u frames in %ums = %u.%02u fps "
                 "(convert %ums/f, push %ums/f, %u errors)\n",
                 (unsigned int)frames, (unsigned int)el,
                 (unsigned int)(el ? frames * 1000 / el : 0),
                 (unsigned int)(el ? frames * 100000 / el % 100 : 0),
                 (unsigned int)(conv_ms / frames),
                 (unsigned int)(push_ms / frames),
                 (unsigned int)errors);
        }

      if (max_frames != 0 && frames >= max_frames)
        {
          break;
        }
    }

  printf("preview: done, %u frames, %u errors\n",
         (unsigned int)frames, (unsigned int)errors);
  ret = OK;

out:
  if (enc != NULL)
    {
      preview_jpeg_close(enc);
    }

  preview_cleanup(&p);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
