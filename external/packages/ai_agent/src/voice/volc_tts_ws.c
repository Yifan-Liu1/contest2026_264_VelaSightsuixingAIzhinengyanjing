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
 * Doubao TTS via WebSocket (V1 binary protocol).
 *
 * Protocol: wss://openspeech.bytedance.com/api/v1/tts/ws_binary
 * Flow:
 *   1. TLS connect + HTTP Upgrade to WebSocket
 *   2. Send full_client_request (JSON: app/user/audio/request)
 *   3. Receive audio_only_server_response frames (raw PCM)
 *   4. Last frame has sequence < 0
 *
 * Binary frame format: see volc_asr.c header comment.
 */

#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/volc_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "volc_tts_ws";

#define TTS_WS_PATH "/api/v1/tts/ws_binary"

/* Volcengine binary protocol constants */
#define VOLC_PROTO_VER 0x11
#define VOLC_HDR_SIZE 8
#define VOLC_MSG_FULL_REQ 0x10
#define VOLC_MSG_AUDIO_RESP 0xB0 /* audio_only server response */
#define VOLC_MSG_FRONTEND 0xC0 /* frontend server response */
#define VOLC_MSG_ERROR 0xF0
#define VOLC_SER_JSON 0x10
#define VOLC_SER_JSON_GZ 0x11 /* JSON + gzip */
#define VOLC_SER_RAW 0x00

/* WebSocket constants */
#define WS_BUF_SIZE (32 * 1024)
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08

/* The service sends an unsolicited ping roughly every five seconds while a
 * session is open.  Both halves of handling it matter here:
 *
 *   - it must be answered, or the peer eventually drops the connection.  That
 *     is what "recv ended after 18 chunks (rc=-104)" was: not the end of the
 *     audio, but the server giving up on a client that never replied; and
 *   - it must not be mistaken for audio.  A ping arriving is not evidence that
 *     more speech is coming, so it cannot be allowed to reset the idle counter
 *     below -- doing so kept a finished stream open until the peer closed it,
 *     turning a four-second reply into a thirty-three-second wait.
 */

#define WS_OPCODE_PING 0x09
#define WS_OPCODE_PONG 0x0A
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

/* Credentials */
static char s_appid[64];
static char s_token[128];
static char s_cluster[64];
static char s_speaker[64];

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tts_tls_ctx_t;

/* ── Entropy ─────────────────────────────────────────────────── */

static int tts_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    syslog(LOG_ERR, "[volc_tts] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

/* ── TLS connect / free ──────────────────────────────────────── */

static void tts_tls_free(tts_tls_ctx_t* ctx);

static int tts_tls_connect(tts_tls_ctx_t* ctx, const char* host,
    const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, tts_entropy_func, NULL,
        (const unsigned char*)"volc_tts_ws", 11);
    if (ret != 0) {
        goto fail;
    }

    if (http_proxy_is_enabled()) {
        int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);

        if (tunnel_fd < 0) {
            ret = -ECONNREFUSED;
            goto fail;
        }

        ctx->net.fd = tunnel_fd;
    } else {
        ret = mbedtls_net_connect(&ctx->net, host, port, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            ret = -ECONNREFUSED;
            goto fail;
        }
    }

    mbedtls_net_set_block(&ctx->net);

    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ret = mbedtls_ssl_config_defaults(&ctx->cfg, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ret = -EIO;
        goto fail;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        ret = -EIO;
        goto fail;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send, mbedtls_net_recv,
        NULL);

    syslog(LOG_INFO, "[%s] Handshake start: %s:%s\n", TAG, host, port);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            ret = -EIO;
            goto fail;
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected\n", TAG);
    return 0;

fail:
    tts_tls_free(ctx);
    return ret;
}

static void tts_tls_free(tts_tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── TLS I/O helpers ─────────────────────────────────────────── */

static int tls_write_all(tts_tls_ctx_t* ctx, const unsigned char* buf,
    size_t len)
{
    size_t written = 0;

    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);

        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -EIO;
        }
    }

    return 0;
}

