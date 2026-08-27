/****************************************************************************
 * app/agent_camera/agent_camera_main.c
 *
 * Bench for the camera side of the ai_agent port.
 *
 * packages/ai_agent's camera_capture tool (src/tools/tool_camera.c) reaches
 * the camera through V4L2 and then hands the frame straight to a Vision LLM.
 * The AP now has a real wlan0 backed by the CP, so the managed tool can run
 * end to end.  This command remains the focused camera-side bench: it uses
 * the same capture sequence but validates or exports the JPEG locally instead
 * of requiring an LLM response.
 *
 * It answers two camera-specific questions:
 *
 *   1. Does the JPEG capture path work at all through V4L2?  The path is
 *      new (bk7258_jpeg_enc.c plus the JPEG branch of
 *      bk7258_camera_imgdata.c) and the UYVY path is the only one with
 *      board evidence behind it, so "JPEG works" cannot be assumed from
 *      "preview works".
 *
 *   2. Does tool_camera.c's requested geometry fit this board?  The pinned
 *      upstream baseline asks for 320x180 ("low") or 1280x720 ("high"),
 *      while this driver programs 480x480 / 640x480 / 864x480 and rejects
 *      any other exact size.  The complete managed tool_camera.c now retries
 *      an enumerated size.  This bench mirrors that algorithm: `low` succeeds
 *      through negotiation, while `low strict` reproduces the pinned
 *      baseline's rejected-geometry behavior.
 *
 * Usage:
 *   agent_camera [auto|low|high|<W>x<H>] [options]
 *
 *     auto      (default) first JPEG size the driver enumerates
 *     low       320x180  -- tool_camera.c's default request
 *     high      1280x720 -- tool_camera.c's "high" request
 *     <W>x<H>   an explicit geometry
 *
 *   Options:
 *     out=<path>   write the frame (e.g. out=/mnt/cap.jpg; needs a mounted
 *                  filesystem -- see docs/reference/camera.md 9.3)
 *     b64          print the first frame as base64 between fence lines, to
 *                  retrieve it with no filesystem involved:
 *
 *                    ./serial_cmd.sh -r -w 60 -o /tmp/b64.log 'agent_camera b64'
 *                    sed -n '/BEGIN AGENT_CAMERA/,/END AGENT_CAMERA/p' /tmp/b64.log \
 *                      | sed '1d;$d' | tr -d '\r\n' | base64 -d > cap.jpg
 *
 *                  Then check it with a real decoder, not just the markers:
 *                  `identify cap.jpg` / PIL.  See docs/reference/camera.md
 *                  14.5-14.6 for why the marker check is not enough.
 *     n=<count>    capture <count> frames in one streaming session
 *                  (default 1, which is what the tool does)
 *     b64all       like b64 but for *every* frame of the session, each in
 *                  its own fenced block tagged with its index and length.
 *                  This is the recording interface: a burst comes out as a
 *                  sequence of JPEGs that the host assembles into a clip.
 *
 *                    ./serial_cmd.sh -r -w 150 -o /tmp/clip.log \
 *                       'agent_camera n=24 b64all'
 *
 *                  Every session also ends with a measured frame rate line
 *                  ("session N frame(s) in X ms = Y.YY fps").  Take that
 *                  number from a run *without* b64all: printing tens of
 *                  kilobytes per frame down a 115200 console dominates the
 *                  session and the rate reported with b64all is the console
 *                  speed, not the camera's.
 *     rec=<count>  record <count> frames into memory at the sensor's own
 *                  rate and dump them afterwards.  This is what a clip
 *                  needs: with b64all the console pace (3.5s per frame at
 *                  115200) sets the sampling interval and the result is a
 *                  time lapse, not motion.  Combine with n= large enough to
 *                  cover the recording, e.g.
 *
 *                    'agent_camera 640x480 bufs=4 n=60 rec=20'
 *
 *                  rec frames are kept out of the n captured, spread evenly:
 *                  n=60 rec=20 keeps every third frame, i.e. a 20-frame clip
 *                  at about 10fps with the sensor still running at 30.  Ask
 *                  for rec == n only at low resolution: copying a 640x480
 *                  frame out takes longer than the 33ms between frames (both
 *                  ends are non-cacheable PSRAM), the DMA catches up, and
 *                  most frames come back with a truncated scan.
 *
 *                  Frames are held in the heap, which reaches PSRAM here, so
 *                  30 x ~30KB is fine; the SRAM heap alone would not be.
 *     caps         only enumerate formats and sizes, capture nothing
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/video/video.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAM_DEV            "/dev/video0"

/* Mirrors tool_camera.c: two MMAP buffers, a 160KB sizeimage hint and a 5s
 * frame timeout.  The sizeimage hint is what v4l2_cap.c's get_bufsize()
 * uses for a compressed format, so it -- not width*height -- decides how
 * much the imgdata allocator takes out of PSRAM for each buffer.
 */

#define CAM_BUF_SIZE       (160 * 1024)
#define CAM_NUM_BUFFERS    2

/* How many buffers this tool can hold at once.  The *default* stays at
 * CAM_NUM_BUFFERS because that is what packages/ai_agent's camera_capture
 * asks for and this tool exists to reproduce it; 'bufs=4' is for recording,
 * where the copy out of each frame has to overlap the DMA filling the next
 * one and two buffers leave no slack.
 */

#define CAM_MAX_BUFFERS    4
#define CAM_TIMEOUT_MS     5000

/* tool_camera.c's two hardcoded geometries. */

#define TOOL_LOW_WIDTH     320
#define TOOL_LOW_HEIGHT    180
#define TOOL_HIGH_WIDTH    1280
#define TOOL_HIGH_HEIGHT   720

/* tool_camera.c's fallback format, spelled the same way it spells it. */

#ifndef V4L2_PIX_FMT_ENTROPY
#  define V4L2_PIX_FMT_ENTROPY  v4l2_fourcc('G', 'R', 'E', 'P')
#endif

#define MAX_ENUM_SIZES     16

/* base64 characters per output line.  Must be a multiple of 4 so a line never
 * splits a quad, and <= 255 so one line is exactly one CP bridge log entry
 * (AP_BRIDGE_LOG_LINE_SIZE = 256 in the CP's driver.c) -- see
 * agent_camera_print_b64().
 */

#define B64_LINE_CHARS     252

/* Bytes per second the console route is paced at.  The CP's UART0 runs at
 * 115200 8N1 = 11520 B/s and the CP drops rather than blocks when its log
 * queue backs up, so this has to stay under the drain rate with margin for
 * the driver's own log lines.  Raise it only together with the CP's baud.
 */

#define B64_CONSOLE_BPS    9600

/* TCP delivery: bytes handed to send() at a time, and how long to wait for
 * the connect.
 */

#define TCP_SEND_CHUNK     1460
#define TCP_CONNECT_MS     5000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct agent_camera_size_s
{
  uint16_t width;
  uint16_t height;
};

