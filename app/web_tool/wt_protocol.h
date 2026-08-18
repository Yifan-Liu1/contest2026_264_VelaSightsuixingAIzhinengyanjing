/****************************************************************************
 * app/web_tool/wt_protocol.h
 *
 * Frame encode/decode for the web_tool link.  Deliberately free of any NuttX
 * dependency: the same .c is compiled on the host by host/tests/Makefile, so
 * the framing edge cases (half a header, a payload split across recv calls,
 * several frames in one recv) are covered by unit tests instead of by staring
 * at a board.
 *
 * Wire format -- one TCP byte stream carries command responses, log lines and
 * JPEG frames at the same time, so it has to be self-delimiting:
 *
 *   offset  size  field
 *   0       1     type
 *   1       1     flags     (reserved, 0)
 *   2       2     req_id    little endian; 0 on server-initiated pushes
 *   4       4     len       little endian, payload bytes
 *   8       len   payload
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_WEB_TOOL_WT_PROTOCOL_H
#define __APP_WEB_TOOL_WT_PROTOCOL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WT_HDR_LEN            8

/* 64 KB.  A single JPEG measures 27-33 KB on this board, so this is roughly
 * double the largest payload anyone should send.  A frame that claims more is
 * treated as a broken peer and drops the connection rather than being
 * truncated: silent truncation turns an implementation bug into a data bug,
 * which is much harder to find.
 */

#define WT_MAX_PAYLOAD        (64 * 1024)

/* Frame types. */

#define WT_TYPE_REQ           0x01  /* JSON request, host -> board          */
#define WT_TYPE_RSP           0x02  /* JSON response, board -> host         */
#define WT_TYPE_EVT_LOG       0x03  /* JSON log event, board -> host        */
#define WT_TYPE_EVT_FRAME     0x04  /* seq + fnv1a + JPEG, board -> host    */
#define WT_TYPE_PING          0x05  /* empty payload                        */
#define WT_TYPE_PONG          0x06  /* empty payload                        */

/* Sent by the board as the first frame after the TLS handshake, when it is the
 * one that dialled out.  Carries the shared token from kvdb `web.token`.
 *
 * It exists because TLS here authenticates only the console: the board has no
 * certificate, so anything that can reach the console's port can complete a
 * handshake and pretend to be a board.  That matters more than it sounds --
 * an operator who types an API key into the page would be handing it to
 * whoever is on the other end.  The token is what makes the console willing to
 * believe the peer is the board.
 */

#define WT_TYPE_HELLO         0x07  /* JSON, board -> console               */

/* EVT_FRAME payload prefix:
 *
 *   offset  size  field
 *   0       4     seq     little endian
 *   4       4     fnv1a   little endian, over the JPEG bytes only
 *   8       ...   JPEG
 *
 * The FNV-1a is redundant over TCP.  It is kept because tools/b64frames.py
 * already computes the same hash for the serial route, which does lose bytes,
 * and one checksum shared by both routes is cheaper to reason about than two
 * different integrity stories.  Cost is 8 bytes per frame.
 */

#define WT_FRAME_META_LEN     8

/* wt_parser_push() return values. */

#define WT_PARSE_MORE         0   /* need more bytes                       */
#define WT_PARSE_FRAME        1   /* p->type/req_id/payload/paylen are set */
#define WT_PARSE_ERR_LEN      (-1) /* len exceeds WT_MAX_PAYLOAD           */
#define WT_PARSE_ERR_TYPE     (-2) /* type is not one of the seven above     */
#define WT_PARSE_ERR_CAP      (-3) /* len exceeds the buffer given to init */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Streaming decoder.  Holds no buffer of its own: the payload lands in
 * storage the caller supplies, so the board can allocate one 64 KB block for
 * the lifetime of a connection and the tests can use a stack array.
 */

struct wt_parser_s
{
  uint8_t  hdr[WT_HDR_LEN];
  size_t   hdr_got;      /* header bytes accumulated so far    */
  uint8_t *buf;          /* caller-owned payload buffer        */
  size_t   bufcap;
  size_t   payload_got;  /* payload bytes accumulated so far   */
  int      poisoned;     /* framing lost; every push now fails */

  /* Valid only while wt_parser_push() has just returned WT_PARSE_FRAME. */

  uint8_t  type;
  uint8_t  flags;
  uint16_t req_id;
  uint32_t paylen;
  uint8_t *payload;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: wt_hdr_encode
 *
 * Description:
 *   Write the 8-byte header for a frame into out.  Returns WT_HDR_LEN, or a
 *   negative value when out is too small or len exceeds WT_MAX_PAYLOAD.
 *
 ****************************************************************************/

int wt_hdr_encode(uint8_t *out, size_t outlen, uint8_t type,
                  uint16_t req_id, uint32_t len);

/****************************************************************************
 * Name: wt_type_is_known
 *
 * Description:
 *   True for the seven types above.  Split out so both the parser and the
 *   command layer agree on what "unknown" means.
 *
 ****************************************************************************/

int wt_type_is_known(uint8_t type);

/****************************************************************************
 * Name: wt_parser_init / wt_parser_reset
 *
 * Description:
 *   buf/bufcap is where payloads are assembled.  bufcap smaller than the
 *   largest frame the peer may send makes wt_parser_push() return
 *   WT_PARSE_ERR_CAP for those frames instead of overflowing.
 *
 ****************************************************************************/

void wt_parser_init(struct wt_parser_s *p, uint8_t *buf, size_t bufcap);
void wt_parser_reset(struct wt_parser_s *p);

/****************************************************************************
 * Name: wt_parser_push
 *
 * Description:
 *   Feed up to len bytes.  On return *consumed says how many were taken;
 *   the caller must loop while bytes remain, because one recv() can hold
 *   several frames.
 *
 *   Returns WT_PARSE_FRAME when a whole frame is available (fields on p),
 *   WT_PARSE_MORE when more bytes are needed, or one of the negative codes.
 *   After a negative return the parser is poisoned: the byte stream has lost
 *   its framing and the only correct recovery is to drop the connection.
 *
 ****************************************************************************/

int wt_parser_push(struct wt_parser_s *p, const uint8_t *data, size_t len,
                   size_t *consumed);

/****************************************************************************
 * Name: wt_fnv1a
 *
 * Description:
 *   FNV-1a, 32 bit, same constants as tools/b64frames.py.
 *
 ****************************************************************************/

uint32_t wt_fnv1a(const void *data, size_t len);

/****************************************************************************
 * Name: wt_frame_meta_encode
 *
 * Description:
 *   Write the 8-byte EVT_FRAME prefix (seq, fnv1a) into out.  Returns
 *   WT_FRAME_META_LEN or a negative value when out is too small.
 *
 ****************************************************************************/

int wt_frame_meta_encode(uint8_t *out, size_t outlen, uint32_t seq,
                         uint32_t fnv1a);

/****************************************************************************
 * Name: wt_rd16 / wt_rd32
 *
 * Description:
 *   Little-endian reads that do not assume the host's byte order or that the
 *   pointer is aligned.
 *
 ****************************************************************************/

uint16_t wt_rd16(const uint8_t *p);
uint32_t wt_rd32(const uint8_t *p);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEB_TOOL_WT_PROTOCOL_H */
