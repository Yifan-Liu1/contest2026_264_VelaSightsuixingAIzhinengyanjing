/****************************************************************************
 * app/provisioning_web/tests/test_vp_store.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vp_store.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                       \
  do                                                            \
    {                                                           \
      g_checks++;                                                \
      if (!(cond))                                              \
        {                                                       \
          g_failures++;                                         \
          printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);   \
        }                                                       \
    }                                                           \
  while (0)

static char g_dir[] = "/tmp/vp_store_testXXXXXX";
static char g_path[256];

static void fill(struct velasight_prov_credentials_s *cred,
                 const char *ssid, const char *psk, uint32_t generation)
{
  memset(cred, 0, sizeof(*cred));
  snprintf(cred->ssid, sizeof(cred->ssid), "%s", ssid);
  snprintf(cred->password, sizeof(cred->password), "%s", psk);
  cred->generation   = generation;
  cred->open_network = cred->password[0] == '\0';
}

static void test_crc(void)
{
  /* The IEEE CRC32 of "123456789" is a published value, so a wrong table or a
   * wrong reflection shows up here rather than as an unreadable record.
   */

  CHECK(vp_crc32("123456789", 9) == 0xcbf43926u, "CRC32 matches the check value");
  CHECK(vp_crc32("", 0) == 0u, "CRC32 of nothing is zero");
  CHECK(vp_crc32("a", 1) != vp_crc32("b", 1), "CRC32 separates inputs");
}

static void test_record_roundtrip(void)
{
  struct velasight_prov_credentials_s in;
  struct velasight_prov_credentials_s out;
  uint8_t buf[VP_RECORD_SIZE];
  uint8_t small[VP_RECORD_SIZE - 1];

  fill(&in, "AIPC", "passphrase", 7);
  CHECK(vp_record_encode(buf, sizeof(buf), &in) == VP_RECORD_SIZE,
        "encode fills exactly the record");
  CHECK(vp_record_encode(small, sizeof(small), &in) == -E2BIG,
        "encode refuses a short buffer");
  CHECK(memcmp(buf, VP_RECORD_MAGIC, 4) == 0, "the magic leads the record");

  memset(&out, 0xff, sizeof(out));
  CHECK(vp_record_decode(buf, sizeof(buf), &out) == 0 &&
        strcmp(out.ssid, "AIPC") == 0 &&
        strcmp(out.password, "passphrase") == 0 &&
        out.generation == 7 && !out.open_network,
        "decode returns what encode was given");

  fill(&in, "OpenNet", "", 1);
  CHECK(vp_record_encode(buf, sizeof(buf), &in) == VP_RECORD_SIZE &&
        vp_record_decode(buf, sizeof(buf), &out) == 0 &&
        out.open_network && out.password[0] == '\0',
        "an open network survives the round trip");

  fill(&in, "", "passphrase", 1);
  CHECK(vp_record_encode(buf, sizeof(buf), &in) == -EINVAL,
        "encode will not persist credentials that do not validate");
}

