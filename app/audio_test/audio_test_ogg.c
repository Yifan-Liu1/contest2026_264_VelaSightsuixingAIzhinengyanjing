/****************************************************************************
 * app/audio_test/audio_test_ogg.c
 *
 * Encode captured PCM as Ogg Opus and print it as base64.
 *
 * Why a container and not raw Opus packets: Opus packets carry no sample
 * rate, channel count or packet boundaries, so a bare concatenation is not
 * decodable without side information.  Every speech-recognition service that
 * accepts Opus accepts it in Ogg (RFC 7845), which is 28 bytes of page
 * header plus two small headers -- cheap enough that emitting anything less
 * would only move the work to whoever receives the file.
 *
 * Why 16 kHz: it is one of the rates Opus accepts natively (8000, 12000,
 * 16000, 24000, 48000) and it is what this board's ADC produces, so nothing
 * resamples.  32 kHz, which the ADC also supports, is *not* an Opus rate and
 * would force a resample through 48 kHz.
 *
 * The granule positions in an Ogg Opus stream are always in 48 kHz units
 * whatever the input rate (RFC 7845 section 4), so a 20 ms frame advances
 * the granule by 960 regardless of the 320 samples actually fed in at
 * 16 kHz.  Getting that wrong yields a file that decodes but reports the
 * wrong duration, which is the kind of bug that surfaces as "the cloud says
 * the audio is 1.7 seconds and we recorded 5".
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arch/chip/bk7258_psram.h>

#include <opus.h>

#include "audio_test_ogg.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 20 ms per Opus frame.  Opus allows 2.5/5/10/20/40/60 ms; 20 ms is the
 * usual speech trade-off between packet overhead and latency, and at 16 kHz
 * it is exactly 320 samples.
 */

#define OGG_FRAME_MS            20

/* Largest packet Opus can emit for one frame. */

#define OGG_MAX_PACKET          1275

/* Packets per audio page.  A page can hold at most 255 segments and each of
 * our packets is one segment, so 50 leaves plenty of margin while keeping
 * one second of audio per page at 20 ms per packet.
 */

#define OGG_PACKETS_PER_PAGE    50

/* Base64 input bytes per printed line: 57 bytes -> 76 characters, the same
 * chunking jpeg_test uses, which this console is known to survive.
 */

#define OGG_B64_CHUNK           57

/* Lines between short pauses while dumping.
 *
 * The console is a mailbox path with a finite queue, not a UART FIFO the
 * writer can block on, so a few thousand lines pushed back to back can be
 * dropped in the middle.  A pause every few lines costs a fraction of a
 * second over the whole dump and is the difference between a file that
 * decodes and one whose CRC32 disagrees for no visible reason.
 */

/* One pause per line.
 *
 * 16 lines between pauses was not enough: the console answered a 16 KiB dump
 * with "!!some LOGs discarded!!" and 23 lines went missing.  The relay
 * queue, not the 115200 baud line, is the limit, so the pause has to be
 * frequent enough that the queue drains as fast as it fills.  At 8 ms a
 * line a 16 KiB file takes about three seconds to leave the board, which
 * is a fair price for not having to notice a hole in the middle of it.
 */

#define OGG_B64_PACE_LINES      1
#define OGG_B64_PACE_US         8000

/* Frames encoded between yields.
 *
 * Encoding is a solid CPU loop that never blocks, and NuttX schedules FIFO
 * within a priority: a task like this one starves anything at its own
 * priority until it gives the CPU up voluntarily.  One of those things is
 * the mailbox heartbeat worker, and if it misses its 2 s deadline the CP
 * asserts (mb_ipc_task:297) and the board reboots -- which is exactly what
 * a five second capture did before this yield existed.  Half a second of
 * audio per yield keeps the worker comfortably inside its deadline.
 */

#define OGG_YIELD_FRAMES        25
#define OGG_YIELD_US            2000

/* Vendor string for the comment header. */

#define OGG_VENDOR              "bk7258 audio_test"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A reusable encoder: the Opus state plus the scratch one Ogg page's worth
 * of packets is assembled in.  Held across chunks so continuous capture does
 * not allocate 100 KiB every two seconds.
 */

