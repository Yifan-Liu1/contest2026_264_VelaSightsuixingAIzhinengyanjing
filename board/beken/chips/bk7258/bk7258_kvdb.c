/****************************************************************************
 * board/beken/chips/bk7258/bk7258_kvdb.c
 *
 * A small key-value store in the AP's own flash partition, so configuration
 * is typed once instead of compiled in or re-entered after every reset.
 *
 * What it replaces
 * ----------------
 * Two things were being hard-coded, both of which this removes the need for:
 *
 *   - the MiMo API key and endpoint, through
 *     packages/ai_agent/include/agent_secrets.h -- a header copied into a
 *     *public* repository and compiled into the image, so the key ended up in
 *     the .bin and the firmware could not be shared.
 *   - Wi-Fi credentials, re-typed with `wapi psk`/`wapi essid` after each
 *     reset because /mnt is a PSRAM ramdisk and loses everything.
 *
 * Layout
 * ------
 * One 4KB sector holds the whole store, rewritten as a unit:
 *
 *   magic "VKVDB\0\0\0" | version | count | crc32 of the record area
 *   records: key length, value length, key bytes, value bytes, repeated
 *
 * Rewriting everything on each set is the right trade here.  The store is
 * well under 1KB, a set happens when a human types one, and the alternative
 * -- appending records and compacting later -- is more code and more failure
 * modes for no gain at this size.  The second sector of the partition is
 * deliberately left alone: it is where a future A/B scheme would go, and
 * leaving it erased means a half-written sector can never look valid.
 *
 * A store whose CRC does not match is treated as empty rather than repaired.
 * Configuration is cheap to retype; guessing at damaged bytes is not.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/mutex.h>

#include "bk7258_flash_client.h"
#include "bk7258_kvdb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KVDB_MAGIC        "VKVDB\0\0"
#define KVDB_MAGIC_LEN    8u
#define KVDB_VERSION      1u
#define KVDB_SECTOR       BK7258_FLASH_AP_ENV_BASE
#define KVDB_CAPACITY     BK7258_FLASH_SECTOR_SIZE

/****************************************************************************
 * Private Types
 ****************************************************************************/

begin_packed_struct struct kvdb_header_s
{
  char     magic[KVDB_MAGIC_LEN];
  uint32_t version;
  uint32_t count;
  uint32_t bytes;      /* Record area length */
  uint32_t crc;        /* CRC-32 over the record area */
} end_packed_struct;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_kvdb_lock = NXMUTEX_INITIALIZER;

/* The whole store, mirrored in RAM.  4KB out of the system heap, which
 * reaches PSRAM on this board; keeping it resident is what makes get() free
 * and avoids a flash round trip on the boot path.
 */

static FAR uint8_t *g_kvdb_image;
static bool g_kvdb_loaded;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t kvdb_crc32(FAR const uint8_t *data, size_t len)
{
  uint32_t crc = 0xffffffffu;
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
        }
    }

  return ~crc;
}

static FAR struct kvdb_header_s *kvdb_hdr(void)
{
  return (FAR struct kvdb_header_s *)g_kvdb_image;
}

static FAR uint8_t *kvdb_records(void)
{
  return g_kvdb_image + sizeof(struct kvdb_header_s);
}

static void kvdb_reset_image(void)
{
  FAR struct kvdb_header_s *hdr = kvdb_hdr();

  memset(g_kvdb_image, 0, KVDB_CAPACITY);
  memcpy(hdr->magic, KVDB_MAGIC, KVDB_MAGIC_LEN);
  hdr->version = KVDB_VERSION;
  hdr->count = 0;
  hdr->bytes = 0;
  hdr->crc = kvdb_crc32(kvdb_records(), 0);
}

/* Walk the records, returning the offset of key or -1.  Record format is
 * two bytes of key length, two of value length, then the bytes; both are
 * stored without terminators so a value may contain anything.
 */

static ssize_t kvdb_find(FAR const char *key, FAR uint16_t *vlen)
{
  FAR uint8_t *rec = kvdb_records();
  uint32_t used = kvdb_hdr()->bytes;
  size_t klen = strlen(key);
  size_t off = 0;

  while (off + 4u <= used)
    {
      uint16_t rk = (uint16_t)(rec[off] | (rec[off + 1] << 8));
      uint16_t rv = (uint16_t)(rec[off + 2] | (rec[off + 3] << 8));

      if (off + 4u + rk + rv > used)
        {
          break;
        }

      if (rk == klen && memcmp(&rec[off + 4], key, klen) == 0)
        {
          if (vlen != NULL)
            {
              *vlen = rv;
            }

          return (ssize_t)off;
        }

      off += 4u + rk + rv;
    }

  return -1;
}

