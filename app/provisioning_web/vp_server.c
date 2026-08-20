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
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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
#define VP_STORE_PATH_MAX  192
#define VP_THREAD_STACK    8192

struct vp_server_s
{
  int       listenfd;
  pthread_t thread;
  int       state;
  bool      stopping;
  bool      one_shot;
  char      store_path[VP_STORE_PATH_MAX];
  uint32_t  generation;
  velasight_prov_saved_cb_t on_saved;
  void     *cb_arg;
};

static struct vp_server_s g_server =
{
  .listenfd = -1,
  .state    = VP_STATE_IDLE,
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Request and response buffers are static because only one connection is
 * served at a time and 6KB of thread stack on this part is not free.
 */

static char g_request[VP_HTTP_MAX_HEADERS + VP_HTTP_MAX_BODY + 1];
static char g_response[VP_HTTP_RESPONSE_MAX];

static int vp_write_all(int fd, const char *data, size_t len)
{
  size_t sent = 0;

  while (sent < len)
    {
      ssize_t n = write(fd, data + sent, len - sent);

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
          return -EPIPE;
        }

      sent += (size_t)n;
    }

  return 0;
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
      struct pollfd pfd;
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

      pfd.fd = fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, VP_ACCEPT_POLL_MS);
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
         len = vp_http_form_page_with_ssid(g_response, sizeof(g_response),
                                           NULL, ssid);
       }
    }
  else if (req.action == VP_HTTP_ACTION_REJECT)
    {
      len = vp_http_status_page(g_response, sizeof(g_response), req.status,
                               req.status == 404 ? "页面不存在。" :
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

            /* The API key field is intentionally never pre-filled in HTML.
             * An ordinary Wi-Fi-only resubmit must not erase the existing key. */
            if (cred.api_key[0] == '\0' &&
                velasight_provisioning_load(&previous) == 0)
              {
                snprintf(cred.api_key, sizeof(cred.api_key), "%s",
                         previous.api_key);
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
      ret = vp_write_all(fd, g_response, len);
      if (ret < 0)
        {
          /* Keep AP mode available when the phone disconnected before it
           * received the result page.  The record itself was already saved.
           */
          if (*saved)
            {
              *saved = false;
            }
          *save_status = ret;
        }
    }
}

static void *vp_thread(void *arg)
{
  (void)arg;

  for (; ; )
    {
      struct pollfd pfd;
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

      pfd.fd = g_server.listenfd;
      pfd.events = POLLIN;
      pfd.revents = 0;

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

      ret = poll(&pfd, 1, VP_ACCEPT_POLL_MS);
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

      fd = accept(g_server.listenfd, NULL, NULL);
      if (fd < 0)
        {
          if (errno == EINTR || errno == ECONNABORTED)
            {
              continue;
            }

          break;
        }

      printf("provision_web: accepted client\n");
      vp_handle(fd, &saved, &save_status, &generation);

       /* Close before notifying.  The application may leave SoftAP from the
        * callback, and doing that with the response still in flight is what
        * makes a successful save look like a failed submit on the phone.
       */

      shutdown(fd, SHUT_RDWR);
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

  pthread_mutex_lock(&g_lock);
  vp_reap_locked();
  if (g_server.state != VP_STATE_IDLE)
    {
      pthread_mutex_unlock(&g_lock);
      return -EALREADY;
    }

  port = cfg.port != 0 ? cfg.port :
         (uint16_t)CONFIG_VELASIGHT_PROVISION_PORT;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      ret = -errno;
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
      pthread_mutex_unlock(&g_lock);
      return ret;
    }

  g_server.listenfd   = fd;
  g_server.stopping   = false;
  g_server.one_shot   = cfg.one_shot;
  g_server.generation = 0;
  g_server.on_saved   = cfg.on_saved;
  g_server.cb_arg     = cfg.cb_arg;
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
      g_server.listenfd = -1;
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
