/****************************************************************************
 * app/provisioning_web/vp_store.h
 *
 * The persisted record and its atomic replacement.  Fixed size, versioned,
 * length-checked and CRC-protected, because a half-written credential file on
 * a VFAT volume that loses power mid-write is otherwise indistinguishable
 * from a valid one.
 *
 * The passphrase is stored in the clear.  VFAT has no permission model worth
 * relying on and this chip has no confirmed device key, so pretending to
 * encrypt would only hide the exposure.  What is controlled instead: it is
 * never logged, never echoed back over HTTP, and only read through the public
 * API.  Physical access to the SD-NAND reveals it.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_PROVISIONING_WEB_VP_STORE_H
#define __APP_PROVISIONING_WEB_VP_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "velasight_provisioning.h"

/* Layout, little-endian:
 *
 *   0   4   magic "VSWP"
 *   4   2   version, currently 1
 *   6   2   flags, bit0 = open network
 *   8   4   generation
 *   12  1   ssid_len, 1..32
 *   13  1   psk_len, 0 or 8..63
 *   14  32  ssid, zero padded
 *   46  63  password, zero padded
 *   109 1   reserved, must be zero
 *   110 4   CRC32 of bytes 0..109
 */

#define VP_RECORD_SIZE    114
#define VP_RECORD_VERSION 1
#define VP_RECORD_MAGIC   "VSWP"

#define VP_RECORD_FLAG_OPEN 0x0001u

uint32_t vp_crc32(const void *data, size_t len);

/****************************************************************************
 * Name: vp_record_encode / vp_record_decode
 *
 * Description:
 *   Serialize and parse the record.  encode returns VP_RECORD_SIZE, -EINVAL
 *   for credentials that do not validate, or -E2BIG for a short buffer.
 *   decode returns 0 or -EBADMSG; it checks the magic, the version, both
 *   lengths, that the open flag agrees with psk_len, the reserved byte and
 *   the CRC.  A record that fails any of those is corrupt, not repairable.
 *
 ****************************************************************************/

int vp_record_encode(uint8_t *buf, size_t buflen,
                     const struct velasight_prov_credentials_s *cred);

int vp_record_decode(const uint8_t *buf, size_t len,
                     struct velasight_prov_credentials_s *cred);

/****************************************************************************
 * Name: vp_store_temp_path
 *
 * Description:
 *   The scratch file used by vp_store_save(), placed beside the record.
 *
 *   It is a fixed name rather than "<path>.tmp" because the SD-NAND is VFAT
 *   without long-name support: "wifi.bin.tmp" has two dots and every
 *   component must fit 8.3, so the obvious derivation is rejected with
 *   -EINVAL by the filesystem.  One save runs at a time, so a fixed name
 *   costs nothing.
 *
 *   Returns 0, or -E2BIG when the buffer is too small.
 *
 ****************************************************************************/

int vp_store_temp_path(const char *path, char *buf, size_t buflen);

/****************************************************************************
 * Name: vp_store_save
 *
 * Description:
 *   Write the record to the scratch file, flush it, fsync it, then rename it
 *   over the target, so a reader sees a complete record rather than a
 *   half-written one.  Creates the parent directory when it is missing.  On
 *   failure the scratch file is removed and any previous record is left
 *   intact.  Returns 0 or a negative errno.
 *
 *   On VFAT the rename is not a single atomic step -- NuttX's VFS unlinks an
 *   existing target first, and FAT's own rename refuses to overwrite -- so
 *   there is a brief window in which the record is absent.  That is the
 *   strongest guarantee this filesystem offers; a reader in that window gets
 *   -ENOENT, never a truncated record, because the CRC would catch it.
 *
 ****************************************************************************/

int vp_store_save(const char *path,
                  const struct velasight_prov_credentials_s *cred);

/****************************************************************************
 * Name: vp_store_load
 *
 * Description:
 *   Read and validate the record.  Returns 0, -ENOENT, -EBADMSG for a corrupt
 *   or wrong-sized file, or a negative errno.
 *
 ****************************************************************************/

int vp_store_load(const char *path,
                  struct velasight_prov_credentials_s *cred);

/****************************************************************************
 * Name: vp_store_next_generation
 *
 * Description:
 *   The generation to stamp on the next save: one past whatever is stored, or
 *   1 when nothing readable is there.  A corrupt record does not reset the
 *   counter to a value already used, it simply starts over at 1, which is the
 *   only honest answer when the old value is unreadable.
 *
 ****************************************************************************/

uint32_t vp_store_next_generation(const char *path);

#endif /* __APP_PROVISIONING_WEB_VP_STORE_H */
