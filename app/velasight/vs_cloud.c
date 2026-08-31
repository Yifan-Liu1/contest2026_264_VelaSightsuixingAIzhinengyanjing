/****************************************************************************
 * app/velasight/vs_cloud.c
 *
 * Device side of the social-session cloud's /contest/v1 interface.  See
 * include/vs_cloud.h for the protocol, where it is documented, and how this
 * differs from the V1 integration plan.
 *
 * Why there is a plain-HTTP client in here
 * ----------------------------------------
 * packages/ai_agent/src/infra/vela_tls.c already provides everything the TLS
 * path needs -- arbitrary method, arbitrary body bytes, DNS through
 * getaddrinfo(), chunked and Content-Length decoding, and a keep-alive pool
 * that matters when a session polls every second and a half.  So HTTPS is
 * delegated to it and no second TLS client exists here.
 *
 * Its cleartext entry point cannot be reused, though: vela_http_post_json()
 * is fixed to POST with application/json, and three of the four endpoints
 * here are PUT, GET and DELETE.  Cleartext is not a corner case either -- the
 * cloud document's own worked examples are curl against
 * http://127.0.0.1:18080, and that is what bring-up runs against before a
 * real host with a real certificate chain exists.  So the cleartext path is
 * implemented here, with the same non-blocking connect and explicit poll()
 * timeout as app/audio_test/audio_test_stream.c's stream_connect().
 *
 * Buffers
 * -------
 * Response buffers are drawn per call, PSRAM first and the SRAM heap only as
 * a fallback.  The sizes are build options: a getResult that returns a whole
 * session's two timelines is a different order of magnitude from an upload
 * registration, and sizing both for the larger would waste PSRAM on every
 * frame.
 *
 * Per-call rather than shared so that the upload thread and the poll thread
 * do not serialize on one buffer -- but only the cleartext path actually gets
 * that.  vela_tls.c reads every response through one of two static 8 KiB
 * buffers taken under a mutex (s_tls_raw_buf), with a blocking wait on the
 * first when both are busy, so on the TLS path the two threads still
 * contend there, and a build with CONN_POOL_SIZE=1 has only one buffer.
 * Worth knowing before concluding that concurrent uploads and polls are
 * independent.
 *
 * The cJSON DOM is a separate matter: cJSON uses its own global hooks, which
 * are plain malloc/free against the SRAM heap.  So a 64 KiB getResult body
 * lands in PSRAM but its parse tree, and the cJSON_PrintUnformatted() copy
 * taken for the history record, do not.  Routing those to PSRAM means
 * cJSON_InitHooks(), which is process-global and shared with vs_history.c and
 * vs_voice.c, so it is deliberately not done here -- measure SRAM high water
 * over a long session before deciding.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <arch/chip/bk7258_psram.h>
#include <netutils/cJSON.h>
#include <netutils/netlib.h>

#include <infra/vela_tls.h>

#include "velasight_provisioning.h"

#include "include/vs_cloud.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Kconfig fallbacks.  Present so this translation unit still builds in a tree
 * whose .config predates these options; every one of them is also declared in
 * Kconfig with the same default.
 */

#ifndef CONFIG_VS_SOCIAL_CLOUD_HOST
#  define CONFIG_VS_SOCIAL_CLOUD_HOST ""
#endif

#ifndef CONFIG_VS_SOCIAL_CLOUD_PORT
#  define CONFIG_VS_SOCIAL_CLOUD_PORT 18080
#endif

#ifndef CONFIG_VS_SOCIAL_REG_RESP_BYTES
#  define CONFIG_VS_SOCIAL_REG_RESP_BYTES 4096
#endif

#ifndef CONFIG_VS_SOCIAL_RESP_MAX_BYTES
#  define CONFIG_VS_SOCIAL_RESP_MAX_BYTES 65536
#endif

#ifndef CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES
#  define CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES 262144
#endif

#ifndef CONFIG_VS_SOCIAL_CONNECT_TIMEOUT_MS
#  define CONFIG_VS_SOCIAL_CONNECT_TIMEOUT_MS 3000
#endif

#ifndef CONFIG_VS_SOCIAL_IO_TIMEOUT_MS
#  define CONFIG_VS_SOCIAL_IO_TIMEOUT_MS 8000
#endif

#ifndef CONFIG_VS_SOCIAL_DEVICE_ID
#  define CONFIG_VS_SOCIAL_DEVICE_ID ""
#endif

/* Whether the transfer to the presigned URL names its Content-Type.
 *
 * Kept as a value rather than tested with #ifdef at the call site, so the
 * header array is always built and always referenced and neither branch of
 * the option produces an unused-variable diagnostic.
 */

#ifdef CONFIG_VS_SOCIAL_UPLOAD_CONTENT_TYPE
#  define CLOUD_UPLOAD_SEND_CT 1
#else
#  define CLOUD_UPLOAD_SEND_CT 0
#endif

/* Endpoint paths.  Kept as literals rather than being assembled, so grepping
 * for a path in this file finds the document's own spelling.
 */

#define CLOUD_PATH_SESSION    "/contest/v1/session"
#define CLOUD_PATH_UPLOAD     "/contest/v1/upload"
#define CLOUD_PATH_GET_RESULT "/contest/v1/getResult"

/* Presigned URLs carry their signature as query parameters.  Measured shapes
 * sit around 300 characters, so this leaves generous room; a URL beyond it is
 * rejected by cloud_json_exact() with a log line naming both lengths, rather
 * than truncated and PUT in mutilated form, which would fail the signature
 * check for reasons nobody could see.
 */

#define CLOUD_PRESIGNED_PATH_MAX 1024
#define CLOUD_HOST_MAX           128
#define CLOUD_PORT_MAX           8

/* The prefix in front of the paths above.  One byte wider than the
 * provisioning field so a full-length stored value still terminates here
 * rather than being reported as too long by a check the user cannot act on.
 */

#define CLOUD_BASE_PATH_MAX      (VELASIGHT_PROV_CLOUD_PATH_MAX + 1)

/* There is deliberately no single identifier length limit here.  An earlier
 * version had one, wider than the structures that receive identifiers, so a
 * msgId longer than VS_CLOUD_MSG_ID_MAX passed validation and was then
 * truncated on its way into a buffer -- after which it never matched anything
 * and the message it named could never be collected.  Every identifier is now
 * checked against the capacity of the specific buffer that has to hold it;
 * see cloud_id_valid().
 */

/* How many identifiers one getResult may carry.  The interface takes an
 * array with no stated limit; this bounds the query string.
 *
 * The value itself is in the header, because a caller has to size its own
 * arrays from it.  The local alias stays so the buffer arithmetic below reads
 * as arithmetic on a local constant.
 */

#define CLOUD_POLL_MAX_IDS VS_CLOUD_POLL_MAX_IDS

/* Staging sizes for the getResult query, derived from the limits above rather
 * than picked, so the advertised batch size is one the buffers can carry.
 *
 *   REQ  the JSON document: '"<id>",' per identifier, plus the envelope
 *        fields and their punctuation
 *   ENC  the same after percent-encoding, which triples every byte outside
 *        the unreserved set -- and a JSON document is mostly braces, quotes,
 *        brackets and commas, so assume the worst
 *   MAX  the path, "?data=" and the encoded document
 */

#define CLOUD_QUERY_REQ_MAX \
  (CLOUD_POLL_MAX_IDS * (VS_CLOUD_MSG_ID_MAX + 4) + \
   VS_CLOUD_DEVICE_ID_MAX + VS_CLOUD_SESSION_ID_MAX + 128)

#define CLOUD_QUERY_ENC_MAX (3 * CLOUD_QUERY_REQ_MAX + 1)

/* Path, "?data=" and the encoded document, which is written in place after
 * the prefix rather than staged separately.
 */

#define CLOUD_QUERY_MAX (CLOUD_QUERY_ENC_MAX + 64)

/* struct cloud_url_s::path has to hold whichever is longer: a presigned URL's
 * path and query, or the getResult path with the encoded document appended.
 *
 * Sizing it from only the first is what an earlier version did, and a full
 * batch of identifiers then failed -E2BIG inside cloud_api_call(), one layer
 * below where the batch limit is checked and stated -- so the batch size the
 * API advertised was not one it could actually send.
 *
 * The base path prefix is added to both candidates rather than only to the
 * larger.  A presigned URL never carries it -- that URL is absolute and comes
 * from the cloud -- but writing the bound that way would make the two arms
 * differ for a reason a reader has to reconstruct, and the few bytes buy
 * nothing.
 */
#if CLOUD_QUERY_MAX > CLOUD_PRESIGNED_PATH_MAX
#  define CLOUD_PATH_MAX (CLOUD_QUERY_MAX + CLOUD_BASE_PATH_MAX)
#else
#  define CLOUD_PATH_MAX (CLOUD_PRESIGNED_PATH_MAX + CLOUD_BASE_PATH_MAX)
#endif

#define CLOUD_TAG "vs_cloud"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct cloud_url_s
{
  bool tls;
  char host[CLOUD_HOST_MAX];
  char port[CLOUD_PORT_MAX];
  char path[CLOUD_PATH_MAX];
};

struct cloud_state_s
{
  char device_id[VS_CLOUD_DEVICE_ID_MAX];
  char host[CLOUD_HOST_MAX];
  char port[CLOUD_PORT_MAX];

  /* What sits in front of /contest/v1, empty when the interface is at the
   * document root.  Stored with a leading slash and no trailing one, which
   * cloud_api_call() relies on to concatenate without testing.
   */

  char base_path[CLOUD_BASE_PATH_MAX];

  /* The port as a number as well as a string.  The string is what goes in the
   * Host header and into cloud_connect(); the number is what
   * vs_cloud_endpoint() hands back and what a comparison against the record
   * uses, and deriving one from the other at each use invited an atoi() of an
   * empty buffer.
   */

  uint16_t port_value;

  enum vs_cloud_origin_e origin;

  bool tls;
  bool configured;
  bool initialized;

  /* A session is open and its identifiers only mean anything on the host that
   * issued them, so the endpoint must not move underneath it.  Set by a
   * successful open, cleared when a close reaches a terminal answer or when a
   * later open supersedes it.
   *
   * Not a lock.  It only gates vs_cloud_reload_endpoint(), which answers
   * -EBUSY rather than waiting, so the worst case is that a re-provisioned
   * endpoint takes effect at the next session instead of immediately.
   */

  bool session_live;

  /* A reload arrived while a session was open.  Applied at the next open,
   * which is the first moment it is safe.
   */

  bool reload_pending;

  /* False while device_id is still the per-boot fallback, which means a later
   * call should retry the MAC.  See cloud_resolve_device_id().
   */

  bool device_id_stable;

  /* Set once the first unusable presigned URL has been logged, so a session
   * running against the mock cloud does not print the same line three times a
   * second.
   */

  bool mock_url_logged;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct cloud_state_s g_cloud;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cloud_now_ms
 *
 * Description:
 *   Wall-clock milliseconds for the protocol's timestamp field.
 *
 *   The value is echoed back by the cloud and is not used for ordering or
 *   correlation on this side, which is what makes it safe to send even though
 *   this board has no RTC and may not have been through SNTP.  Nothing here
 *   depends on it being a true date.
 *
 ****************************************************************************/

static uint64_t cloud_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: cloud_id_valid
 *
 * Description:
 *   Accept only identifiers that are safe to interpolate into a JSON string
 *   without escaping -- letters, digits, '-', '_' and '.' -- and that fit in
 *   cap bytes including the terminator.
 *
 *   Every identifier this module sends is one it generated itself, except
 *   msgId, which comes from the cloud and goes straight back out in a
 *   getResult query.  That is the one that matters: a msgId containing a
 *   quote would otherwise let the cloud's response reshape the next request
 *   body.  Rejecting it here is cheaper and more obvious than escaping.
 *
 *   cap is passed in rather than being a constant because the three kinds of
 *   identifier land in differently sized buffers, and a limit wider than the
 *   destination is worse than no limit: it turns a rejection into a silent
 *   truncation.
 *
 ****************************************************************************/

static bool cloud_id_valid(const char *id, size_t cap)
{
  size_t i;

  if (id == NULL || id[0] == '\0' || cap == 0)
    {
      return false;
    }

  for (i = 0; id[i] != '\0'; i++)
    {
      unsigned char c = (unsigned char)id[i];

      if (i + 1 >= cap)
        {
          return false;
        }

      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
        {
          continue;
        }

      return false;
    }

  return true;
}

/****************************************************************************
 * Name: cloud_copy_utf8
 *
 * Description:
 *   Copy a string into a fixed buffer, truncating on a UTF-8 character
 *   boundary rather than mid-sequence.
 *
 *   Everything the cloud sends that reaches the screen is Chinese, so a
 *   byte-wise truncation lands inside a three-byte sequence two times out of
 *   three, and the display renders the remainder as a replacement glyph.
 *
 ****************************************************************************/

static void cloud_copy_utf8(char *dst, size_t cap, const char *src)
{
  size_t len;

  if (dst == NULL || cap == 0)
    {
      return;
    }

  dst[0] = '\0';

  if (src == NULL)
    {
      return;
    }

  len = strlen(src);
  if (len >= cap)
    {
      len = cap - 1;

      /* Back off over continuation bytes (10xxxxxx) so the cut lands on a
       * lead byte or ASCII.
       */

      while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80)
        {
          len--;
        }
    }

