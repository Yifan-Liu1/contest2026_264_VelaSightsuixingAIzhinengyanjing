/****************************************************************************
 * app/web_tool/wt_command.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/boardctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <nuttx/video/video.h>

#include <netutils/cJSON.h>
#include <netutils/netlib.h>

#include <arch/board/kvdb.h>

#include "wt_command.h"
#include "wt_protocol.h"
#include "wt_queue.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WT_CAM_DEV        "/dev/video0"
#define WT_CAM_NBUFS      2

/* Same sizeimage hint the ai_agent camera tool uses.  For a compressed format
 * this -- not width*height -- is what the framework's get_bufsize() takes out
 * of PSRAM per buffer, so changing it changes the allocation, not the picture.
 */

#define WT_CAM_BUF_SIZE   (160 * 1024)
#define WT_CAM_TIMEOUT_MS 5000

#define WT_IFNAME         "wlan0"

/* How long to wait for queue space for the exit notice.  It must not be
 * dropped, but it must also not wedge the shell relay for ever if the link is
 * gone; five seconds is the same budget the session uses for a response.
 */

#define WT_SHELL_EXIT_TIMEOUT_MS  5000

/* kvdb values are capped at 512 bytes by bk7258_kvdb_get()'s callers. */

#define WT_KVDB_VAL_MAX   513

#ifndef V4L2_PIX_FMT_ENTROPY
#  define V4L2_PIX_FMT_ENTROPY  v4l2_fourcc('G', 'R', 'E', 'P')
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Growable text buffer.  Responses range from "{}" to a kvdb listing, and a
 * fixed buffer would either waste the common case or truncate the listing --
 * and a truncated JSON response is indistinguishable, at the far end, from a
 * link that dropped bytes.
 */

struct wt_sb_s
{
  char   *p;
  size_t  len;
  size_t  cap;
  bool    failed;
};

struct wt_errname_s
{
  int         err;
  const char *name;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The errnos this service can actually produce.  The front end shows the
 * name; a bare -2 tells an operator nothing, ENOENT tells them the key is not
 * set.
 */

static const struct wt_errname_s g_errnames[] =
{
  { EPERM,        "EPERM"        },
  { ENOENT,       "ENOENT"       },
  { EINTR,        "EINTR"        },
  { EIO,          "EIO"          },
  { EAGAIN,       "EAGAIN"       },
  { ENOMEM,       "ENOMEM"       },
  { EACCES,       "EACCES"       },
  { EBUSY,        "EBUSY"        },
  { EEXIST,       "EEXIST"       },
  { ENODEV,       "ENODEV"       },
  { EINVAL,       "EINVAL"       },
  { ENOSPC,       "ENOSPC"       },
  { EPIPE,        "EPIPE"        },
  { ERANGE,       "ERANGE"       },
  { ENOSYS,       "ENOSYS"       },
  { ENOTCONN,     "ENOTCONN"     },
  { ETIMEDOUT,    "ETIMEDOUT"    },
  { ECONNREFUSED, "ECONNREFUSED" },
  { ENETUNREACH,  "ENETUNREACH"  },
  { ENETDOWN,     "ENETDOWN"     },
  { E2BIG,        "E2BIG"        },
  { ESHUTDOWN,    "ESHUTDOWN"    },
};

/* Geometries bk7258_camera_imgsensor.c programs, matched exactly. */

static const struct
{
  int w;
  int h;
}
g_cam_sizes[] =
{
  { 480, 480 },
  { 640, 480 },
  { 864, 480 },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static const char *wt_errname(int err)
{
  size_t i;

  if (err < 0)
    {
      err = -err;
    }

  for (i = 0; i < sizeof(g_errnames) / sizeof(g_errnames[0]); i++)
    {
      if (g_errnames[i].err == err)
        {
          return g_errnames[i].name;
        }
    }

  return "EUNKNOWN";
}

/* ---- Growable buffer -------------------------------------------------- */

static bool wt_sb_init(struct wt_sb_s *sb, size_t cap)
{
  sb->p = malloc(cap);
  sb->len = 0;
  sb->cap = cap;
  sb->failed = sb->p == NULL;
  if (sb->p != NULL)
    {
      sb->p[0] = '\0';
    }

  return !sb->failed;
}

static bool wt_sb_reserve(struct wt_sb_s *sb, size_t extra)
{
  size_t need = sb->len + extra + 1;
  char *np;

  if (sb->failed)
    {
      return false;
    }

  if (need <= sb->cap)
    {
      return true;
    }

  while (sb->cap < need)
    {
      sb->cap *= 2;
    }

  np = realloc(sb->p, sb->cap);
  if (np == NULL)
    {
      sb->failed = true;
      return false;
    }

  sb->p = np;
  return true;
}

static void wt_sb_addstr(struct wt_sb_s *sb, const char *s)
{
  size_t n = strlen(s);

  if (!wt_sb_reserve(sb, n))
    {
      return;
    }

  memcpy(sb->p + sb->len, s, n + 1);
  sb->len += n;
}

static void wt_sb_addf(struct wt_sb_s *sb, const char *fmt, ...)
{
  va_list ap;
  int n;

  if (sb->failed)
    {
      return;
    }

  /* Measure, then write.  Two passes rather than a guess, because the values
   * here include kvdb entries whose length is not bounded by anything local.
   */

  va_start(ap, fmt);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (n < 0 || !wt_sb_reserve(sb, (size_t)n))
    {
      sb->failed = true;
      return;
    }

  va_start(ap, fmt);
  vsnprintf(sb->p + sb->len, sb->cap - sb->len, fmt, ap);
  va_end(ap);
  sb->len += (size_t)n;
}

/* Escape in into out as a JSON string body (no surrounding quotes).  Returns
 * the number of bytes written, never more than outcap-1, and always
 * NUL-terminates.  Allocation free: log lines come through here several times
 * a second and building a cJSON object per line would be the only malloc in
 * the whole streaming path.
 */

static size_t wt_esc_into(char *out, size_t outcap, const char *in)
{
  const unsigned char *p = (const unsigned char *)in;
  size_t pos = 0;

  if (outcap == 0)
    {
      return 0;
    }

  for (; *p != '\0'; p++)
    {
      char tmp[8];
      const char *rep = tmp;
      size_t replen;

      switch (*p)
        {
          case '"':
            rep = "\\\"";
            replen = 2;
            break;

          case '\\':
            rep = "\\\\";
            replen = 2;
            break;

          case '\n':
            rep = "\\n";
            replen = 2;
            break;

          case '\r':
            rep = "\\r";
            replen = 2;
            break;

          case '\t':
            rep = "\\t";
            replen = 2;
            break;

          default:
            if (*p < 0x20 || *p == 0x7f)
              {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)*p);
                replen = strlen(tmp);
              }
            else
              {
                tmp[0] = (char)*p;
                replen = 1;
              }

            break;
        }

      if (pos + replen > outcap - 1)
        {
          break;                /* truncate rather than overflow */
        }

      memcpy(out + pos, rep, replen);
      pos += replen;
    }

