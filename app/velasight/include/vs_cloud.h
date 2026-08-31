/****************************************************************************
 * app/velasight/include/vs_cloud.h
 *
 * Device side of the team's own social-session cloud, its /contest/v1
 * interface.  This is not the MiMo/Volcengine model service vs_voice.c talks
 * to; it is the project's own server, and the two have nothing in common
 * beyond both being reached over TLS.
 *
 * Protocol source
 * ---------------
 * "VelaSight 云端接口" (feishu wiki YZVJwQ3raithMZkDcBYcI0SFnnb), section 四
 * 云端实现 - 对设备端.  Four HTTP entry points plus one direct transfer to
 * object storage:
 *
 *   PUT    /contest/v1/session     open a session
 *   POST   /contest/v1/upload      register one file, receive a presigned URL
 *   PUT    <presignedUrl>          the file bytes themselves, straight to FDS
 *   GET    /contest/v1/getResult   poll one or more msgIds
 *   DELETE /contest/v1/session     ask for the session to be closed
 *
 * Every request and response above is application/json and carries
 * deviceId, sessionId and timestamp.  Responses wrap their payload in a
 * "value" member: an object for session/upload/close, an array of per-message
 * results for getResult.
 *
 * The paths above are what the interface document writes, and they are not the
 * whole path on the wire.  A configurable prefix goes in front of all four --
 * "/hlthopen/public" for the staging deployment, empty for the document's own
 * 127.0.0.1:18080 examples -- because the deployment does not host the
 * interface at the root the examples assume, and answers the unprefixed path
 * with an empty 204.  The prefix is applied in one place, cloud_api_call(), so
 * the literals here keep the document's spelling.
 *
 * Where the endpoint comes from
 * -----------------------------
 * Host, port and prefix are resolved once by vs_cloud_init() and re-resolved
 * by vs_cloud_reload_endpoint() after a successful Web save.  Three sources,
 * highest priority first:
 *
 *   1. the Web provisioning record, field by field
 *   2. CONFIG_VS_SOCIAL_CLOUD_HOST/PORT, when the host is not empty
 *   3. CONFIG_VELASIGHT_PROVISION_CLOUD_HOST/_PATH/_PORT, the factory default
 *
 * Two consequences worth stating.  A device that has never been provisioned
 * still has a working address, so "unconfigured" is a build that deliberately
 * blanked the factory default rather than the normal state of a new board.
 * And the record is never read on a request path: a session registers an
 * upload several times a second, and a file read there would put VFAT I/O
 * between a captured frame and the socket.
 *
 * The scheme is not in the record.  It stays a build option because it is not
 * something a user can get right from a phone -- see
 * CONFIG_VS_SOCIAL_CLOUD_TLS.
 *
 * Deltas from VELASIGHT_SOCIAL_MODE_INTEGRATION_PLAN.md V1
 * -------------------------------------------------------
 * That plan was written against an earlier revision of the cloud document and
 * left four questions open (its section 5.2).  The revision this file
 * implements answers three of them, so the code here deliberately differs
 * from the plan:
 *
 *   - Upload is *not* multipart.  /contest/v1/upload is a plain JSON
 *     registration that returns presignedUrl, and the bytes go to that URL as
 *     a bare body.  The plan's "insert a multipart writer here" branch is
 *     therefore not implemented, and should not be added.
 *   - Audio is Ogg.  The document's own example presigned URLs end in .jpg
 *     for images and .ogg for audio, which settles the format question in
 *     favour of app/audio_test/audio_test_ogg.c's Opus encoder over raw PCM.
 *   - Session close completes at peer status 21 on msgEvent 2, not 50.  The
 *     document states this explicitly ("约定会话关闭完毕用 21 表示").  50 is
 *     a server-side code that reaches the cloud from the AI side; the device
 *     never sees it on a healthy path.
 *
 * The fourth question, an acknowledgement interface, is still unanswered
 * because the cloud still has no such endpoint.  vs_cloud_social_ack() stays
 * a local no-op; see its comment.
 *
 * Threading
 * ---------
 * Every call here blocks for a network round trip and must not run on the UI
 * task.  The five session operations are safe to call concurrently from
 * different threads: the session identifiers travel as arguments, response
 * buffers are drawn per call, and the connection pool in packages/ai_agent's
 * vela_tls.c takes its own lock.
 *
 * Two qualifications on that.  Concurrent calls are safe but not fully
 * independent -- on the TLS path they still contend for vela_tls.c's one or
 * two static response buffers; see the note at the top of vs_cloud.c.  And
 * there is a little module state after all: a once-only log flag for
 * untransferable presigned URLs, whose worst failure is a duplicated log
 * line, and the device identifier, which vs_cloud_social_open() may still be
 * resolving on its first call.  Neither is locked, and both are only written
 * from the startup task or from a session open, never from an upload or a
 * poll.  If session opens ever become concurrent, that changes.
 *
 * vs_cloud_init() and vs_cloud_deinit() are not re-entrant, and both are
 * meant to be called once from the startup path.
 *
 * Errors common to every operation
 * --------------------------------
 * Besides what each prototype lists:
 *
 *   -ENODATA  no endpoint configured (CONFIG_VS_SOCIAL_CLOUD_HOST empty)
 *   -EPROTO   the response was not the documented shape
 *   -ESTALE   the cloud echoed a different sessionId than the one sent, which
 *             means this session has been superseded -- one deviceId may hold
 *             only one live session and a later open replaces an earlier one.
 *             Stop feeding the old session; its results are gone.
 *   -E2BIG    a response, or an identifier or URL within it, did not fit its
 *             buffer.  Raise the relevant CONFIG_VS_SOCIAL_* budget.
 *   -ECONNREFUSED / -ETIMEDOUT / -EHOSTUNREACH / -EIO   transport failures
 *
 * A note on TLS, for whoever turns CONFIG_VS_SOCIAL_CLOUD_TLS on: the
 * cleartext path sends the port in the Host header when it is not the scheme
 * default, because an object store may have signed it.  vela_tls.c does not,
 * ever.  A presigned transfer over HTTPS to a non-443 port is therefore the
 * one case where the two transports differ in a way the server can see.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_CLOUD_H
#define __APP_VELASIGHT_INCLUDE_VS_CLOUD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vs_types.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Identifier widths.  deviceId is ours to choose and is derived from the
 * wlan0 MAC; sessionId is "vs-264-" plus eight hex digits; msgId is assigned
 * by the cloud and is a small decimal in every example, so the generous width
 * is only there so a cloud that switches to a UUID does not silently
 * truncate.
 */

