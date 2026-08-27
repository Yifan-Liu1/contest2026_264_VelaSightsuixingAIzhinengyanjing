/****************************************************************************
 * app/provisioning_web/tests/test_vp_server.c
 *
 * The listener over loopback.  This is the part that would otherwise only be
 * testable with a phone, a SoftAP switch and a re-flash, so it is worth the
 * sockets: the ordering guarantee (response first, callback second) and
 * one-shot mode are exactly the behaviours an application depends on.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "velasight_provisioning.h"
#include "vp_store.h"

/* One byte past VELASIGHT_PROV_VOLC_APPID_MAX, so the field is refused on
 * length rather than on content.
 */

#define LONG_APPID_65 \
  "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                       \
  do                                                            \
    {                                                           \
      g_checks++;                                                \
      if (!(cond))                                              \
        {                                                       \
          g_failures++;                                         \
          printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);   \
        }                                                       \
    }                                                           \
  while (0)

static char g_dir[] = "/tmp/vp_server_testXXXXXX";
static char g_path[256];
static char g_history_path[256];
static uint16_t g_port = 18080;
static int g_history_open_calls;

static const struct velasight_prov_history_entry_s g_history_entries[] =
{
  {
    .record_key = "R0000001",
    .date = "运行+0:00:02",
    .title = "最近记录",
    .summary = "正常摘要",
    .calm = 65,
    .happy = 25,
    .tense = 10,
    .incomplete = false,
  },
  {
    .record_key = "R0000000",
    .date = "<date>",
    .title = "<script>alert(1)</script>",
    .summary = "A&B",
    .calm = 40,
    .happy = 20,
    .tense = 40,
    .incomplete = true,
  },
};

static int history_snapshot(
    unsigned int offset, struct velasight_prov_history_entry_s *out,
    size_t capacity, unsigned int *total, unsigned int *copied, void *arg)
{
  const unsigned int count =
    (unsigned int)(sizeof(g_history_entries) / sizeof(g_history_entries[0]));
  unsigned int available;
  unsigned int n;

  (void)arg;
  if (total == NULL || copied == NULL ||
      (capacity > 0 && out == NULL))
    {
      return -EINVAL;
    }

  *total = count;
  *copied = 0;
  if (offset >= count || capacity == 0)
    {
      return 0;
    }

  available = count - offset;
  n = available < capacity ? available : (unsigned int)capacity;
  memcpy(out, &g_history_entries[offset], (size_t)n * sizeof(*out));
  *copied = n;
  return 0;
}

static int history_open(const char *record_key, int *fd, size_t *size,
                        void *arg)
{
  struct stat st;
  int opened;

  (void)arg;
  g_history_open_calls++;
  if (record_key == NULL || fd == NULL || size == NULL ||
      (strcmp(record_key, "R0000000") != 0 &&
       strcmp(record_key, "R0000001") != 0))
    {
      return -ENOENT;
    }

  opened = open(g_history_path, O_RDONLY);
  if (opened < 0)
    {
      return -errno;
    }

  if (fstat(opened, &st) < 0 || st.st_size < 0)
    {
      int error = errno != 0 ? -errno : -EIO;
      close(opened);
      return error;
    }

  *fd = opened;
  *size = (size_t)st.st_size;
  return 0;
}

/* The callback records what it saw and, more importantly, whether the socket
 * had already been closed when it ran.
 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cb_calls;
static int g_cb_status;
static uint32_t g_cb_generation;

static void on_saved(int status, uint32_t generation, void *arg)
{
  pthread_mutex_lock(&g_lock);
  g_cb_calls++;
  g_cb_status = status;
  g_cb_generation = generation;
  pthread_mutex_unlock(&g_lock);

  if (arg != NULL)
    {
      *(bool *)arg = true;
    }
}

static int cb_calls(void)
{
  int calls;

  pthread_mutex_lock(&g_lock);
  calls = g_cb_calls;
  pthread_mutex_unlock(&g_lock);
  return calls;
}

static void reset_callback(void)
{
  pthread_mutex_lock(&g_lock);
  g_cb_calls = 0;
  g_cb_status = -1;
  g_cb_generation = 0;
  pthread_mutex_unlock(&g_lock);
}

/****************************************************************************
 * A blunt HTTP client: send a request, read until the peer closes.  The close
 * matters -- the service must not leave the connection open after answering,
 * because the application is about to take the network away.
 ****************************************************************************/

