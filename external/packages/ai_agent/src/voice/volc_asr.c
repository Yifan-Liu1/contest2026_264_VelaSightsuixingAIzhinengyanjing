/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Doubao streaming ASR via WebSocket (V2 binary protocol).
 *
 * Protocol: wss://openspeech.bytedance.com/api/v2/asr
 * Flow:
 *   1. TLS connect + HTTP Upgrade to WebSocket
 *   2. Send full_client_request (JSON metadata) in Volcengine frame
 *   3. Send audio_only chunks (PCM), last chunk flagged
 *   4. Receive full_server_response, extract result text
 *
 * Volcengine binary frame (carried inside standard WebSocket frames):
 *   Byte 0: [protocol_version:4][header_size:4]  = 0x11
 *   Byte 1: [message_type:4][message_type_flags:4]
 *   Byte 2: [serialization:4][compression:4]
 *   Byte 3: reserved = 0x00
 *   Bytes 4-7: payload size (big-endian uint32)
 *   Bytes 8+: payload
 */

#include "voice/volc_asr.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "netutils/base64.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "volc_asr";

/* Volcengine binary protocol constants */
#define VOLC_PROTO_VER 0x11 /* version=1, header_size=1 (x4=4B) */
#define VOLC_MSG_FULL_REQ 0x10 /* full_client_request */
#define VOLC_MSG_AUDIO 0x20 /* audio_only, not last */
#define VOLC_MSG_AUDIO_LAST 0x22 /* audio_only, last chunk */
#define VOLC_MSG_FULL_RESP 0x90 /* full_server_response */
#define VOLC_MSG_ERROR 0xF0 /* server error */
#define VOLC_SER_JSON 0x10 /* JSON, no compression */
#define VOLC_SER_RAW 0x00 /* raw audio, no compression */
#define VOLC_HDR_SIZE 8 /* 4-byte header + 4-byte payload len */

#define WS_BUF_SIZE 4096
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_OPCODE_PING 0x09
#define WS_OPCODE_PONG 0x0A
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

#define ASR_RESP_CODE_OK 1000

/* Two backend codes describe an empty utterance rather than a fault, and the
 * caller has to be able to tell them from a transport failure: 1013 is audio
 * that carried no speech, 1012 is a stream that carried no usable data at all.
 * Both are the ordinary outcome of someone releasing the key without speaking,
 * or speaking too quietly, so they are reported as -ENODATA -- the same value
 * an empty final result produces -- and not as -EIO.
 */

#define ASR_RESP_CODE_NO_SPEECH 1013
#define ASR_RESP_CODE_NO_DATA 1012
#define ASR_UUID_LEN 37 /* 36 chars + NUL */

/* TLS context for ASR connection (not pooled — single use) */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} asr_tls_ctx_t;

/* ── Entropy source ──────────────────────────────────────────── */

static int asr_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    syslog(LOG_ERR, "[volc_asr] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

/* ── TLS connect / free ──────────────────────────────────────── */

/* Where the time goes while a session is being opened.
 *
 * The open path had no instrumentation, and the two lines it did emit bracket
 * only its cheap half: "TLS connected" is printed at the end of this function
 * and "WebSocket upgrade OK" at the end of ws_upgrade(), so a board log can be
 * read for the upgrade and the first request and for nothing before them.
 * Measured that way on 2026-09-09 the visible part was 176 ms, and the
 * credential read, the DRBG seed, the name resolution, the TCP connect and the
 * whole TLS handshake were all inside the unlit gap ahead of it.
 *
 * That gap is exactly what a TLS session cache or a pre-opened connection would
 * remove, so its size decides which of those is worth building -- and it was the
 * one number nothing recorded.
 *
 * Resolution and the connect are now separate figures, and the reason they had
 * to be is that together they were the largest and least stable term: three runs
 * on 2026-09-09 measured 172, 171 and 791 ms for the pair.  A fourfold spread
 * has two candidate explanations that call for opposite work -- a name lookup
 * that missed its cache, or a TCP connect that lost a SYN on a link whose IOB
 * pool was observed down to 55 of 60 in the same session -- and one figure
 * covering both cannot choose between them.
 *
 * The split is measurement only.  mbedtls_net_connect() is still handed the
 * hostname and still iterates every address it resolves, which is what makes a
 * v6-first answer on a v4-only link work; resolving here and passing a literal
 * on would have taken that away.  What this does instead is resolve once first
 * and time it, so that the resolution inside the connect is a cache hit and the
 * connect figure is the connect.
 *
 * That depends on NuttX caching answers, and it does: CONFIG_NETDB_DNSCLIENT is
 * enabled with eight entries and an hour of lifetime, and gethostentbyname_r()
 * consults that cache before it queries.  So the extra call costs a cache lookup
 * rather than a second query.  Were the cache ever turned off, dns_ms would stay
 * honest and tcp_ms would start including a query of its own.
 */

struct asr_open_timing_s {
    uint32_t seed_ms;      /* mbedtls_ctr_drbg_seed, entropy included */
    uint32_t dns_ms;       /* name resolution alone; 0 on the proxy path */
    uint32_t tcp_ms;       /* the connect alone, or the tunnel on the proxy path */
    uint32_t handshake_ms; /* config, setup and mbedtls_ssl_handshake */
    bool proxied;          /* went through a CONNECT tunnel instead */
    bool offered;          /* a cached TLS session was offered for resumption */
};

/* The one TLS session this client keeps, for resumption.
 *
 * Worth keeping because of what the first measurements said about the handshake:
 * 557 to 720 ms across three runs against a round trip near 150 ms, so most of
 * it is not the network.  This build compiles no MBEDTLS_*_ALT backend and the
 * server picked TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256, so every connection
 * was paying for a P-256 scalar multiplication and an RSA-2048 signature check
 * in software on a Cortex-M33.  A resumed handshake skips both and one round
 * trip with them.  The server offers the material to do it: the probe added with
 * the timing reported resumable=yes on every run.
 *
 * Credentials are deliberately not part of the key.  They travel inside the
 * tunnel -- the Authorization header on the upgrade and the appid/token in the
 * first request frame -- so a session says nothing about who is using it and a
 * credential change does not invalidate one.  This is the difference between
 * resuming a session and pre-opening a whole ASR stream: the second carries a
 * full_client_request built from credentials read at the time and would go stale
 * when they changed.
 *
 * One entry, because there is one endpoint.  Guarded because unlike the
 * credential statics beside it this outlives a call, and the cost of a mutex
 * held across two copies is not worth reasoning about the alternative.
 */

static pthread_mutex_t s_tls_session_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_ssl_session s_tls_session;
static bool s_tls_session_valid;

/* Forget the cached session.  Called when a handshake that offered it failed,
 * which is the only way this client learns that what it held was not usable.
 */

static void asr_session_drop(void)
{
    pthread_mutex_lock(&s_tls_session_lock);
    if (s_tls_session_valid) {
        mbedtls_ssl_session_free(&s_tls_session);
        s_tls_session_valid = false;
    }

    pthread_mutex_unlock(&s_tls_session_lock);
}

static uint32_t asr_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint32_t)((uint64_t)ts.tv_sec * 1000u
        + (uint64_t)ts.tv_nsec / 1000000u);
}

