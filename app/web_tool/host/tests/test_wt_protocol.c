/****************************************************************************
 * app/web_tool/host/tests/test_wt_protocol.c
 *
 * Host unit tests for wt_protocol.c.  The point of keeping the framing in a
 * NuttX-free .c is that these cases can be provoked here instead of on the
 * board, where "the picture is broken" and "the transfer is broken" look the
 * same.  The list comes from the design doc, section 11:
 *
 *   - half a frame header
 *   - payload arriving across several recv() calls
 *   - several complete frames in one recv()
 *   - len that does not match what is actually delivered
 *   - len over the limit
 *   - unknown type
 *
 * Build and run: make -C app/web_tool/host/tests
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_protocol.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_checks;
static int g_failures;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#define CHECK(cond, ...)                                    \
  do                                                        \
    {                                                       \
      g_checks++;                                           \
      if (!(cond))                                          \
        {                                                   \
          g_failures++;                                     \
          printf("  FAIL %s:%d: ", __func__, __LINE__);     \
          printf(__VA_ARGS__);                              \
          printf("\n");                                     \
        }                                                    \
    }                                                       \
  while (0)

/* Push the whole buffer, collecting every frame the parser emits.  This is
 * the loop a real reader has to write, so testing through it also tests that
 * the *consumed contract is usable.
 */

struct collected_s
{
  uint8_t  type;
  uint16_t req_id;
  uint32_t paylen;
  uint8_t  first;      /* payload[0], or 0 for an empty payload */
  uint8_t  last;
};

static int feed(struct wt_parser_s *p, const uint8_t *data, size_t len,
                struct collected_s *out, int outmax, int *outn)
{
  size_t off = 0;

  while (off < len)
    {
      size_t consumed = 0;
      int ret = wt_parser_push(p, data + off, len - off, &consumed);

      off += consumed;

      if (ret == WT_PARSE_FRAME)
        {
          if (*outn < outmax)
            {
              struct collected_s *c = &out[*outn];

              c->type   = p->type;
              c->req_id = p->req_id;
              c->paylen = p->paylen;
              c->first  = p->paylen > 0 ? p->payload[0] : 0;
              c->last   = p->paylen > 0 ? p->payload[p->paylen - 1] : 0;
              (*outn)++;
            }

          continue;
        }

      if (ret < 0)
        {
          return ret;
        }

      /* WT_PARSE_MORE with nothing consumed would spin for ever; the parser
       * must never do that when input is available.
       */

      if (consumed == 0)
        {
          printf("  FAIL %s: parser consumed 0 bytes with %zu available\n",
                 __func__, len - off);
          g_failures++;
          return WT_PARSE_MORE;
        }
    }

  return WT_PARSE_MORE;
}

/* Build one frame into buf; returns total length. */

static size_t build(uint8_t *buf, uint8_t type, uint16_t req_id,
                    const void *payload, uint32_t paylen)
{
  int n = wt_hdr_encode(buf, WT_HDR_LEN, type, req_id, paylen);

  if (n != WT_HDR_LEN)
    {
      abort();
    }

  if (paylen > 0)
    {
      memcpy(buf + WT_HDR_LEN, payload, paylen);
    }

  return WT_HDR_LEN + paylen;
}

/* ---- 1. Header encode / decode round trip ------------------------------ */

static void test_hdr_roundtrip(void)
{
  uint8_t hdr[WT_HDR_LEN];

  CHECK(wt_hdr_encode(hdr, sizeof(hdr), WT_TYPE_RSP, 0x1234, 0x0000ABCD)
        == WT_HDR_LEN, "encode should return 8");

  CHECK(hdr[0] == WT_TYPE_RSP, "type byte");
  CHECK(hdr[1] == 0, "flags must be zero");
  CHECK(wt_rd16(hdr + 2) == 0x1234, "req_id little endian");
  CHECK(wt_rd32(hdr + 4) == 0x0000ABCDu, "len little endian");

  /* Byte order spelled out, not just round-tripped: a decoder on the other
   * end is written from the table in the design doc, not from this code.
   */

  CHECK(hdr[2] == 0x34 && hdr[3] == 0x12, "req_id LE byte order");
  CHECK(hdr[4] == 0xCD && hdr[5] == 0xAB && hdr[6] == 0x00 && hdr[7] == 0x00,
        "len LE byte order");

  CHECK(wt_hdr_encode(hdr, WT_HDR_LEN - 1, WT_TYPE_RSP, 1, 0) < 0,
        "short out buffer must fail");
  CHECK(wt_hdr_encode(hdr, sizeof(hdr), WT_TYPE_RSP, 1, WT_MAX_PAYLOAD + 1)
        < 0, "oversized len must fail at encode time too");
}