static int http_exchange(const char *request, char *reply, size_t replylen)
{
  struct sockaddr_in addr;
  struct timeval tv;
  size_t total = 0;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  tv.tv_sec  = 5;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(g_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      int error = -errno;
      close(fd);
      return error;
    }

  if (write(fd, request, strlen(request)) < 0)
    {
      int error = -errno;
      close(fd);
      return error;
    }

  for (; ; )
    {
      ssize_t n = read(fd, reply + total, replylen - 1 - total);

      if (n < 0)
        {
          int error = -errno;
          close(fd);
          return error;
        }

      if (n == 0)
        {
          break;
        }

      total += (size_t)n;
      if (total >= replylen - 1)
        {
          break;
        }
    }

  close(fd);
  reply[total] = '\0';
  return (int)total;
}

static int start_on_free_port(const struct velasight_prov_config_s *base)
{
  struct velasight_prov_config_s cfg = *base;
  int attempt;
  int ret = -1;

  for (attempt = 0; attempt < 32; attempt++)
    {
      cfg.port = g_port;
      ret = velasight_provisioning_start(&cfg);
      if (ret == 0)
        {
          return 0;
        }

      g_port = (uint16_t)(g_port + 1);
    }

  return ret;
}

static void test_lifecycle(void)
{
  struct velasight_prov_config_s cfg;

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  cfg.on_saved   = on_saved;

  CHECK(!velasight_provisioning_is_running(),
        "nothing is running before start");
  CHECK(velasight_provisioning_stop() == -EALREADY,
        "stopping an idle service reports -EALREADY");

  CHECK(start_on_free_port(&cfg) == 0, "the service starts");
  CHECK(velasight_provisioning_is_running(), "the service reports running");
  CHECK(velasight_provisioning_start(&cfg) == -EALREADY,
        "a second start is refused instead of leaking a listener");
  CHECK(velasight_provisioning_stop() == 0, "the service stops");
  CHECK(!velasight_provisioning_is_running(), "the service is idle again");
  CHECK(velasight_provisioning_stop() == -EALREADY,
        "a second stop reports -EALREADY");
}

static void test_stop_interrupts_client(void)
{
  struct velasight_prov_config_s cfg;
  struct sockaddr_in addr;
  struct timespec start;
  struct timespec end;
  long elapsed_ms;
  int fd;

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  CHECK(start_on_free_port(&cfg) == 0,
        "the service starts for an interrupted client");

  fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0, "the stalled client socket opens");
  if (fd < 0)
    {
      (void)velasight_provisioning_stop();
      return;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0,
        "the stalled client connects");
  CHECK(write(fd, "POST / HTTP/1.1\r\n", 17) == 17,
        "the stalled client sends an incomplete request");
  usleep(50000);

  clock_gettime(CLOCK_MONOTONIC, &start);
  CHECK(velasight_provisioning_stop() == 0,
        "stop interrupts an active client");
  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
               (end.tv_nsec - start.tv_nsec) / 1000000L;
  CHECK(elapsed_ms < 500,
        "stop does not wait for the client request timeout");
  close(fd);
}

static void test_connection_burst_and_stop(void)
{
  struct velasight_prov_config_s cfg;
  struct timespec start;
  struct timespec end;
  char reply[8192];
  long elapsed_ms;
  int request;

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  CHECK(start_on_free_port(&cfg) == 0,
        "the service starts for a browser connection burst");

  for (request = 0; request < 24; request++)
    {
      int len = http_exchange(
          "GET / HTTP/1.1\r\nHost: 192.168.10.1\r\n"
          "Connection: close\r\n\r\n", reply, sizeof(reply));

      CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
            "every short browser connection receives the form");
    }

  clock_gettime(CLOCK_MONOTONIC, &start);
  CHECK(velasight_provisioning_stop() == 0,
        "the service stops after the connection burst");
  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
               (end.tv_nsec - start.tv_nsec) / 1000000L;
  CHECK(elapsed_ms < 500,
        "connection cleanup cannot hold the network switch worker");
}