/* Resolve host so that the connect after it does not have to.
 *
 * The result is thrown away on purpose.  This is not a lookup the connection
 * uses -- mbedtls_net_connect() does its own, and keeping that is what preserves
 * its walk over every address a name answers with.  This call exists to move the
 * cost of resolution out of the connect's figure and into one of its own, and it
 * is free to do so because the answer lands in NuttX's DNS cache where the
 * connect's lookup will find it.
 *
 * Failure is not reported.  If a name cannot be resolved here the connect will
 * fail on its own and say so, with its own error, exactly as it did before this
 * function existed; returning something would only invite a second opinion about
 * a decision that is not this function's to make.
 */

static void asr_warm_resolve(const char* host, const char* port)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) == 0) {
        freeaddrinfo(res);
    }
}

/* timing may be NULL, which is what the batch path passes: it has its own
 * infer/e2e measurements and does not need the breakdown.
 */

static int asr_tls_connect(asr_tls_ctx_t* ctx,
    const char* host, const char* port,
    struct asr_open_timing_s* timing)
{
    int ret;
    uint32_t mark;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    const char* pers = "volc_asr";

    mark = asr_now_ms();
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, asr_entropy_func,
        NULL, (const unsigned char*)pers,
        strlen(pers));
    if (timing != NULL) {
        timing->seed_ms = asr_now_ms() - mark;
    }

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    /* If proxy is enabled, open a CONNECT tunnel first */
    if (http_proxy_is_enabled()) {
        int port_num = atoi(port);
        int tunnel_fd;

        if (timing != NULL) {
            timing->proxied = true;
        }

        /* No resolution figure on this path: the proxy resolves the name, and
         * the tunnel is a hop plus its own request rather than a connect, so it
         * is recorded as one number and flagged as a different measurement
         * instead of being compared against a direct connect.
         */

        mark = asr_now_ms();
        tunnel_fd = proxy_open_tunnel(host, port_num, 30000);
        if (timing != NULL) {
            timing->tcp_ms = asr_now_ms() - mark;
        }

        if (tunnel_fd < 0) {
            syslog(LOG_ERR, "[%s] proxy tunnel failed\n", TAG);
            return -ECONNREFUSED;
        }

        ctx->net.fd = tunnel_fd;
        syslog(LOG_INFO, "[%s] Using proxy tunnel fd=%d for %s:%s\n",
            TAG, tunnel_fd, host, port);
    } else {
        mark = asr_now_ms();
        asr_warm_resolve(host, port);
        if (timing != NULL) {
            timing->dns_ms = asr_now_ms() - mark;
        }

        mark = asr_now_ms();
        ret = mbedtls_net_connect(&ctx->net, host, port,
            MBEDTLS_NET_PROTO_TCP);
        if (timing != NULL) {
            timing->tcp_ms = asr_now_ms() - mark;
        }

        if (ret != 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s: -0x%04x\n",
                TAG, host, port, -ret);
            return -ECONNREFUSED;
        }
    }

    mark = asr_now_ms();
    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };

        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv));
    }

    ret = mbedtls_ssl_config_defaults(&ctx->cfg,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        return -EIO;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_2);
