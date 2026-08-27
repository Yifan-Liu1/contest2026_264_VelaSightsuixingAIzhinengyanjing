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
#include <stdbool.h>
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

/* Both records in this file open with the same prologue -- a 4-byte magic
 * then a 2-byte little-endian version -- which is what lets one reader
 * validate either of them.  A third record added here has to keep it.
 */

#define VS_REC_OFF_MAGIC   0
#define VS_REC_OFF_VERSION 4

#define VS_SETTINGS_MAGIC   "VSST"
#define VS_SETTINGS_VERSION 1

/* magic(4) version(2) volume(1) reserved(1) crc32(4) */

#define VS_SETTINGS_SIZE     12
#define VS_SET_OFF_MAGIC     0
#define VS_SET_OFF_VERSION   4
#define VS_SET_OFF_VOLUME    6
#define VS_SET_OFF_RESERVED  7
#define VS_SET_OFF_CRC       8

/* The generated SoftAP passphrase.  A second file rather than another field
 * in VOL.BIN: the two have different writers and different lifetimes, and
 * sharing a record would force each save to read the other value back first
 * just to avoid erasing it.  A read-modify-write on this card costs more than
 * the whole point of caching the passphrase in the first place.
 *
 * magic(4) version(2) psk_len(1) reserved(1) passphrase(63) crc32(4)
 */

#define VS_APPW_PATH VS_SETTINGS_DIR "/APPW.BIN"
#define VS_APPW_TMP  VS_SETTINGS_DIR "/APPWSAVE.TMP"

#define VS_APPW_MAGIC   "VSAP"
#define VS_APPW_VERSION 1

#define VS_APPW_PSK_MIN      8
#define VS_APPW_PSK_MAX      63
#define VS_APPW_SIZE         75
#define VS_APPW_OFF_MAGIC    0
#define VS_APPW_OFF_VERSION  4
#define VS_APPW_OFF_LEN      6
#define VS_APPW_OFF_RESERVED 7
#define VS_APPW_OFF_PSK      8
#define VS_APPW_OFF_CRC      71

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Bitwise, for the same reason vp_store.c's copy is: a few dozen bytes per
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
 * Name: vs_settings_read_record
 *
 * Description:
 *   Read one whole fixed-size record and check the parts every record here
 *   has in common: exact length, magic, version, zero reserved byte and CRC.
 *   The caller's buffer must be one byte longer than the record so a file
 *   that is too long is rejected rather than silently truncated to something
 *   that checksums.
 *
 *   Returns 0, -ENOENT when nothing is stored, -EBADMSG when a file is there
 *   but is not a record this code wrote, or a negative errno.
 *
 ****************************************************************************/

static int vs_settings_read_record(const char *path, uint8_t *record,
                                   size_t cap, size_t size,
                                   const char *magic, uint16_t version,
                                   size_t reserved_off, size_t crc_off)
{
  FILE *stream;
  size_t nread;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      /* ENOTDIR belongs with ENOENT: NuttX's FAT reports it for a missing
       * directory component, and a device that has never written this record
       * must not look like a broken one.
       */

      if (errno == ENOENT || errno == ENOTDIR)
        {
          return -ENOENT;
        }

      return -errno;
    }

  nread = fread(record, 1, cap, stream);
  fclose(stream);

  if (nread != size ||
      memcmp(record + VS_REC_OFF_MAGIC, magic, 4) != 0 ||
      vs_settings_get16(record + VS_REC_OFF_VERSION) != version ||
      record[reserved_off] != 0 ||
      vs_settings_get32(record + crc_off) !=
      vs_settings_crc32(record, crc_off))
    {
      return -EBADMSG;
    }

  return 0;
}

/****************************************************************************
 * Name: vs_settings_write_record
 *
 * Description:
 *   Write, flush, fsync, close, rename.  Skipping the fsync would let the
 *   rename become durable while the bytes behind it are not, which on a power
 *   cut leaves a record that fails its own CRC for no reason the next boot can
 *   explain -- and these files exist precisely to survive power cuts.
 *
 *   The scratch file is removed on every failure path, including a failed
 *   rename: unlike the provisioning record there is no load-time promotion of
 *   a leftover scratch file here, so leaving one behind would only be litter
 *   that the next save has to overwrite anyway.
 *
 ****************************************************************************/