static void *slow_history_reader(void *arg)
{
  int fd = *(int *)arg;
  char buffer[4096];

  for (;;)
    {
      ssize_t n = read(fd, buffer, sizeof(buffer));

      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          break;
        }

      /* Keep making enough progress that the former per-chunk timeout never
       * expired, while making a multi-megabyte response take far longer than
       * the one-second response-wide service budget. */
      usleep(75000);
    }

  return NULL;
}

static void test_history_response_deadline(void)
{
  static const char request[] =
      "GET /history/R0000000 HTTP/1.1\r\nConnection: close\r\n\r\n";
  struct velasight_prov_config_s cfg;
  struct sockaddr_in addr;
  struct timespec start;
  struct timespec end;
  struct timeval tv;
  pthread_t reader;
  char reply[8192];
  long elapsed_ms;
  int receive_buffer = 4096;
  int history_fd;
  int reader_ret;
  int slow_fd;
  int len;

  history_fd = open(g_history_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(history_fd >= 0 &&
        ftruncate(history_fd, (off_t)(32u * 1024u * 1024u)) == 0,
        "a large sparse history fixture is created");
  if (history_fd < 0)
    {
      return;
    }
  close(history_fd);

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  cfg.history.snapshot = history_snapshot;
  cfg.history.open = history_open;
  CHECK(start_on_free_port(&cfg) == 0,
        "the service starts for the response-deadline test");
  if (!velasight_provisioning_is_running())
    {
      return;
    }

  slow_fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(slow_fd >= 0, "the paced history client socket opens");
  if (slow_fd < 0)
    {
      (void)velasight_provisioning_stop();
      return;
    }

  tv.tv_sec = 6;
  tv.tv_usec = 0;
  (void)setsockopt(slow_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(slow_fd, SOL_SOCKET, SO_RCVBUF,
                   &receive_buffer, sizeof(receive_buffer));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(connect(slow_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0,
        "the paced history client connects");
  CHECK(write(slow_fd, request, sizeof(request) - 1u) ==
        (ssize_t)(sizeof(request) - 1u),
        "the paced history client requests a large record");

  reader_ret = pthread_create(&reader, NULL, slow_history_reader, &slow_fd);
  CHECK(reader_ret == 0, "the paced history reader starts");
  if (reader_ret != 0)
    {
      close(slow_fd);
      (void)velasight_provisioning_stop();
      return;
    }

  usleep(100000);
  clock_gettime(CLOCK_MONOTONIC, &start);
  len = http_exchange("GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
                      reply, sizeof(reply));
  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
               (end.tv_nsec - start.tv_nsec) / 1000000L;

  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
        "a second client is served after the slow response is abandoned");
  CHECK(elapsed_ms < 4000,
        "one response deadline bounds the single listener occupation");

  (void)shutdown(slow_fd, SHUT_RDWR);
  pthread_join(reader, NULL);
  close(slow_fd);
  CHECK(velasight_provisioning_stop() == 0,
        "the response-deadline service stops");
}

static void test_history_routes(void)
{
  static const char prefix[] = "{\"text\":\"";
  static const char suffix[] = "\"}";
  struct velasight_prov_config_s cfg;
  const size_t payload_len = 7000;
  const size_t json_len = sizeof(prefix) - 1 + payload_len +
                          sizeof(suffix) - 1;
  char *json = malloc(json_len);
  char *reply = malloc(json_len + 16384);
  char *body;

  /* Its own store.  This test submits once to prove the confirmation renders
   * in place, and the shared record's generation counter is asserted exactly
   * by test_get_and_save(), so sharing the file would make one test's coverage
   * depend on the other's execution order.
   */

  char store[sizeof(g_path) + 16];
  size_t written = 0;
  int history_fd;
  int before;
  int len;

  snprintf(store, sizeof(store), "%s.hist", g_path);
  unlink(store);

  CHECK(json != NULL && reply != NULL,
        "history route test buffers allocate");
  if (json == NULL || reply == NULL)
    {
      free(json);
      free(reply);
      return;
    }

  memcpy(json, prefix, sizeof(prefix) - 1);
  memset(json + sizeof(prefix) - 1, 'x', payload_len);
  memcpy(json + sizeof(prefix) - 1 + payload_len, suffix,
         sizeof(suffix) - 1);

  history_fd = open(g_history_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(history_fd >= 0, "the fake history file opens");
  while (history_fd >= 0 && written < json_len)
    {
      ssize_t n = write(history_fd, json + written, json_len - written);

      if (n < 0 && errno == EINTR)
        {
          continue;
        }
      if (n <= 0)
        {
          break;
        }
      written += (size_t)n;
    }
  if (history_fd >= 0)
    {
      close(history_fd);
    }
  CHECK(written == json_len, "the fake history file is complete");

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = store;
  cfg.history.snapshot = history_snapshot;
  CHECK(velasight_provisioning_start(&cfg) == -EINVAL,
        "a half-configured history provider is rejected");
  cfg.history.open = history_open;

  reset_callback();
  g_history_open_calls = 0;
  CHECK(start_on_free_port(&cfg) == 0,
        "the service starts with a history provider");

  len = http_exchange("GET / HTTP/1.1\r\n\r\n", reply,
                      json_len + 16384);
  CHECK(len > 0 && strstr(reply, "href=\"/history\"") != NULL,
        "the root page exposes history when configured");

  /* The submit answers at the address it was posted to, carrying both the
   * result and the tab bar.  This is the whole point of dropping the separate
   * endpoint: the phone stays on one URL, so the bar it can see is the bar for
   * the page it is actually on.
   */

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 32\r\n\r\n"
                      "ssid=TabBar&password=goodpass123",
                      reply, json_len + 16384);
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL &&
        strstr(reply, "class=\"ok\">已保存") != NULL,
        "the submit answers in place and keeps its confirmation");
  CHECK(strstr(reply, "class=\"tab on\" href=\"/\">联网设置") != NULL &&
        strstr(reply, "class=\"tab\" href=\"/history\">聊天记录") != NULL,
        "the confirmation carries the tab bar, so history stays reachable");
  CHECK(strstr(reply, "action=\"/\"") != NULL &&
        strstr(reply, "/save") == NULL,
        "the form still posts to the page itself, never to a save endpoint");

  len = http_exchange("GET /history HTTP/1.1\r\n\r\n", reply,
                      json_len + 16384);
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL &&
        strstr(reply, "R0000001") != NULL &&
        strstr(reply, "href=\"/history/R0000000/download\"") != NULL,
        "the history list contains preview and download links");
  CHECK(strstr(reply, "class=\"tab on\" href=\"/history\">聊天记录") != NULL,
        "the history page marks itself as the page being shown");
  CHECK(strstr(reply, "<script>") == NULL &&
        strstr(reply, "&lt;script&gt;") != NULL &&
        strstr(reply, "A&amp;B") != NULL,
        "the server escapes untrusted history metadata");

  len = http_exchange("GET /history/R0000001 HTTP/1.1\r\n\r\n", reply,
                      json_len + 16384);
  body = len > 0 ? strstr(reply, "\r\n\r\n") : NULL;
  if (body != NULL)
    {
      body += 4;
    }
  CHECK(body != NULL && strstr(reply,
        "Content-Type: application/json; charset=utf-8") != NULL &&
        (size_t)(len - (int)(body - reply)) == json_len &&
        memcmp(body, json, json_len) == 0,
        "JSON preview streams every byte beyond the 4KB response buffer");

  len = http_exchange(
      "GET /history/R0000000/download HTTP/1.1\r\n\r\n", reply,
      json_len + 16384);
  body = len > 0 ? strstr(reply, "\r\n\r\n") : NULL;
  if (body != NULL)
    {
      body += 4;
    }
  CHECK(body != NULL && strstr(reply,
        "Content-Disposition: attachment; filename=\"R0000000.json\"") !=
        NULL && (size_t)(len - (int)(body - reply)) == json_len &&
        memcmp(body, json, json_len) == 0,
        "history download is a complete attachment");

  before = g_history_open_calls;
  len = http_exchange("GET /history/R000000X HTTP/1.1\r\n\r\n", reply,
                      json_len + 16384);
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 404") != NULL &&
        g_history_open_calls == before,
        "an invalid key is rejected before the provider");
  len = http_exchange("GET /history/R9999999 HTTP/1.1\r\n\r\n", reply,
                      json_len + 16384);
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 404") != NULL,
        "a missing valid history key is 404");
  CHECK(cb_calls() == 0,
        "history reads never trigger the credential callback");
  CHECK(velasight_provisioning_stop() == 0,
        "the history-enabled service stops");

  free(reply);
  free(json);
  unlink(store);
}