#endif

#if defined(MBEDTLS_SSL_ALPN)
    static const char* alpn[] = { "http/1.1", NULL };

    mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random,
        &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        return -EIO;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    /* Offer what the last handshake left, if it left anything.
     *
     * set_session copies, so the cache is not handed out and the lock is only
     * held across that copy.  A server that declines the ticket does not fail
     * the handshake -- it simply does not resume, and mbedtls carries on with a
     * full one -- so there is no fallback to write here, only a figure that
     * comes back large instead of small.
     */

    {
        bool offered = false;

        pthread_mutex_lock(&s_tls_session_lock);
        if (s_tls_session_valid
            && mbedtls_ssl_set_session(&ctx->ssl, &s_tls_session) == 0) {
            offered = true;
        }

        pthread_mutex_unlock(&s_tls_session_lock);

        if (timing != NULL) {
            timing->offered = offered;
        }
    }

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ
            && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);

            /* Drop what was offered.  A handshake that fails while resuming is
             * the only evidence this client gets that the material it kept is no
             * longer usable, and keeping it would make the next attempt fail the
             * same way.  Unconditional because a full handshake failing has
             * nothing to do with the cache and dropping an entry costs one
             * handshake, while keeping a bad one costs every one after it.
             */

            asr_session_drop();
            return -EIO;
        }
    }

    /* Keep what this handshake produced, resumed or not.
     *
     * Saved on every success rather than only the first, because a server may
     * issue a fresh ticket on a resumed handshake and the one just used may be
     * single-use; re-reading it here is what keeps a long-lived device resuming
     * rather than resuming once.
     */

    pthread_mutex_lock(&s_tls_session_lock);
    if (s_tls_session_valid) {
        mbedtls_ssl_session_free(&s_tls_session);
        s_tls_session_valid = false;
    }

    mbedtls_ssl_session_init(&s_tls_session);
    if (mbedtls_ssl_get_session(&ctx->ssl, &s_tls_session) == 0) {
        s_tls_session_valid = true;
    } else {
        mbedtls_ssl_session_free(&s_tls_session);
    }

    pthread_mutex_unlock(&s_tls_session_lock);

    /* Config, setup and the handshake as one figure.  They are separable but
     * there is no reason to: the first two are local work with no round trip in
     * them, so anything large here is the handshake, and the handshake is what a
     * resumed session would shorten.
     */

    if (timing != NULL) {
        timing->handshake_ms = asr_now_ms() - mark;
    }

    syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, host, port);

    /* What this handshake negotiated, whether a session was offered to it, and
     * whether one is now held for the next.
     *
     * Read offered against handshake_ms, because that pair is the whole test of
     * whether resumption is working.  offered=no with a large figure is the first
     * connection after a boot and is expected to be slow.  offered=yes with a
     * small figure is a resumed handshake.  offered=yes with a figure as large as
     * a full one means the server took the ticket and declined to resume, which
     * looks like nothing at all in the log unless these two are read together.
     *
     * The version and the ciphersuite stay because they are what make the figure
     * interpretable at all: TLS 1.2 puts a full handshake at two round trips and
     * a resumed one at one, and an ECDHE suite is what says the difference
     * between them is arithmetic and not just a round trip.
     */

    if (timing != NULL) {
        bool held;

        pthread_mutex_lock(&s_tls_session_lock);
        held = s_tls_session_valid;
        pthread_mutex_unlock(&s_tls_session_lock);

        syslog(LOG_INFO, "[%s] TLS %s / %s, offered=%s held=%s\n", TAG,
            mbedtls_ssl_get_version(&ctx->ssl),
            mbedtls_ssl_get_ciphersuite(&ctx->ssl),
            timing->offered ? "yes" : "no",
            held ? "yes" : "no");
    }

    return 0;
}

static void asr_tls_free(asr_tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── Low-level TLS I/O helpers ───────────────────────────────── */

static int tls_write_all(asr_tls_ctx_t* ctx,
    const unsigned char* buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written,
            len - written);

        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] ssl_write: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    return 0;
}

static int tls_read_all(asr_tls_ctx_t* ctx,
    unsigned char* buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);

        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0
            || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    return 0;
}

/* ── UUID generation (simple v4-like) ────────────────────────── */

static void generate_uuid(char* out, size_t cap)
{
    unsigned char rnd[16];

    asr_entropy_func(NULL, rnd, sizeof(rnd));
    rnd[6] = (rnd[6] & 0x0F) | 0x40; /* version 4 */
    rnd[8] = (rnd[8] & 0x3F) | 0x80; /* variant 1 */
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        rnd[0], rnd[1], rnd[2], rnd[3],
        rnd[4], rnd[5], rnd[6], rnd[7],
        rnd[8], rnd[9], rnd[10], rnd[11],
        rnd[12], rnd[13], rnd[14], rnd[15]);
}

