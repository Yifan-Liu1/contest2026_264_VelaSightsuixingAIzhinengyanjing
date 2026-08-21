#ifndef __APP_VELASIGHT_INCLUDE_VS_TYPES_H
#define __APP_VELASIGHT_INCLUDE_VS_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define VS_TEXT_SHORT 40
#define VS_TEXT_LONG  128

enum vs_key_e
{
  VS_KEY_CONFIRM = 0,
  VS_KEY_BACK,
  VS_KEY_NEXT,
  VS_KEY_COUNT
};

enum vs_input_event_e
{
  VS_INPUT_NONE = 0,
  VS_INPUT_PRESS,
  VS_INPUT_SHORT,
  VS_INPUT_LONG,
  VS_INPUT_PROGRESS,
  VS_INPUT_CANCEL,
  VS_INPUT_COMBO_PROGRESS,
  VS_INPUT_COMBO_CANCEL,
  VS_INPUT_NET_TOGGLE
};

struct vs_input_event_s
{
  enum vs_input_event_e type;
  enum vs_key_e key;
  uint8_t progress;
  uint32_t held_ms;
};

enum vs_page_e
{
  VS_PAGE_PREPARING = 0,
  VS_PAGE_HISTORY,
  VS_PAGE_HISTORY_BLANK,
  VS_PAGE_SOCIAL_ENTER,
  VS_PAGE_SOCIAL_STARTING,
  VS_PAGE_SOCIAL_RUNNING,
  VS_PAGE_SOCIAL_ALERT,
  VS_PAGE_SOCIAL_PAUSING,
  VS_PAGE_SOCIAL_PAUSED,
  VS_PAGE_SOCIAL_RESUMING,
  VS_PAGE_SOCIAL_EXITING,
  VS_PAGE_SOCIAL_FINALIZING,
  VS_PAGE_SOCIAL_RESULT,
  VS_PAGE_VOICE_LISTENING,
  VS_PAGE_VOICE_THINKING,
  VS_PAGE_VOICE_SPEAKING,
  VS_PAGE_PHOTO_CAPTURE,
  VS_PAGE_NET_SWITCHING,
  VS_PAGE_SOFTAP,
  VS_PAGE_ERROR
};

enum vs_history_view_e
{
  VS_HISTORY_SUMMARY = 0,
  VS_HISTORY_CHART,
  VS_HISTORY_LEGEND,
  VS_HISTORY_VIEW_COUNT
};

enum vs_net_mode_e
{
  VS_NET_STA = 0,
  VS_NET_AP
};

enum vs_net_state_e
{
  VS_NET_DOWN = 0,
  VS_NET_SWITCHING,
  VS_NET_STA_READY,
  VS_NET_AP_READY,
  VS_NET_ERROR
};

struct vs_net_status_s
{
  enum vs_net_mode_e mode;
  enum vs_net_state_e state;
  int error;
  char ssid[33];
  char password[64];
  char address[16];
  char error_reason[VS_TEXT_SHORT];
};

struct vs_history_item_s
{
  const char *date;
  const char *title;
  const char *summary;
  uint8_t calm;
  uint8_t happy;
  uint8_t tense;
  bool incomplete;
};

enum vs_emotion_e
{
  VS_EMOTION_NONE = 0,
  VS_EMOTION_CALM,
  VS_EMOTION_HAPPY,
  VS_EMOTION_CONFUSED,
  VS_EMOTION_TENSE
};

enum vs_progress_kind_e
{
  VS_PROGRESS_NONE = 0,
  VS_PROGRESS_HOLD,
  VS_PROGRESS_WAIT
};

struct vs_softkey_s
{
  bool visible;
  bool highlighted;
  char text[VS_TEXT_SHORT];
};

struct vs_ui_snapshot_s
{
  enum vs_page_e page;
  enum vs_history_view_e history_view;
  struct vs_net_status_s network;
  const struct vs_history_item_s *history;
  uint8_t history_index;
  uint8_t history_count;
  uint8_t progress;
  enum vs_progress_kind_e progress_kind;
  enum vs_emotion_e emotion;
  uint32_t emotion_color;
  bool history_is_blank;
  bool photo_context;
  bool error_retryable;
  bool wifi_ready;
  bool battery_present;
  bool api_ready;
  char error_reason[VS_TEXT_LONG];
  bool response_active;
  enum vs_key_e response_key;
  struct vs_softkey_s softkey[VS_KEY_COUNT];
  char content_title[VS_TEXT_SHORT];
  char content_body[VS_TEXT_LONG];
  char content_meta[VS_TEXT_SHORT];
  char status_title[VS_TEXT_SHORT];
  char status_value[VS_TEXT_SHORT];
  char status_meta[VS_TEXT_SHORT];
};

#endif