static void test_get_and_save(void)
{
  struct velasight_prov_credentials_s cred;
  struct velasight_prov_config_s cfg;
  volatile bool saw_callback = false;
  char reply[8192];
  int len;

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  cfg.on_saved   = on_saved;
  cfg.cb_arg     = (void *)&saw_callback;

  reset_callback();
  CHECK(start_on_free_port(&cfg) == 0, "the service starts for a submit");

  len = http_exchange("GET / HTTP/1.1\r\nHost: 192.168.10.1\r\n"
                      "Connection: close\r\n\r\n", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL &&
        strstr(reply, "name=\"ssid\"") != NULL,
        "GET / returns the form and the peer closes");
  CHECK(cb_calls() == 0, "serving the page does not fire the callback");

  len = http_exchange("POST / HTTP/1.1\r\nHost: 192.168.10.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 30\r\n\r\n"
                      "ssid=AIPC&password=testpass123", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
        "a valid submit is accepted");
  CHECK(strstr(reply, "AIPC") != NULL,
        "the success page names the network that was saved");
  CHECK(strstr(reply, "testpass123") == NULL,
        "the success page does not echo the passphrase");

  /* The response arrived and the socket closed before the callback ran; the
   * flag was set by the callback, so seeing it now proves the order rather
   * than merely that both happened.
   */

  for (len = 0; len < 200 && cb_calls() == 0; len++)
    {
      usleep(10000);
    }

  CHECK(cb_calls() == 1, "the callback fired exactly once");
  CHECK(saw_callback, "the callback ran after the response was delivered");
  CHECK(g_cb_status == 0, "the callback reported success");
  CHECK(g_cb_generation == 1, "the callback carried generation 1");
  CHECK(velasight_provisioning_generation() == 1,
        "the service exposes the same generation");

  CHECK(velasight_provisioning_load_from(g_path, &cred) == 0 &&
        strcmp(cred.ssid, "AIPC") == 0 &&
        strcmp(cred.password, "testpass123") == 0 && cred.generation == 1,
        "the credentials are on disk and readable");

  /* Still listening: the default is to stay up until the application stops
   * it, and a second submit replaces the first with a new generation.
   */

  CHECK(velasight_provisioning_is_running(),
        "the service keeps running after a save by default");

  /* The regression this whole split exists for.  Someone opens the page to
   * add a key; the password box is blank because it is never pre-filled.  The
   * stored passphrase has to survive that, and until it did, a device that had
   * been working came back as an open network it could no longer join.
   */

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 43\r\n\r\n"
                      "ssid=AIPC&password=&mimo_apikey=added-later",
                      reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
        "a resubmit with a blank password box is accepted");
  for (len = 0; len < 200 && cb_calls() < 2; len++)
    {
      usleep(10000);
    }

  CHECK(velasight_provisioning_load_from(g_path, &cred) == 0 &&
        strcmp(cred.password, "testpass123") == 0 && !cred.open_network,
        "a blank password box leaves the stored passphrase alone");
  CHECK(strcmp(cred.api_key, "added-later") == 0,
        "the key that was actually submitted is stored");

  /* Clearing it is still possible, but only by saying so.  The checkbox is
   * the whole difference between an intention and an accident.
   */

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 37\r\n\r\n"
                      "ssid=OpenNet&password=&no_password=on",
                      reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
        "a submit for a network with no password is accepted");
  CHECK(strstr(reply, "记成了没有密码") != NULL,
        "the confirmation warns that the network was stored without one");
  for (len = 0; len < 200 && cb_calls() < 3; len++)
    {
      usleep(10000);
    }

  CHECK(cb_calls() == 3 && g_cb_generation == 3,
        "the third save produces generation 3");
  CHECK(velasight_provisioning_load_from(g_path, &cred) == 0 &&
        strcmp(cred.ssid, "OpenNet") == 0 && cred.open_network &&
        cred.password[0] == '\0',
        "ticking the box is what clears the stored passphrase");
  CHECK(strcmp(cred.api_key, "added-later") == 0,
        "clearing the passphrase does not disturb the stored key");

  CHECK(velasight_provisioning_stop() == 0, "the service stops after saves");
}

