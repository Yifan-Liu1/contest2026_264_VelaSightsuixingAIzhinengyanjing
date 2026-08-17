/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <nuttx/fs/partition.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/signal.h>
#include <nuttx/wqueue.h>

#include "bk7258_sdio.h"

#define BK7258_MMCSD_PROBE_WAIT_MS 7000
#define BK7258_MMCSD_PROBE_POLL_US 250000

#ifndef CONFIG_BK7258_SDNAND_MOUNTPOINT
#  define CONFIG_BK7258_SDNAND_MOUNTPOINT "/mnt/sdnand"
#endif

enum bk7258_mmcsd_state_e
{
  BK7258_MMCSD_NOT_STARTED = 0,
  BK7258_MMCSD_RUNNING,
  BK7258_MMCSD_COMPLETE
};

static mutex_t g_mmcsd_lock = NXMUTEX_INITIALIZER;
static struct work_s g_mmcsd_work;
static enum bk7258_mmcsd_state_e g_mmcsd_state;
static int g_mmcsd_result = -EAGAIN;
static int g_partition_result = -ENOENT;
static bool g_mmcsd_mounted;
static bool g_mmcsd_maintenance;
static char g_mount_source[sizeof("/dev/mmcsd0p0")];

static bool bk7258_block_exists(FAR const char *path)
{
  FAR struct inode *inode;
  int ret;

  ret = open_blockdriver(path, 0, &inode);
  if (ret < 0)
    {
      return false;
    }

  close_blockdriver(inode);
  return true;
}

