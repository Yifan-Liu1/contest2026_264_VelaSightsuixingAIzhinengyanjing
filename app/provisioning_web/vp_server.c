/****************************************************************************
 * app/provisioning_web/vp_server.c
 *
 * The socket half of the provisioning entry.  Deliberately POSIX-only so the
 * accept loop, the ordering guarantee and one-shot mode can be exercised over
 * loopback on the host; the only thing left for the board is whether the
 * SoftAP and SD-NAND are actually there.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "velasight_provisioning.h"
#include "vp_form.h"
#include "vp_http.h"
#include "vp_store.h"

#define VP_STATE_IDLE     0
#define VP_STATE_RUNNING  1
#define VP_STATE_FINISHED 2  /* the thread returned, still needs a join */

#define VP_ACCEPT_POLL_MS  200
#define VP_REQUEST_TIMEOUT_MS 1500
#define VP_RESPONSE_RETRY_MS 25
#define VP_RESPONSE_TIMEOUT_MS 1000
#define VP_SOCKET_TIMEOUT_SEC 2
#define VP_SOCKET_LINGER_SEC 1
#define VP_STORE_PATH_MAX  192
#define VP_THREAD_STACK    8192

struct vp_server_s
{
  int       listenfd;
  int       clientfd;
  int       stoprd;
  int       stopwr;
  pthread_t thread;
  int       state;
  bool      stopping;
  bool      one_shot;
  char      store_path[VP_STORE_PATH_MAX];
  uint32_t  generation;
  velasight_prov_saved_cb_t on_saved;
  void     *cb_arg;
  struct velasight_prov_history_provider_s history;
};

static struct vp_server_s g_server =
{
  .listenfd = -1,
  .clientfd = -1,
  .stoprd   = -1,
  .stopwr   = -1,
  .state    = VP_STATE_IDLE,
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Request and response buffers are static because only one connection is
 * served at a time and 6KB of thread stack on this part is not free.
 */

static char g_request[VP_HTTP_MAX_HEADERS + VP_HTTP_MAX_BODY + 1];
static char g_response[VP_HTTP_RESPONSE_MAX];

static bool vp_stopping(void)
{
  bool stopping;

  pthread_mutex_lock(&g_lock);
  stopping = g_server.stopping;
  pthread_mutex_unlock(&g_lock);
  return stopping;
}

static int vp_wait_stop(int timeout_ms)
{
  struct pollfd pfd;
  int ret;

  pfd.fd = g_server.stoprd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  ret = poll(&pfd, 1, timeout_ms);
  if (ret < 0 && errno == EINTR)
    {
      return 0;
    }

  if (ret > 0 || vp_stopping())
    {
      return -ECANCELED;
    }

  return ret < 0 ? -errno : 0;
}

static int vp_monotonic_ms(uint64_t *now_ms)
{
  struct timespec now;

  if (now_ms == NULL)
    {
      return -EINVAL;
    }

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return -errno;
    }

  *now_ms = (uint64_t)now.tv_sec * 1000u +
            (uint64_t)now.tv_nsec / 1000000u;
  return 0;
}

static int vp_response_deadline(uint64_t *deadline_ms)
{
  uint64_t now;
  int ret = vp_monotonic_ms(&now);

  if (ret < 0)
    {
      return ret;
    }

  *deadline_ms = now + VP_RESPONSE_TIMEOUT_MS;
  return 0;
}

static int vp_deadline_delay(uint64_t deadline_ms, int *delay_ms)
{
  uint64_t now;
  uint64_t remaining;
  int ret = vp_monotonic_ms(&now);

  if (ret < 0)
    {
      return ret;
    }

  if (now >= deadline_ms)
    {
      return -ETIMEDOUT;
    }

  remaining = deadline_ms - now;
  *delay_ms = remaining < VP_RESPONSE_RETRY_MS ? (int)remaining :
                                                    VP_RESPONSE_RETRY_MS;
  if (*delay_ms <= 0)
    {
      *delay_ms = 1;
    }

  return 0;
}