/* ── WebSocket upgrade handshake ─────────────────────────────── */

static int ws_upgrade(asr_tls_ctx_t* ctx, const char* host,
    const char* path, const char* token)
{
    /* Generate random 16-byte key, base64-encode it */
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    asr_entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    char req[768];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer;%s\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64,
        token);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);

    if (ret != 0) {
        return ret;
    }

    /* Read HTTP 101 response */
    char resp[WS_BUF_SIZE];
    size_t rlen = 0;

    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);

        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0
            || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        }
    }

    int status = 0;

    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n",
            TAG, status);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (client must mask) ─────────────────── */

static int ws_send_frame(asr_tls_ctx_t* ctx, unsigned char opcode,
    const unsigned char* payload, size_t plen)
{
    unsigned char hdr[14]; /* max WS header: 2 + 8 + 4 */
    size_t hdr_len = 0;

    hdr[0] = WS_FIN_BIT | opcode;

    if (plen < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)plen;
        hdr_len = 2;
    } else if (plen <= 0xFFFF) {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(plen >> 8);
        hdr[3] = (unsigned char)(plen & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = WS_MASK_BIT | 127;
        memset(hdr + 2, 0, 4); /* high 32 bits = 0 */
        hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(plen & 0xFF);
        hdr_len = 10;
    }

    /* Generate 4-byte mask key */
    unsigned char mask[WS_MASK_KEY_LEN];

    asr_entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    /* Send header */
    int ret = tls_write_all(ctx, hdr, hdr_len);

    if (ret != 0) {
        return ret;
    }

    /* Mask and send payload in chunks to avoid large stack alloc */
    unsigned char chunk[1024];
    size_t sent = 0;

    while (sent < plen) {
        size_t clen = plen - sent;

        if (clen > sizeof(chunk)) {
            clen = sizeof(chunk);
        }
        for (size_t i = 0; i < clen; i++) {
            chunk[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        }
        ret = tls_write_all(ctx, chunk, clen);
        if (ret != 0) {
            return ret;
        }
        sent += clen;
    }

    return 0;
}

static int ws_send_binary(asr_tls_ctx_t* ctx,
    const unsigned char* payload, size_t plen)
{
    return ws_send_frame(ctx, WS_OPCODE_BINARY, payload, plen);
}

/* ── WebSocket frame recv ────────────────────────────────────── */

static int ws_recv_frame(asr_tls_ctx_t* ctx,
    unsigned char* buf, size_t cap,
    size_t* out_len, int* out_opcode)
{
    unsigned char hdr[2];
    int ret = tls_read_all(ctx, hdr, 2);

    if (ret != 0) {
        return ret;
    }

    *out_opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & WS_MASK_BIT) != 0;
    size_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];

        ret = tls_read_all(ctx, ext, 2);
        if (ret != 0) {
            return ret;
        }
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];

        ret = tls_read_all(ctx, ext, 8);
        if (ret != 0) {
            return ret;
        }
        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16)
            | ((size_t)ext[6] << 8) | ext[7];
    }

    unsigned char mask[WS_MASK_KEY_LEN];

    if (masked) {
        ret = tls_read_all(ctx, mask, WS_MASK_KEY_LEN);
        if (ret != 0) {
            return ret;
        }
    }

    if (plen > cap) {
        syslog(LOG_ERR, "[%s] WS frame too large: %zu > %zu\n",
            TAG, plen, cap);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) {
            return ret;
        }
        if (masked) {
            for (size_t i = 0; i < plen; i++) {
                buf[i] ^= mask[i % 4];
            }
        }
    }

    *out_len = plen;
    return 0;
}

/* ── Volcengine binary frame builders ────────────────────────── */

/* Build a Volcengine header (8 bytes) and wrap payload into a
 * standard WebSocket binary frame for sending. */
static int send_volc_frame(asr_tls_ctx_t* ctx,
    unsigned char msg_type,
    unsigned char serialization,
    const unsigned char* payload,
    size_t plen)
{
    unsigned char* frame = malloc(VOLC_HDR_SIZE + plen);

    if (!frame) {
        return -ENOMEM;
    }

    frame[0] = VOLC_PROTO_VER;
    frame[1] = msg_type;
    frame[2] = serialization;
    frame[3] = 0x00;
    frame[4] = (unsigned char)((plen >> 24) & 0xFF);
    frame[5] = (unsigned char)((plen >> 16) & 0xFF);
    frame[6] = (unsigned char)((plen >> 8) & 0xFF);
    frame[7] = (unsigned char)(plen & 0xFF);

    if (plen > 0) {
        memcpy(frame + VOLC_HDR_SIZE, payload, plen);
    }

    int ret = ws_send_binary(ctx, frame, VOLC_HDR_SIZE + plen);

    free(frame);
    return ret;
}