static void test_repeated_password_save_cycles(void)
{
  struct velasight_prov_credentials_s cred;
  struct velasight_prov_config_s cfg;
  char path[sizeof(g_path) + 16];
  char reply[8192];
  int cycle;

  snprintf(path, sizeof(path), "%s.cycles", g_path);
  unlink(path);
  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = path;
  cfg.on_saved = on_saved;

  for (cycle = 0; cycle < 4; cycle++)
    {
      int wait;
      int len;

      reset_callback();
      CHECK(start_on_free_port(&cfg) == 0,
            "the service restarts for a password save cycle");
      len = http_exchange(
          "POST / HTTP/1.1\r\n"
          "Content-Type: application/x-www-form-urlencoded\r\n"
          "Content-Length: 35\r\n\r\n"
          "ssid=CycleNet&password=cyclepass123", reply, sizeof(reply));
      CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
            "the password save cycle returns success");
      for (wait = 0; wait < 200 && cb_calls() == 0; wait++)
        {
          usleep(10000);
        }

      CHECK(cb_calls() == 1 && g_cb_status == 0,
            "the password save cycle reports persistence success");
      CHECK(velasight_provisioning_stop() == 0,
            "the service stops after a password save cycle");
    }

  CHECK(velasight_provisioning_load_from(path, &cred) == 0 &&
        strcmp(cred.ssid, "CycleNet") == 0 &&
        strcmp(cred.password, "cyclepass123") == 0 &&
        cred.generation == 4,
        "repeated password saves retain the latest valid record");
  unlink(path);
}