  memcpy(dst, src, len);
  dst[len] = '\0';
}

/****************************************************************************
 * Name: cloud_alloc / cloud_free
 *
 * Description:
 *   PSRAM first, SRAM heap as a fallback.  Response buffers here are tens of
 *   kilobytes and must not compete with task stacks in the small SRAM heap
 *   for the duration of a round trip; but a transient PSRAM failure should
 *   degrade a session rather than end it.  Same policy as vs_media.c.
 *
 ****************************************************************************/

static void *cloud_alloc(size_t size, bool *from_psram)
{
  void *ptr = bk7258_psram_malloc(size);

  if (ptr != NULL)
    {
      *from_psram = true;
      return ptr;
    }

  *from_psram = false;
  return malloc(size);
}

static void cloud_free(void *ptr, bool from_psram)
{
  if (ptr == NULL)
    {
      return;
    }

  if (from_psram)
    {
      bk7258_psram_free(ptr);
    }
  else
    {
      free(ptr);
    }
}

void vs_cloud_release(unsigned char *data, bool from_psram)
{
  cloud_free(data, from_psram);
}

/****************************************************************************
 * Name: cloud_url_parse
 *
 * Description:
 *   Split an absolute URL into the pieces both transports want.  A URL with
 *   no scheme, or one starting with '/', is taken as relative to the
 *   configured cloud host -- which is what the document's example ttsMinutes
 *   value ("xxx/contest/api/session/20261001/xxx") looks like.
 *
 * Returned Value:
 *   0, -EINVAL on a malformed URL, -E2BIG when a component does not fit, or
 *   -EPROTONOSUPPORT for a scheme that is neither http nor https.  That last
 *   one is not a defect: the cloud's mock mode answers with "mock://..." and
 *   the caller is expected to handle it.
 *
 ****************************************************************************/

static int cloud_url_parse(const char *url, struct cloud_url_s *out)
{
  const char *host_start;
  const char *host_end;
  const char *path_start;
  const char *colon;
  size_t host_len;

  if (url == NULL || out == NULL)
    {
      return -EINVAL;
    }

  memset(out, 0, sizeof(*out));

  if (strncmp(url, "https://", 8) == 0)
    {
      out->tls = true;
      host_start = url + 8;
      snprintf(out->port, sizeof(out->port), "443");
    }
  else if (strncmp(url, "http://", 7) == 0)
    {
      out->tls = false;
      host_start = url + 7;
      snprintf(out->port, sizeof(out->port), "80");
    }
  else if (strstr(url, "://") != NULL)
    {
      return -EPROTONOSUPPORT;
    }
  else
    {
      /* Relative: reuse the configured endpoint. */

      if (!g_cloud.configured)
        {
          return -ENODATA;
        }

      out->tls = g_cloud.tls;
      snprintf(out->host, sizeof(out->host), "%s", g_cloud.host);
      snprintf(out->port, sizeof(out->port), "%s", g_cloud.port);

      if (snprintf(out->path, sizeof(out->path), "%s%s",
                   url[0] == '/' ? "" : "/", url) >= (int)sizeof(out->path))
        {
          return -E2BIG;
        }

      return 0;
    }

  /* Authority runs to the first '/', '?' or end of string. */

  host_end = host_start;
  while (*host_end != '\0' && *host_end != '/' && *host_end != '?')
    {
      host_end++;
    }

  if (host_end == host_start)
    {
      return -EINVAL;
    }

  /* A ':' inside the authority is a port.  IPv6 literals would also use
   * brackets and colons; the cloud is IPv4 and this interface has never been
   * given an IPv6 address, so they are rejected rather than half-supported.
   */

  colon = memchr(host_start, ':', (size_t)(host_end - host_start));
  if (memchr(host_start, '[', (size_t)(host_end - host_start)) != NULL)
    {
      return -EPROTONOSUPPORT;
    }

  host_len = colon != NULL ? (size_t)(colon - host_start)
                           : (size_t)(host_end - host_start);

  if (host_len == 0 || host_len >= sizeof(out->host))
    {
      return -E2BIG;
    }

  memcpy(out->host, host_start, host_len);
  out->host[host_len] = '\0';

  if (colon != NULL)
    {
      size_t port_len = (size_t)(host_end - colon - 1);

      if (port_len == 0 || port_len >= sizeof(out->port))
        {
          return -EINVAL;
        }

      memcpy(out->port, colon + 1, port_len);
      out->port[port_len] = '\0';
    }

  path_start = host_end;
  if (*path_start == '\0')
    {
      snprintf(out->path, sizeof(out->path), "/");
      return 0;
    }

  if (snprintf(out->path, sizeof(out->path), "%s%s",
               *path_start == '/' ? "" : "/",
               path_start) >= (int)sizeof(out->path))
    {
      return -E2BIG;
    }

  return 0;
}

/****************************************************************************
 * Name: cloud_urlencode
 *
 * Description:
 *   Percent-encode into a fixed buffer, leaving only the unreserved set
 *   alone.  Used for the getResult query, whose value is a whole JSON
 *   document -- braces, quotes, brackets and commas all have to survive
 *   intact through a query string.
 *
 * Returned Value:
 *   0, or -E2BIG when the encoded form does not fit.
 *
 ****************************************************************************/

static int cloud_urlencode(const char *src, char *dst, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  size_t i;

  if (src == NULL || dst == NULL || cap == 0)
    {
      return -EINVAL;
    }

  for (i = 0; src[i] != '\0'; i++)
    {
      unsigned char c = (unsigned char)src[i];

      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
          c == '~')
        {
          if (o + 1 >= cap)
            {
              return -E2BIG;
            }

          dst[o++] = (char)c;
        }
      else
        {
          if (o + 3 >= cap)
            {
              return -E2BIG;
            }

          dst[o++] = '%';
          dst[o++] = hex[(c >> 4) & 0x0f];
          dst[o++] = hex[c & 0x0f];
        }
    }

  dst[o] = '\0';
  return 0;
}

/****************************************************************************
 * Name: cloud_range_has_token
 *
 * Description:
 *   Case-insensitive substring search over [start, end).  Needed because the
 *   header section cannot be NUL-terminated for a bounded strcasestr()
 *   without writing into the response buffer, and the body may already be
 *   sitting in the bytes just past it.
 *
 ****************************************************************************/

static bool cloud_range_has_token(const char *start, const char *end,
                                  const char *token)
{
  size_t tlen = strlen(token);

  if (start == NULL || end == NULL || end < start ||
      (size_t)(end - start) < tlen)
    {
      return false;
    }

  for (; start + tlen <= end; start++)
    {
      if (strncasecmp(start, token, tlen) == 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: cloud_decode_chunked
 *
 * Description:
 *   Collapse a chunked body in place.  vela_tls.c has an equivalent for the
 *   TLS path but it is static there, so the cleartext path needs its own.
 *
 *   The size line is parsed by hand rather than with strtol().  strtol()
 *   skips leading whitespace, and '\n' is whitespace, so on a tail of bare
 *   line endings it walks straight past the newline memchr() just found and
 *   keeps looking for hex digits -- off the end of the buffer if the body
 *   filled it exactly.  Parsing within [src, eol) cannot leave the buffer
 *   whatever the peer sends.
 *
 * Returned Value:
 *   Decoded length.
 *
 ****************************************************************************/

static size_t cloud_decode_chunked(char *buf, size_t len)
{
  char *src = buf;
  char *dst = buf;
  char *end = buf + len;

  while (src < end)
    {
      char *eol;
      long chunk = 0;
      bool digits = false;
      const char *p;

      /* Chunk size line, hex, possibly with extensions after a ';'. */

      eol = memchr(src, '\n', (size_t)(end - src));
      if (eol == NULL)
        {
          break;
        }

      for (p = src; p < eol; p++)
        {
          int v;

          if (*p >= '0' && *p <= '9')
            {
              v = *p - '0';
            }
          else if (*p >= 'a' && *p <= 'f')
            {
              v = *p - 'a' + 10;
            }
          else if (*p >= 'A' && *p <= 'F')
            {
              v = *p - 'A' + 10;
            }
          else
            {
              /* ';' starts chunk extensions and CR ends the line; either way
               * the size is complete.  Anything else is malformed, and
               * stopping is the same safe action.
               */

              break;
            }

          digits = true;

          /* A size line long enough to overflow is malformed; clamping to
           * the remaining buffer makes the clamp below do the rejecting.
           */

          if (chunk > (long)len)
            {
              chunk = (long)len;
              break;
            }

          chunk = chunk * 16 + v;
        }

      src = eol + 1;

      if (!digits || chunk <= 0)
        {
          break;
        }

      if (src + chunk > end)
        {
          chunk = (long)(end - src);
        }

      memmove(dst, src, (size_t)chunk);
      dst += chunk;
      src += chunk;

      /* Trailing CRLF after the chunk data. */

      if (src + 2 <= end && src[0] == '\r' && src[1] == '\n')
        {
          src += 2;
        }
    }

  return (size_t)(dst - buf);
}

/****************************************************************************
 * Name: cloud_connect
 *
 * Description:
 *   Connected socket to host:port, or a negated errno.  Non-blocking connect
 *   with an explicit poll() so it cannot sit for the stack's own timeout;
 *   same shape as audio_test_stream.c's stream_connect().
 *
 *   An address that is already a dotted quad skips getaddrinfo() entirely.
 *   That is not just an optimization: bring-up runs against 127.0.0.1 before
 *   any DNS server is configured, and a resolver failure there would look
 *   like the cloud being unreachable.
 *
 ****************************************************************************/

static int cloud_connect(const char *host, const char *port)
{
  struct sockaddr_in addr;
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct timeval tv;
  struct pollfd pfd;
  const struct sockaddr *sa;
  socklen_t salen;
  int flags;
  int sock;
  int ret;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)atoi(port));

  if (inet_pton(AF_INET, host, &addr.sin_addr) == 1)
    {
      sa = (const struct sockaddr *)&addr;
      salen = sizeof(addr);
    }
  else
    {
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;

      if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL)
        {
          printf("%s: cannot resolve %s\n", CLOUD_TAG, host);
          return -EHOSTUNREACH;
        }

      sa = res->ai_addr;
      salen = res->ai_addrlen;
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      ret = -errno;
      goto errout;
    }

  flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  ret = connect(sock, sa, salen);
  if (ret < 0 && errno != EINPROGRESS)
    {
      ret = -errno;
      close(sock);
      goto errout;
    }

  if (ret < 0)
    {
      pfd.fd = sock;
      pfd.events = POLLOUT;
      pfd.revents = 0;

      ret = poll(&pfd, 1, CONFIG_VS_SOCIAL_CONNECT_TIMEOUT_MS);
      if (ret <= 0)
        {
          ret = ret == 0 ? -ETIMEDOUT : -errno;
          close(sock);
          goto errout;
        }

      if ((pfd.revents & POLLOUT) == 0)
        {
          close(sock);
          ret = -ECONNREFUSED;
          goto errout;
        }

      /* POLLOUT alone does not mean success -- a refused connection wakes
       * the poll too, and only the socket knows which happened.
       */

      {
        int err = 0;
        socklen_t len = sizeof(err);

        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
            err != 0)
          {
            close(sock);
            ret = -err;
            goto errout;
          }
      }
    }

  fcntl(sock, F_SETFL, flags);

  tv.tv_sec = CONFIG_VS_SOCIAL_IO_TIMEOUT_MS / 1000;
  tv.tv_usec = (CONFIG_VS_SOCIAL_IO_TIMEOUT_MS % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (res != NULL)
    {
      freeaddrinfo(res);
    }

  return sock;

errout:
  if (res != NULL)
    {
      freeaddrinfo(res);
    }

  return ret;
}

/****************************************************************************
 * Name: cloud_send_all
 ****************************************************************************/

static int cloud_send_all(int sock, const void *data, size_t len)
{
  const unsigned char *p = data;
  size_t sent = 0;

  while (sent < len)
    {
      ssize_t n = send(sock, p + sent, len - sent, 0);

      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          return n < 0 ? -errno : -ECONNRESET;
        }

      sent += (size_t)n;
    }

  return 0;
}

/****************************************************************************
 * Name: cloud_plain_http
 *
 * Description:
 *   One cleartext HTTP/1.1 exchange.  Connection: close throughout, which
 *   makes the read loop trivially correct and costs a handshake that is not
 *   worth pooling on a link this cheap -- the TLS path, where a handshake is
 *   expensive, is the one that pools.
 *
 *   The response body is left NUL-terminated for the JSON callers and its
 *   true length is reported through resp_len for the binary ones.
 *
 * Returned Value:
 *   The HTTP status code on a complete exchange, otherwise a negative errno.
 *   A 4xx or 5xx is a successful exchange: the caller decides what it means.
 *
 ****************************************************************************/