/* Build and send the full_client_request JSON metadata. */
static int send_full_client_request(asr_tls_ctx_t* ctx,
    const char* app_id,
    const char* token,
    const char* cluster)
{
    char reqid[ASR_UUID_LEN];

    generate_uuid(reqid, sizeof(reqid));

    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return -ENOMEM;
    }

    /* app */
    cJSON* app = cJSON_AddObjectToObject(root, "app");

    cJSON_AddStringToObject(app, "appid", app_id);
    cJSON_AddStringToObject(app, "token", token);
    cJSON_AddStringToObject(app, "cluster", cluster);

    /* user */
    cJSON* user = cJSON_AddObjectToObject(root, "user");

    cJSON_AddStringToObject(user, "uid", "agent");

    /* audio */
    cJSON* audio = cJSON_AddObjectToObject(root, "audio");

    cJSON_AddStringToObject(audio, "format", "raw");
    cJSON_AddNumberToObject(audio, "rate",
        AGENT_VOICE_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "bits", AGENT_VOICE_BITS);
    cJSON_AddNumberToObject(audio, "channel",
        AGENT_VOICE_CHANNELS);
    cJSON_AddStringToObject(audio, "language", "zh-CN");

    /* request */
    cJSON* req = cJSON_AddObjectToObject(root, "request");

    cJSON_AddStringToObject(req, "reqid", reqid);
    cJSON_AddStringToObject(req, "workflow",
        "audio_in,resample,partition,vad,fe,decode");
    cJSON_AddNumberToObject(req, "sequence", 1);
    cJSON_AddNumberToObject(req, "nbest", 1);
    cJSON_AddBoolToObject(req, "show_utterances", 0);
    /* VAD: avoid cutting short commands on QEMU virtual mic */
    cJSON_AddBoolToObject(req, "vad_signal", 1);
    cJSON_AddStringToObject(req, "start_silence_time", "3000");
    cJSON_AddStringToObject(req, "vad_silence_time", "800");

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (!json_str) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] full_client_request: reqid=%s\n",
        TAG, reqid);

    int ret = send_volc_frame(ctx, VOLC_MSG_FULL_REQ, VOLC_SER_JSON,
        (const unsigned char*)json_str,
        strlen(json_str));
    free(json_str);
    return ret;
}

/* Send one audio chunk via Volcengine binary protocol. */
static int send_audio_chunk(asr_tls_ctx_t* ctx,
    const unsigned char* pcm, size_t len,
    int is_last)
{
    unsigned char msg_type = is_last ? VOLC_MSG_AUDIO_LAST
                                     : VOLC_MSG_AUDIO;

    return send_volc_frame(ctx, msg_type, VOLC_SER_RAW, pcm, len);
}

/* ── Receive and parse Volcengine server response ────────────── */

/* Read a big-endian uint32 from buf at offset off. */
static uint32_t read_be32(const unsigned char* buf, size_t off)
{
    return ((uint32_t)buf[off] << 24)
        | ((uint32_t)buf[off + 1] << 16)
        | ((uint32_t)buf[off + 2] << 8)
        | (uint32_t)buf[off + 3];
}

/* Receive one WS frame, unwrap Volcengine header, parse JSON.
 * Extracts result[0].text into text_out.
 * Returns: 1 = got final result, 0 = intermediate, negative = error
 *
 * Volcengine binary protocol layout varies by message type:
 *
 *   Normal response (msg_type 0x9):
 *     [4B header] [4B payload_size] [payload...]
 *
 *   Error response (msg_type 0xF):
 *     [4B header] [4B error_code] [4B error_msg_size] [error_msg...]
 */
