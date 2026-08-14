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
#include <nuttx/sdio.h>

#include "bk7258_sdio.h"

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