/* ---- 2. Half a header, then the rest ---------------------------------- */

static void test_split_header(void)
{
  uint8_t wire[WT_HDR_LEN + 4];
  struct wt_parser_s p;
  uint8_t pbuf[64];
  struct collected_s got[4];
  int n = 0;
  size_t total = build(wire, WT_TYPE_REQ, 7, "abcd", 4);

  wt_parser_init(&p, pbuf, sizeof(pbuf));

  /* Four bytes: not even the length field is complete. */

  feed(&p, wire, 4, got, 4, &n);
  CHECK(n == 0, "no frame from half a header, got %d", n);

  /* One byte at a time for the remainder -- the worst case a TCP stack can
   * hand a reader.
   */

  for (size_t i = 4; i < total; i++)
    {
      feed(&p, wire + i, 1, got, 4, &n);
    }

  CHECK(n == 1, "one frame after the last byte, got %d", n);
  if (n == 1)
    {
      CHECK(got[0].type == WT_TYPE_REQ, "type survived the split");
      CHECK(got[0].req_id == 7, "req_id survived the split");
      CHECK(got[0].paylen == 4, "paylen %u", got[0].paylen);
      CHECK(got[0].first == 'a' && got[0].last == 'd', "payload contents");
    }
}

/* ---- 3. Payload spanning several recv() calls ------------------------- */

static void test_split_payload(void)
{
  const uint32_t paylen = 30000;      /* a plausible JPEG */
  uint8_t *wire = malloc(WT_HDR_LEN + paylen);
  uint8_t *pay  = malloc(paylen);
  uint8_t *pbuf = malloc(WT_MAX_PAYLOAD);
  struct wt_parser_s p;
  struct collected_s got[4];
  int n = 0;
  size_t total;
  size_t off;

  for (uint32_t i = 0; i < paylen; i++)
    {
      pay[i] = (uint8_t)(i & 0xff);
    }

  total = build(wire, WT_TYPE_EVT_FRAME, 0, pay, paylen);
  wt_parser_init(&p, pbuf, WT_MAX_PAYLOAD);

  /* 1460-byte slices: one Ethernet MSS, which is what actually comes out of
   * recv() on this link (CONFIG_NET_ETH_PKTSIZE=1514).
   */

  for (off = 0; off < total; off += 1460)
    {
      size_t chunk = total - off < 1460 ? total - off : 1460;

      feed(&p, wire + off, chunk, got, 4, &n);

      if (off + chunk < total)
        {
          CHECK(n == 0, "frame emitted early at offset %zu", off);
        }
    }

  CHECK(n == 1, "exactly one frame, got %d", n);
  if (n == 1)
    {
      CHECK(got[0].paylen == paylen, "paylen %u", got[0].paylen);
      CHECK(got[0].first == 0, "payload[0]");
      CHECK(got[0].last == (uint8_t)((paylen - 1) & 0xff), "payload[last]");
    }

  free(wire);
  free(pay);
  free(pbuf);
}

/* ---- 4. Several complete frames in one recv() ------------------------- */

