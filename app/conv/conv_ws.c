/****************************************************************************
 * app/conv/conv_ws.c
 *
 * A WebSocket client cut down to what this board needs, and the loop that
 * answers the console's history queries with it.  See conv_ws.h for what of
 * RFC 6455 is implemented and what is deliberately left out.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "conv_net.h"
#include "conv_store.h"
#include "conv_ws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WS_OP_TEXT        0x1
#define WS_OP_CLOSE       0x8
#define WS_OP_PING        0x9
#define WS_OP_PONG        0xa

/* Identifies this board to the console.  Short and within the deviceId
 * pattern the server enforces (letters, digits, dot, underscore, hyphen).
 */

#define WS_DEVICE_ID      "bk7258-001"
#define WS_DEVICE_NAME    "VelaSight BK7258"

/* How long a receive waits before returning so the loop can do other work.
 * Only relevant once there is other work; for now it bounds how quickly a
 * Ctrl-C is noticed.
 */

#define WS_POLL_MS        1000

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char g_ws_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws_b64_16
 *
 * Description:
 *   Base64-encode 16 bytes into the 24-character form Sec-WebSocket-Key
 *   requires.
 *
 ****************************************************************************/

static void ws_b64_16(const uint8_t *in, char *out)
{
  int i;
  int o = 0;

  for (i = 0; i < 15; i += 3)
    {
      uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                   in[i + 2];

      out[o++] = g_ws_b64[(v >> 18) & 0x3f];
      out[o++] = g_ws_b64[(v >> 12) & 0x3f];
      out[o++] = g_ws_b64[(v >> 6) & 0x3f];
      out[o++] = g_ws_b64[v & 0x3f];
    }

  /* The sixteenth byte on its own: two output characters and one pad. */

  out[o++] = g_ws_b64[(in[15] >> 2) & 0x3f];
  out[o++] = g_ws_b64[(in[15] << 4) & 0x3f];
  out[o++] = '=';
  out[o++] = '=';
  out[o] = '\0';
}

/****************************************************************************
 * Name: ws_random
 *
 * Description:
 *   Fill a buffer with random bytes for the handshake key and the frame
 *   masks.
 *
 *   The mask is not a security measure -- the payload travels in clear
 *   either way -- it exists so that a hostile page cannot make a client emit
 *   bytes of its choosing at a proxy.  arc4random is seeded from the kernel
 *   pool, which this board's TRNG feeds, so it is a better source than
 *   anything this file could build from the clock.
 *
 ****************************************************************************/

static void ws_random(uint8_t *buf, size_t len)
{
  arc4random_buf(buf, len);
}

/****************************************************************************
 * Name: ws_send_all
 ****************************************************************************/

static int ws_send_all(int sock, const void *data, size_t len)
{
  const uint8_t *p = data;
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

          return n < 0 ? -errno : -EIO;
        }

      sent += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Name: ws_recv_exact
 *
 * Description:
 *   Read exactly len bytes, or fail.
 *
 *   Framing needs this: a header read that comes up short would otherwise be
 *   interpreted as a frame, and the stream would never resynchronise.
 *
 ****************************************************************************/

