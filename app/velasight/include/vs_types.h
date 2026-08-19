#ifndef __APP_VELASIGHT_INCLUDE_VS_TYPES_H
#define __APP_VELASIGHT_INCLUDE_VS_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define VS_TEXT_SHORT 32
#define VS_TEXT_LONG  96

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
};

enum vs_page_e
{
  VS_PAGE_HISTORY = 0,
  VS_PAGE_SOCIAL_ENTER,
  VS_PAGE_SOCIAL_RUNNING,
  VS_PAGE_SOCIAL_PAUSED,
  VS_PAGE_SOCIAL_EXITING,
  VS_PAGE_VOICE_LISTENING,
  VS_PAGE_VOICE_THINKING,
  VS_PAGE_VOICE_SPEAKING,
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
  char address[16];
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

struct vs_ui_snapshot_s
{
  enum vs_page_e page;
  enum vs_history_view_e history_view;
  struct vs_net_status_s network;
  const struct vs_history_item_s *history;
  uint8_t history_index;
  uint8_t history_count;
  uint8_t progress;
  bool network_target_ap;
  bool show_progress;
  char primary[VS_TEXT_SHORT];
  char secondary[VS_TEXT_LONG];
};

#endif