static int recv_volc_response(asr_tls_ctx_t* ctx,
    char* text_out, size_t text_cap)
{
    unsigned char* buf = malloc(WS_BUF_SIZE);

    if (!buf) {
        return -ENOMEM;
    }

    size_t flen;
    int opcode;
    int ret = ws_recv_frame(ctx, buf, WS_BUF_SIZE, &flen, &opcode);

    if (ret != 0) {
        free(buf);
        return ret;
    }

    if (opcode == WS_OPCODE_CLOSE) {
        syslog(LOG_WARNING, "[%s] Server sent WS close\n", TAG);
        free(buf);
        return -ECONNRESET;
    }

    /* Keepalive control frames, not Volcengine data.  Seen in practice once
     * a session runs long enough (~8s) for the server to probe the
     * connection: a zero-length PING was previously fed to the header
     * parser below, which read it as a 0-byte payload and failed with
     * "Frame too short".  A PONG must echo it back or the server may
     * decide the client is gone and close the session. */
    if (opcode == WS_OPCODE_PING) {
        int pong_ret = ws_send_frame(ctx, WS_OPCODE_PONG, buf, flen);

        if (pong_ret != 0) {
            free(buf);
            return pong_ret;
        }

        free(buf);
        return 0; /* treat like an intermediate/no-op response */
    }

    if (opcode == WS_OPCODE_PONG) {
        free(buf);
        return 0;
    }

    /* Need at least Volcengine header (4 bytes minimum) */
    if (flen < 4) {
        syslog(LOG_ERR, "[%s] Frame too short: %zu\n", TAG, flen);
        free(buf);
        return -EPROTO;
    }

    /* Parse Volcengine header */
    int hdr_units = buf[0] & 0x0F;
    size_t volc_hdr_len = (size_t)hdr_units * 4;
    unsigned char msg_type_byte = buf[1];
    unsigned char msg_type_nibble = (msg_type_byte >> 4) & 0x0F;

    if (volc_hdr_len < 4 || flen < volc_hdr_len) {
        syslog(LOG_ERR,
            "[%s] Bad volc header: hdr_units=%d flen=%zu\n",
            TAG, hdr_units, flen);
        free(buf);
        return -EPROTO;
    }

    /* ── Error response: special layout with error_code field ── */
    if (msg_type_nibble == 0x0F) {
        /* [4B hdr][4B error_code][4B msg_size][msg...] */
        if (flen < volc_hdr_len + 8) {
            syslog(LOG_ERR,
                "[%s] Error frame too short: %zu\n",
                TAG, flen);
            free(buf);
            return -EPROTO;
        }

        uint32_t err_code = read_be32(buf, volc_hdr_len);
        uint32_t msg_size = read_be32(buf, volc_hdr_len + 4);
        size_t msg_off = volc_hdr_len + 8;

        if (msg_off + msg_size > flen) {
            msg_size = (uint32_t)(flen - msg_off);
        }

        /* NUL-terminate and log the error message */
        char errbuf[256];
        size_t cplen = msg_size;

        if (cplen >= sizeof(errbuf)) {
            cplen = sizeof(errbuf) - 1;
        }
        memcpy(errbuf, buf + msg_off, cplen);
        errbuf[cplen] = '\0';

        syslog(LOG_ERR,
            "[%s] Server error %lu: %s\n",
            TAG, (unsigned long)err_code, errbuf);
        free(buf);
        return -EIO;
    }

    /* ── Normal response: [4B hdr][4B payload_size][payload] ── */
    if (flen < volc_hdr_len + 4) {
        syslog(LOG_ERR,
            "[%s] Frame too short for payload size: %zu\n",
            TAG, flen);
        free(buf);
        return -EPROTO;
    }

    uint32_t payload_len = read_be32(buf, volc_hdr_len);
    size_t data_off = volc_hdr_len + 4;

    if (data_off + payload_len > flen) {
        syslog(LOG_ERR,
            "[%s] Payload overrun: off=%zu + plen=%lu "
            "> flen=%zu\n",
            TAG, data_off, (unsigned long)payload_len, flen);
        free(buf);
        return -EPROTO;
    }

    /* We only care about full_server_response (0x9) */
    if (msg_type_nibble != 0x09) {
        free(buf);
        return 0; /* skip unknown message types */
    }

    /* Parse JSON payload */
    buf[data_off + payload_len] = '\0';
    cJSON* root = cJSON_Parse((char*)(buf + data_off));

    free(buf);
    buf = NULL;

    if (!root) {
        syslog(LOG_ERR, "[%s] Bad JSON in response\n", TAG);
        return -EPROTO;
    }

    cJSON* code_j = cJSON_GetObjectItem(root, "code");
    int code = code_j ? (int)code_j->valuedouble : -1;

    if (code != ASR_RESP_CODE_OK) {
        cJSON* msg_j = cJSON_GetObjectItem(root, "message");

        syslog(LOG_ERR, "[%s] ASR error %d: %s\n", TAG, code,
            (msg_j && msg_j->valuestring)
                ? msg_j->valuestring
                : "unknown");
        cJSON_Delete(root);

        /* "Nothing was said" is not a read/write failure.  Collapsing it into
         * -EIO put "设备读写失败" on screen for an utterance the service had
         * simply found no speech in, which sends the user looking at the
         * microphone instead of just speaking up. */

        if (code == ASR_RESP_CODE_NO_SPEECH
            || code == ASR_RESP_CODE_NO_DATA) {
            return -ENODATA;
        }

        return -EIO;
    }

    /* Extract result[0].text.
     * Volcengine streaming ASR sends multiple responses per session.
     * We maintain a "confirmed" offset: text_out[0..confirmed_len-1]
     * holds final utterances that must not be overwritten.
     * Intermediate results (seq >= 0) overwrite only the portion
     * after confirmed_len.  Final results (seq < 0) append and
     * advance confirmed_len. */
    cJSON* result = cJSON_GetObjectItem(root, "result");

    /* Negative sequence means final response */
    cJSON* seq_j = cJSON_GetObjectItem(root, "sequence");
    int seq = seq_j ? (int)seq_j->valuedouble : 0;

    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        cJSON* first = cJSON_GetArrayItem(result, 0);
        cJSON* txt = cJSON_GetObjectItem(first, "text");

        if (txt && cJSON_IsString(txt) && txt->valuestring
            && txt->valuestring[0] != '\0') {
            static size_t confirmed_len;
            if (text_out[0] == '\0') {
                confirmed_len = 0;
            }

            if (seq < 0) {
                /* Final: write after confirmed, then advance */
                size_t remain = text_cap - confirmed_len - 1;
                if (remain > 0) {
                    strncpy(text_out + confirmed_len,
                        txt->valuestring, remain);
                    text_out[text_cap - 1] = '\0';
                }
                confirmed_len = strlen(text_out);
            } else {
                /* Intermediate: overwrite only after confirmed */
                size_t remain = text_cap - confirmed_len - 1;
                if (remain > 0) {
                    strncpy(text_out + confirmed_len,
                        txt->valuestring, remain);
                    text_out[text_cap - 1] = '\0';
                }
            }
        }
    }

    cJSON_Delete(root);

    return (seq < 0) ? 1 : 0;
}

