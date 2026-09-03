/****************************************************************************
 * board/beken/chips/bk7258/bk7258_jpeg_entropy.c
 *
 * Validate and repair the BK7258 JPEG encoder's entropy-bit alignment.
 * Contains no register or NuttX dependencies so the exact bitstream logic is
 * host-testable.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bk7258_jpeg_enc.h"

/* Bits resolved by one lookup in the Huffman acceleration tables.
 *
 * Each of the four tables is uint16_t[1 << JPEG_FAST_BITS], so this is a
 * multiplier on 8 KB of SRAM: at 12 bits the four cost 32 KB, at 8 bits they
 * cost 2 KB.
 *
 * Eight is what libjpeg has used as HUFF_LOOKAHEAD for decades, and it is
 * enough for the great majority of codes, which are short.  Nothing depends on
 * a hit: jpeg_decode_huff() falls through to the canonical bit-at-a-time
 * decoder whenever the table entry is zero, so a smaller table costs decode
 * time on long codes and nothing else.
 *
 * The 30 KB this releases goes to CONFIG_IOB_NBUFFERS, where it does far more
 * good.  The IOB pool sets the advertised TCP receive window through
 * tcp_get_recvwindow(), it cannot be moved to PSRAM because iob_initialize()
 * runs before the CP powers PSRAM up, and a measured download had it down to
 * 9 free of 40 with the window collapsed to 1094 bytes.  This table, by
 * contrast, is only touched while realigning a captured camera frame
 * (bk7258_camera_imgdata.c), never on the network path.
 */

#define JPEG_FAST_BITS 8u
#define JPEG_FAST_SIZE (1u << JPEG_FAST_BITS)

struct jpeg_huff_s
{
  FAR const uint8_t *counts;
  FAR const uint8_t *values;
  FAR uint16_t *fast;
};

struct jpeg_bits_s
{
  FAR const uint8_t *buf;
  size_t len;
  size_t pos;
  uint32_t reservoir;
  uint8_t nbits;
  bool invalid;
};

static const uint8_t g_dc_luma_counts[16] =
{
  0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};