static int vp_write_all(int fd, const char *data, size_t len,
                        uint64_t deadline_ms)
{
  size_t sent = 0;

  while (sent < len)
    {
      int flags = MSG_DONTWAIT;
      int delay_ms;
      ssize_t n;
      int ret;

      ret = vp_deadline_delay(deadline_ms, &delay_ms);
      if (ret < 0)
        {
          return ret;
        }

#ifdef MSG_NOSIGNAL
      flags |= MSG_NOSIGNAL;
#endif
      n = send(fd, data + sent, len - sent, flags);

      if (n < 0)
        {
          bool retryable;

          if (errno == EINTR)
            {
              continue;
            }

          /* NuttX's buffered TCP send returns ENOMEM when the global socket
           * callback pool is momentarily busy, and EAGAIN/ENOBUFS when the
           * WRB or IOB pool is busy.  Every fragment of one response shares
           * the same absolute deadline, so a paced slow client cannot occupy
           * the only listener forever by periodically freeing one buffer. */

          retryable = errno == ENOMEM || errno == EAGAIN || errno == ENOBUFS;
#ifdef EWOULDBLOCK
          retryable = retryable || errno == EWOULDBLOCK;
#endif
          if (retryable)
            {
              ret = vp_deadline_delay(deadline_ms, &delay_ms);
              if (ret < 0)
                {
                  return ret;
                }

              ret = vp_wait_stop(delay_ms);
              if (ret < 0)
                {
                  return ret;
                }

              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          return -EPIPE;
        }

      sent += (size_t)n;
    }

  return 0;
}

static int vp_write_response(int fd, const char *data, size_t len)
{
  uint64_t deadline_ms;
  int ret = vp_response_deadline(&deadline_ms);

  return ret < 0 ? ret : vp_write_all(fd, data, len, deadline_ms);
}

/****************************************************************************
 * Name: vp_read_request
 *
 * Description:
 *   Read until the headers plus the declared body have arrived, the cap is
 *   reached or the deadline passes.  Returns the total length, or negative on
 *   error.  A client that opens a connection and says nothing costs one
 *   timeout, not the service.
 *
 ****************************************************************************/

static int vp_read_request(int fd, struct vp_http_request_s *req)
{
  size_t total = 0;
  int waited = 0;

  for (; ; )
    {
      struct pollfd pfd[2];
      ssize_t n;
      int ret;

      ret = vp_http_parse(g_request, total, req);
      if (ret == 0)
        {
          if (req->action != VP_HTTP_ACTION_SAVE ||
              total >= req->header_len + req->content_length)
            {
              return (int)total;
            }
        }
      else if (ret != -EAGAIN)
        {
          return ret;
        }

      if (total >= sizeof(g_request) - 1)
        {
          return -E2BIG;
        }

      pfd[0].fd = fd;
      pfd[0].events = POLLIN;
      pfd[0].revents = 0;
      pfd[1].fd = g_server.stoprd;
      pfd[1].events = POLLIN;
      pfd[1].revents = 0;
      ret = poll(pfd, 2, VP_ACCEPT_POLL_MS);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (ret == 0)
        {
          waited += VP_ACCEPT_POLL_MS;
          if (waited >= VP_REQUEST_TIMEOUT_MS)
            {
              return -ETIMEDOUT;
            }

          continue;
        }

      if (pfd[1].revents != 0 || vp_stopping())
        {
          return -ECANCELED;
        }

      n = read(fd, g_request + total, sizeof(g_request) - 1 - total);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          /* Peer closed.  Whatever arrived is all there will be, so let the
           * parser decide between "incomplete" and "answerable".
           */

          if (vp_http_parse(g_request, total, req) == 0 &&
              (req->action != VP_HTTP_ACTION_SAVE ||
               total >= req->header_len + req->content_length))
            {
              return (int)total;
            }

          return -ECONNRESET;
        }

      total += (size_t)n;
    }
}

static bool vp_history_enabled(void)
{
  return g_server.history.snapshot != NULL && g_server.history.open != NULL;
}

static int vp_send_status(int fd, int status, const char *message)
{
  size_t len = vp_http_status_page(g_response, sizeof(g_response), status,
                                   message);

  return len == 0 ? -EOVERFLOW : vp_write_response(fd, g_response, len);
}

static int vp_history_error_status(int error)
{
  if (error == -ENOENT)
    {
      return 404;
    }

  if (error == -ENODEV || error == -EAGAIN)
    {
      return 503;
    }

  return 500;
}