  out[pos] = '\0';
  return pos;
}

static void wt_sb_addesc(struct wt_sb_s *sb, const char *s)
{
  /* Worst case is six bytes out per byte in (\u00xx), so reserving that up
   * front means one pass and no reallocation inside the loop.
   */

  size_t worst = strlen(s) * 6;

  if (!wt_sb_reserve(sb, worst))
    {
      return;
    }

  sb->len += wt_esc_into(sb->p + sb->len, sb->cap - sb->len, s);
}

/****************************************************************************
 * Public Functions: small helpers
 ****************************************************************************/

uint32_t wt_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

size_t wt_json_escape(char *out, size_t outcap, const char *in)
{
  return wt_esc_into(out, outcap, in);
}

int wt_json_log(char *out, size_t outcap, uint32_t t_ms, const char *line)
{
  size_t pos;
  int n;

  n = snprintf(out, outcap, "{\"t\":%lu,\"line\":\"", (unsigned long)t_ms);
  if (n < 0 || (size_t)n >= outcap)
    {
      return -E2BIG;
    }

  pos = (size_t)n;
  pos += wt_esc_into(out + pos, outcap - pos, line);

  n = snprintf(out + pos, outcap - pos, "\"}");
  if (n < 0 || (size_t)n >= outcap - pos)
    {
      return -E2BIG;
    }

  return (int)(pos + (size_t)n);
}

int wt_json_dropped(char *out, size_t outcap, uint32_t t_ms, uint32_t n)
{
  int len = snprintf(out, outcap, "{\"t\":%lu,\"dropped\":%lu}",
                     (unsigned long)t_ms, (unsigned long)n);

  return len < 0 || (size_t)len >= outcap ? -E2BIG : len;
}

int wt_json_exit(char *out, size_t outcap, uint32_t t_ms, int status,
                 bool known)
{
  int len;

  if (known)
    {
      len = snprintf(out, outcap, "{\"t\":%lu,\"exit\":%d}",
                     (unsigned long)t_ms, status);
    }
  else
    {
      len = snprintf(out, outcap,
                     "{\"t\":%lu,\"exit\":null,\"exit_unknown\":true}",
                     (unsigned long)t_ms);
    }

  return len < 0 || (size_t)len >= outcap ? -E2BIG : len;
}

bool wt_kvdb_is_secret(const char *key)
{
  size_t len = strlen(key);

  return len >= 4 && (strcmp(key + len - 4, ".key") == 0 ||
                      strcmp(key + len - 4, ".psk") == 0);
}

bool wt_mask_value(char *out, size_t outcap, const char *key,
                   const char *value, bool raw)
{
  size_t len = strlen(value);

  if (raw || !wt_kvdb_is_secret(key) || len == 0)
    {
      strlcpy(out, value, outcap);
      return false;
    }

  if (len <= 8)
    {
      snprintf(out, outcap, "**** (%zu bytes)", len);
    }
  else
    {
      snprintf(out, outcap, "%.4s...%s (%zu bytes)", value,
               value + len - 4, len);
    }

  return true;
}