/* How many consecutive SO_RCVTIMEO expiries a partially read frame tolerates
 * before the connection is declared broken.  At the grain this code sets after
 * the first audio chunk that is a four-second budget for the remainder of one
 * frame -- far more than a server that has already started sending it should
 * need, and bounded so a dead peer cannot hang synthesis.
 */

#define TTS_PARTIAL_FRAME_STALLS 20

/* Read exactly len bytes, or fail without having consumed a partial frame.
 *
 * The distinction between a timeout at a frame boundary and one part-way
 * through a frame is the whole point of this function, because the two mean
 * opposite things and only one of them is safe to retry:
 *
 *   got == 0  Nothing was consumed, so the stream is still positioned on a
 *             frame boundary.  The server may simply have finished without
 *             sending a close frame, which is how this API detects
 *             end-of-stream.  -ETIMEDOUT tells the caller it may retry.
 *
 *   got > 0   Part of a frame has been consumed and TLS gives no way to push
 *             it back.  Retrying would resume in the middle of a frame body
 *             and read audio as a header: that is exactly what produced
 *             "WS frame too large: 4093638143" on hardware, 0xF3FFF5FF being
 *             two quiet PCM samples rather than any length.  So keep waiting
 *             for the rest of the frame, and if it never comes report -EPROTO,
 *             which the caller does not retry.
 */
static int tls_read_all(tts_tls_ctx_t* ctx, unsigned char* buf, size_t len)
{
    size_t got = 0;
    int stalls = 0;

    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);

        if (ret > 0) {
            got += (size_t)ret;
            stalls = 0;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            /* A close in the middle of a frame is a truncated frame, not a
             * clean end of stream. */
            return got > 0 ? -EPROTO : -ECONNRESET;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            /* SO_RCVTIMEO expiry reaches us as MBEDTLS_ERR_NET_RECV_FAILED
             * (-0x004C) with errno EAGAIN/EWOULDBLOCK; a real I/O failure does
             * not set those. */
            if (ret == -0x004C && (errno == EAGAIN || errno == EWOULDBLOCK
                                   || errno == ETIMEDOUT)) {
                if (got == 0) {
                    return -ETIMEDOUT;
                }

                if (++stalls > TTS_PARTIAL_FRAME_STALLS) {
                    syslog(LOG_ERR,
                        "[%s] frame stalled at %zu/%zu bytes\n",
                        TAG, got, len);
                    return -EPROTO;
                }

                continue;
            }

            return got > 0 ? -EPROTO : -EIO;
        }
    }

    return 0;
}

/* ── WebSocket upgrade ───────────────────────────────────────── */

static int ws_upgrade(tts_tls_ctx_t* ctx, const char* host, const char* path,
    const char* token)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    tts_entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len, key_raw,
        sizeof(key_raw));

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
        path, host, (int)key_b64_len, key_b64, token);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);

    if (ret != 0) {
        return ret;
    }

    char resp[1024];
    size_t rlen = 0;

    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);

        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        }
    }

    int status = 0;

    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (client must mask) ─────────────────── */

static int ws_send_frame(tts_tls_ctx_t* ctx, unsigned char opcode,
    const unsigned char* payload, size_t plen)
{
    unsigned char hdr[14];
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
        memset(hdr + 2, 0, 4);
        hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(plen & 0xFF);
        hdr_len = 10;
    }

    unsigned char mask[WS_MASK_KEY_LEN];

    tts_entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = tls_write_all(ctx, hdr, hdr_len);

    if (ret != 0) {
        return ret;
    }

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

/* ── WebSocket frame recv ────────────────────────────────────── */