struct audio_test_ogg_enc_s
{
  OpusEncoder *enc;
  uint8_t *encmem;                     /* non-NULL: state is in PSRAM */
  uint8_t *packets;
  unsigned int rate;
  unsigned int bitrate;
  unsigned int frame;                  /* samples per Opus frame */
  int lookahead;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char g_ogg_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The encoded file is kept after the dump so it can be sent again.
 *
 * The console drops output under load, and a dump that arrives with a hole
 * in it is worthless.  Keeping the bytes means a retry costs one command
 * rather than another recording, which matters when the recording needs a
 * person to speak into the microphone at the right moment.
 */

static uint8_t *g_ogg_file;
static size_t g_ogg_len;
static unsigned int g_ogg_rate;
static unsigned int g_ogg_bitrate;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ogg_crc
 *
 * Description:
 *   Ogg's page checksum: CRC32 with polynomial 0x04c11db7, no reflection,
 *   zero initial value and no final inversion.  It is deliberately not the
 *   zlib CRC32 used below for the transfer check, and using one where the
 *   other belongs produces a file that every player rejects.
 *
 ****************************************************************************/

static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
  uint32_t crc = 0;
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      crc ^= (uint32_t)data[i] << 24;

      for (bit = 0; bit < 8; bit++)
        {
          if ((crc & 0x80000000u) != 0)
            {
              crc = (crc << 1) ^ 0x04c11db7u;
            }
          else
            {
              crc <<= 1;
            }
        }
    }

  return crc;
}

/****************************************************************************
 * Name: ogg_crc32_zlib
 *
 * Description:
 *   The ordinary reflected CRC32, printed alongside the dump so the host can
 *   verify the transfer with zlib.crc32() before trusting the bytes.
 *
 ****************************************************************************/

static uint32_t ogg_crc32_zlib(const uint8_t *data, size_t len)
{
  uint32_t crc = 0xffffffffu;
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      crc ^= data[i];

      for (bit = 0; bit < 8; bit++)
        {
          if ((crc & 1u) != 0)
            {
              crc = (crc >> 1) ^ 0xedb88320u;
            }
          else
            {
              crc >>= 1;
            }
        }
    }

  return crc ^ 0xffffffffu;
}

/****************************************************************************
 * Name: ogg_put16 / ogg_put32 / ogg_put64
 *
 * Description:
 *   Ogg and the Opus headers are little-endian on the wire.  Writing the
 *   bytes out one at a time rather than casting keeps this correct if the
 *   code is ever built for a big-endian target.
 *
 ****************************************************************************/