#define VS_CLOUD_DEVICE_ID_MAX  32
#define VS_CLOUD_SESSION_ID_MAX 24
#define VS_CLOUD_MSG_ID_MAX     40

/* Where the endpoint comes from, for a log line and for vs_social.c's
 * "cannot start" message.  Which one is in force is not obvious from the
 * outside and is the first thing worth knowing when the cloud is unreachable:
 * a device pointed at a stale provisioned host looks exactly like a device
 * with a broken network until you can see that the record won.
 */

enum vs_cloud_origin_e
{
  VS_CLOUD_ORIGIN_NONE = 0,    /* nothing usable; every call is -ENODATA */
  VS_CLOUD_ORIGIN_DEFAULT,     /* the compiled-in factory endpoint */
  VS_CLOUD_ORIGIN_PROVISIONED  /* the Web provisioning record */
};

/* Enough for the document's own ttsMinutes shape
 * ("xxx/contest/api/session/20261001/xxx") with room for a real host and a
 * short query.  Presigned upload URLs can be much longer than this, but they
 * are consumed inside vs_cloud.c and never handed back to a caller, so they
 * are not bound by this.
 */

#define VS_CLOUD_URL_MAX 512

/* The "event" field of an upload registration.  The document defines only
 * 0 (emotion recognition) and marks the field reserved for future event
 * types, so it exists here as a named constant rather than an enum.
 */