static int ws_recv_frame(tts_tls_ctx_t* ctx, unsigned char* buf, size_t cap,
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

        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) | ((size_t)ext[6] << 8) | ext[7];
    }

    unsigned char mask_key[WS_MASK_KEY_LEN];

    if (masked) {
        ret = tls_read_all(ctx, mask_key, WS_MASK_KEY_LEN);
        if (ret != 0) {
            return ret;
        }
    }

    if (plen > cap) {
        syslog(LOG_ERR, "[%s] WS frame too large: %zu\n", TAG, plen);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) {
            return ret;
        }

        if (masked) {
            for (size_t i = 0; i < plen; i++) {
                buf[i] ^= mask_key[i % 4];
            }
        }
    }

    *out_len = plen;
    return 0;
}

/* ── Volcengine frame send ───────────────────────────────────── */

static int send_volc_frame(tts_tls_ctx_t* ctx, unsigned char msg_type,
    unsigned char serialization,
    const unsigned char* payload, size_t plen)
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

    int ret = ws_send_frame(ctx, WS_OPCODE_BINARY, frame,
        VOLC_HDR_SIZE + plen);

    free(frame);
    return ret;
}

/* RFC 4122 version 4, from the same entropy source as the WebSocket key.
 *
 * The request id used to be the fixed string "agent-tts".  The service treats
 * it as the identifier of one synthesis request -- the vendor's reference
 * client generates a fresh uuid per call, and volc_asr.c in this same tree
 * already does -- so reusing one value across every reply of every session
 * gives the backend no way to tell them apart.
 */

static void tts_generate_uuid(char* out, size_t cap)
{
    unsigned char rnd[16];

    tts_entropy_func(NULL, rnd, sizeof(rnd));
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

/* ── Build and send TTS full_client_request ──────────────────── */

static int send_tts_request(tts_tls_ctx_t* ctx, const char* text)
{
    char reqid[37]; /* 36 characters plus NUL */
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return -ENOMEM;
    }

    tts_generate_uuid(reqid, sizeof(reqid));

    /* app */
    cJSON* app = cJSON_AddObjectToObject(root, "app");

    cJSON_AddStringToObject(app, "appid", s_appid);
    cJSON_AddStringToObject(app, "token", s_token);
    cJSON_AddStringToObject(app, "cluster", s_cluster);

    /* user */
    cJSON* user = cJSON_AddObjectToObject(root, "user");

    cJSON_AddStringToObject(user, "uid", "agent");

    /* audio */
    cJSON* audio = cJSON_AddObjectToObject(root, "audio");

    cJSON_AddStringToObject(audio, "voice_type", s_speaker);
    cJSON_AddStringToObject(audio, "encoding", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", AGENT_TTS_WS_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "speed_ratio", 1.0);

    /* request */
    cJSON* req = cJSON_AddObjectToObject(root, "request");

    cJSON_AddStringToObject(req, "reqid", reqid);
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "text_type", "plain");
    cJSON_AddStringToObject(req, "operation", "submit");

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (!json_str) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] TTS request: reqid=%s text=%zu bytes\n",
        TAG, reqid, strlen(text));

    int ret = send_volc_frame(ctx, VOLC_MSG_FULL_REQ, VOLC_SER_JSON,
        (const unsigned char*)json_str, strlen(json_str));
    free(json_str);
    return ret;
}

/* ── Receive audio responses ─────────────────────────────────── */