static int vp_history_snapshot(
    struct velasight_prov_history_entry_s **entries_out,
    unsigned int *count_out)
{
  struct velasight_prov_history_entry_s *entries = NULL;
  unsigned int total = 0;
  unsigned int copied = 0;
  unsigned int attempt;
  int ret;

  if (entries_out == NULL || count_out == NULL || !vp_history_enabled())
    {
      return -ENODEV;
    }

  *entries_out = NULL;
  *count_out = 0;
  ret = g_server.history.snapshot(0, NULL, 0, &total, &copied,
                                  g_server.history.arg);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < 2; attempt++)
    {
      unsigned int current_total = 0;

      if (total > VELASIGHT_PROV_HISTORY_MAX_ENTRIES)
        {
          return -E2BIG;
        }

      if (total == 0)
        {
          return 0;
        }

      entries = calloc(total, sizeof(*entries));
      if (entries == NULL)
        {
          return -ENOMEM;
        }

      copied = 0;
      ret = g_server.history.snapshot(0, entries, total, &current_total,
                                      &copied, g_server.history.arg);
      if (ret < 0)
        {
          free(entries);
          return ret;
        }

      if (copied > total)
        {
          free(entries);
          return -EBADMSG;
        }

      if (current_total <= total)
        {
          *entries_out = entries;
          *count_out = copied;
          return 0;
        }

      free(entries);
      entries = NULL;
      total = current_total;
    }

  return -EAGAIN;
}

static int vp_send_history_list(int fd)
{
  struct velasight_prov_history_entry_s *entries = NULL;
  unsigned int count = 0;
  unsigned int i;
  size_t body_length = 0;
  size_t len;
  uint64_t deadline_ms;
  int status;
  int ret;

  if (!vp_history_enabled())
    {
      return vp_send_status(fd, 404, "历史记录功能未启用。");
    }

  ret = vp_history_snapshot(&entries, &count);
  if (ret < 0)
    {
      status = vp_history_error_status(ret);
      return vp_send_status(fd, status,
                            status == 503 ? "历史存储暂不可用。" :
                            status == 404 ? "历史记录不存在。" :
                                            "读取历史记录失败。");
    }

  len = vp_http_history_head_fragment(g_response, sizeof(g_response), count);
  if (len == 0)
    {
      free(entries);
      return vp_send_status(fd, 500, "生成历史页面失败。");
    }
  body_length = len;

  for (i = 0; i < count; i++)
    {
      len = vp_http_history_entry_fragment(g_response, sizeof(g_response),
                                           &entries[i]);
      if (len == 0 || body_length > SIZE_MAX - len)
        {
          free(entries);
          return vp_send_status(fd, 500, "历史索引格式无效。");
        }

      body_length += len;
    }

  len = vp_http_history_tail_fragment(g_response, sizeof(g_response));
  if (len == 0 || body_length > SIZE_MAX - len)
    {
      free(entries);
      return vp_send_status(fd, 500, "生成历史页面失败。");
    }
  body_length += len;

  len = vp_http_response_header(g_response, sizeof(g_response), 200,
                                "text/html; charset=utf-8", body_length,
                                NULL, NULL);
  if (len == 0)
    {
      free(entries);
      return -EOVERFLOW;
    }

  ret = vp_response_deadline(&deadline_ms);
  if (ret == 0)
    {
      ret = vp_write_all(fd, g_response, len, deadline_ms);
    }
  if (ret == 0)
    {
      len = vp_http_history_head_fragment(g_response, sizeof(g_response),
                                          count);
      ret = len == 0 ? -EOVERFLOW :
                       vp_write_all(fd, g_response, len, deadline_ms);
    }

  for (i = 0; ret == 0 && i < count; i++)
    {
      len = vp_http_history_entry_fragment(g_response, sizeof(g_response),
                                           &entries[i]);
      ret = len == 0 ? -EBADMSG :
                       vp_write_all(fd, g_response, len, deadline_ms);
    }

  if (ret == 0)
    {
      len = vp_http_history_tail_fragment(g_response, sizeof(g_response));
      ret = len == 0 ? -EOVERFLOW :
                       vp_write_all(fd, g_response, len, deadline_ms);
    }

  free(entries);
  return ret;
}

