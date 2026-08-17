/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>

#ifdef CONFIG_FSUTILS_MKFATFS
#  include <fsutils/mkfatfs.h>
#endif

#ifndef CONFIG_BK7258_SDNAND_MOUNTPOINT
#  define CONFIG_BK7258_SDNAND_MOUNTPOINT "/mnt/sdnand"
#endif

#define SDNAND_SECTOR_SIZE 512
#define SDNAND_MARKER      CONFIG_BK7258_SDNAND_MOUNTPOINT \
                           "/VELA.TST"

int bk7258_mmcsd_initialize(void);
int bk7258_mmcsd_mount(FAR const char *source);
int bk7258_mmcsd_unmount(void);
int bk7258_mmcsd_maintenance_begin(void);
void bk7258_mmcsd_maintenance_abort(void);
int bk7258_mmcsd_maintenance_complete(FAR const char *source);
int bk7258_mmcsd_status(FAR bool *mounted, FAR char *source,
                        size_t source_len);

static void sdnand_usage(void)
{
  printf("usage: sdnand_init [status|mount|unmount|"
         "provision --confirm]\n");
}

static int sdnand_initialize(void)
{
  FAR struct inode *inode;
  int ret;

  ret = open_blockdriver("/dev/mmcsd0", 0, &inode);
  if (ret == OK)
    {
      close_blockdriver(inode);
      return OK;
    }

  if (ret != -ENOENT)
    {
      return ret;
    }

  printf("SD-NAND manual initialization begin\n");
  ret = bk7258_mmcsd_initialize();
  if (ret == OK)
    {
      printf("SD-NAND manual initialization complete\n");
    }

  return ret;
}

static void sdnand_fill_pattern(FAR uint8_t *buffer, unsigned int pattern)
{
  uint32_t state = 0x7258a5c3;
  size_t i;

  for (i = 0; i < SDNAND_SECTOR_SIZE; i++)
    {
      switch (pattern)
        {
          case 0:
            buffer[i] = 0x00;
            break;
          case 1:
            buffer[i] = 0xff;
            break;
          case 2:
            buffer[i] = (i & 1) == 0 ? 0x55 : 0xaa;
            break;
          default:
            state = state * 1664525u + 1013904223u;
            buffer[i] = state >> 24;
            break;
        }
    }
}

static int sdnand_raw_gate(void)
{
  uint8_t storage[SDNAND_SECTOR_SIZE * 2 + 1];
  FAR uint8_t *pattern = storage;
  FAR uint8_t *readback = storage + SDNAND_SECTOR_SIZE + 1;
  FAR struct inode *inode;
  struct geometry geo;
  blkcnt_t sector;
  unsigned int pass;
  ssize_t nsectors;
  int ret;

  ret = open_blockdriver("/dev/mmcsd0", 0, &inode);
  if (ret < 0)
    {
      return ret;
    }

  if (inode->u.i_bops->write == NULL)
    {
      close_blockdriver(inode);
      return -EROFS;
    }

  ret = inode->u.i_bops->geometry(inode, &geo);
  if (ret < 0 || geo.geo_sectorsize != SDNAND_SECTOR_SIZE ||
      geo.geo_nsectors < 2 || !geo.geo_writeenabled)
    {
      close_blockdriver(inode);
      return ret < 0 ? ret : -EROFS;
    }

  sector = geo.geo_nsectors - 1;
  printf("SD-NAND destructive raw test: sector=%lu\n",
         (unsigned long)sector);
  for (pass = 0; pass < 4; pass++)
    {
      sdnand_fill_pattern(pattern, pass);
      nsectors = inode->u.i_bops->write(inode, pattern, sector, 1);
      if (nsectors != 1)
        {
          ret = nsectors < 0 ? nsectors : -EIO;
          goto out;
        }

      nsectors = inode->u.i_bops->read(inode, readback, sector, 1);
      if (nsectors != 1 || memcmp(pattern, readback, SDNAND_SECTOR_SIZE) != 0)
        {
          ret = nsectors < 0 ? nsectors : -EIO;
          goto out;
        }

      printf("SD-NAND raw write/read pass: pattern=%u\n", pass);
    }

  ret = OK;

out:
  close_blockdriver(inode);
  return ret;
}

