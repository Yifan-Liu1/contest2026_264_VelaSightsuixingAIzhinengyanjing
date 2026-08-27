/****************************************************************************
 * app/provisioning_web/provisioning_web_main.c
 *
 * A console front end for the provisioning service, for bringing it up by
 * hand on a board that has been switched into SoftAP.  Applications should
 * call velasight_provisioning_start() from their own long-lived task instead:
 * the listener runs on a pthread, and a pthread does not outlive the task that
 * created it, so a command that started the service and returned would take
 * the service with it.  That is why this command stays in the foreground.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "velasight_provisioning.h"

static volatile bool g_interrupted;

static void provision_usage(void)
{
  printf("usage: provision_web [run [port] [--one-shot]] | selftest | show |"
         " path\n"
         "\n"
         "  run       serve the provisioning page until Ctrl-C.  port\n"
         "            defaults to %d.  --one-shot returns after the first\n"
         "            accepted save.\n"
         "  selftest  submit a canned form over loopback and verify it was\n"
         "            persisted.  Overwrites the stored record with a test\n"
         "            network; needs no phone.\n"
         "  show      print the stored network, never the passphrase\n"
         "  path      print the store path\n"
         "\n"
         "This service never changes the Wi-Fi role.  Put the board into\n"
         "SoftAP first (see docs/WiFi使用说明.md) and switch back afterwards.\n"
         "The page is plain HTTP with no authentication, so run it only while\n"
         "provisioning.\n",
         CONFIG_VELASIGHT_PROVISION_PORT);
}

static void provision_on_signal(int signo)
{
  (void)signo;
  g_interrupted = true;
}

static void provision_on_saved(int status, uint32_t generation, void *arg)
{
  (void)arg;

  if (status < 0)
    {
      printf("provision_web: save failed: %d\n", status);
      return;
    }

  printf("provision_web: credentials saved, generation %u\n",
         (unsigned)generation);
}

static int provision_show(void)
{
  struct velasight_prov_credentials_s cred;
  int ret;

  ret = velasight_provisioning_load(&cred);
  if (ret == -ENOENT)
    {
      printf("provision_web: nothing provisioned at %s\n",
             CONFIG_VELASIGHT_PROVISION_STORE);
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      printf("provision_web: cannot read %s: %d\n",
             CONFIG_VELASIGHT_PROVISION_STORE, ret);
      return EXIT_FAILURE;
    }

  /* Deliberately only whether a passphrase exists.  Printing it would put it
   * into the serial log, which is the one place it is guaranteed to be read by
   * someone who did not need it.
   */

  printf("provision_web: ssid=%s security=%s password=%s generation=%u\n",
         cred.ssid, cred.open_network ? "open" : "wpa2",
         cred.open_network ? "none" : "set",
         (unsigned)cred.generation);
  return EXIT_SUCCESS;
}

