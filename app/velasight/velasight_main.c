#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

#include "include/vs_app.h"

int main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  printf("velasight: starting after board display initialization\n");
  return vs_app_run();
}

static int velasight_task(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  return vs_app_run();
}

int velasight_autostart(void)
{
  int pid;

  pid = task_create("velasight", SCHED_PRIORITY_DEFAULT, 8192,
                    velasight_task, NULL);
  return pid < 0 ? pid : 0;
}