static int ws_recv_exact(int sock, void *data, size_t len)
{
  uint8_t *p = data;
  size_t got = 0;

  while (got < len)
    {
      ssize_t n = recv(sock, p + got, len - got, 0);

      if (n == 0)
        {
          return -ENOTCONN;
        }

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      got += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Name: ws_send_frame
 *
 * Description:
 *   Send one masked frame with the given opcode.
 *
 ****************************************************************************/

static int ws_send_frame(struct conv_ws_s *ws, uint8_t opcode,
                         const void *payload, size_t len)
{
  uint8_t header[14];
  uint8_t mask[4];
  uint8_t chunk[256];
  const uint8_t *src = payload;
  size_t offset = 0;
  size_t hlen = 0;
  int ret;

  if (len > CONV_WS_MAX_MSG)
    {
      printf("conv: refusing to send a %zu byte frame; the console's limit "
             "is %d\n", len, CONV_WS_MAX_MSG);
      return -EMSGSIZE;
    }

  header[hlen++] = 0x80 | opcode;          /* FIN set, single frame */

  if (len < 126)
    {
      header[hlen++] = 0x80 | (uint8_t)len;
    }
  else
    {
      header[hlen++] = 0x80 | 126;
      header[hlen++] = (uint8_t)((len >> 8) & 0xff);
      header[hlen++] = (uint8_t)(len & 0xff);
    }

  ws_random(mask, sizeof(mask));
  memcpy(header + hlen, mask, sizeof(mask));
  hlen += sizeof(mask);

  ret = ws_send_all(ws->sock, header, hlen);
  if (ret < 0)
    {
      return ret;
    }

  /* Masked in blocks rather than in place: the caller's buffer is not ours
   * to modify, and copying the whole payload only to XOR it would double the
   * memory for a 16 KB message.
   */

  while (offset < len)
    {
      size_t n = len - offset;
      size_t i;

      if (n > sizeof(chunk))
        {
          n = sizeof(chunk);
        }

      for (i = 0; i < n; i++)
        {
          chunk[i] = src[offset + i] ^ mask[(offset + i) & 3];
        }

      ret = ws_send_all(ws->sock, chunk, n);
      if (ret < 0)
        {
          return ret;
        }

      offset += n;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int conv_ws_connect(struct conv_ws_s *ws, const char *host, int port)
{
  struct sockaddr_in addr;
  struct timeval tv;
  uint8_t keybytes[16];
  char key[32];
  char request[512];
  char response[512];
  size_t total = 0;
  int len;
  int ret;

  memset(ws, 0, sizeof(*ws));
  ws->sock = -1;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      printf("conv: %s is not an IPv4 address\n", host);
      return -EINVAL;
    }

  ws->sock = socket(AF_INET, SOCK_STREAM, 0);
  if (ws->sock < 0)
    {
      return -errno;
    }

  if (connect(ws->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      /* Silent: the caller retries and decides how often to say so.  Printing
       * here as well meant two lines per attempt, and a console that stayed
       * down filled the scrollback with them.
       */

      ret = -errno;
      close(ws->sock);
      ws->sock = -1;
      return ret;
    }

  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(ws->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(ws->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ws_random(keybytes, sizeof(keybytes));
  ws_b64_16(keybytes, key);

  len = snprintf(request, sizeof(request),
                 "GET /ws HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n",
                 host, port, key);

  ret = ws_send_all(ws->sock, request, (size_t)len);
  if (ret < 0)
    {
      close(ws->sock);
      ws->sock = -1;
      return ret;
    }

  /* Read until the end of the header block.  The server sends nothing else
   * until the client speaks, so this cannot swallow a frame.
   */

  while (total < sizeof(response) - 1)
    {
      ssize_t got = recv(ws->sock, response + total,
                         sizeof(response) - 1 - total, 0);

      if (got <= 0)
        {
          break;
        }

      total += (size_t)got;
      response[total] = '\0';

      if (strstr(response, "\r\n\r\n") != NULL)
        {
          break;
        }
    }

  response[total] = '\0';

  if (strstr(response, " 101") == NULL)
    {
      /* Print the status line: a 403 or a 404 here means the path or the
       * server is wrong, and that is worth seeing rather than "handshake
       * failed".
       */

      char *eol = strstr(response, "\r\n");

      if (eol != NULL)
        {
          *eol = '\0';
        }

      printf("conv: %s:%d did not upgrade: %.60s\n", host, port,
             total > 0 ? response : "(no reply)");
      close(ws->sock);
      ws->sock = -1;
      return -EPROTO;
    }

  ws->connected = true;
  return OK;
}

void conv_ws_close(struct conv_ws_s *ws)
{
  if (ws->sock >= 0)
    {
      if (ws->connected)
        {
          /* Best effort: a close frame is courtesy, and the socket is going
           * away regardless of whether it lands.
           */

          uint8_t reason[2];

          reason[0] = 0x03;
          reason[1] = 0xe8;                /* 1000, normal closure */
          ws_send_frame(ws, WS_OP_CLOSE, reason, sizeof(reason));
        }

      close(ws->sock);
      ws->sock = -1;
    }

  ws->connected = false;
}

int conv_ws_send_text(struct conv_ws_s *ws, const char *text)
{
  if (!ws->connected)
    {
      return -ENOTCONN;
    }

  return ws_send_frame(ws, WS_OP_TEXT, text, strlen(text));
}

int conv_ws_recv_text(struct conv_ws_s *ws, char *buf, size_t len,
                      int timeout_ms)
{
  for (; ; )
    {
      struct pollfd pfd;
      uint8_t header[2];
      size_t payload;
      uint8_t opcode;
      int ret;

      if (!ws->connected)
        {
          return -ENOTCONN;
        }

      pfd.fd = ws->sock;
      pfd.events = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : -1);
      if (ret == 0)
        {
          return 0;
        }

      if (ret < 0)
        {
          return errno == EINTR ? 0 : -errno;
        }

      ret = ws_recv_exact(ws->sock, header, sizeof(header));
      if (ret < 0)
        {
          ws->connected = false;
          return ret;
        }

      opcode = header[0] & 0x0f;

      if ((header[0] & 0x80) == 0)
        {
          printf("conv: the console sent a fragmented frame, which this "
                 "client does not reassemble\n");
          ws->connected = false;
          return -EPROTO;
        }

      payload = header[1] & 0x7f;

      if (payload == 126)
        {
          uint8_t ext[2];

          ret = ws_recv_exact(ws->sock, ext, sizeof(ext));
          if (ret < 0)
            {
              ws->connected = false;
              return ret;
            }

          payload = ((size_t)ext[0] << 8) | ext[1];
        }
      else if (payload == 127)
        {
          printf("conv: the console sent a 64-bit length, which cannot be "
                 "right against its own %d byte limit\n", CONV_WS_MAX_MSG);
          ws->connected = false;
          return -EMSGSIZE;
        }

      /* A server frame is never masked; if one were, the payload would be
       * garbage and the mask bytes would be read as data.
       */

      if ((header[1] & 0x80) != 0)
        {
          printf("conv: the console masked a frame, which servers must "
                 "not\n");
          ws->connected = false;
          return -EPROTO;
        }

      if (payload > len - 1)
        {
          printf("conv: a %zu byte frame does not fit the %zu byte "
                 "buffer\n", payload, len);
          ws->connected = false;
          return -EMSGSIZE;
        }

      if (payload > 0)
        {
          ret = ws_recv_exact(ws->sock, buf, payload);
          if (ret < 0)
            {
              ws->connected = false;
              return ret;
            }
        }

      buf[payload] = '\0';

      switch (opcode)
        {
          case WS_OP_TEXT:
            return (int)payload;

          case WS_OP_PING:

            /* Answered rather than ignored: the server is entitled to drop a
             * peer that stops responding, and a history query that fails
             * because a keepalive went unanswered would look like a bug in
             * the query.
             */

            ws_send_frame(ws, WS_OP_PONG, buf, payload);
            break;

          case WS_OP_PONG:
            break;

          case WS_OP_CLOSE:
            ws->connected = false;
            return -ENOTCONN;

          default:
            printf("conv: ignoring opcode 0x%x\n", opcode);
            break;
        }
    }
}