bool wt_camera_geometry_ok(int width, int height)
{
  size_t i;

  for (i = 0; i < sizeof(g_cam_sizes) / sizeof(g_cam_sizes[0]); i++)
    {
      if (g_cam_sizes[i].w == width && g_cam_sizes[i].h == height)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Private Functions: response shapes
 ****************************************************************************/

static char *wt_ok_raw(const char *data_json)
{
  struct wt_sb_s sb;

  if (!wt_sb_init(&sb, 128))
    {
      return NULL;
    }

  wt_sb_addf(&sb, "{\"ok\":true,\"data\":%s}", data_json);
  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

static char *wt_fail(const char *msg, int err)
{
  struct wt_sb_s sb;

  if (err > 0)
    {
      err = -err;
    }

  if (!wt_sb_init(&sb, 160))
    {
      return NULL;
    }

  wt_sb_addstr(&sb, "{\"ok\":false,\"err\":\"");
  wt_sb_addesc(&sb, msg);
  wt_sb_addf(&sb, "\",\"errno\":%d,\"errname\":\"%s\"}", err,
             wt_errname(err));

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

/****************************************************************************
 * Private Functions: kvdb
 ****************************************************************************/

struct wt_kvdb_list_s
{
  struct wt_sb_s *sb;
  bool            raw;
  int             count;
};

static void wt_kvdb_list_one(const char *key, const char *value, void *arg)
{
  struct wt_kvdb_list_s *st = arg;
  char *shown = malloc(WT_KVDB_VAL_MAX + 64);
  bool masked;

  if (shown == NULL)
    {
      st->sb->failed = true;
      return;
    }

  masked = wt_mask_value(shown, WT_KVDB_VAL_MAX + 64, key, value, st->raw);

  if (st->count > 0)
    {
      wt_sb_addstr(st->sb, ",");
    }

  wt_sb_addstr(st->sb, "{\"key\":\"");
  wt_sb_addesc(st->sb, key);
  wt_sb_addstr(st->sb, "\",\"value\":\"");
  wt_sb_addesc(st->sb, shown);
  wt_sb_addf(st->sb, "\",\"masked\":%s}", masked ? "true" : "false");

  st->count++;
  free(shown);
}

static char *wt_cmd_kvdb_list(bool raw)
{
  struct wt_sb_s sb;
  struct wt_kvdb_list_s st;
  int ret;

  if (!wt_sb_init(&sb, 512))
    {
      return NULL;
    }

  st.sb = &sb;
  st.raw = raw;
  st.count = 0;

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":{\"items\":[");
  ret = bk7258_kvdb_foreach(wt_kvdb_list_one, &st);
  wt_sb_addf(&sb, "],\"persistent\":%s}}",
             bk7258_kvdb_persistent() ? "true" : "false");

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  if (ret < 0)
    {
      free(sb.p);
      return wt_fail("kvdb: list failed", ret);
    }

  return sb.p;
}

static char *wt_cmd_kvdb_get(const char *key, bool raw)
{
  char value[WT_KVDB_VAL_MAX];
  char *shown;
  struct wt_sb_s sb;
  bool masked;
  int ret;

  ret = bk7258_kvdb_get(key, value, sizeof(value));
  if (ret == -ENOENT)
    {
      return wt_fail("kvdb: key not found", -ENOENT);
    }

  if (ret < 0)
    {
      return wt_fail("kvdb: get failed", ret);
    }

  shown = malloc(WT_KVDB_VAL_MAX + 64);
  if (shown == NULL)
    {
      return NULL;
    }

  masked = wt_mask_value(shown, WT_KVDB_VAL_MAX + 64, key, value, raw);

  if (!wt_sb_init(&sb, 256))
    {
      free(shown);
      return NULL;
    }

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":{\"key\":\"");
  wt_sb_addesc(&sb, key);
  wt_sb_addstr(&sb, "\",\"value\":\"");
  wt_sb_addesc(&sb, shown);
  wt_sb_addf(&sb, "\",\"masked\":%s}}", masked ? "true" : "false");
  free(shown);

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

static char *wt_cmd_kvdb_set(const char *key, const char *value)
{
  char data[64];
  int ret = bk7258_kvdb_set(key, value);

  if (ret < 0)
    {
      return wt_fail("kvdb: set failed", ret);
    }

  /* The front end has to be able to tell "saved" from "saved until reset":
   * with the flash backend unavailable the value is still usable now, and
   * still gone after a reboot.
   */

  snprintf(data, sizeof(data), "{\"persistent\":%s}",
           bk7258_kvdb_persistent() ? "true" : "false");
  return wt_ok_raw(data);
}

static char *wt_cmd_kvdb_del(const char *key)
{
  int ret = bk7258_kvdb_del(key);

  if (ret < 0)
    {
      return wt_fail("kvdb: del failed", ret);
    }

  return wt_ok_raw("{}");
}

/****************************************************************************
 * Private Functions: wifi
 ****************************************************************************/

/* "Really up" is IFF_RUNNING plus an address that is not NuttX's default
 * static 10.0.0.2 -- see docs/WiFi使用说明.md.  Neither ping (this network
 * blocks ICMP) nor the agent's own "Network connected: yes" (true even with
 * the interface down) can be used for this.
 */

static void wt_wifi_state(struct wt_sb_s *sb)
{
  struct in_addr addr;
  struct in_addr mask;
  struct in_addr gw;
  char essid[36];
  uint8_t flags = 0;
  bool running;

  memset(&addr, 0, sizeof(addr));
  memset(&mask, 0, sizeof(mask));
  memset(&gw, 0, sizeof(gw));
  essid[0] = '\0';

  netlib_getifstatus(WT_IFNAME, &flags);
  netlib_get_ipv4addr(WT_IFNAME, &addr);
  netlib_get_ipv4netmask(WT_IFNAME, &mask);
  netlib_get_dripv4addr(WT_IFNAME, &gw);
  netlib_getessid(WT_IFNAME, essid, sizeof(essid));

  running = (flags & IFF_RUNNING) != 0 &&
            addr.s_addr != htonl(0x0a000002);

  wt_sb_addf(sb, "{\"running\":%s,\"flags\":%u,\"ssid\":\"",
             running ? "true" : "false", (unsigned int)flags);
  wt_sb_addesc(sb, essid);
  wt_sb_addf(sb, "\",\"ip\":\"%s\"", inet_ntoa(addr));
  wt_sb_addf(sb, ",\"netmask\":\"%s\"", inet_ntoa(mask));
  wt_sb_addf(sb, ",\"gw\":\"%s\"}", inet_ntoa(gw));
}

static char *wt_cmd_wifi_status(void)
{
  struct wt_sb_s sb;

  if (!wt_sb_init(&sb, 256))
    {
      return NULL;
    }

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":");
  wt_wifi_state(&sb);
  wt_sb_addstr(&sb, "}");

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

static char *wt_cmd_wifi_connect(struct wt_ctx_s *ctx, const char *ssid,
                                 const char *psk)
{
  struct wt_sb_s sb;
  int ret;

  if (ssid == NULL || ssid[0] == '\0')
    {
      return wt_fail("wifi.connect: ssid is required", -EINVAL);
    }

  if (strlen(ssid) >= sizeof(ctx->wifi_ssid) ||
      (psk != NULL && strlen(psk) >= sizeof(ctx->wifi_psk)))
    {
      return wt_fail("wifi.connect: ssid or passphrase too long", -E2BIG);
    }

  /* Store first, so a reboot keeps whatever was just made to work.  The board
   * applies these at boot (bk7258_kvdb_apply_wifi), which is the whole point
   * of having them in kvdb.  Storing does not touch the network, so it is safe
   * to do before answering.
   */

  ret = bk7258_kvdb_set(BK7258_KVDB_KEY_WIFI_SSID, ssid);
  if (ret < 0)
    {
      return wt_fail("wifi.connect: could not store ssid", ret);
    }

  if (psk != NULL && psk[0] != '\0')
    {
      ret = bk7258_kvdb_set(BK7258_KVDB_KEY_WIFI_PSK, psk);
    }
  else
    {
      ret = bk7258_kvdb_del(BK7258_KVDB_KEY_WIFI_PSK);
      if (ret == -ENOENT)
        {
          ret = 0;              /* open network, nothing stored: fine */
        }
    }

  if (ret < 0)
    {
      return wt_fail("wifi.connect: could not store passphrase", ret);
    }

  /* Hand the association to the sender, to be done once this response is out.
   * Doing it here would drop the connection the response has to travel on.
   */

  strlcpy(ctx->wifi_ssid, ssid, sizeof(ctx->wifi_ssid));
  strlcpy(ctx->wifi_psk, psk != NULL ? psk : "", sizeof(ctx->wifi_psk));
  ctx->wifi_pending = true;

  if (!wt_sb_init(&sb, 256))
    {
      return NULL;
    }

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":{\"applying\":true,\"ssid\":\"");
  wt_sb_addesc(&sb, ssid);
  wt_sb_addf(&sb, "\",\"persistent\":%s,\"note\":\"%s\",",
             bk7258_kvdb_persistent() ? "true" : "false",
             "re-associating drops this connection; the board dials back in");
  wt_sb_addstr(&sb, "\"before\":");
  wt_wifi_state(&sb);
  wt_sb_addstr(&sb, "}}");

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

/****************************************************************************
 * Name: wt_wifi_apply_pending
 *
 * Description:
 *   Associate and ask for an address, using what wifi.connect stored.  Called
 *   from the sender once the response has left the socket -- see the comment on
 *   wifi_pending in wt_command.h.
 *
 ****************************************************************************/

void wt_wifi_apply_pending(struct wt_ctx_s *ctx)
{
  int attempt;
  int ret;

  ctx->wifi_pending = false;

  syslog(LOG_INFO, "web_tool: associating with %s\n", ctx->wifi_ssid);

  ret = bk7258_kvdb_apply_wifi();
  if (ret < 0)
    {
      syslog(LOG_ERR, "web_tool: association failed (%d)\n", ret);
      return;
    }

  /* DHCP, with one retry.  The first request after association is measured to
   * fail -- there is a window between "associated" and "can exchange DHCP" --
   * and treating that first failure as the answer is how scripted setup ends
   * up reporting a working network as broken.
   */

  for (attempt = 0; attempt < 2; attempt++)
    {
      ret = netlib_obtain_ipv4addr(WT_IFNAME);
      if (ret >= 0)
        {
          syslog(LOG_INFO, "web_tool: address obtained on attempt %d\n",
                 attempt + 1);
          return;
        }

      sleep(1);
    }

  syslog(LOG_WARNING, "web_tool: no address after 2 attempts\n");
}

/****************************************************************************
 * Private Functions: camera
 ****************************************************************************/

struct wt_cam_s
{
  int      fd;
  uint32_t nbuffers;
  void    *bufs[WT_CAM_NBUFS];
  size_t   buflen[WT_CAM_NBUFS];
  bool     streaming;
};

static int wt_cam_try_format(int fd, int width, int height, uint32_t pixfmt)
{
  struct v4l2_format fmt;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = (uint16_t)width;
  fmt.fmt.pix.height      = (uint16_t)height;
  fmt.fmt.pix.pixelformat = pixfmt;
  fmt.fmt.pix.sizeimage   = WT_CAM_BUF_SIZE;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;

  return ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt);
}

static void wt_cam_teardown(struct wt_cam_s *cam)
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
      if (cam->bufs[i] != NULL)
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

static int wt_cam_setup(struct wt_cam_s *cam, int width, int height)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_requestbuffers req;
  uint32_t i;

  memset(cam, 0, sizeof(*cam));
  cam->fd = open(WT_CAM_DEV, O_RDWR);
  if (cam->fd < 0)
    {
      return -errno;
    }

  /* JPEG first, then the driver's ENTROPY fallback: same order the ai_agent
   * camera tool uses, so a failure here means the same thing it means there.
   */

  if (wt_cam_try_format(cam->fd, width, height, V4L2_PIX_FMT_JPEG) < 0 &&
      wt_cam_try_format(cam->fd, width, height, V4L2_PIX_FMT_ENTROPY) < 0)
    {
      int err = -errno;

      close(cam->fd);
      cam->fd = -1;
      return err;
    }

  memset(&req, 0, sizeof(req));
  req.count  = WT_CAM_NBUFS;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(cam->fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      int err = -errno;

      close(cam->fd);
      cam->fd = -1;
      return err;
    }

  cam->nbuffers = req.count < WT_CAM_NBUFS ? req.count : WT_CAM_NBUFS;

  for (i = 0; i < cam->nbuffers; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(cam->fd, VIDIOC_QUERYBUF, (uintptr_t)&buf) < 0)
        {
          int err = -errno;

          wt_cam_teardown(cam);
          return err;
        }

      cam->buflen[i] = buf.length;
      cam->bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, cam->fd, buf.m.offset);
      if (cam->bufs[i] == MAP_FAILED)
        {
          int err = -errno;

          cam->bufs[i] = NULL;
          wt_cam_teardown(cam);
          return err;
        }
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
          int err = -errno;

          wt_cam_teardown(cam);
          return err;
        }
    }

  if (ioctl(cam->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      int err = -errno;

      wt_cam_teardown(cam);
      return err;
    }

  cam->streaming = true;
  return 0;
}

