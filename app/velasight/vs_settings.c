/****************************************************************************
 * app/velasight/vs_settings.c
 *
 * See include/vs_settings.h for why this is a file of its own rather than a
 * field in the provisioning record or a key in the agent config store.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "include/vs_settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Kept in the provisioning directory because that tree already exists by the
 * time anything writes here, and 8.3 names because the rest of the project
 * uses them regardless of CONFIG_FAT_LFN.
 */

#define VS_SETTINGS_DIR  "/mnt/sdnand/prov"
#define VS_SETTINGS_PATH VS_SETTINGS_DIR "/VOL.BIN"
#define VS_SETTINGS_TMP  VS_SETTINGS_DIR "/VOLSAVE.TMP"

#define VS_SETTINGS_MAGIC   "VSST"
#define VS_SETTINGS_VERSION 1

/* magic(4) version(2) volume(1) reserved(1) crc32(4) */

#define VS_SETTINGS_SIZE     12
#define VS_SET_OFF_MAGIC     0
#define VS_SET_OFF_VERSION   4
#define VS_SET_OFF_VOLUME    6
#define VS_SET_OFF_RESERVED  7
#define VS_SET_OFF_CRC       8

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Bitwise, for the same reason vp_store.c's copy is: twelve bytes twice per
 * user action does not justify a 1 KiB table on a part this full.  Kept
 * separate from vp_crc32() rather than exported, because these two files have
 * no other reason to depend on each other.
 */

static uint32_t vs_settings_crc32(const void *data, size_t len)
{
  const unsigned char *p = data;
  uint32_t crc = 0xffffffffu;
  size_t i;

  for (i = 0; i < len; i++)
    {
      unsigned bit;

      crc ^= p[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }

  return crc ^ 0xffffffffu;
}

static void vs_settings_put16(uint8_t *buf, uint16_t value)
{
  buf[0] = (uint8_t)(value & 0xffu);
  buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t vs_settings_get16(const uint8_t *buf)
{
  return (uint16_t)((uint16_t)buf[0] | (uint16_t)((uint16_t)buf[1] << 8));
}

static void vs_settings_put32(uint8_t *buf, uint32_t value)
{
  buf[0] = (uint8_t)(value & 0xffu);
  buf[1] = (uint8_t)((value >> 8) & 0xffu);
  buf[2] = (uint8_t)((value >> 16) & 0xffu);
  buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t vs_settings_get32(const uint8_t *buf)
{
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_settings_load_volume(uint8_t *level)
{
  /* One byte longer than the record, so a file that is too long is rejected
   * rather than silently truncated to something that checksums.
   */

  uint8_t record[VS_SETTINGS_SIZE + 1];
  FILE *stream;
  size_t nread;

  if (level == NULL)
    {
      return -EINVAL;
    }

  stream = fopen(VS_SETTINGS_PATH, "rb");
  if (stream == NULL)
    {
      /* ENOTDIR belongs with ENOENT: NuttX's FAT reports it for a missing
       * directory component, and a device that has never had its volume
       * changed must not look like a broken one.
       */

      if (errno == ENOENT || errno == ENOTDIR)
        {
          return -ENOENT;
        }

      return -errno;
    }

  nread = fread(record, 1, sizeof(record), stream);
  fclose(stream);

  if (nread != VS_SETTINGS_SIZE ||
      memcmp(record + VS_SET_OFF_MAGIC, VS_SETTINGS_MAGIC, 4) != 0 ||
      vs_settings_get16(record + VS_SET_OFF_VERSION) !=
      VS_SETTINGS_VERSION ||
      record[VS_SET_OFF_RESERVED] != 0 ||
      vs_settings_get32(record + VS_SET_OFF_CRC) !=
      vs_settings_crc32(record, VS_SET_OFF_CRC))
    {
      return -EBADMSG;
    }

  /* Range is part of the format.  A value outside it means the record is not
   * what this code wrote, whatever the CRC says about how it got there.
   */

  if (record[VS_SET_OFF_VOLUME] > 100)
    {
      return -EBADMSG;
    }

  *level = record[VS_SET_OFF_VOLUME];
  return 0;
}

int vs_settings_save_volume(uint8_t level)
{
  uint8_t record[VS_SETTINGS_SIZE];
  FILE *stream;
  int ret;

  if (level > 100)
    {
      return -EINVAL;
    }

  memset(record, 0, sizeof(record));
  memcpy(record + VS_SET_OFF_MAGIC, VS_SETTINGS_MAGIC, 4);
  vs_settings_put16(record + VS_SET_OFF_VERSION, VS_SETTINGS_VERSION);
  record[VS_SET_OFF_VOLUME] = level;
  vs_settings_put32(record + VS_SET_OFF_CRC,
                    vs_settings_crc32(record, VS_SET_OFF_CRC));

  if (mkdir(VS_SETTINGS_DIR, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  stream = fopen(VS_SETTINGS_TMP, "wb");
  if (stream == NULL)
    {
      return -errno;
    }

  /* Write, flush, fsync, close, rename.  Skipping the fsync would let the
   * rename become durable while the bytes behind it are not, which on a power
   * cut leaves a record that fails its own CRC for no reason the next boot can
   * explain -- and this file exists precisely to survive power cuts.
   */

  if (fwrite(record, 1, sizeof(record), stream) != sizeof(record) ||
      fflush(stream) != 0 || fsync(fileno(stream)) != 0)
    {
      ret = errno != 0 ? -errno : -EIO;
      fclose(stream);
      unlink(VS_SETTINGS_TMP);
      return ret;
    }

  if (fclose(stream) != 0)
    {
      ret = errno != 0 ? -errno : -EIO;
      unlink(VS_SETTINGS_TMP);
      return ret;
    }

  if (rename(VS_SETTINGS_TMP, VS_SETTINGS_PATH) < 0)
    {
      ret = -errno;
      unlink(VS_SETTINGS_TMP);
      return ret;
    }

  sync();
  return 0;
}