static void test_record_corruption(void)
{
  struct velasight_prov_credentials_s in;
  struct velasight_prov_credentials_s out;
  uint8_t buf[VP_RECORD_SIZE];

  fill(&in, "AIPC", "passphrase", 3);
  CHECK(vp_record_encode(buf, sizeof(buf), &in) == VP_RECORD_SIZE,
        "baseline record encodes");
  CHECK(vp_record_decode(buf, sizeof(buf) - 1, &out) == -EBADMSG,
        "a truncated record is rejected");

  buf[0] = 'X';
  CHECK(vp_record_decode(buf, sizeof(buf), &out) == -EBADMSG,
        "a wrong magic is rejected");
  buf[0] = 'V';

  buf[4] = VP_RECORD_VERSION + 1;
  CHECK(vp_record_decode(buf, sizeof(buf), &out) == -EBADMSG,
        "an unknown version is rejected rather than guessed");
  buf[4] = VP_RECORD_VERSION;

  buf[14] ^= 0x01;
  CHECK(vp_record_decode(buf, sizeof(buf), &out) == -EBADMSG,
        "a flipped SSID byte fails the CRC");
  buf[14] ^= 0x01;

  buf[113] ^= 0x80;
  CHECK(vp_record_decode(buf, sizeof(buf), &out) == -EBADMSG,
        "a flipped CRC byte is caught");
  buf[113] ^= 0x80;

  CHECK(vp_record_decode(buf, sizeof(buf), &out) == 0,
        "the record is intact again after undoing the flips");

  /* Lengths and the open flag have to agree with each other; a record saying
   * "open" while carrying a passphrase has no correct interpretation.
   */

  buf[13] = 4;
  buf[110] = 0;
  buf[111] = 0;
  buf[112] = 0;
  buf[113] = 0;
  {
    uint32_t crc = vp_crc32(buf, VP_RECORD_SIZE - 4);
    buf[110] = (uint8_t)(crc & 0xff);
    buf[111] = (uint8_t)((crc >> 8) & 0xff);
    buf[112] = (uint8_t)((crc >> 16) & 0xff);
    buf[113] = (uint8_t)((crc >> 24) & 0xff);
  }

  CHECK(vp_record_decode(buf, sizeof(buf), &out) == -EBADMSG,
        "a 4-byte password length is rejected even with a valid CRC");

  fill(&in, "AIPC", "passphrase", 3);
  CHECK(vp_record_encode(buf, sizeof(buf), &in) == VP_RECORD_SIZE,
        "re-encode for the reserved byte case");
  buf[109] = 1;
  {
    uint32_t crc = vp_crc32(buf, VP_RECORD_SIZE - 4);
    buf[110] = (uint8_t)(crc & 0xff);
    buf[111] = (uint8_t)((crc >> 8) & 0xff);
    buf[112] = (uint8_t)((crc >> 16) & 0xff);
    buf[113] = (uint8_t)((crc >> 24) & 0xff);
  }

  CHECK(vp_record_decode(buf, sizeof(buf), &out) == -EBADMSG,
        "a non-zero reserved byte is rejected");
}

static void test_store_file(void)
{
  struct velasight_prov_credentials_s in;
  struct velasight_prov_credentials_s out;
  char nested[sizeof(g_path) + 32];
  char tmp[sizeof(g_path) + 8];
  FILE *stream;

  CHECK(vp_store_load(g_path, &out) == -ENOENT,
        "nothing provisioned reads as -ENOENT");
  CHECK(vp_store_next_generation(g_path) == 1,
        "the first save is generation 1");

  fill(&in, "AIPC", "passphrase", vp_store_next_generation(g_path));
  CHECK(vp_store_save(g_path, &in) == 0, "the first save succeeds");
  CHECK(vp_store_load(g_path, &out) == 0 &&
        strcmp(out.ssid, "AIPC") == 0 &&
        strcmp(out.password, "passphrase") == 0 && out.generation == 1,
        "the saved record reads back");

  snprintf(tmp, sizeof(tmp), "%s/vpsave.tmp", g_dir);
  CHECK(access(tmp, F_OK) != 0, "the temporary file is not left behind");

  CHECK(vp_store_next_generation(g_path) == 2,
        "the next generation follows the stored one");

  fill(&in, "OtherNet", "", vp_store_next_generation(g_path));
  CHECK(vp_store_save(g_path, &in) == 0, "a second save replaces the first");
  CHECK(vp_store_load(g_path, &out) == 0 &&
        strcmp(out.ssid, "OtherNet") == 0 && out.open_network &&
        out.generation == 2, "the replacement is what reads back");

  /* A corrupt file must read as corrupt, and must not be silently replaced by
   * the loader; only an explicit save may overwrite it.
   */

  stream = fopen(g_path, "wb");
  CHECK(stream != NULL, "the record can be opened for corruption");
  if (stream != NULL)
    {
      CHECK(fwrite("not a record", 1, 12, stream) == 12, "corruption written");
      fclose(stream);
    }

  CHECK(vp_store_load(g_path, &out) == -EBADMSG,
        "a corrupt record reads as -EBADMSG");
  CHECK(vp_store_next_generation(g_path) == 1,
        "an unreadable generation restarts at 1 instead of reusing a value");

  fill(&in, "AIPC", "passphrase", 5);
  CHECK(vp_store_save(g_path, &in) == 0, "saving over a corrupt record works");
  CHECK(vp_store_load(g_path, &out) == 0 && out.generation == 5,
        "the explicit generation is preserved");

  /* The default path has a directory component that will not exist on a fresh
   * SD-NAND, so the store has to create it.
   */

  snprintf(nested, sizeof(nested), "%s/velasight/wifi-provision.bin", g_dir);
  fill(&in, "Nested", "passphrase", 1);
  CHECK(vp_store_save(nested, &in) == 0,
        "a missing parent directory is created");
  CHECK(vp_store_load(nested, &out) == 0 && strcmp(out.ssid, "Nested") == 0,
        "the nested record reads back");

  CHECK(vp_store_save("/tmp/vp_no_such_dir/x/y/rec.bin", &in) < 0,
        "an unusable path fails instead of pretending to save");

  /* A missing directory in the path means nothing was ever provisioned, and
   * the caller should be told that rather than a raw filesystem error.  FAT
   * reports -ENOTDIR here where Linux reports -ENOENT, and the board printed
   * "cannot read ...: -20" for a device that was simply unprovisioned.
   */

  snprintf(nested, sizeof(nested), "%s/notadir/rec.bin", g_path);
  CHECK(vp_store_load(nested, &out) == -ENOENT,
        "a path whose parent is a file reads as not provisioned");

  snprintf(nested, sizeof(nested), "%s/missing/rec.bin", g_dir);
  CHECK(vp_store_load(nested, &out) == -ENOENT,
        "a path with a missing directory reads as not provisioned");
}