/* Producer.  Does not pace itself: the driver delivers at
 * CONFIG_BK7258_CAMERA_JPEG_FPS (5) and already drops the oldest unprocessed
 * frame.  A second rate limiter here would mean two drop policies fighting
 * each other, and the observed frame rate would stop being attributable.
 */

static void *wt_cam_thread(void *arg)
{
  struct wt_ctx_s *ctx = arg;
  struct wt_cam_s cam;
  uint32_t seq = 0;
  int ret;

  ret = wt_cam_setup(&cam, ctx->cam_width, ctx->cam_height);
  if (ret < 0)
    {
      syslog(LOG_ERR, "web_tool: camera setup failed: %d\n", ret);
      ctx->cam_running = false;
      return NULL;
    }

  syslog(LOG_INFO, "web_tool: camera streaming %dx%d\n",
         ctx->cam_width, ctx->cam_height);

  while (!ctx->cam_stop)
    {
      struct v4l2_buffer dq;
      struct pollfd pfd;
      uint8_t *payload;
      size_t jpeglen;
      int nready;

      pfd.fd = cam.fd;
      pfd.events = POLLIN;
      pfd.revents = 0;

      nready = poll(&pfd, 1, WT_CAM_TIMEOUT_MS);
      if (nready == 0)
        {
          syslog(LOG_WARNING, "web_tool: camera poll timeout\n");
          continue;
        }

      if (nready < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          syslog(LOG_ERR, "web_tool: camera poll failed: %d\n", errno);
          break;
        }

      memset(&dq, 0, sizeof(dq));
      dq.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      dq.memory = V4L2_MEMORY_MMAP;

      if (ioctl(cam.fd, VIDIOC_DQBUF, (uintptr_t)&dq) < 0)
        {
          syslog(LOG_ERR, "web_tool: DQBUF failed: %d\n", errno);
          break;
        }

      jpeglen = dq.bytesused;

      if (jpeglen == 0 ||
          jpeglen > WT_MAX_PAYLOAD - WT_FRAME_META_LEN)
        {
          /* Over the limit would force the connection down at the far end.
           * Dropping the frame and saying so keeps the link and still leaves
           * evidence, which is the opposite trade from a malformed header.
           */

          syslog(LOG_WARNING, "web_tool: frame %lu is %zu bytes, skipped\n",
                 (unsigned long)seq, jpeglen);
          ioctl(cam.fd, VIDIOC_QBUF, (uintptr_t)&dq);
          continue;
        }

      payload = malloc(WT_FRAME_META_LEN + jpeglen);
      if (payload == NULL)
        {
          ioctl(cam.fd, VIDIOC_QBUF, (uintptr_t)&dq);
          continue;
        }

      memcpy(payload + WT_FRAME_META_LEN,
             cam.bufs[dq.index], jpeglen);

      /* Requeue before hashing and enqueuing: the buffer is the driver's DMA
       * target and holding it while doing work costs a frame at 5 fps.
       */

      ioctl(cam.fd, VIDIOC_QBUF, (uintptr_t)&dq);

      wt_frame_meta_encode(payload, WT_FRAME_META_LEN, seq,
                           wt_fnv1a(payload + WT_FRAME_META_LEN, jpeglen));

      if (wt_queue_put(ctx->queue, WT_TYPE_EVT_FRAME, 0, payload,
                       WT_FRAME_META_LEN + jpeglen, 0) < 0)
        {
          break;                /* queue closed: connection went away */
        }

      seq++;
    }

  wt_cam_teardown(&cam);
  ctx->cam_running = false;
  syslog(LOG_INFO, "web_tool: camera stopped after %lu frame(s)\n",
         (unsigned long)seq);
  return NULL;
}

