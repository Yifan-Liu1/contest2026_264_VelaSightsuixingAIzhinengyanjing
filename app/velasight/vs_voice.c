/****************************************************************************
 * app/velasight/vs_voice.c
 *
 * Idle voice assistant: one worker thread per round that turns a Power
 * short press into "listen, recognize, ask the model, speak the answer".
 *
 * Why this does not call voice_channel_start/stop/stop_with_text/speak
 * -----------------------------------------------------------------------
 * packages/ai_agent is a public repository this project must not modify
 * (see board/.../bk7258_agent_config.c's own comment on the same
 * constraint).  voice_channel.c's state machine has two properties that
 * conflict with this product and cannot be worked around from outside it:
 *
 *   1. When its VAD fires -- the normal, most common way a user finishes
 *      speaking -- the recording thread calls voice_channel_stop() (not
 *      stop_with_text()), which pushes the recognized text onto
 *      message_bus's inbound queue instead of returning it.  Nothing in
 *      this product runs agent_loop to drain that queue, so the most common
 *      path would silently lose every answer.
 *
 *   2. voice_channel_speak()'s playback handle (s_voice.tts_pb) is a file
 *      static with no accessor, so nothing outside voice_channel.c can stop
 *      a playback in progress.
 *
 * Both problems disappear by going one layer down.  voice_asr.h,
 * voice_tts.h, voice_vad.h, audio_capture.h and audio_playback.h are
 * self-contained, state-machine-free primitives that voice_channel.c itself
 * is built from; this file drives them directly, so every intermediate
 * result (recognized text, model answer, TTS chunk) stays in this file's
 * own hands, cancellable and un-routed through anything this product does
 * not run.  volc_asr_register()/volc_tts_register() -- the two calls
 * voice_channel_init() makes before anything else -- are called here for
 * the same reason: to activate the Volcengine backends without pulling in
 * the state machine wrapped around them.
 *
 * Why vs_voice_open() re-seeds the LLM credential on every boot
 * -----------------------------------------------------------------------
 * llm_chat()/llm_chat_vision_raw() read a static s_api_key in llm_proxy.c
 * that is only populated by llm_proxy_init() (from config_store's on-disk
 * JSON) or by a later llm_set_all()/llm_router_apply() call.  Nothing in
 * this product runs ai_agent's own agent_loop or NSH commands, so nothing
 * would ever call llm_router_apply() to move a router-slot credential into
 * llm_proxy's live state.  Rather than depend on that chain, this file
 * reads the credential Web provisioning already wrote
 * (velasight_provisioning_load(), the same record board's
 * bk7258_nand_seed_agent_config() mirrors into ai_agent's config file) and
 * calls llm_set_all() directly, once, at startup.  This also sidesteps
 * whether ai_agent's own config.json survived the SD-NAND's FAT
 * configuration: llm_set_all() updates live memory regardless of whether
 * its own file write succeeds.
 *
 * The Volcengine ASR/TTS credentials provisioning also collects (app_id and
 * token, see velasight_prov_credentials_s) take a different path on
 * purpose: volc_asr.c and volc_tts_ws.c re-read the config store on every
 * call (they have no cached, in-memory state to update the way llm_proxy.c
 * does), so vs_voice_seed_volc_credentials() writes config_store directly
 * with claw_config_set() instead of reaching for an llm_set_all()-style
 * shortcut that does not exist for them.  This does depend on
 * config_store's own file surviving the SD-NAND's FAT configuration --
 * unlike the LLM path, there is no fallback if it does not.
 *
 * Threading model
 * -----------------------------------------------------------------------
 * At most one round is ever in flight: vs_voice_start() takes a busy flag
 * and returns -EBUSY otherwise, so there is exactly one worker thread or
 * none.  A mutex-protected struct holds that worker's in-progress resource
 * handles (capture device, playback device) so vs_voice_cancel(),
 * vs_voice_stop_recording() and vs_voice_stop_speaking() -- all called from
 * the main UI thread -- can interrupt them from the outside using the
 * cross-thread-safe primitives those headers already document
 * (audio_capture_abort(), audio_playback_stop()).  ASR/LLM/TTS network
 * calls are synchronous with no such primitive; cancelling during one of
 * those steps only stops this file from acting on the result once the call
 * returns, exactly as documented for llm_chat_vision_raw() elsewhere in
 * this codebase's plan.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netutils/cJSON.h>

#include <agent_compat.h>
#include <infra/config_store.h>
#include <core/message_bus.h>
#include <llm/llm_proxy.h>
#include <llm/llm_router.h>
#include <voice/audio_capture.h>
#include <voice/audio_playback.h>
#include <voice/voice_asr.h>
#include <voice/voice_tts.h>
#include <voice/voice_vad.h>
#include <voice/volc_asr.h>
#include <voice/volc_tts.h>
#include <agent_config.h>

#include "velasight_provisioning.h"

#include "include/vs_app.h"
#include "include/vs_history.h"
#include "include/vs_media.h"
#include "include/vs_voice.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Field budgets for the whitelist copy out of a history record.  Kept well
 * inside CONFIG_VS_VOICE_PROMPT_MAX_BYTES so several of them plus the fixed
 * system prompt never approach the model's own request-size limits. */