#define VS_CLOUD_EVENT_EMOTION 0

/* How many identifiers one vs_cloud_social_poll_event() may carry.
 *
 * Public because a caller has to size its own arrays from it: the request is
 * built from msg_count identifiers and the response can hold more entries than
 * that -- an extreme frame yields both an image result and, later, the advice
 * derived from it, under the same msgId.  A caller that guessed this number
 * would either waste stack or get -E2BIG from a layer below the one that
 * documents the limit.
 */

#define VS_CLOUD_POLL_MAX_IDS 16

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Server-side status, the code the cloud assigns a message when the AI side
 * reports on it.
 *
 * The device does not normally see these: getResult returns the peer-side
 * code below.  They are declared because the two sets share values with
 * different meanings -- 20, 21 and 30 all mean something different depending
 * on which side is speaking -- and the integration plan requires them to be
 * separate C types so the two can never be compared by accident.  They are
 * also the fallback path in vs_cloud_server_to_peer(), for a cloud build that
 * leaks a server code through the device-facing interface.
 *
 * Source: cloud document section 三.流程设计 / 获取结果 / 状态流转, the
 * 服务端状态 column.
 */

enum vs_cloud_server_state_e
{
  VS_CLOUD_SRV_SESSION_OPEN       = 0,  /* session just created */
  VS_CLOUD_SRV_IMAGE_PENDING      = 10, /* image queued, no result yet */
  VS_CLOUD_SRV_AUDIO_PENDING      = 11, /* audio queued, no result yet */
  VS_CLOUD_SRV_IMAGE_CALM         = 20, /* image judged non-extreme */
  VS_CLOUD_SRV_IMAGE_EXTREME      = 21, /* image judged extreme -> audio */
  VS_CLOUD_SRV_AUDIO_DONE         = 22, /* audio advice produced */
  VS_CLOUD_SRV_IMAGE_NO_FACE      = 30, /* no usable face in the image */
  VS_CLOUD_SRV_IMAGE_UNRECOGNIZED = 31, /* image could not be read */
  VS_CLOUD_SRV_AUDIO_NO_SPEECH    = 32, /* no usable speech in the audio */
  VS_CLOUD_SRV_AUDIO_UNRECOGNIZED = 33, /* audio could not be read */
  VS_CLOUD_SRV_CLOSING            = 40, /* close accepted, still working */
  VS_CLOUD_SRV_CLOSED             = 50  /* close finished (AI side code) */
};

/* Peer-side status, the code that actually arrives in a getResult entry.
 *
 * The mapping collapses detail on purpose: the cloud's 20 (calm) and 21
 * (extreme) both arrive as 20, and all four failure codes arrive as 30.  A
 * consequence worth stating because it is easy to get wrong: the device
 * cannot tell an extreme frame from a calm one by status alone.  See
 * vs_social_event_s::extreme for what it can use instead.
 *
 * Source: same table, the 对端侧状态 column.
 */

enum vs_cloud_peer_state_e
{
  VS_CLOUD_PEER_SESSION_OPEN      = 0,  /* session acknowledged */
  VS_CLOUD_PEER_EMOTION_ANALYZING = 10, /* image still being analysed */
  VS_CLOUD_PEER_ADVICE_PENDING    = 11, /* advice still being generated */
  VS_CLOUD_PEER_EMOTION_DONE      = 20, /* image result attached */

  /* Advice attached, and -- on msgEvent 2 -- the session's final minutes. */

  VS_CLOUD_PEER_ADVICE_DONE       = 21,
  VS_CLOUD_PEER_FAILED            = 30, /* value.log carries the reason */
  VS_CLOUD_PEER_CLOSING           = 40  /* close accepted, still working */
};