struct agent_camera_s
{
  int      fd;
  void    *bufs[CAM_MAX_BUFFERS];
  size_t   buflen[CAM_MAX_BUFFERS];
  uint32_t nbuffers;
  bool     streaming;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Buffer count, settable with bufs=N.
 *
 * Exists because frames alternate good/short by buffer index on this board:
 * with two buffers the ones landing in index 0 come back at about 13.6KB and
 * do not decode, while index 1 gives a full ~43KB frame.  Being able to ask
 * for a single buffer separates "the JPEG path is broken" from "re-arming the
 * DMA onto the next buffer loses the head of the frame".
 */

static uint32_t g_nbufs = CAM_NUM_BUFFERS;

static void agent_camera_usage(void)
{
  printf("Usage: agent_camera [auto|low|high|<W>x<H>] "
         "[out=<path>] [n=<count>] [strict] [caps]\n"
         "  auto     first JPEG size the driver enumerates (default)\n"
         "  low      320x180, tool_camera.c's default request\n"
         "  high     1280x720, tool_camera.c's \"high\" request\n"
         "  strict   do not negotiate a refused geometry (pinned upstream\n"
         "           tool_camera.c behaviour)\n"
         "  b64      print the last frame as base64 (retrieve without a\n"
         "           filesystem; see the file header)\n"
         "  b64all   print every frame as base64, one fenced block each\n"
         "  tcp=<ip>:<port>  push the frame over TCP as raw JPEG, one\n"
         "           connection per frame (host: nc -l -p <port> >f.jpg, or\n"
         "           tools/tcpframes.py).  Needs wlan0 up; far faster than\n"
         "           the console -- see agent_camera_send_tcp()\n"
         "  session=<id>  tag frames with a session id, sequence and\n"
         "           monotonic timestamp (the metadata an upload needs)\n"
         "  rec=<n>  record n frames at the sensor rate into memory, then\n"
         "           dump them all as base64 -- use this for a clip\n"
         "  caps     enumerate formats and sizes only\n");
}

/* Prints a fourcc the way the rest of the tree does, so log lines can be
 * grepped against the driver's own format tables.
 */

static void agent_camera_print_fourcc(uint32_t fourcc)
{
  printf("%c%c%c%c",
         (char)(fourcc & 0xff),
         (char)((fourcc >> 8) & 0xff),
         (char)((fourcc >> 16) & 0xff),
         (char)((fourcc >> 24) & 0xff));
}

/****************************************************************************
 * Name: agent_camera_enum
 *
 * Description:
 *   Enumerates the formats the device offers and, for each, its discrete
 *   frame sizes.  Collects the sizes belonging to want_fmt into sizes[].
 *
 *   This is the part tool_camera.c is missing: it hardcodes a geometry and
 *   gives up when VIDIOC_S_FMT rejects it, with no way to learn what the
 *   device would have accepted.  Everything the driver can do is already
 *   discoverable through these two ioctls.
 *
 * Returned Value:
 *   Number of sizes collected for want_fmt, or a negated errno.
 *
 ****************************************************************************/

static int agent_camera_enum(int fd, uint32_t want_fmt,
                            struct agent_camera_size_s *sizes,
                            size_t max_sizes, bool verbose)
{
  struct v4l2_fmtdesc fmtdesc;
  unsigned int nsizes = 0;
  unsigned int i;

  for (i = 0; ; i++)
    {
      memset(&fmtdesc, 0, sizeof(fmtdesc));
      fmtdesc.index = i;
      fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(fd, VIDIOC_ENUM_FMT, (uintptr_t)&fmtdesc) < 0)
        {
          break;
        }

      if (verbose)
        {
          printf("agent_camera: format[%u] ", i);
          agent_camera_print_fourcc(fmtdesc.pixelformat);
          printf(" flags=0x%" PRIx32 " %s\n",
                 (uint32_t)fmtdesc.flags,
                 fmtdesc.description[0] ? (char *)fmtdesc.description : "");
        }

      /* Sizes are enumerated per format, so this inner loop has to run for
       * every format even when only one of them is wanted: the framesize
       * enumeration is indexed by (pixel_format, index) and the driver
       * returns EINVAL for an index that belongs to another format.
       */

      unsigned int j;

      for (j = 0; ; j++)
        {
          struct v4l2_frmsizeenum frmsize;

          memset(&frmsize, 0, sizeof(frmsize));
          frmsize.index        = j;
          frmsize.pixel_format = fmtdesc.pixelformat;
          frmsize.buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, (uintptr_t)&frmsize) < 0)
            {
              break;
            }

          if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
            {
              continue;
            }

          if (verbose)
            {
              printf("agent_camera:   size[%u] %" PRIu32 "x%" PRIu32 "\n",
                     j,
                     (uint32_t)frmsize.discrete.width,
                     (uint32_t)frmsize.discrete.height);
            }

          if (fmtdesc.pixelformat == want_fmt && nsizes < max_sizes)
            {
              sizes[nsizes].width  = (uint16_t)frmsize.discrete.width;
              sizes[nsizes].height = (uint16_t)frmsize.discrete.height;
              nsizes++;
            }
        }
    }

  if (i == 0)
    {
      printf("agent_camera: VIDIOC_ENUM_FMT rejected index 0: %d\n", errno);
      return -errno;
    }

  return (int)nsizes;
}

/****************************************************************************
 * Name: agent_camera_try_format
 *
 * Description:
 *   Offers one (geometry, pixel format) pair to the driver.  Sets sizeimage
 *   the way tool_camera.c does, since for a compressed format that is what
 *   decides how large a buffer the framework allocates.
 *
 ****************************************************************************/

static int agent_camera_try_format(int fd, int width, int height,
                                  uint32_t pixelformat)
{
  struct v4l2_format fmt;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = pixelformat;
  fmt.fmt.pix.sizeimage   = CAM_BUF_SIZE;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;

  return ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0 ? ERROR : OK;
}

/****************************************************************************
 * Name: agent_camera_set_format
 *
 * Description:
 *   tool_camera.c's format step: ask for V4L2_PIX_FMT_JPEG and, if that is
 *   refused, ask for V4L2_PIX_FMT_ENTROPY.
 *
 *   When negotiate is true this also mirrors the geometry negotiation in the
 *   overlay-managed complete target file:
 *   external/packages/ai_agent/src/tools/tool_camera.c
 *   A refused geometry is retried at the enumerated size whose pixel count
 *   is closest.  Same algorithm, same device, same driver, so `low` versus
 *   `low strict` directly compares the overlay behavior with the fixed
 *   baseline on this board.
 *
 *   Note that VIDIOC_ENUM_FRAMESIZES cannot be trusted to filter by pixel
 *   format: v4l2_cap.c's capture_enum_frmsize() indexes the imgsensor's
 *   frmsizes array and ignores f->pixel_format entirely, so every format
 *   enumerates every size.  VIDIOC_S_FMT is therefore the only authority on
 *   whether a (format, geometry) pair is real, which is why each candidate
 *   is put back through it rather than trusted because it was enumerated.
 *
 ****************************************************************************/