#define VS_VOICE_TITLE_MAX   96
#define VS_VOICE_SUMMARY_MAX 192
#define VS_VOICE_BODY_MAX    768

#define VS_VOICE_QUESTION_MAX 512

/* The model is asked to keep "answer" under ~150 Chinese characters (see
 * the system prompt below), which is well under 1 KiB of UTF-8.  This cap
 * is deliberately independent of CONFIG_VS_VOICE_RESP_MAX_BYTES (the raw
 * model response buffer, which also carries JSON syntax and other fields):
 * sizing the spoken-answer copy off the raw buffer's own, much larger cap
 * would put a 32 KiB array on a worker thread stack for no benefit. */

#define VS_VOICE_ANSWER_MAX 4096

/* Volcengine credential path: this product has no provisioning entry for
 * it yet (only the MiMo key travels through Web provisioning), so a round
 * fails fast and visibly with -ENOKEY rather than silently hanging in a
 * recording state nothing will ever finish. */

enum vs_voice_stage_e
{
  VS_VOICE_STAGE_IDLE = 0,
  VS_VOICE_STAGE_PHOTO,
  VS_VOICE_STAGE_RECORDING,
  VS_VOICE_STAGE_THINKING,
  VS_VOICE_STAGE_SPEAKING
};

struct vs_voice_ctx_s
{
  char record_key[VS_HISTORY_KEY_MAX];
  char title[VS_VOICE_TITLE_MAX];
  char summary[VS_VOICE_SUMMARY_MAX];
  char date[VS_TEXT_SHORT];
  char body[VS_VOICE_BODY_MAX];
  bool content_truncated;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  pthread_mutex_t     lock;
  bool                opened;
  bool                busy;
  bool                cancelled;
  enum vs_voice_stage_e stage;
  uint32_t            request_id;
  audio_capture_t     *cap;
  audio_playback_t    *pb;
  pthread_t            worker;
} g_voice =
{
  .lock = PTHREAD_MUTEX_INITIALIZER
};

/****************************************************************************
 * Private Functions -- setup
 ****************************************************************************/

/* Mirrors board/.../bk7258_agent_config.c's host selection so the two
 * places that derive a MiMo host from the key's prefix cannot drift apart
 * silently; that file seeds ai_agent's own (8.3-fragile) config file, this
 * one seeds llm_proxy's live memory state directly. */

static void vs_voice_seed_llm_credentials(
    const struct velasight_prov_credentials_s *credentials)
{
  const char *host;

  if (credentials->api_key[0] == '\0')
    {
      printf("vs_voice: no MiMo API key provisioned yet\n");
      return;
    }

  host = strncmp(credentials->api_key, "tp-", 3) == 0 ?
         "token-plan-cn.xiaomimimo.com" : "api.xiaomimimo.com";

  llm_set_all(host, "/v1/chat/completions", "443", credentials->api_key,
              "mimo-v2.5");
  printf("vs_voice: LLM credential seeded from provisioning (host=%s)\n",
         host);
}

