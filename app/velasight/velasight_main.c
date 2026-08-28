#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "include/vs_app.h"
#include "include/vs_cloud.h"

int main(int argc, FAR char *argv[])
{
  /* "velasight cloudprobe" drives one complete session against the
   * configured /contest/v1 endpoint and prints what happened, without
   * opening the displays or taking the input keys.
   *
   * It is a subcommand rather than its own application because the cloud
   * client is compiled into this program: a second PROGNAME would mean a
   * second link of the same objects. It exists because the cloud client has
   * to be verifiable on its own -- the session orchestration and the resident
   * capture workers that will eventually call it are not written yet, and
   * waiting for them to exist before finding out whether the protocol works
   * would be the wrong order to discover a protocol problem in.
   */

  if (argc > 1 && strcmp(argv[1], "cloudprobe") == 0)
    {
      return vs_cloud_probe() < 0 ? 1 : 0;
    }

  printf("velasight: taking ownership of both displays\n");
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