static void ogg_put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void ogg_put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void ogg_put64(uint8_t *p, uint64_t v)
{
  ogg_put32(p, (uint32_t)(v & 0xffffffffu));
  ogg_put32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

/****************************************************************************
 * Name: ogg_write_page
 *
 * Description:
 *   Append one Ogg page carrying npackets packets to out.
 *
 *   Every packet here is shorter than 255 bytes, so each needs exactly one
 *   lacing value and the segment count equals the packet count.  A packet of
 *   255 bytes or more would need continuation lacing values, which this does
 *   not emit: the caller's frame size and bitrate keep packets well under
 *   that, and the assumption is checked rather than assumed.
 *
 * Returned Value:
 *   Bytes appended, or a negated errno if the page does not fit or a packet
 *   is too long for single-byte lacing.
 *
 ****************************************************************************/

static int ogg_write_page(uint8_t *out, size_t room, uint32_t serial,
                          uint32_t seq, uint64_t granule, bool first,
                          bool last, const uint8_t **packets,
                          const uint16_t *lens, unsigned int npackets)
{
  size_t need = 27 + npackets;
  unsigned int i;
  size_t pos;
  uint8_t flags;

  for (i = 0; i < npackets; i++)
    {
      if (lens[i] >= 255)
        {
          return -EMSGSIZE;
        }

      need += lens[i];
    }

  if (need > room)
    {
      return -ENOSPC;
    }

  flags = 0;
  if (first)
    {
      flags |= 0x02;
    }

  if (last)
    {
      flags |= 0x04;
    }

  memcpy(out, "OggS", 4);
  out[4] = 0;
  out[5] = flags;
  ogg_put64(out + 6, granule);
  ogg_put32(out + 14, serial);
  ogg_put32(out + 18, seq);
  ogg_put32(out + 22, 0);              /* CRC, filled in once complete */
  out[26] = (uint8_t)npackets;

  for (i = 0; i < npackets; i++)
    {
      out[27 + i] = (uint8_t)lens[i];
    }

  pos = 27 + npackets;
  for (i = 0; i < npackets; i++)
    {
      memcpy(out + pos, packets[i], lens[i]);
      pos += lens[i];
    }

  ogg_put32(out + 22, ogg_crc(out, pos));
  return (int)pos;
}

/****************************************************************************
 * Name: ogg_dump_base64
 ****************************************************************************/

static void ogg_dump_base64(const uint8_t *data, size_t len)
{
  unsigned int lines = 0;
  size_t i;

  for (i = 0; i < len; i += OGG_B64_CHUNK)
    {
      char line[80];
      size_t n = len - i;
      size_t j;
      int p = 0;

      if (n > OGG_B64_CHUNK)
        {
          n = OGG_B64_CHUNK;
        }

      for (j = 0; j < n; j += 3)
        {
          uint32_t v = (uint32_t)data[i + j] << 16;
          size_t rem = n - j;

          if (rem > 1)
            {
              v |= (uint32_t)data[i + j + 1] << 8;
            }

          if (rem > 2)
            {
              v |= data[i + j + 2];
            }

          line[p++] = g_ogg_b64[(v >> 18) & 0x3f];
          line[p++] = g_ogg_b64[(v >> 12) & 0x3f];
          line[p++] = rem > 1 ? g_ogg_b64[(v >> 6) & 0x3f] : '=';
          line[p++] = rem > 2 ? g_ogg_b64[v & 0x3f] : '=';
        }

      line[p] = '\0';
      printf("%s\n", line);

      if (++lines % OGG_B64_PACE_LINES == 0)
        {
          usleep(OGG_B64_PACE_US);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int audio_test_send_raw(const char *host, int port, const void *data,
                        size_t len)
{
  struct sockaddr_in addr;
  const uint8_t *p = data;
  size_t sent = 0;
  int sock;
  int ret = OK;

  if (data == NULL || len == 0)
    {
      printf("audio_test: nothing captured to send\n");
      return -ENODATA;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      printf("audio_test: %s is not an IPv4 address\n", host);
      return -EINVAL;
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      printf("audio_test: socket failed: %d\n", errno);
      return -errno;
    }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      printf("audio_test: connect to %s:%d failed: %d\n", host, port, errno);
      close(sock);
      return -errno;
    }

  printf("audio_test: sending %zu bytes of raw PCM to %s:%d "
         "(crc32=0x%08lx)\n", len, host, port,
         (unsigned long)ogg_crc32_zlib(p, len));

  while (sent < len)
    {
      ssize_t n = send(sock, p + sent, len - sent, 0);

      if (n <= 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          printf("audio_test: send stalled after %zu bytes: %d\n", sent,
                 errno);
          ret = -errno;
          break;
        }

      sent += (size_t)n;
    }

  close(sock);

  if (ret == OK)
    {
      printf("audio_test: sent %zu bytes\n", sent);
    }

  return ret;
}

int audio_test_ogg_send(const char *host, int port)
{
  struct sockaddr_in addr;
  size_t sent = 0;
  int sock;
  int ret = OK;

  if (g_ogg_file == NULL || g_ogg_len == 0)
    {
      printf("audio_test: nothing encoded yet, run 'audio_test opus' "
             "first\n");
      return -ENODATA;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      printf("audio_test: %s is not an IPv4 address\n", host);
      return -EINVAL;
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      printf("audio_test: socket failed: %d\n", errno);
      return -errno;
    }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      printf("audio_test: connect to %s:%d failed: %d\n", host, port, errno);
      close(sock);
      return -errno;
    }

  printf("audio_test: connected to %s:%d, sending %zu bytes "
         "(crc32=0x%08lx)\n", host, port, g_ogg_len,
         (unsigned long)ogg_crc32_zlib(g_ogg_file, g_ogg_len));

  /* Loop: a stream socket is free to accept less than it was offered, and a
   * single write() returning short is not an error to be reported as one.
   */

  while (sent < g_ogg_len)
    {
      ssize_t n = send(sock, g_ogg_file + sent, g_ogg_len - sent, 0);

      if (n <= 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          printf("audio_test: send stalled after %zu bytes: %d\n", sent,
                 errno);
          ret = -errno;
          break;
        }

      sent += (size_t)n;
    }

  /* Close before reporting success: the receiver treats end-of-stream as
   * end-of-file, so a file that looks complete here but was never flushed
   * would be truncated there.
   */

  close(sock);

  if (ret == OK)
    {
      printf("audio_test: sent %zu bytes\n", sent);
    }

  return ret;
}

int audio_test_ogg_save(const char *path)
{
  size_t written = 0;
  int fd;

  if (g_ogg_file == NULL || g_ogg_len == 0)
    {
      printf("audio_test: nothing encoded yet\n");
      return -ENODATA;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      printf("audio_test: cannot create %s: %d\n", path, errno);
      return -errno;
    }

  /* Looped: a short write on a FAT filesystem is not an error, and treating
   * one as success would leave a truncated file that still decodes for part
   * of its length -- the kind of corruption that is noticed much later.
   */

  while (written < g_ogg_len)
    {
      ssize_t n = write(fd, g_ogg_file + written, g_ogg_len - written);

      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          printf("audio_test: write to %s stalled after %zu byte(s): %d\n",
                 path, written, errno);
          close(fd);
          return n < 0 ? -errno : -EIO;
        }

      written += (size_t)n;
    }

  close(fd);

  printf("audio_test: wrote %zu byte(s) to %s (crc32=0x%08lx)\n", written,
         path, (unsigned long)ogg_crc32_zlib(g_ogg_file, g_ogg_len));
  return OK;
}

int audio_test_ogg_redump(void)
{
  if (g_ogg_file == NULL || g_ogg_len == 0)
    {
      printf("audio_test: nothing encoded yet, run 'audio_test opus' "
             "first\n");
      return -ENODATA;
    }

  printf("audio_test: ---BEGIN OGG OPUS BASE64--- bytes=%zu rate=%u "
         "bitrate=%u crc32=0x%08lx\n", g_ogg_len, g_ogg_rate, g_ogg_bitrate,
         (unsigned long)ogg_crc32_zlib(g_ogg_file, g_ogg_len));
  ogg_dump_base64(g_ogg_file, g_ogg_len);
  printf("audio_test: ---END OGG OPUS BASE64---\n");
  return OK;
}

/****************************************************************************
 * Name: audio_test_ogg_encoder_create
 ****************************************************************************/

void *audio_test_ogg_encoder_create(unsigned int rate, unsigned int bitrate,
                                    bool prefer_psram)
{
  struct audio_test_ogg_enc_s *h;
  int encsize;
  int n;

  if (rate == 0 || rate / 1000 * OGG_FRAME_MS == 0)
    {
      return NULL;
    }

  h = calloc(1, sizeof(*h));
  if (h == NULL)
    {
      return NULL;
    }

  h->rate = rate;
  h->bitrate = bitrate;
  h->frame = rate / 1000 * OGG_FRAME_MS;

  h->packets = bk7258_psram_malloc((size_t)OGG_MAX_PACKET *
                                   OGG_PACKETS_PER_PAGE);
  if (h->packets == NULL)
    {
      free(h);
      return NULL;
    }

  /* SRAM for the state when it fits and the caller has not asked otherwise:
   * it is touched constantly while encoding and PSRAM is mapped
   * non-cacheable.  PSRAM is the fallback rather than the default for that
   * reason alone -- and the deliberate choice when the caller needs the SRAM
   * for a thread stack, which cannot live anywhere else.
   */

  encsize = opus_encoder_get_size(1);

  if (!prefer_psram)
    {
      h->enc = malloc((size_t)encsize);
    }

  if (h->enc == NULL)
    {
      h->encmem = bk7258_psram_malloc((size_t)encsize);
      h->enc = (OpusEncoder *)h->encmem;
    }

  if (h->enc == NULL)
    {
      bk7258_psram_free(h->packets);
      free(h);
      return NULL;
    }

  n = opus_encoder_init(h->enc, (opus_int32)rate, 1, OPUS_APPLICATION_VOIP);
  if (n != OPUS_OK)
    {
      printf("audio_test: opus_encoder_init failed: %d\n", n);
      audio_test_ogg_encoder_destroy(h);
      return NULL;
    }

  opus_encoder_ctl(h->enc, OPUS_SET_BITRATE((opus_int32)bitrate));
  opus_encoder_ctl(h->enc, OPUS_SET_VBR(1));
  opus_encoder_ctl(h->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(h->enc, OPUS_GET_LOOKAHEAD(&h->lookahead));

  return h;
}

/****************************************************************************
 * Name: audio_test_ogg_encoder_destroy
 ****************************************************************************/

void audio_test_ogg_encoder_destroy(void *handle)
{
  struct audio_test_ogg_enc_s *h = handle;

  if (h == NULL)
    {
      return;
    }

  if (h->encmem != NULL)
    {
      bk7258_psram_free(h->encmem);
    }
  else
    {
      free(h->enc);
    }

  if (h->packets != NULL)
    {
      bk7258_psram_free(h->packets);
    }

  free(h);
}

/****************************************************************************
 * Name: audio_test_ogg_encode
 ****************************************************************************/

int audio_test_ogg_encode(void *handle, const int16_t *pcm, size_t nsamples,
                          uint32_t serial, uint8_t *out, size_t outcap,
                          size_t *outlen)
{
  struct audio_test_ogg_enc_s *h = handle;
  const uint8_t *page_pkt[OGG_PACKETS_PER_PAGE];
  uint16_t page_len[OGG_PACKETS_PER_PAGE];
  uint8_t head[19];
  uint8_t tags[8 + 4 + sizeof(OGG_VENDOR) - 1 + 4];
  const uint8_t *pagep;
  uint16_t pagelen;
  unsigned int npage = 0;
  uint32_t seq = 0;
  uint64_t granule = 0;
  size_t len = 0;
  size_t offset = 0;
  size_t pktoff = 0;
  int n;

  if (h == NULL || pcm == NULL || nsamples == 0 || out == NULL)
    {
      return -EINVAL;
    }

  /* Each chunk is an independent stream, so the encoder must not carry
   * prediction state across the boundary: without the reset the first
   * frames of a chunk reference samples the decoder of that chunk has
   * never seen, which decodes as a click.
   */

  opus_encoder_ctl(h->enc, OPUS_RESET_STATE);

  memcpy(head, "OpusHead", 8);
  head[8] = 1;
  head[9] = 1;
  ogg_put16(head + 10,
            (uint16_t)(h->lookahead * 48000 / (int)h->rate));
  ogg_put32(head + 12, h->rate);
  ogg_put16(head + 16, 0);
  head[18] = 0;

  pagep = head;
  pagelen = sizeof(head);
  n = ogg_write_page(out + len, outcap - len, serial, seq++, 0,
                     true, false, &pagep, &pagelen, 1);
  if (n < 0)
    {
      return n;
    }

  len += n;

  memcpy(tags, "OpusTags", 8);
  ogg_put32(tags + 8, sizeof(OGG_VENDOR) - 1);
  memcpy(tags + 12, OGG_VENDOR, sizeof(OGG_VENDOR) - 1);
  ogg_put32(tags + 12 + sizeof(OGG_VENDOR) - 1, 0);

  pagep = tags;
  pagelen = sizeof(tags);
  n = ogg_write_page(out + len, outcap - len, serial, seq++, 0,
                     false, false, &pagep, &pagelen, 1);
  if (n < 0)
    {
      return n;
    }

  len += n;

  while (offset < nsamples)
    {
      int16_t tail[960];
      const int16_t *src = pcm + offset;
      size_t have = nsamples - offset;
      bool lastframe;

      if (have < h->frame)
        {
          memset(tail, 0, sizeof(tail));
          memcpy(tail, src, have * sizeof(int16_t));
          src = tail;
        }

      n = opus_encode(h->enc, src, (int)h->frame, h->packets + pktoff,
                      OGG_MAX_PACKET);
      if (n < 0)
        {
          printf("audio_test: opus_encode failed: %d\n", n);
          return -EIO;
        }

      page_pkt[npage] = h->packets + pktoff;
      page_len[npage] = (uint16_t)n;
      pktoff += (size_t)n;
      npage++;

      offset += h->frame;
      granule += (uint64_t)h->frame * 48000 / h->rate;
      lastframe = offset >= nsamples;

      /* Same reason as the dump path: this is a solid CPU loop and NuttX
       * schedules FIFO within a priority, so without a voluntary yield the
       * mailbox heartbeat worker misses its 2 s deadline and the CP
       * asserts.
       */

      if (offset / h->frame % OGG_YIELD_FRAMES == 0)
        {
          usleep(OGG_YIELD_US);
        }

      if (npage == OGG_PACKETS_PER_PAGE || lastframe)
        {
          n = ogg_write_page(out + len, outcap - len, serial, seq++,
                             granule, false, lastframe, page_pkt, page_len,
                             npage);
          if (n < 0)
            {
              return n;
            }

          len += n;
          npage = 0;
          pktoff = 0;
        }
    }

  *outlen = len;
  return OK;
}

int audio_test_ogg_opus_dump(const int16_t *pcm, size_t nsamples,
                             unsigned int rate, unsigned int bitrate,
                             const char *save_path)
{
  uint32_t serial = 0x4f505553;        /* "OPUS"; any constant will do */
  void *handle;
  uint8_t *out;
  size_t outcap;
  size_t outlen = 0;
  int ret;

  if (pcm == NULL || nsamples == 0)
    {
      return -EINVAL;
    }

  /* Room for the encoded file: the requested bitrate plus a wide margin for
   * VBR excursions and page headers.  This is PSRAM, where the cost of being
   * generous is nil, and running out halfway would waste a recording.
   */

  outcap = nsamples / rate * bitrate / 4 + 65536;
  out = bk7258_psram_malloc(outcap);
  if (out == NULL)
    {
      printf("audio_test: no PSRAM for the Ogg buffer\n");
      return -ENOMEM;
    }

  /* The one-shot path has the whole SRAM heap to itself, so it keeps the
   * faster placement.
   */

  handle = audio_test_ogg_encoder_create(rate, bitrate, false);
  if (handle == NULL)
    {
      printf("audio_test: no memory for the opus encoder\n");
      bk7258_psram_free(out);
      return -ENOMEM;
    }

  ret = audio_test_ogg_encode(handle, pcm, nsamples, serial, out, outcap,
                              &outlen);
  audio_test_ogg_encoder_destroy(handle);

  if (ret < 0)
    {
      printf("audio_test: encode failed: %d\n", ret);
      bk7258_psram_free(out);
      return ret;
    }

  /* Hand the file to the retry path before printing it, so a dump the
   * console mangles can be repeated without recording again.
   */

  if (g_ogg_file != NULL)
    {
      bk7258_psram_free(g_ogg_file);
    }

  g_ogg_file = out;
  g_ogg_len = outlen;
  g_ogg_rate = rate;
  g_ogg_bitrate = bitrate;

  /* A file and a console dump are alternatives, not both: the dump paces
   * itself at 8 ms a line to survive the mailbox relay, so printing 15 KiB
   * that has already been written to the card would add seconds of waiting
   * for output nobody is reading.
   */

  if (save_path != NULL)
    {
      return audio_test_ogg_save(save_path);
    }

  audio_test_ogg_redump();
  return OK;
}