static void test_rejections(void)
{
  struct velasight_prov_credentials_s before;
  struct velasight_prov_credentials_s after;
  struct velasight_prov_config_s cfg;
  char reply[8192];
  int len;

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  cfg.on_saved   = on_saved;

  CHECK(velasight_provisioning_load_from(g_path, &before) == 0,
        "a stored record exists before the rejection tests");

  reset_callback();
  CHECK(start_on_free_port(&cfg) == 0, "the service starts for rejections");

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 22\r\n\r\n"
                      "ssid=AIPC&password=abc", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 400") != NULL,
        "a 3-byte passphrase is refused with 400");

  /* A refusal that answered a bare status page cost the user everything they
   * had typed, which for a 512-byte key means retyping all of it to fix one
   * character.  The form comes back, the network name is still in it, and the
   * notice says which field is wrong.
   */

  CHECK(strstr(reply, "name=\"password\"") != NULL &&
        strstr(reply, "value=\"AIPC\"") != NULL,
        "a refused submit returns the form with the stored name pre-filled");
  CHECK(strstr(reply, "Wi-Fi 密码需要 8 到 63 个字符") != NULL,
        "the refusal names the field that was wrong");
  CHECK(strstr(reply, "abc") == NULL,
        "the refusal does not echo what was typed into the password box");

  /* When the name itself is what failed, echoing it back would leave the user
   * hunting for a character they cannot see, so that one case falls back to
   * the stored name instead.
   */

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 33\r\n\r\n"
                      "ssid=bad%09name&password=goodpass",
                      reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 400") != NULL &&
        strstr(reply, "value=\"OpenNet\"") != NULL,
        "a bad network name falls back to the stored one, not itself");

  /* An over-long value is a different remedy from an unacceptable one, so it
   * gets a different sentence rather than the same catch-all.
   */

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 86\r\n\r\n"
                      "ssid=AIPC&volc_appid=" LONG_APPID_65,
                      reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 400") != NULL &&
        strstr(reply, "语音 App ID 太长了") != NULL,
        "an over-long app id is refused by name and by reason");

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Sec-Fetch-Site: cross-site\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 32\r\n\r\n"
                      "ssid=Blocked&password=passphrase",
                      reply, sizeof(reply));
  CHECK(len > 0 &&
        strstr(reply, "HTTP/1.1 403 Forbidden") != NULL,
        "an explicit cross-site submit is refused with 403");

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 4\r\n\r\nssid", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 415") != NULL,
        "a wrong content type is refused with 415");

  len = http_exchange("GET /nope HTTP/1.1\r\n\r\n", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 404") != NULL,
        "an unknown path is refused with 404");

  len = http_exchange("GET /history HTTP/1.1\r\n\r\n", reply,
                      sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 404") != NULL,
        "history is hidden when no provider is configured");

  len = http_exchange("PUT / HTTP/1.1\r\n\r\n", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 405") != NULL,
        "an unsupported method is refused with 405");

  CHECK(cb_calls() == 0, "no rejection fires the saved callback");
  CHECK(velasight_provisioning_load_from(g_path, &after) == 0 &&
        after.generation == before.generation &&
        strcmp(after.ssid, before.ssid) == 0,
        "a rejected submit leaves the stored record untouched");
  CHECK(velasight_provisioning_is_running(),
        "rejections do not stop the service");
  CHECK(velasight_provisioning_stop() == 0, "the service stops");
}

