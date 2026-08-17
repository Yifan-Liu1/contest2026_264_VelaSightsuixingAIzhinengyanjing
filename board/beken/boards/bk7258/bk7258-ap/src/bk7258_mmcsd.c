/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mount.h>

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

#ifdef CONFIG_MBR_PARTITION
static void bk7258_partition_handler(FAR struct partition_s *part,
                                      FAR void *arg)
{
  char path[sizeof("/dev/mmcsd0p0")];

  (void)arg;
  if (part->index >= 10)
    {
      return;
    }

  snprintf(path, sizeof(path), "/dev/mmcsd0p%u", (unsigned)part->index);
  (void)register_blockpartition(path, 0, "/dev/mmcsd0",
                                part->firstblock, part->nblocks);
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
  ret = parse_block_partition("/dev/mmcsd0", bk7258_partition_handler, NULL);
  if (ret < 0)
    {
      printf("SD-NAND has no parseable MBR, using super-floppy path: %d\n",
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