static bool g_kvdb_persistent;

static int kvdb_flush(void)
{
  FAR struct kvdb_header_s *hdr = kvdb_hdr();
  int ret;

  hdr->crc = kvdb_crc32(kvdb_records(), hdr->bytes);

  /* Nothing to write to when the store is memory-only.  Reporting an error
   * here would be wrong: the value *was* stored, it just will not outlive the
   * boot, and bk7258_kvdb_init() already said so once.
   */

  if (!g_kvdb_persistent)
    {
      return OK;
    }

  ret = bk7258_flash_erase_sector(KVDB_SECTOR);
  if (ret < 0)
    {
      return ret;
    }

  /* Only the part in use is written back.  The rest of the sector stays
   * erased, which costs nothing and makes a truncated write obvious.
   */

  return bk7258_flash_write(KVDB_SECTOR, g_kvdb_image,
                            sizeof(struct kvdb_header_s) + hdr->bytes);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_kvdb_init(void)
{
  nxmutex_lock(&g_kvdb_lock);

  if (g_kvdb_loaded)
    {
      nxmutex_unlock(&g_kvdb_lock);
      return OK;
    }

  if (g_kvdb_image == NULL)
    {
      g_kvdb_image = malloc(KVDB_CAPACITY);
      if (g_kvdb_image == NULL)
        {
          nxmutex_unlock(&g_kvdb_lock);
          return -ENOMEM;
        }
    }

  /* Start empty and usable.  No flash access here on purpose: this runs from
   * board bring-up, and talking to the CP's flash server from there wedged
   * the AP -- the request never completed, not even into its own timeout, so
   * the mailbox stopped being serviced, the heartbeat lapsed and the CP's
   * 8-second watchdog reset the chip.  Measured as a reboot loop printing
   * "connected to the CP flash server" every ~9 s, ending in
   * "ap_bridg: link down" every time.
   *
   * The flash side is done later from a task (bk7258_kvdb_load()), where a
   * blocking cross-core request is just a blocked task.
   */

  kvdb_reset_image();
  g_kvdb_loaded = true;
  g_kvdb_persistent = false;
  nxmutex_unlock(&g_kvdb_lock);
  return OK;
}

int bk7258_kvdb_load(void)
{
#ifndef CONFIG_BK7258_KVDB_FLASH
  printf("kvdb: in-memory only (flash backend disabled)\n");
  return -ENOTSUP;
#else
  FAR struct kvdb_header_s *hdr;
  int ret;

  ret = bk7258_flash_client_init();
  if (ret < 0)
    {
      printf("kvdb: in-memory only (no flash service: %d)\n", ret);
      return ret;
    }

  nxmutex_lock(&g_kvdb_lock);

  ret = bk7258_flash_read(KVDB_SECTOR, g_kvdb_image, KVDB_CAPACITY);
  if (ret < 0)
    {
      /* Keep whatever is in memory rather than clearing it: a value set
       * before the load finished is more useful than an empty store.
       */

      nxmutex_unlock(&g_kvdb_lock);
      printf("kvdb: flash read failed (%d), staying in memory only\n", ret);
      return ret;
    }

  hdr = kvdb_hdr();

  if (memcmp(hdr->magic, KVDB_MAGIC, KVDB_MAGIC_LEN) != 0 ||
      hdr->version != KVDB_VERSION ||
      hdr->bytes > KVDB_CAPACITY - sizeof(struct kvdb_header_s) ||
      hdr->crc != kvdb_crc32(kvdb_records(), hdr->bytes))
    {
      printf("kvdb: no valid store at 0x%08x, starting empty\n",
             (unsigned int)KVDB_SECTOR);
      kvdb_reset_image();
    }
  else
    {
      printf("kvdb: %lu key(s) loaded from flash, %lu bytes used\n",
             (unsigned long)hdr->count, (unsigned long)hdr->bytes);
    }

  g_kvdb_persistent = true;
  nxmutex_unlock(&g_kvdb_lock);
  return OK;
#endif
}

bool bk7258_kvdb_persistent(void)
{
  return g_kvdb_persistent;
}

int bk7258_kvdb_get(FAR const char *key, FAR char *value, size_t size)
{
  uint16_t vlen = 0;
  ssize_t off;
  int ret;

  if (key == NULL || value == NULL || size == 0 || !g_kvdb_loaded)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_kvdb_lock);

  off = kvdb_find(key, &vlen);
  if (off < 0)
    {
      ret = -ENOENT;
    }
  else if ((size_t)vlen + 1u > size)
    {
      ret = -E2BIG;
    }
  else
    {
      FAR uint8_t *rec = kvdb_records() + off;

      memcpy(value, &rec[4 + strlen(key)], vlen);
      value[vlen] = '\0';
      ret = (int)vlen;
    }

  nxmutex_unlock(&g_kvdb_lock);
  return ret;
}