static void test_one_shot(void)
{
  struct velasight_prov_config_s cfg;
  char reply[8192];
  int len;

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = g_path;
  cfg.on_saved   = on_saved;
  cfg.one_shot   = true;

  reset_callback();
  CHECK(start_on_free_port(&cfg) == 0, "the one-shot service starts");

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 32\r\n\r\n"
                      "ssid=OneShot&password=passphrase", reply,
                      sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
        "the one-shot submit is accepted and answered");

  for (len = 0; len < 300 && velasight_provisioning_is_running(); len++)
    {
      usleep(10000);
    }

  CHECK(!velasight_provisioning_is_running(),
        "one-shot mode stops itself after a save");
  CHECK(cb_calls() == 1, "one-shot mode still notifies the application");
  CHECK(velasight_provisioning_stop() == -EALREADY,
        "stopping an already finished one-shot service is not an error");
}

static void test_store_failure_is_reported(void)
{
  struct velasight_prov_config_s cfg;
  char reply[8192];
  int len;

  /* An unwritable store is what SD-NAND being unmounted looks like from here.
   * The submit must fail loudly rather than answer 200 and lose the input.
   */

  memset(&cfg, 0, sizeof(cfg));
  cfg.store_path = "/tmp/vp_no_such_dir/x/y/rec.bin";
  cfg.on_saved   = on_saved;

  reset_callback();
  CHECK(start_on_free_port(&cfg) == 0,
        "the service starts even with an unusable store");

  len = http_exchange("POST / HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 31\r\n\r\n"
                      "ssid=AIPC&password=passphrase12", reply,
                      sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 500") != NULL,
        "a failed write answers 500");

  for (len = 0; len < 200 && cb_calls() == 0; len++)
    {
      usleep(10000);
    }

  CHECK(cb_calls() == 1 && g_cb_status < 0,
        "the application is told the save failed");
  CHECK(velasight_provisioning_stop() == 0, "the service stops");
}

static void cleanup(void)
{
  char path[sizeof(g_path) + 8];

  snprintf(path, sizeof(path), "%s.tmp", g_path);
  unlink(path);
  unlink(g_path);
  unlink(g_history_path);
  rmdir(g_dir);
}

int main(void)
{
  int status;

  if (mkdtemp(g_dir) == NULL)
    {
      printf("FAIL cannot create a temporary directory\n");
      return 1;
    }

  snprintf(g_path, sizeof(g_path), "%s/wifi-provision.bin", g_dir);
  snprintf(g_history_path, sizeof(g_history_path), "%s/R0000000.JSN", g_dir);

  test_lifecycle();
  test_stop_interrupts_client();
  test_connection_burst_and_stop();
  test_history_response_deadline();
  test_history_routes();
  test_get_and_save();
  test_repeated_password_save_cycles();
  test_rejections();
  test_one_shot();
  test_store_failure_is_reported();
  cleanup();

  status = g_failures == 0 ? 0 : 1;
  printf("%s: %d checks, %d failures\n",
         status == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return status;
}