static int recv_tts_audio(tts_tls_ctx_t* ctx, volc_tts_chunk_cb cb,
    void* user_data, volc_tts_cancel_cb cancel, void* cancel_data)
{
    unsigned char* buf = malloc(WS_BUF_SIZE);

    if (!buf) {
        return -ENOMEM;
    }

    /* Recv timeout strategy:
     * - First chunk: keep the handshake timeout (10s) since TTS synthesis
     *   latency varies with text length and server load (200ms–2s typical).
     * - After first chunk: tighten to 500ms per recv.  To tolerate
     *   transient network stalls without silently truncating audio,
     *   allow up to 3 consecutive timeouts (~1.5s total) before
     *   declaring end-of-stream.  A single stall just retries.
     * The initial 10s timeout was set by tts_tls_connect(), so we only
     * need to tighten it after the first audio chunk arrives. */

/* Idle grain and budget for giving up on a stream that has stopped talking.
 *
 * This is a fault path, not the normal end of a reply.  A session ends either
 * on an audio frame whose sequence number is negative, or on the peer closing
 * the connection, and both are detected below and break out immediately.  The
 * budget only decides how long to wait when neither happens.
 *
 * It is therefore sized for the worst legitimate gap, not for a quick exit.
 * The previous 2.4 seconds was sized the other way round and truncated
 * replies: measured on hardware a 60-byte answer yielded 1.83 s of audio where
 * about 4.5 s was due, and every one of those sessions also reported "0
 * frontend" -- the epilogue that follows the audio never arrived either,
 * because the client had already stopped listening.  Shortening this budget
 * cannot make a good reply faster (the terminator ends it), but it can and did
 * cut good replies short.
 */

#define TTS_IDLE_GRAIN_US 500000
#define TTS_MAX_CONSECUTIVE_TIMEOUTS 20

    int chunks = 0;
    int consecutive_timeouts = 0;
    int err = 0;

    /* Enough to say why the loop ended, without a line per frame.
     *
     * Ending on the idle timeout costs a fixed five seconds every reply, and
     * it is only reached because neither documented terminator is being seen:
     * a negative sequence number on the last audio frame, or a frontend
     * message after the audio.  Whether the server omits them or this parser
     * misreads them cannot be told apart without knowing what arrived, so the
     * counts and the last sequence number are reported once at the end.
     */

    size_t audio_bytes = 0;
    int32_t last_seq = 0;
    int pings = 0;
    int frontend = 0;
    int acks = 0;
    int last_flags = -1;
    int skipped = 0;
    const char* ended = "idle timeout";

    while (1) {
        size_t flen;
        int opcode;
        int ret;

        /* Checked before receiving rather than after, so a round abandoned
         * while the server is still thinking costs nothing more. */

        if (cancel != NULL && cancel(cancel_data)) {
            syslog(LOG_INFO, "[%s] cancelled after %d chunk(s)\n", TAG,
                chunks);
            break;
        }

        ret = ws_recv_frame(ctx, buf, WS_BUF_SIZE, &flen, &opcode);

        if (ret != 0) {
            /* Timeout after audio started: retry up to N times to
             * tolerate transient stalls.  Only declare EOF after
             * consecutive timeouts exceed the threshold (~1.5s). */
            if (chunks > 0 && ret == -ETIMEDOUT) {
                /* Not logged per grain.  At twenty grains that was
                 * twenty lines per reply on a console this project shares with
                 * the UI's frame budget, and the summary below carries the
                 * same information.
                 */

                consecutive_timeouts++;
                if (consecutive_timeouts < TTS_MAX_CONSECUTIVE_TIMEOUTS) {
                    continue;
                }

                break;
            }

            /* Peer close after audio started is normal EOF. */
            if (chunks > 0 && ret == -ECONNRESET) {
                ended = "peer closed";
                break;
            }

            syslog(LOG_ERR, "[%s] recv error after %d chunk(s): %d\n", TAG,
                chunks, ret);
            err = ret;
            break;
        }

        /* Control frames are handled before the idle counter is touched,
         * because they say nothing about whether more audio is coming. */

        if (opcode == WS_OPCODE_PING) {
            /* Echo the payload back, as RFC 6455 requires. */

            pings++;
            (void)ws_send_frame(ctx, WS_OPCODE_PONG, buf, flen);
            continue;
        }

        if (opcode == WS_OPCODE_PONG) {
            continue;
        }

        if (opcode == WS_OPCODE_CLOSE) {
            ended = "close frame";
            break;
        }

        /* A data frame, and only a data frame, means the stream is alive. */

        consecutive_timeouts = 0;

        if (flen < 4) {
            skipped++;
            continue;
        }

        unsigned char msg_type = buf[1] & 0xF0;
        unsigned char msg_flags = buf[1] & 0x0F;
        size_t volc_hdr_len = (size_t)(buf[0] & 0x0F) * 4;


        if (volc_hdr_len < 4 || flen < volc_hdr_len) {
            skipped++;
            continue;
        }

        /* Error response */
        if (msg_type == VOLC_MSG_ERROR) {
            uint32_t code = 0;

            if (flen >= volc_hdr_len + 4) {
                code = ((uint32_t)buf[volc_hdr_len] << 24) | ((uint32_t)buf[volc_hdr_len + 1] << 16) | ((uint32_t)buf[volc_hdr_len + 2] << 8) | (uint32_t)buf[volc_hdr_len + 3];
            }

            syslog(LOG_ERR, "[%s] server error: %lu\n", TAG, (unsigned long)code);
            err = -EIO;
            break;
        }

        /* Frontend response (e.g. duration info) — signals end of audio
         * when it arrives after audio chunks have been received. */
        if (msg_type == VOLC_MSG_FRONTEND) {
            frontend++;
            if (chunks > 0) {
                ended = "frontend epilogue";
                break;
            }
            continue;
        }

        /* Audio-only response (0xB) */
        if (msg_type == VOLC_MSG_AUDIO_RESP) {
            /* flags: 0=ack(no audio), 1+=has audio data */
            if (msg_flags == 0) {
                acks++;
                continue; /* ACK, no audio data */
            }

            /* After volc header: 4-byte sequence (signed) + 4-byte payload_size */
            size_t audio_off = volc_hdr_len + 8;

            if (flen <= audio_off) {
                skipped++;
                continue;
            }

            /* Extract sequence as signed 32-bit (big-endian).
             * Per Volcengine binary protocol: sequence < 0 means last frame. */
            int32_t seq = (int32_t)(
                ((uint32_t)buf[volc_hdr_len] << 24) |
                ((uint32_t)buf[volc_hdr_len + 1] << 16) |
                ((uint32_t)buf[volc_hdr_len + 2] << 8) |
                (uint32_t)buf[volc_hdr_len + 3]);

            unsigned char* pcm = buf + audio_off;
            size_t pcm_len = flen - audio_off;

            cb(pcm, pcm_len, 0, user_data);
            chunks++;
            audio_bytes += pcm_len;
            last_seq = seq;
            last_flags = msg_flags;

            /* After first chunk, tighten recv timeout so we detect
             * end-of-stream quickly (server may not send close frame). */
            if (chunks == 1 && ctx->net.fd >= 0) {
                struct timeval tv = { .tv_sec = 0,
                    .tv_usec = TTS_IDLE_GRAIN_US };
                if (setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
                        &tv, sizeof(tv)) < 0) {
                    syslog(LOG_WARNING,
                        "[%s] setsockopt SO_RCVTIMEO failed: %d\n",
                        TAG, errno);
                }
            }

            /* Negative sequence marks the final audio frame.  Per the
             * vendor's reference client this is the normal terminator, and
             * flags 2 and 3 both carry it.
             */

            if (seq < 0) {
                ended = "final sequence";
                break;
            }
        }
    }

    free(buf);

    /* Audio that already reached the speaker is not a failed request.  The
     * caller's only recovery would be to synthesise the same text again, on
     * top of what the user has already heard, and reporting a hard error made
     * the screen say "系统操作失败" about a reply the user had just listened
     * to.  Report the truncation and let the round stand. */

    if (chunks > 0 && err != 0) {
        syslog(LOG_WARNING, "[%s] audio truncated after %d chunk(s): %d\n",
            TAG, chunks, err);
        err = 0;
    }

    if (chunks > 0) {
        cb(NULL, 0, 1, user_data);
    }

    syslog(LOG_INFO,
        "[%s] %s: %d chunk(s) %zu byte(s) (%lu ms audio), last seq %ld "
        "flags %d, %d ping(s) %d ack(s) %d frontend %d skipped\n",
        TAG, ended, chunks, audio_bytes,
        (unsigned long)(audio_bytes * 1000u
            / (AGENT_TTS_WS_SAMPLE_RATE * 2u)),
        (long)last_seq, last_flags, pings, acks, frontend, skipped);

    if (chunks == 0 && err == 0) {
        return -EPROTO;
    }

    return err;
}

