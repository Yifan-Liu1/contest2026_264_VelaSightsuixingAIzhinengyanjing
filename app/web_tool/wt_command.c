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

#include <mqueue.h>

#include <sys/boardctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <nuttx/audio/audio.h>
#include <nuttx/video/video.h>

#include <netutils/cJSON.h>
#include <netutils/netlib.h>

#include <arch/board/kvdb.h>

#include "wt_command.h"
#include "wt_protocol.h"
#include "wt_queue.h"

/* app/conv's record store, reached by REALPATH from this directory -- see the
 * comment in CMakeLists.txt.  Only the reading half is used here.
 */

#include "conv_store.h"

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

/* Conversation record read buffers.  Same sizes the `conv` command uses
 * (CONV_TEXT_MAX and its cue_json buffer), so a record that prints in full on
 * the console also arrives in full at the page -- two different truncation
 * points for the same file would be a bug that only shows up on long
 * conversations.
 */

#define WT_CONV_TEXT_MAX  1024
#define WT_CONV_CUE_MAX   512

/* Playback device, and where 0 dB lands in the upper half's 0..1000 scale.
 *
 * The driver maps 0..1000 onto the DAC's 6-bit digital gain whose 0 dB point
 * is 0x2d of 0x3f, so unity is 714 rather than the top of the range: above it
 * the DAC is amplifying and can clip.  The page marks that point instead of
 * presenting 100% as "normal".
 */

#define WT_AUDIO_PLAY_DEV     "/dev/audio/pcm0p"
#define WT_AUDIO_UNITY_VOLUME ((0x2d * 1000) / 0x3f)

/* The spoken confirmation: mono signed 16-bit little-endian, no header.
 *
 * 8 kHz, not 16: at 16 kHz the two-second phrase is 63.7 KB and the protocol's
 * frame limit is 65.5 KB, which leaves nothing for a slightly longer phrase --
 * a limit that is met exactly today is a limit that breaks on the next edit.
 * 8 kHz halves it and speech at 8 kHz is what a telephone is.
 *
 * 8.3 name, like everything else on this card: CONFIG_FAT_LFN is off and a
 * longer name would be silently mangled.
 */

#define WT_ANNOUNCE_PCM   "/mnt/sdnand/ai_agent/VOLSET.PCM"
#define WT_ANNOUNCE_RATE  8000
#define WT_ANNOUNCE_MAX   (256 * 1024)

/* Upload chunk ceiling, in base64 characters.  Well under the 64 KB frame so
 * the JSON around it cannot push a legal chunk over the limit -- the far end
 * drops the connection on an oversized frame rather than trying to recover.
 */

#define WT_ANNOUNCE_B64_MAX  (32 * 1024)

/* Playback plumbing for the announcement. */

#define WT_PLAY_MQ           "wt_play"
#define WT_PLAY_MAX_BUFFERS  8
#define WT_PLAY_POLL_MS      50

/* Consecutive idle polls before giving up on a clip.  Without it a driver that
 * stopped returning buffers would hold the session thread for ever.  At 50 ms
 * this is five seconds, well over the length of any confirmation.
 */

#define WT_PLAY_IDLE_LIMIT   100

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

static double wt_arg_dbl(cJSON *args, const char *name, double dflt)
{
  cJSON *a = args != NULL ? cJSON_GetObjectItemCaseSensitive(args, name)
                          : NULL;

  return cJSON_IsNumber(a) ? a->valuedouble : dflt;
}

/****************************************************************************
 * Private Functions: playback volume
 *
 * The DAC's digital gain, reached through the standard NuttX audio feature
 * ioctl rather than anything private to this board.
 *
 * No AUDIOIOC_RESERVE around it, deliberately: audio.c forwards
 * AUDIOIOC_CONFIGURE straight to the lower half without checking whether the
 * device is reserved, so the volume can be changed while something is
 * playing -- which is when an operator actually wants to change it.  Taking
 * the reservation would have made this fail with EBUSY exactly then.
 ****************************************************************************/

/* One direction of the audio device, opened for the duration of one clip.
 *
 * The sequence below is audio_test_open()'s, kept in the same order because
 * that order is not arbitrary: GETBUFFERINFO has to precede ALLOCBUFFER or the
 * upper half's buffer quota is still zero and every allocation returns 0
 * (audio.c:786) -- which presents as "no buffers" rather than as a missing
 * ioctl.
 */

