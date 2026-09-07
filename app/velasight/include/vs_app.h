#ifndef __APP_VELASIGHT_INCLUDE_VS_APP_H
#define __APP_VELASIGHT_INCLUDE_VS_APP_H


#include <stdint.h>

#include "vs_types.h"

enum vs_app_event_e
{
  VS_APP_EVENT_PHOTO_READY = 0,
  VS_APP_EVENT_PHOTO_FAILED,
  VS_APP_EVENT_VOICE_LISTENING_DONE,
  VS_APP_EVENT_VOICE_REPLY,
  VS_APP_EVENT_VOICE_LISTENING_AGAIN,

  /* The capture device is open and recording.  Posted separately from the
   * page switch because the two are seconds apart: a round first loads the
   * referenced record from SD-NAND and completes a TLS handshake and
   * WebSocket upgrade to the ASR service, and only then opens the
   * microphone.  Showing "请说话" before this arrives asked the user to talk
   * into a device that was not listening yet, and their opening words were
   * simply never captured.
   */

  VS_APP_EVENT_VOICE_LISTENING_READY,
  VS_APP_EVENT_VOICE_CONVERSATION_DONE,
  VS_APP_EVENT_VOICE_FAILED,
  VS_APP_EVENT_SOCIAL_STARTED,
  VS_APP_EVENT_SOCIAL_START_FAILED,
  VS_APP_EVENT_SOCIAL_ALERT,
  VS_APP_EVENT_SOCIAL_ALERT_CLEARED,
  VS_APP_EVENT_SOCIAL_PAUSED,
  VS_APP_EVENT_SOCIAL_RESUMED,
  VS_APP_EVENT_SOCIAL_PAUSE_FAILED,

  /* Progress within the finalize sequence.  Not a page change: the finalizing
   * page stays where it is and only its body line and its watchdog move.
   *
   * It exists because that page used to be one static frame over everything
   * between the user's long-press and the summary -- stopping capture, closing
   * the cloud session, waiting for the minutes, writing them to the card and
   * fetching the audio, measured together at over seventy seconds.  A user
   * cannot tell that from a hang, and neither can a log.
   */

  VS_APP_EVENT_SOCIAL_STAGE,
  VS_APP_EVENT_SOCIAL_RESULT,
  VS_APP_EVENT_SOCIAL_FINALIZE_FAILED,
  VS_APP_EVENT_NETWORK_READY,
  VS_APP_EVENT_NETWORK_FAILED
};

/****************************************************************************
 * Where a finalizing session has got to.
 *
 * Ordered as they occur, which is what lets the UI treat a stage that arrives
 * out of order or twice as harmless.  Each one owns its own share of the
 * watchdog budget, because they are not waiting for comparable things: SAVING
 * is a card write measured in single-digit seconds, WAITING is the cloud
 * producing minutes and was measured at anywhere from twelve to over sixty.
 ****************************************************************************/

enum vs_social_stage_e
{
  VS_SOCIAL_STAGE_NONE = 0,
  VS_SOCIAL_STAGE_STOPPING,   /* draining capture, uploading the tail */
  VS_SOCIAL_STAGE_CLOSING,    /* DELETE /session, waiting for its msgId */
  VS_SOCIAL_STAGE_WAITING,    /* polling getResult for the minutes */
  VS_SOCIAL_STAGE_SAVING,     /* vs_history_append to SD-NAND */
  VS_SOCIAL_STAGE_FETCHING,   /* downloading the spoken minutes */
  VS_SOCIAL_STAGE_COUNT
};

struct vs_app_event_s
{
  enum vs_app_event_e type;
  uint32_t request_id;
  enum vs_emotion_e emotion;
  uint32_t color;
  int error;

  /* Meaningful only on VS_APP_EVENT_SOCIAL_STAGE.  A separate field rather
   * than a reuse of error, because a stage is not a failure and the two would
   * have had to be told apart by the event type at every read anyway.
   */

  enum vs_social_stage_e stage;
  char text[VS_TEXT_LONG];
};

int vs_app_run(void);
int vs_app_post_event(const struct vs_app_event_s *event);

/****************************************************************************
 * Name: vs_app_take_event
 *
 * Description:
 *   Take one queued event.  Returns 0 with *out filled, or -EAGAIN when the
 *   queue is empty.
 *
 *   vs_app_run() drains the queue itself and does not need this.  It exists
 *   for the headless subcommands, which drive a worker without a UI: workers
 *   retry vs_app_post_event() indefinitely when the queue is full, so a
 *   command that started one and never drained would wedge it on the eighth
 *   event rather than merely losing the ninth.
 *
 ****************************************************************************/

int vs_app_take_event(struct vs_app_event_s *out);

uint32_t vs_app_current_request_id(void);
int velasight_autostart(void);

#endif