static int provision_run(int argc, char *argv[])
{
  struct velasight_prov_config_s cfg;
  struct sigaction sa;
  int i;
  int ret;

  memset(&cfg, 0, sizeof(cfg));
  cfg.on_saved = provision_on_saved;

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "--one-shot") == 0)
        {
          cfg.one_shot = true;
        }
      else
        {
          long port = strtol(argv[i], NULL, 10);

          if (port <= 0 || port > 65535)
            {
              printf("provision_web: bad port \"%s\"\n", argv[i]);
              return EXIT_FAILURE;
            }

          cfg.port = (uint16_t)port;
        }
    }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = provision_on_signal;
  sigaction(SIGINT, &sa, NULL);

  ret = velasight_provisioning_start(&cfg);
  if (ret < 0)
    {
      printf("provision_web: start failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("provision_web: open http://<board-ip>/ from a device on the "
         "hotspot, Ctrl-C to stop\n");

  while (!g_interrupted && velasight_provisioning_is_running())
    {
      usleep(200000);
    }

  velasight_provisioning_stop();
  printf("provision_web: stopped after %u save(s)\n",
         (unsigned)velasight_provisioning_generation());
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: provision_selftest
 *
 * Description:
 *   Drive the service over loopback on the board itself.  The host tests
 *   already cover the parsing and the record; what only the board can answer
 *   is whether this network stack accepts the listener and whether the SD-NAND
 *   write really lands.  Needs no phone, which matters because a Wi-Fi client
 *   is not always available when the firmware is.
 *
 *   It overwrites the stored record with an obvious test network.  The
 *   passphrase below is a fixed test string, never a real one.
 *
 ****************************************************************************/

#define PROV_TEST_PORT 18080
#define PROV_TEST_SSID "VelaSightProvTest"
#define PROV_TEST_PSK  "selftest-pass"

static int g_selftest_checks;
static int g_selftest_failures;
static volatile int g_selftest_cb_status = 1;
static volatile uint32_t g_selftest_cb_generation;

static void provision_check(bool ok, const char *what)
{
  g_selftest_checks++;
  if (!ok)
    {
      g_selftest_failures++;
    }

  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void provision_selftest_saved(int status, uint32_t generation,
                                     void *arg)
{
  (void)arg;
  g_selftest_cb_generation = generation;
  g_selftest_cb_status = status;
}

static int provision_exchange(const char *request, char *reply,
                              size_t replylen)
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
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(PROV_TEST_PORT);
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

      if (n <= 0)
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

static int provision_selftest(void)
{
  struct velasight_prov_credentials_s cred;
  struct velasight_prov_config_s cfg;
  static char reply[2048];
  int waited;
  int ret;

  memset(&cfg, 0, sizeof(cfg));
  cfg.port     = PROV_TEST_PORT;
  cfg.on_saved = provision_selftest_saved;

  /* NuttX keeps this app's statics between invocations of the builtin, so a
   * second `provision_web selftest` would otherwise start with the previous
   * run's callback status already at 0 -- a pass inherited rather than
   * observed -- and report accumulated check counts.
   */

  g_selftest_checks = 0;
  g_selftest_failures = 0;
  g_selftest_cb_status = 1;
  g_selftest_cb_generation = 0;

  printf("provision_web selftest: loopback port %d, store %s\n",
         PROV_TEST_PORT, CONFIG_VELASIGHT_PROVISION_STORE);

  ret = velasight_provisioning_start(&cfg);
  provision_check(ret == 0, "service starts on this network stack");
  if (ret < 0)
    {
      printf("provision_web selftest: start failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  ret = provision_exchange("GET / HTTP/1.1\r\nHost: localhost\r\n"
                           "Connection: close\r\n\r\n",
                           reply, sizeof(reply));
  provision_check(ret > 0 && strstr(reply, "200 OK") != NULL,
                  "GET / answers 200");
  provision_check(ret > 0 && strstr(reply, "name=\"ssid\"") != NULL &&
                  strstr(reply, "name=\"password\"") != NULL,
                  "the form carries both inputs");

  ret = provision_exchange("POST / HTTP/1.1\r\nHost: localhost\r\n"
                           "Content-Type: application/x-www-form-urlencoded"
                           "\r\nContent-Length: 45\r\n\r\n"
                           "ssid=" PROV_TEST_SSID
                           "&password=" PROV_TEST_PSK,
                           reply, sizeof(reply));
  if (ret <= 0 || strstr(reply, "200 OK") == NULL)
    {
      /* Print the status line and the exchange result: a 500 here means the
       * store rejected the write, which on this board has meant a path the
       * VFAT could not represent.
       */

      printf("  submit result=%d, first line: %.*s\n", ret,
             (int)strcspn(reply, "\r"), reply);
    }

  provision_check(ret > 0 && strstr(reply, "200 OK") != NULL,
                  "the submit answers 200");
  provision_check(ret > 0 && strstr(reply, PROV_TEST_SSID) != NULL,
                  "the success page names the network");
  provision_check(ret > 0 && strstr(reply, PROV_TEST_PSK) == NULL,
                  "the success page does not echo the passphrase");

  for (waited = 0; waited < 50 && g_selftest_cb_status == 1; waited++)
    {
      usleep(100000);
    }

  provision_check(g_selftest_cb_status == 0,
                  "the saved callback reported success");
  provision_check(g_selftest_cb_generation > 0,
                  "the callback carried a generation");

  ret = velasight_provisioning_load(&cred);
  if (ret < 0)
    {
      printf("  load result=%d for %s\n", ret,
             CONFIG_VELASIGHT_PROVISION_STORE);
    }

  provision_check(ret == 0, "the record reads back from persistent storage");
  if (ret == 0)
    {
      provision_check(strcmp(cred.ssid, PROV_TEST_SSID) == 0,
                      "the stored SSID matches what was submitted");
      provision_check(strcmp(cred.password, PROV_TEST_PSK) == 0,
                      "the stored passphrase matches what was submitted");
      provision_check(!cred.open_network, "the record is marked secured");
      provision_check(cred.generation == g_selftest_cb_generation,
                      "the stored generation matches the callback");
    }

  velasight_provisioning_stop();
  provision_check(!velasight_provisioning_is_running(),
                  "the service stops");

  printf("provision_web selftest: %s, %d checks, %d failures\n",
         g_selftest_failures == 0 ? "PASS" : "FAIL",
         g_selftest_checks, g_selftest_failures);
  printf("provision_web selftest: reboot and run `provision_web show` to "
         "confirm the record survives\n");
  return g_selftest_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, FAR char *argv[])
{
  const char *command = argc > 1 ? argv[1] : "run";

  if (strcmp(command, "run") == 0 || strcmp(command, "start") == 0)
    {
      return provision_run(argc, argv);
    }

  if (strcmp(command, "selftest") == 0)
    {
      return provision_selftest();
    }

  if (strcmp(command, "show") == 0)
    {
      return provision_show();
    }

  if (strcmp(command, "path") == 0)
    {
      printf("%s\n", CONFIG_VELASIGHT_PROVISION_STORE);
      return EXIT_SUCCESS;
    }

  provision_usage();
  return EXIT_FAILURE;
}
