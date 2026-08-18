/****************************************************************************
 * app/web_tool/wt_io.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"

#include "wt_io.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WT_IO_PLAIN  0
#define WT_IO_TLS    1

/* Read timeout on the transport.  The session loop treats a timeout as "keep
 * waiting", so this only bounds how long a close goes unnoticed.
 */

#define WT_IO_RECV_TIMEOUT_S  30
#define WT_IO_SEND_TIMEOUT_S  10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wt_io_s
{
  int kind;
  int fd;                              /* plain: the socket; TLS: net.fd */

  /* TLS state.  Only touched when kind == WT_IO_TLS. */

  mbedtls_ssl_context      ssl;
  mbedtls_ssl_config       cfg;
  mbedtls_net_context      net;
  mbedtls_ctr_drbg_context drbg;
  bool                     tls_up;
  char                     desc[64];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Ciphersuite preference, and the one place in this project where the TLS
 * measurements in docs/local/2026-08-17-TLS性能核查.md are actually acted on.
 *
 * ECDHE first because the key exchange is what costs: a full handshake on this
 * part measured 541 ms with DHE-2048, of which at least ~460 ms was
 * computation, and an ECDHE-P256 point multiply is an order of magnitude
 * cheaper than a 2048-bit modular exponentiation.
 *
 * ChaCha20-Poly1305 ahead of AES-GCM because on this chip AES-GCM is *slower*,
 * not faster: measured 315 KiB/s for AES-GCM-256 against 1575 KiB/s for
 * ChaCha20-Poly1305, because GHASH is software here and MBEDTLS_AESCE_C is an
 * ARMv8-A feature this ARMv8-M core does not have.  Choosing "the modern AEAD"
 * without measuring would have cost a factor of five on every frame.
 */

static const int g_wt_ciphersuites[] =
{
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
  0
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Entropy straight from the kernel pool.  /dev/urandom exists in this
 * configuration and bring-up seeds the pool from the hardware TRNG
 * (bk7258_trng.c: 32 words), so this is not interrupt-timing jitter.
 *
 * Failure returns an error rather than falling back to anything weaker: a TLS
 * handshake with predictable key material is worse than no handshake, because
 * it still looks encrypted.
 */

static int wt_io_entropy(void *ctx, unsigned char *out, size_t len)
{
  int fd;
  size_t got = 0;

  UNUSED(ctx);

  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    {
      fd = open("/dev/random", O_RDONLY);
    }

  if (fd < 0)
    {
      syslog(LOG_ERR, "web_tool: no entropy source (/dev/urandom missing)\n");
      return -1;
    }

  while (got < len)
    {
      ssize_t n = read(fd, out + got, len - got);

      if (n <= 0)
        {
          close(fd);
          return -1;
        }

      got += (size_t)n;
    }

  close(fd);
  return 0;
}

static void wt_io_hex(const unsigned char *in, size_t len, char *out)
{
  static const char digits[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < len; i++)
    {
      out[i * 2] = digits[in[i] >> 4];
      out[i * 2 + 1] = digits[in[i] & 0x0f];
    }

  out[len * 2] = '\0';
}

/* SHA-256 of the peer certificate's DER, which is what gets pinned.  The DER
 * is hashed rather than the public key so that the fingerprint the operator
 * copies is the same string openssl prints for the file on the other end
 * (`openssl x509 -outform der | sha256sum`), and can therefore be compared by
 * eye without either side computing something bespoke.
 */

static int wt_io_peer_fp(mbedtls_ssl_context *ssl, char *out, size_t cap)
{
  const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(ssl);
  unsigned char digest[32];

  if (crt == NULL || cap < WT_FP_HEX_LEN)
    {
      return -1;
    }

  if (mbedtls_sha256(crt->raw.p, crt->raw.len, digest, 0) != 0)
    {
      return -1;
    }

  wt_io_hex(digest, sizeof(digest), out);
  return 0;
}

static void wt_io_tls_free(struct wt_io_s *io)
{
  if (io->tls_up)
    {
      mbedtls_ssl_close_notify(&io->ssl);
      io->tls_up = false;
    }

  mbedtls_ssl_free(&io->ssl);
  mbedtls_ssl_config_free(&io->cfg);
  mbedtls_ctr_drbg_free(&io->drbg);
  mbedtls_net_free(&io->net);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct wt_io_s *wt_io_plain(int fd)
{
  struct wt_io_s *io = calloc(1, sizeof(*io));

  if (io == NULL)
    {
      return NULL;
    }

  io->kind = WT_IO_PLAIN;
  io->fd = fd;
  strlcpy(io->desc, "plaintext", sizeof(io->desc));
  return io;
}

struct wt_io_s *wt_io_tls_connect(const char *host, int port,
                                  const char *pin_fp,
                                  char *fp_out, size_t fp_cap,
                                  int *err)
{
  struct wt_io_s *io;
  char portstr[8];
  char seen[WT_FP_HEX_LEN];
  struct timeval tv;
  struct timespec t_start;
  struct timespec t_connected;
  struct timespec t_seeded;
  struct timespec t_done;
  unsigned long connect_ms;
  unsigned long seed_ms;
  unsigned long handshake_ms;
  int ret;

  if (err != NULL)
    {
      *err = WT_IO_OK;
    }

  if (fp_out != NULL && fp_cap > 0)
    {
      fp_out[0] = '\0';
    }

  io = calloc(1, sizeof(*io));
  if (io == NULL)
    {
      if (err != NULL)
        {
          *err = WT_IO_ERR_MEM;
        }

      return NULL;
    }

  io->kind = WT_IO_TLS;
  io->fd = -1;

  mbedtls_ssl_init(&io->ssl);
  mbedtls_ssl_config_init(&io->cfg);
  mbedtls_net_init(&io->net);
  mbedtls_ctr_drbg_init(&io->drbg);

  clock_gettime(CLOCK_MONOTONIC, &t_start);

  ret = mbedtls_ctr_drbg_seed(&io->drbg, wt_io_entropy, NULL,
                              (const unsigned char *)"web_tool", 8);
  if (ret != 0)
    {
      syslog(LOG_ERR, "web_tool: ctr_drbg_seed failed -0x%04x\n", -ret);
      if (err != NULL)
        {
          *err = WT_IO_ERR_ENTROPY;
        }

      goto fail;
    }

  clock_gettime(CLOCK_MONOTONIC, &t_seeded);
  snprintf(portstr, sizeof(portstr), "%d", port);

  ret = mbedtls_net_connect(&io->net, host, portstr, MBEDTLS_NET_PROTO_TCP);
  if (ret != 0)
    {
      syslog(LOG_ERR, "web_tool: connect %s:%d failed -0x%04x\n",
             host, port, -ret);
      if (err != NULL)
        {
          *err = WT_IO_ERR_CONNECT;
        }

      goto fail;
    }

  io->fd = io->net.fd;
  clock_gettime(CLOCK_MONOTONIC, &t_connected);

  /* Blocking socket with SO_RCVTIMEO rather than mbedtls_net_recv_timeout():
   * the select()/poll() inside that helper returns MBEDTLS_ERR_NET_POLL_FAILED
   * on NuttX.  Same workaround the upstream agent's TLS code uses, and for the
   * same reason.
   */

  mbedtls_net_set_block(&io->net);

  tv.tv_sec = WT_IO_RECV_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(io->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  tv.tv_sec = WT_IO_SEND_TIMEOUT_S;
  setsockopt(io->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  ret = mbedtls_ssl_config_defaults(&io->cfg, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      syslog(LOG_ERR, "web_tool: ssl_config_defaults -0x%04x\n", -ret);
      if (err != NULL)
        {
          *err = WT_IO_ERR_HANDSHAKE;
        }

      goto fail;
    }

  /* TLS 1.2 both ends: 1.3 is not compiled into this mbedTLS build, and
   * pinning the range makes the failure a configuration error here rather than
   * a BAD_CONFIG out of mbedtls_ssl_setup().
   */

  mbedtls_ssl_conf_min_tls_version(&io->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&io->cfg, MBEDTLS_SSL_VERSION_TLS1_2);

  mbedtls_ssl_conf_ciphersuites(&io->cfg, g_wt_ciphersuites);

  /* The certificate is checked by fingerprint below, not by chain.  Asking
   * mbedTLS to verify would need a CA bundle we do not ship and a clock we do
   * not have; VERIFY_NONE here is honest about that, and the pin is what
   * actually decides whether to proceed.
   */

  mbedtls_ssl_conf_authmode(&io->cfg, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&io->cfg, mbedtls_ctr_drbg_random, &io->drbg);

  ret = mbedtls_ssl_setup(&io->ssl, &io->cfg);
  if (ret != 0)
    {
      syslog(LOG_ERR, "web_tool: ssl_setup -0x%04x\n", -ret);
      if (err != NULL)
        {
          *err = WT_IO_ERR_HANDSHAKE;
        }

      goto fail;
    }

  (void)mbedtls_ssl_set_hostname(&io->ssl, host);
  mbedtls_ssl_set_bio(&io->ssl, &io->net, mbedtls_net_send,
                      mbedtls_net_recv, NULL);

  while ((ret = mbedtls_ssl_handshake(&io->ssl)) != 0)
    {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          char errbuf[96];

          mbedtls_strerror(ret, errbuf, sizeof(errbuf));
          syslog(LOG_ERR, "web_tool: TLS handshake failed -0x%04x: %s\n",
                 -ret, errbuf);
          if (err != NULL)
            {
              *err = WT_IO_ERR_HANDSHAKE;
            }

          goto fail;
        }
    }

  io->tls_up = true;

  /* Reported because it is the number the ciphersuite choice was made on.  For
   * comparison, the upstream agent's TLS path -- which offers everything and
   * lets the server choose, and ends up on DHE-2048 -- measured 541 ms on this
   * board (docs/local/2026-08-17-TLS性能核查.md).
   */

  clock_gettime(CLOCK_MONOTONIC, &t_done);

#define WT_MS(a, b) ((unsigned long)(((b).tv_sec - (a).tv_sec) * 1000 + \
                                     ((b).tv_nsec - (a).tv_nsec) / 1000000))
  seed_ms      = WT_MS(t_start, t_seeded);
  connect_ms   = WT_MS(t_seeded, t_connected);
  handshake_ms = WT_MS(t_connected, t_done);
#undef WT_MS

  if (wt_io_peer_fp(&io->ssl, seen, sizeof(seen)) < 0)
    {
      syslog(LOG_ERR, "web_tool: server sent no certificate\n");
      if (err != NULL)
        {
          *err = WT_IO_ERR_PIN;
        }

      goto fail;
    }

  if (fp_out != NULL && fp_cap >= sizeof(seen))
    {
      strlcpy(fp_out, seen, fp_cap);
    }

  if (pin_fp == NULL || pin_fp[0] == '\0')
    {
      /* Nothing pinned.  Refuse, and print what to pin.  Continuing here
       * would create the state the whole design is trying to avoid: a link
       * that looks protected and authenticates nobody.
       */

      if (err != NULL)
        {
          *err = WT_IO_ERR_NOPIN;
        }

      goto fail;
    }

  if (strcasecmp(pin_fp, seen) != 0)
    {
      syslog(LOG_ERR, "web_tool: certificate fingerprint mismatch\n");
      if (err != NULL)
        {
          *err = WT_IO_ERR_PIN;
        }

      goto fail;
    }

  snprintf(io->desc, sizeof(io->desc), "TLS %s / %s",
           mbedtls_ssl_get_version(&io->ssl),
           mbedtls_ssl_get_ciphersuite(&io->ssl));

  /* Split, because a single number here would be unusable: the interesting
   * quantity is the handshake, and lumping the DRBG seed and the TCP connect
   * into it is how a measurement stops meaning anything.  For comparison the
   * upstream agent's path -- everything offered, server picks, ends on
   * DHE-2048 -- measured 541 ms of handshake alone to a public host
   * (docs/local/2026-08-17-TLS性能核查.md).
   */

  syslog(LOG_INFO, "web_tool: %s to %s:%d, certificate pinned "
                   "(drbg seed %lu ms, tcp %lu ms, handshake %lu ms)\n",
         io->desc, host, port, seed_ms, connect_ms, handshake_ms);
  return io;

fail:
  wt_io_tls_free(io);
  free(io);
  return NULL;
}

int wt_io_read(struct wt_io_s *io, void *buf, size_t len)
{
  if (io->kind == WT_IO_PLAIN)
    {
      ssize_t n = recv(io->fd, buf, len, 0);

      return n < 0 ? -errno : (int)n;
    }

  for (; ; )
    {
      int ret = mbedtls_ssl_read(&io->ssl, buf, len);

      if (ret > 0)
        {
          return ret;
        }

      if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          return 0;
        }

      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }

      /* A socket read timeout arrives as NET_RECV_FAILED with EAGAIN under it.
       * Reported as -EAGAIN so the session loop can treat it as "nothing yet"
       * instead of as a dead link -- otherwise an idle console would be
       * disconnected every SO_RCVTIMEO.
       */

      if (ret == MBEDTLS_ERR_NET_RECV_FAILED &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT))
        {
          return -EAGAIN;
        }

      return -EIO;
    }
}

