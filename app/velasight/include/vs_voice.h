#ifndef __APP_VELASIGHT_INCLUDE_VS_VOICE_H
#define __APP_VELASIGHT_INCLUDE_VS_VOICE_H

#include <stdbool.h>
#include <stdint.h>

#include "vs_history.h"

enum vs_voice_ctx_e
{
  VS_VOICE_CTX_RECORD = 0,   /* History-record question: text-only model */
  VS_VOICE_CTX_PHOTO         /* Blank-page question: one photo + vision model */
};

struct vs_voice_request_s
{
  enum vs_voice_ctx_e ctx;
  uint32_t            request_id;                    /* from vs_begin_request() */
  char                 record_key[VS_HISTORY_KEY_MAX]; /* CTX_RECORD only */
};

/****************************************************************************
 * Name: vs_voice_open
 *
 * Description:
 *   Bring up the subset of packages/ai_agent this module depends on
 *   (config store, message bus, LLM proxy/router, the Volcengine ASR/TTS
 *   backends) without starting ai_agent's NSH shell, network/BLE channels
 *   or agent_loop -- none of those are reachable from a normal boot and
 *   this product does not need them.  Also seeds the LLM credential that
 *   Web provisioning wrote to SD-NAND (see board's
 *   bk7258_nand_seed_agent_config()) directly into llm_proxy's in-memory
 *   state via llm_set_all(), because that credential's on-disk mirror
 *   inside packages/ai_agent's own config store is not guaranteed to
 *   survive a reset on every SD-NAND image (short-name volumes reject its
 *   4-character ".json" extension) -- re-seeding on boot and after each
 *   successful Web save is what makes that irrelevant.
 *
 *   Call once from vs_app_run(), before the main loop starts.  Safe to
 *   call even when no credential has been provisioned yet: the assistant
 *   simply answers every request with -ENOKEY until one is.
 *
 ****************************************************************************/

void vs_voice_open(void);

/* True once vs_voice_open() has finished bringing the voice subsystem up.
 * It now runs on a background task so the UI need not wait for it at boot;
 * callers check this before starting a request and show a "preparing" hint
 * until it returns true.
 */

bool vs_voice_ready(void);

/* Re-read the durable provisioning record and refresh the live MiMo and
 * Volcengine providers.  When a conversation is active the request is
 * coalesced and applied after its worker finishes, before another worker may
 * start.  Returns 0 when applied or queued, otherwise a negative errno. */

int  vs_voice_reload_credentials(void);

/* Start one multi-turn voice conversation.  Non-blocking: spins up one
 * worker and returns immediately.  After each answer/TTS the worker records
 * another utterance until follow-up silence, an explicit end request, the
 * turn limit, or an error.  Only one conversation may be in flight. */

int  vs_voice_start(const struct vs_voice_request_s *request);

/* End the in-flight conversation while preserving all completed turns.
 * Non-blocking: capture/playback are interrupted immediately where possible;
 * synchronous ASR/LLM/TTS calls may first have to return.  The worker then
 * persists CHAT history and emits exactly one VOICE_CONVERSATION_DONE or
 * VOICE_FAILED terminal event. */

int  vs_voice_end_conversation(void);

/* Compatibility alias for vs_voice_end_conversation().  It no longer drops
 * terminal events or completed conversation history. */

void vs_voice_cancel(void);

/* Cut only the current utterance short and finish ASR normally.  The
 * conversation continues when recognized text is available.  Valid only
 * while recording; returns -EINVAL otherwise. */

int  vs_voice_stop_recording(void);

/* Stop the current playback handle.  Valid only while the conversation is
 * speaking; returns -EINVAL otherwise. */

int  vs_voice_stop_speaking(void);

/* Request shutdown and wait until the detached worker has either delivered
 * its terminal event or suppressed it because shutdown is in progress.
 * A synchronous network call may delay return. */

void vs_voice_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_VOICE_H */