/* ── Credentials (self-loaded from config store) ─────────────── */

static char s_app_id[128];
static char s_token[256];
static char s_cluster[128];

static int volc_asr_init(void)
{
    memset(s_app_id, 0, sizeof(s_app_id));
    memset(s_token, 0, sizeof(s_token));
    memset(s_cluster, 0, sizeof(s_cluster));

    claw_config_get(AGENT_CFG_KEY_VOLC_APPKEY,
        s_app_id, sizeof(s_app_id));
    claw_config_get(AGENT_CFG_KEY_VOLC_TOKEN,
        s_token, sizeof(s_token));

    if (claw_config_get(AGENT_CFG_KEY_VOLC_ASR_CLUSTER,
            s_cluster, sizeof(s_cluster))
            != OK
        || s_cluster[0] == '\0') {
        strncpy(s_cluster, AGENT_DOUBAO_ASR_CLUSTER,
            sizeof(s_cluster) - 1);
    }

    return 0;
}

/* ── Public API (direct call, kept for backward compat) ──────── */

int volc_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    const char* app_id,
    const char* token,
    const char* cluster,
    char* text_out,
    size_t text_cap)
{
    if (!pcm_data || pcm_len == 0 || !app_id || !token
        || !cluster || !text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    struct timespec e2e_t0;
    clock_gettime(CLOCK_MONOTONIC, &e2e_t0);

    asr_tls_ctx_t ctx;
    int ret;

    /* 1. TLS connect */
    ret = asr_tls_connect(&ctx, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_PORT, NULL);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 2. WebSocket upgrade */
    ret = ws_upgrade(&ctx, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_WS_PATH, token);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 3. Send full_client_request (JSON metadata) */
    ret = send_full_client_request(&ctx, app_id, token, cluster);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 4. Stream audio chunks */
    size_t offset = 0;
    size_t chunk_size = AGENT_ASR_CHUNK_SIZE;

    while (offset < pcm_len) {
        size_t remain = pcm_len - offset;
        size_t clen = (remain < chunk_size) ? remain : chunk_size;
        int is_last = (offset + clen >= pcm_len) ? 1 : 0;

        ret = send_audio_chunk(&ctx, pcm_data + offset, clen,
            is_last);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] send_audio_chunk failed: %d\n",
                TAG, ret);
            asr_tls_free(&ctx);
            return ret;
        }
        offset += clen;
    }

    syslog(LOG_INFO, "[%s] Sent %zu bytes in %zu-byte chunks\n",
        TAG, pcm_len, chunk_size);

    /* 5. Receive responses until final (sequence < 0) */
    struct timespec asr_t0;
    clock_gettime(CLOCK_MONOTONIC, &asr_t0);

    int attempts = 0;
    int max_attempts = 100; /* safety limit */

    while (attempts < max_attempts) {
        ret = recv_volc_response(&ctx, text_out, text_cap);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] recv error: %d\n", TAG, ret);
            asr_tls_free(&ctx);
            return ret;
        }
        if (ret == 1) {
            break; /* final response received */
        }
        attempts++;
    }

    struct timespec asr_t1;
    clock_gettime(CLOCK_MONOTONIC, &asr_t1);
    long asr_ms = (asr_t1.tv_sec - asr_t0.tv_sec) * 1000
        + (asr_t1.tv_nsec - asr_t0.tv_nsec) / 1000000;

    asr_tls_free(&ctx);

    if (text_out[0] == '\0') {
        syslog(LOG_WARNING, "[%s] No text recognized (%ldms)\n",
            TAG, asr_ms);
        return -ENODATA;
    }

    struct timespec e2e_t1;
    clock_gettime(CLOCK_MONOTONIC, &e2e_t1);
    long e2e_ms = (e2e_t1.tv_sec - e2e_t0.tv_sec) * 1000
        + (e2e_t1.tv_nsec - e2e_t0.tv_nsec) / 1000000;

    syslog(LOG_INFO,
        "[%s] Recognized: %.80s (infer=%ldms e2e=%ldms)\n",
        TAG, text_out, asr_ms, e2e_ms);
    return 0;
}

/* ── Streaming ASR API ────────────────────────────────────────── */

struct volc_asr_stream {
    asr_tls_ctx_t tls;
    int ready; /* 1 after successful open */
};