static int vp_send_history_file(int fd, const char *record_key,
                                bool download)
{
  size_t remaining;
  size_t size = 0;
  size_t len;
  uint64_t deadline_ms;
  int history_fd = -1;
  int status;
  int ret;

  if (!vp_history_enabled())
    {
      return vp_send_status(fd, 404, "历史记录功能未启用。");
    }

  ret = g_server.history.open(record_key, &history_fd, &size,
                              g_server.history.arg);
  if (ret < 0 || history_fd < 0)
    {
      if (history_fd >= 0)
        {
          close(history_fd);
        }

      if (ret >= 0)
        {
          ret = -EBADMSG;
        }
      status = vp_history_error_status(ret);
      return vp_send_status(fd, status,
                            status == 503 ? "历史存储暂不可用。" :
                            status == 404 ? "历史记录不存在。" :
                                            "打开历史记录失败。");
    }

  len = vp_http_response_header(
      g_response, sizeof(g_response), 200,
      download ? "application/json" : "application/json; charset=utf-8",
      size, download ? "attachment" : NULL,
      download ? record_key : NULL);
  if (len == 0)
    {
      close(history_fd);
      return -EOVERFLOW;
    }

  ret = vp_response_deadline(&deadline_ms);
  if (ret == 0)
    {
      ret = vp_write_all(fd, g_response, len, deadline_ms);
    }
  remaining = size;
  while (ret == 0 && remaining > 0)
    {
      size_t wanted = remaining < sizeof(g_response) ? remaining :
                                                       sizeof(g_response);
      ssize_t n = read(history_fd, g_response, wanted);

      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          ret = n < 0 ? -errno : -EIO;
          break;
        }

      ret = vp_write_all(fd, g_response, (size_t)n, deadline_ms);
      remaining -= (size_t)n;
    }

  close(history_fd);
  return ret;
}

/****************************************************************************
 * Name: vp_handle
 *
 * Description:
 *   Serve one connection.  Sets *saved when credentials were persisted and
 *   *save_status to 0 or the negative errno of a failed save, so the caller can
 *   notify the application only after the socket is gone.
 *
 ****************************************************************************/