static int bk7258_mmcsd_mount_locked(FAR const char *source)
{
#ifdef CONFIG_FS_FAT
  FAR const char *mount_source = source;
  int ret;

  if (g_mmcsd_maintenance)
    {
      return -EBUSY;
    }

  if (g_mmcsd_mounted)
    {
      if (source == NULL || strcmp(source, g_mount_source) == 0)
        {
          return OK;
        }

      return -EBUSY;
    }

  if (mount_source == NULL)
    {
      mount_source = g_partition_result == OK &&
                     bk7258_block_exists("/dev/mmcsd0p0") ?
                     "/dev/mmcsd0p0" : "/dev/mmcsd0";
    }

  if (!bk7258_block_exists(mount_source))
    {
      return -ENOENT;
    }

  if (mkdir("/mnt", 0777) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  if (mkdir(CONFIG_BK7258_SDNAND_MOUNTPOINT, 0777) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  ret = nx_mount(mount_source, CONFIG_BK7258_SDNAND_MOUNTPOINT,
                 "vfat", 0, NULL);
  if (ret < 0)
    {
      printf("SD-NAND VFAT mount failed: source=%s target=%s error=%d\n",
             mount_source, CONFIG_BK7258_SDNAND_MOUNTPOINT, ret);
      return ret;
    }

  strlcpy(g_mount_source, mount_source, sizeof(g_mount_source));
  g_mmcsd_mounted = true;
  printf("SD-NAND persistent VFAT mounted: %s -> %s\n",
         g_mount_source, CONFIG_BK7258_SDNAND_MOUNTPOINT);
  return OK;
#else
  (void)source;
  return -ENOTSUP;
#endif
}

#ifdef CONFIG_MBR_PARTITION
static void bk7258_partition_handler(FAR struct partition_s *part,
                                       FAR void *arg)
{
  char path[sizeof("/dev/mmcsd0p0")];
  int ret;

  (void)arg;
  if (part->index >= 10)
    {
      return;
    }

  snprintf(path, sizeof(path), "/dev/mmcsd0p%u", (unsigned)part->index);
  ret = register_blockpartition(path, 0, "/dev/mmcsd0",
                                part->firstblock, part->nblocks);
  if (ret == -EEXIST && bk7258_block_exists(path))
    {
      ret = OK;
    }

  if (part->index == 0)
    {
      g_partition_result = ret;
    }

  if (ret < 0)
    {
      printf("SD-NAND partition registration failed: %s error=%d\n",
             path, ret);
    }
}
#endif

static int bk7258_mmcsd_probe(void)
{
  FAR struct sdio_dev_s *dev;
  FAR struct inode *inode;
  int ret;

  dev = bk7258_sdio_initialize(0);
  if (dev == NULL)
    {
      printf("SDIO initialize failed\n");
      return -ENODEV;
    }

  ret = mmcsd_slotinitialize(0, dev);
  if (ret < 0)
    {
      printf("MMCSD registration/probe failed, error=%d\n", ret);
      return ret;
    }

  ret = open_blockdriver("/dev/mmcsd0", MS_RDONLY, &inode);
  if (ret == -ENOENT)
    {
      unsigned int elapsed;

      for (elapsed = 0; elapsed < BK7258_MMCSD_PROBE_WAIT_MS;
           elapsed += BK7258_MMCSD_PROBE_POLL_US / 1000)
        {
          nxsig_usleep(BK7258_MMCSD_PROBE_POLL_US);
          ret = open_blockdriver("/dev/mmcsd0", MS_RDONLY, &inode);
          if (ret != -ENOENT)
            {
              break;
            }
        }
    }

  if (ret < 0)
    {
      printf("MMCSD probe did not register /dev/mmcsd0, error=%d\n", ret);
      return ret;
    }

  close_blockdriver(inode);

  printf("SD-NAND MMCSD ready: /dev/mmcsd0\n");

#ifdef CONFIG_MBR_PARTITION
  g_partition_result = -ENOENT;
  ret = parse_block_partition("/dev/mmcsd0", bk7258_partition_handler, NULL);
  if (ret < 0)
    {
      printf("SD-NAND has no parseable MBR, using super-floppy path: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_BK7258_SDNAND_AUTOMOUNT
  ret = bk7258_mmcsd_mount_locked(NULL);
  if (ret < 0)
    {
      printf("SD-NAND initialized without persistent mount, error=%d\n",
             ret);
    }
#endif

  return OK;
}

int bk7258_mmcsd_initialize(void)
{
  FAR struct inode *inode;
  int ret;

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_mmcsd_state == BK7258_MMCSD_COMPLETE)
    {
      ret = open_blockdriver("/dev/mmcsd0", MS_RDONLY, &inode);
      if (ret == OK)
        {
          close_blockdriver(inode);
#ifdef CONFIG_BK7258_SDNAND_AUTOMOUNT
          if (!g_mmcsd_mounted)
            {
              ret = bk7258_mmcsd_mount_locked(NULL);
            }
#endif
        }
      else
        {
          ret = g_mmcsd_result;
        }

      nxmutex_unlock(&g_mmcsd_lock);
      return ret;
    }

  g_mmcsd_state = BK7258_MMCSD_RUNNING;
  ret = bk7258_mmcsd_probe();
  g_mmcsd_result = ret;
  g_mmcsd_state = BK7258_MMCSD_COMPLETE;
  nxmutex_unlock(&g_mmcsd_lock);
  return ret;
}

int bk7258_mmcsd_mount(FAR const char *source)
{
  int ret;

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_mmcsd_state != BK7258_MMCSD_COMPLETE ||
      !bk7258_block_exists("/dev/mmcsd0"))
    {
      nxmutex_unlock(&g_mmcsd_lock);
      return -ENODEV;
    }

  ret = bk7258_mmcsd_mount_locked(source);
  if (ret == OK)
    {
      g_mmcsd_result = OK;
    }

  nxmutex_unlock(&g_mmcsd_lock);
  return ret;
}

int bk7258_mmcsd_maintenance_begin(void)
{
  char path[sizeof("/dev/mmcsd0p0")];
  unsigned int index;
  int ret;

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_mmcsd_state != BK7258_MMCSD_COMPLETE ||
      !bk7258_block_exists("/dev/mmcsd0"))
    {
      ret = -ENODEV;
      goto out_unlock;
    }

  if (g_mmcsd_maintenance)
    {
      ret = -EBUSY;
      goto out_unlock;
    }

  g_mmcsd_maintenance = true;
  if (g_mmcsd_mounted)
    {
      ret = nx_umount2(CONFIG_BK7258_SDNAND_MOUNTPOINT, 0);
      if (ret < 0)
        {
          g_mmcsd_maintenance = false;
          goto out_unlock;
        }

      g_mmcsd_mounted = false;
      g_mount_source[0] = '\0';
    }

  for (index = 0; index < 10; index++)
    {
      snprintf(path, sizeof(path), "/dev/mmcsd0p%u", index);
      if (bk7258_block_exists(path))
        {
          ret = unregister_blockdriver(path);
          if (ret < 0)
            {
              printf("SD-NAND partition quarantine failed: %s error=%d\n",
                     path, ret);
              g_mmcsd_maintenance = false;
              goto out_unlock;
            }
        }
    }

  g_partition_result = -ENOENT;

  ret = OK;

out_unlock:
  nxmutex_unlock(&g_mmcsd_lock);
  return ret;
}

void bk7258_mmcsd_maintenance_abort(void)
{
  if (nxmutex_lock(&g_mmcsd_lock) == OK)
    {
      g_mmcsd_maintenance = false;
      nxmutex_unlock(&g_mmcsd_lock);
    }
}

int bk7258_mmcsd_maintenance_complete(FAR const char *source)
{
  int ret;

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_mmcsd_maintenance)
    {
      ret = -EINVAL;
      goto out_unlock;
    }

  g_mmcsd_maintenance = false;
  ret = bk7258_mmcsd_mount_locked(source);

out_unlock:
  nxmutex_unlock(&g_mmcsd_lock);
  return ret;
}