static char *wt_cmd_camera_start(struct wt_ctx_s *ctx, int width, int height)
{
  pthread_attr_t attr;
  struct sched_param sp;
  int ret;

  if (ctx->cam_running)
    {
      return wt_fail("camera already streaming", -EBUSY);
    }

  if (!wt_camera_geometry_ok(width, height))
    {
      return wt_fail("camera: size must be 480x480, 640x480 or 864x480",
                     -EINVAL);
    }

  ctx->cam_width  = width;
  ctx->cam_height = height;
  ctx->cam_stop   = false;
  ctx->cam_running = true;
  wt_queue_frame_reset(ctx->queue);

  pthread_attr_init(&attr);

  /* 8 KB, not the 4 KB default: this thread runs the V4L2 ioctl sequence and
   * syslog formatting, and the first board run showed how a marginal stack
   * here presents itself -- as garbage on the wire, not as a stack report.
   */

  pthread_attr_setstacksize(&attr, 8192);

  /* Above the low-priority work queue, which is where the software JPEG
   * encoder runs (CONFIG_SCHED_LPWORKPRIORITY=100).
   *
   * This was 70, chosen from a comment that said the encoder ran at 80.  It
   * does not -- it runs at 100 -- so the grabber sat *below* the encoder and
   * below the sender thread, and on this board that stalled the preview
   * outright: the driver hands out a raw frame only after the application
   * queues a buffer back, the grabber never got scheduled to do so, and
   * streaming stopped after three frames with `camera poll timeout` repeating.
   * The same geometry through `agent_camera`, which runs at the default 100,
   * streamed fine -- which is what made it look like a driver fault.
   *
   * 110 keeps it above LPWORK so requeueing is never starved, and well below
   * HPWORK (224) so it cannot preempt the capture interrupt path.
   */

  sp.sched_priority = 110;
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

  ret = pthread_create(&ctx->cam_thread, &attr, wt_cam_thread, ctx);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      ctx->cam_running = false;
      return wt_fail("camera: could not start thread", -ret);
    }

  return wt_ok_raw("{}");
}