volc_asr_stream_t* volc_asr_stream_open(void)
{
    struct asr_open_timing_s timing;
    uint32_t open_t0;
    uint32_t init_ms;
    uint32_t upgrade_ms;
    uint32_t request_ms;
    uint32_t mark;

    memset(&timing, 0, sizeof(timing));
    open_t0 = asr_now_ms();

    volc_asr_init();
    init_ms = asr_now_ms() - open_t0;

    if (s_app_id[0] == '\0' || s_token[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: credentials not configured\n",
            TAG);
        return NULL;
    }

    volc_asr_stream_t* s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }

    int ret = asr_tls_connect(&s->tls, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_PORT, &timing);
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    mark = asr_now_ms();
    ret = ws_upgrade(&s->tls, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_WS_PATH, s_token);
    upgrade_ms = asr_now_ms() - mark;
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    mark = asr_now_ms();
    ret = send_full_client_request(&s->tls, s_app_id, s_token,
        s_cluster);
    request_ms = asr_now_ms() - mark;
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    s->ready = 1;

    /* One line, every segment, in the order they happen.
     *
     * What each figure is for, now that the first three runs have narrowed the
     * question down:
     *
     *   dns        resolution alone, and the reason it is alone.  The pair it was
     *              part of measured 172, 171 and 791 ms, and a fourfold spread
     *              could have been a lookup or a lost SYN.  This says which.
     *              NuttX caches answers for an hour, so a large figure here twice
     *              in a row means the eight-entry cache is being evicted by the
     *              other hosts this device talks to.
     *   tcp        the connect alone.  Large here is the link, not the name --
     *              a retransmitted SYN, or an IOB pool with nothing left in it.
     *              On the proxy path this is the tunnel instead and dns is zero.
     *   handshake  read with the offered flag on the line above.  Small means a
     *              session was resumed; large with offered=yes means the server
     *              declined to.
     *   upgrade    one round trip plus the server's own work on it, and the
     *              cheapest estimate of the link's RTT this path produces.  Not
     *              something the device can shorten.
     *   request    should stay near zero: the frame is written and not waited on.
     */

    syslog(LOG_INFO,
        "[%s] stream: session opened in %lums "
        "(init=%lu seed=%lu dns=%lu %s=%lu handshake=%lu "
        "upgrade=%lu request=%lu)\n",
        TAG, (unsigned long)(asr_now_ms() - open_t0),
        (unsigned long)init_ms, (unsigned long)timing.seed_ms,
        (unsigned long)timing.dns_ms,
        timing.proxied ? "tunnel" : "tcp",
        (unsigned long)timing.tcp_ms,
        (unsigned long)timing.handshake_ms,
        (unsigned long)upgrade_ms, (unsigned long)request_ms);
    return s;
}

int volc_asr_stream_send(volc_asr_stream_t* s,
    const unsigned char* pcm, size_t len)
{
    if (!s || !s->ready || !pcm || len == 0) {
        return -EINVAL;
    }

    return send_audio_chunk(&s->tls, pcm, len, 0);
}

int volc_asr_stream_finish(volc_asr_stream_t* s,
    char* text_out, size_t text_cap)
{
    if (!s || !text_out || text_cap == 0) {
        free(s);
        return -EINVAL;
    }

    text_out[0] = '\0';

    if (!s->ready) {
        free(s);
        return -EINVAL;
    }

    /* Send empty last-chunk to signal end of audio */
    int ret = send_audio_chunk(&s->tls, (const unsigned char*)"",
        0, 1);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] stream: send last chunk failed: %d\n",
            TAG, ret);
        asr_tls_free(&s->tls);
        free(s);
        return ret;
    }

    /* Receive responses until final */
    int attempts = 0;

    while (attempts < 100) {
        ret = recv_volc_response(&s->tls, text_out, text_cap);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] stream: recv error: %d\n",
                TAG, ret);
            asr_tls_free(&s->tls);
            free(s);
            return ret;
        }
        if (ret == 1) {
            break;
        }
        attempts++;
    }

    asr_tls_free(&s->tls);
    free(s);

    if (text_out[0] == '\0') {
        syslog(LOG_WARNING, "[%s] stream: no text recognized\n",
            TAG);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] stream: recognized: %.80s\n",
        TAG, text_out);
    return 0;
}

void volc_asr_stream_abort(volc_asr_stream_t* s)
{
    if (!s) {
        return;
    }

    if (s->ready) {
        asr_tls_free(&s->tls);
    }

    free(s);
    syslog(LOG_INFO, "[%s] stream: aborted\n", TAG);
}

/* ── Ops-based wrapper (simplified signature for framework) ──── */

static int volc_asr_recognize_ops(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    /* Reload credentials in case they changed at runtime */
    volc_asr_init();

    if (s_app_id[0] == '\0' || s_token[0] == '\0') {
        syslog(LOG_ERR, "[%s] ASR credentials not configured\n", TAG);
        return -ENOENT;
    }

    return volc_asr_recognize(pcm_data, pcm_len,
        s_app_id, s_token, s_cluster,
        text_out, text_cap);
}

/* Backend ops registration */
static const voice_asr_ops_t s_volc_asr_ops = {
    .name = "volcengine",
    .init = volc_asr_init,
    .recognize = volc_asr_recognize_ops,
    .deinit = NULL,
};

int volc_asr_register(void)
{
    return voice_asr_register(&s_volc_asr_ops);
}
