#ifndef __APP_VELASIGHT_INCLUDE_VS_TYPES_H
#define __APP_VELASIGHT_INCLUDE_VS_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
/* Thread priorities, relative to SCHED_PRIORITY_DEFAULT (100).
 *
 * They were all left at the default, which put the UI loop, the voice worker
 * and the audio threads at the same level.  With CONFIG_RR_INTERVAL=200 that
 * means a thread which does not yield promptly can hold the others off, and
 * the two that must not be held off are the ones moving audio: the capture and
 * playback threads keep the driver's 256 ms queue serviced, and the voice
 * worker drains the TTS socket -- while it waits, the IOBs holding that data
 * are not returned to the pool and the sender's window stays shut.
 *
 * The UI is the one that can afford to wait, so it stays at the bottom.
 * Everything here is far below CONFIG_SCHED_HPWORKPRIORITY (224), so the
 * network stack still preempts all of it.
 */

/* Moves samples between the driver and the staging rings.  Highest of the
 * four: it runs briefly and often, and being late shows up as a gap the
 * listener hears.
 */

#define VS_PRIORITY_AUDIO  (SCHED_PRIORITY_DEFAULT + 10)

/* Runs the cloud round trip, including the socket reads that free IOBs. */

#define VS_PRIORITY_VOICE  (SCHED_PRIORITY_DEFAULT + 5)

/* Key sampling.  Above the UI so a press is timestamped promptly, below the
 * audio path because 5 ms of jitter in a debounce window is invisible.
 */

#define VS_PRIORITY_INPUT  (SCHED_PRIORITY_DEFAULT + 1)

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

  /* Last stop of the main carousel, after the history records.  Grouped with
   * the other two browsable pages rather than with the modal ones because the
   * three of them are one ring: blank -> records -> volume -> blank.
   */

  VS_PAGE_VOLUME,

  /* Shown while a setting is being committed to SD-NAND.  A rename plus a
   * sync on this card takes long enough to read as the UI having frozen, and
   * the page it freezes on is the one the user is trying to leave.
   */

  VS_PAGE_SAVING,
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

enum vs_wifi_issue_e
{
  VS_WIFI_ISSUE_NONE = 0,
  VS_WIFI_ISSUE_SSID_NOT_FOUND,
  VS_WIFI_ISSUE_PASSWORD,
  VS_WIFI_ISSUE_DISCONNECTED
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
  enum vs_wifi_issue_e wifi_issue;
  uint8_t ap_client_count;
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

  /* Ring filling while a key is held.  The key hints are hidden for it,
   * because during a hold the only key that matters is the one being held.
   */

  VS_PROGRESS_HOLD,

  /* Animated dots in the left footer while something is in flight. */

  VS_PROGRESS_WAIT,

  /* Same ring as HOLD, showing a setting rather than the progress of a
   * gesture.  It keeps the key hints visible: a level the user is adjusting
   * is exactly when they need to be told which key does what.
   */

  VS_PROGRESS_LEVEL
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
  uint16_t history_index;
  uint16_t history_count;
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