static void cleanup(void)
{
  char path[sizeof(g_path) + 40];

  snprintf(path, sizeof(path), "%s/velasight/wifi-provision.bin", g_dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/velasight/vpsave.tmp", g_dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/velasight", g_dir);
  rmdir(path);
  unlink(g_path);
  snprintf(path, sizeof(path), "%s/vpsave.tmp", g_dir);
  unlink(path);
  rmdir(g_dir);
}

/****************************************************************************
 * The SD-NAND is VFAT without long-name support (CONFIG_FAT_LFN is not set on
 * this board), so every path component must fit 8.3.  This was not a
 * hypothetical: "velasight/" and "wifi-provision.bin" both failed with EINVAL
 * on the board, and appending ".tmp" to a name that already had an extension
 * produced "wifi.bin.tmp", which has two dots and is equally invalid.
 ****************************************************************************/

static bool fits_83(const char *name)
{
  const char *dot = strchr(name, '.');
  size_t base;
  size_t ext;

  if (strchr(name, '.') != strrchr(name, '.'))
    {
      return false; /* more than one dot */
    }

  base = dot != NULL ? (size_t)(dot - name) : strlen(name);
  ext  = dot != NULL ? strlen(dot + 1) : 0;
  return base >= 1 && base <= 8 && ext <= 3;
}

static void check_path_83(const char *path, const char *what)
{
  char copy[256];
  char *cursor;
  bool ok = true;

  snprintf(copy, sizeof(copy), "%s", path);
  cursor = copy;
  while (*cursor == '/')
    {
      cursor++;
    }

  while (*cursor != '\0')
    {
      char *slash = strchr(cursor, '/');

      if (slash != NULL)
        {
          *slash = '\0';
        }

      if (!fits_83(cursor))
        {
          printf("        component \"%s\" does not fit 8.3\n", cursor);
          ok = false;
        }

      if (slash == NULL)
        {
          break;
        }

      cursor = slash + 1;
    }

  CHECK(ok, what);
}

static void test_short_names(void)
{
  char tmp[256];
  int ret;

  check_path_83(CONFIG_VELASIGHT_PROVISION_STORE,
                "the default store path fits 8.3 on every component");

  ret = vp_store_temp_path("/mnt/sdnand/prov/wifi.bin", tmp, sizeof(tmp));
  CHECK(ret == 0, "a temp path is derived");
  CHECK(strcmp(tmp, "/mnt/sdnand/prov/vpsave.tmp") == 0,
        "the temp file is a fixed 8.3 name beside the record");
  check_path_83(tmp, "the temp path fits 8.3 on every component");

  ret = vp_store_temp_path("record.bin", tmp, sizeof(tmp));
  CHECK(ret == 0 && strcmp(tmp, "vpsave.tmp") == 0,
        "a bare filename gets a bare temp name");

  CHECK(vp_store_temp_path("/mnt/sdnand/prov/wifi.bin", tmp, 8) == -E2BIG,
        "a short buffer is refused");
}

int main(void)
{
  int status;

  if (mkdtemp(g_dir) == NULL)
    {
      printf("FAIL cannot create a temporary directory\n");
      return 1;
    }

  snprintf(g_path, sizeof(g_path), "%s/wifi-provision.bin", g_dir);

  test_crc();
  test_record_roundtrip();
  test_record_corruption();
  test_short_names();
  test_store_file();
  cleanup();

  status = g_failures == 0 ? 0 : 1;
  printf("%s: %d checks, %d failures\n",
         status == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return status;
}