int wt_io_write(struct wt_io_s *io, const void *buf, size_t len)
{
  const unsigned char *p = buf;
  size_t off = 0;

  while (off < len)
    {
      int ret;

      if (io->kind == WT_IO_PLAIN)
        {
          ssize_t n = send(io->fd, p + off, len - off, 0);

          if (n > 0)
            {
              off += (size_t)n;
              continue;
            }

          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          return n < 0 ? -errno : -EPIPE;
        }

      ret = mbedtls_ssl_write(&io->ssl, p + off, len - off);
      if (ret > 0)
        {
          off += (size_t)ret;
          continue;
        }

      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }

      if (ret == MBEDTLS_ERR_NET_SEND_FAILED &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT))
        {
          return -EAGAIN;
        }

      return -EIO;
    }

  return 0;
}

int wt_io_fd(struct wt_io_s *io)
{
  return io->fd;
}

void wt_io_shutdown(struct wt_io_s *io)
{
  if (io != NULL && io->fd >= 0)
    {
      shutdown(io->fd, SHUT_RDWR);
    }
}

void wt_io_close(struct wt_io_s *io)
{
  if (io == NULL)
    {
      return;
    }

  if (io->kind == WT_IO_TLS)
    {
      wt_io_tls_free(io);
    }
  else if (io->fd >= 0)
    {
      close(io->fd);
    }

  free(io);
}

const char *wt_io_describe(struct wt_io_s *io)
{
  return io != NULL ? io->desc : "none";
}