/* Which handler a getResult entry belongs to.  Sent by the cloud as
 * "msgEvent"; the same field name in an upload registration says what kind of
 * file is being registered, which is why the two enums below overlap in value
 * but are declared separately.
 */

enum vs_cloud_msg_event_e
{
  VS_CLOUD_MSG_EVENT_IMAGE = 0, /* per-frame emotion result */
  VS_CLOUD_MSG_EVENT_AUDIO = 1, /* advice derived from audio */
  VS_CLOUD_MSG_EVENT_CLOSE = 2  /* end-of-session minutes */
};

/* What is being uploaded.  Decides the file extension the cloud picks, and
 * the Content-Type of the transfer to the presigned URL.
 */

enum vs_cloud_media_e
{
  VS_CLOUD_MEDIA_IMAGE = 0, /* JPEG */
  VS_CLOUD_MEDIA_AUDIO = 1  /* Ogg */
};

/* Identity of one session.  Both fields travel in every request.  device_id
 * is filled by vs_cloud_init() and is stable for the life of the process;
 * session_id is drawn per session by vs_cloud_new_session_id().
 */

struct vs_cloud_session_s
{
  char device_id[VS_CLOUD_DEVICE_ID_MAX];
  char session_id[VS_CLOUD_SESSION_ID_MAX];
};

/* One file to hand to the cloud.  data is borrowed for the duration of the
 * call and is neither retained nor freed here.
 */

struct vs_cloud_media_packet_s
{
  enum vs_cloud_media_e type;
  const unsigned char  *data;
  size_t                len;

  /* Per-type counter, for logging only.  The cloud names files itself, so
   * nothing in the protocol depends on this.
   */

  uint32_t sequence;
};

/* Result of one upload.
 *
 * payload_sent deserves an explanation.  Registration and transfer are two
 * separate exchanges, and the second one can be impossible while the first
 * succeeded: the cloud's own mock mode answers with a presignedUrl of the
 * form "mock://contest/s1/1.jpg", which is not a scheme anything can PUT to.
 * Failing the whole call there would make bring-up against the mock server
 * impossible, so the registration's msg_id is returned with payload_sent
 * false.  The msgId is still valid and still worth polling -- against a mock
 * cloud the result does not depend on the bytes arriving.  A caller that
 * needs to know whether real data moved must check this flag.
 */

struct vs_cloud_upload_s
{
  char msg_id[VS_CLOUD_MSG_ID_MAX];
  bool payload_sent;
};

/* One entry from a getResult response, already mapped onto UI vocabulary.
 *
 * Which members are meaningful depends on msg_event and peer_state:
 *
 *   IMAGE + EMOTION_DONE   emotion, color, confidence, extreme, display_text
 *   AUDIO + ADVICE_DONE    suggestion
 *   CLOSE + ADVICE_DONE    nothing here; call vs_cloud_social_get_result()
 *   anything + FAILED      log
 *   anything else          nothing; the message is still in flight
 */

struct vs_social_event_s
{
  char msg_id[VS_CLOUD_MSG_ID_MAX];
  enum vs_cloud_msg_event_e   msg_event;
  enum vs_cloud_peer_state_e  peer_state;

  /* The status exactly as received, before mapping.  Kept so a log line can
   * show what the cloud actually said rather than what it was understood as.
   */

  int raw_status;

  bool has_response;

  enum vs_emotion_e emotion;

  /* 0xRRGGBB, from the cloud's emotionColor bucket, matched to the palette
   * vs_app.c uses for its own defaults.
   */

  uint32_t color;

  /* confidence as whole percent, 0..100.  The wire format is a string
   * ("0.95"); a bare number is accepted too.  101 means absent.
   */

  uint16_t confidence;

