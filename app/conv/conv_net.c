/****************************************************************************
 * app/conv/conv_net.c
 *
 * Time from the console, and the model credentials on the card.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "conv_net.h"
#include "conv_store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Below this, CLOCK_REALTIME is counting from boot rather than from 1970.
 *
 * 1500000000 is mid-2017: comfortably past any uptime this board will reach
 * (that many seconds is 47 years) and comfortably before any date it will be
 * told, so the two ranges cannot be confused.
 */

#define CONV_EPOCH_FLOOR  1500000000u

/* A console that has gone away must not hold a command. */

#define CONV_NET_MS       3000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: conv_connect
 ****************************************************************************/

static int conv_connect(const char *host, int port)
{
  struct sockaddr_in addr;
  struct timeval tv;
  int sock;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      printf("conv: %s is not an IPv4 address\n", host);
      return -EINVAL;
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      return -errno;
    }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      int err = errno;

      close(sock);
      return -err;
    }

  tv.tv_sec = CONV_NET_MS / 1000;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  return sock;
}

/****************************************************************************
 * Name: conv_send_all
 ****************************************************************************/

static int conv_send_all(int sock, const void *data, size_t len)
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
 * Name: conv_json_field
 *
 * Description:
 *   Copy a string field out of a small JSON file into out.
 *
 *   Substring matching rather than parsing, for the same reason as elsewhere
 *   in this application: three fields do not justify a parser and its working
 *   memory, and this file is one the board wrote itself.
 *
 ****************************************************************************/

static bool conv_json_field(const char *json, const char *key, char *out,
                           size_t len)
{
  char pattern[32];
  const char *p;
  size_t o = 0;

  snprintf(pattern, sizeof(pattern), "\"%s\"", key);

  p = strstr(json, pattern);
  if (p == NULL)
    {
      return false;
    }

  p = strchr(p, ':');
  if (p == NULL)
    {
      return false;
    }

  p++;
  while (*p == ' ')
    {
      p++;
    }

  if (*p != '"')
    {
      return false;
    }

  p++;
  while (*p != '\0' && *p != '"' && o + 1 < len)
    {
      out[o++] = *p++;
    }

  out[o] = '\0';
  return o > 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool conv_clock_synced(void)
{
  return (uint32_t)time(NULL) >= CONV_EPOCH_FLOOR;
}

void conv_clock_report(void)
{
  uint32_t now = (uint32_t)time(NULL);

  if (conv_clock_synced())
    {
      printf("conv: clock reads %u (epoch %lu) -- synced\n",
             conv_epoch_to_date(now), (unsigned long)now);
    }
  else
    {
      printf("conv: clock reads %lu, which is seconds since boot, not a "
             "date -- this board has no RTC, so set it with 'conv time -s "
             "<ip> <port>' before recording anything\n",
             (unsigned long)now);
    }
}

int conv_clock_set(uint32_t epoch)
{
  struct timespec ts;

  if (epoch < CONV_EPOCH_FLOOR)
    {
      printf("conv: %lu is not a plausible epoch (before 2017)\n",
             (unsigned long)epoch);
      return -EINVAL;
    }

  ts.tv_sec = (time_t)epoch;
  ts.tv_nsec = 0;

  if (clock_settime(CLOCK_REALTIME, &ts) < 0)
    {
      printf("conv: clock_settime failed: %d\n", errno);
      return -errno;
    }

  printf("conv: clock set to %u (epoch %lu)\n", conv_epoch_to_date(epoch),
         (unsigned long)epoch);
  return OK;
}

int conv_clock_fetch(const char *host, int port)
{
  char request[256];
  char reply[512];
  const char *field;
  uint32_t epoch;
  size_t total = 0;
  int sock;
  int len;
  int ret;

  sock = conv_connect(host, port);
  if (sock < 0)
    {
      printf("conv: cannot reach %s:%d: %d\n", host, port, sock);
      return sock;
    }

  len = snprintf(request, sizeof(request),
                 "GET /api/time HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "User-Agent: bk7258-conv\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 host, port);

  ret = conv_send_all(sock, request, (size_t)len);
  if (ret < 0)
    {
      close(sock);
      return ret;
    }

  /* Read until the peer closes: the body is small and Connection: close
   * makes end-of-stream the end of the message, so no chunked or
   * content-length handling is needed for this one request.
   */

  while (total < sizeof(reply) - 1)
    {
      ssize_t got = recv(sock, reply + total, sizeof(reply) - 1 - total, 0);

      if (got <= 0)
        {
          break;
        }

      total += (size_t)got;
    }

  close(sock);
  reply[total] = '\0';

  field = strstr(reply, "\"epoch\"");
  if (field == NULL)
    {
      printf("conv: %s:%d did not answer with an epoch; is this the web "
             "console?\n", host, port);
      return -EPROTO;
    }

  field = strchr(field, ':');
  if (field == NULL)
    {
      return -EPROTO;
    }

  epoch = (uint32_t)strtoul(field + 1, NULL, 10);
  return conv_clock_set(epoch);
}

int conv_llm_set(const char *host, const char *model, const char *key)
{
  FILE *f;

  if (host == NULL || model == NULL || key == NULL ||
      host[0] == '\0' || model[0] == '\0' || key[0] == '\0')
    {
      return -EINVAL;
    }

  if (strlen(key) >= CONV_KEY_MAX || strlen(host) >= CONV_HOST_MAX ||
      strlen(model) >= CONV_MODEL_MAX)
    {
      return -E2BIG;
    }

  f = fopen(CONV_LLM_FILE, "w");
  if (f == NULL)
    {
      printf("conv: cannot write %s: %d\n", CONV_LLM_FILE, errno);
      return -errno;
    }

  fprintf(f, "{\n  \"host\": \"%s\",\n  \"model\": \"%s\",\n"
             "  \"key\": \"%s\"\n}\n", host, model, key);
  fclose(f);

  /* Deliberately not printing the key.  This console output is routinely
   * pasted into chat while debugging, and the project's own config_show
   * shows four characters for the same reason.
   */

  printf("conv: model set to %s / %s, key stored (%zu chars, starts %.4s)\n",
         host, model, strlen(key), key);
  return OK;
}

int conv_llm_report(char *out, size_t len)
{
  char json[CONV_KEY_MAX + CONV_HOST_MAX + CONV_MODEL_MAX + 64];
  char host[CONV_HOST_MAX];
  char model[CONV_MODEL_MAX];
  char key[CONV_KEY_MAX];
  int got;

  if (out == NULL || len == 0)
    {
      return -EINVAL;
    }

  got = conv_store_read_raw(CONV_LLM_FILE, json, sizeof(json));
  if (got < 0)
    {
      snprintf(out, len, "{\"configured\":false}");
      return OK;
    }

  if (!conv_json_field(json, "host", host, sizeof(host)))
    {
      host[0] = '\0';
    }

  if (!conv_json_field(json, "model", model, sizeof(model)))
    {
      model[0] = '\0';
    }

  if (!conv_json_field(json, "key", key, sizeof(key)))
    {
      key[0] = '\0';
    }

  /* Four characters and a length, never the key.  Enough to tell one key from
   * another and to confirm something was saved; not enough to use.
   */

  snprintf(out, len,
           "{\"configured\":%s,\"host\":\"%s\",\"model\":\"%s\","
           "\"keyPrefix\":\"%.4s\",\"keyLength\":%zu}",
           key[0] != '\0' ? "true" : "false", host, model, key, strlen(key));

  return OK;
}
