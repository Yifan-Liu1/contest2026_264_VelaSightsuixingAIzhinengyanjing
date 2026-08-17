/****************************************************************************
 * app/kvdb_tool/kvdb_tool_main.c
 *
 * `kvdb` -- read and write the configuration that survives a reset.
 *
 * The store lives in the AP's own flash partition and is written through the
 * CP's flash service; see board/beken/chips/bk7258/bk7258_kvdb.c.  What this
 * command exists for is the developer loop: type the API key and the Wi-Fi
 * passphrase once, on the board, and never again -- instead of compiling the
 * key into the image through packages/ai_agent/include/agent_secrets.h (a
 * public repository, and the key ends up in the .bin) or re-entering the
 * Wi-Fi credentials after every reset (/mnt is a PSRAM ramdisk).
 *
 * Usage:
 *   kvdb                       list keys, secrets masked
 *   kvdb list [--raw]          same; --raw prints secrets in full
 *   kvdb get <key> [--raw]     print one value
 *   kvdb set <key> <value>     write and persist
 *   kvdb del <key>             remove and persist
 *
 * Anything whose key ends in ".key" or ".psk" is masked unless --raw is
 * given, because these values otherwise end up in serial logs and in the AI
 * coding logs this project has to submit.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <arch/board/kvdb.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void kvdb_usage(void)
{
  printf("Usage: kvdb [list|get|set|del] ...\n"
         "  kvdb                    list keys (secrets masked)\n"
         "  kvdb list [--raw]       same, --raw shows secrets\n"
         "  kvdb get <key> [--raw]  print one value\n"
         "  kvdb set <key> <value>  write and persist across reset\n"
         "  kvdb del <key>          remove and persist\n"
         "\n"
         "Keys the rest of the system reads:\n"
         "  llm.key llm.host llm.model wifi.ssid wifi.psk\n");
}

static bool kvdb_is_secret(const char *key)
{
  size_t len = strlen(key);

  return (len >= 4 && strcmp(key + len - 4, ".key") == 0) ||
         (len >= 4 && strcmp(key + len - 4, ".psk") == 0);
}

/* Show enough to recognise which key is loaded, not enough to use it. */

static void kvdb_print_value(const char *key, const char *value, bool raw)
{
  size_t len = strlen(value);

  if (raw || !kvdb_is_secret(key) || len == 0)
    {
      printf("%s = %s\n", key, value);
    }
  else if (len <= 8)
    {
      printf("%s = **** (%zu bytes)\n", key, len);
    }
  else
    {
      printf("%s = %.4s...%s (%zu bytes)\n", key, value, value + len - 4,
             len);
    }
}

static void kvdb_list_one(const char *key, const char *value, void *arg)
{
  kvdb_print_value(key, value, *(bool *)arg);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  bool raw = false;
  int i;
  int ret;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--raw") == 0)
        {
          raw = true;
        }
    }

  ret = bk7258_kvdb_init();
  if (ret < 0)
    {
      /* Not fatal: the store still answers for this boot, it just will not
       * survive a reset.  Say so rather than pretending it persisted.
       */

      printf("kvdb: not persistent (%d) -- changes last until reset\n", ret);
    }

  if (argc < 2 || strcmp(argv[1], "list") == 0)
    {
      ret = bk7258_kvdb_foreach(kvdb_list_one, &raw);
      if (ret == 0)
        {
          printf("kvdb: empty\n");
        }
      else if (ret < 0)
        {
          printf("kvdb: list failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "get") == 0 && argc >= 3)
    {
      char value[513];

      ret = bk7258_kvdb_get(argv[2], value, sizeof(value));
      if (ret == -ENOENT)
        {
          printf("kvdb: %s is not set\n", argv[2]);
          return EXIT_FAILURE;
        }

      if (ret < 0)
        {
          printf("kvdb: get %s failed: %d\n", argv[2], ret);
          return EXIT_FAILURE;
        }

      kvdb_print_value(argv[2], value, raw);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "set") == 0 && argc >= 4)
    {
      ret = bk7258_kvdb_set(argv[2], argv[3]);
      if (ret < 0)
        {
          printf("kvdb: set %s failed: %d\n", argv[2], ret);
          return EXIT_FAILURE;
        }

      printf("kvdb: %s %s\n", argv[2],
             bk7258_kvdb_persistent() ? "written to flash" :
             "stored for this boot only (flash backend disabled)");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "del") == 0 && argc >= 3)
    {
      ret = bk7258_kvdb_del(argv[2]);
      if (ret < 0)
        {
          printf("kvdb: del %s failed: %d\n", argv[2], ret);
          return EXIT_FAILURE;
        }

      printf("kvdb: %s removed\n", argv[2]);
      return EXIT_SUCCESS;
    }

  kvdb_usage();
  return EXIT_FAILURE;
}