static int agent_camera_set_format(int fd, int *width, int *height,
                                  bool negotiate, uint32_t *out_fmt)
{
  static const uint32_t candidates[] =
  {
    V4L2_PIX_FMT_JPEG,
    V4L2_PIX_FMT_ENTROPY,
  };

  struct agent_camera_size_s sizes[MAX_ENUM_SIZES];
  size_t c;

  for (c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++)
    {
      if (agent_camera_try_format(fd, *width, *height, candidates[c]) == OK)
        {
          *out_fmt = candidates[c];
          return OK;
        }

      printf("agent_camera: S_FMT ");
      agent_camera_print_fourcc(candidates[c]);
      printf(" %dx%d failed: %d\n", *width, *height, errno);
    }

  if (!negotiate)
    {
      return -EINVAL;
    }

  for (c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++)
    {
      unsigned long want = (unsigned long)(*width) *
                           (unsigned long)(*height);
      unsigned long best_delta = ULONG_MAX;
      int best_w = 0;
      int best_h = 0;
      int nsizes;
      int i;

      nsizes = agent_camera_enum(fd, candidates[c], sizes,
                                 MAX_ENUM_SIZES, false);
      if (nsizes <= 0)
        {
          continue;
        }

      for (i = 0; i < nsizes; i++)
        {
          unsigned long area = (unsigned long)sizes[i].width *
                               (unsigned long)sizes[i].height;
          unsigned long delta = area > want ? area - want : want - area;

          if (delta < best_delta)
            {
              best_delta = delta;
              best_w = sizes[i].width;
              best_h = sizes[i].height;
            }
        }

      if (best_w == 0 || (best_w == *width && best_h == *height))
        {
          continue;
        }

      if (agent_camera_try_format(fd, best_w, best_h, candidates[c]) == OK)
        {
          printf("agent_camera: %dx%d refused, using enumerated %dx%d\n",
                 *width, *height, best_w, best_h);
          *width   = best_w;
          *height  = best_h;
          *out_fmt = candidates[c];
          return OK;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: agent_camera_check_jpeg
 *
 * Description:
 *   Reports whether the frame is a structurally valid JPEG file.
 *
 *   This used to check only SOI at the front and EOI at the end, and that
 *   judgement was wrong in a way worth spelling out: a frame can pass it and
 *   still be undecodable.  The encoder on this board emits
 *   SOI / APP0 / SOF0 / DQT / DHT and then entropy data with **no SOS
 *   segment**, so libjpeg rejects the file outright ("Invalid JPEG file
 *   structure: missing SOS marker") while SOI and EOI are both present.  A
 *   check both the driver and this tool agreed on was simply not sufficient
 *   -- see docs/reference/camera.md 14.6.
 *
 *   So the segment chain is walked properly here.  Note that markers on this
 *   encoder are preceded by 0xFF fill bytes (three of them, e.g.
 *   ff ff ff ff db); the standard permits that, and a scanner that does not
 *   skip fill will desynchronise and report segments that do not exist.
 *
 ****************************************************************************/

static void agent_camera_check_jpeg(const uint8_t *data, size_t len)
{
  bool have_sof = false;
  bool have_sos = false;
  bool have_dqt = false;
  bool have_dht = false;
  size_t i = 0;

  if (len < 4)
    {
      printf("agent_camera: frame too short to inspect (%zu bytes)\n", len);
      return;
    }

  if (data[0] != 0xff || data[1] != 0xd8)
    {
      printf("agent_camera: no SOI: starts %02x %02x\n", data[0], data[1]);
      return;
    }

  /* Walk the segments.  Stops at SOS, which is where entropy data begins. */

  i = 2;
  while (i + 1 < len)
    {
      uint8_t marker;
      size_t seglen;

      if (data[i] != 0xff)
        {
          break;
        }

      /* Skip fill bytes: any number of 0xFF may precede a marker. */

      while (i + 1 < len && data[i + 1] == 0xff)
        {
          i++;
        }

      if (i + 1 >= len)
        {
          break;
        }

      marker = data[i + 1];

      if (marker == 0xd9)
        {
          break;
        }

      if (marker == 0xda)
        {
          have_sos = true;
          break;
        }

      if (i + 3 >= len)
        {
          break;
        }

      seglen = ((size_t)data[i + 2] << 8) | data[i + 3];
      if (seglen < 2)
        {
          break;
        }

      switch (marker)
        {
          case 0xc0:
          case 0xc1:
            have_sof = true;
            break;

          case 0xdb:
            have_dqt = true;
            break;

          case 0xc4:
            have_dht = true;
            break;

          default:
            break;
        }

      i += 2 + seglen;
    }

  printf("agent_camera: structure SOF=%s DQT=%s DHT=%s SOS=%s EOI=%s\n",
         have_sof ? "yes" : "NO", have_dqt ? "yes" : "NO",
         have_dht ? "yes" : "NO", have_sos ? "yes" : "NO",
         (data[len - 2] == 0xff && data[len - 1] == 0xd9) ? "yes" : "NO");

  if (!have_sos)
    {
      printf("agent_camera: NOT a decodable JPEG -- no SOS segment, so the "
             "entropy data has no scan header.  A Vision LLM (or any "
             "standard decoder) will reject this.  See "
             "docs/reference/camera.md 14.6\n");
    }
}

/****************************************************************************
 * Name: agent_camera_print_b64
 *
 * Description:
 *   Prints the frame as base64 between two fence lines, so it can be pulled
 *   off the board without a filesystem.
 *
 *   The alternative is `hexdump` plus docs/tools/hexdump2raw.py, which works
 *   (and is what first got a frame onto the host) but needs a mounted
 *   filesystem, costs 4.4 bytes of console per payload byte -- a 35KB frame
 *   became 170KB of serial output -- and has to be parsed with tolerance for
 *   block headers, ANSI escapes and the ASCII column.  base64 is 1.37 bytes
 *   per byte and `base64 -d` is exact, so there is nothing to get wrong on
 *   the host side.
 *
 *   Written in 252-character lines straight to fd 1 with write(), not through
 *   stdout's buffer, and with no intermediate copy of the frame: a second
 *   copy of a 160KB frame does not fit in the AP's kernel heap.
 *
 *   Both the line size and the pacing are transport facts, not taste.
 *   Measured 2026-08-17 on 640x480 (~36.4KB frames):
 *
 *     76-char lines via fwrite + usleep(30ms)/2 lines : 41.5s, 1.19KB/s
 *     1024-char lines via write(), no pacing          :  2.5s, but LOSSY
 *     252-char lines via write() + clock pacing       : see docs/, lossless
 *
 *   Two independent limits, found in that order:
 *
 *   1. Cost per write(), not per byte.  CONFIG_STDIO_LINEBUFFER with
 *      CONFIG_STDIO_BUFFER_SIZE=64 turns every 76-char line into two flushes;
 *      each flush becomes its own mailbox transaction, and
 *      bk7258_mb_uart.c's worker advances on a 10ms tick
 *      (MB_UART_WORK_INTERVAL) carrying at most 128 bytes
 *      (BK7258_MB_UART_CHUNK_SIZE).  38 bytes per 10ms tick is ~4KB/s at any
 *      baud rate.  One write() per line fixes that.
 *
 *   2. The CP drops instead of blocking.  With the pacing removed the run
 *      finished in 2.5s but the payload was short, and the CP's own counter
 *      says why: `ap_console status` went from qfail=158 to qfail=317 across
 *      one frame, i.e. 159 discarded entries, while rx_bytes rose by the full
 *      56659.  So the AP handed everything over and the CP threw part of it
 *      away -- its bridge calls shell_log_raw_data_nonblock() and only bumps
 *      a counter on failure (CP driver.c, ap_bridge_log_output()).  The
 *      mailbox-level RTS the AP honours covers the CP's mailbox rx_buf, not
 *      the shell log queue behind it, so there is no end-to-end backpressure
 *      to lean on and the AP has to stay under the CP's drain rate itself.
 *
 *   Hence B64_CONSOLE_BPS and agent_camera_pace(): a clock-driven budget
 *   rather than a fixed sleep every N lines.  The old code did have such a
 *   sleep, but its comment described a "1024-byte bridge flushing half-lines
 *   on a 50ms timer" -- which is not what any of this code does (8192-byte
 *   ring, 128-byte chunks, 10ms worker) -- so the value could not be reasoned
 *   about, only re-tuned by trial.
 *
 *   The single write() also removes an interleaving hazard rather than adding
 *   one: uart_write() holds dev->xmit.lock for the whole call
 *   (nuttx/drivers/serial/serial.c), so a driver log line from another thread
 *   can land between payload lines but no longer inside one.  A log line
 *   spliced into the middle of a payload line is what b64frames.py has to
 *   discard, and discarding it loses the frame.
 *
 *   None of this makes the console fast.  11520 B/s is the ceiling and base64
 *   inflates by 4/3, so ~4.3s per frame is the floor here and 5 fps is out of
 *   reach by an order of magnitude.  Use tcp= when the network is up; this
 *   path exists for when it is not.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: agent_camera_fnv1a
 *
 * Description:
 *   32-bit FNV-1a over the frame, printed next to the base64 fence so the
 *   host can prove the retrieval was lossless before drawing any conclusion
 *   about the picture.
 *
 *   This exists because two frames came back decoding only 8 rows while a
 *   third decoded fully, and "the board produced junk" and "the console
 *   dropped bytes" predict exactly the same thing on the host.  Length
 *   agreement does not separate them: a dropped chunk replaced by an
 *   interleaved driver log line keeps the byte count plausible.  FNV-1a is
 *   enough here (we are detecting loss, not defending against tampering)
 *   and costs one pass with no buffer.
 *
 ****************************************************************************/

static uint32_t agent_camera_fnv1a(const uint8_t *data, size_t len)
{
  uint32_t hash = 2166136261u;
  size_t i;

  for (i = 0; i < len; i++)
    {
      hash ^= (uint32_t)data[i];
      hash *= 16777619u;
    }

  return hash;
}

/****************************************************************************
 * Name: agent_camera_copy32
 *
 * Description:
 *   Word-at-a-time copy out of a capture buffer.  This libc's memcpy is
 *   byte-wise, and the source is non-cacheable PSRAM, so copying a ~28KB
 *   frame with it takes longer than the 33ms the sensor gives us between
 *   frames.  With only a couple of V4L2 buffers in flight that loses the
 *   race: the DMA reaches the buffer we are still reading and the frame
 *   comes out with a truncated scan -- SOI, EOI and every marker present, so
 *   the structure check passes, but a real decoder reports "broken data
 *   stream".  Measured before this existed: 3 of 29 recorded frames decoded.
 *
 *   Both ends are 4-byte aligned here (the capture buffers come from a PSRAM
 *   pool, the destination from malloc), and the tail is handled bytewise.
 *
 ****************************************************************************/

static void agent_camera_copy32(FAR uint8_t *dst, FAR const uint8_t *src,
                                size_t len)
{
  FAR uint32_t *d = (FAR uint32_t *)dst;
  FAR const uint32_t *s = (FAR const uint32_t *)src;
  size_t words = len >> 2;
  size_t i;

  for (i = 0; i < words; i++)
    {
      d[i] = s[i];
    }

  for (i = words << 2; i < len; i++)
    {
      dst[i] = src[i];
    }
}

static void agent_camera_write_all(FAR const char *buf, size_t len)
{
  while (len > 0)
    {
      ssize_t nwritten = write(STDOUT_FILENO, buf, len);

      if (nwritten <= 0)
        {
          if (nwritten < 0 && (errno == EINTR || errno == EAGAIN))
            {
              continue;
            }

          return;
        }

      buf += nwritten;
      len -= (size_t)nwritten;
    }
}

/****************************************************************************
 * Name: agent_camera_pace
 *
 * Description:
 *   Hold the cumulative output rate at B64_CONSOLE_BPS by sleeping only when
 *   we are ahead of that budget.  Paced against CLOCK_MONOTONIC and the total
 *   byte count, not with a fixed sleep every N lines: a fixed sleep has to be
 *   tuned for the worst case and then pays it always, and it silently becomes
 *   wrong the moment the line length or the baud rate changes.
 *
 ****************************************************************************/

static void agent_camera_pace(FAR const struct timespec *start,
                              unsigned long emitted)
{
  struct timespec now;
  unsigned long elapsed_ms;
  unsigned long budget_ms;

  clock_gettime(CLOCK_MONOTONIC, &now);

  elapsed_ms = (unsigned long)((now.tv_sec - start->tv_sec) * 1000 +
                               (now.tv_nsec - start->tv_nsec) / 1000000);
  budget_ms = emitted * 1000ul / B64_CONSOLE_BPS;

  if (budget_ms > elapsed_ms)
    {
      usleep((budget_ms - elapsed_ms) * 1000);
    }
}

static void agent_camera_print_b64(const uint8_t *data, size_t len)
{
  static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static char line[B64_LINE_CHARS + 1];
  struct timespec t_start;
  unsigned long emitted = 0;
  size_t used = 0;
  size_t i;

  printf("agent_camera: payload len=%zu fnv1a=0x%08" PRIx32 "\n",
         len, agent_camera_fnv1a(data, len));

  printf("-----BEGIN AGENT_CAMERA JPEG %zu-----\n", len);

  /* Drain the fence line out of the stdio buffer before switching to raw
   * write(): everything below bypasses stdout's buffer, so anything still
   * sitting in it would surface after the payload.
   */

  fflush(stdout);
  clock_gettime(CLOCK_MONOTONIC, &t_start);

  for (i = 0; i < len; i += 3)
    {
      size_t remain = len - i;
      uint32_t v = (uint32_t)data[i] << 16;

      if (remain > 1)
        {
          v |= (uint32_t)data[i + 1] << 8;
        }

      if (remain > 2)
        {
          v |= data[i + 2];
        }

      line[used++] = tbl[(v >> 18) & 0x3f];
      line[used++] = tbl[(v >> 12) & 0x3f];
      line[used++] = remain > 1 ? tbl[(v >> 6) & 0x3f] : '=';
      line[used++] = remain > 2 ? tbl[v & 0x3f] : '=';

      if (used == B64_LINE_CHARS)
        {
          line[used++] = '\n';
          agent_camera_write_all(line, used);
          emitted += used;
          used = 0;
          agent_camera_pace(&t_start, emitted);
        }
    }

  if (used > 0)
    {
      line[used++] = '\n';
      agent_camera_write_all(line, used);
    }

  printf("-----END AGENT_CAMERA JPEG-----\n");
  fflush(stdout);
}

/****************************************************************************
 * Name: agent_camera_connect_timeout
 *
 * Description:
 *   connect() with an upper bound.  Returns true on success; on failure errno
 *   holds the reason (ETIMEDOUT when the bound expired) and the socket is
 *   left for the caller to close.
 *
 ****************************************************************************/

static bool agent_camera_connect_timeout(int sock,
                                         FAR const struct sockaddr_in *addr,
                                         int timeout_ms)
{
  struct pollfd pfd;
  int flags;
  int error = 0;
  socklen_t errlen = sizeof(error);
  int ret;

  flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      return false;
    }

  ret = connect(sock, (FAR const struct sockaddr *)addr, sizeof(*addr));
  if (ret < 0 && errno != EINPROGRESS)
    {
      return false;
    }

  if (ret < 0)
    {
      pfd.fd = sock;
      pfd.events = POLLOUT;
      pfd.revents = 0;

      ret = poll(&pfd, 1, timeout_ms);
      if (ret == 0)
        {
          errno = ETIMEDOUT;
          return false;
        }

      if (ret < 0)
        {
          return false;
        }

      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errlen) < 0)
        {
          return false;
        }

      if (error != 0)
        {
          errno = error;
          return false;
        }
    }

  /* Back to blocking so the send() loop below does not have to spin. */

  if (fcntl(sock, F_SETFL, flags) < 0)
    {
      return false;
    }

  return true;
}

/****************************************************************************
 * Name: agent_camera_send_tcp
 *
 * Description:
 *   Push one frame to <host>:<port> over TCP and close.  The close is the
 *   frame delimiter, so the host side can be `nc -l -p 5000 > frame.jpg`
 *   with nothing else involved; tools/tcpframes.py does the same thing for a
 *   burst, one file per connection.
 *
 *   Why this exists next to 'b64': the console cannot carry a frame at any
 *   useful rate.  115200 8N1 is 11.5KB/s and base64 inflates 36KB to 49KB,
 *   so 4.3s is the floor there even with a perfect transport -- and 5 fps of
 *   640x480 needs 125KB/s, which the console cannot reach at any baud this
 *   board offers.  The Wi-Fi path does not have that problem: the AP hands
 *   whole packets to the CP by reference over the mailbox
 *   (bk7258_wifi.c: wifi_send_node_async(), a node descriptor rather than a
 *   128-byte copy), so a frame is a few dozen packets, not a few hundred
 *   mailbox chunks.
 *
 *   The payload is the raw JPEG.  No base64 (saves 33%), no fence lines, no
 *   escaping: TCP already delivers bytes in order or not at all.  The
 *   len/fnv1a line is still printed on the console so the host can prove the
 *   file it received is the frame the board sent.
 *
 ****************************************************************************/

static int agent_camera_send_tcp(FAR const char *target,
                                 const uint8_t *data, size_t len)
{
  struct sockaddr_in addr;
  struct timespec t_start;
  struct timespec t_end;
  unsigned long elapsed_ms;
  FAR const char *colon;
  char host[INET_ADDRSTRLEN];
  size_t hostlen;
  size_t sent = 0;
  int port;
  int sock;

  colon = strrchr(target, ':');
  if (colon == NULL)
    {
      printf("agent_camera: tcp=%s needs <ip>:<port>\n", target);
      return -EINVAL;
    }

  hostlen = (size_t)(colon - target);
  if (hostlen == 0 || hostlen >= sizeof(host))
    {
      printf("agent_camera: tcp=%s has an unusable address\n", target);
      return -EINVAL;
    }

  memcpy(host, target, hostlen);
  host[hostlen] = '\0';

  port = atoi(colon + 1);
  if (port <= 0 || port > 65535)
    {
      printf("agent_camera: tcp=%s has an unusable port\n", target);
      return -EINVAL;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);

  /* Numeric addresses only.  A DNS lookup here would add a failure mode
   * (and a timeout) to a path whose whole point is to be fast, and the host
   * running the receiver is on the same LAN by construction.
   */

  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      printf("agent_camera: tcp=%s is not a numeric IPv4 address\n", host);
      return -EINVAL;
    }

  printf("agent_camera: payload len=%zu fnv1a=0x%08" PRIx32 "\n",
         len, agent_camera_fnv1a(data, len));

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      printf("agent_camera: socket failed: %d\n", errno);
      return -errno;
    }

  clock_gettime(CLOCK_MONOTONIC, &t_start);

  /* Bounded connect.  A blocking connect() to an address that is routable but
   * silently dropped -- a guest SSID with client isolation is the ordinary
   * case -- retries SYNs for far longer than the capture session lasts, and
   * because this runs inside the frame loop the whole command wedges with the
   * sensor still streaming.  Observed exactly that before this existed.
   */

  if (!agent_camera_connect_timeout(sock, &addr, TCP_CONNECT_MS))
    {
      printf("agent_camera: connect %s:%d failed: %d\n", host, port, errno);
      close(sock);
      return -errno;
    }

  while (sent < len)
    {
      size_t chunk = len - sent;
      ssize_t nsent;

      if (chunk > TCP_SEND_CHUNK)
        {
          chunk = TCP_SEND_CHUNK;
        }

      nsent = send(sock, data + sent, chunk, 0);
      if (nsent < 0)
        {
          if (errno == EINTR || errno == EAGAIN)
            {
              continue;
            }

          printf("agent_camera: send failed at %zu of %zu: %d\n",
                 sent, len, errno);
          close(sock);
          return -errno;
        }

      sent += (size_t)nsent;
    }

  /* The close is the delimiter, and it is also what flushes the last
   * segment, so its cost belongs inside the measurement.
   */

  close(sock);
  clock_gettime(CLOCK_MONOTONIC, &t_end);

  elapsed_ms = (unsigned long)
    ((t_end.tv_sec - t_start.tv_sec) * 1000 +
     (t_end.tv_nsec - t_start.tv_nsec) / 1000000);

  printf("agent_camera: sent %zu bytes to %s:%d in %lu ms\n",
         sent, host, port, elapsed_ms);

  return OK;
}

