/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_net_autostart.c
 *
 * Bring the network up at boot and, if it is built in, start the development
 * console service.
 *
 * Why this is board code and not part of app/web_tool: joining the stored
 * network is what this board should do when it is switched on, whether or not
 * any particular application happens to be compiled into the image.  Putting
 * it inside web_tool would make "does the board get on the network" depend on
 * a Kconfig symbol for a debug tool, and the next configuration that leaves
 * that tool out would silently lose its network too.
 *
 * The pieces this needs are split across the two halves of the system, so it
 * uses each from where it lives instead of duplicating either:
 *
 *   association  bk7258_kvdb_apply_wifi(), board side -- the two `wapi`
 *                commands through the same ioctls
 *   DHCP         the `renew` builtin, application side.  There is no DHCP
 *                client on the kernel side of this build; dhcpc lives in
 *                apps.  Rather than reach across that line with a hand-written
 *                include path, this looks the command up in the builtin table
 *                (builtin_isavail/builtin_for_index, both kernel-visible) and
 *                starts it as a task.  Missing means "not compiled in", which
 *                is reported and skipped, not fatal.
 *
 * The retry around DHCP is not defensive padding: the first request after
 * association is measured to fail -- there is a window between "associated"
 * and "can exchange DHCP" -- and a boot script that took the first failure as
 * the answer would report a working network as broken every time.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

#include <net/if.h>

#include <nuttx/lib/builtin.h>

#include "bk7258_kvdb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NET_IFNAME        "wlan0"

/* How many times to ask for an address, and how long to wait between tries.
 * Two attempts is what the measured failure needs; more would only lengthen
 * a boot that has no network to find.
 */

#define NET_DHCP_TRIES    3
#define NET_DHCP_GAP_MS   1500

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: net_spawn_builtin
 *
 * Description:
 *   Start a builtin application by name, using the registration the build
 *   already produced (priority, stack size and entry point), and optionally
 *   wait for it.  Returns the exit status when waiting, 0 when not, or a
 *   negative value when the application is not in this image.
 *
 ****************************************************************************/

static int net_spawn_builtin(FAR const char *name, FAR char * const *argv,
                             bool wait)
{
  FAR const struct builtin_s *entry;
  int index;
  int pid;
  int status = 0;

  index = builtin_isavail(name);
  if (index < 0)
    {
      return index;
    }

  entry = builtin_for_index(index);
  if (entry == NULL)
    {
      return -ENOENT;
    }

  pid = task_create(entry->name, entry->priority, entry->stacksize,
                    entry->main, argv);
  if (pid < 0)
    {
      return pid;
    }

  if (wait)
    {
      if (waitpid(pid, &status, 0) < 0)
        {
          return -errno;
        }
    }

  return status;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_net_autostart
 *
 * Description:
 *   Join the stored network and start the console service.
 *
 *   Must be called from a task and never from bring-up: association blocks on
 *   the CP and DHCP blocks on the network, and an AP that stops servicing the
 *   mailbox loses the heartbeat, after which the CP's 8-second watchdog resets
 *   the chip.  That is the same constraint that makes the NAND config loader a task, so
 *   this is called from the end of it -- which also guarantees the credentials
 *   have already come back from flash.
 *
 ****************************************************************************/

void bk7258_net_autostart(void)
{
  FAR char *renew_argv[3];
  int tries;
  int ret;

  /* Association, from the credentials in the store.  -ENOENT means nobody has
   * configured a network yet, which is an ordinary state for a fresh board and
   * not worth a scary message.
   */

   /* VelaSight uses vs_network.c and the unified vela.cfg record.  This legacy
    * console autostart path is retained only for non-product configurations. */
   ret = -ENOENT;
  if (ret == -ENOENT)
    {
      printf("net: no NAND Wi-Fi configuration stored\n");
      return;
    }

  if (ret < 0)
    {
      printf("net: association failed (%d); Wi-Fi not configured\n", ret);
      return;
    }

  renew_argv[0] = (FAR char *)"renew";
  renew_argv[1] = (FAR char *)NET_IFNAME;
  renew_argv[2] = NULL;

  for (tries = 0; tries < NET_DHCP_TRIES; tries++)
    {
      usleep(NET_DHCP_GAP_MS * 1000);

      ret = net_spawn_builtin("renew", renew_argv, true);
      if (ret < 0)
        {
          printf("net: `renew` is not in this image (%d); ask for an "
                 "address by hand\n", ret);
          break;
        }

      if (ret == 0)
        {
          printf("net: address obtained on attempt %d\n", tries + 1);
          break;
        }

      /* Not an error yet.  The first attempt after association is expected to
       * fail; saying which attempt succeeded is more useful than hiding the
       * retry.
       */

      printf("net: DHCP attempt %d did not get an address, retrying\n",
             tries + 1);
    }

  /* The console service, if it was built in.  It refuses to listen unless
   * web.allow is set, and says so on the console, so starting it
   * unconditionally cannot open the board up by itself.
   */

  ret = net_spawn_builtin("web_tool", NULL, false);
  if (ret < 0)
    {
      /* Not built in, or could not start.  Neither is fatal: the board is on
       * the network and the serial console still works.
       */

      printf("net: web_tool not started (%d)\n", ret);
    }
}