struct wt_play_s
{
  int                 fd;
  mqd_t               mq;
  unsigned int        nbuffers;
  unsigned int        buffersize;
  struct ap_buffer_s *buffers[WT_PLAY_MAX_BUFFERS];
  unsigned int        inflight;
  bool                started;
};

static void wt_play_close(struct wt_play_s *p)
{
  struct audio_buf_desc_s desc;
  unsigned int i;

  if (p->fd < 0)
    {
      return;
    }

  if (p->started)
    {
      ioctl(p->fd, AUDIOIOC_STOP, 0);
      p->started = false;
    }

  if (p->mq != (mqd_t)-1)
    {
      ioctl(p->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)p->mq);
    }

  for (i = 0; i < p->nbuffers; i++)
    {
      if (p->buffers[i] != NULL)
        {
          desc.u.buffer = p->buffers[i];
          ioctl(p->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          p->buffers[i] = NULL;
        }
    }

  ioctl(p->fd, AUDIOIOC_RELEASE, 0);

  if (p->mq != (mqd_t)-1)
    {
      mq_close(p->mq);
      mq_unlink(WT_PLAY_MQ);
      p->mq = (mqd_t)-1;
    }

  close(p->fd);
  p->fd = -1;
}

static int wt_play_open(struct wt_play_s *p, unsigned int samplerate)
{
  struct audio_caps_desc_s cap_desc;
  struct ap_buffer_info_s buf_info;
  struct audio_buf_desc_s desc;
  struct mq_attr attr;
  unsigned int i;
  int ret;

  memset(p, 0, sizeof(*p));
  p->fd = -1;
  p->mq = (mqd_t)-1;

  p->fd = open(WT_AUDIO_PLAY_DEV, O_RDWR | O_CLOEXEC);
  if (p->fd < 0)
    {
      return -errno;
    }

  if (ioctl(p->fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      ret = -errno;
      close(p->fd);
      p->fd = -1;
      return ret;
    }

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_channels       = 1;
  cap_desc.caps.ac_controls.hw[0] = samplerate & 0xffff;
  cap_desc.caps.ac_controls.b[3]  = samplerate >> 16;
  cap_desc.caps.ac_controls.b[2]  = 16;
  cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;

  if (ioctl(p->fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      ret = -errno;
      goto err;
    }

  if (ioctl(p->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info) < 0)
    {
      ret = -errno;
      goto err;
    }

  p->buffersize = buf_info.buffer_size;
  p->nbuffers   = buf_info.nbuffers > WT_PLAY_MAX_BUFFERS ?
                  WT_PLAY_MAX_BUFFERS : buf_info.nbuffers;

  for (i = 0; i < p->nbuffers; i++)
    {
      desc.numbytes  = p->buffersize;
      desc.u.pbuffer = &p->buffers[i];

      /* Zero is not an error code here, it is the quota answer -- see the
       * comment on the struct.
       */

      ret = ioctl(p->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
      if (ret <= 0 || p->buffers[i] == NULL)
        {
          p->nbuffers = i;
          ret = -ENOMEM;
          goto err;
        }
    }

  attr.mq_maxmsg  = p->nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = 0;

  p->mq = mq_open(WT_PLAY_MQ, O_RDWR | O_CREAT, 0644, &attr);
  if (p->mq == (mqd_t)-1)
    {
      ret = -errno;
      goto err;
    }

  if (ioctl(p->fd, AUDIOIOC_REGISTERMQ, (unsigned long)p->mq) < 0)
    {
      ret = -errno;
      goto err;
    }

  return OK;

err:
  wt_play_close(p);
  return ret;
}

/* Copy the next slice of the clip into a buffer.  Returns the bytes supplied,
 * zero once the clip is spent.
 */

static size_t wt_play_fill(struct ap_buffer_s *apb, const uint8_t *pcm,
                           size_t len, size_t *pos)
{
  size_t copy = len - *pos;

  if (copy > apb->nmaxbytes)
    {
      copy = apb->nmaxbytes;
    }

  if (copy > 0)
    {
      memcpy(apb->samp, pcm + *pos, copy);
      *pos += copy;
    }

  apb->nbytes  = copy;
  apb->curbyte = 0;
  return copy;
}

static int wt_play_enqueue(struct wt_play_s *p, struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s desc;

  desc.numbytes = apb->nbytes;
  desc.u.buffer = apb;

  if (ioctl(p->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc) < 0)
    {
      return -errno;
    }

  p->inflight++;
  return OK;
}

/****************************************************************************
 * Name: wt_play_pcm
 *
 * Description:
 *   Play one mono 16-bit clip to the speaker, blocking until it has drained.
 *
 *   Blocking is deliberate.  The clip is under two seconds and the caller is
 *   the session thread, which has nothing else to do until it answers; a
 *   thread for it would need its own lifetime and a way to say "still
 *   playing", and the only thing that buys is a response that arrives before
 *   the sound the operator is waiting to hear.
 *
 ****************************************************************************/

static int wt_play_pcm(const uint8_t *pcm, size_t len,
                       unsigned int samplerate, unsigned int *played)
{
  struct wt_play_s p;
  size_t pos = 0;
  unsigned int idle = 0;
  unsigned int i;
  int ret;

  *played = 0;

  ret = wt_play_open(&p, samplerate);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < p.nbuffers; i++)
    {
      if (wt_play_fill(p.buffers[i], pcm, len, &pos) == 0)
        {
          break;
        }

      ret = wt_play_enqueue(&p, p.buffers[i]);
      if (ret < 0)
        {
          wt_play_close(&p);
          return ret;
        }
    }

  if (ioctl(p.fd, AUDIOIOC_START, 0) < 0)
    {
      ret = -errno;
      wt_play_close(&p);
      return ret;
    }

  p.started = true;

  while ((pos < len || p.inflight > 0) && idle < WT_PLAY_IDLE_LIMIT)
    {
      struct audio_msg_s msg;
      struct timespec ts;
      unsigned int prio;
      ssize_t got;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += WT_PLAY_POLL_MS * 1000000;
      if (ts.tv_nsec >= 1000000000)
        {
          ts.tv_nsec -= 1000000000;
          ts.tv_sec++;
        }

      got = mq_timedreceive(p.mq, (char *)&msg, sizeof(msg), &prio, &ts);
      if (got != sizeof(msg))
        {
          idle++;
          continue;
        }

      if (msg.msg_id != AUDIO_MSG_DEQUEUE || msg.u.ptr == NULL)
        {
          continue;
        }

      idle = 0;
      if (p.inflight > 0)
        {
          p.inflight--;
        }

      (*played)++;

      if (pos < len)
        {
          struct ap_buffer_s *apb = msg.u.ptr;

          if (wt_play_fill(apb, pcm, len, &pos) > 0)
            {
              wt_play_enqueue(&p, apb);
            }
        }
    }

  wt_play_close(&p);
  return idle >= WT_PLAY_IDLE_LIMIT ? -ETIMEDOUT : OK;
}

/* base64 decode, in place of a dependency.
 *
 * Rejects anything that is not base64 rather than skipping it: this decodes a
 * clip that is about to be played at whatever volume was just set, and silently
 * dropping stray bytes would turn a truncated upload into noise out of the
 * speaker instead of an error on the page.  Padding is optional because the
 * chunks are cut on 3-byte boundaries by the sender and only the last one has
 * any.
 */

static int wt_b64_val(char c)
{
  if (c >= 'A' && c <= 'Z')
    {
      return c - 'A';
    }

  if (c >= 'a' && c <= 'z')
    {
      return c - 'a' + 26;
    }

  if (c >= '0' && c <= '9')
    {
      return c - '0' + 52;
    }

  if (c == '+')
    {
      return 62;
    }

  if (c == '/')
    {
      return 63;
    }

  return -1;
}

static int wt_b64_decode(const char *in, uint8_t *out, size_t outcap,
                         size_t *outlen)
{
  uint32_t acc = 0;
  unsigned int nbits = 0;
  size_t pos = 0;

  for (; *in != '\0'; in++)
    {
      int v;

      if (*in == '=')
        {
          break;
        }

      v = wt_b64_val(*in);
      if (v < 0)
        {
          return -EINVAL;
        }

      acc = (acc << 6) | (uint32_t)v;
      nbits += 6;

      if (nbits >= 8)
        {
          nbits -= 8;
          if (pos >= outcap)
            {
              return -E2BIG;
            }

          out[pos++] = (uint8_t)((acc >> nbits) & 0xff);
        }
    }

  *outlen = pos;
  return OK;
}

/* mkdir every component of the path's directory.  The card starts empty after
 * a format and the clip's directory is shared with the conversation store, so
 * whichever feature is used first has to create it.
 */

static int wt_mkdir_parents(const char *path)
{
  char dir[96];
  char *p;

  strlcpy(dir, path, sizeof(dir));

  p = strrchr(dir, '/');
  if (p == NULL)
    {
      return OK;
    }

  *p = '\0';

  for (p = dir + 1; *p != '\0'; p++)
    {
      if (*p != '/')
        {
          continue;
        }

      *p = '\0';
      if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        {
          return -errno;
        }

      *p = '/';
    }

  if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return OK;
}

/****************************************************************************
 * Name: wt_announce
 *
 * Description:
 *   Play the "volume set" confirmation from the SD card.
 *
 *   On the card rather than in the firmware because there is no room for it:
 *   this configuration is at 92% of a 1088 KB flash, and the clip is 29 KB of
 *   raw PCM.  The card has it to spare and the phrase can be re-recorded
 *   without a reflash, which is the same trade as keeping the page on the
 *   development machine.
 *
 *   Raw PCM rather than Opus, even though libopus is linked: a decoder here
 *   would be code and heap for a file that is already small enough, and the
 *   samples being exactly what comes out of the file makes "is it the volume
 *   or is it the clip" answerable by ear.
 *
 * Returned Value:
 *   true when the clip was played.  On failure *err carries the reason: the
 *   volume itself has already been set by the time this runs, so a missing
 *   clip must not turn into a failed command.
 *
 ****************************************************************************/

/* The clip is played on its own thread, not on the session thread.
 *
 * Blocking the session thread was the first attempt and it broke the link: that
 * thread is the one that reads the socket and answers PING (web_tool_main.c's
 * receive loop dispatches commands and replies to keepalives from the same
 * place), so two seconds of playback is two seconds of not answering.  The host
 * gives up after 16 s, and several volume changes in a row were enough to be
 * declared dead -- observed as "board stopped answering (no frame for 16s)"
 * while the board was healthy and audibly playing.
 *
 * So the handler starts a thread and answers immediately.  What the page loses
 * is confirmation that the sound finished; what it gains is a link that
 * survives using the feature.  `announced` therefore means "playback started",
 * and the syslog line carries the buffer count when it ends.
 */

struct wt_announce_job_s
{
  uint8_t *pcm;
  size_t   len;
};

static volatile bool g_announce_busy;

static void *wt_announce_thread(void *arg)
{
  struct wt_announce_job_s *job = arg;
  unsigned int played = 0;
  int ret;

  ret = wt_play_pcm(job->pcm, job->len, WT_ANNOUNCE_RATE, &played);
  if (ret < 0)
    {
      syslog(LOG_ERR, "web_tool: announcement failed: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "web_tool: announcement played, %u buffer(s)\n",
             played);
    }

  free(job->pcm);
  free(job);
  g_announce_busy = false;
  return NULL;
}

static bool wt_announce(int *err)
{
  struct wt_announce_job_s *job;
  pthread_attr_t attr;
  struct stat st;
  pthread_t tid;
  uint8_t *pcm;
  size_t len;
  int fd;
  int ret;

  *err = 0;

  if (g_announce_busy)
    {
      /* One at a time.  Two clips on the DAC at once is not a thing the device
       * can do, and queueing them would mean a burst of slider clicks keeps the
       * speaker talking long after the operator stopped.
       */

      *err = -EBUSY;
      return false;
    }

  if (stat(WT_ANNOUNCE_PCM, &st) < 0)
    {
      *err = -errno;
      return false;
    }

  if (st.st_size < 2 || (size_t)st.st_size > WT_ANNOUNCE_MAX)
    {
      /* Refused rather than truncated: half a phrase played at the new volume
       * still sounds like a working feature, so it would be a bug that hides.
       */

      *err = -E2BIG;
      return false;
    }

  len = (size_t)st.st_size & ~(size_t)1;   /* whole samples only */

  pcm = malloc(len);
  if (pcm == NULL)
    {
      *err = -ENOMEM;
      return false;
    }

  fd = open(WT_ANNOUNCE_PCM, O_RDONLY);
  if (fd < 0)
    {
      *err = -errno;
      free(pcm);
      return false;
    }

  ret = (int)read(fd, pcm, len);
  close(fd);

  if (ret < (int)len)
    {
      *err = ret < 0 ? -errno : -EIO;
      free(pcm);
      return false;
    }

  /* Read on this thread, played on the other: the file is on the card and a
   * read that fails is something the page should hear about in its response,
   * whereas the playback is what must not hold the session up.
   */

  job = malloc(sizeof(*job));
  if (job == NULL)
    {
      free(pcm);
      *err = -ENOMEM;
      return false;
    }

  job->pcm = pcm;
  job->len = len;
  g_announce_busy = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);

  /* Above the low-priority work queue (100) for the same reason the camera
   * grabber is: the DAC asks for the next buffer through this thread, and below
   * LPWORK it would be starved and the clip would stutter.  Below HPWORK (224)
   * so it cannot delay the audio interrupt itself.
   */

  {
    struct sched_param sp;

    sp.sched_priority = 110;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  }

  ret = pthread_create(&tid, &attr, wt_announce_thread, job);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_announce_busy = false;
      free(pcm);
      free(job);
      *err = -ret;
      return false;
    }

  pthread_detach(tid);
  return true;
}

