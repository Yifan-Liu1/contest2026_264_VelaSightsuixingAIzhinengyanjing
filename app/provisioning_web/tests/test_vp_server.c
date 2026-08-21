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
static uint16_t g_port = 18080;

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
  CHECK(write(fd, "POST /save HTTP/1.1\r\n", 21) == 21,
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

  len = http_exchange("POST /save HTTP/1.1\r\nHost: 192.168.10.1\r\n"
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

  len = http_exchange("POST /save HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 22\r\n\r\n"
                      "ssid=OpenNet&password=", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 200 OK") != NULL,
        "an open network submit is accepted");
  for (len = 0; len < 200 && cb_calls() < 2; len++)
    {
      usleep(10000);
    }

  CHECK(cb_calls() == 2 && g_cb_generation == 2,
        "the second save produces generation 2");
  CHECK(velasight_provisioning_load_from(g_path, &cred) == 0 &&
        strcmp(cred.ssid, "OpenNet") == 0 && cred.open_network,
        "the replacement is what is stored");

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
          "POST /save HTTP/1.1\r\n"
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

  len = http_exchange("POST /save HTTP/1.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 22\r\n\r\n"
                      "ssid=AIPC&password=abc", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 400") != NULL,
        "a 3-byte passphrase is refused with 400");

  len = http_exchange("POST /save HTTP/1.1\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 4\r\n\r\nssid", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 415") != NULL,
        "a wrong content type is refused with 415");

  len = http_exchange("GET /nope HTTP/1.1\r\n\r\n", reply, sizeof(reply));
  CHECK(len > 0 && strstr(reply, "HTTP/1.1 404") != NULL,
        "an unknown path is refused with 404");

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

  len = http_exchange("POST /save HTTP/1.1\r\n"
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

  len = http_exchange("POST /save HTTP/1.1\r\n"
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

  test_lifecycle();
  test_stop_interrupts_client();
  test_connection_burst_and_stop();
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