/* Unlike llm_proxy's s_api_key, volc_asr.c and volc_tts_ws.c re-read their
 * credentials from the config store on every call (volc_asr_init()/
 * tts_ws_init()), so there is no live-memory shortcut to take here the way
 * vs_voice_seed_llm_credentials() takes for the LLM: writing config_store
 * once at startup is both necessary and sufficient, and it is what those
 * two backends were already built to expect. */

static void vs_voice_seed_volc_credentials(
    const struct velasight_prov_credentials_s *credentials)
{
  if (credentials->volc_appid[0] == '\0' ||
      credentials->volc_token[0] == '\0')
    {
      printf("vs_voice: no Volcengine app_id/token provisioned yet\n");
      return;
    }

  claw_config_set(AGENT_CFG_KEY_VOLC_APPKEY, credentials->volc_appid);
  claw_config_set(AGENT_CFG_KEY_VOLC_TOKEN, credentials->volc_token);
  printf("vs_voice: Volcengine credentials seeded from provisioning "
         "(app_id %zu bytes)\n", strlen(credentials->volc_appid));
}

void vs_voice_open(void)
{
  int ret;

  pthread_mutex_lock(&g_voice.lock);
  if (g_voice.opened)
    {
      pthread_mutex_unlock(&g_voice.lock);
      return;
    }

  g_voice.opened = true;
  pthread_mutex_unlock(&g_voice.lock);

  ret = config_store_init();
  printf("vs_voice: config_store_init -> %d\n", ret);

  ret = message_bus_init();
  printf("vs_voice: message_bus_init -> %d\n", ret);

  ret = llm_proxy_init();
  printf("vs_voice: llm_proxy_init -> %d\n", ret);

  ret = llm_router_init();
  printf("vs_voice: llm_router_init -> %d\n", ret);

  ret = volc_asr_register();
  printf("vs_voice: volc_asr_register -> %d\n", ret);

  ret = volc_tts_register();
  printf("vs_voice: volc_tts_register -> %d\n", ret);

  {
    struct velasight_prov_credentials_s credentials;

    if (velasight_provisioning_load(&credentials) < 0)
      {
        printf("vs_voice: no provisioning record yet\n");
      }
    else
      {
        vs_voice_seed_llm_credentials(&credentials);
        vs_voice_seed_volc_credentials(&credentials);
      }
  }
}

/****************************************************************************
 * Private Functions -- small helpers
 ****************************************************************************/

static uint32_t vs_voice_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bool vs_voice_is_cancelled(void)
{
  bool cancelled;

  pthread_mutex_lock(&g_voice.lock);
  cancelled = g_voice.cancelled;
  pthread_mutex_unlock(&g_voice.lock);
  return cancelled;
}

static void vs_voice_set_stage(enum vs_voice_stage_e stage)
{
  pthread_mutex_lock(&g_voice.lock);
  g_voice.stage = stage;
  pthread_mutex_unlock(&g_voice.lock);
}

/* Post an event, retrying while the queue is full, but abandoning the
 * retry the moment the round is cancelled -- otherwise a cancelled worker
 * could sit spinning on a full queue instead of unwinding. */

static void vs_voice_post(enum vs_app_event_e type, uint32_t request_id,
                          int error, const char *text)
{
  struct vs_app_event_s event;

  memset(&event, 0, sizeof(event));
  event.type       = type;
  event.request_id = request_id;
  event.error      = error;
  if (text != NULL)
    {
      snprintf(event.text, sizeof(event.text), "%s", text);
    }

  while (vs_app_post_event(&event) == -EAGAIN)
    {
      if (vs_voice_is_cancelled())
        {
          return;
        }

      usleep(10000);
    }
}

/* Largest prefix of a NUL-terminated UTF-8 string that fits in max_bytes
 * (not counting the terminator) without splitting a multi-byte sequence.
 * Continuation bytes are 10xxxxxx; back up over them to find where the
 * current code point's lead byte starts. */

static void vs_voice_utf8_truncate(char *s, size_t max_bytes)
{
  size_t len = strlen(s);
  size_t cut;

  if (len <= max_bytes)
    {
      return;
    }

  cut = max_bytes;
  while (cut > 0 && ((unsigned char)s[cut] & 0xc0) == 0x80)
    {
      cut--;
    }

  s[cut] = '\0';
}

