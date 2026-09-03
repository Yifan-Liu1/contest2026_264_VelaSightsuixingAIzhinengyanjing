/****************************************************************************
 * Hardware JPEG SOS/entropy boundary regression tests.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "../../../../board/beken/chips/bk7258/include/bk7258_jpeg_enc.h"

static int checks;
static int failures;

#define CHECK(cond, text)                    \
  do                                         \
    {                                        \
      checks++;                              \
      if (!(cond))                           \
        {                                    \
          failures++;                        \
          printf("  FAIL %s\n", text);      \
        }                                    \
    }                                        \
  while (0)

static void put_sos(unsigned char *buf, size_t at)
{
  static const unsigned char sos[] =
    {0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02,
     0x11, 0x03, 0x11, 0x00, 0x3f, 0x00};
  memcpy(buf + at, sos, sizeof(sos));
}

int main(void)
{
  unsigned char buf[256];

  memset(buf, 0xff, sizeof(buf));
  put_sos(buf, 80);
  CHECK(bk7258_jpeg_find_sos_entropy(buf, 32, 200, 88) == 94,
        "finds SOS that starts before the parser fallback");
  CHECK(bk7258_jpeg_find_sos_entropy(buf, 32, 200, 64) == 94,
        "finds SOS that starts after the parser fallback");

  memset(buf, 0, sizeof(buf));
  buf[72] = 0xff;
  buf[73] = 0xda;
  buf[74] = 0x00;
  buf[75] = 0x08;
  buf[76] = 0x01;
  buf[77] = 0x01;
  buf[78] = 0x00;
  buf[79] = 0x00;
  buf[80] = 0x3f;
  buf[81] = 0x00;
  CHECK(bk7258_jpeg_find_sos_entropy(buf, 32, 200, 78) == 82,
        "accepts any bounded length-bearing SOS near the fallback");

  memset(buf, 0, sizeof(buf));
  buf[70] = 0xff;
  buf[71] = 0x00; /* byte-stuffed entropy, not a marker */
  CHECK(bk7258_jpeg_find_sos_entropy(buf, 32, 200, 73) == 73,
        "returns fallback when no valid SOS exists");

  memset(buf, 0, sizeof(buf));
  put_sos(buf, 180);
  CHECK(bk7258_jpeg_find_sos_entropy(buf, 32, 190, 77) == 77,
        "rejects an SOS whose segment extends beyond the scan bound");

  {
    unsigned char good[16] = {0x28, 0xa0, 0x0f};
    size_t len = 3;
    int shift = bk7258_jpeg_realign_entropy(good, &len, sizeof(good),
                                             16, 8);

    CHECK(shift == 0, "accepts an already aligned 4:2:2 scan");
    CHECK(len == 3 && good[0] == 0x28 && good[1] == 0xa0 &&
          good[2] == 0x0f,
          "leaves an aligned entropy scan byte-for-byte unchanged");
  }

  {
    unsigned char shifted[16] = {0x14, 0x50, 0x07};
    size_t len = 3;
    int shift = bk7258_jpeg_realign_entropy(shifted, &len,
                                             sizeof(shifted), 16, 8);

    CHECK(shift == 1, "detects one spurious leading entropy bit");
    CHECK(len == 3 && shifted[0] == 0x28 && shifted[1] == 0xa0 &&
          shifted[2] == 0x0f,
          "removes the leading bit and restores JPEG padding");
  }

  {
    unsigned char invalid[16] = {0, 0, 0};
    size_t len = 3;

    CHECK(bk7258_jpeg_realign_entropy(invalid, &len, sizeof(invalid),
                                      16, 8) < 0,
          "rejects a scan that no bit alignment can validate");
    CHECK(len == 3 && invalid[0] == 0 && invalid[1] == 0 &&
          invalid[2] == 0,
          "does not mutate an invalid scan");
  }

  /* Prefix validation: the capture driver decides bit alignment from the
   * first few MCUs instead of the whole frame, so the two entry points must
   * agree wherever the limit does not bite, and the limited one must still
   * find and repair a shift.
   */

  {
    unsigned char good[16] = {0x28, 0xa0, 0x0f};
    size_t len = 3;

    CHECK(bk7258_jpeg_realign_entropy_prefix(good, &len, sizeof(good),
                                             16, 8, 0) == 0,
          "prefix mode with no limit accepts an aligned scan");

    len = 3;
    CHECK(bk7258_jpeg_realign_entropy_prefix(good, &len, sizeof(good),
                                             16, 8, 99) == 0,
          "a limit above the frame's MCU count behaves as full validation");
    CHECK(len == 3 && good[0] == 0x28 && good[1] == 0xa0 &&
          good[2] == 0x0f,
          "prefix mode leaves an aligned scan byte-for-byte unchanged");
  }

  {
    unsigned char shifted[16] = {0x14, 0x50, 0x07};
    size_t len = 3;
    int shift = bk7258_jpeg_realign_entropy_prefix(shifted, &len,
                                                   sizeof(shifted),
                                                   16, 8, 1);

    CHECK(shift == 1, "prefix mode detects the spurious leading bit");
    CHECK(len == 3 && shifted[0] == 0x28 && shifted[1] == 0xa0 &&
          shifted[2] == 0x0f,
          "prefix mode repairs the scan exactly as full validation does");
  }

  {
    /* One MCU of payload described as a two-MCU frame.  This is the
     * documented cost of the prefix: the full walk sees the scan end early,
     * the prefix walk stops before it would.
     */

    unsigned char one_mcu[16] = {0x28, 0xa0, 0x0f};
    size_t len = 3;

    CHECK(bk7258_jpeg_realign_entropy(one_mcu, &len, sizeof(one_mcu),
                                      32, 8) < 0,
          "full validation rejects a scan that stops one MCU short");

    len = 3;
    CHECK(bk7258_jpeg_realign_entropy_prefix(one_mcu, &len,
                                             sizeof(one_mcu),
                                             32, 8, 1) == 0,
          "prefix validation accepts it after the MCU it was told to check");
  }

  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