/****************************************************************************
 * Name: wt_cmd_audio_announce
 *
 * Description:
 *   Receive the confirmation clip and write it to the card.
 *
 *   This exists because there was no way to put a file on the board at all:
 *   the console is a mailbox command-injection path rather than a byte stream,
 *   and nothing else in this project uploads.  Rather than add a general file
 *   transfer for one 32 KB clip, the clip arrives base64 in the request that
 *   already crosses TLS.
 *
 *   Chunked with an explicit offset, and the offset is checked against the file
 *   position instead of trusted: base64 makes each chunk about a third larger
 *   than the raw bytes, so a 32 KB clip is several requests, and a lost or
 *   reordered one would otherwise produce a file that is the right length and
 *   the wrong sound.
 *
 ****************************************************************************/

static char *wt_cmd_audio_announce(cJSON *args)
{
  const char *b64 = wt_arg_str(args, "b64");
  int offset = wt_arg_int(args, "offset", 0);
  bool final = wt_arg_bool(args, "final", false);
  uint8_t *raw;
  size_t rawlen;
  char data[128];
  int fd;
  int ret;

  if (b64 == NULL)
    {
      /* No payload: report what is already there, so the page can decide
       * whether it needs to upload at all.
       */

      struct stat st;

      if (stat(WT_ANNOUNCE_PCM, &st) < 0)
        {
          snprintf(data, sizeof(data), "{\"present\":false,\"bytes\":0}");
        }
      else
        {
          snprintf(data, sizeof(data),
                   "{\"present\":true,\"bytes\":%ld,\"seconds\":%u.%02u}",
                   (long)st.st_size,
                   (unsigned int)(st.st_size / 2 / WT_ANNOUNCE_RATE),
                   (unsigned int)(((st.st_size / 2) % WT_ANNOUNCE_RATE) *
                                  100 / WT_ANNOUNCE_RATE));
        }

      return wt_ok_raw(data);
    }

  if (offset < 0 || offset > (int)WT_ANNOUNCE_MAX)
    {
      return wt_fail("audio.announce: offset out of range", -EINVAL);
    }

  rawlen = strlen(b64);
  if (rawlen == 0 || rawlen > WT_ANNOUNCE_B64_MAX)
    {
      return wt_fail("audio.announce: chunk too large", -E2BIG);
    }

  raw = malloc(rawlen);            /* decoded is always smaller than encoded */
  if (raw == NULL)
    {
      return NULL;
    }

  ret = wt_b64_decode(b64, raw, rawlen, &rawlen);
  if (ret < 0)
    {
      free(raw);
      return wt_fail("audio.announce: chunk is not valid base64", ret);
    }

  if (offset == 0)
    {
      ret = wt_mkdir_parents(WT_ANNOUNCE_PCM);
      if (ret < 0)
        {
          free(raw);
          return wt_fail("audio.announce: cannot create the directory -- is "
                         "/mnt/sdnand mounted?", ret);
        }
    }

  /* O_TRUNC only at offset zero, so a retry of the first chunk restarts the
   * file rather than appending to a half-written one.
   */

  fd = open(WT_ANNOUNCE_PCM,
            O_WRONLY | O_CREAT | (offset == 0 ? O_TRUNC : 0), 0644);
  if (fd < 0)
    {
      int err = -errno;

      free(raw);
      return wt_fail("audio.announce: cannot open the clip for writing", err);
    }

  if (lseek(fd, offset, SEEK_SET) != offset)
    {
      int err = -errno;

      close(fd);
      free(raw);
      return wt_fail("audio.announce: seek failed", err);
    }

  ret = (int)write(fd, raw, rawlen);
  free(raw);

  if (ret < (int)rawlen)
    {
      int err = ret < 0 ? -errno : -ENOSPC;

      close(fd);
      return wt_fail("audio.announce: short write", err);
    }

  ret = (int)lseek(fd, 0, SEEK_CUR);
  close(fd);

  snprintf(data, sizeof(data), "{\"bytes\":%d,\"final\":%s}", ret,
           final ? "true" : "false");
  return wt_ok_raw(data);
}