int bk7258_mmcsd_unmount(void)
{
  int ret;

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_mmcsd_mounted)
    {
      nxmutex_unlock(&g_mmcsd_lock);
      return OK;
    }

  ret = nx_umount2(CONFIG_BK7258_SDNAND_MOUNTPOINT, 0);
  if (ret == OK)
    {
      g_mmcsd_mounted = false;
      g_mount_source[0] = '\0';
    }

  nxmutex_unlock(&g_mmcsd_lock);
  return ret;
}

int bk7258_mmcsd_status(FAR bool *mounted, FAR char *source,
                        size_t source_len)
{
  int ret;

  if (mounted == NULL || source == NULL || source_len == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  *mounted = g_mmcsd_mounted;
  strlcpy(source, g_mmcsd_mounted ? g_mount_source : "", source_len);
  nxmutex_unlock(&g_mmcsd_lock);
  return OK;
}

static void bk7258_mmcsd_worker(FAR void *arg)
{
  int ret;

  (void)arg;
  printf("SD-NAND delayed initialization begin\n");
  ret = bk7258_mmcsd_initialize();
  if (ret < 0)
    {
      printf("SD-NAND delayed initialization failed, error=%d\n", ret);
    }
  else
    {
      printf("SD-NAND delayed initialization complete\n");
    }
}

int bk7258_mmcsd_schedule(unsigned int delay_ms)
{
  int ret;

  ret = nxmutex_lock(&g_mmcsd_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_mmcsd_state != BK7258_MMCSD_NOT_STARTED ||
      !work_available(&g_mmcsd_work))
    {
      nxmutex_unlock(&g_mmcsd_lock);
      return OK;
    }

  ret = work_queue(LPWORK, &g_mmcsd_work, bk7258_mmcsd_worker, NULL,
                   MSEC2TICK(delay_ms));
  nxmutex_unlock(&g_mmcsd_lock);

  if (ret == OK)
    {
      printf("SD-NAND initialization scheduled in %u ms\n", delay_ms);
    }

  return ret;
}