static char *wt_cmd_camera_stop(struct wt_ctx_s *ctx)
{
  char data[96];
  uint32_t sent = 0;
  uint32_t dropped = 0;

  if (ctx->cam_running)
    {
      ctx->cam_stop = true;
      pthread_join(ctx->cam_thread, NULL);
    }

  wt_queue_frame_stats(ctx->queue, &sent, &dropped);
  snprintf(data, sizeof(data),
           "{\"frames_sent\":%lu,\"frames_dropped\":%lu}",
           (unsigned long)sent, (unsigned long)dropped);
  return wt_ok_raw(data);
}

/****************************************************************************
 * Private Functions: log subscription
 ****************************************************************************/

static char *wt_cmd_log_subscribe(struct wt_ctx_s *ctx, bool on)
{
  char data[64];
  int replayed = 0;

  if (on && !ctx->log_on)
    {
      /* Replay what the ring already holds, in order, before switching to
       * live.  The ring has been filling since start-up regardless of
       * subscription, which is what lets the host see the boot messages that
       * happened before it connected.
       */

      char *line = malloc(WT_LOG_LINE_MAX);
      char *body = malloc(WT_LOG_BODY_MAX);

      if (line == NULL || body == NULL)
        {
          free(line);
          free(body);
          return NULL;
        }

      while (wt_logring_getline(ctx->logring, line, WT_LOG_LINE_MAX) > 0)
        {
          int n = wt_json_log(body, WT_LOG_BODY_MAX, wt_now_ms(), line);
          uint8_t *copy;

          if (n <= 0)
            {
              continue;
            }

          copy = malloc((size_t)n);
          if (copy == NULL)
            {
              break;
            }

          memcpy(copy, body, (size_t)n);
          if (wt_queue_put(ctx->queue, WT_TYPE_EVT_LOG, 0, copy,
                           (size_t)n, 0) != WT_PUT_OK)
            {
              /* The queue is only as deep as the link is fast; the rest of
               * the backlog is reported as dropped, which is honest, rather
               * than blocking the command that asked for it.
               */

              break;
            }

          replayed++;
        }

      free(line);
      free(body);
    }

  ctx->log_on = on;
  snprintf(data, sizeof(data), "{\"replayed\":%d}", replayed);
  return wt_ok_raw(data);
}

/****************************************************************************
 * Private Functions: sys
 ****************************************************************************/

/* /proc/meminfo is the same table `free` prints (nsh_mmcmds.c just cats it),
 * so parsing it here gives numbers an operator can cross-check by hand.
 */

static void wt_sys_heaps(struct wt_sb_s *sb)
{
  FILE *f = fopen("/proc/meminfo", "r");
  char line[160];
  int n = 0;

  wt_sb_addstr(sb, "[");

  if (f == NULL)
    {
      wt_sb_addstr(sb, "]");
      return;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      unsigned long total;
      unsigned long used;
      unsigned long freeb;
      unsigned long maxused;
      unsigned long maxfree;
      unsigned long nused;
      unsigned long nfree;
      char name[32];

      if (sscanf(line, "%lu %lu %lu %lu %lu %lu %lu %31s",
                 &total, &used, &freeb, &maxused, &maxfree,
                 &nused, &nfree, name) != 8)
        {
          continue;             /* the header row */
        }

      wt_sb_addf(sb, "%s{\"name\":\"", n > 0 ? "," : "");
      wt_sb_addesc(sb, name);
      wt_sb_addf(sb, "\",\"total\":%lu,\"used\":%lu,\"free\":%lu,"
                     "\"maxfree\":%lu}", total, used, freeb, maxfree);
      n++;
    }

  fclose(f);
  wt_sb_addstr(sb, "]");
}

static int wt_sys_tasks(void)
{
  DIR *dir = opendir("/proc");
  struct dirent *ent;
  int n = 0;

  if (dir == NULL)
    {
      return -1;
    }

  while ((ent = readdir(dir)) != NULL)
    {
      if (ent->d_name[0] >= '0' && ent->d_name[0] <= '9')
        {
          n++;
        }
    }

  closedir(dir);
  return n;
}

static double wt_sys_uptime(void)
{
  FILE *f = fopen("/proc/uptime", "r");
  double up = 0.0;

  if (f != NULL)
    {
      if (fscanf(f, "%lf", &up) != 1)
        {
          up = 0.0;
        }

      fclose(f);
    }

  return up;
}

