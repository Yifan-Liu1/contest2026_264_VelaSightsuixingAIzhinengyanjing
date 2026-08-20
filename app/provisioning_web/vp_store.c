/****************************************************************************
 * app/provisioning_web/vp_store.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vp_form.h"
#include "vp_store.h"

#define VP_OFF_MAGIC     0
#define VP_OFF_VERSION   4
#define VP_OFF_FLAGS     6
#define VP_OFF_GEN       8
#define VP_OFF_SSID_LEN  12
#define VP_OFF_PSK_LEN   13
#define VP_OFF_SSID      14
#define VP_OFF_PSK       46
#define VP_OFF_API       109
#define VP_OFF_RESERVED  621
#define VP_OFF_CRC       622

#define VP_STORE_PATH_MAX 192

uint32_t vp_crc32(const void *data, size_t len)
{
  const unsigned char *p = (const unsigned char *)data;
  uint32_t crc = 0xffffffffu;
  size_t i;

  /* Bitwise rather than a table: 114 bytes twice per save is not worth 1KB of
   * flash on a part that is already at 90% of its region.
   */

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

static void vp_put16(uint8_t *buf, uint16_t value)
{
  buf[0] = (uint8_t)(value & 0xffu);
  buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void vp_put32(uint8_t *buf, uint32_t value)
{
  buf[0] = (uint8_t)(value & 0xffu);
  buf[1] = (uint8_t)((value >> 8) & 0xffu);
  buf[2] = (uint8_t)((value >> 16) & 0xffu);
  buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint16_t vp_get16(const uint8_t *buf)
{
  return (uint16_t)((uint16_t)buf[0] | (uint16_t)((uint16_t)buf[1] << 8));
}

static uint32_t vp_get32(const uint8_t *buf)
{
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static size_t vp_len(const char *s, size_t maxlen)
{
  size_t n = 0;

  while (n < maxlen && s[n] != '\0')
    {
      n++;
    }

  return n;
}

int vp_record_encode(uint8_t *buf, size_t buflen,
                     const struct velasight_prov_credentials_s *cred)
{
  size_t ssid_len;
  size_t psk_len;
  size_t api_len;
  int ret;

  if (buf == NULL || cred == NULL)
    {
      return -EINVAL;
    }

  if (buflen < VP_RECORD_SIZE)
    {
      return -E2BIG;
    }

  ret = vp_credentials_validate(cred);
  if (ret < 0)
    {
      return ret;
    }

  ssid_len = vp_len(cred->ssid, sizeof(cred->ssid));
  psk_len  = vp_len(cred->password, sizeof(cred->password));
  api_len  = vp_len(cred->api_key, sizeof(cred->api_key));

  memset(buf, 0, VP_RECORD_SIZE);
  memcpy(buf + VP_OFF_MAGIC, VP_RECORD_MAGIC, 4);
  vp_put16(buf + VP_OFF_VERSION, VP_RECORD_VERSION);
  vp_put16(buf + VP_OFF_FLAGS,
           cred->open_network ? VP_RECORD_FLAG_OPEN : 0);
  vp_put32(buf + VP_OFF_GEN, cred->generation);
  buf[VP_OFF_SSID_LEN] = (uint8_t)ssid_len;
  buf[VP_OFF_PSK_LEN]  = (uint8_t)psk_len;
  memcpy(buf + VP_OFF_SSID, cred->ssid, ssid_len);
  memcpy(buf + VP_OFF_PSK, cred->password, psk_len);
  memcpy(buf + VP_OFF_API, cred->api_key, api_len);
  vp_put32(buf + VP_OFF_CRC, vp_crc32(buf, VP_RECORD_SIZE - 4));
  return VP_RECORD_SIZE;
}

static bool vp_padding_is_zero(const uint8_t *buf, size_t offset,
                               size_t used, size_t field)
{
  size_t i;

  for (i = used; i < field; i++)
    {
      if (buf[offset + i] != 0)
        {
          return false;
        }
    }

  return true;
}

int vp_record_decode(const uint8_t *buf, size_t len,
                     struct velasight_prov_credentials_s *cred)
{
  struct velasight_prov_credentials_s parsed;
  size_t ssid_len;
  size_t psk_len;
  size_t api_len;
  uint16_t flags;

  if (buf == NULL || cred == NULL)
    {
      return -EINVAL;
    }

  /* Exactly one size is legal.  A shorter file is a truncated write and a
   * longer one is not this format, and neither is worth guessing about.
   */

  if (len != VP_RECORD_SIZE)
    {
      return -EBADMSG;
    }

  if (memcmp(buf + VP_OFF_MAGIC, VP_RECORD_MAGIC, 4) != 0 ||
      vp_get16(buf + VP_OFF_VERSION) != VP_RECORD_VERSION ||
      buf[VP_OFF_RESERVED] != 0)
    {
      return -EBADMSG;
    }

  if (vp_get32(buf + VP_OFF_CRC) != vp_crc32(buf, VP_RECORD_SIZE - 4))
    {
      return -EBADMSG;
    }

  flags    = vp_get16(buf + VP_OFF_FLAGS);
  ssid_len = buf[VP_OFF_SSID_LEN];
  psk_len  = buf[VP_OFF_PSK_LEN];
  api_len  = vp_len((const char *)(buf + VP_OFF_API),
                    VELASIGHT_PROV_API_KEY_MAX);

  if ((flags & ~(uint16_t)VP_RECORD_FLAG_OPEN) != 0)
    {
      return -EBADMSG;
    }

  if (ssid_len == 0 || ssid_len > VELASIGHT_PROV_SSID_MAX)
    {
      return -EBADMSG;
    }

  if (psk_len != 0 &&
      (psk_len < VELASIGHT_PROV_PSK_MIN || psk_len > VELASIGHT_PROV_PSK_MAX))
    {
      return -EBADMSG;
    }

  if (((flags & VP_RECORD_FLAG_OPEN) != 0) != (psk_len == 0))
    {
      return -EBADMSG;
    }

  if (!vp_padding_is_zero(buf, VP_OFF_SSID, ssid_len,
                          VELASIGHT_PROV_SSID_MAX) ||
      !vp_padding_is_zero(buf, VP_OFF_PSK, psk_len,
                          VELASIGHT_PROV_PSK_MAX))
    {
      return -EBADMSG;
    }

  if (api_len > VELASIGHT_PROV_API_KEY_MAX ||
      !vp_padding_is_zero(buf, VP_OFF_API, api_len,
                          VELASIGHT_PROV_API_KEY_MAX))
    {
      return -EBADMSG;
    }

  memset(&parsed, 0, sizeof(parsed));
  memcpy(parsed.ssid, buf + VP_OFF_SSID, ssid_len);
  memcpy(parsed.password, buf + VP_OFF_PSK, psk_len);
  memcpy(parsed.api_key, buf + VP_OFF_API, api_len);
  parsed.generation   = vp_get32(buf + VP_OFF_GEN);
  parsed.open_network = psk_len == 0;

  /* The record can be structurally perfect and still describe credentials the
   * running code would refuse, for instance after a limit changes.  Treat that
   * as corrupt rather than handing the caller something it cannot use.
   */

  if (vp_credentials_validate(&parsed) < 0)
    {
      return -EBADMSG;
    }

  *cred = parsed;
  return 0;
}

static int vp_make_parent(const char *path)
{
  char dir[VP_STORE_PATH_MAX];
  const char *slash;
  size_t len;

  slash = strrchr(path, '/');
  if (slash == NULL || slash == path)
    {
      return 0;
    }

  len = (size_t)(slash - path);
  if (len >= sizeof(dir))
    {
      return -ENAMETOOLONG;
    }

  memcpy(dir, path, len);
  dir[len] = '\0';

  if (mkdir(dir, 0777) == 0 || errno == EEXIST)
    {
      return 0;
    }

  return -errno;
}

int vp_store_temp_path(const char *path, char *buf, size_t buflen)
{
  static const char name[] = "vpsave.tmp";
  const char *slash;
  size_t dirlen;

  if (path == NULL || buf == NULL)
    {
      return -EINVAL;
    }

  slash = strrchr(path, '/');
  dirlen = slash != NULL ? (size_t)(slash - path) + 1 : 0;

  if (dirlen + sizeof(name) > buflen)
    {
      return -E2BIG;
    }

  memcpy(buf, path, dirlen);
  memcpy(buf + dirlen, name, sizeof(name));
  return 0;
}

int vp_store_save(const char *path,
                  const struct velasight_prov_credentials_s *cred)
{
  uint8_t record[VP_RECORD_SIZE];
  char tmp[VP_STORE_PATH_MAX + 16];
  FILE *stream;
  int ret;

  if (path == NULL || cred == NULL)
    {
      return -EINVAL;
    }

  if (strlen(path) >= VP_STORE_PATH_MAX)
    {
      return -ENAMETOOLONG;
    }

  ret = vp_record_encode(record, sizeof(record), cred);
  if (ret < 0)
    {
      return ret;
    }

  ret = vp_make_parent(path);
  if (ret < 0)
    {
      return ret;
    }

  ret = vp_store_temp_path(path, tmp, sizeof(tmp));
  if (ret < 0)
    {
      return ret;
    }

  stream = fopen(tmp, "wb");
  if (stream == NULL)
    {
      return -errno;
    }

  /* Write, flush, fsync, close, rename.  Without the fsync the rename can be
   * durable while the data behind it is not, which on a power cut leaves a
   * record that passes its own CRC check by accident or fails it for no
   * reason the next boot can explain.
   */

  if (fwrite(record, 1, sizeof(record), stream) != sizeof(record) ||
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
      /* Keep a complete, fsynced scratch record.  FAT replacement unlinks an
       * existing destination before rename, so this may be the only valid
       * copy left after an interrupted replacement.  load() recovers it when
       * the final path is absent.
       */
      return ret;
    }

  sync();
  return 0;
}

int vp_store_load(const char *path,
                  struct velasight_prov_credentials_s *cred)
{
  uint8_t record[VP_RECORD_SIZE + 1];
  char tmp[VP_STORE_PATH_MAX + 16];
  FILE *stream;
  size_t nread;
  bool recover = false;

  if (path == NULL || cred == NULL)
    {
      return -EINVAL;
    }

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      /* ENOTDIR belongs with ENOENT here: a directory component that is
       * missing, or is a file, means nothing was ever stored.  NuttX's FAT
       * reports ENOTDIR for the missing-directory case where Linux reports
       * ENOENT, and a device that has simply never been provisioned should
       * not look like a broken one.  EINVAL is deliberately left alone: on
       * that filesystem it means the configured path cannot be represented,
       * which is a misconfiguration worth seeing.
       */

      if (errno == ENOENT || errno == ENOTDIR)
        {
          if (vp_store_temp_path(path, tmp, sizeof(tmp)) < 0)
            {
              return -ENOENT;
            }

          stream = fopen(tmp, "rb");
          if (stream == NULL)
            {
              return errno == ENOENT || errno == ENOTDIR ? -ENOENT : -errno;
            }

          recover = true;
        }
      else
        {
          return -errno;
        }
    }

  nread = fread(record, 1, sizeof(record), stream);
  fclose(stream);

  if (vp_record_decode(record, nread, cred) < 0)
    {
      return -EBADMSG;
    }

  if (recover)
    {
      if (rename(tmp, path) < 0)
        {
          return -errno;
        }

      sync();
    }

  return 0;
}

uint32_t vp_store_next_generation(const char *path)
{
  struct velasight_prov_credentials_s cred;

  if (vp_store_load(path, &cred) < 0 || cred.generation == UINT32_MAX)
    {
      return 1;
    }

  return cred.generation + 1;
}