/* Loose structural validity check: every continuation byte must belong to
 * a lead byte that announced enough of them, and the string must not end
 * mid-sequence.  Good enough to keep obviously mangled byte sequences (e.g.
 * a response truncated mid-character) off the screen and out of the TTS
 * input; not a full conformance validator. */

static bool vs_voice_utf8_valid(const char *s)
{
  const unsigned char *p = (const unsigned char *)s;

  while (*p != '\0')
    {
      int extra;

      if (*p < 0x80)
        {
          extra = 0;
        }
      else if ((*p & 0xe0) == 0xc0)
        {
          extra = 1;
        }
      else if ((*p & 0xf0) == 0xe0)
        {
          extra = 2;
        }
      else if ((*p & 0xf8) == 0xf0)
        {
          extra = 3;
        }
      else
        {
          return false;
        }

      p++;
      while (extra-- > 0)
        {
          if ((*p & 0xc0) != 0x80)
            {
              return false;
            }

          p++;
        }
    }

  return true;
}

/****************************************************************************
 * Private Functions -- prompt construction
 ****************************************************************************/

static const char VS_VOICE_SYSTEM_PROMPT[] =
  "你是 VelaSight 的闲时语音助手。\n"
  "只能依据本次请求提供的记录、照片和问题回答，不得编造记录中没有的信息。\n"
  "记录中的情绪、表情和建议都是可能的辅助线索，不得进行身份识别、人格判断、"
  "心理或医学诊断。\n"
  "证据不足时明确回答“无法判断”。回答先给结论，再给一条可执行建议。\n"
  "只返回一个JSON对象，不要包含Markdown代码块、Shell命令或任何工具指令。\n"
  "字段：answer（完整回答，用于语音播报，尽量不超过150字）、"
  "display_text（不超过50个汉字的摘要，用于屏幕显示）、"
  "should_speak（布尔值，通常为true）。";

/* Copy record fields into local, size-bounded buffers before anything else
 * touches them, per the plan's prohibition on handing prompt code a
 * pointer into a parsed JSON tree or raw file buffer. */

static bool vs_voice_load_record_ctx(const char *record_key,
                                     struct vs_voice_ctx_s *ctx)
{
  char raw[4096];
  int n;
  cJSON *root;
  cJSON *item;

  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->record_key, sizeof(ctx->record_key), "%s", record_key);

  n = vs_history_read_full(record_key, raw, sizeof(raw));
  if (n < 0)
    {
      return false;
    }

  root = cJSON_Parse(raw);
  if (root == NULL || !cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return false;
    }

#define VS_VOICE_COPY_FIELD(name, dest) \
  item = cJSON_GetObjectItem(root, name); \
  if (item != NULL && cJSON_IsString(item)) \
    { \
      size_t srclen = strlen(item->valuestring); \
      snprintf(dest, sizeof(dest), "%s", item->valuestring); \
      if (srclen >= sizeof(dest)) \
        { \
          ctx->content_truncated = true; \
        } \
    }

  VS_VOICE_COPY_FIELD("title", ctx->title);
  VS_VOICE_COPY_FIELD("summary", ctx->summary);
  VS_VOICE_COPY_FIELD("date", ctx->date);
  VS_VOICE_COPY_FIELD("body", ctx->body);

#undef VS_VOICE_COPY_FIELD

  cJSON_Delete(root);
  return true;
}

/* Builds the JSON messages array llm_chat() expects: one user message
 * whose content is the whitelisted record context followed by the
 * question.  Returns the number of bytes written, or a negative errno. */