static char *wt_cmd_audio_volume(cJSON *args)
{
  struct audio_caps_desc_s cap_desc;
  struct wt_sb_s sb;
  int requested = wt_arg_int(args, "volume", -1);
  int current = -1;
  int announce_err = 0;
  bool announced = false;
  int fd;

  if (requested > 1000 || (requested < 0 && requested != -1))
    {
      return wt_fail("audio.volume: volume must be 0..1000", -EINVAL);
    }

  fd = open(WT_AUDIO_PLAY_DEV, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      return wt_fail("audio.volume: cannot open " WT_AUDIO_PLAY_DEV, -errno);
    }

  if (requested >= 0)
    {
      memset(&cap_desc, 0, sizeof(cap_desc));
      cap_desc.caps.ac_len       = sizeof(struct audio_caps_s);
      cap_desc.caps.ac_type      = AUDIO_TYPE_FEATURE;
      cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
      cap_desc.caps.ac_controls.hw[0] = (uint16_t)requested;

      if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
        {
          int err = -errno;

          close(fd);
          return wt_fail("audio.volume: the driver rejected the volume", err);
        }
    }

  /* Read back rather than echo the request: the driver clamps and quantises
   * to 6 bits of digital gain, so 700 and 714 are the same setting and
   * reporting the request would invent a precision that does not exist.
   *
   * Read before the announcement, so the value reported is the one the
   * announcement is about to be played at.
   */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len       = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type      = AUDIO_TYPE_FEATURE;
  cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;

  if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&cap_desc.caps) >= 0 &&
      cap_desc.caps.ac_channels != 0)
    {
      current = cap_desc.caps.ac_controls.hw[0];
    }

  close(fd);

  /* Say it out loud, at the volume just set.
   *
   * This is the whole point of the control: a number on a slider does not tell
   * anyone whether the board is too quiet in the room they are standing in, and
   * the confirmation being spoken *at* the new gain makes the setting audible
   * rather than merely acknowledged.
   *
   * One fixed phrase rather than one per level: the level is carried by how
   * loud the phrase is, so eleven recordings would say the same thing eleven
   * times and only differ in the number nobody needs to hear.
   *
   * Announced only on a set.  A read is what the page does on connect and
   * whenever the link comes back, and a board that says "音量设置成功" every
   * time a browser reconnects would be a poltergeist.
   */

  if (requested >= 0 && wt_arg_bool(args, "announce", true))
    {
      announced = wt_announce(&announce_err);
    }

  if (!wt_sb_init(&sb, 256))
    {
      return NULL;
    }

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":{");
  if (current >= 0)
    {
      wt_sb_addf(&sb, "\"volume\":%d,\"percent\":%d,", current,
                 (current * 100 + 500) / 1000);
    }
  else
    {
      /* Say so rather than substituting the request: a slider that shows a
       * number the hardware never confirmed is the failure this read-back
       * exists to avoid.
       */

      wt_sb_addstr(&sb, "\"volume\":null,\"percent\":null,");
    }

  /* 0 dB is where the device comes up, and it is not the top of the range --
   * the page needs it to mark the point above which the DAC is amplifying.
   */

  wt_sb_addf(&sb, "\"unity\":%d,\"max\":1000,\"applied\":%s",
             WT_AUDIO_UNITY_VOLUME, requested >= 0 ? "true" : "false");

  /* Reported separately from ok:true, because the volume did take even when
   * the announcement did not: the device can be busy with a recording, and
   * folding that into a failure would tell the operator the setting was lost
   * when it was not.
   */

  wt_sb_addf(&sb, ",\"announced\":%s", announced ? "true" : "false");
  if (!announced && announce_err != 0)
    {
      wt_sb_addf(&sb, ",\"announce_errno\":%d,\"announce_errname\":\"%s\"",
                 announce_err, wt_errname(announce_err));
    }

  wt_sb_addstr(&sb, "}}");

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
}