/****************************************************************************
 * Name: agent_camera_write_file
 ****************************************************************************/

static int agent_camera_write_file(const char *path,
                                   const uint8_t *data, size_t len)
{
  ssize_t nwritten;
  int fd;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      printf("agent_camera: open %s failed: %d\n", path, errno);
      return -errno;
    }

  nwritten = write(fd, data, len);
  close(fd);

  if (nwritten < 0 || (size_t)nwritten != len)
    {
      printf("agent_camera: write %s short (%zd of %zu): %d\n",
             path, nwritten, len, errno);
      return -EIO;
    }

  printf("agent_camera: wrote %zu bytes to %s\n", len, path);
  return OK;
}

/****************************************************************************
 * Name: agent_camera_teardown
 *
 * Description:
 *   Unwinds in tool_camera.c's order: STREAMOFF, munmap, REQBUFS(0),
 *   close.  REQBUFS(0) is what releases the PSRAM the imgdata allocator
 *   handed out; skipping it leaks a buffer heap that is larger than the
 *   whole AP kernel heap.
 *
 ****************************************************************************/

static void agent_camera_teardown(struct agent_camera_s *cam)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_requestbuffers rel;
  uint32_t i;

  if (cam->fd < 0)
    {
      return;
    }

  if (cam->streaming)
    {
      ioctl(cam->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
      cam->streaming = false;
    }

  for (i = 0; i < cam->nbuffers; i++)
    {
      if (cam->bufs[i] != NULL && cam->bufs[i] != MAP_FAILED)
        {
          munmap(cam->bufs[i], cam->buflen[i]);
          cam->bufs[i] = NULL;
        }
    }

  memset(&rel, 0, sizeof(rel));
  rel.count  = 0;
  rel.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rel.memory = V4L2_MEMORY_MMAP;
  ioctl(cam->fd, VIDIOC_REQBUFS, (uintptr_t)&rel);

  close(cam->fd);
  cam->fd = -1;
  cam->nbuffers = 0;
}