static int vs_voice_build_record_messages(const struct vs_voice_ctx_s *ctx,
                                          const char *question,
                                          char *out, size_t out_cap)
{
  cJSON *array;
  cJSON *msg;
  char *content;
  char *text;
  int content_len;
  int ret = -ENOMEM;

  content = malloc(CONFIG_VS_VOICE_PROMPT_MAX_BYTES);
  if (content == NULL)
    {
      return -ENOMEM;
    }

  content_len = snprintf(content, CONFIG_VS_VOICE_PROMPT_MAX_BYTES,
                         "[CONTEXT]\n"
                         "record_id: %s\n"
                         "date: %s\n"
                         "title: %s\n"
                         "summary: %s\n"
                         "body: %s\n"
                         "content_truncated: %s\n"
                         "[USER QUESTION]\n%s",
                         ctx->record_key, ctx->date, ctx->title, ctx->summary,
                         ctx->body, ctx->content_truncated ? "true" : "false",
                         question);

  if (content_len < 0 ||
      content_len >= CONFIG_VS_VOICE_PROMPT_MAX_BYTES)
    {
      free(content);
      return -EMSGSIZE;
    }

  array = cJSON_CreateArray();
  msg   = cJSON_CreateObject();
  if (array == NULL || msg == NULL)
    {
      cJSON_Delete(array);
      cJSON_Delete(msg);
      free(content);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(msg, "role", "user");
  cJSON_AddStringToObject(msg, "content", content);
  cJSON_AddItemToArray(array, msg);
  free(content);

  text = cJSON_PrintUnformatted(array);
  cJSON_Delete(array);
  if (text == NULL)
    {
      return -ENOMEM;
    }

  ret = snprintf(out, out_cap, "%s", text);
  free(text);

  if (ret < 0 || (size_t)ret >= out_cap)
    {
      return -EMSGSIZE;
    }

  return ret;
}

static int vs_voice_build_photo_prompt(const char *question,
                                       char *out, size_t out_cap)
{
  int ret = snprintf(out, out_cap, "%s\n\n[USER QUESTION]\n%s",
                     VS_VOICE_SYSTEM_PROMPT, question);

  if (ret < 0 || (size_t)ret >= out_cap)
    {
      return -EMSGSIZE;
    }

  return ret;
}

/* Bounded parse of the model's JSON reply.  Returns 0 and fills
 * display_text/should_speak/answer_out on success, or a negative errno:
 *   -EBADMSG  not parseable JSON, or answer/display_text missing or empty
 *   -EILSEQ   display_text is not well-formed UTF-8
 */

static int vs_voice_parse_reply(const char *raw, char *display_text,
                                size_t display_cap, char *answer_out,
                                size_t answer_cap, bool *should_speak)
{
  cJSON *root;
  cJSON *item;
  const char *display_src;
  const char *answer_src;

  root = cJSON_Parse(raw);
  if (root == NULL || !cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }

  item = cJSON_GetObjectItem(root, "answer");
  answer_src = (item != NULL && cJSON_IsString(item)) ?
               item->valuestring : NULL;

  item = cJSON_GetObjectItem(root, "display_text");
  display_src = (item != NULL && cJSON_IsString(item)) ?
                item->valuestring : answer_src;

  if (answer_src == NULL || answer_src[0] == '\0' ||
      display_src == NULL || display_src[0] == '\0')
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }

  snprintf(answer_out, answer_cap, "%s", answer_src);
  snprintf(display_text, display_cap, "%s", display_src);

  item = cJSON_GetObjectItem(root, "should_speak");
  *should_speak = item == NULL || !cJSON_IsBool(item) || cJSON_IsTrue(item);

  cJSON_Delete(root);

  if (!vs_voice_utf8_valid(answer_out) || !vs_voice_utf8_valid(display_text))
    {
      return -EILSEQ;
    }

  return 0;
}

/****************************************************************************
 * Private Functions -- recording + streaming ASR
 ****************************************************************************/

/* Records until the shared VAD says to stop, vs_voice_stop_recording() or
 * vs_voice_cancel() aborts the capture device from another thread, or
 * CONFIG_VS_VOICE_RECORD_MAX_MS elapses.  On return the capture device is
 * already closed and the ASR stream, if recognition should proceed, is
 * hand-finished by the caller (the two cancellation reasons need different
 * treatment: a manual/VAD stop still wants the recognized text, a genuine
 * cancel does not). */