/****************************************************************************
 * Private Functions: conversation history
 *
 * The store is app/conv's; this is the second consumer of it, after the `conv`
 * NSH command.  Only reads live here.  Writing a record is the recogniser's
 * job and clearing history is destructive enough to be worth typing out on a
 * console, so neither is exposed to a page that a stray click can drive.
 ****************************************************************************/

struct wt_conv_list_s
{
  struct wt_sb_s *sb;
  int             matched;
  int             returned;
  int             limit;
};

static int wt_conv_visit(const struct conv_entry_s *e, void *arg)
{
  struct wt_conv_list_s *st = arg;

  st->matched++;

  /* Count every match but stop appending at the limit, rather than asking
   * conv_store_query() to stop early: a visitor that returns non-zero makes
   * the query return that value instead of a count, so the page would lose
   * the one number it needs to say "showing 20 of 57" -- and "57 matches, 20
   * shown" and "20 matches" are different answers to the operator's question.
   */

  if (st->returned >= st->limit)
    {
      return 0;
    }

  wt_sb_addf(st->sb, "%s{\"seq\":%u,\"date\":%u,\"epoch\":%lu",
             st->returned > 0 ? "," : "", e->seq,
             conv_epoch_to_date(e->epoch), (unsigned long)e->epoch);
  wt_sb_addf(st->sb, ",\"duration_ms\":%u,\"cue\":\"", e->duration_ms);
  wt_sb_addesc(st->sb, e->cue);
  wt_sb_addf(st->sb, "\",\"confidence\":%.2f,\"unable_to_judge\":%s",
             (double)e->confidence, e->unable_to_judge ? "true" : "false");
  wt_sb_addf(st->sb, ",\"text_bytes\":%zu,\"summary\":\"", e->text_bytes);
  wt_sb_addesc(st->sb, e->summary);
  wt_sb_addstr(st->sb, "\"}");

  st->returned++;
  return 0;
}