static char *wt_cmd_sys_status(struct wt_ctx_s *ctx)
{
  struct wt_sb_s sb;

  if (!wt_sb_init(&sb, 768))
    {
      return NULL;
    }

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":{\"heaps\":");
  wt_sys_heaps(&sb);
  wt_sb_addf(&sb, ",\"tasks\":%d,\"uptime\":%.2f", wt_sys_tasks(),
             wt_sys_uptime());
  wt_sb_addf(&sb, ",\"log_buffered\":%zu",
             wt_logring_used(ctx->logring));
  wt_sb_addf(&sb, ",\"camera\":%s", ctx->cam_running ? "true" : "false");
  wt_sb_addf(&sb, ",\"shell\":%s", ctx->shell_running ? "true" : "false");
  wt_sb_addstr(&sb, "}}");

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

static char *wt_cmd_sys_reboot(struct wt_ctx_s *ctx)
{
  /* Answer first, reboot after the response has left the socket.  Doing it
   * the other way round would make this the one command the front end has to
   * treat specially, and "no reply" would become indistinguishable from a
   * crash.
   */

  ctx->reboot_pending = true;
  return wt_ok_raw("{}");
}

/****************************************************************************
 * Private Functions: shell passthrough
 ****************************************************************************/

static void *wt_shell_thread(void *arg)
{
  struct wt_ctx_s *ctx = arg;
  char cmd[WT_SHELL_CMD_MAX];
  char *line = malloc(WT_LOG_LINE_MAX);
  char *body = malloc(WT_LOG_BODY_MAX);
  FILE *fp;
  int status = -1;
  bool status_known = false;

  if (line == NULL || body == NULL)
    {
      free(line);
      free(body);
      ctx->shell_running = false;
      return NULL;
    }

  pthread_mutex_lock(&ctx->shell_lock);
  strlcpy(cmd, ctx->shell_cmd, sizeof(cmd));
  pthread_mutex_unlock(&ctx->shell_lock);

  /* popen() rather than posix_spawn() plus dup2(): CONFIG_SYSTEM_POPEN is
   * already in this configuration and it does the fork/redirect/reap dance
   * that section 13.4 of the design left open, with a FILE * to read from.
   * Nothing here parses the output -- that is the point of passthrough.
   */

  fp = popen(cmd, "r");
  if (fp == NULL)
    {
      int n = wt_json_log(body, WT_LOG_BODY_MAX, wt_now_ms(),
                          "web_tool: popen failed");

      if (n > 0)
        {
          uint8_t *copy = malloc((size_t)n);

          if (copy != NULL)
            {
              memcpy(copy, body, (size_t)n);
              wt_queue_put(ctx->queue, WT_TYPE_EVT_LOG, 0, copy,
                           (size_t)n, 0);
            }
        }
    }
  else
    {
      while (!ctx->shell_kill &&
             fgets(line, WT_LOG_LINE_MAX, fp) != NULL)
        {
          size_t len = strlen(line);
          int n;

          while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            {
              line[--len] = '\0';
            }

          n = wt_json_log(body, WT_LOG_BODY_MAX, wt_now_ms(), line);
          if (n > 0)
            {
              uint8_t *copy = malloc((size_t)n);

              if (copy != NULL)
                {
                  memcpy(copy, body, (size_t)n);
                  if (wt_queue_put(ctx->queue, WT_TYPE_EVT_LOG, 0, copy,
                                   (size_t)n, 0) < 0)
                    {
                      break;
                    }
                }
            }
        }

      status = pclose(fp);

      /* pclose() returns ERROR when waitpid() can no longer find the shell,
       * which is the usual outcome for a command that finished before we got
       * here.  That is not the command's exit status and must not be presented
       * as one.
       */

      status_known = status >= 0;
    }

  /* The closing line carries the exit status, which is how the page knows to
   * stop showing the command as running -- so it is queued as an RSP, not as a
   * log event.
   *
   * As an EVT_LOG it was droppable, and a command whose output arrives in a
   * burst drops exactly this: `ls /dev` emits ~18 lines into a 16-deep queue,
   * the tail is discarded, and the front end shows the command as still running
   * for ever.  Observed on hardware 2026-08-18.  RSP is the class that must not
   * be dropped, which is the same reason the gap notice is sent that way.
   */

  {
    int n = wt_json_exit(body, WT_LOG_BODY_MAX, wt_now_ms(), status,
                         status_known);

    if (n > 0)
      {
        uint8_t *copy = malloc((size_t)n);

        if (copy != NULL)
          {
            memcpy(copy, body, (size_t)n);
            wt_queue_put(ctx->queue, WT_TYPE_RSP, 0, copy,
                         (size_t)n, WT_SHELL_EXIT_TIMEOUT_MS);
          }
      }
  }

  free(line);
  free(body);
  ctx->shell_running = false;
  return NULL;
}

static char *wt_cmd_shell_exec(struct wt_ctx_s *ctx, const char *cmdline)
{
  pthread_attr_t attr;
  int ret;

  if (cmdline == NULL || cmdline[0] == '\0')
    {
      return wt_fail("shell.exec: cmdline is required", -EINVAL);
    }

  if (strlen(cmdline) >= WT_SHELL_CMD_MAX)
    {
      return wt_fail("shell.exec: command line too long", -E2BIG);
    }

  if (ctx->shell_running)
    {
      /* One at a time.  Passthrough lets the operator launch any app on the
       * board; without this gate a handful of clicks leaves a dozen tasks
       * running and the board out of heap.
       */

      return wt_fail("busy", -EBUSY);
    }

  pthread_mutex_lock(&ctx->shell_lock);
  strlcpy(ctx->shell_cmd, cmdline, sizeof(ctx->shell_cmd));
  pthread_mutex_unlock(&ctx->shell_lock);

  ctx->shell_kill = false;
  ctx->shell_running = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  ret = pthread_create(&ctx->shell_thread, &attr, wt_shell_thread, ctx);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      ctx->shell_running = false;
      return wt_fail("shell.exec: could not start task", -ret);
    }

  pthread_detach(ctx->shell_thread);
  return wt_ok_raw("{\"accepted\":true}");
}