  /* True when the frame is one the cloud considers an extreme emotion, which
   * is what the UI raises an alert for.
   *
   * This cannot come from peer_state, which collapses calm and extreme into
   * 20.  It is derived from the emotionColor bucket instead: red (生气,
   * 反感) is extreme, blue and green are not.  Independently, the cloud only
   * starts an audio-advice chain for a frame it judged extreme, so a
   * subsequent AUDIO entry for the same msgId corroborates it.
   */

  bool extreme;

  /* emotionDetail for an image entry, advice for an audio entry.  Truncated
   * on a UTF-8 boundary at VS_TEXT_LONG - 1.
   */

  char display_text[VS_TEXT_LONG];

  /* advice, on an audio entry.  Same content as display_text there; kept as
   * its own member so the alert path can hold a frame's emotion and the
   * advice that arrived later side by side.
   */

  char suggestion[VS_TEXT_LONG];

  /* value.log on a failure.  Empty otherwise. */

  char log[VS_TEXT_SHORT];
};

/* The end-of-session payload: everything a CLOSE entry carries once it
 * reaches ADVICE_DONE.
 *
 * calm/happy/tense are computed here from emotionTimeline rather than sent by
 * the cloud, because that is the shape vs_history_index_s wants.  The three
 * buckets come from the cloud's three colours: green 中立 is calm, green 愉悦
 * is happy, and both red and blue fold into tense -- blue covers 害怕, 伤心,
 * 疑惑 and 惊讶, none of which belong under calm or happy in a three-way
 * split.  They sum to 100 unless the timeline was empty, in which case all
 * three are zero.
 */

struct vs_cloud_minutes_s
{
  /* ttsMinutes.  A download URL; fetch it with vs_cloud_download(). */

  char tts_url[VS_CLOUD_URL_MAX];

  /* txtMinutes, truncated on a UTF-8 boundary for the result page.  The
   * untruncated text is in the full JSON, which is what gets persisted.
   */

  char summary[VS_TEXT_LONG];

  uint8_t calm;
  uint8_t happy;
  uint8_t tense;