static int vs_voice_record_and_recognize(char *question, size_t question_cap)
{
  audio_capture_t *cap;
  voice_asr_stream_t *stream;
  voice_vad_t vad;
  unsigned char chunk[AGENT_ASR_CHUNK_SIZE];
  uint32_t start_ms;
  int ret;

  question[0] = '\0';

  /* Pre-open the ASR session before the microphone so the TLS handshake's
   * few hundred milliseconds happen before any speech could be lost -- the
   * same ordering voice_channel_start() itself uses. */

  stream = voice_asr_stream_open();
  if (stream == NULL)
    {
      return -ENOKEY;
    }

  cap = audio_capture_open(AGENT_AUDIO_CAPTURE_DEV, AGENT_VOICE_SAMPLE_RATE,
                           AGENT_VOICE_CHANNELS, AGENT_VOICE_BITS);
  if (cap == NULL)
    {
      voice_asr_stream_abort(stream);
      return -EIO;
    }

  if (audio_capture_start(cap) < 0)
    {
      audio_capture_close(cap);
      voice_asr_stream_abort(stream);
      return -EIO;
    }

  pthread_mutex_lock(&g_voice.lock);
  g_voice.cap = cap;
  pthread_mutex_unlock(&g_voice.lock);

  voice_vad_init(&vad, AGENT_VOICE_SAMPLE_RATE);
  start_ms = vs_voice_now_ms();

  for (;;)
    {
      int n;

      if (vs_voice_now_ms() - start_ms >= CONFIG_VS_VOICE_RECORD_MAX_MS)
        {
          break;
        }

      n = audio_capture_read(cap, chunk, sizeof(chunk));
      if (n == -EAGAIN || n == -EWOULDBLOCK)
        {
          usleep(10000);
          continue;
        }

      if (n <= 0)
        {
          /* Either a genuine device error, or audio_capture_abort() was
           * called from vs_voice_cancel()/vs_voice_stop_recording().  Both
           * end the loop the same way; which one it was is read back from
           * the cancelled flag by the caller. */

          break;
        }

      voice_asr_stream_send(stream, chunk, (size_t)n);

      if (voice_vad_process(&vad, (const int16_t *)chunk,
                            (size_t)n / sizeof(int16_t)))
        {
          break;
        }
    }

  pthread_mutex_lock(&g_voice.lock);
  g_voice.cap = NULL;
  pthread_mutex_unlock(&g_voice.lock);
  audio_capture_close(cap);

  if (vs_voice_is_cancelled())
    {
      voice_asr_stream_abort(stream);
      return -ECANCELED;
    }

  ret = voice_asr_stream_finish(stream, question, question_cap);
  if (ret != 0)
    {
      return -EIO;
    }

  if (question[0] == '\0')
    {
      return -ENODATA;
    }

  if (!vs_voice_utf8_valid(question))
    {
      question[0] = '\0';
      return -EILSEQ;
    }

  return 0;
}

/****************************************************************************
 * Private Functions -- streaming TTS playback
 ****************************************************************************/

struct vs_voice_tts_ctx_s
{
  audio_playback_t *pb;
};

static void vs_voice_tts_chunk(const unsigned char *pcm_data, size_t pcm_len,
                               int is_last, void *user_data)
{
  struct vs_voice_tts_ctx_s *tts = user_data;

  (void)is_last;
  if (pcm_data != NULL && pcm_len > 0 && tts->pb != NULL)
    {
      /* A return of -ECANCELED here means vs_voice_stop_speaking() already
       * stopped this handle; nothing more to do with this or later chunks,
       * but the underlying stream call is left to finish or fail on its
       * own since it has no exposed abort. */

      (void)audio_playback_write(tts->pb, pcm_data, pcm_len);
    }
}

static void vs_voice_speak(const char *answer)
{
  struct vs_voice_tts_ctx_s tts;

  if (vs_voice_is_cancelled())
    {
      return;
    }

  tts.pb = audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV,
                               AGENT_TTS_WS_SAMPLE_RATE, AGENT_VOICE_CHANNELS,
                               AGENT_VOICE_BITS);
  if (tts.pb == NULL)
    {
      printf("vs_voice: playback device unavailable\n");
      return;
    }

  pthread_mutex_lock(&g_voice.lock);
  g_voice.pb = tts.pb;
  pthread_mutex_unlock(&g_voice.lock);

  vs_voice_set_stage(VS_VOICE_STAGE_SPEAKING);
  (void)volc_tts_ws_synthesize_stream(answer, vs_voice_tts_chunk, &tts);

  pthread_mutex_lock(&g_voice.lock);
  g_voice.pb = NULL;
  pthread_mutex_unlock(&g_voice.lock);
  audio_playback_close(tts.pb);
}