static char *wt_cmd_shell_kill(struct wt_ctx_s *ctx)
{
  /* Stops relaying and lets the command finish on its own.  There is no
   * portable way to signal what popen() started here, and claiming to have
   * killed a task that is still running would be worse than admitting the
   * relay stopped.
   */

  ctx->shell_kill = true;
  return wt_ok_raw("{}");
}

/* Argument accessors.  Plain functions rather than macros so the file stays
 * within standard C: statement expressions are a GNU extension and this tree
 * builds application code with -Werror.
 */

static const char *wt_arg_str(cJSON *args, const char *name)
{
  cJSON *a = args != NULL ? cJSON_GetObjectItemCaseSensitive(args, name)
                          : NULL;

  return cJSON_IsString(a) ? a->valuestring : NULL;
}

static bool wt_arg_bool(cJSON *args, const char *name, bool dflt)
{
  cJSON *a = args != NULL ? cJSON_GetObjectItemCaseSensitive(args, name)
                          : NULL;

  return cJSON_IsBool(a) ? cJSON_IsTrue(a) != 0 : dflt;
}

static int wt_arg_int(cJSON *args, const char *name, int dflt)
{
  cJSON *a = args != NULL ? cJSON_GetObjectItemCaseSensitive(args, name)
                          : NULL;

  return cJSON_IsNumber(a) ? (int)a->valuedouble : dflt;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

char *wt_command_dispatch(struct wt_ctx_s *ctx, const char *json, size_t len)
{
  cJSON *root;
  cJSON *node;
  const char *cmd;
  cJSON *args;
  char *rsp = NULL;

  root = cJSON_ParseWithLength(json, len);
  if (root == NULL)
    {
      return wt_fail("request is not valid JSON", -EINVAL);
    }

  node = cJSON_GetObjectItemCaseSensitive(root, "cmd");
  if (!cJSON_IsString(node) || node->valuestring == NULL)
    {
      cJSON_Delete(root);
      return wt_fail("request has no cmd", -EINVAL);
    }

  cmd = node->valuestring;
  args = cJSON_GetObjectItemCaseSensitive(root, "args");

  if (strcmp(cmd, "kvdb.list") == 0)
    {
      rsp = wt_cmd_kvdb_list(wt_arg_bool(args, "raw", false));
    }
  else if (strcmp(cmd, "kvdb.get") == 0)
    {
      const char *key = wt_arg_str(args, "key");

      rsp = key == NULL ? wt_fail("kvdb.get: key is required", -EINVAL)
                        : wt_cmd_kvdb_get(key, wt_arg_bool(args, "raw", false));
    }
  else if (strcmp(cmd, "kvdb.set") == 0)
    {
      const char *key = wt_arg_str(args, "key");
      const char *value = wt_arg_str(args, "value");

      rsp = (key == NULL || value == NULL)
            ? wt_fail("kvdb.set: key and value are required", -EINVAL)
            : wt_cmd_kvdb_set(key, value);
    }
  else if (strcmp(cmd, "kvdb.del") == 0)
    {
      const char *key = wt_arg_str(args, "key");

      rsp = key == NULL ? wt_fail("kvdb.del: key is required", -EINVAL)
                        : wt_cmd_kvdb_del(key);
    }
  else if (strcmp(cmd, "wifi.status") == 0)
    {
      rsp = wt_cmd_wifi_status();
    }
  else if (strcmp(cmd, "wifi.connect") == 0)
    {
      rsp = wt_cmd_wifi_connect(ctx, wt_arg_str(args, "ssid"),
                                wt_arg_str(args, "psk"));
    }
  else if (strcmp(cmd, "camera.start") == 0)
    {
      rsp = wt_cmd_camera_start(ctx,
                                wt_arg_int(args, "width", WT_CAM_DEFAULT_W),
                                wt_arg_int(args, "height", WT_CAM_DEFAULT_H));
    }
  else if (strcmp(cmd, "camera.stop") == 0)
    {
      rsp = wt_cmd_camera_stop(ctx);
    }
  else if (strcmp(cmd, "log.subscribe") == 0)
    {
      rsp = wt_cmd_log_subscribe(ctx, wt_arg_bool(args, "on", true));
    }
  else if (strcmp(cmd, "sys.status") == 0)
    {
      rsp = wt_cmd_sys_status(ctx);
    }
  else if (strcmp(cmd, "sys.reboot") == 0)
    {
      rsp = wt_cmd_sys_reboot(ctx);
    }
  else if (strcmp(cmd, "shell.exec") == 0)
    {
      rsp = wt_cmd_shell_exec(ctx, wt_arg_str(args, "cmdline"));
    }
  else if (strcmp(cmd, "shell.kill") == 0)
    {
      rsp = wt_cmd_shell_kill(ctx);
    }
  else
    {
      rsp = wt_fail("unknown cmd", -ENOSYS);
    }

  cJSON_Delete(root);

  if (rsp == NULL)
    {
      rsp = wt_fail("out of memory", -ENOMEM);
    }

  return rsp;
}
