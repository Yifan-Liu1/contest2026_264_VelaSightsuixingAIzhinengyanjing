/****************************************************************************
 * app/web_tool/wt_protocol.c
 *
 * Frame encode/decode.  No NuttX headers on purpose -- see wt_protocol.h and
 * host/tests/test_wt_protocol.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "wt_protocol.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint16_t wt_rd16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t wt_rd32(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int wt_type_is_known(uint8_t type)
{
  switch (type)
    {
      case WT_TYPE_REQ:
      case WT_TYPE_RSP:
      case WT_TYPE_EVT_LOG:
      case WT_TYPE_EVT_FRAME:
      case WT_TYPE_PING:
      case WT_TYPE_PONG:
      case WT_TYPE_HELLO:
        return 1;

      default:
        return 0;
    }
}

int wt_hdr_encode(uint8_t *out, size_t outlen, uint8_t type,
                  uint16_t req_id, uint32_t len)
{
  if (out == NULL || outlen < WT_HDR_LEN)
    {
      return -1;
    }

  if (len > WT_MAX_PAYLOAD || !wt_type_is_known(type))
    {
      return -1;
    }

  out[0] = type;
  out[1] = 0;                          /* flags, reserved */
  out[2] = (uint8_t)(req_id & 0xff);
  out[3] = (uint8_t)((req_id >> 8) & 0xff);
  out[4] = (uint8_t)(len & 0xff);
  out[5] = (uint8_t)((len >> 8) & 0xff);
  out[6] = (uint8_t)((len >> 16) & 0xff);
  out[7] = (uint8_t)((len >> 24) & 0xff);

  return WT_HDR_LEN;
}

int wt_frame_meta_encode(uint8_t *out, size_t outlen, uint32_t seq,
                         uint32_t fnv1a)
{
  if (out == NULL || outlen < WT_FRAME_META_LEN)
    {
      return -1;
    }

  out[0] = (uint8_t)(seq & 0xff);
  out[1] = (uint8_t)((seq >> 8) & 0xff);
  out[2] = (uint8_t)((seq >> 16) & 0xff);
  out[3] = (uint8_t)((seq >> 24) & 0xff);
  out[4] = (uint8_t)(fnv1a & 0xff);
  out[5] = (uint8_t)((fnv1a >> 8) & 0xff);
  out[6] = (uint8_t)((fnv1a >> 16) & 0xff);
  out[7] = (uint8_t)((fnv1a >> 24) & 0xff);

  return WT_FRAME_META_LEN;
}

uint32_t wt_fnv1a(const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t hash = 0x811c9dc5u;
  size_t i;

  for (i = 0; i < len; i++)
    {
      hash ^= p[i];
      hash *= 16777619u;
    }

  return hash;
}

void wt_parser_init(struct wt_parser_s *p, uint8_t *buf, size_t bufcap)
{
  memset(p, 0, sizeof(*p));
  p->buf    = buf;
  p->bufcap = bufcap;
}

void wt_parser_reset(struct wt_parser_s *p)
{
  p->hdr_got     = 0;
  p->payload_got = 0;
  p->paylen      = 0;
  p->payload     = NULL;
  p->type        = 0;
  p->flags       = 0;
  p->req_id      = 0;
}

int wt_parser_push(struct wt_parser_s *p, const uint8_t *data, size_t len,
                   size_t *consumed)
{
  size_t take;

  if (consumed != NULL)
    {
      *consumed = 0;
    }

  if (p->poisoned)
    {
      /* Framing was lost earlier.  Report the same failure for ever; the
       * connection has to be dropped, and pretending to resync would produce
       * frames assembled from the middle of a payload.
       */

      return WT_PARSE_ERR_LEN;
    }

  /* Phase 1: header. */

  if (p->hdr_got < WT_HDR_LEN)
    {
      take = WT_HDR_LEN - p->hdr_got;
      if (take > len)
        {
          take = len;
        }

      memcpy(p->hdr + p->hdr_got, data, take);
      p->hdr_got += take;

      if (consumed != NULL)
        {
          *consumed = take;
        }

      if (p->hdr_got < WT_HDR_LEN)
        {
          return WT_PARSE_MORE;
        }

      p->type   = p->hdr[0];
      p->req_id = wt_rd16(p->hdr + 2);
      p->paylen = wt_rd32(p->hdr + 4);

      /* Order matters: an unknown type is reported even when the length is
       * also nonsense, because the type is what tells the operator which
       * side has the wrong idea of the protocol.
       */

      if (!wt_type_is_known(p->type))
        {
          p->poisoned = 1;
          return WT_PARSE_ERR_TYPE;
        }

      if (p->paylen > WT_MAX_PAYLOAD)
        {
          p->poisoned = 1;
          return WT_PARSE_ERR_LEN;
        }

      if (p->paylen > p->bufcap)
        {
          p->poisoned = 1;
          return WT_PARSE_ERR_CAP;
        }

      p->payload_got = 0;

      if (p->paylen == 0)
        {
          p->payload = p->buf;
          p->hdr_got = 0;              /* ready for the next header */
          return WT_PARSE_FRAME;
        }

      /* Whatever is left in this buffer belongs to the payload; the caller
       * loops and comes back in below.
       */

      return WT_PARSE_MORE;
    }

  /* Phase 2: payload. */

  take = p->paylen - p->payload_got;
  if (take > len)
    {
      take = len;
    }

  if (take > 0)
    {
      memcpy(p->buf + p->payload_got, data, take);
      p->payload_got += take;

      if (consumed != NULL)
        {
          *consumed = take;
        }
    }

  if (p->payload_got < p->paylen)
    {
      return WT_PARSE_MORE;
    }

  p->payload = p->buf;
  p->hdr_got = 0;
  return WT_PARSE_FRAME;
}