  uint16_t emotion_samples; /* entries in emotionTimeline */
  uint16_t audio_samples;   /* entries in audioTimeline */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: vs_cloud_init
 *
 * Description:
 *   Resolve this device's identifier and the endpoint to use.  Performs no
 *   network I/O, so it is safe on the startup path and cheap before Wi-Fi is
 *   up.
 *
 *   The endpoint is read from the Web provisioning record, falling back to
 *   the compiled-in default when the record carries none.  It does not read
 *   the record on every request: the resolved endpoint is cached in module
 *   state, and vs_cloud_reload_endpoint() is the only other thing that reads
 *   SD-NAND.  That is deliberate -- a session registers an upload several
 *   times a second, and a file read on that path would put VFAT I/O between
 *   a captured frame and the socket.
 *
 *   Reading the record here does mean this call touches SD-NAND once, so it
 *   belongs after the card is mounted.  It does not wait for the mount
 *   itself; a call made too early resolves to the default endpoint and logs
 *   which one it picked, and vs_cloud_reload_endpoint() corrects it later.
 *
 *   The identifier is "bk7258-" plus the wlan0 MAC in lower-case hex, so it
 *   is stable across reboots without needing to be stored.  If the interface
 *   cannot be queried -- which it cannot before the driver registers --
 *   CONFIG_VS_SOCIAL_DEVICE_ID is used when set, and failing that a
 *   TRNG-drawn value that lasts only for this boot.  Since it is only ever
 *   compared against itself within one session, a per-boot value is
 *   serviceable; it just makes cloud-side logs harder to follow across
 *   reboots, so it is logged when it happens.
 *
 * Returned Value:
 *   0 on success, or -ENODATA when no endpoint is configured.  The device
 *   identifier is resolved either way, so a caller that only wants
 *   vs_cloud_device_id() can ignore the error.
 *
 ****************************************************************************/

int vs_cloud_init(void);

/* True when a host is resolved and the six operations may be attempted.
 * With no host they all fail with -ENODATA without touching the network.
 *
 * Normally true, because the factory default is a real address: it takes both
 * an empty provisioned host and an empty CONFIG_VELASIGHT_PROVISION_CLOUD_HOST
 * to end up unconfigured, which is a build that deliberately ships without an
 * endpoint rather than an accident.
 */

bool vs_cloud_configured(void);

/* This device's identifier, valid after vs_cloud_init().  Never NULL. */

const char *vs_cloud_device_id(void);

/****************************************************************************
 * Name: vs_cloud_reload_endpoint
 *
 * Description:
 *   Re-read the endpoint from the provisioning record.  Called from the same
 *   place vs_voice_reload_credentials() is, after a successful Web save.
 *
 *   Refuses to change anything while a session is open, and says so with
 *   -EBUSY.  Swapping the host under a live session would strand it: the
 *   in-flight msgIds only exist on the old host, so the poll loop would ask
 *   the new one about identifiers it never issued and read the answers as
 *   failures.  The caller does not need to retry -- the next
 *   vs_cloud_social_open() picks up whatever is stored then.
 *
 * Returned Value:
 *   1 when the endpoint changed, 0 when it is the same as before, -EBUSY
 *   while a session is open, or a negative errno from the record.
 *
 ****************************************************************************/

int vs_cloud_reload_endpoint(void);

/* Which source the live endpoint came from.  For logs and for the message
 * social mode shows when it cannot start.
 */

enum vs_cloud_origin_e vs_cloud_origin(void);

/* The live endpoint, for a log line or a status page.  host is never NULL but
 * may be empty when nothing is configured; base_path may be empty, which
 * means the interface sits at the document root.
 */

void vs_cloud_endpoint(const char **host, uint16_t *port,
                       const char **base_path, bool *tls);

/****************************************************************************
 * Name: vs_cloud_new_session_id
 *
 * Description:
 *   Draw a session identifier: "vs-264-" plus eight hex digits from
 *   /dev/urandom, which this board seeds from the hardware TRNG.
 *
 *   Deliberately not derived from the clock.  The board has no RTC and may
 *   not have been through SNTP, so two sessions after two reboots could
 *   otherwise present the same timestamp and the cloud would treat the second
 *   as a repeat of the first.
 *
 * Returned Value:
 *   0 on success, -EINVAL for a short buffer, or a negative errno from the
 *   random source.
 *
 ****************************************************************************/

int vs_cloud_new_session_id(char *out, size_t cap);

/****************************************************************************
 * Name: vs_cloud_social_open
 *
 * Description:
 *   PUT /contest/v1/session.  session->device_id is filled in from
 *   vs_cloud_device_id() and session->session_id is drawn when empty, so the
 *   normal caller passes a zeroed structure and reads both back out.
 *
 * Returned Value:
 *   0 when the cloud accepted the session (status 0).
 *   -EBUSY  the cloud refused.  Status 30 is this endpoint's only failure
 *           code and it covers both "this deviceId already has a session with
 *           results outstanding" and any server-side error, so treat -EBUSY
 *           as "do not immediately retry" rather than as a precise diagnosis;
 *           the log line carries what the cloud actually said.
 *
 ****************************************************************************/

int vs_cloud_social_open(struct vs_cloud_session_s *session);

/****************************************************************************
 * Name: vs_cloud_social_upload
 *
 * Description:
 *   Register one file with POST /contest/v1/upload and transfer its bytes to
 *   the presigned URL that comes back.  Two round trips, both blocking.
 *
 *   The transfer is a bare PUT: body is the file, with no multipart framing.
 *   Whether it carries a Content-Type is a build option
 *   (CONFIG_VS_SOCIAL_UPLOAD_CONTENT_TYPE), because object stores differ on
 *   whether the header was covered by the signature -- sending one the
 *   signature did not cover is rejected, and so is omitting one it did.
 *
 * Returned Value:
 *   0 on success.  See vs_cloud_upload_s::payload_sent for the case where
 *   registration succeeded but the URL could not be used.
 *   -EINVAL   empty packet, or a session id that is not usable
 *   -EPROTO   the response had no msgId, or a msgId or presigned URL too long
 *             for its buffer -- rejected rather than truncated, since a
 *             shortened msgId names nothing and a shortened URL fails its
 *             signature check invisibly
 *
 ****************************************************************************/

int vs_cloud_social_upload(const char *session_id,
                           const struct vs_cloud_media_packet_s *packet,
                           struct vs_cloud_upload_s *out);

/****************************************************************************
 * Name: vs_cloud_social_poll_event
 *
 * Description:
 *   GET /contest/v1/getResult for up to msg_count identifiers at once, which
 *   the interface supports natively -- msgId is an array.
 *
 *   One msgId can produce more than one entry.  An extreme frame yields both
 *   an IMAGE entry with the emotion and an AUDIO entry for the advice derived
 *   from the surrounding audio, under the same msgId, which is why out is an
 *   array sized independently of msg_count.  Entries past out_cap are
 *   dropped and counted in the log rather than reported as an error.
 *
 * Input Parameters:
 *   msg_ids   - array of msg_count NUL-terminated identifiers
 *   out       - receives up to out_cap entries
 *   out_count - number actually written
 *
 * Returned Value:
 *   0 on success, even when out_count is zero.  A negative errno on
 *   transport or protocol failure; -EPROTO specifically means the response
 *   parsed as JSON but did not have the documented shape.
 *
 ****************************************************************************/

int vs_cloud_social_poll_event(const char *session_id,
                               const char *const *msg_ids, size_t msg_count,
                               struct vs_social_event_s *out, size_t out_cap,
                               size_t *out_count);

/****************************************************************************
 * Name: vs_cloud_social_finalize
 *
 * Description:
 *   DELETE /contest/v1/session.  Asynchronous by design: it returns a msgId
 *   and status 40 (closing), and the minutes have to be polled for.  Treating
 *   it as synchronous would be wrong even if a future cloud answered
 *   immediately, whereas polling costs one extra round trip in that case.
 *
 * Returned Value:
 *   0 with msg_id_out filled.  -EPROTO when no msgId came back, otherwise a
 *   negative errno.
 *
 ****************************************************************************/

int vs_cloud_social_finalize(const char *session_id, char *msg_id_out,
                             size_t msg_id_cap);

/****************************************************************************
 * Name: vs_cloud_social_get_result
 *
 * Description:
 *   Poll the identifier from vs_cloud_social_finalize() once.  This is the
 *   same GET as vs_cloud_social_poll_event(); it exists separately because
 *   the caller wants a different thing out of the answer -- the minutes
 *   rather than a stream of events -- and because the CLOSE entry's response
 *   object is the one worth keeping verbatim.
 *
 *   Call it on an interval until it stops returning -EAGAIN.  It does not
 *   loop internally: the finalize page has to stay responsive to a second
 *   long-press, and a call that blocked for the whole timeout could not.
 *
 * Input Parameters:
 *   minutes   - filled only on success; may be NULL
 *   full_json - receives the CLOSE entry's complete response object, for
 *               vs_history_append().  May be NULL.  -E2BIG if too small,
 *               which is reported rather than silently truncated because a
 *               clipped record is worse than none.
 *
 * Returned Value:
 *   0        minutes are in hand.  Requires an entry for this msgId with
 *            msgEvent 2 and peer status 21, which is what the document fixes
 *            as the completion code for a close.  Nothing else is accepted:
 *            the cloud reuses a msgId across events, and an image entry under
 *            the same id would otherwise pass as a session with empty
 *            minutes.
 *   -EAGAIN  still closing, or no close entry in the response yet
 *   -EIO     the cloud reported the close as failed; minutes->summary holds
 *            whatever it said
 *   -E2BIG   full_json too small.  Checked before minutes is touched, so
 *            "filled only on success" holds.
 *
 ****************************************************************************/

int vs_cloud_social_get_result(const char *session_id, const char *msg_id,
                               struct vs_cloud_minutes_s *minutes,
                               char *full_json, size_t full_cap);

/****************************************************************************
 * Name: vs_cloud_social_ack
 *
 * Description:
 *   Local no-op, on purpose.
 *
 *   The device-side interface has four endpoints and none of them is an
 *   acknowledgement.  The orchestration layer still has a point in its flow
 *   where it has finished with a session and would say so, so the call exists
 *   to keep that flow honest and to give the eventual endpoint one obvious
 *   place to land.  It logs and returns 0.
 *
 *   Do not implement this against a guessed path.  The integration plan lists
 *   exactly that as a rollback trigger.
 *
 ****************************************************************************/

int vs_cloud_social_ack(const char *session_id);

/****************************************************************************
 * Name: vs_cloud_download
 *
 * Description:
 *   GET an absolute URL and return its body.  Used for ttsMinutes, whose
 *   payload is Ogg audio rather than JSON, so the body is returned as bytes
 *   with an explicit length instead of a string.
 *
 *   A relative URL -- which is what the document's example ttsMinutes looks
 *   like -- is resolved against the configured cloud host.
 *
 * Input Parameters:
 *   data       - receives a buffer the caller owns
 *   len        - receives its length
 *   from_psram - receives which allocator to release it with; pass the whole
 *                triple back to vs_cloud_release()
 *
 * Returned Value:
 *   0 on success.  -EFBIG when the body is larger than
 *   CONFIG_VS_SOCIAL_DOWNLOAD_MAX_BYTES; a body of exactly that many bytes is
 *   accepted.  -EIO when the peer closed before Content-Length was reached,
 *   which matters here because a partial Ogg file would otherwise be handed
 *   back as complete and played.
 *
 ****************************************************************************/

int vs_cloud_download(const char *url, unsigned char **data, size_t *len,
                      bool *from_psram);

/* Release a buffer from vs_cloud_download(). */

void vs_cloud_release(unsigned char *data, bool from_psram);

/****************************************************************************
 * Name: vs_cloud_server_to_peer
 *
 * Description:
 *   Map a server-side code to the peer-side code the device would have seen.
 *   The device-facing interface is supposed to do this itself, so this is a
 *   tolerance path: if a getResult entry carries a status that is not a valid
 *   peer code but is a valid server one, it is mapped here rather than the
 *   entry being discarded.
 *
 * Returned Value:
 *   true when state was recognised and *out was written.
 *
 ****************************************************************************/

bool vs_cloud_server_to_peer(int state, enum vs_cloud_peer_state_e *out);

/****************************************************************************
 * Name: vs_cloud_probe
 *
 * Description:
 *   Drive one complete session against the configured endpoint and print
 *   what happened: open, register and transfer a small synthetic image and
 *   audio object, poll them, close, and poll for the minutes.
 *
 *   This is the connectivity gate from the integration plan's P1 and P3
 *   stages, reachable from nsh as "velasight cloudprobe" so the client can
 *   be exercised before any of the UI or capture work exists.  It uploads
 *   synthetic bytes, never camera or microphone data.
 *
 *   Every failure path closes the session before returning.  It has to: one
 *   deviceId may hold one live session, so a probe that walked away from an
 *   open one would make the next probe fail with -EBUSY on open, reporting a
 *   different problem from the one that happened.
 *
 * Returned Value:
 *   0        the full round trip succeeded and payload bytes moved
 *   -ENOTSUP the round trip succeeded but no bytes were transferred, because
 *            the cloud answered with mock presigned URLs.  Expected against a
 *            mock server; a failure against a real one.
 *   otherwise the first negative errno that stopped it
 *
 ****************************************************************************/

int vs_cloud_probe(void);

/* Release the connection pool.  Only worth calling at shutdown. */

void vs_cloud_deinit(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_CLOUD_H */