static char *wt_cmd_conv_query(cJSON *args)
{
  struct conv_filter_s filter;
  struct wt_conv_list_s st;
  struct wt_sb_s sb;
  unsigned int from;
  unsigned int to;
  int ret;

  ret = conv_store_ready();
  if (ret < 0)
    {
      /* Distinguishable from "no records": bring-up defers the SD-NAND mount,
       * so a query run in the first few seconds finds nothing for a reason
       * that has nothing to do with the filter.
       */

      return wt_fail("conv: store not ready -- is /mnt/sdnand mounted?", ret);
    }

  memset(&filter, 0, sizeof(filter));

  /* Dates arrive as YYYYMMDD integers, the same form the index holds and the
   * `conv` command takes, so the page never has to know about epochs.
   */

  from = (unsigned int)wt_arg_int(args, "from", 0);
  to   = (unsigned int)wt_arg_int(args, "to", 0);

  if (from != 0)
    {
      filter.from_epoch = conv_date_to_epoch(from, false);
    }

  if (to != 0)
    {
      filter.to_epoch = conv_date_to_epoch(to, true);
    }

  filter.cue     = wt_arg_str(args, "cue");
  filter.keyword = wt_arg_str(args, "keyword");
  filter.min_confidence = (float)wt_arg_dbl(args, "min_confidence", 0.0);

  /* Records the model could not judge are included by default.  Hiding them
   * would make a conversation the device saw disappear from history with
   * nothing to indicate it ever happened, and "we are not sure" is the honest
   * answer this product is built around.
   */

  filter.include_unjudged = wt_arg_bool(args, "include_unjudged", true);

  if (!wt_sb_init(&sb, 512))
    {
      return NULL;
    }

  st.sb       = &sb;
  st.matched  = 0;
  st.returned = 0;
  st.limit    = wt_arg_int(args, "limit", 50);

  if (st.limit <= 0 || st.limit > 200)
    {
      st.limit = 50;
    }

  wt_sb_addstr(&sb, "{\"ok\":true,\"data\":{\"items\":[");
  ret = conv_store_query(&filter, wt_conv_visit, &st);
  wt_sb_addf(&sb, "],\"matched\":%d,\"returned\":%d,\"limit\":%d}}",
             st.matched, st.returned, st.limit);

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  if (ret < 0)
    {
      free(sb.p);
      return wt_fail("conv: query failed", ret);
    }

  return sb.p;
}