static void vp_handle(int fd, bool *saved, int *save_status,
                      uint32_t *generation)
{
  struct velasight_prov_credentials_s cred;
  struct vp_http_request_s req;
  size_t len = 0;
  int ret;

  *saved = false;
  *save_status = 0;

  ret = vp_read_request(fd, &req);
  if (ret < 0)
    {
      if (ret == -E2BIG)
        {
          len = vp_http_status_page(g_response, sizeof(g_response), 431,
                                    "请求过大。");
        }
      else
        {
          /* Nothing usable arrived; answering a client that already gave up
           * is not worth a second syscall.
           */

          return;
        }
    }
  else if (req.action == VP_HTTP_ACTION_PAGE)
    {
       {
         struct velasight_prov_credentials_s current;
         const char *ssid = NULL;

         if (velasight_provisioning_load(&current) == 0)
           ssid = current.ssid;
         len = vp_http_form_page_with_ssid_history(
             g_response, sizeof(g_response), NULL, ssid,
             vp_history_enabled());
       }
    }
  else if (req.action == VP_HTTP_ACTION_HISTORY_LIST)
    {
      ret = vp_send_history_list(fd);
      if (ret < 0)
        {
          printf("provision_web: history list write failed: %d\n", ret);
        }
      return;
    }
  else if (req.action == VP_HTTP_ACTION_HISTORY_JSON ||
           req.action == VP_HTTP_ACTION_HISTORY_DOWNLOAD)
    {
      ret = vp_send_history_file(
          fd, req.record_key,
          req.action == VP_HTTP_ACTION_HISTORY_DOWNLOAD);
      if (ret < 0)
        {
          printf("provision_web: history file write failed: %d\n", ret);
        }
      return;
    }
  else if (req.action == VP_HTTP_ACTION_REJECT)
    {
      len = vp_http_status_page(g_response, sizeof(g_response), req.status,
                               req.status == 404 ? "页面不存在。" :
                               req.status == 403 ? "不允许跨站提交。" :
                               req.status == 405 ? "不支持该请求方法。" :
                               req.status == 413 ? "提交内容过大。" :
                               req.status == 415 ? "提交格式不受支持。" :
                               req.status == 411 ? "缺少 Content-Length。" :
                                                   "请求无法解析。");
    }
  else
    {
      memset(&cred, 0, sizeof(cred));
      ret = vp_form_parse(g_request + req.header_len, req.content_length,
                          &cred);
      if (ret < 0)
        {
          /* No detail about which field failed and no echo of what was typed:
           * the notice has to be useful to the person at the phone without
           * repeating a passphrase back over an open network.
           */

           {
             struct velasight_prov_credentials_s current;
             const char *ssid = NULL;

             if (velasight_provisioning_load(&current) == 0)
               ssid = current.ssid;
             len = vp_http_form_page_with_ssid(
                 g_response, sizeof(g_response),
                 "SSID 需 1-32 字节，密码需留空或 8-63 字节，"
                 "API key 需为可打印字符，请检查后重试。", ssid);
           }
          if (len > 0)
            {
              /* Reuse the form page body but answer 400, so a client that
               * checks the status still learns the submit failed.
               */

              len = vp_http_status_page(g_response, sizeof(g_response), 400,
                                        "SSID 需 1-32 字节，密码需留空或 "
                                        "8-63 字节。");
            }
        }
        else
          {
            struct velasight_prov_credentials_s previous;
            bool have_previous = velasight_provisioning_load(&previous) == 0;

            /* None of the three secret fields are pre-filled in HTML.  An
             * ordinary Wi-Fi-only resubmit must not erase any of them. */
            if (cred.api_key[0] == '\0' && have_previous)
              {
                snprintf(cred.api_key, sizeof(cred.api_key), "%s",
                         previous.api_key);
              }

            if (cred.volc_appid[0] == '\0' && have_previous)
              {
                snprintf(cred.volc_appid, sizeof(cred.volc_appid), "%s",
                         previous.volc_appid);
              }

            if (cred.volc_token[0] == '\0' && have_previous)
              {
                snprintf(cred.volc_token, sizeof(cred.volc_token), "%s",
                         previous.volc_token);
              }

            cred.generation = vp_store_next_generation(g_server.store_path);
          ret = vp_store_save(g_server.store_path, &cred);
          if (ret < 0)
            {
              *saved = false;
              *save_status = ret;
              len = vp_http_status_page(g_response, sizeof(g_response), 500,
                                        "写入持久存储失败，凭据未保存。");
            }
          else
            {
              *saved = true;
              *save_status = 0;
              *generation = cred.generation;
              len = vp_http_saved_page(g_response, sizeof(g_response),
                                       cred.ssid, cred.generation,
                                       cred.open_network);
            }
        }
    }

  if (len > 0)
    {
      ret = vp_write_response(fd, g_response, len);
      if (ret < 0)
        {
          /* Transport failure does not undo a completed persistent save.
           * In particular, NuttX may return ENOMEM when TCP write buffers are
           * temporarily exhausted by repeated phone connections.  Report the
           * stored result to the application; only vp_store_save() decides
           * whether the credentials were saved.
           */

          printf("provision_web: response write failed: %d\n", ret);
        }
    }
}