static void test_multiple_frames_one_recv(void)
{
  uint8_t wire[256];
  size_t off = 0;
  struct wt_parser_s p;
  uint8_t pbuf[64];
  struct collected_s got[8];
  int n = 0;

  off += build(wire + off, WT_TYPE_RSP,     1, "one",   3);
  off += build(wire + off, WT_TYPE_EVT_LOG, 0, "two",   3);
  off += build(wire + off, WT_TYPE_PING,    0, NULL,    0);
  off += build(wire + off, WT_TYPE_PONG,    0, NULL,    0);
  off += build(wire + off, WT_TYPE_RSP,     2, "three", 5);

  wt_parser_init(&p, pbuf, sizeof(pbuf));
  feed(&p, wire, off, got, 8, &n);

  CHECK(n == 5, "five frames from one push, got %d", n);
  if (n == 5)
    {
      CHECK(got[0].type == WT_TYPE_RSP && got[0].req_id == 1, "frame 0");
      CHECK(got[1].type == WT_TYPE_EVT_LOG, "frame 1");
      CHECK(got[2].type == WT_TYPE_PING && got[2].paylen == 0,
            "empty PING payload");
      CHECK(got[3].type == WT_TYPE_PONG && got[3].paylen == 0,
            "empty PONG payload");
      CHECK(got[4].type == WT_TYPE_RSP && got[4].req_id == 2
            && got[4].paylen == 5, "frame 4");
    }
}

/* ---- 5. len larger than what is actually delivered -------------------- */

static void test_len_mismatch(void)
{
  uint8_t wire[WT_HDR_LEN + 10];
  struct wt_parser_s p;
  uint8_t pbuf[64];
  struct collected_s got[4];
  int n = 0;

  /* Declare 10, hand over 9.  The stream is now permanently out of step, and
   * the only honest answer is "no frame here" -- never a short frame.  A
   * decoder that returned the 9 bytes as a complete frame would resync on
   * the next payload byte and every later frame would be garbage.
   */

  build(wire, WT_TYPE_RSP, 3, "0123456789", 10);
  wt_parser_init(&p, pbuf, sizeof(pbuf));
  feed(&p, wire, 0, got, 4, &n);      /* len == 0 must be a clean no-op */
  CHECK(n == 0, "empty push produced a frame");
  feed(&p, wire, WT_HDR_LEN + 9, got, 4, &n);

  CHECK(n == 0, "declared 10 delivered 9 must not emit a frame, got %d", n);

  /* The 10th byte finishes it, proving nothing was lost or double counted. */

  feed(&p, wire + WT_HDR_LEN + 9, 1, got, 4, &n);
  CHECK(n == 1, "frame completes on the last byte, got %d", n);
  if (n == 1)
    {
      CHECK(got[0].paylen == 10, "paylen %u", got[0].paylen);
      CHECK(got[0].last == '9', "last payload byte");
    }
}

/* ---- 6. len over the limit -------------------------------------------- */

static void test_len_over_limit(void)
{
  uint8_t hdr[WT_HDR_LEN];
  struct wt_parser_s p;
  uint8_t pbuf[64];
  size_t consumed = 0;
  int ret;

  /* Built by hand: wt_hdr_encode() refuses to produce this, which is the
   * point -- only a broken or hostile peer sends it.
   */

  hdr[0] = WT_TYPE_EVT_FRAME;
  hdr[1] = 0;
  hdr[2] = 0;
  hdr[3] = 0;
  hdr[4] = 0x01;
  hdr[5] = 0x00;
  hdr[6] = 0x01;
  hdr[7] = 0x00;                      /* 0x00010001 = 64 KB + 1 */

  wt_parser_init(&p, pbuf, sizeof(pbuf));
  ret = wt_parser_push(&p, hdr, sizeof(hdr), &consumed);

  CHECK(ret == WT_PARSE_ERR_LEN, "expected ERR_LEN, got %d", ret);

  /* Poisoned: further pushes must keep failing rather than silently
   * resyncing, so a caller that ignores the first error cannot end up
   * handing garbage to the command layer.
   */

  consumed = 0;
  ret = wt_parser_push(&p, (const uint8_t *)"xxxx", 4, &consumed);
  CHECK(ret < 0, "parser must stay poisoned, got %d", ret);
}

/* ---- 7. Unknown type -------------------------------------------------- */

