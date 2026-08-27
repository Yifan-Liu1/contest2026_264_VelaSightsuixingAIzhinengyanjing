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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the Volcengine TTS backend with the voice_tts framework. */
int volc_tts_register(void);

/* Legacy direct-call API (kept for backward compatibility). */
int volc_tts_synthesize_compat(const char *text,
                               const char *appid,
                               const char *token,
                               const char *speaker,
                               unsigned char *pcm_out,
                               size_t pcm_cap,
                               size_t *pcm_len);

/* Streaming TTS: deliver decoded PCM chunks via callback. */
typedef void (*volc_tts_chunk_cb)(const unsigned char *pcm_data,
                                  size_t pcm_len,
                                  int is_last,
                                  void *user_data);

int volc_tts_synthesize_stream(const char *text,
                               volc_tts_chunk_cb cb,
                               void *user_data);

/* WebSocket streaming TTS (bidirectional, no buffer size limit). */
int volc_tts_ws_synthesize_stream(const char *text,
                                  volc_tts_chunk_cb cb,
                                  void *user_data);

/* Drop the cached Volcengine credentials so the next synthesis re-reads them.
 *
 * They are cached because reading them costs a JSON parse off the config store
 * and that was measured at 1.4 s per sentence.  Call this after writing new
 * credentials, or the next reply will still use the old ones.
 */
void volc_tts_ws_invalidate(void);

/* Polled between frames; returning true abandons synthesis. */
typedef bool (*volc_tts_cancel_cb)(void *user_data);

/* As above, but able to give up part-way through.
 *
 * Synthesis of one sentence occupies the calling thread for as long as the
 * server takes to produce it, which is seconds.  Without a way out, a user who
 * leaves the conversation waits for a reply they have already dismissed --
 * observed on hardware as the UI sitting on its "saving" page for half a
 * minute.  cancel is polled once per receive, so the worst case is one socket
 * timeout rather than the whole utterance.
 *
 * Audio already delivered stays delivered: cancelling is not an error, and
 * whatever reached the speaker is reported as success.
 */
int volc_tts_ws_synthesize_stream_cancellable(const char *text,
                                              volc_tts_chunk_cb cb,
                                              void *user_data,
                                              volc_tts_cancel_cb cancel,
                                              void *cancel_data);

#ifdef __cplusplus
}
#endif