/****************************************************************************
 * Name: agent_camera_capture
 *
 * Description:
 *   The capture half of tool_camera.c's camera_v4l2_capture(), with the
 *   frame count made a parameter so a single session can be watched for
 *   more than one frame.  The tool takes exactly one.
 *
 ****************************************************************************/

static int agent_camera_capture(struct agent_camera_s *cam,
                                int width, int height,
                                unsigned int count, bool negotiate,
                                const char *out_path, bool b64,
                                bool b64_all, unsigned int rec,
                                FAR const char *session,
                                FAR const char *tcp_target)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_requestbuffers req;
  struct v4l2_capability cap;
  struct timeval t_start;
  struct timeval t_end;
  struct timespec t_session = { 0, 0 };
  unsigned long elapsed_ms;
  FAR uint8_t **rec_frames = NULL;
  FAR size_t *rec_lens = NULL;
  unsigned int rec_stored = 0;
  unsigned int rec_every = 1;
  uint32_t chosen_fmt = 0;
  unsigned int frame;
  uint32_t i;
  int ret;

  memset(&cap, 0, sizeof(cap));
  if (ioctl(cam->fd, VIDIOC_QUERYCAP, (uintptr_t)&cap) < 0)
    {
      printf("agent_camera: VIDIOC_QUERYCAP failed: %d\n", errno);
      return -errno;
    }

  if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
    {
      printf("agent_camera: device reports no capture capability "
             "(caps=0x%08" PRIx32 ")\n", (uint32_t)cap.capabilities);
      return -ENODEV;
    }

  ret = agent_camera_set_format(cam->fd, &width, &height, negotiate,
                                &chosen_fmt);
  if (ret < 0)
    {
      return ret;
    }

  printf("agent_camera: format ");
  agent_camera_print_fourcc(chosen_fmt);
  printf(" %dx%d sizeimage=%d\n", width, height, CAM_BUF_SIZE);

  memset(&req, 0, sizeof(req));
  req.count  = g_nbufs;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(cam->fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      printf("agent_camera: VIDIOC_REQBUFS failed: %d\n", errno);
      return -errno;
    }

  cam->nbuffers = req.count < CAM_MAX_BUFFERS ? req.count : CAM_MAX_BUFFERS;
  printf("agent_camera: %" PRIu32 " buffer(s) granted\n", cam->nbuffers);

  for (i = 0; i < cam->nbuffers; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(cam->fd, VIDIOC_QUERYBUF, (uintptr_t)&buf) < 0)
        {
          printf("agent_camera: VIDIOC_QUERYBUF(%" PRIu32 ") failed: %d\n",
                 i, errno);
          return -errno;
        }

      cam->buflen[i] = buf.length;
      cam->bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, cam->fd, buf.m.offset);
      if (cam->bufs[i] == MAP_FAILED)
        {
          cam->bufs[i] = NULL;
          printf("agent_camera: mmap(%" PRIu32 ", %zu) failed: %d\n",
                 i, cam->buflen[i], errno);
          return -errno;
        }

      printf("agent_camera: buffer[%" PRIu32 "] len=%zu at %p\n",
             i, cam->buflen[i], cam->bufs[i]);
    }

  for (i = 0; i < cam->nbuffers; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(cam->fd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
        {
          printf("agent_camera: VIDIOC_QBUF(%" PRIu32 ") failed: %d\n",
                 i, errno);
          return -errno;
        }
    }

  if (ioctl(cam->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("agent_camera: VIDIOC_STREAMON failed: %d\n", errno);
      return -errno;
    }

  cam->streaming = true;

  /* Wall clock across the streaming session, so the session reports the
   * frame rate it actually achieved rather than the 30fps the sensor is
   * configured for.  Note that 'b64all' prints tens of kilobytes per
   * frame down a 115200 console, which dominates everything else; take
   * the rate from a run without it.
   */

  /* Recording mode keeps the frames instead of printing them, so the
   * capture runs at the sensor's rate and the console work happens
   * afterwards.  Printing inside the loop cannot do that: one frame of
   * base64 is ~40KB, i.e. 3.5s at 115200, so every "next" frame would be
   * whatever the driver had most recently overwritten and the clip would be
   * a time lapse rather than motion.  The frames come from the heap, which
   * on this board reaches into PSRAM (6MB), so 30 x ~30KB is affordable
   * while the 300KB SRAM heap alone would not be.
   */

  if (rec > 0)
    {
      /* Keep rec frames out of the count captured, spread evenly.  Copying
       * a frame out costs more than the 33ms between frames at 640x480 --
       * both ends are non-cacheable PSRAM -- so recording every frame loses
       * the race with the DMA and most scans come back truncated (measured:
       * 2 to 11 clean out of 30).  Sampling every rec_every'th frame buys
       * that many frame times per copy, and the result is still a real
       * recording: the frames are the sensor's own, spaced by a known
       * interval, just at a lower rate than 30fps.
       */

      rec_every = count > rec ? count / rec : 1;

      rec_frames = (FAR uint8_t **)calloc(rec, sizeof(FAR uint8_t *));
      rec_lens = (FAR size_t *)calloc(rec, sizeof(size_t));
      if (rec_frames == NULL || rec_lens == NULL)
        {
          printf("agent_camera: cannot hold %u frames\n", rec);
          free(rec_frames);
          free(rec_lens);
          return -ENOMEM;
        }
    }

  gettimeofday(&t_start, NULL);

  for (frame = 0; frame < count; frame++)
    {
      struct pollfd pfd;
      struct v4l2_buffer dqbuf;
      int nready;

      pfd.fd     = cam->fd;
      pfd.events = POLLIN;

      nready = poll(&pfd, 1, CAM_TIMEOUT_MS);
      if (nready < 0)
        {
          printf("agent_camera: poll failed: %d\n", errno);
          return -errno;
        }

      if (nready == 0)
        {
          printf("agent_camera: no frame within %d ms -- the capture path "
                 "produced nothing\n", CAM_TIMEOUT_MS);
          return -ETIMEDOUT;
        }

      memset(&dqbuf, 0, sizeof(dqbuf));
      dqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      dqbuf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(cam->fd, VIDIOC_DQBUF, (uintptr_t)&dqbuf) < 0)
        {
          printf("agent_camera: VIDIOC_DQBUF failed: %d\n", errno);
          return -errno;
        }

      printf("agent_camera: frame %u: index=%" PRIu32
             " bytesused=%" PRIu32 " flags=0x%08" PRIx32 "\n",
             frame, (uint32_t)dqbuf.index, (uint32_t)dqbuf.bytesused,
             (uint32_t)dqbuf.flags);

      /* The metadata the upload path has to put on every frame: which
       * session it belongs to, a monotonically increasing sequence, and a
       * monotonic clock offset from the start of the session (spec 8.3's
       * X-Sequence / X-Timestamp-Ms).  Printed here rather than invented at
       * upload time so the numbers come from the same place the frame does.
       *
       * gettimeofday() is deliberately not used for the offset: the first TLS
       * handshake shoves the realtime clock from 1970 to 2026, which would
       * make every earlier timestamp nonsense.  clock_gettime(MONOTONIC) is
       * unaffected.
       */

      if (session != NULL)
        {
          struct timespec mono;

          clock_gettime(CLOCK_MONOTONIC, &mono);

          if (t_session.tv_sec == 0 && t_session.tv_nsec == 0)
            {
              t_session = mono;
            }

          printf("agent_camera: meta session=%s sequence=%u "
                 "timestamp_ms=%lu width=%d height=%d bytes=%" PRIu32 "\n",
                 session, frame + 1,
                 (unsigned long)((mono.tv_sec - t_session.tv_sec) * 1000ul +
                                 (mono.tv_nsec - t_session.tv_nsec) /
                                 1000000l),
                 width, height, (uint32_t)dqbuf.bytesused);
        }

      /* The driver completes a buffer with V4L2_BUF_FLAG_ERROR when its
       * watchdog fires rather than leaving DQBUF blocked forever (see
       * invariant 8 in docs/reference/camera.md).  Such a buffer carries no
       * usable frame, so say so instead of validating its contents.
       */

      if ((dqbuf.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          printf("agent_camera: buffer flagged ERROR -- capture watchdog "
                 "or encoder fault, contents not usable\n");
        }
      else if (dqbuf.bytesused == 0)
        {
          printf("agent_camera: empty frame\n");
        }
      else if (dqbuf.index < cam->nbuffers)
        {
          const uint8_t *data = cam->bufs[dqbuf.index];

          /* Get the bytes out of the capture buffer *first*, before anything
           * else touches them.  agent_camera_check_jpeg() walks the whole
           * frame looking for markers, and doing that in place -- byte at a
           * time, out of non-cacheable PSRAM -- costs more than the 33ms the
           * sensor leaves between frames, so the DMA catches up and the scan
           * we then copy is already truncated.  Measured: validating first
           * gave 11 of 29 frames a clean decode, copying first gives the
           * numbers quoted in the recording section of the docs.
           */

          if (rec > 0 && rec_stored < rec && (frame % rec_every) == 0)
            {
              FAR uint8_t *copy = (FAR uint8_t *)malloc(dqbuf.bytesused);

              if (copy == NULL)
                {
                  printf("agent_camera: out of memory at frame %u, "
                         "recording %u frame(s)\n", frame, rec_stored);
                  rec = rec_stored;
                }
              else
                {
                  agent_camera_copy32(copy, data, dqbuf.bytesused);
                  rec_frames[rec_stored] = copy;
                  rec_lens[rec_stored] = dqbuf.bytesused;
                  rec_stored++;

                  /* Validate the copy, not the live buffer. */

                  data = copy;
                }
            }

          agent_camera_check_jpeg(data, dqbuf.bytesused);

          /* Deliver the *last* frame of the session, not the first.
           *
           * This used to take frame 0, which is the one frame guaranteed to
           * be affected by the startup transient: the encoder is enabled at
           * an arbitrary phase of the continuously-streaming sensor, so the
           * driver reports err=3 resets=3 on every session (camera.md
           * §14.4).  Those frames still pass the marker checks, so nothing
           * in the log says the *contents* are wrong -- and because every
           * image ever retrieved with 'b64' or 'out=' was frame 0, the
           * spliced picture that came out was mistaken for the pipeline's
           * normal output.
           *
           * With count == 1 (the default, and what ai_agent's
           * camera_capture does) this is still frame 0.  Ask for more
           * frames to step past the transient.
           */

          /* Every frame, when asked for.  This is the interface a host-side
           * recording uses: one fenced base64 block per frame, each carrying
           * its own index and length, so a burst can be reassembled into a
           * clip without a filesystem on the board.  See the file header for
           * the extraction one-liner.
           */

          if (b64_all)
            {
              printf("agent_camera: frame %u of %u\n", frame + 1, count);
              agent_camera_print_b64(data, dqbuf.bytesused);
            }

          if (tcp_target != NULL && rec == 0 &&
              (b64_all || frame + 1 == count))
            {
              agent_camera_send_tcp(tcp_target, data, dqbuf.bytesused);
            }

          if (frame + 1 == count)
            {
              if (out_path != NULL)
                {
                  agent_camera_write_file(out_path, data, dqbuf.bytesused);
                }

              if (b64 && !b64_all)
                {
                  agent_camera_print_b64(data, dqbuf.bytesused);
                }
            }
        }

      if (ioctl(cam->fd, VIDIOC_QBUF, (uintptr_t)&dqbuf) < 0)
        {
          printf("agent_camera: re-QBUF failed: %d\n", errno);
          return -errno;
        }
    }

  gettimeofday(&t_end, NULL);
  elapsed_ms = (unsigned long)((t_end.tv_sec - t_start.tv_sec) * 1000 +
                              (t_end.tv_usec - t_start.tv_usec) / 1000);

  printf("agent_camera: session %u frame(s) in %lu ms = %lu.%02lu fps%s\n",
         count, elapsed_ms,
         elapsed_ms ? (unsigned long)count * 1000ul / elapsed_ms : 0ul,
         elapsed_ms ? (unsigned long)count * 100000ul / elapsed_ms % 100ul
                    : 0ul,
         b64_all ? " (inflated by base64 output)" : "");

  if (rec_stored > 0)
    {
      unsigned int stored;

      printf("agent_camera: recording %u frame(s), 1 in every %u captured "
             "(about %lu.%02lu fps), dumping now\n",
             rec_stored, rec_every,
             elapsed_ms ? (unsigned long)rec_stored * 1000ul / elapsed_ms
                        : 0ul,
             elapsed_ms ? (unsigned long)rec_stored * 100000ul / elapsed_ms
                          % 100ul : 0ul);

      for (stored = 0; stored < rec_stored; stored++)
        {
          printf("agent_camera: frame %u of %u\n", stored + 1, rec_stored);

          /* With a TCP target the console dump is redundant and costs
           * seconds per frame, so tcp= replaces it rather than adding to it.
           * Ask for both only by passing b64 explicitly.
           */

          if (tcp_target != NULL)
            {
              agent_camera_send_tcp(tcp_target, rec_frames[stored],
                                    rec_lens[stored]);
            }

          if (tcp_target == NULL || b64)
            {
              agent_camera_print_b64(rec_frames[stored], rec_lens[stored]);
            }

          free(rec_frames[stored]);
        }

      printf("agent_camera: recording done\n");
    }

  free(rec_frames);
  free(rec_lens);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct agent_camera_size_s sizes[MAX_ENUM_SIZES];
  struct agent_camera_s cam;
  const char *out_path = NULL;
  unsigned int count = 1;
  bool caps_only = false;
  bool auto_size = true;
  bool negotiate = true;
  bool b64 = false;
  bool b64_all = false;
  unsigned int rec = 0;
  FAR const char *session = NULL;
  FAR const char *tcp_target = NULL;
  int width = 0;
  int height = 0;
  int nsizes;
  int ret;
  int i;

  memset(&cam, 0, sizeof(cam));
  cam.fd = -1;

  for (i = 1; i < argc; i++)
    {
      const char *arg = argv[i];

      if (strcmp(arg, "auto") == 0)
        {
          auto_size = true;
        }
      else if (strcmp(arg, "low") == 0)
        {
          width     = TOOL_LOW_WIDTH;
          height    = TOOL_LOW_HEIGHT;
          auto_size = false;
        }
      else if (strcmp(arg, "high") == 0)
        {
          width     = TOOL_HIGH_WIDTH;
          height    = TOOL_HIGH_HEIGHT;
          auto_size = false;
        }
      else if (strcmp(arg, "caps") == 0)
        {
          caps_only = true;
        }
      else if (strcmp(arg, "strict") == 0)
        {
          negotiate = false;
        }
      else if (strncmp(arg, "bufs=", 5) == 0)
        {
          unsigned long v = strtoul(arg + 5, NULL, 10);

          if (v >= 1 && v <= CAM_MAX_BUFFERS)
            {
              g_nbufs = (uint32_t)v;
            }
        }
      else if (strcmp(arg, "b64") == 0)
        {
          b64 = true;
        }
      else if (strncmp(arg, "session=", 8) == 0)
        {
          session = arg + 8;
        }
      else if (strncmp(arg, "rec=", 4) == 0)
        {
          rec = (unsigned int)atoi(arg + 4);
        }
      else if (strcmp(arg, "b64all") == 0)
        {
          b64 = true;
          b64_all = true;
        }
      else if (strncmp(arg, "out=", 4) == 0)
        {
          out_path = arg + 4;
        }
      else if (strncmp(arg, "tcp=", 4) == 0)
        {
          tcp_target = arg + 4;
        }
      else if (strncmp(arg, "n=", 2) == 0)
        {
          count = (unsigned int)strtoul(arg + 2, NULL, 10);
          if (count == 0)
            {
              count = 1;
            }
        }
      else if (strchr(arg, 'x') != NULL)
        {
          char *endp;

          width = (int)strtol(arg, &endp, 10);
          if (endp == NULL || *endp != 'x')
            {
              agent_camera_usage();
              return EXIT_FAILURE;
            }

          height    = (int)strtol(endp + 1, NULL, 10);
          auto_size = false;

          if (width <= 0 || height <= 0)
            {
              agent_camera_usage();
              return EXIT_FAILURE;
            }
        }
      else
        {
          agent_camera_usage();
          return EXIT_FAILURE;
        }
    }

  cam.fd = open(CAM_DEV, O_RDWR);
  if (cam.fd < 0)
    {
      printf("agent_camera: open %s failed: %d\n", CAM_DEV, errno);
      return EXIT_FAILURE;
    }

  nsizes = agent_camera_enum(cam.fd, V4L2_PIX_FMT_JPEG, sizes,
                             MAX_ENUM_SIZES, true);
  if (nsizes < 0)
    {
      close(cam.fd);
      return EXIT_FAILURE;
    }

  printf("agent_camera: driver enumerates %d JPEG size(s)\n", nsizes);

  if (caps_only)
    {
      close(cam.fd);
      return EXIT_SUCCESS;
    }

  if (auto_size)
    {
      if (nsizes == 0)
        {
          printf("agent_camera: no JPEG size enumerated -- is the JPEG "
                 "format registered by the imgsensor driver?\n");
          close(cam.fd);
          return EXIT_FAILURE;
        }

      width  = sizes[0].width;
      height = sizes[0].height;
      printf("agent_camera: auto-selected %dx%d\n", width, height);
    }
  else
    {
      bool supported = false;

      for (i = 0; i < nsizes; i++)
        {
          if (sizes[i].width == width && sizes[i].height == height)
            {
              supported = true;
              break;
            }
        }

      if (!supported)
        {
          /* Not fatal on purpose: the point of `low` and `high` is to show
           * what tool_camera.c's hardcoded request does on this board, and
           * that means letting the request reach VIDIOC_S_FMT.
           */

          printf("agent_camera: %dx%d is not enumerated by this driver; "
                 "requesting it anyway (%s)\n",
                 width, height,
                 negotiate ? "will negotiate if refused" :
                             "strict: no negotiation");
        }
    }

  ret = agent_camera_capture(&cam, width, height, count, negotiate,
                             out_path, b64, b64_all, rec, session,
                             tcp_target);
  agent_camera_teardown(&cam);

  if (ret < 0)
    {
      printf("agent_camera: FAILED (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("agent_camera: OK\n");
  return EXIT_SUCCESS;
}
