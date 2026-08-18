/****************************************************************************
 * app/web_tool/wt_io.h
 *
 * One byte-stream interface with two implementations, so the session loop does
 * not know whether it is talking plaintext or TLS.
 *
 * Why the board is the TLS *client* and not the server.  The board sits behind
 * the access point's NAT: measured 2026-08-18, the board reaches the
 * development machine (a 31 KB frame delivered in 91 ms, the host seeing the
 * source as the NAT address) while the development machine cannot reach the
 * board at all.  So the board has to dial out, which makes it the client, and
 * that turns out to be the better arrangement anyway:
 *
 *   - no private key and no certificate on the board, and no key generation
 *     (RSA-2048 keygen does not finish on this part -- measured);
 *   - the thing being authenticated is the development machine's console,
 *     which is exactly the party that gets to issue commands;
 *   - an IP whitelist is meaningless once the peer is NAT'd, and certificate
 *     pinning replaces it with something that still means something.
 *
 * Authentication is a pinned SHA-256 of the server certificate, held in kvdb
 * as `web.fp`.  Chain verification is deliberately not used: there is no CA
 * bundle and no RTC, so a chain check would either fail or be theatre (see
 * docs/2026-08-17-TLS性能核查.md, and note that the upstream agent works
 * around the missing clock by forcing the year to 2026, which makes validity
 * checking meaningless).  A pinned fingerprint needs neither a clock nor a CA
 * and is a real decision about who is on the other end.
 *
 * With `web.fp` unset the board prints the fingerprint it saw and refuses to
 * continue -- same rule as `web.allow`: no silent unauthenticated mode,
 * because the failure state of a security feature must not be "it looked like
 * it was on".
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_WEB_TOOL_WT_IO_H
#define __APP_WEB_TOOL_WT_IO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SHA-256 as lowercase hex, plus the terminator. */

#define WT_FP_HEX_LEN   65

/* wt_io_tls_connect() failures, so the caller can say something useful rather
 * than printing a number.
 */

#define WT_IO_OK             0
#define WT_IO_ERR_CONNECT   (-1)
#define WT_IO_ERR_HANDSHAKE (-2)
#define WT_IO_ERR_PIN       (-3)   /* fingerprint mismatch                */
#define WT_IO_ERR_NOPIN     (-4)   /* nothing pinned; fingerprint printed */
#define WT_IO_ERR_MEM       (-5)
#define WT_IO_ERR_ENTROPY   (-6)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct wt_io_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/* Wrap an already-connected socket.  Used by the loopback self-test, where
 * encryption would add nothing: both ends are this process.
 */

struct wt_io_s *wt_io_plain(int fd);

/****************************************************************************
 * Name: wt_io_tls_connect
 *
 * Description:
 *   Dial host:port, do a TLS handshake, and check the server certificate
 *   against pin_fp (lowercase hex SHA-256 of the DER).  pin_fp NULL or empty
 *   means nothing is pinned: the fingerprint that was seen is written to
 *   fp_out and WT_IO_ERR_NOPIN is returned without any application data
 *   having been exchanged.
 *
 *   fp_out always receives the fingerprint that was seen when the handshake
 *   got far enough to produce one, so a mismatch can be reported with both
 *   values.
 *
 ****************************************************************************/

struct wt_io_s *wt_io_tls_connect(const char *host, int port,
                                  const char *pin_fp,
                                  char *fp_out, size_t fp_cap,
                                  int *err);

/* Returns bytes read, 0 at end of stream, negative on error. */

int wt_io_read(struct wt_io_s *io, void *buf, size_t len);

/* Writes all of len, or returns negative.  TLS records are 16 KB here
 * (MBEDTLS_SSL_OUT_CONTENT_LEN), so a 27 KB frame is several records and the
 * partial writes that implies are handled inside.
 */

int wt_io_write(struct wt_io_s *io, const void *buf, size_t len);

/* The underlying descriptor, for poll(POLLOUT) before writing a frame.
 * Writability of the socket is the right question even under TLS: the record
 * layer has no buffer of its own to fill.
 */

int wt_io_fd(struct wt_io_s *io);

/* Wake a reader parked in read().  Does not free anything. */

void wt_io_shutdown(struct wt_io_s *io);

void wt_io_close(struct wt_io_s *io);

/* Which transport this is, for the log line that says so. */

const char *wt_io_describe(struct wt_io_s *io);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEB_TOOL_WT_IO_H */
