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
  VS_APP_EVENT_VOICE_FAILED,
  VS_APP_EVENT_SOCIAL_STARTED,
  VS_APP_EVENT_SOCIAL_START_FAILED,
  VS_APP_EVENT_SOCIAL_ALERT,
  VS_APP_EVENT_SOCIAL_ALERT_CLEARED,
  VS_APP_EVENT_SOCIAL_PAUSED,
  VS_APP_EVENT_SOCIAL_RESUMED,
  VS_APP_EVENT_SOCIAL_PAUSE_FAILED,
  VS_APP_EVENT_SOCIAL_RESULT,
  VS_APP_EVENT_SOCIAL_FINALIZE_FAILED,
  VS_APP_EVENT_NETWORK_READY,
  VS_APP_EVENT_NETWORK_FAILED
};

struct vs_app_event_s
{
  enum vs_app_event_e type;
  uint32_t request_id;
  enum vs_emotion_e emotion;
  uint32_t color;
  int error;
  char text[VS_TEXT_LONG];
};

int vs_app_run(void);
int vs_app_post_event(const struct vs_app_event_s *event);
uint32_t vs_app_current_request_id(void);
int velasight_autostart(void);

#endif