int bk7258_kvdb_set(FAR const char *key, FAR const char *value)
{
  FAR struct kvdb_header_s *hdr;
  size_t klen;
  size_t vlen;
  ssize_t off;
  int ret;

  if (key == NULL || value == NULL || !g_kvdb_loaded)
    {
      return -EINVAL;
    }

  klen = strlen(key);
  vlen = strlen(value);

  if (klen == 0 || klen > 64 || vlen > 512)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_kvdb_lock);
  hdr = kvdb_hdr();

  /* Replace by removing then appending: values change length, and moving the
   * tail once is simpler than patching in place and cheaper to reason about.
   */

  off = kvdb_find(key, NULL);
  if (off >= 0)
    {
      FAR uint8_t *rec = kvdb_records() + off;
      uint16_t rk = (uint16_t)(rec[0] | (rec[1] << 8));
      uint16_t rv = (uint16_t)(rec[2] | (rec[3] << 8));
      size_t rlen = 4u + rk + rv;

      memmove(rec, rec + rlen, hdr->bytes - off - rlen);
      hdr->bytes -= rlen;
      hdr->count--;
    }

  if (sizeof(struct kvdb_header_s) + hdr->bytes + 4u + klen + vlen >
      KVDB_CAPACITY)
    {
      nxmutex_unlock(&g_kvdb_lock);
      return -ENOSPC;
    }

  {
    FAR uint8_t *rec = kvdb_records() + hdr->bytes;

    rec[0] = (uint8_t)(klen & 0xff);
    rec[1] = (uint8_t)(klen >> 8);
    rec[2] = (uint8_t)(vlen & 0xff);
    rec[3] = (uint8_t)(vlen >> 8);
    memcpy(&rec[4], key, klen);
    memcpy(&rec[4 + klen], value, vlen);

    hdr->bytes += 4u + klen + vlen;
    hdr->count++;
  }

  ret = kvdb_flush();

  nxmutex_unlock(&g_kvdb_lock);
  return ret;
}

int bk7258_kvdb_del(FAR const char *key)
{
  FAR struct kvdb_header_s *hdr;
  ssize_t off;
  int ret;

  if (key == NULL || !g_kvdb_loaded)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_kvdb_lock);
  hdr = kvdb_hdr();

  off = kvdb_find(key, NULL);
  if (off < 0)
    {
      nxmutex_unlock(&g_kvdb_lock);
      return -ENOENT;
    }

  {
    FAR uint8_t *rec = kvdb_records() + off;
    uint16_t rk = (uint16_t)(rec[0] | (rec[1] << 8));
    uint16_t rv = (uint16_t)(rec[2] | (rec[3] << 8));
    size_t rlen = 4u + rk + rv;

    memmove(rec, rec + rlen, hdr->bytes - off - rlen);
    hdr->bytes -= rlen;
    hdr->count--;
  }

  ret = kvdb_flush();

  nxmutex_unlock(&g_kvdb_lock);
  return ret;
}

int bk7258_kvdb_foreach(bk7258_kvdb_cb_t callback, FAR void *arg)
{
  FAR uint8_t *rec;
  uint32_t used;
  size_t off = 0;
  int n = 0;

  if (callback == NULL || !g_kvdb_loaded)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_kvdb_lock);
  rec = kvdb_records();
  used = kvdb_hdr()->bytes;

  while (off + 4u <= used)
    {
      uint16_t rk = (uint16_t)(rec[off] | (rec[off + 1] << 8));
      uint16_t rv = (uint16_t)(rec[off + 2] | (rec[off + 3] << 8));
      char key[65];
      char value[513];

      if (off + 4u + rk + rv > used || rk > 64 || rv > 512)
        {
          break;
        }

      memcpy(key, &rec[off + 4], rk);
      key[rk] = '\0';
      memcpy(value, &rec[off + 4 + rk], rv);
      value[rv] = '\0';

      callback(key, value, arg);
      n++;

      off += 4u + rk + rv;
    }

  nxmutex_unlock(&g_kvdb_lock);
  return n;
}