static int cloud_plain_http(const struct cloud_url_s *url, const char *method,
                            const vela_header_t *headers,
                            const void *body, size_t body_len,
                            char *resp, size_t resp_cap, size_t *resp_len)
{
  char *hdr = NULL;
  char *body_start;
  size_t hdr_cap = CLOUD_PATH_MAX + 512;
  size_t filled = 0;
  size_t header_len;
  size_t moved;
  long content_length = -1;
  bool chunked = false;
  bool header_done = false;
  int status = 0;
  int sock = -1;
  int pos = 0;
  int ret;

  if (resp == NULL || resp_cap < 2)
    {
      return -EINVAL;
    }

  resp[0] = '\0';
  if (resp_len != NULL)
    {
      *resp_len = 0;
    }

  hdr = malloc(hdr_cap);
  if (hdr == NULL)
    {
      return -ENOMEM;
    }

  sock = cloud_connect(url->host, url->port);
  if (sock < 0)
    {
      ret = sock;
      goto errout;
    }

#define CLOUD_HDR_APPEND(...)                                             \
  do                                                                      \
    {                                                                     \
      int _n = snprintf(hdr + pos, hdr_cap - (size_t)pos, __VA_ARGS__);    \
      if (_n < 0 || (size_t)(pos + _n) >= hdr_cap)                         \
        {                                                                 \
          ret = -E2BIG;                                                   \
          goto errout;                                                    \
        }                                                                 \
      pos += _n;                                                          \
    }                                                                     \
  while (0)

  CLOUD_HDR_APPEND("%s %s HTTP/1.1\r\n", method, url->path);

  /* Port 80 is implied by the scheme; naming it changes the Host header some
   * servers sign or route on, so it is omitted when it is the default.
   */

  if (strcmp(url->port, "80") == 0)
    {
      CLOUD_HDR_APPEND("Host: %s\r\n", url->host);
    }
  else
    {
      CLOUD_HDR_APPEND("Host: %s:%s\r\n", url->host, url->port);
    }

  CLOUD_HDR_APPEND("User-Agent: velasight/1.0\r\n");
  CLOUD_HDR_APPEND("Connection: close\r\n");
  CLOUD_HDR_APPEND("Accept: */*\r\n");

  if (headers != NULL)
    {
      const vela_header_t *h;

      for (h = headers; h->name != NULL; h++)
        {
          CLOUD_HDR_APPEND("%s: %s\r\n", h->name, h->value);
        }
    }

  /* Sent even when zero.  A PUT or DELETE with neither a length nor a
   * transfer encoding leaves the server guessing whether a body follows, and
   * some answer 411 rather than guess.
   */

  CLOUD_HDR_APPEND("Content-Length: %zu\r\n", body_len);
  CLOUD_HDR_APPEND("\r\n");

#undef CLOUD_HDR_APPEND

  ret = cloud_send_all(sock, hdr, (size_t)pos);
  if (ret < 0)
    {
      goto errout;
    }

  if (body != NULL && body_len > 0)
    {
      ret = cloud_send_all(sock, body, body_len);
      if (ret < 0)
        {
          goto errout;
        }
    }

  /* Read the header section.  It is text and arrives before any body byte,
   * so scanning the accumulated buffer for the terminator is safe even when
   * the body that follows is binary.
   */

  while (filled < resp_cap - 1)
    {
      ssize_t n = recv(sock, resp + filled, resp_cap - 1 - filled, 0);

      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          break;
        }

      filled += (size_t)n;
      resp[filled] = '\0';

      if (strstr(resp, "\r\n\r\n") != NULL)
        {
          header_done = true;
          break;
        }
    }

  if (!header_done)
    {
      ret = filled == 0 ? -ETIMEDOUT : -EPROTO;
      goto errout;
    }

  if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1 || status < 100)
    {
      printf("%s: malformed status line\n", CLOUD_TAG);
      ret = -EPROTO;
      goto errout;
    }

  body_start = strstr(resp, "\r\n\r\n") + 4;
  header_len = (size_t)(body_start - resp);

  /* Both header scans have to happen before the body is moved over the
   * header, which is what the next step does.
   */

  {
    char *cl = strcasestr(resp, "\r\nContent-Length:");
    char *te = strcasestr(resp, "\r\nTransfer-Encoding:");

    if (cl != NULL && cl < body_start)
      {
        content_length = strtol(cl + strlen("\r\nContent-Length:"), NULL, 10);
        if (content_length < 0)
          {
            content_length = -1;
          }
      }

    if (te != NULL && te < body_start)
      {
        const char *eol = strstr(te + 2, "\r\n");

        chunked = eol != NULL && eol <= body_start &&
                  cloud_range_has_token(te, eol, "chunked");
      }
  }

  /* A body the caller cannot hold is refused before it is read.  Truncating
   * it and returning 200 would hand back JSON that fails to parse, and the
   * -EPROTO that produced would send someone looking for a parser bug when
   * the real answer is that CONFIG_VS_SOCIAL_RESP_MAX_BYTES is too small.
   * Content-Length is the only advance warning available; a chunked body has
   * none, so that case is caught after the fact below.
   */

  if (content_length >= 0 && (size_t)content_length > resp_cap - 1)
    {
      printf("%s: response is %ld bytes, buffer holds %zu\n", CLOUD_TAG,
             content_length, resp_cap - 1);
      ret = -E2BIG;
      goto errout;
    }

  moved = filled - header_len;
  memmove(resp, body_start, moved);
  filled = moved;

  while (filled < resp_cap - 1)
    {
      ssize_t n;

      if (!chunked && content_length >= 0 && (long)filled >= content_length)
        {
          break;
        }

      n = recv(sock, resp + filled, resp_cap - 1 - filled, 0);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          break;
        }

      filled += (size_t)n;
    }

  /* Terminate before decoding: cloud_decode_chunked() reads the buffer as
   * text and the loop above may have filled it to the last usable byte.
   */

  resp[filled] = '\0';

  if (chunked)
    {
      if (filled >= resp_cap - 1)
        {
          printf("%s: chunked response filled the %zu byte buffer\n",
                 CLOUD_TAG, resp_cap - 1);
          ret = -E2BIG;
          goto errout;
        }

      filled = cloud_decode_chunked(resp, filled);
      resp[filled] = '\0';
    }
  else if (content_length >= 0 && (long)filled < content_length)
    {
      /* The peer stopped early.  Reporting the HTTP status here would pass a
       * partial Ogg download or half a JSON document off as complete, and
       * both of those fail later somewhere less obvious.
       */

      printf("%s: body ended at %zu of %ld bytes\n", CLOUD_TAG, filled,
             content_length);
      ret = -EIO;
      goto errout;
    }

  if (resp_len != NULL)
    {
      *resp_len = filled;
    }

  ret = status;

errout:
  if (sock >= 0)
    {
      close(sock);
    }

  free(hdr);
  return ret;
}

/****************************************************************************
 * Name: cloud_http
 *
 * Description:
 *   Dispatch one exchange to the right transport and normalise the result.
 *   vela_tls.c reports its own error space (VELA_TLS_ERR_*); everything past
 *   this point sees HTTP status codes and negative errnos only.
 *
 ****************************************************************************/

static int cloud_http(const struct cloud_url_s *url, const char *method,
                      const vela_header_t *headers,
                      const void *body, size_t body_len,
                      char *resp, size_t resp_cap, size_t *resp_len)
{
  int ret;

  if (!url->tls)
    {
      return cloud_plain_http(url, method, headers, body, body_len,
                              resp, resp_cap, resp_len);
    }

  ret = vela_https_request(url->host, url->port, method, url->path, headers,
                           (const char *)body, body_len, resp, resp_cap,
                           resp_len);
  if (ret > 0)
    {
      return ret;
    }

  switch (ret)
    {
      case VELA_TLS_ERR_CONNECT:
        return -ECONNREFUSED;

      case VELA_TLS_ERR_HANDSHAKE:
        return -EPROTO;

      case VELA_TLS_ERR_WRITE:
      case VELA_TLS_ERR_READ:
        return -EIO;

      /* Raised when the request header does not fit vela_tls.c's own 4 KiB
       * builder -- not when the response is too large for resp_buf, which
       * that path does not detect at all.  A presigned URL long enough to
       * overflow it is the realistic cause.
       */

      case VELA_TLS_ERR_OVERFLOW:
        return -E2BIG;

      default:
        return ret < 0 ? ret : -EIO;
    }
}

/****************************************************************************
 * Name: cloud_api_call
 *
 * Description:
 *   One exchange with a /contest/v1 endpoint on the configured host.
 *   path_and_query is appended to the configured origin, so callers deal in
 *   paths rather than URLs.
 *
 * Returned Value:
 *   HTTP status code, or a negative errno.
 *
 ****************************************************************************/

static int cloud_api_call(const char *method, const char *path_and_query,
                          const char *json_body, char *resp, size_t resp_cap)
{
  struct cloud_url_s *url;
  vela_header_t headers[2];
  int ret;

  if (!g_cloud.configured)
    {
      return -ENODATA;
    }

  url = malloc(sizeof(*url));
  if (url == NULL)
    {
      return -ENOMEM;
    }

  memset(url, 0, sizeof(*url));
  url->tls = g_cloud.tls;
  snprintf(url->host, sizeof(url->host), "%s", g_cloud.host);
  snprintf(url->port, sizeof(url->port), "%s", g_cloud.port);

  /* The base path goes on here rather than into the four CLOUD_PATH_*
   * literals, so those keep the document's own spelling and a grep for a path
   * still finds it.  This is also the only place all four endpoints pass
   * through, which is what makes one concatenation enough.
   *
   * No slash is inserted: the prefix is stored with a leading slash and
   * without a trailing one (vp_cloud_path_ok() enforces both), and every
   * path_and_query starts with one.  An empty prefix therefore also works
   * without a special case.
   */

  if (snprintf(url->path, sizeof(url->path), "%s%s", g_cloud.base_path,
               path_and_query) >= (int)sizeof(url->path))
    {
      free(url);
      return -E2BIG;
    }

  headers[0].name = "Content-Type";
  headers[0].value = "application/json";
  headers[1].name = NULL;
  headers[1].value = NULL;

  ret = cloud_http(url, method, headers, json_body,
                   json_body != NULL ? strlen(json_body) : 0,
                   resp, resp_cap, NULL);

  free(url);
  return ret;
}

/****************************************************************************
 * Name: cloud_json_int
 *
 * Description:
 *   Read an integer that may have been sent either as a number or as a
 *   string.  The cloud document does both for the same fields -- msgEvent
 *   appears as 0 in one example and "0" in another -- so accepting only one
 *   form would make the parser depend on which example the server was
 *   written from.
 *
 ****************************************************************************/