/* ── Init credentials ────────────────────────────────────────── */

/* Cleared by volc_tts_ws_invalidate(), set once the keys have been read.
 *
 * Each claw_config_get() parses the config store's JSON file off SD-NAND, and
 * doing that four times before every sentence measured 1.4 s on hardware --
 * spent between the model's reply arriving and the TTS request going out, where
 * the user is waiting in silence.  The values only change when provisioning
 * changes, which has an explicit invalidation path.
 */

static bool s_creds_valid;

void volc_tts_ws_invalidate(void)
{
    s_creds_valid = false;
}

static void tts_ws_init(void)
{
    if (s_creds_valid) {
        return;
    }

    memset(s_appid, 0, sizeof(s_appid));
    memset(s_token, 0, sizeof(s_token));
    memset(s_cluster, 0, sizeof(s_cluster));
    memset(s_speaker, 0, sizeof(s_speaker));

    claw_config_get(AGENT_CFG_KEY_VOLC_APPKEY, s_appid, sizeof(s_appid));
    claw_config_get(AGENT_CFG_KEY_VOLC_TOKEN, s_token, sizeof(s_token));

    if (claw_config_get(AGENT_CFG_KEY_VOLC_CLUSTER, s_cluster,
            sizeof(s_cluster))
            != OK
        || s_cluster[0] == '\0') {
        strncpy(s_cluster, AGENT_VOICE_DEFAULT_CLUSTER, sizeof(s_cluster) - 1);
    }

    if (claw_config_get(AGENT_CFG_KEY_VOLC_SPEAKER, s_speaker,
            sizeof(s_speaker))
            != OK
        || s_speaker[0] == '\0') {
        strncpy(s_speaker, AGENT_VOICE_DEFAULT_SPEAKER, sizeof(s_speaker) - 1);
    }

    /* Only cache a usable set.  Caching an empty app_id would make a device
     * that started before provisioning finished stay broken until it rebooted.
     */

    s_creds_valid = s_appid[0] != '\0' && s_token[0] != '\0';
}

/* ── Public API ──────────────────────────────────────────────── */

int volc_tts_ws_synthesize_stream(const char* text, volc_tts_chunk_cb cb,
    void* user_data)
{
    return volc_tts_ws_synthesize_stream_cancellable(text, cb, user_data,
        NULL, NULL);
}

int volc_tts_ws_synthesize_stream_cancellable(const char* text,
    volc_tts_chunk_cb cb, void* user_data, volc_tts_cancel_cb cancel,
    void* cancel_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    tts_ws_init();

    if (s_appid[0] == '\0' || s_token[0] == '\0') {
        syslog(LOG_ERR, "[%s] credentials not configured\n", TAG);
        return -ENOENT;
    }

    tts_tls_ctx_t ctx;
    int ret = tts_tls_connect(&ctx, AGENT_DOUBAO_TTS_HOST, AGENT_DOUBAO_TTS_PORT);

    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = ws_upgrade(&ctx, AGENT_DOUBAO_TTS_HOST, TTS_WS_PATH, s_token);
    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = send_tts_request(&ctx, text);
    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = recv_tts_audio(&ctx, cb, user_data, cancel, cancel_data);

    tts_tls_free(&ctx);
    return ret;
}