static void *vp_thread(void *arg)
{
  (void)arg;

  for (; ; )
    {
      struct pollfd pfd[2];
      uint32_t generation = 0;
      int save_status = 0;
      bool saved = false;
      bool stopping;
      int fd;
      int ret;

      pthread_mutex_lock(&g_lock);
      stopping = g_server.stopping;
      pthread_mutex_unlock(&g_lock);
      if (stopping)
        {
          break;
        }

      pfd[0].fd = g_server.listenfd;
      pfd[0].events = POLLIN;
      pfd[0].revents = 0;
      pfd[1].fd = g_server.stoprd;
      pfd[1].events = POLLIN;
      pfd[1].revents = 0;

      /* Polling rather than blocking in accept(): stop() then needs no signal
       * and no socket shutdown trick that may or may not wake accept on a
       * given stack.
       *
       * This requires CONFIG_NET_TCPBACKLOG on NuttX, which the Kconfig
       * depends on.  Without it TCP_BACKLOG events never fire, POLLIN on a
       * listening socket is unreachable, and accept() blocks with no timeout
       * at all -- measured on the board as a service that reported
       * "listening" and then answered nothing.
       */

      ret = poll(pfd, 2, VP_ACCEPT_POLL_MS);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          break;
        }

      if (ret == 0)
        {
          continue;
        }

      if (pfd[1].revents != 0 || vp_stopping())
        {
          break;
        }

      fd = accept(g_server.listenfd, NULL, NULL);
      if (fd < 0)
        {
          if (errno == EINTR || errno == ECONNABORTED)
            {
              continue;
            }

          break;
        }

      {
        struct linger linger;
        struct timeval tv;

        tv.tv_sec = VP_SOCKET_TIMEOUT_SEC;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /* NuttX otherwise leaves an active HTTP close, its send callback and
         * its close callback alive for the 120-second TIME_WAIT default.  A
         * one-second linger deadline preserves the normal graceful response
         * path, but forcibly reclaims a phone connection that never finishes
         * closing.  This is essential because callbacks are a global pool.
         */

        linger.l_onoff = 1;
        linger.l_linger = VP_SOCKET_LINGER_SEC;
        if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
                       sizeof(linger)) < 0)
          {
            printf("provision_web: SO_LINGER failed: %d\n", -errno);
          }
      }

      stopping = vp_stopping();
      if (stopping)
        {
          close(fd);
          break;
        }

      pthread_mutex_lock(&g_lock);
      if (!g_server.stopping)
        {
          g_server.clientfd = fd;
        }
      pthread_mutex_unlock(&g_lock);

      printf("provision_web: accepted client\n");
      vp_handle(fd, &saved, &save_status, &generation);

       /* Close before notifying.  The application may leave SoftAP from the
        * callback, and doing that with the response still in flight is what
        * makes a successful save look like a failed submit on the phone.
       */

      pthread_mutex_lock(&g_lock);
      if (g_server.clientfd == fd)
        {
          g_server.clientfd = -1;
        }
      pthread_mutex_unlock(&g_lock);
      close(fd);

      if (saved)
        {
          pthread_mutex_lock(&g_lock);
          g_server.generation = generation;
          pthread_mutex_unlock(&g_lock);
        }

      if (saved || save_status < 0)
        {
          if (g_server.on_saved != NULL)
            {
              g_server.on_saved(save_status, generation, g_server.cb_arg);
            }
        }

      if (saved && g_server.one_shot)
        {
          break;
        }

      pthread_mutex_lock(&g_lock);
      stopping = g_server.stopping;
      pthread_mutex_unlock(&g_lock);
      if (stopping)
        {
          break;
        }
    }

  pthread_mutex_lock(&g_lock);
  if (g_server.listenfd >= 0)
    {
      close(g_server.listenfd);
      g_server.listenfd = -1;
    }

  if (g_server.stoprd >= 0)
    {
      close(g_server.stoprd);
      g_server.stoprd = -1;
    }

  g_server.state = VP_STATE_FINISHED;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

/* Caller holds g_lock. */

static void vp_reap_locked(void)
{
  if (g_server.state == VP_STATE_FINISHED)
    {
      pthread_t thread = g_server.thread;

      pthread_mutex_unlock(&g_lock);
      pthread_join(thread, NULL);
      pthread_mutex_lock(&g_lock);
      if (g_server.stopwr >= 0)
        {
          close(g_server.stopwr);
          g_server.stopwr = -1;
        }

      g_server.state = VP_STATE_IDLE;
    }
}

int velasight_provisioning_start(
    const struct velasight_prov_config_s *config)
{
  struct velasight_prov_config_s cfg;
  struct sockaddr_in addr;
  pthread_attr_t attr;
  uint16_t port;
  int stopfds[2];
  int one = 1;
  int fd;
  int ret;

  memset(&cfg, 0, sizeof(cfg));
  if (config != NULL)
    {
      cfg = *config;
    }

  if (cfg.store_path != NULL &&
      strlen(cfg.store_path) >= VP_STORE_PATH_MAX)
    {
      return -ENAMETOOLONG;
    }

  if ((cfg.history.snapshot == NULL) != (cfg.history.open == NULL))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_lock);
  vp_reap_locked();
  if (g_server.state != VP_STATE_IDLE)
    {
      pthread_mutex_unlock(&g_lock);
      return -EALREADY;
    }

  if (pipe(stopfds) < 0)
    {
      ret = -errno;
      pthread_mutex_unlock(&g_lock);
      return ret;
    }

  port = cfg.port != 0 ? cfg.port :
         (uint16_t)CONFIG_VELASIGHT_PROVISION_PORT;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      ret = -errno;
      close(stopfds[0]);
      close(stopfds[1]);
      pthread_mutex_unlock(&g_lock);
      return ret;
    }

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, 2) < 0)
    {
      ret = -errno;
      close(fd);
      close(stopfds[0]);
      close(stopfds[1]);
      pthread_mutex_unlock(&g_lock);
      return ret;
    }

  g_server.listenfd   = fd;
  g_server.stopping   = false;
  g_server.one_shot   = cfg.one_shot;
  g_server.generation = 0;
  g_server.on_saved   = cfg.on_saved;
  g_server.cb_arg     = cfg.cb_arg;
  g_server.history   = cfg.history;
  g_server.stoprd     = stopfds[0];
  g_server.stopwr     = stopfds[1];
  g_server.clientfd   = -1;
  snprintf(g_server.store_path, sizeof(g_server.store_path), "%s",
           cfg.store_path != NULL ? cfg.store_path :
           CONFIG_VELASIGHT_PROVISION_STORE);
  g_server.state = VP_STATE_RUNNING;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, VP_THREAD_STACK);
  ret = pthread_create(&g_server.thread, &attr, vp_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      close(fd);
      close(stopfds[0]);
      close(stopfds[1]);
      g_server.listenfd = -1;
      g_server.clientfd = -1;
      g_server.stoprd = -1;
      g_server.stopwr = -1;
      g_server.state = VP_STATE_IDLE;
      pthread_mutex_unlock(&g_lock);
      return -ret;
    }

  pthread_mutex_unlock(&g_lock);

  printf("provision_web: listening on 0.0.0.0:%u, store %s\n",
         (unsigned)port, g_server.store_path);
  return 0;
}