static bool cloud_json_int(const cJSON *obj, const char *name, int *out)
{
  const cJSON *item;

  if (obj == NULL)
    {
      return false;
    }

  item = cJSON_GetObjectItem((cJSON *)obj, name);
  if (item == NULL)
    {
      return false;
    }

  if (cJSON_IsNumber(item))
    {
      *out = item->valueint;
      return true;
    }

  if (cJSON_IsString(item) && item->valuestring != NULL)
    {
      char *endp = NULL;
      long v = strtol(item->valuestring, &endp, 10);

      if (endp != item->valuestring)
        {
          *out = (int)v;
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: cloud_json_str
 *
 * Description:
 *   Copy a string member out, truncating on a UTF-8 boundary.  Returns false
 *   when the member is absent or is not a string, leaving dst empty.
 *
 ****************************************************************************/

static bool cloud_json_str(const cJSON *obj, const char *name, char *dst,
                           size_t cap)
{
  const cJSON *item;

  if (dst != NULL && cap > 0)
    {
      dst[0] = '\0';
    }

  if (obj == NULL)
    {
      return false;
    }

  item = cJSON_GetObjectItem((cJSON *)obj, name);
  if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL)
    {
      return false;
    }

  cloud_copy_utf8(dst, cap, item->valuestring);
  return true;
}

/****************************************************************************
 * Name: cloud_json_exact
 *
 * Description:
 *   Copy a string member out, or fail if it does not fit.  For identifiers
 *   and URLs, which are the members where truncation is not a cosmetic loss:
 *   a shortened msgId names nothing and a shortened presigned URL fails its
 *   signature check with no indication of why.  Display text keeps using
 *   cloud_json_str(), where truncating is the right answer.
 *
 * Returned Value:
 *   true when the member was present, a string, and fitted.  dst is left
 *   empty on any failure.
 *
 ****************************************************************************/

static bool cloud_json_exact(const cJSON *obj, const char *name, char *dst,
                             size_t cap)
{
  const cJSON *item;

  if (dst == NULL || cap == 0)
    {
      return false;
    }

  dst[0] = '\0';

  if (obj == NULL)
    {
      return false;
    }

  item = cJSON_GetObjectItem((cJSON *)obj, name);
  if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL)
    {
      return false;
    }

  if (strlen(item->valuestring) + 1 > cap)
    {
      printf("%s: %s is %zu bytes, buffer holds %zu\n", CLOUD_TAG, name,
             strlen(item->valuestring), cap - 1);
      return false;
    }

  memcpy(dst, item->valuestring, strlen(item->valuestring) + 1);
  return true;
}

/****************************************************************************
 * Name: cloud_json_confidence
 *
 * Description:
 *   confidence arrives as a string in the range 0..1 ("0.95").  Returned as
 *   whole percent; 101 means absent, which the UI treats as "do not show a
 *   number" rather than as zero confidence.
 *
 ****************************************************************************/

static uint16_t cloud_json_confidence(const cJSON *obj)
{
  const cJSON *item;
  double v;

  if (obj == NULL)
    {
      return 101;
    }

  item = cJSON_GetObjectItem((cJSON *)obj, "confidence");
  if (item == NULL)
    {
      return 101;
    }

  if (cJSON_IsNumber(item))
    {
      v = item->valuedouble;
    }
  else if (cJSON_IsString(item) && item->valuestring != NULL)
    {
      char *endp = NULL;

      v = strtod(item->valuestring, &endp);
      if (endp == item->valuestring)
        {
          return 101;
        }
    }
  else
    {
      return 101;
    }

  /* A value above 1 is read as a percentage rather than a fraction.
   *
   * This is a guess, not a decidable question: an integer 1 could mean one
   * percent and is treated as 100%.  The document only ever shows the
   * fractional string form ("0.95"), so the guess costs nothing there, and
   * the alternative -- rejecting anything above 1 -- would throw away a
   * usable value from a server that switched to percent.
   */

  if (v > 1.0)
    {
      v = v / 100.0;
    }

  if (v < 0.0)
    {
      v = 0.0;
    }

  if (v > 1.0)
    {
      v = 1.0;
    }

  return (uint16_t)((v * 100.0) + 0.5);
}

/****************************************************************************
 * Name: cloud_classify_emotion
 *
 * Description:
 *   Turn the cloud's (emotionColor, emotionDetail) pair into the UI's
 *   vocabulary.
 *
 *   The colour is the authoritative part -- the document fixes the three
 *   buckets and enumerates which details fall in each -- so the detail is
 *   consulted only to split green, whose two members (愉悦, 中立) are the one
 *   place where a single colour spans two of the UI's emotions.
 *
 *   The RGB values are the palette vs_app.c already uses for its own
 *   defaults, so a cloud-driven colour and a locally-derived one look the
 *   same.  Blue has no local equivalent (vs_app.c maps CONFUSED to amber) and
 *   gets a real blue here, because the cloud's three-colour scheme is
 *   something the user is meant to be able to read off the screen.
 *
 * Input Parameters:
 *   extreme - set true only for the red bucket.  This is the device's only
 *             way to tell an extreme frame from a calm one: the peer-side
 *             status collapses the cloud's 20 and 21 into a single 20.
 *
 ****************************************************************************/

static void cloud_classify_emotion(const char *color, const char *detail,
                                   enum vs_emotion_e *emotion, uint32_t *rgb,
                                   bool *extreme)
{
  *emotion = VS_EMOTION_NONE;
  *rgb = 0xe8eef2;
  *extreme = false;

  if (color == NULL)
    {
      return;
    }

  if (strcasecmp(color, "red") == 0)
    {
      *emotion = VS_EMOTION_TENSE;
      *rgb = 0xe85d5d;
      *extreme = true;
    }
  else if (strcasecmp(color, "blue") == 0)
    {
      *emotion = VS_EMOTION_CONFUSED;
      *rgb = 0x5d8fe8;
    }
  else if (strcasecmp(color, "green") == 0)
    {
      if (detail != NULL && strstr(detail, "愉悦") != NULL)
        {
          *emotion = VS_EMOTION_HAPPY;
          *rgb = 0x48c78e;
        }
      else
        {
          *emotion = VS_EMOTION_CALM;
          *rgb = 0xe8eef2;
        }
    }
}

/****************************************************************************
 * Name: cloud_check_envelope
 *
 * Description:
 *   Validate the parts every /contest/v1 response shares and hand back the
 *   "value" member.
 *
 *   The sessionId echo is checked rather than trusted.  One deviceId may only
 *   have one live session, and the cloud replaces an old session with a newer
 *   one, so a reply carrying a different sessionId means this session has
 *   already been superseded -- continuing to feed it frames would attribute
 *   them to a session that no longer exists.
 *
 * Returned Value:
 *   0 with *status and *value filled, or a negative errno.
 *
 ****************************************************************************/

static int cloud_check_envelope(cJSON *root, const char *session_id,
                                int *status, cJSON **value)
{
  char echoed[VS_CLOUD_SESSION_ID_MAX];

  if (root == NULL || !cJSON_IsObject(root))
    {
      return -EPROTO;
    }

  if (!cloud_json_int(root, "status", status))
    {
      printf("%s: response has no status\n", CLOUD_TAG);
      return -EPROTO;
    }

  if (session_id != NULL &&
      cloud_json_exact(root, "sessionId", echoed, sizeof(echoed)) &&
      echoed[0] != '\0' && strcmp(echoed, session_id) != 0)
    {
      printf("%s: session mismatch, sent %s got %s\n", CLOUD_TAG,
             session_id, echoed);
      return -ESTALE;
    }

  *value = cJSON_GetObjectItem(root, "value");
  return 0;
}

/****************************************************************************
 * Name: cloud_value_log
 *
 * Description:
 *   Copy the failure explanation out of a response, wherever it sits.  The
 *   document puts it in value.log for the session and upload endpoints and at
 *   the top level of a per-message getResult entry, so both are checked.
 *
 ****************************************************************************/

static void cloud_value_log(cJSON *value, cJSON *root, char *dst, size_t cap)
{
  if (dst == NULL || cap == 0)
    {
      return;
    }

  dst[0] = '\0';

  if (value != NULL && cloud_json_str(value, "log", dst, cap) &&
      dst[0] != '\0')
    {
      return;
    }

  if (root != NULL)
    {
      (void)cloud_json_str(root, "log", dst, cap);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool vs_cloud_server_to_peer(int state, enum vs_cloud_peer_state_e *out)
{
  if (out == NULL)
    {
      return false;
    }

  switch (state)
    {
      case VS_CLOUD_SRV_SESSION_OPEN:
        *out = VS_CLOUD_PEER_SESSION_OPEN;
        return true;

      case VS_CLOUD_SRV_IMAGE_PENDING:
        *out = VS_CLOUD_PEER_EMOTION_ANALYZING;
        return true;

      case VS_CLOUD_SRV_AUDIO_PENDING:
        *out = VS_CLOUD_PEER_ADVICE_PENDING;
        return true;

      /* Both the calm and the extreme verdict arrive as one peer state; the
       * distinction survives only in emotionColor.
       */

      case VS_CLOUD_SRV_IMAGE_CALM:
      case VS_CLOUD_SRV_IMAGE_EXTREME:
        *out = VS_CLOUD_PEER_EMOTION_DONE;
        return true;

      case VS_CLOUD_SRV_AUDIO_DONE:
      case VS_CLOUD_SRV_CLOSED:
        *out = VS_CLOUD_PEER_ADVICE_DONE;
        return true;

      case VS_CLOUD_SRV_IMAGE_NO_FACE:
      case VS_CLOUD_SRV_IMAGE_UNRECOGNIZED:
      case VS_CLOUD_SRV_AUDIO_NO_SPEECH:
      case VS_CLOUD_SRV_AUDIO_UNRECOGNIZED:
        *out = VS_CLOUD_PEER_FAILED;
        return true;

      case VS_CLOUD_SRV_CLOSING:
        *out = VS_CLOUD_PEER_CLOSING;
        return true;

      default:
        return false;
    }
}

/****************************************************************************
 * Name: cloud_resolve_device_id
 *
 * Description:
 *   Derive the device identifier from the wlan0 MAC, falling back to the
 *   Kconfig override and then to a per-boot random value.
 *
 *   Separated out and callable more than once because of when it first runs.
 *   vs_cloud_init() is on the startup path, and the driver only populates the
 *   netdev's MAC from BK7258_WIFI_CMD_GET_MAC_ADDR during Wi-Fi bring-up --
 *   which the network worker is still doing at that point.  A read that lands
 *   before that returns zeros, and the fallback then quietly gives up the
 *   across-reboot stability that is the whole reason for using the MAC.  So
 *   vs_cloud_social_open() calls this again while the identifier is still
 *   provisional, by which time the interface has long settled.
 *
 *   Only ever called from the startup task or from a session open, so the
 *   write to g_cloud does not race a concurrent upload or poll: those read
 *   the identifier but never resolve it.
 *
 ****************************************************************************/

static void cloud_resolve_device_id(void)
{
  uint8_t mac[6];

  if (netlib_getmacaddr("wlan0", mac) == 0 &&
      (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0)
    {
      char derived[VS_CLOUD_DEVICE_ID_MAX];

      snprintf(derived, sizeof(derived), "bk7258-%02x%02x%02x%02x%02x%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      if (g_cloud.device_id_stable && strcmp(derived, g_cloud.device_id) == 0)
        {
          return;
        }

      if (g_cloud.device_id[0] != '\0')
        {
          printf("%s: device id now %s (was provisional %s)\n", CLOUD_TAG,
                 derived, g_cloud.device_id);
        }

      snprintf(g_cloud.device_id, sizeof(g_cloud.device_id), "%s", derived);
      g_cloud.device_id_stable = true;
    }
  else if (g_cloud.device_id_stable)
    {
      /* Already holding a MAC-derived value; a later failed read does not
       * invalidate it.
       */

      return;
    }
  else if (CONFIG_VS_SOCIAL_DEVICE_ID[0] != '\0')
    {
      snprintf(g_cloud.device_id, sizeof(g_cloud.device_id), "%s",
               CONFIG_VS_SOCIAL_DEVICE_ID);
      g_cloud.device_id_stable = true;
    }
  else if (g_cloud.device_id[0] == '\0')
    {
      /* Last resort: unique for this boot only, and provisional -- the next
       * call retries the MAC.  Logged because it makes cloud-side logs
       * impossible to follow across a reset, which is confusing unless you
       * know why.
       */

      char suffix[16];

      if (vs_cloud_new_session_id(suffix, sizeof(suffix)) < 0)
        {
          snprintf(suffix, sizeof(suffix), "vs-264-00000000");
        }

      snprintf(g_cloud.device_id, sizeof(g_cloud.device_id), "bk7258-%s",
               suffix + 7);
      printf("%s: wlan0 MAC not readable yet, provisional device id %s\n",
             CLOUD_TAG, g_cloud.device_id);
    }

  if (!cloud_id_valid(g_cloud.device_id, sizeof(g_cloud.device_id)))
    {
      printf("%s: device id %s is not usable\n", CLOUD_TAG,
             g_cloud.device_id);
      snprintf(g_cloud.device_id, sizeof(g_cloud.device_id),
               "bk7258-invalid");
      g_cloud.device_id_stable = true;
    }
}

/****************************************************************************
 * Name: cloud_apply_endpoint
 *
 * Description:
 *   Write one resolved endpoint into module state.  host may be NULL or
 *   empty, which leaves the module unconfigured.  base_path may be NULL or
 *   empty, meaning the interface is at the document root.
 *
 *   Returns true when anything actually changed, so a reload can report "no
 *   change" instead of claiming a swap that did not happen.
 *
 ****************************************************************************/

static bool cloud_apply_endpoint(const char *host, uint16_t port,
                                 const char *base_path, bool tls,
                                 enum vs_cloud_origin_e origin)
{
  char next_host[CLOUD_HOST_MAX];
  char next_path[CLOUD_BASE_PATH_MAX];
  char next_port[CLOUD_PORT_MAX];
  bool changed;

  snprintf(next_host, sizeof(next_host), "%s", host != NULL ? host : "");
  snprintf(next_path, sizeof(next_path), "%s",
           base_path != NULL ? base_path : "");
  snprintf(next_port, sizeof(next_port), "%u", (unsigned int)port);

  changed = strcmp(next_host, g_cloud.host) != 0 ||
            strcmp(next_path, g_cloud.base_path) != 0 ||
            strcmp(next_port, g_cloud.port) != 0 ||
            tls != g_cloud.tls;

  memcpy(g_cloud.host, next_host, sizeof(g_cloud.host));
  memcpy(g_cloud.base_path, next_path, sizeof(g_cloud.base_path));
  memcpy(g_cloud.port, next_port, sizeof(g_cloud.port));
  g_cloud.port_value = port;
  g_cloud.tls        = tls;
  g_cloud.configured = g_cloud.host[0] != '\0';
  g_cloud.origin     = g_cloud.configured ? origin : VS_CLOUD_ORIGIN_NONE;
  return changed;
}

/****************************************************************************
 * Name: cloud_resolve_endpoint
 *
 * Description:
 *   Decide which endpoint to use and install it.
 *
 *   The provisioning record wins field by field rather than as a whole.  A
 *   user who fills in only the host on the setup page means "that host, with
 *   the usual port and prefix", and requiring all three together would make
 *   the common case -- pointing a board at a local mock on the documented
 *   prefix -- impossible to express.
 *
 *   The origin reported is PROVISIONED when any of the three came from the
 *   record.  That is the honest answer for a log line whose job is to say
 *   whether the stored record had any say in the address being used.
 *
 * Returned Value:
 *   true when the live endpoint changed.
 *
 ****************************************************************************/

static bool cloud_resolve_endpoint(void)
{
  struct velasight_prov_credentials_s *cred;
  const char *host = VELASIGHT_PROV_CLOUD_HOST_DEFAULT;
  const char *path = VELASIGHT_PROV_CLOUD_PATH_DEFAULT;
  uint16_t port    = VELASIGHT_PROV_CLOUD_PORT_DEFAULT;
  enum vs_cloud_origin_e origin = VS_CLOUD_ORIGIN_DEFAULT;
  bool changed;
  bool tls;
  int ret;

  /* Heap rather than a 976-byte local.  This runs on the network worker
   * pthread (CONFIG_PTHREAD_STACK_DEFAULT, 4 KiB) via vs_network_open(); see
   * vp_store.c's vp_record_decode() for the incident that made this struct's
   * stack footprint worth caring about on every thread that touches it.
   *
   * host/path below may end up pointing into *cred, so cred stays alive
   * until after cloud_apply_endpoint() has copied them out.
   */

  cred = malloc(sizeof(*cred));
  if (cred == NULL)
    {
      return cloud_apply_endpoint(host, port, path, false, origin);
    }

  /* Kconfig still decides the scheme.  It is not in the record because it is
   * not a field a user can get right from a phone: turning TLS on against a
   * cloud that only speaks cleartext fails in the handshake, with nothing on
   * the page to explain it, and this board has no CA bundle to make the
   * encrypted case actually authenticated.  See CONFIG_VS_SOCIAL_CLOUD_TLS.
   */

#ifdef CONFIG_VS_SOCIAL_CLOUD_TLS
  tls = true;
#else
  tls = false;
#endif

  /* A Kconfig host, when set, overrides the shared factory default.  This is
   * what makes a build aimed at a local mock work without provisioning: the
   * option existed before the record did, and a build that sets it is asking
   * for that endpoint deliberately.
   */

  if (CONFIG_VS_SOCIAL_CLOUD_HOST[0] != '\0')
    {
      host = CONFIG_VS_SOCIAL_CLOUD_HOST;
      port = (uint16_t)CONFIG_VS_SOCIAL_CLOUD_PORT;
    }

  ret = velasight_provisioning_load(cred);
  if (ret == 0)
    {
      if (cred->cloud_host[0] != '\0')
        {
          host   = cred->cloud_host;
          origin = VS_CLOUD_ORIGIN_PROVISIONED;
        }

      if (cred->cloud_port != 0)
        {
          port   = cred->cloud_port;
          origin = VS_CLOUD_ORIGIN_PROVISIONED;
        }

      /* An empty stored prefix cannot be told from "not set" here, and the
       * two want opposite things: a device deliberately talking to a server
       * that hosts the interface at its root needs the empty value to win.
       *
       * Resolved in favour of the record whenever the record supplied any
       * endpoint field at all.  A user who typed a host and cleared the
       * prefix box asked for exactly that combination; a record that carries
       * no endpoint at all falls through to the default prefix.
       */

      if (origin == VS_CLOUD_ORIGIN_PROVISIONED ||
          cred->cloud_path[0] != '\0')
        {
          path = cred->cloud_path;
          origin = VS_CLOUD_ORIGIN_PROVISIONED;
        }
    }
  else if (ret != -ENOENT)
    {
      /* A corrupt record is not a reason to refuse to run: the default
       * endpoint is a working address.  It is a reason to say so, because the
       * device is now using a different host than the page would show.
       */

      printf("%s: provisioning record unreadable (%d), using default "
             "endpoint\n", CLOUD_TAG, ret);
    }

  changed = cloud_apply_endpoint(host, port, path, tls, origin);
  free(cred);
  return changed;
}

static const char *cloud_origin_text(void)
{
  switch (g_cloud.origin)
    {
      case VS_CLOUD_ORIGIN_PROVISIONED:
        return "provisioned";
      case VS_CLOUD_ORIGIN_DEFAULT:
        return "default";
      default:
        return "unset";
    }
}

int vs_cloud_init(void)
{
  if (g_cloud.initialized)
    {
      return g_cloud.configured ? 0 : -ENODATA;
    }

  memset(&g_cloud, 0, sizeof(g_cloud));
  g_cloud.initialized = true;

  /* Deliberately no vp_store read here.  This runs on vs_app_run()'s startup
   * path, which the SD-NAND rule (docs/SD-NAND使用说明.md) forbids for
   * exactly the reason a real board demonstrated: the AP's IPC heartbeat
   * thread has a fixed 3 s budget from the CP side, that path already spends
   * most of it on bk7258_nand_seed_agent_config() and vs_history_open(), and
   * one more synchronous VFAT read -- 1-bit PIO, single-block, "near-second"
   * per the same doc -- was enough to push the total past the deadline.  The
   * CP's mb_ipc_task then asserts and the board reboots in a tight loop, the
   * heartbeat thread never having had a chance to run.
   *
   * The compiled-in default is applied here instead, which touches nothing
   * but memory.  The provisioned endpoint, if any, is picked up by
   * vs_cloud_reload_endpoint() the first time the network worker thread runs
   * -- off the startup path, same as every other SD-NAND-backed config this
   * module's neighbours load (vs_config_load_wifi(), vs_voice's credential
   * seed).
   */

  cloud_apply_endpoint(VELASIGHT_PROV_CLOUD_HOST_DEFAULT,
                       VELASIGHT_PROV_CLOUD_PORT_DEFAULT,
                       VELASIGHT_PROV_CLOUD_PATH_DEFAULT,
#ifdef CONFIG_VS_SOCIAL_CLOUD_TLS
                       true,
#else
                       false,
#endif
                       VS_CLOUD_ORIGIN_DEFAULT);

  if (CONFIG_VS_SOCIAL_CLOUD_HOST[0] != '\0')
    {
      cloud_apply_endpoint(CONFIG_VS_SOCIAL_CLOUD_HOST,
                           (uint16_t)CONFIG_VS_SOCIAL_CLOUD_PORT,
                           VELASIGHT_PROV_CLOUD_PATH_DEFAULT,
#ifdef CONFIG_VS_SOCIAL_CLOUD_TLS
                           true,
#else
                           false,
#endif
                           VS_CLOUD_ORIGIN_DEFAULT);
    }

  /* Device identifier.  The MAC is the only value available that is both
   * unique and stable without storage.
   */

  cloud_resolve_device_id();

  if (!g_cloud.configured)
    {
      printf("%s: no cloud host configured, social mode will not start\n",
             CLOUD_TAG);
      return -ENODATA;
    }

  printf("%s: endpoint %s://%s:%s%s (%s, provisioning read deferred) "
         "device %s\n", CLOUD_TAG, g_cloud.tls ? "https" : "http",
         g_cloud.host, g_cloud.port, g_cloud.base_path,
         cloud_origin_text(), g_cloud.device_id);
  return 0;
}

int vs_cloud_reload_endpoint(void)
{
  bool changed;

  if (!g_cloud.initialized)
    {
      return vs_cloud_init();
    }

  if (g_cloud.session_live)
    {
      /* Deferred rather than dropped.  The identifiers of a live session only
       * exist on the host that issued them, so moving the endpoint now would
       * make the poll loop ask a stranger about msgIds it never saw and read
       * the resulting failures as the session going wrong.
       */

      g_cloud.reload_pending = true;
      printf("%s: endpoint reload deferred, a session is open\n", CLOUD_TAG);
      return -EBUSY;
    }

  changed = cloud_resolve_endpoint();
  g_cloud.reload_pending = false;

  if (changed)
    {
      printf("%s: endpoint now %s://%s:%s%s (%s)\n", CLOUD_TAG,
             g_cloud.tls ? "https" : "http", g_cloud.host, g_cloud.port,
             g_cloud.base_path, cloud_origin_text());
    }

  return changed ? 1 : 0;
}

enum vs_cloud_origin_e vs_cloud_origin(void)
{
  return g_cloud.origin;
}

void vs_cloud_endpoint(const char **host, uint16_t *port,
                       const char **base_path, bool *tls)
{
  if (host != NULL)
    {
      *host = g_cloud.host;
    }

  if (port != NULL)
    {
      *port = g_cloud.port_value;
    }

  if (base_path != NULL)
    {
      *base_path = g_cloud.base_path;
    }

  if (tls != NULL)
    {
      *tls = g_cloud.tls;
    }
}

bool vs_cloud_configured(void)
{
  return g_cloud.configured;
}

const char *vs_cloud_device_id(void)
{
  return g_cloud.device_id[0] != '\0' ? g_cloud.device_id : "bk7258-unknown";
}

int vs_cloud_new_session_id(char *out, size_t cap)
{
  unsigned char seed[4];
  size_t got = 0;
  int fd;

  if (out == NULL || cap < 16)
    {
      return -EINVAL;
    }

  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  while (got < sizeof(seed))
    {
      ssize_t n = read(fd, seed + got, sizeof(seed) - got);

      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          close(fd);
          return n < 0 ? -errno : -EIO;
        }

      got += (size_t)n;
    }

  close(fd);

  snprintf(out, cap, "vs-264-%02x%02x%02x%02x", seed[0], seed[1], seed[2],
           seed[3]);
  return 0;
}

int vs_cloud_social_open(struct vs_cloud_session_s *session)
{
  char body[192];
  char *resp = NULL;
  bool from_psram = false;
  cJSON *root = NULL;
  cJSON *value = NULL;
  int status = 0;
  int http;
  int ret;

  if (session == NULL)
    {
      return -EINVAL;
    }

  /* A reload that arrived during the previous session is applied here, before
   * anything is sent.  This is the first moment it is safe: the old session's
   * identifiers are no longer going to be asked about, and the new session's
   * do not exist yet.
   *
   * Deliberately before the configured check below, because the reload is
   * what may make an unconfigured module configured.
   */

  if (g_cloud.reload_pending)
    {
      g_cloud.session_live   = false;
      g_cloud.reload_pending = false;

      if (cloud_resolve_endpoint())
        {
          printf("%s: endpoint now %s://%s:%s%s (%s)\n", CLOUD_TAG,
                 g_cloud.tls ? "https" : "http", g_cloud.host, g_cloud.port,
                 g_cloud.base_path, cloud_origin_text());
        }
    }

  if (!g_cloud.configured)
    {
      return -ENODATA;
    }

  /* Last chance to upgrade a provisional identifier to the MAC-derived one:
   * a session's requests all have to carry the same value, so after this it
   * is fixed for the duration.
   */

  if (!g_cloud.device_id_stable)
    {
      cloud_resolve_device_id();
    }

  snprintf(session->device_id, sizeof(session->device_id), "%s",
           vs_cloud_device_id());

  if (session->session_id[0] == '\0')
    {
      ret = vs_cloud_new_session_id(session->session_id,
                                    sizeof(session->session_id));
      if (ret < 0)
        {
          printf("%s: cannot draw a session id: %d\n", CLOUD_TAG, ret);
          return ret;
        }
    }

  if (!cloud_id_valid(session->session_id, sizeof(session->session_id)))
    {
      return -EINVAL;
    }

  snprintf(body, sizeof(body),
           "{\"deviceId\":\"%s\",\"sessionId\":\"%s\",\"timestamp\":\"%llu\"}",
           session->device_id, session->session_id,
           (unsigned long long)cloud_now_ms());

  resp = cloud_alloc(CONFIG_VS_SOCIAL_REG_RESP_BYTES, &from_psram);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  http = cloud_api_call("PUT", CLOUD_PATH_SESSION, body, resp,
                        CONFIG_VS_SOCIAL_REG_RESP_BYTES);
  if (http < 0)
    {
      printf("%s: open transport failed: %d\n", CLOUD_TAG, http);
      ret = http;
      goto out;
    }

  if (http < 200 || http >= 300)
    {
      printf("%s: open returned HTTP %d\n", CLOUD_TAG, http);
      ret = -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  ret = cloud_check_envelope(root, session->session_id, &status, &value);
  if (ret < 0)
    {
      goto out;
    }

  if (status != 0)
    {
      char log[VS_TEXT_SHORT];

      cloud_value_log(value, root, log, sizeof(log));
      printf("%s: open refused, status %d%s%s\n", CLOUD_TAG, status,
             log[0] != '\0' ? ": " : "", log);

      /* 30 is the only failure code this endpoint has, so it covers both
       * "this deviceId already has a session with results outstanding" and
       * any server-side error.  -EBUSY is the more useful of the two to
       * report, because it is the one a caller can act on -- wait rather than
       * retry immediately -- and the log line above carries whatever the
       * cloud actually said.  Do not read more into it than that: nothing
       * here inspects value.log to tell the cases apart.
       */

      ret = status == 30 ? -EBUSY : -EIO;
      goto out;
    }

  printf("%s: session %s open\n", CLOUD_TAG, session->session_id);

  /* From here the endpoint is pinned until the close reaches a terminal
   * answer.  See cloud_state_s::session_live.
   */

  g_cloud.session_live = true;
  ret = 0;

out:
  cJSON_Delete(root);
  cloud_free(resp, from_psram);
  return ret;
}

int vs_cloud_social_upload(const char *session_id,
                           const struct vs_cloud_media_packet_s *packet,
                           struct vs_cloud_upload_s *out)
{
  char body[224];
  char presigned[CLOUD_PRESIGNED_PATH_MAX];
  char *resp = NULL;
  bool from_psram = false;
  cJSON *root = NULL;
  cJSON *value = NULL;
  struct cloud_url_s *url = NULL;
  vela_header_t headers[2];
  int status = 0;
  int http;
  int ret;

  if (session_id == NULL || packet == NULL || out == NULL ||
      packet->data == NULL || packet->len == 0)
    {
      return -EINVAL;
    }

  if (!g_cloud.configured)
    {
      return -ENODATA;
    }

  if (!cloud_id_valid(session_id, VS_CLOUD_SESSION_ID_MAX))
    {
      return -EINVAL;
    }

  memset(out, 0, sizeof(*out));

  /* Step one: register.  event is the reserved outer event type (emotion
   * recognition); msgEvent says which kind of file this is.
   */

  snprintf(body, sizeof(body),
           "{\"event\":%d,\"msgEvent\":%d,\"deviceId\":\"%s\","
           "\"sessionId\":\"%s\",\"timestamp\":\"%llu\"}",
           VS_CLOUD_EVENT_EMOTION,
           packet->type == VS_CLOUD_MEDIA_AUDIO ? VS_CLOUD_MEDIA_AUDIO
                                                : VS_CLOUD_MEDIA_IMAGE,
           vs_cloud_device_id(), session_id,
           (unsigned long long)cloud_now_ms());

  resp = cloud_alloc(CONFIG_VS_SOCIAL_REG_RESP_BYTES, &from_psram);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  http = cloud_api_call("POST", CLOUD_PATH_UPLOAD, body, resp,
                        CONFIG_VS_SOCIAL_REG_RESP_BYTES);
  if (http < 0)
    {
      ret = http;
      goto out;
    }

  if (http < 200 || http >= 300)
    {
      printf("%s: upload register returned HTTP %d\n", CLOUD_TAG, http);
      ret = -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  ret = cloud_check_envelope(root, session_id, &status, &value);
  if (ret < 0)
    {
      goto out;
    }

  /* 10 (image accepted) and 11 (audio accepted) are the documented success
   * codes; 0 is what the document's own response example shows for the same
   * case, so all three are accepted.  30 is the only failure.
   */

  if (status != 0 && status != 10 && status != 11)
    {
      char log[VS_TEXT_SHORT];

      cloud_value_log(value, root, log, sizeof(log));
      printf("%s: upload refused, status %d%s%s\n", CLOUD_TAG, status,
             log[0] != '\0' ? ": " : "", log);
      ret = -EIO;
      goto out;
    }

  if (!cloud_json_exact(value, "msgId", out->msg_id, sizeof(out->msg_id)) ||
      !cloud_id_valid(out->msg_id, sizeof(out->msg_id)))
    {
      printf("%s: upload response has no usable msgId\n", CLOUD_TAG);
      ret = -EPROTO;
      goto out;
    }

  if (!cloud_json_exact(value, "presignedUrl", presigned,
                        sizeof(presigned)))
    {
      printf("%s: upload response has no presignedUrl\n", CLOUD_TAG);
      ret = -EPROTO;
      goto out;
    }

  /* Step two: the bytes.  A bare PUT -- no multipart framing, which is what
   * the registration-plus-presigned-URL shape means.
   */

  url = malloc(sizeof(*url));
  if (url == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = cloud_url_parse(presigned, url);
  if (ret == -EPROTONOSUPPORT)
    {
      /* The cloud's mock mode answers with "mock://...".  Registration
       * succeeded and the msgId is still worth polling, so this is reported
       * through payload_sent rather than as a failure.
       */

      if (!g_cloud.mock_url_logged)
        {
          g_cloud.mock_url_logged = true;
          printf("%s: presigned URL %s has no transferable scheme; "
                 "registering only (mock cloud)\n", CLOUD_TAG, presigned);
        }

      out->payload_sent = false;
      ret = 0;
      goto out;
    }

  if (ret < 0)
    {
      printf("%s: presigned URL rejected: %d\n", CLOUD_TAG, ret);
      goto out;
    }

  headers[0].name = "Content-Type";
  headers[0].value = packet->type == VS_CLOUD_MEDIA_AUDIO ? "audio/ogg"
                                                          : "image/jpeg";
  headers[1].name = NULL;
  headers[1].value = NULL;

  http = cloud_http(url, "PUT", CLOUD_UPLOAD_SEND_CT ? headers : NULL,
                    packet->data, packet->len, resp,
                    CONFIG_VS_SOCIAL_REG_RESP_BYTES, NULL);
  if (http < 0)
    {
      printf("%s: %s transfer failed: %d\n", CLOUD_TAG,
             packet->type == VS_CLOUD_MEDIA_AUDIO ? "audio" : "image", http);
      ret = http;
      goto out;
    }

  if (http < 200 || http >= 300)
    {
      /* Object stores answer 403 when the signature did not cover exactly the
       * headers that were sent, which is the failure
       * CONFIG_VS_SOCIAL_UPLOAD_CONTENT_TYPE exists to let you flip without
       * a code change.  Worth naming, because the response body is usually
       * XML nobody reads.
       */

      /* The store's reply is sitting in resp and is the only thing that says
       * which of the two signing mismatches happened, so print the head of
       * it.  Bounded and on one line: it is XML, and the useful part is the
       * <Code> element near the front.
       */

      printf("%s: %s transfer returned HTTP %d%s: %.160s\n", CLOUD_TAG,
             packet->type == VS_CLOUD_MEDIA_AUDIO ? "audio" : "image", http,
             http == 403 ? " (check VS_SOCIAL_UPLOAD_CONTENT_TYPE)" : "",
             resp);
      ret = -EIO;
      goto out;
    }

  out->payload_sent = true;
  ret = 0;

out:
  free(url);
  cJSON_Delete(root);
  cloud_free(resp, from_psram);
  return ret;
}

/****************************************************************************
 * Name: cloud_parse_entry
 *
 * Description:
 *   Map one element of a getResult value array onto struct vs_social_event_s.
 *
 * Returned Value:
 *   true when the entry was understood.  A false is not fatal to the poll:
 *   the caller counts it and moves on, because one malformed entry among
 *   several must not discard the others.
 *
 ****************************************************************************/

static bool cloud_parse_entry(cJSON *entry, struct vs_social_event_s *out)
{
  enum vs_cloud_peer_state_e peer;
  cJSON *response;
  int msg_event = VS_CLOUD_MSG_EVENT_IMAGE;
  int status = 0;

  if (entry == NULL || !cJSON_IsObject(entry) || out == NULL)
    {
      return false;
    }

  memset(out, 0, sizeof(*out));
  out->confidence = 101;

  if (!cloud_json_exact(entry, "msgId", out->msg_id, sizeof(out->msg_id)) ||
      out->msg_id[0] == '\0')
    {
      return false;
    }

  if (!cloud_json_int(entry, "status", &status))
    {
      return false;
    }

  out->raw_status = status;

  switch (status)
    {
      case VS_CLOUD_PEER_SESSION_OPEN:
      case VS_CLOUD_PEER_EMOTION_ANALYZING:
      case VS_CLOUD_PEER_ADVICE_PENDING:
      case VS_CLOUD_PEER_EMOTION_DONE:
      case VS_CLOUD_PEER_ADVICE_DONE:
      case VS_CLOUD_PEER_FAILED:
      case VS_CLOUD_PEER_CLOSING:
        out->peer_state = (enum vs_cloud_peer_state_e)status;
        break;

      default:

        /* Not a peer code.  Try it as a server-side one before giving up --
         * see vs_cloud_server_to_peer().
         */

        if (!vs_cloud_server_to_peer(status, &peer))
          {
            printf("%s: msg %s has unknown status %d\n", CLOUD_TAG,
                   out->msg_id, status);
            return false;
          }

        printf("%s: msg %s carried server status %d, mapped to %d\n",
               CLOUD_TAG, out->msg_id, status, (int)peer);
        out->peer_state = peer;
        break;
    }

  /* msgEvent decides which handler the orchestration layer dispatches to and
   * which shape the response object has, so a missing or unrecognised value
   * cannot be defaulted.  Treating it as an image result -- which an earlier
   * version did -- means reading emotionColor out of an object that has none
   * and reporting the absence as a neutral emotion.
   */

  if (!cloud_json_int(entry, "msgEvent", &msg_event))
    {
      printf("%s: msg %s has no msgEvent\n", CLOUD_TAG, out->msg_id);
      return false;
    }

  switch (msg_event)
    {
      case VS_CLOUD_MSG_EVENT_IMAGE:
      case VS_CLOUD_MSG_EVENT_AUDIO:
      case VS_CLOUD_MSG_EVENT_CLOSE:
        out->msg_event = (enum vs_cloud_msg_event_e)msg_event;
        break;

      default:
        printf("%s: msg %s has unknown msgEvent %d\n", CLOUD_TAG,
               out->msg_id, msg_event);
        return false;
    }

  if (out->peer_state == VS_CLOUD_PEER_FAILED)
    {
      cloud_value_log(NULL, entry, out->log, sizeof(out->log));
      return true;
    }

  response = cJSON_GetObjectItem(entry, "response");
  if (response == NULL || !cJSON_IsObject(response))
    {
      return true;
    }

  out->has_response = true;

  if (out->msg_event == VS_CLOUD_MSG_EVENT_IMAGE)
    {
      char color[16];
      char detail[VS_TEXT_LONG];

      (void)cloud_json_exact(response, "emotionColor", color,
                             sizeof(color));
      (void)cloud_json_str(response, "emotionDetail", detail,
                           sizeof(detail));

      cloud_classify_emotion(color, detail, &out->emotion, &out->color,
                             &out->extreme);
      out->confidence = cloud_json_confidence(response);
      cloud_copy_utf8(out->display_text, sizeof(out->display_text), detail);
    }
  else if (out->msg_event == VS_CLOUD_MSG_EVENT_AUDIO)
    {
      (void)cloud_json_str(response, "advice", out->suggestion,
                           sizeof(out->suggestion));
      cloud_copy_utf8(out->display_text, sizeof(out->display_text),
                      out->suggestion);
    }

  /* A CLOSE entry's response is the session minutes, which are far larger
   * than this structure and are read out by vs_cloud_social_get_result()
   * instead.  Nothing to copy here.
   */

  return true;
}

/****************************************************************************
 * Name: cloud_get_result_call
 *
 * Description:
 *   Shared GET for both polling entry points.  Builds the query -- the
 *   interface takes its whole request as one urlencoded JSON document in a
 *   "data" parameter -- and returns the parsed root.
 *
 *   The caller owns *root and must cJSON_Delete() it.  resp does not have to
 *   outlive the parse: cJSON_Parse() copies every string it extracts into its
 *   own allocations, which is why vs_cloud_social_upload() can reuse its
 *   response buffer for the presigned transfer while the parsed registration
 *   is still live.
 *
 ****************************************************************************/

static int cloud_get_result_call(const char *session_id,
                                 const char *const *msg_ids, size_t msg_count,
                                 char *resp, size_t resp_cap, cJSON **root,
                                 cJSON **value)
{
  char *scratch = NULL;
  char *request;
  char *query;
  int prefix;
  int status = 0;
  int pos = 0;
  size_t i;
  int http;
  int ret;

  if (session_id == NULL || msg_ids == NULL || msg_count == 0 ||
      msg_count > CLOUD_POLL_MAX_IDS)
    {
      return -EINVAL;
    }

  if (!cloud_id_valid(session_id, VS_CLOUD_SESSION_ID_MAX))
    {
      return -EINVAL;
    }

  /* One allocation for both staging buffers rather than 2.5 KB of frame.
   * This runs on the poll thread and, through vs_cloud_social_get_result(),
   * on the finalize path -- both several calls deep on the 8 KiB stacks the
   * build files declare, with cloud_api_call() and cloud_plain_http() each
   * wanting a kilobyte of their own below this.
   *
   * Two buffers, not three: the encoded form is written straight after the
   * "?data=" prefix inside query rather than into a buffer of its own.  That
   * also keeps the two snprintf() destinations from being provably-disjoint
   * halves of one object, which -Wrestrict cannot see and warns about.
   */

  scratch = malloc(CLOUD_QUERY_REQ_MAX + CLOUD_QUERY_MAX);
  if (scratch == NULL)
    {
      return -ENOMEM;
    }

  request = scratch;
  query = request + CLOUD_QUERY_REQ_MAX;

  pos = snprintf(request, CLOUD_QUERY_REQ_MAX, "{\"msgId\":[");
  for (i = 0; i < msg_count; i++)
    {
      int n;

      if (!cloud_id_valid(msg_ids[i], VS_CLOUD_MSG_ID_MAX))
        {
          printf("%s: refusing to poll unusable msgId\n", CLOUD_TAG);
          ret = -EINVAL;
          goto errout;
        }

      n = snprintf(request + pos, CLOUD_QUERY_REQ_MAX - (size_t)pos,
                   "%s\"%s\"", i == 0 ? "" : ",", msg_ids[i]);
      if (n < 0 || (size_t)(pos + n) >= CLOUD_QUERY_REQ_MAX)
        {
          ret = -E2BIG;
          goto errout;
        }

      pos += n;
    }

  if (snprintf(request + pos, CLOUD_QUERY_REQ_MAX - (size_t)pos,
               "],\"deviceId\":\"%s\",\"sessionId\":\"%s\","
               "\"timestamp\":\"%llu\"}",
               vs_cloud_device_id(), session_id,
               (unsigned long long)cloud_now_ms()) >=
      (int)(CLOUD_QUERY_REQ_MAX - (size_t)pos))
    {
      ret = -E2BIG;
      goto errout;
    }

  prefix = snprintf(query, CLOUD_QUERY_MAX, "%s?data=",
                    CLOUD_PATH_GET_RESULT);
  if (prefix < 0 || (size_t)prefix >= CLOUD_QUERY_MAX)
    {
      ret = -E2BIG;
      goto errout;
    }

  ret = cloud_urlencode(request, query + prefix,
                        CLOUD_QUERY_MAX - (size_t)prefix);
  if (ret < 0)
    {
      goto errout;
    }

  http = cloud_api_call("GET", query, NULL, resp, resp_cap);

  /* Released before the parse below, which is the largest allocation in the
   * call and has no reason to overlap with this one.
   */

  free(scratch);
  scratch = NULL;

  if (http < 0)
    {
      return http;
    }

  if (http < 200 || http >= 300)
    {
      printf("%s: getResult returned HTTP %d\n", CLOUD_TAG, http);
      return -EIO;
    }

  *root = cJSON_Parse(resp);
  ret = cloud_check_envelope(*root, session_id, &status, value);
  if (ret < 0)
    {
      return ret;
    }

  if (status != 0)
    {
      char log[VS_TEXT_SHORT];

      cloud_value_log(*value, *root, log, sizeof(log));
      printf("%s: getResult refused, status %d%s%s\n", CLOUD_TAG, status,
             log[0] != '\0' ? ": " : "", log);
      return -EIO;
    }

  return 0;

errout:
  free(scratch);
  return ret;
}

int vs_cloud_social_poll_event(const char *session_id,
                               const char *const *msg_ids, size_t msg_count,
                               struct vs_social_event_s *out, size_t out_cap,
                               size_t *out_count)
{
  char *resp = NULL;
  bool from_psram = false;
  cJSON *root = NULL;
  cJSON *value = NULL;
  cJSON *entry;
  size_t written = 0;
  size_t dropped = 0;
  size_t malformed = 0;
  int ret;

  if (out == NULL || out_cap == 0 || out_count == NULL)
    {
      return -EINVAL;
    }

  *out_count = 0;

  if (!g_cloud.configured)
    {
      return -ENODATA;
    }

  resp = cloud_alloc(CONFIG_VS_SOCIAL_RESP_MAX_BYTES, &from_psram);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  ret = cloud_get_result_call(session_id, msg_ids, msg_count, resp,
                              CONFIG_VS_SOCIAL_RESP_MAX_BYTES, &root, &value);
  if (ret < 0)
    {
      goto out;
    }

  /* value is an array of per-message results on success and an object
   * carrying only a log on failure.  The failure case was already handled by
   * the status check, so anything but an array here is malformed.
   */

  if (value == NULL || !cJSON_IsArray(value))
    {
      printf("%s: getResult value is not an array\n", CLOUD_TAG);
      ret = -EPROTO;
      goto out;
    }

  cJSON_ArrayForEach(entry, value)
    {
      if (written >= out_cap)
        {
          dropped++;
          continue;
        }

      if (!cloud_parse_entry(entry, &out[written]))
        {
          malformed++;
          continue;
        }

      written++;
    }

  if (dropped > 0 || malformed > 0)
    {
      printf("%s: poll kept %zu entries, dropped %zu over capacity, "
             "%zu malformed\n", CLOUD_TAG, written, dropped, malformed);
    }

  *out_count = written;
  ret = 0;

out:
  cJSON_Delete(root);
  cloud_free(resp, from_psram);
  return ret;
}

int vs_cloud_social_finalize(const char *session_id, char *msg_id_out,
                             size_t msg_id_cap)
{
  char body[192];
  char *resp = NULL;
  bool from_psram = false;
  cJSON *root = NULL;
  cJSON *value = NULL;
  int status = 0;
  int http;
  int ret;

  if (session_id == NULL || msg_id_out == NULL || msg_id_cap == 0)
    {
      return -EINVAL;
    }

  msg_id_out[0] = '\0';

  if (!g_cloud.configured)
    {
      return -ENODATA;
    }

  if (!cloud_id_valid(session_id, VS_CLOUD_SESSION_ID_MAX))
    {
      return -EINVAL;
    }

  snprintf(body, sizeof(body),
           "{\"deviceId\":\"%s\",\"sessionId\":\"%s\",\"timestamp\":\"%llu\"}",
           vs_cloud_device_id(), session_id,
           (unsigned long long)cloud_now_ms());

  resp = cloud_alloc(CONFIG_VS_SOCIAL_REG_RESP_BYTES, &from_psram);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  http = cloud_api_call("DELETE", CLOUD_PATH_SESSION, body, resp,
                        CONFIG_VS_SOCIAL_REG_RESP_BYTES);
  if (http < 0)
    {
      ret = http;
      goto out;
    }

  if (http < 200 || http >= 300)
    {
      printf("%s: finalize returned HTTP %d\n", CLOUD_TAG, http);
      ret = -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  ret = cloud_check_envelope(root, session_id, &status, &value);
  if (ret < 0)
    {
      goto out;
    }

  /* 40 means the close was accepted and is in progress, which is the
   * documented success.  0 is accepted too for a cloud that answers with the
   * generic success code.
   */

  if (status != 40 && status != 0)
    {
      char log[VS_TEXT_SHORT];

      cloud_value_log(value, root, log, sizeof(log));
      printf("%s: finalize refused, status %d%s%s\n", CLOUD_TAG, status,
             log[0] != '\0' ? ": " : "", log);
      ret = -EIO;
      goto out;
    }

  if (!cloud_json_exact(value, "msgId", msg_id_out, msg_id_cap) ||
      !cloud_id_valid(msg_id_out, msg_id_cap))
    {
      printf("%s: finalize response has no usable msgId\n", CLOUD_TAG);
      ret = -EPROTO;
      goto out;
    }

  printf("%s: session %s closing, result under msg %s\n", CLOUD_TAG,
         session_id, msg_id_out);
  ret = 0;

out:
  cJSON_Delete(root);
  cloud_free(resp, from_psram);
  return ret;
}

/****************************************************************************
 * Name: cloud_summarize_timeline
 *
 * Description:
 *   Reduce emotionTimeline to the three percentages vs_history_index_s wants.
 *
 *   The three buckets follow the cloud's three colours: green 愉悦 is happy,
 *   green 中立 is calm, and red and blue both land in tense.  Folding blue in
 *   with red is a judgement: blue covers 害怕, 伤心, 疑惑 and 惊讶, and in a
 *   three-way split none of those reads as calm or happy.
 *
 *   tense takes the rounding remainder so the three always total 100.
 *
 ****************************************************************************/

static void cloud_summarize_timeline(cJSON *timeline,
                                     struct vs_cloud_minutes_s *minutes)
{
  unsigned int counts[3] = { 0, 0, 0 }; /* calm, happy, tense */
  unsigned int total = 0;
  cJSON *item;

  minutes->calm = 0;
  minutes->happy = 0;
  minutes->tense = 0;
  minutes->emotion_samples = 0;

  if (timeline == NULL || !cJSON_IsArray(timeline))
    {
      return;
    }

  cJSON_ArrayForEach(item, timeline)
    {
      enum vs_emotion_e emotion;
      uint32_t rgb;
      bool extreme;
      char color[16];
      char detail[VS_TEXT_LONG];

      if (!cJSON_IsObject(item))
        {
          continue;
        }

      (void)cloud_json_exact(item, "emotionColor", color, sizeof(color));
      (void)cloud_json_str(item, "emotionDetail", detail,
                           sizeof(detail));
      cloud_classify_emotion(color, detail, &emotion, &rgb, &extreme);

      switch (emotion)
        {
          case VS_EMOTION_HAPPY:
            counts[1]++;
            break;

          case VS_EMOTION_CALM:
            counts[0]++;
            break;

          case VS_EMOTION_TENSE:
          case VS_EMOTION_CONFUSED:
            counts[2]++;
            break;

          default:

            /* An unrecognised colour is not evidence of anything, so it is
             * left out of the denominator rather than being guessed at.
             */

            continue;
        }

      total++;
    }

  minutes->emotion_samples = (uint16_t)(total > UINT16_MAX ? UINT16_MAX
                                                           : total);

  if (total == 0)
    {
      return;
    }

  minutes->calm = (uint8_t)(counts[0] * 100u / total);
  minutes->happy = (uint8_t)(counts[1] * 100u / total);
  minutes->tense = (uint8_t)(counts[2] * 100u / total);

  /* Give the rounding remainder to whichever bucket has the most samples,
   * rather than always to tense.
   *
   * Always crediting tense meant a session of one calm and two happy frames
   * reported tense = 1, and tense is the bucket that drives the alert
   * vocabulary -- "1% tense" for a conversation with no tense frames at all
   * is a claim the data does not support.  The largest bucket is where a
   * single stray point is least misleading.
   */

  {
    unsigned int sum = (unsigned int)minutes->calm +
                       (unsigned int)minutes->happy +
                       (unsigned int)minutes->tense;
    unsigned int remainder = sum < 100u ? 100u - sum : 0u;

    if (remainder > 0)
      {
        if (counts[0] >= counts[1] && counts[0] >= counts[2])
          {
            minutes->calm = (uint8_t)(minutes->calm + remainder);
          }
        else if (counts[1] >= counts[2])
          {
            minutes->happy = (uint8_t)(minutes->happy + remainder);
          }
        else
          {
            minutes->tense = (uint8_t)(minutes->tense + remainder);
          }
      }
  }
}

int vs_cloud_social_get_result(const char *session_id, const char *msg_id,
                               struct vs_cloud_minutes_s *minutes,
                               char *full_json, size_t full_cap)
{
  const char *ids[1];
  char *resp = NULL;
  bool from_psram = false;
  cJSON *root = NULL;
  cJSON *value = NULL;
  cJSON *entry;
  cJSON *chosen = NULL;
  cJSON *response;
  char *printed = NULL;
  int ret;

  if (session_id == NULL || msg_id == NULL)
    {
      return -EINVAL;
    }

  if (minutes != NULL)
    {
      memset(minutes, 0, sizeof(*minutes));
    }

  if (full_json != NULL && full_cap > 0)
    {
      full_json[0] = '\0';
    }

  if (!g_cloud.configured)
    {
      return -ENODATA;
    }

  ids[0] = msg_id;

  resp = cloud_alloc(CONFIG_VS_SOCIAL_RESP_MAX_BYTES, &from_psram);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  ret = cloud_get_result_call(session_id, ids, 1, resp,
                              CONFIG_VS_SOCIAL_RESP_MAX_BYTES, &root, &value);
  if (ret < 0)
    {
      goto out;
    }

  if (value == NULL || !cJSON_IsArray(value))
    {
      ret = -EPROTO;
      goto out;
    }

  /* Take only an entry that says it is a close result.
   *
   * Accepting any entry under this msgId, as an earlier version did, is worse
   * than waiting: the cloud reuses a msgId across events, so an image entry
   * can appear here, and an image entry at status 20 satisfied every check
   * below.  The call then returned success with an empty tts_url, an empty
   * summary and three zeroed percentages -- indistinguishable to the caller
   * from a session that genuinely produced nothing, and about to be persisted
   * as history and queued for playback.  Since every other unexpected shape
   * already returns -EAGAIN, insisting on the right entry costs one poll.
   */

  cJSON_ArrayForEach(entry, value)
    {
      char id[VS_CLOUD_MSG_ID_MAX];
      int msg_event = -1;

      if (!cJSON_IsObject(entry))
        {
          continue;
        }

      if (!cloud_json_exact(entry, "msgId", id, sizeof(id)) ||
          strcmp(id, msg_id) != 0)
        {
          continue;
        }

      if (!cloud_json_int(entry, "msgEvent", &msg_event) ||
          msg_event != VS_CLOUD_MSG_EVENT_CLOSE)
        {
          continue;
        }

      chosen = entry;
      break;
    }

  if (chosen == NULL)
    {
      ret = -EAGAIN;
      goto out;
    }

  {
    int status = 0;

    if (!cloud_json_int(chosen, "status", &status))
      {
        ret = -EPROTO;
        goto out;
      }

    if (status == VS_CLOUD_PEER_CLOSING ||
        status == VS_CLOUD_PEER_EMOTION_ANALYZING ||
        status == VS_CLOUD_PEER_ADVICE_PENDING)
      {
        ret = -EAGAIN;
        goto out;
      }

    if (status == VS_CLOUD_PEER_FAILED)
      {
        if (minutes != NULL)
          {
            cloud_value_log(NULL, chosen, minutes->summary,
                            sizeof(minutes->summary));
          }

        printf("%s: close of %s failed\n", CLOUD_TAG, session_id);
        ret = -EIO;
        goto out;
      }

    /* 21 and only 21.  The cloud document states it outright for this event
     * ("约定会话关闭完毕用 21 表示").  20 is not a synonym -- on the peer
     * side 20 means an image result is attached and 21 means advice is -- so
     * accepting it here would accept the wrong payload shape.
     */

    if (status != VS_CLOUD_PEER_ADVICE_DONE)
      {
        printf("%s: close of %s in unexpected state %d\n", CLOUD_TAG,
               session_id, status);
        ret = -EAGAIN;
        goto out;
      }
  }

  response = cJSON_GetObjectItem(chosen, "response");
  if (response == NULL || !cJSON_IsObject(response))
    {
      /* Completed without a payload.  Nothing to persist and nothing to
       * play, so this is a failure of the result rather than of the call.
       */

      printf("%s: close of %s completed with no response object\n",
             CLOUD_TAG, session_id);
      ret = -EPROTO;
      goto out;
    }

  /* Serialize first, so a full_json buffer too small for the record fails
   * before minutes is touched.  Filling minutes and only then returning
   * -E2BIG contradicted the header's "filled only on success" and left the
   * caller holding a half-built result it had been told it did not have.
   */

  if (full_json != NULL)
    {
      size_t len;

      printed = cJSON_PrintUnformatted(response);
      if (printed == NULL)
        {
          ret = -ENOMEM;
          goto out;
        }

      len = strlen(printed);
      if (len + 1 > full_cap)
        {
          /* Reported rather than truncated.  A clipped minutes record would
           * be persisted and then fail to parse on the next boot, which is
           * worse than not having it.
           */

          printf("%s: minutes need %zu bytes, buffer holds %zu\n", CLOUD_TAG,
                 len + 1, full_cap);
          ret = -E2BIG;
          goto out;
        }

      memcpy(full_json, printed, len + 1);
    }

  if (minutes != NULL)
    {
      (void)cloud_json_exact(response, "ttsMinutes", minutes->tts_url,
                             sizeof(minutes->tts_url));
      (void)cloud_json_str(response, "txtMinutes", minutes->summary,
                           sizeof(minutes->summary));

      cloud_summarize_timeline(cJSON_GetObjectItem(response,
                                                  "emotionTimeline"),
                               minutes);

      {
        cJSON *audio = cJSON_GetObjectItem(response, "audioTimeline");
        int n = cJSON_IsArray(audio) ? cJSON_GetArraySize(audio) : 0;

        minutes->audio_samples = (uint16_t)(n < 0 ? 0 : n);
      }
    }

  ret = 0;

out:
  if (printed != NULL)
    {
      cJSON_free(printed);
    }

  /* Anything but -EAGAIN is the close having reached a terminal answer, so the
   * endpoint is free to move again.  -EAGAIN keeps it pinned: the caller is
   * going to poll this same msgId, which only exists on the current host.
   *
   * A session abandoned without ever polling to a terminal answer leaves this
   * set.  That is survivable rather than a leak: vs_cloud_reload_endpoint()
   * then defers to the next vs_cloud_social_open(), which clears it and
   * applies the pending endpoint before sending anything.
   */

  if (ret != -EAGAIN)
    {
      g_cloud.session_live = false;
    }

  cJSON_Delete(root);
  cloud_free(resp, from_psram);
  return ret;
}

int vs_cloud_social_ack(const char *session_id)
{
  /* No cloud endpoint exists for this.  See the header: the call is kept so
   * the orchestration layer has one place to put the acknowledgement if the
   * interface ever grows one, and so that reviewing this file makes it
   * obvious the omission is known rather than forgotten.
   */

  printf("%s: session %s complete (no cloud ack endpoint; local no-op)\n",
         CLOUD_TAG, session_id != NULL ? session_id : "?");
  return 0;
}

int vs_cloud_download(const char *url, unsigned char **data, size_t *len,
                      bool *from_psram)
{
  struct cloud_url_s *parsed = NULL;
  unsigned char *buf = NULL;
  bool psram = false;
  size_t got = 0;
  int http;
  int ret;

  if (url == NULL || data == NULL || len == NULL || from_psram == NULL)
    {
      return -EINVAL;
    }

  *data = NULL;
  *len = 0;
  *from_psram = false;

  parsed = malloc(sizeof(*parsed));
  if (parsed == NULL)
    {
      return -ENOMEM;
    }

  ret = cloud_url_parse(url, parsed);
  if (ret < 0)
    {
      if (ret == -EPROTONOSUPPORT)
        {
          printf("%s: cannot download %s, unsupported scheme\n", CLOUD_TAG,
                 url);
        }

      goto out;
    }

  /* Two bytes of slack.  One is for the transport's NUL, which it writes past
   * the payload.  The other is what makes a body of exactly the budget
   * distinguishable from one that overran it: the transport can then read one
   * byte more than the budget allows, and the check below sees it.
   */

  buf = cloud_alloc(CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES + 2, &psram);
  if (buf == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  http = cloud_http(parsed, "GET", NULL, NULL, 0, (char *)buf,
                    CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES + 2, &got);
  if (http < 0)
    {
      ret = http;
      goto out;
    }

  if (http < 200 || http >= 300)
    {
      printf("%s: download returned HTTP %d\n", CLOUD_TAG, http);
      ret = -EIO;
      goto out;
    }

  if (got == 0)
    {
      ret = -ENODATA;
      goto out;
    }

  if (got > CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES)
    {
      printf("%s: download of %zu bytes exceeds the %d byte budget\n",
             CLOUD_TAG, got, CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES);
      ret = -EFBIG;
      goto out;
    }

  *data = buf;
  *len = got;
  *from_psram = psram;
  buf = NULL;
  ret = 0;

out:
  cloud_free(buf, psram);
  free(parsed);
  return ret;
}

/****************************************************************************
 * Name: cloud_probe_upload
 *
 * Description:
 *   Register and transfer one synthetic object, then report.  Helper for
 *   vs_cloud_probe() so the image and audio legs do not duplicate the log
 *   formatting.
 *
 ****************************************************************************/

static int cloud_probe_upload(const char *session_id,
                              enum vs_cloud_media_e type,
                              const unsigned char *data, size_t len,
                              char *msg_id_out, size_t msg_id_cap,
                              bool *payload_moved)
{
  struct vs_cloud_media_packet_s packet;
  struct vs_cloud_upload_s result;
  const char *what = type == VS_CLOUD_MEDIA_AUDIO ? "audio" : "image";
  int ret;

  memset(&packet, 0, sizeof(packet));
  packet.type = type;
  packet.data = data;
  packet.len = len;
  packet.sequence = 0;

  ret = vs_cloud_social_upload(session_id, &packet, &result);
  if (ret < 0)
    {
      printf("%s: probe %s upload failed: %d\n", CLOUD_TAG, what, ret);
      return ret;
    }

  printf("%s: probe %s registered as msg %s, payload %s\n", CLOUD_TAG, what,
         result.msg_id, result.payload_sent ? "transferred" : "NOT sent");

  /* Only ever cleared, so one leg that did not transfer makes the whole probe
   * report that no payload moved.
   */

  if (!result.payload_sent)
    {
      *payload_moved = false;
    }

  snprintf(msg_id_out, msg_id_cap, "%s", result.msg_id);
  return 0;
}

int vs_cloud_probe(void)
{
  struct vs_cloud_session_s session;
  struct vs_social_event_s events[4];
  struct vs_cloud_minutes_s minutes;
  unsigned char synthetic[256];
  char image_msg[VS_CLOUD_MSG_ID_MAX];
  char audio_msg[VS_CLOUD_MSG_ID_MAX];
  char close_msg[VS_CLOUD_MSG_ID_MAX];
  const char *ids[2];
  bool payload_moved = true;
  size_t count = 0;
  size_t i;
  int attempts;
  int ret;

  if (vs_cloud_init() < 0)
    {
      /* Reaching here takes an empty host in the provisioning record, an
       * empty CONFIG_VS_SOCIAL_CLOUD_HOST and an empty
       * CONFIG_VELASIGHT_PROVISION_CLOUD_HOST, so naming only the middle one
       * would send someone to the wrong file two times out of three.
       */

      printf("%s: probe needs an endpoint -- set one on the setup page, or "
             "CONFIG_VS_SOCIAL_CLOUD_HOST, or "
             "CONFIG_VELASIGHT_PROVISION_CLOUD_HOST\n", CLOUD_TAG);
      return -ENODATA;
    }

  /* Synthetic payload.  This is a transport and protocol probe, so the bytes
   * are a recognisable pattern rather than a real frame -- it must never be
   * possible for this command to put camera or microphone data on the wire.
   * A cloud that validates content will answer "no valid face" or similar,
   * and that still counts: the round trip is what is being measured.
   */

  for (i = 0; i < sizeof(synthetic); i++)
    {
      synthetic[i] = (unsigned char)i;
    }

  memset(&session, 0, sizeof(session));

  printf("%s: probe starting against %s://%s:%s%s/contest/v1 (%s)\n",
         CLOUD_TAG, g_cloud.tls ? "https" : "http", g_cloud.host,
         g_cloud.port, g_cloud.base_path, cloud_origin_text());

  ret = vs_cloud_social_open(&session);
  if (ret < 0)
    {
      printf("%s: probe open failed: %d%s\n", CLOUD_TAG, ret,
             ret == -EBUSY ? " (a session is still open for this device; it "
                             "may be an earlier probe that did not close)"
                           : "");
      return ret;
    }

  /* From here on every exit goes through abandon, which closes the session.
   *
   * One deviceId may hold one live session and the cloud refuses to open
   * another while results are owed, so a probe that returned early would
   * poison every later probe with -EBUSY on open -- a second, different-
   * looking failure for a command whose entire job is to be the connectivity
   * gate.
   */

  ret = cloud_probe_upload(session.session_id, VS_CLOUD_MEDIA_IMAGE,
                           synthetic, sizeof(synthetic), image_msg,
                           sizeof(image_msg), &payload_moved);
  if (ret < 0)
    {
      goto abandon;
    }

  ret = cloud_probe_upload(session.session_id, VS_CLOUD_MEDIA_AUDIO,
                           synthetic, sizeof(synthetic), audio_msg,
                           sizeof(audio_msg), &payload_moved);
  if (ret < 0)
    {
      goto abandon;
    }

  ids[0] = image_msg;
  ids[1] = audio_msg;

  ret = vs_cloud_social_poll_event(session.session_id, ids, 2, events,
                                   sizeof(events) / sizeof(events[0]),
                                   &count);
  if (ret < 0)
    {
      printf("%s: probe poll failed: %d\n", CLOUD_TAG, ret);
      goto abandon;
    }

  printf("%s: probe poll returned %zu entries\n", CLOUD_TAG, count);
  for (i = 0; i < count; i++)
    {
      printf("%s:   msg %s event %d status %d%s%s conf %u extreme %d\n",
             CLOUD_TAG, events[i].msg_id, (int)events[i].msg_event,
             events[i].raw_status,
             events[i].display_text[0] != '\0' ? " text " : "",
             events[i].display_text,
             (unsigned)events[i].confidence, events[i].extreme ? 1 : 0);
    }

  ret = vs_cloud_social_finalize(session.session_id, close_msg,
                                 sizeof(close_msg));
  if (ret < 0)
    {
      printf("%s: probe finalize failed: %d\n", CLOUD_TAG, ret);
      return ret;
    }

  /* Bounded polling with the same interval the session orchestration will
   * use.  Ten attempts is enough to show the asynchronous close working
   * without turning a probe into a long wait when the cloud is not going to
   * answer.
   */

  for (attempts = 0; attempts < 10; attempts++)
    {
      ret = vs_cloud_social_get_result(session.session_id, close_msg,
                                       &minutes, NULL, 0);
      if (ret != -EAGAIN)
        {
          break;
        }

      usleep(1500 * 1000);
    }

  if (ret < 0)
    {
      printf("%s: probe result failed: %d%s\n", CLOUD_TAG, ret,
             ret == -EAGAIN ? " (still closing after 10 attempts)" : "");
      return ret;
    }

  printf("%s: probe minutes: calm %u happy %u tense %u, %u emotion / %u "
         "audio samples\n", CLOUD_TAG, minutes.calm, minutes.happy,
         minutes.tense, minutes.emotion_samples, minutes.audio_samples);
  printf("%s: probe tts url: %s\n", CLOUD_TAG,
         minutes.tts_url[0] != '\0' ? minutes.tts_url : "(none)");
  printf("%s: probe summary: %s\n", CLOUD_TAG,
         minutes.summary[0] != '\0' ? minutes.summary : "(none)");

  (void)vs_cloud_social_ack(session.session_id);

  /* A round trip that registered but moved no bytes is a pass against the
   * cloud's mock mode and a failure against a real one, and the probe cannot
   * tell which it is talking to.  Reporting it as its own outcome rather than
   * as success keeps the gate honest either way.
   */

  if (!payload_moved)
    {
      printf("%s: probe completed WITHOUT transferring any payload "
             "(mock presigned URLs)\n", CLOUD_TAG);
      return -ENOTSUP;
    }

  printf("%s: probe completed\n", CLOUD_TAG);
  return 0;

abandon:
  {
    char abandon_msg[VS_CLOUD_MSG_ID_MAX];
    int close_ret = vs_cloud_social_finalize(session.session_id, abandon_msg,
                                             sizeof(abandon_msg));

    printf("%s: probe closing session %s after failure %d (close %s)\n",
           CLOUD_TAG, session.session_id, ret,
           close_ret == 0 ? "accepted" : "also failed");
  }

  return ret;
}

void vs_cloud_deinit(void)
{
  /* This is process-wide, not scoped to this module: vela_tls.c keeps one
   * pool for every caller in the image, so any idle keep-alive connection
   * belonging to the idle assistant's model client is dropped along with
   * ours.  In-use slots are skipped with a warning rather than torn down.
   * Only safe at shutdown, once nothing else is going to make a request.
   */

  vela_tls_pool_cleanup();
}
