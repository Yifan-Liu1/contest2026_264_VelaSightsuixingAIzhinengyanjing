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
 *   4-character ".json" extension) -- re-seeding on every boot is what
 *   makes that irrelevant.
 *
 *   Call once from vs_app_run(), before the main loop starts.  Safe to
 *   call even when no credential has been provisioned yet: the assistant
 *   simply answers every request with -ENOKEY until one is.
 *
 ****************************************************************************/

void vs_voice_open(void);

/* Start one voice round.  Non-blocking: spins up a worker thread and
 * returns immediately.  Only one round may be in flight; a second call
 * before the first has finished (successfully, with failure, or via
 * vs_voice_cancel()) returns -EBUSY. */

int  vs_voice_start(const struct vs_voice_request_s *request);

/* Ask the in-flight round, if any, to stop producing events.  Bounded time:
 * the worker checks a cancellation flag between blocking steps and around
 * network retries, so it unwinds within roughly one network I/O's worth of
 * time, not instantly, but never past the request that was already
 * in-flight when this was called.  Does not block the caller. */

void vs_voice_cancel(void);

/* Cut recording short, as if the pre-roll/VAD tail had just fired.  Valid
 * only while the round is in its recording step; returns -EINVAL
 * otherwise. */

int  vs_voice_stop_recording(void);

/* Cut a TTS playback short and close the playback device.  Valid only
 * while the round is speaking; returns -EINVAL otherwise. Idempotent. */

int  vs_voice_stop_speaking(void);

/* Release resources.  Call at most once, from vs_app_run()'s shutdown
 * path. */

void vs_voice_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_VOICE_H */