static char *wt_cmd_conv_get(int seq)
{
  struct wt_sb_s sb;
  char *text;
  char *cue;
  int textlen;
  int cuelen;
  int ret;

  if (seq <= 0)
    {
      return wt_fail("conv.get: seq is required", -EINVAL);
    }

  ret = conv_store_ready();
  if (ret < 0)
    {
      return wt_fail("conv: store not ready -- is /mnt/sdnand mounted?", ret);
    }

  /* Heap, not stack: this runs on the session thread and a 1.5 KB frame there
   * is a stack overflow that presents itself as corrupted JSON on the wire.
   */

  text = malloc(WT_CONV_TEXT_MAX);
  cue  = malloc(WT_CONV_CUE_MAX);
  if (text == NULL || cue == NULL)
    {
      free(text);
      free(cue);
      return NULL;
    }

  textlen = conv_store_read_file(CONV_FMT_TEXT, (unsigned int)seq, text,
                                 WT_CONV_TEXT_MAX);
  if (textlen < 0)
    {
      free(text);
      free(cue);
      return wt_fail("conv.get: no such record", textlen);
    }

  /* A record with no analysis is normal -- the file is written after the
   * transcript -- so an empty object rather than a failure.
   */

  cuelen = conv_store_read_file(CONV_FMT_CUE, (unsigned int)seq, cue,
                               WT_CONV_CUE_MAX);
  if (cuelen < 0)
    {
      cue[0] = '\0';
    }

  if (!wt_sb_init(&sb, (size_t)textlen + 512))
    {
      free(text);
      free(cue);
      return NULL;
    }

  wt_sb_addf(&sb, "{\"ok\":true,\"data\":{\"seq\":%d,\"text\":\"", seq);
  wt_sb_addesc(&sb, text);

  /* The analysis is already JSON, so it goes in as a value rather than as an
   * escaped string: the page reads .cue.cues[0].confidence, and a string
   * would make every consumer parse it a second time.
   */

  wt_sb_addf(&sb, "\",\"text_bytes\":%d,\"cue\":%s}}", textlen,
             cue[0] != '\0' ? cue : "null");

  free(text);
  free(cue);

  if (sb.failed)
    {
      free(sb.p);
      return NULL;
    }

  return sb.p;
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
  else if (strcmp(cmd, "audio.announce") == 0)
    {
      rsp = wt_cmd_audio_announce(args);
    }
  else if (strcmp(cmd, "audio.volume") == 0)
    {
      /* One command for both directions: with no `volume` argument it reads,
       * with one it sets and then reads back.  Two commands would have made
       * the page do two round trips to show the result of its own change.
       */

      rsp = wt_cmd_audio_volume(args);
    }
  else if (strcmp(cmd, "conv.query") == 0)
    {
      rsp = wt_cmd_conv_query(args);
    }
  else if (strcmp(cmd, "conv.get") == 0)
    {
      rsp = wt_cmd_conv_get(wt_arg_int(args, "seq", 0));
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
