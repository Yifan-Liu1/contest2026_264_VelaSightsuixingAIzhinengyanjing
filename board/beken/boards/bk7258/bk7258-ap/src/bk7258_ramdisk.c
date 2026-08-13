/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_ramdisk.c
 *
 * PSRAM-backed RAM disk so that captured frames can be written to a real
 * file ("file mode" of apps/system/nxcamera's `output <path> [nframes]`).
 *
 * Why this exists: the only writable filesystem this board had was tmpfs,
 * whose storage comes from the kernel heap.  CONFIG_RAM_SIZE is 344064
 * bytes and at the time the SRAM heap held roughly 300KB of that, while one
 * 640x480 YUYV frame is 614400 bytes -- so `output /tmp/frame.yuv 1` could
 * never succeed, the write() always failed with ENOMEM.  This board does,
 * however, have 16MB of PSRAM, so a RAM disk carved out of PSRAM gives the
 * filesystem enough room for several full frames without touching the SRAM
 * heap at all.
 *
 * Since "fix(psram): give the system heap the unused tail of PSRAM_SECTION"
 * the system heap itself reaches into PSRAM (6.4MB arena), so tmpfs is no
 * longer categorically too small.  This ramdisk is still the better place for
 * frame-sized files: it does not compete with task stacks and allocations for
 * the same free blocks, and its 2MB is bounded and predictable.
 *
 * Usage after boot (the ramdisk is registered but intentionally left
 * unformatted, since its contents are volatile anyway):
 *
 *   mkfatfs /dev/ram0
 *   mount -t vfat /dev/ram0 /mnt
 *   nxcamera
 *     input /dev/video0
 *     output /mnt/frame.yuv 1     <- exactly one frame, then stream stops
 *     stream 640 480 30 YUYV
 *     q
 *   ls -l /mnt
 *   hexdump /mnt/frame.yuv count=64
 *
 * The .yuv file is raw YUYV (YUV422), so on a host it converts with e.g.
 *   ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x480 -i frame.yuv frame.png
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/drivers/ramdisk.h>
#include <nuttx/fs/fs.h>

#include "bk7258_psram.h"
#include "bk7258_ramdisk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_RAMDISK_MINOR      0
#define BK7258_RAMDISK_SECTOR     512u

/* Where it gets mounted.  Keep this in step with
 * CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR in configs/ai_agent/defconfig, whose
 * value is "/mnt/ai_agent": the agent creates the ai_agent/ subtree itself,
 * but only if the mount point below is a real filesystem.
 */

#define BK7258_RAMDISK_MOUNTPT    "/mnt"

/* 2MB: enough for three 640x480 YUYV frames plus FAT overhead, out of the
 * 2.9MB AP PSRAM heap (BK7258_AP_PSRAM_HEAP_*, bk7258_psram.c).  The V4L2
 * capture buffers do not come from this heap -- they are allocated from the
 * separate PSRAM DISPLAY pool by bk7258_camera_imgdata.c -- so the two do
 * not compete.
 */

#define BK7258_RAMDISK_BYTES      (2u * 1024u * 1024u)
#define BK7258_RAMDISK_SECTORS    (BK7258_RAMDISK_BYTES / BK7258_RAMDISK_SECTOR)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_ramdisk_initialize(void)
{
#ifdef CONFIG_BK7258_PSRAM
  FAR uint8_t *buffer;
  int ret;

  if (!bk7258_psram_is_online())
    {
      printf("ramdisk: PSRAM offline, /dev/ram0 not registered\n");
      return -ENODEV;
    }

  buffer = bk7258_psram_memalign(BK7258_RAMDISK_SECTOR,
                                 BK7258_RAMDISK_BYTES);
  if (buffer == NULL)
    {
      printf("ramdisk: PSRAM allocation of %u bytes failed\n",
             (unsigned int)BK7258_RAMDISK_BYTES);
      return -ENOMEM;
    }

  ret = ramdisk_register(BK7258_RAMDISK_MINOR, buffer,
                         BK7258_RAMDISK_SECTORS, BK7258_RAMDISK_SECTOR,
                         RDFLAG_WRENABLED);
  if (ret < 0)
    {
      printf("ramdisk: ramdisk_register failed, error=%d\n", ret);
      bk7258_psram_free(buffer);
      return ret;
    }

  printf("ramdisk: /dev/ram0 registered, %u KB at %p (PSRAM)\n",
         (unsigned int)(BK7258_RAMDISK_BYTES / 1024u), buffer);

  /* Mount it, rather than leaving the mount to whoever remembers to type it.
   *
   * Applications that keep state ask for a path, not for a block device:
   * ai_agent's config store, memory store and skill loader all write under
   * CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR ("/mnt/ai_agent").  They do build
   * their directory tree themselves -- config_store.c's mkdirs() and
   * memory_store.c's ensure_dir() both walk the path -- but no amount of
   * mkdir() helps while /mnt is still pseudo-filesystem, so without this the
   * agent starts and then loses every skill and its whole config:
   *
   *   [skills] Cannot write skill: /mnt/ai_agent/skills/weather.md
   *
   * littlefs with -o autoformat is what makes this a one-liner: it formats
   * the device when the mount finds no valid superblock, which is exactly
   * right for a RAM disk whose contents are gone after every reset anyway.
   * FAT would need mkfatfs(), which lives in apps/fsutils and is not
   * reachable from board bring-up.
   *
   * This is not persistence.  The backing store is PSRAM, so the tree is
   * rebuilt from scratch on each boot; what it buys is that the agent's
   * paths are writable at all.  Real persistence needs a flash partition,
   * and when it arrives only the device name below has to change.
   */

#ifdef CONFIG_FS_LITTLEFS
  ret = nx_mount("/dev/ram0", BK7258_RAMDISK_MOUNTPT, "littlefs", 0,
                 "autoformat");
  if (ret < 0)
    {
      printf("ramdisk: mount %s failed, error=%d; format with "
             "\"mkfatfs /dev/ram0\" then \"mount -t vfat /dev/ram0 %s\"\n",
             BK7258_RAMDISK_MOUNTPT, ret, BK7258_RAMDISK_MOUNTPT);
      return OK;
    }

  printf("ramdisk: %s mounted (littlefs on /dev/ram0)\n",
         BK7258_RAMDISK_MOUNTPT);
#else
  printf("ramdisk: CONFIG_FS_LITTLEFS not set; format with "
         "\"mkfatfs /dev/ram0\" then \"mount -t vfat /dev/ram0 %s\"\n",
         BK7258_RAMDISK_MOUNTPT);
#endif

  return OK;
#else
  return -ENOTSUP;
#endif
}