/****************************************************************************
 * Private Functions -- worker
 ****************************************************************************/

static void *vs_voice_worker(void *arg)
{
  struct vs_voice_request_s request = *(struct vs_voice_request_s *)arg;
  struct vs_media_frame_s frame;
  struct vs_voice_ctx_s record_ctx;
  char question[VS_VOICE_QUESTION_MAX];
  char *prompt = NULL;
  char *model_resp = NULL;
  char *answer = NULL;
  char display_text[VS_TEXT_LONG];
  bool should_speak;
  bool have_frame = false;
  int ret;

  free(arg);
  memset(&frame, 0, sizeof(frame));

  prompt     = malloc(CONFIG_VS_VOICE_PROMPT_MAX_BYTES);
  model_resp = malloc(CONFIG_VS_VOICE_RESP_MAX_BYTES);
  answer     = malloc(VS_VOICE_ANSWER_MAX);
  if (prompt == NULL || model_resp == NULL || answer == NULL)
    {
      vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id, -ENOMEM,
                   NULL);
      goto cleanup;
    }

  /* ── Step 1 (photo path only): one still frame ───────────────────── */

  if (request.ctx == VS_VOICE_CTX_PHOTO)
    {
      vs_voice_set_stage(VS_VOICE_STAGE_PHOTO);
      ret = vs_media_capture_jpeg(&frame, CONFIG_VS_PHOTO_WIDTH,
                                  CONFIG_VS_PHOTO_HEIGHT);
      if (vs_voice_is_cancelled())
        {
          goto cleanup;
        }

      if (ret < 0)
        {
          vs_voice_post(VS_APP_EVENT_PHOTO_FAILED, request.request_id, ret,
                       NULL);
          goto cleanup;
        }

      have_frame = true;
      vs_voice_post(VS_APP_EVENT_PHOTO_READY, request.request_id, 0, "ok");
    }

  /* ── Step 2: record + recognize ───────────────────────────────────── */

  vs_voice_set_stage(VS_VOICE_STAGE_RECORDING);
  ret = vs_voice_record_and_recognize(question, sizeof(question));
  if (vs_voice_is_cancelled() || ret == -ECANCELED)
    {
      goto cleanup;
    }

  if (ret < 0)
    {
      vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id, ret,
                   NULL);
      goto cleanup;
    }

  vs_voice_post(VS_APP_EVENT_VOICE_LISTENING_DONE, request.request_id, 0,
               NULL);
  vs_voice_set_stage(VS_VOICE_STAGE_THINKING);

  /* ── Step 3: build the prompt and call the model ──────────────────── */

  if (request.ctx == VS_VOICE_CTX_RECORD)
    {
      if (!vs_voice_load_record_ctx(request.record_key, &record_ctx))
        {
          vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id,
                       -ENOENT, NULL);
          goto cleanup;
        }

      ret = vs_voice_build_record_messages(&record_ctx, question, prompt,
                                           CONFIG_VS_VOICE_PROMPT_MAX_BYTES);
      if (ret < 0)
        {
          vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id, ret,
                       NULL);
          goto cleanup;
        }

      ret = llm_chat(VS_VOICE_SYSTEM_PROMPT, prompt, model_resp,
                     CONFIG_VS_VOICE_RESP_MAX_BYTES);
    }
  else
    {
      ret = vs_voice_build_photo_prompt(question, prompt,
                                        CONFIG_VS_VOICE_PROMPT_MAX_BYTES);
      if (ret < 0)
        {
          vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id, ret,
                       NULL);
          goto cleanup;
        }

      ret = have_frame ?
            llm_chat_vision_raw(prompt, frame.data, frame.len, "image/jpeg",
                                model_resp, CONFIG_VS_VOICE_RESP_MAX_BYTES) :
            -ENODATA;
    }

  /* The photo belongs to this round only; release it the moment the model
   * call has returned, win or lose, never after. */

  if (have_frame)
    {
      vs_media_frame_release(&frame);
      have_frame = false;
    }

  if (vs_voice_is_cancelled())
    {
      goto cleanup;
    }

  if (ret != OK)
    {
      printf("vs_voice: model call failed: %.100s\n", model_resp);
      vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id, -EIO,
                   NULL);
      goto cleanup;
    }

  /* ── Step 4: parse, truncate, deliver ─────────────────────────────── */

  ret = vs_voice_parse_reply(model_resp, display_text, sizeof(display_text),
                             answer, VS_VOICE_ANSWER_MAX, &should_speak);
  if (ret < 0)
    {
      vs_voice_post(VS_APP_EVENT_VOICE_FAILED, request.request_id, ret,
                   NULL);
      goto cleanup;
    }

  vs_voice_utf8_truncate(display_text, VS_TEXT_LONG - 1);
  vs_voice_post(VS_APP_EVENT_VOICE_REPLY, request.request_id, 0,
               display_text);

  if (vs_voice_is_cancelled())
    {
      goto cleanup;
    }

  /* ── Step 5: speak ─────────────────────────────────────────────────── */

  if (should_speak)
    {
      vs_voice_speak(answer);
    }