static int vs_settings_write_record(const char *path, const char *tmp,
                                    const uint8_t *record, size_t len)
{
  FILE *stream;
  int ret;

  if (mkdir(VS_SETTINGS_DIR, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  stream = fopen(tmp, "wb");
  if (stream == NULL)
    {
      return -errno;
    }

  if (fwrite(record, 1, len, stream) != len ||
      fflush(stream) != 0 || fsync(fileno(stream)) != 0)
    {
      ret = errno != 0 ? -errno : -EIO;
      fclose(stream);
      unlink(tmp);
      return ret;
    }

  if (fclose(stream) != 0)
    {
      ret = errno != 0 ? -errno : -EIO;
      unlink(tmp);
      return ret;
    }

  if (rename(tmp, path) < 0)
    {
      ret = -errno;
      unlink(tmp);
      return ret;
    }

  sync();
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_settings_load_volume(uint8_t *level)
{
  uint8_t record[VS_SETTINGS_SIZE + 1];
  int ret;

  if (level == NULL)
    {
      return -EINVAL;
    }

  ret = vs_settings_read_record(VS_SETTINGS_PATH, record, sizeof(record),
                                VS_SETTINGS_SIZE, VS_SETTINGS_MAGIC,
                                VS_SETTINGS_VERSION, VS_SET_OFF_RESERVED,
                                VS_SET_OFF_CRC);
  if (ret < 0)
    {
      return ret;
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

  return vs_settings_write_record(VS_SETTINGS_PATH, VS_SETTINGS_TMP,
                                  record, sizeof(record));
}

int vs_settings_load_ap_password(char *password, size_t size)
{
  uint8_t record[VS_APPW_SIZE + 1];
  size_t len;
  size_t i;
  int ret;

  if (password == NULL || size < VS_APPW_PSK_MAX + 1)
    {
      return -EINVAL;
    }

  ret = vs_settings_read_record(VS_APPW_PATH, record, sizeof(record),
                                VS_APPW_SIZE, VS_APPW_MAGIC,
                                VS_APPW_VERSION, VS_APPW_OFF_RESERVED,
                                VS_APPW_OFF_CRC);
  if (ret < 0)
    {
      return ret;
    }

  /* Everything below is "is this a passphrase wpa_supplicant would take", and
   * a record that fails it is corrupt rather than usable.  Returning it would
   * hand vs_network_apply_ap() a value that makes association fail on every
   * AP entry from now on, with nothing on screen to explain why -- whereas
   * -EBADMSG makes the caller draw a fresh one and overwrite this file.
   */

  len = record[VS_APPW_OFF_LEN];
  if (len < VS_APPW_PSK_MIN || len > VS_APPW_PSK_MAX)
    {
      return -EBADMSG;
    }

  for (i = 0; i < VS_APPW_PSK_MAX; i++)
    {
      uint8_t byte = record[VS_APPW_OFF_PSK + i];

      if (i < len)
        {
          /* Printable ASCII only, which is what WPA2 accepts as a
           * passphrase rather than a raw PSK.
           */

          if (byte < 0x20 || byte > 0x7e)
            {
              return -EBADMSG;
            }
        }
      else if (byte != 0)
        {
          return -EBADMSG;
        }
    }

  memcpy(password, record + VS_APPW_OFF_PSK, len);
  password[len] = '\0';
  return 0;
}

int vs_settings_save_ap_password(const char *password)
{
  uint8_t record[VS_APPW_SIZE];
  size_t len;
  size_t i;

  if (password == NULL)
    {
      return -EINVAL;
    }

  len = strnlen(password, VS_APPW_PSK_MAX + 1);
  if (len < VS_APPW_PSK_MIN || len > VS_APPW_PSK_MAX)
    {
      return -EINVAL;
    }

  for (i = 0; i < len; i++)
    {
      if ((uint8_t)password[i] < 0x20 || (uint8_t)password[i] > 0x7e)
        {
          return -EINVAL;
        }
    }

  memset(record, 0, sizeof(record));
  memcpy(record + VS_APPW_OFF_MAGIC, VS_APPW_MAGIC, 4);
  vs_settings_put16(record + VS_APPW_OFF_VERSION, VS_APPW_VERSION);
  record[VS_APPW_OFF_LEN] = (uint8_t)len;
  memcpy(record + VS_APPW_OFF_PSK, password, len);
  vs_settings_put32(record + VS_APPW_OFF_CRC,
                    vs_settings_crc32(record, VS_APPW_OFF_CRC));

  return vs_settings_write_record(VS_APPW_PATH, VS_APPW_TMP,
                                  record, sizeof(record));
}
