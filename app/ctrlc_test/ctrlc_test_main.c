/****************************************************************************
 * BK7258 UART Ctrl-C test
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t g_sigint_received;

static void ctrlc_test_handler(int signo)
{
  (void)signo;
  g_sigint_received = 1;
}

int ctrlc_test_main(int argc, char *argv[])
{
  struct sigaction act;

  (void)argc;
  (void)argv;
  act.sa_handler = ctrlc_test_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;

  if (sigaction(SIGINT, &act, NULL) < 0)
    {
      perror("sigaction");
      return 1;
    }

  printf("ctrlc_test running; press Ctrl-C\n");
  while (!g_sigint_received)
    {
      sleep(1);
    }

  printf("ctrlc_test received SIGINT\n");
  return 0;
}
