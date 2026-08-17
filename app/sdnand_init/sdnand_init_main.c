/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <sys/mount.h>

#include <nuttx/fs/fs.h>

int bk7258_mmcsd_initialize(void);

int main(int argc, FAR char *argv[])
{
  FAR struct inode *inode;
  int ret;

  (void)argc;
  (void)argv;

  ret = open_blockdriver("/dev/mmcsd0", MS_RDONLY, &inode);
  if (ret == OK)
    {
      close_blockdriver(inode);
      printf("SD-NAND already initialized: /dev/mmcsd0\n");
      return OK;
    }

  if (ret != -ENOENT)
    {
      printf("SD-NAND preflight failed, error=%d\n", ret);
      return ret;
    }

  printf("SD-NAND manual initialization begin\n");
  ret = bk7258_mmcsd_initialize();
  if (ret < 0)
    {
      printf("SD-NAND manual initialization failed, error=%d\n", ret);
      return ret;
    }

  printf("SD-NAND manual initialization complete\n");
  return OK;
}