cleanup:
  if (have_frame)
    {
      vs_media_frame_release(&frame);
    }

  free(prompt);
  free(model_resp);
  free(answer);

  pthread_mutex_lock(&g_voice.lock);
  g_voice.busy  = false;
  g_voice.stage = VS_VOICE_STAGE_IDLE;
  pthread_mutex_unlock(&g_voice.lock);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_voice_start(const struct vs_voice_request_s *request)
{
  struct vs_voice_request_s *copy;
  pthread_attr_t attr;
  int ret;

  if (request == NULL || request->request_id == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_voice.lock);
  if (g_voice.busy)
    {
      pthread_mutex_unlock(&g_voice.lock);
      return -EBUSY;
    }

  g_voice.busy      = true;
  g_voice.cancelled = false;
  g_voice.request_id = request->request_id;
  g_voice.stage     = VS_VOICE_STAGE_IDLE;
  pthread_mutex_unlock(&g_voice.lock);

  copy = malloc(sizeof(*copy));
  if (copy == NULL)
    {
      pthread_mutex_lock(&g_voice.lock);
      g_voice.busy = false;
      pthread_mutex_unlock(&g_voice.lock);
      return -ENOMEM;
    }

  *copy = *request;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_VS_VOICE_STACKSIZE);
  ret = pthread_create(&g_voice.worker, &attr, vs_voice_worker, copy);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      free(copy);
      pthread_mutex_lock(&g_voice.lock);
      g_voice.busy = false;
      pthread_mutex_unlock(&g_voice.lock);
      return -ret;
    }

  pthread_detach(g_voice.worker);
  return 0;
}

void vs_voice_cancel(void)
{
  pthread_mutex_lock(&g_voice.lock);
  g_voice.cancelled = true;
  if (g_voice.cap != NULL)
    {
      audio_capture_abort(g_voice.cap);
    }

  if (g_voice.pb != NULL)
    {
      audio_playback_stop(g_voice.pb);
    }

  pthread_mutex_unlock(&g_voice.lock);
}

int vs_voice_stop_recording(void)
{
  pthread_mutex_lock(&g_voice.lock);
  if (g_voice.stage != VS_VOICE_STAGE_RECORDING)
    {
      pthread_mutex_unlock(&g_voice.lock);
      return -EINVAL;
    }

  if (g_voice.cap != NULL)
    {
      audio_capture_abort(g_voice.cap);
    }

  pthread_mutex_unlock(&g_voice.lock);
  return 0;
}

int vs_voice_stop_speaking(void)
{
  pthread_mutex_lock(&g_voice.lock);
  if (g_voice.stage != VS_VOICE_STAGE_SPEAKING)
    {
      pthread_mutex_unlock(&g_voice.lock);
      return -EINVAL;
    }

  if (g_voice.pb != NULL)
    {
      audio_playback_stop(g_voice.pb);
    }

  pthread_mutex_unlock(&g_voice.lock);
  return 0;
}

void vs_voice_close(void)
{
  vs_voice_cancel();
}