static void test_unknown_type(void)
{
  uint8_t hdr[WT_HDR_LEN];
  struct wt_parser_s p;
  uint8_t pbuf[64];
  size_t consumed = 0;
  int ret;

  hdr[0] = 0x42;                      /* not one of the seven */
  hdr[1] = 0;
  hdr[2] = 0;
  hdr[3] = 0;
  hdr[4] = 0;
  hdr[5] = 0;
  hdr[6] = 0;
  hdr[7] = 0;

  wt_parser_init(&p, pbuf, sizeof(pbuf));
  ret = wt_parser_push(&p, hdr, sizeof(hdr), &consumed);

  CHECK(ret == WT_PARSE_ERR_TYPE, "expected ERR_TYPE, got %d", ret);

  CHECK(wt_type_is_known(WT_TYPE_REQ), "REQ is known");
  CHECK(wt_type_is_known(WT_TYPE_PONG), "PONG is known");
  CHECK(!wt_type_is_known(0x00), "0x00 is not a type");
  CHECK(wt_type_is_known(WT_TYPE_HELLO), "HELLO is known");
  CHECK(!wt_type_is_known(0x08), "0x08 is not a type");
}

/* ---- 8. Payload bigger than the buffer the caller gave --------------- */

static void test_over_capacity(void)
{
  uint8_t hdr[WT_HDR_LEN];
  struct wt_parser_s p;
  uint8_t pbuf[16];
  size_t consumed = 0;
  int ret;

  /* Legal on the wire (under 64 KB) but larger than this parser can hold.
   * Reported as a distinct code: it is a local sizing mistake, not a peer
   * that is speaking the protocol wrongly, and conflating the two would send
   * someone looking at the wrong end of the link.
   */

  wt_hdr_encode(hdr, sizeof(hdr), WT_TYPE_RSP, 1, 32);
  wt_parser_init(&p, pbuf, sizeof(pbuf));
  ret = wt_parser_push(&p, hdr, sizeof(hdr), &consumed);

  CHECK(ret == WT_PARSE_ERR_CAP, "expected ERR_CAP, got %d", ret);
}

/* ---- 9. Maximum legal payload ---------------------------------------- */

static void test_max_payload(void)
{
  uint8_t *wire = malloc(WT_HDR_LEN + WT_MAX_PAYLOAD);
  uint8_t *pbuf = malloc(WT_MAX_PAYLOAD);
  uint8_t *pay  = malloc(WT_MAX_PAYLOAD);
  struct wt_parser_s p;
  struct collected_s got[2];
  int n = 0;
  size_t total;

  memset(pay, 0x5a, WT_MAX_PAYLOAD);
  pay[WT_MAX_PAYLOAD - 1] = 0xa5;

  total = build(wire, WT_TYPE_EVT_FRAME, 0, pay, WT_MAX_PAYLOAD);
  wt_parser_init(&p, pbuf, WT_MAX_PAYLOAD);
  feed(&p, wire, total, got, 2, &n);

  CHECK(n == 1, "exactly-64KB payload accepted, got %d frames", n);
  if (n == 1)
    {
      CHECK(got[0].paylen == WT_MAX_PAYLOAD, "paylen %u", got[0].paylen);
      CHECK(got[0].last == 0xa5, "last byte");
    }

  free(wire);
  free(pbuf);
  free(pay);
}

/* ---- 10. Frame metadata and FNV-1a ----------------------------------- */

static void test_frame_meta(void)
{
  uint8_t meta[WT_FRAME_META_LEN];

  CHECK(wt_frame_meta_encode(meta, sizeof(meta), 0x01020304, 0xDEADBEEF)
        == WT_FRAME_META_LEN, "meta encode length");
  CHECK(wt_rd32(meta) == 0x01020304u, "seq round trip");
  CHECK(wt_rd32(meta + 4) == 0xDEADBEEFu, "fnv1a round trip");
  CHECK(wt_frame_meta_encode(meta, 7, 1, 2) < 0, "short buffer refused");

  /* Reference values: FNV-1a 32 bit is fully specified, and tools/
   * b64frames.py computes the same thing, so these constants pin both.
   */

  CHECK(wt_fnv1a("", 0) == 0x811C9DC5u, "empty string basis");
  CHECK(wt_fnv1a("a", 1) == 0xE40C292Cu, "\"a\"");
  CHECK(wt_fnv1a("foobar", 6) == 0xBF9CF968u, "\"foobar\"");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  printf("wt_protocol unit tests\n");

  test_hdr_roundtrip();
  test_split_header();
  test_split_payload();
  test_multiple_frames_one_recv();
  test_len_mismatch();
  test_len_over_limit();
  test_unknown_type();
  test_over_capacity();
  test_max_payload();
  test_frame_meta();

  printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