static const uint8_t g_dc_luma_values[12] =
{
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const uint8_t g_ac_luma_counts[16] =
{
  0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125
};

static const uint8_t g_ac_luma_values[162] =
{
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41,
  0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91,
  0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
  0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a,
  0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38,
  0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53,
  0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66,
  0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
  0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93,
  0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
  0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
  0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
  0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1,
  0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2,
  0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};

static const uint8_t g_dc_chroma_counts[16] =
{
  0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};

static const uint8_t g_dc_chroma_values[12] =
{
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const uint8_t g_ac_chroma_counts[16] =
{
  0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119
};

static const uint8_t g_ac_chroma_values[162] =
{
  0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12,
  0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14,
  0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
  0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17,
  0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37,
  0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
  0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
  0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
  0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
  0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
  0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5,
  0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
  0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
  0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2,
  0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};

static uint16_t g_dc_luma_fast[JPEG_FAST_SIZE];
static uint16_t g_ac_luma_fast[JPEG_FAST_SIZE];
static uint16_t g_dc_chroma_fast[JPEG_FAST_SIZE];
static uint16_t g_ac_chroma_fast[JPEG_FAST_SIZE];
static bool g_huff_fast_ready;

static struct jpeg_huff_s g_dc_luma =
{
  g_dc_luma_counts, g_dc_luma_values, g_dc_luma_fast
};

static struct jpeg_huff_s g_ac_luma =
{
  g_ac_luma_counts, g_ac_luma_values, g_ac_luma_fast
};

static struct jpeg_huff_s g_dc_chroma =
{
  g_dc_chroma_counts, g_dc_chroma_values, g_dc_chroma_fast
};

static struct jpeg_huff_s g_ac_chroma =
{
  g_ac_chroma_counts, g_ac_chroma_values, g_ac_chroma_fast
};

static void jpeg_build_fast(FAR struct jpeg_huff_s *table)
{
  uint32_t code = 0;
  unsigned int index = 0;
  unsigned int length;

  memset(table->fast, 0, JPEG_FAST_SIZE * sizeof(table->fast[0]));

  for (length = 1; length <= 16; length++)
    {
      unsigned int count = table->counts[length - 1u];
      unsigned int i;

      for (i = 0; i < count; i++)
        {
          if (length <= JPEG_FAST_BITS)
            {
              unsigned int fill = 1u << (JPEG_FAST_BITS - length);
              unsigned int base =
                (unsigned int)code << (JPEG_FAST_BITS - length);
              unsigned int j;
              uint16_t entry = (uint16_t)((length << 8) |
                                          table->values[index]);

              for (j = 0; j < fill; j++)
                {
                  table->fast[base + j] = entry;
                }
            }

          code++;
          index++;
        }

      code <<= 1;
    }
}

static void jpeg_init_fast_tables(void)
{
  if (!g_huff_fast_ready)
    {
      jpeg_build_fast(&g_dc_luma);
      jpeg_build_fast(&g_ac_luma);
      jpeg_build_fast(&g_dc_chroma);
      jpeg_build_fast(&g_ac_chroma);
      g_huff_fast_ready = true;
    }
}

static bool jpeg_fill_bits(FAR struct jpeg_bits_s *bits,
                           unsigned int needed)
{
  while ((unsigned int)bits->nbits < needed)
    {
      uint8_t value;

      if (bits->pos >= bits->len)
        {
          return false;
        }

      value = bits->buf[bits->pos++];
      if (value == 0xff)
        {
          if (bits->pos >= bits->len || bits->buf[bits->pos] != 0x00)
            {
              bits->invalid = true;
              return false;
            }

          bits->pos++;
        }

      bits->reservoir = (bits->reservoir << 8) | value;
      bits->nbits = (uint8_t)(bits->nbits + 8u);
    }

  return true;
}

static int jpeg_take_bits(FAR struct jpeg_bits_s *bits,
                          unsigned int count)
{
  uint32_t mask;
  uint32_t value;

  if (count == 0)
    {
      return 0;
    }

  if (count > 16u || !jpeg_fill_bits(bits, count))
    {
      return -ENODATA;
    }

  bits->nbits = (uint8_t)((unsigned int)bits->nbits - count);
  mask = (1u << count) - 1u;
  value = (bits->reservoir >> bits->nbits) & mask;

  if (bits->nbits == 0)
    {
      bits->reservoir = 0;
    }
  else
    {
      bits->reservoir &= (1u << bits->nbits) - 1u;
    }

  return (int)value;
}

static int jpeg_get_bit(FAR struct jpeg_bits_s *bits)
{
  return jpeg_take_bits(bits, 1);
}

static int jpeg_skip_bits(FAR struct jpeg_bits_s *bits, unsigned int count)
{
  return jpeg_take_bits(bits, count) < 0 ? -ENODATA : 0;
}

static int jpeg_decode_huff(FAR struct jpeg_bits_s *bits,
                            FAR const struct jpeg_huff_s *table)
{
  uint32_t first = 0;
  unsigned int index = 0;
  unsigned int available;
  unsigned int length;

  if (jpeg_fill_bits(bits, JPEG_FAST_BITS))
    {
      uint16_t key = (uint16_t)(
        bits->reservoir >> (bits->nbits - JPEG_FAST_BITS));
      uint16_t entry = table->fast[key];

      if (entry != 0)
        {
          length = entry >> 8;
          bits->nbits = (uint8_t)((unsigned int)bits->nbits - length);
          if (bits->nbits == 0)
            {
              bits->reservoir = 0;
            }
          else
            {
              bits->reservoir &= (1u << bits->nbits) - 1u;
            }

          return entry & 0xffu;
        }
    }

  /* Sixteen bits cover the longest baseline code.  Near EOI fewer can
   * remain, so use everything available and let an unmatched table fail.
   */

  (void)jpeg_fill_bits(bits, 16);
  available = bits->nbits;

  for (length = 1; length <= 16 && length <= available; length++)
    {
      unsigned int count = table->counts[length - 1u];
      uint32_t mask = (1u << length) - 1u;
      uint32_t code = (bits->reservoir >> (available - length)) & mask;

      if (code >= first && code - first < count)
        {
          bits->nbits = (uint8_t)(available - length);
          if (bits->nbits == 0)
            {
              bits->reservoir = 0;
            }
          else
            {
              bits->reservoir &= (1u << bits->nbits) - 1u;
            }

          return table->values[index + (unsigned int)(code - first)];
        }

      index += count;
      first = (first + count) << 1;
    }

  return bits->invalid ? -EINVAL : -ENODATA;
}

static bool jpeg_decode_block(FAR struct jpeg_bits_s *bits,
                              FAR const struct jpeg_huff_s *dc,
                              FAR const struct jpeg_huff_s *ac)
{
  int symbol = jpeg_decode_huff(bits, dc);
  unsigned int coefficient = 1;

  if (symbol < 0 || symbol > 11 ||
      jpeg_skip_bits(bits, (unsigned int)symbol) < 0)
    {
      return false;
    }

  while (coefficient < 64)
    {
      unsigned int run;
      unsigned int size;

      symbol = jpeg_decode_huff(bits, ac);
      if (symbol < 0)
        {
          return false;
        }

      run = (unsigned int)symbol >> 4;
      size = (unsigned int)symbol & 15u;

      if (size == 0)
        {
          if (run == 0)
            {
              break;
            }

          if (run != 15 || coefficient + 16u > 64u)
            {
              return false;
            }

          coefficient += 16u;
        }
      else
        {
          coefficient += run;
          if (coefficient >= 64u || jpeg_skip_bits(bits, size) < 0)
            {
              return false;
            }

          coefficient++;
        }
    }

  return true;
}

static bool jpeg_alignment_valid(FAR const uint8_t *buf, size_t len,
                                 uint16_t width, uint16_t height,
                                 unsigned int shift)
{
  struct jpeg_bits_s bits = {buf, len, 0, 0, 0, false};
  uint32_t mcus_x = ((uint32_t)width + 15u) / 16u;
  uint32_t mcus_y = ((uint32_t)height + 7u) / 8u;
  uint32_t mcus = mcus_x * mcus_y;
  uint32_t i;
  unsigned int padding = 0;
  int bit;

  if (width == 0 || height == 0 || shift > 7 ||
      jpeg_skip_bits(&bits, shift) < 0)
    {
      return false;
    }

  for (i = 0; i < mcus; i++)
    {
      if (!jpeg_decode_block(&bits, &g_dc_luma, &g_ac_luma) ||
          !jpeg_decode_block(&bits, &g_dc_luma, &g_ac_luma) ||
          !jpeg_decode_block(&bits, &g_dc_chroma, &g_ac_chroma) ||
          !jpeg_decode_block(&bits, &g_dc_chroma, &g_ac_chroma))
        {
          return false;
        }
    }

  while ((bit = jpeg_get_bit(&bits)) >= 0)
    {
      if (bit == 0 || ++padding > 7u)
        {
          return false;
        }
    }

  return !bits.invalid;
}

static bool jpeg_next_byte(FAR const uint8_t *buf, size_t len,
                           FAR size_t *pos, FAR uint8_t *value)
{
  if (*pos >= len)
    {
      return false;
    }

  *value = buf[(*pos)++];
  if (*value == 0xff && *pos < len && buf[*pos] == 0x00)
    {
      (*pos)++;
    }

  return true;
}

static size_t jpeg_shifted_size(FAR const uint8_t *buf, size_t len,
                                unsigned int shift)
{
  size_t pos = 0;
  size_t outlen = 0;
  uint8_t current;
  uint8_t next;

  if (!jpeg_next_byte(buf, len, &pos, &current))
    {
      return 0;
    }

  while (jpeg_next_byte(buf, len, &pos, &next))
    {
      uint8_t out = (uint8_t)((uint8_t)(current << shift) |
                              (uint8_t)(next >> (8u - shift)));
      outlen += out == 0xff ? 2u : 1u;
      current = next;
    }

  current = (uint8_t)((uint8_t)(current << shift) |
                      (uint8_t)((1u << shift) - 1u));
  return outlen + (current == 0xff ? 2u : 1u);
}

static size_t jpeg_write_shifted(FAR uint8_t *dest,
                                 FAR const uint8_t *src, size_t len,
                                 unsigned int shift)
{
  size_t pos = 0;
  size_t outpos = 0;
  uint8_t current;
  uint8_t next;

  if (!jpeg_next_byte(src, len, &pos, &current))
    {
      return 0;
    }

  do
    {
      bool more = jpeg_next_byte(src, len, &pos, &next);
      uint8_t out;

      if (more)
        {
          out = (uint8_t)((uint8_t)(current << shift) |
                          (uint8_t)(next >> (8u - shift)));
        }
      else
        {
          out = (uint8_t)((uint8_t)(current << shift) |
                          (uint8_t)((1u << shift) - 1u));
        }

      dest[outpos++] = out;
      if (out == 0xff)
        {
          dest[outpos++] = 0x00;
        }

      if (!more)
        {
          break;
        }

      current = next;
    }
  while (true);

  return outpos;
}

int bk7258_jpeg_realign_entropy(FAR uint8_t *buf, FAR size_t *len,
                                size_t capacity, uint16_t width,
                                uint16_t height)
{
  size_t scanlen;
  unsigned int shift;

  if (buf == NULL || len == NULL || *len == 0 || *len > capacity)
    {
      return -EINVAL;
    }

  scanlen = *len;
  jpeg_init_fast_tables();

  while (scanlen > 0 && buf[scanlen - 1u] == 0xff)
    {
      scanlen--;
    }

  for (shift = 0; shift < 8; shift++)
    {
      if (jpeg_alignment_valid(buf, scanlen, width, height, shift))
        {
          size_t outlen;
          FAR uint8_t *scratch;

          if (shift == 0)
            {
              return 0;
            }

          outlen = jpeg_shifted_size(buf, scanlen, shift);
          if (outlen == 0 || *len > capacity - outlen)
            {
              return -ENOSPC;
            }

          scratch = buf + *len;
          if (jpeg_write_shifted(scratch, buf, scanlen, shift) != outlen)
            {
              return -EIO;
            }

          memmove(buf, scratch, outlen);
          *len = outlen;
          return (int)shift;
        }
    }

  return -EBADMSG;
}
