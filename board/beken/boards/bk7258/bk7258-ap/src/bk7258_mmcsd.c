/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include <nuttx/fs/partition.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mmcsd.h>
#include <nuttx/sdio.h>

#include "bk7258_sdio.h"

#define BK7258_SDIO_FALLBACK_SECTORS 2097152u

static ssize_t bk7258_fallback_read(FAR struct inode *inode,
                                    FAR unsigned char *buffer,
                                    blkcnt_t startsector,
                                    unsigned int nsectors)
{
  unsigned int i;
  int ret;

  (void)inode;
  for (i = 0; i < nsectors; i++)
    {
      ret = bk7258_sdio_read_blocks(buffer + i * 512,
                                    (uint32_t)startsector + i, 1);
      if (ret < 0)
        {
          return ret;
        }
    }

  return nsectors;
}

static int bk7258_fallback_geometry(FAR struct inode *inode,
                                    FAR struct geometry *geo)
{
  (void)inode;
  if (geo == NULL)
    {
      return -EINVAL;
    }

  memset(geo, 0, sizeof(*geo));
  geo->geo_available = true;
  geo->geo_mediachanged = false;
  geo->geo_writeenabled = false;
  geo->geo_nsectors = BK7258_SDIO_FALLBACK_SECTORS;
  geo->geo_sectorsize = 512;
  return OK;
}

static int bk7258_fallback_ioctl(FAR struct inode *inode, int cmd,
                                 unsigned long arg)
{
  if (cmd == BIOC_GEOMETRY)
    {
      return bk7258_fallback_geometry(inode,
                                      (FAR struct geometry *)(uintptr_t)arg);
    }

  return -ENOTTY;
}

static const struct block_operations g_bk7258_fallback_ops =
{
  .open = NULL,
  .close = NULL,
  .read = bk7258_fallback_read,
  .write = NULL,
  .geometry = bk7258_fallback_geometry,
  .ioctl = bk7258_fallback_ioctl,
};

static int bk7258_register_fallback(void)
{
  int ret;

  /* A failed MMCSD probe can still leave its block inode registered.  Do
   * not use open() as the availability test: with BCH enabled that inode can
   * be visible while its geometry and state are unusable. */
  (void)unregister_blockdriver("/dev/mmcsd0");
  ret = register_blockdriver("/dev/mmcsd0", &g_bk7258_fallback_ops,
                             0444, NULL);
  return ret;
}

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

int bk7258_mmcsd_initialize(void)
{
  FAR struct sdio_dev_s *dev;
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
      printf("MMCSD registration/probe failed, using fixed geometry: "
             "error=%d\n", ret);
    }

  ret = bk7258_register_fallback();
  if (ret == OK)
    {
      printf("SD-NAND fixed-geometry read-only fallback active\n");
    }
  else
    {
      printf("SD-NAND block device registration failed, error=%d\n", ret);
      return ret;
    }

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
