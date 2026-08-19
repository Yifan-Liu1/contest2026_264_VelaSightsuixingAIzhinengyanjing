/****************************************************************************
 * app/conv/conv_ws.h
 *
 * A WebSocket client, cut down to what this board needs.
 *
 * Why not a library.  apps/netutils has two.  cwebsocket downloads its source
 * from GitHub during the build (curl codeload.github.com), so a build becomes
 * dependent on that host being reachable -- a poor trade for a few hundred
 * lines.  libwebsockets is vendored but is 1562 files of a general-purpose
 * implementation, and this firmware is already at 64% of flash.  What is
 * actually required here is one connection to one known server carrying small
 * JSON text frames, which is a small corner of RFC 6455.
 *
 * What is implemented:
 *
 *   - the HTTP Upgrade handshake, and a check for "101"
 *   - text frames in and out, with the client-to-server masking the spec
 *     requires (an unmasked client frame is a protocol error the server is
 *     entitled to close the connection over)
 *   - both length forms that matter here: 7-bit, and 16-bit (126)
 *   - ping answered with pong, close answered with close
 *
 * What is deliberately not:
 *
 *   - Sec-WebSocket-Accept is not verified.  Doing so means SHA-1 purely to
 *     confirm the server echoed a hash of a key this client just invented.
 *     Its purpose in the spec is to stop a cache or a non-WebSocket server
 *     from being mistaken for one; here the server is known and the payload
 *     is checked JSON, so a wrong peer fails at the first message anyway.
 *   - 64-bit lengths (127) are rejected.  The server's own limit is 16 KB per
 *     frame, so a frame that needs more than 16 bits of length is a bug on
 *     one side or the other, and quietly accepting it would hide that.
 *   - continuation frames are rejected for the same reason: the server sends
 *     each JSON message whole.
 *   - no TLS.  The audio path has the same gap and both should be closed
 *     together, once the endpoint and its authentication are settled.
 *
 * Each limit is checked and reported rather than assumed, so if one of them
 * is ever wrong the symptom is a message saying so.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_CONV_CONV_WS_H
#define __APP_CONV_CONV_WS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Matches the server's MAX_MESSAGE_BYTES.  Sized to agree deliberately: a
 * client that would send more than the peer accepts fails at the far end,
 * where the error is harder to read.
 */

#define CONV_WS_MAX_MSG   (16 * 1024)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct conv_ws_s
{
  int sock;
  bool connected;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: conv_ws_connect
 *
 * Description:
 *   Open a connection and complete the WebSocket handshake against
 *   host:port, path /ws.
 *
 * Returned Value:
 *   Zero on success, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_ws_connect(struct conv_ws_s *ws, const char *host, int port);

/****************************************************************************
 * Name: conv_ws_close
 ****************************************************************************/

void conv_ws_close(struct conv_ws_s *ws);

/****************************************************************************
 * Name: conv_ws_send_text
 *
 * Description:
 *   Send one masked text frame.
 *
 * Returned Value:
 *   Zero on success, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_ws_send_text(struct conv_ws_s *ws, const char *text);

/****************************************************************************
 * Name: conv_ws_recv_text
 *
 * Description:
 *   Wait for one text frame, answering ping and close along the way.
 *
 * Input Parameters:
 *   ws         - connected client
 *   buf        - destination, NUL-terminated on success
 *   len        - bytes available at buf
 *   timeout_ms - how long to wait, 0 to block
 *
 * Returned Value:
 *   Payload length, 0 on timeout, or a negated errno.  -ENOTCONN means the
 *   peer closed.
 *
 ****************************************************************************/

int conv_ws_recv_text(struct conv_ws_s *ws, char *buf, size_t len,
                      int timeout_ms);

/****************************************************************************
 * Name: conv_serve
 *
 * Description:
 *   Register with the console, set the clock from its acknowledgement, then
 *   answer forwarded queries until the connection drops.
 *
 *   Runs in the foreground of whichever task calls it -- from NSH that means
 *   the shell is occupied until Ctrl-C.  Deliberate for now: a resident
 *   daemon would need a way to be stopped and inspected, and neither is worth
 *   building before the message set has settled with the console.
 *
 * Returned Value:
 *   Zero when the peer closed cleanly, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_serve(const char *host, int port);

#endif /* __APP_CONV_CONV_WS_H */