int velasight_provisioning_stop(void)
{
  pthread_t thread;

  pthread_mutex_lock(&g_lock);
  vp_reap_locked();
  if (g_server.state != VP_STATE_RUNNING)
    {
      pthread_mutex_unlock(&g_lock);
      return -EALREADY;
    }

  g_server.stopping = true;
  thread = g_server.thread;

  /* The stop pipe interrupts request parsing and response retries.  If the
   * request was already in close(), turn that close into an abortive close so
   * SO_LINGER cannot make the network-switch worker wait for a phone ACK. */

  if (g_server.clientfd >= 0)
    {
      struct linger linger;

      linger.l_onoff = 1;
      linger.l_linger = 0;
      (void)setsockopt(g_server.clientfd, SOL_SOCKET, SO_LINGER,
                       &linger, sizeof(linger));
    }

  if (g_server.stopwr >= 0)
    {
      char wake = 1;

      while (write(g_server.stopwr, &wake, sizeof(wake)) < 0 &&
             errno == EINTR)
        {
        }
    }

  /* Called from the saved callback, which runs on the listener thread: joining
   * ourselves would deadlock, so only ask the loop to end and let the normal
   * exit path close the socket.  A later stop() from the application reaps it.
   */

  if (pthread_equal(thread, pthread_self()))
    {
      pthread_mutex_unlock(&g_lock);
      return 0;
    }

  pthread_mutex_unlock(&g_lock);
  pthread_join(thread, NULL);

  pthread_mutex_lock(&g_lock);
  if (g_server.stopwr >= 0)
    {
      close(g_server.stopwr);
      g_server.stopwr = -1;
    }

  g_server.state = VP_STATE_IDLE;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

bool velasight_provisioning_is_running(void)
{
  bool running;

  pthread_mutex_lock(&g_lock);
  running = g_server.state == VP_STATE_RUNNING;
  pthread_mutex_unlock(&g_lock);
  return running;
}

uint32_t velasight_provisioning_generation(void)
{
  uint32_t generation;

  pthread_mutex_lock(&g_lock);
  generation = g_server.generation;
  pthread_mutex_unlock(&g_lock);
  return generation;
}

int velasight_provisioning_load_from(
    const char *path, struct velasight_prov_credentials_s *out)
{
  if (path == NULL || out == NULL)
    {
      return -EINVAL;
    }

  return vp_store_load(path, out);
}

int velasight_provisioning_load(
    struct velasight_prov_credentials_s *out)
{
  char path[VP_STORE_PATH_MAX];

  pthread_mutex_lock(&g_lock);
  if (g_server.store_path[0] != '\0')
    {
      snprintf(path, sizeof(path), "%s", g_server.store_path);
    }
  else
    {
      snprintf(path, sizeof(path), "%s", CONFIG_VELASIGHT_PROVISION_STORE);
    }

  pthread_mutex_unlock(&g_lock);

  return velasight_provisioning_load_from(path, out);
}