static int sdnand_persistence_test(void)
{
  static const char marker[] = "VelaSight SD-NAND persistence v1\n";
  char readback[sizeof(marker)];
  FILE *stream;
  size_t nread;

  if (mkdir(CONFIG_BK7258_SDNAND_MOUNTPOINT "/ai_agent", 0777) < 0 &&
      errno != EEXIST)
    {
      return -errno;
    }

  if (mkdir(CONFIG_BK7258_SDNAND_MOUNTPOINT "/captures", 0777) < 0 &&
      errno != EEXIST)
    {
      return -errno;
    }

  stream = fopen(SDNAND_MARKER, "w+");
  if (stream == NULL)
    {
      return -errno;
    }

  if (fwrite(marker, 1, sizeof(marker) - 1, stream) != sizeof(marker) - 1 ||
      fflush(stream) != 0 || fsync(fileno(stream)) != 0 ||
      fseek(stream, 0, SEEK_SET) != 0)
    {
      int error = errno == 0 ? EIO : errno;
      fclose(stream);
      return -error;
    }

  memset(readback, 0, sizeof(readback));
  nread = fread(readback, 1, sizeof(marker) - 1, stream);
  fclose(stream);
  if (nread != sizeof(marker) - 1 || memcmp(readback, marker, nread) != 0)
    {
      return -EIO;
    }

  sync();
  printf("SD-NAND persistent file verified: %s\n", SDNAND_MARKER);
  return OK;
}

static int sdnand_provision(void)
{
#ifdef CONFIG_FSUTILS_MKFATFS
  struct fat_format_s fmt = FAT_FORMAT_INITIALIZER;
  int ret;

  ret = bk7258_mmcsd_maintenance_begin();
  if (ret < 0)
    {
      printf("SD-NAND maintenance begin failed: %d\n", ret);
      return ret;
    }

  ret = sdnand_raw_gate();
  if (ret < 0)
    {
      printf("SD-NAND raw write gate failed: %d\n", ret);
      bk7258_mmcsd_maintenance_abort();
      return ret;
    }

  fmt.ff_fattype = 32;
  fmt.ff_clustshift = 0;
  memcpy(fmt.ff_volumelabel, "VELASIGHT  ", 11);
  printf("SD-NAND formatting /dev/mmcsd0 as FAT32\n");
  if (mkfatfs("/dev/mmcsd0", &fmt) < 0)
    {
      ret = -errno;
      printf("SD-NAND FAT32 format failed: %d\n", ret);
      bk7258_mmcsd_maintenance_abort();
      return ret;
    }

  ret = bk7258_mmcsd_maintenance_complete("/dev/mmcsd0");
  if (ret < 0)
    {
      return ret;
    }

  return sdnand_persistence_test();
#else
  printf("SD-NAND provisioning requires CONFIG_FSUTILS_MKFATFS\n");
  return -ENOTSUP;
#endif
}

int main(int argc, FAR char *argv[])
{
  FAR const char *command = argc > 1 ? argv[1] : "mount";
  bool confirmed = argc == 3 && strcmp(argv[2], "--confirm") == 0;
  int ret;

  ret = sdnand_initialize();
  if (ret < 0 && strcmp(command, "status") != 0)
    {
      printf("SD-NAND initialization failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  if (strcmp(command, "status") == 0)
    {
      char source[sizeof("/dev/mmcsd0p0")];
      bool mounted = false;

      source[0] = '\0';
      (void)bk7258_mmcsd_status(&mounted, source, sizeof(source));
      printf("SD-NAND initialized=%s mounted=%s source=%s target=%s\n",
             ret == OK ? "yes" : "no",
             mounted ? "yes" : "no",
             source[0] == '\0' ? "<none>" : source,
             CONFIG_BK7258_SDNAND_MOUNTPOINT);
      return ret == OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  else if (strcmp(command, "mount") == 0)
    {
      ret = bk7258_mmcsd_mount(NULL);
    }
  else if (strcmp(command, "unmount") == 0)
    {
      sync();
      ret = bk7258_mmcsd_unmount();
    }
  else if (strcmp(command, "provision") == 0 && confirmed)
    {
      ret = sdnand_provision();
    }
  else
    {
      sdnand_usage();
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      printf("SD-NAND %s failed: %d\n", command, ret);
      return EXIT_FAILURE;
    }

  printf("SD-NAND %s complete\n", command);
  return EXIT_SUCCESS;
}
