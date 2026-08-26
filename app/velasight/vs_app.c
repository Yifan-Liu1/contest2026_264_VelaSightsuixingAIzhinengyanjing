#include <nuttx/config.h>
#include <nuttx/sched.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arch/board/board.h>

#include <agent_config.h>

#include "include/vs_app.h"
#include "include/vs_audio.h"
#include "include/vs_display.h"
#include "include/vs_history.h"
#include "include/vs_input.h"
#include "include/vs_network.h"
#include "include/vs_settings.h"
#include "include/vs_voice.h"

/* Shown on the listening page, so it has to agree with the value vs_voice.c
 * actually enforces.  Same fallback as there, for a build without the app's
 * Kconfig fragment. */

#ifndef CONFIG_VS_VOICE_RECORD_MAX_MS
#  define CONFIG_VS_VOICE_RECORD_MAX_MS 15000
#endif

#define VS_INPUT_EVENT_QUEUE_SIZE 64
#define VS_INPUT_EVENTS_PER_FRAME 8
#define VS_RESPONSE_VISIBLE_MS 200
#define VS_WIFI_RETRY_MS 20000

#define VS_APP_EVENT_QUEUE_SIZE 8

static struct
{
  pthread_mutex_t lock;
  struct vs_app_event_s event[VS_APP_EVENT_QUEUE_SIZE];
  uint8_t read;
  uint8_t write;
  uint8_t count;
} g_app_events =
{
  .lock = PTHREAD_MUTEX_INITIALIZER
};

static struct vs_network_s *g_network_result;
static uint32_t g_active_request_id;

static struct
{
  pthread_mutex_t lock;
  struct vs_input_event_s event[VS_INPUT_EVENT_QUEUE_SIZE];
  uint8_t read;
  uint8_t write;
  uint8_t count;
} g_input_events =
{
  .lock = PTHREAD_MUTEX_INITIALIZER
};

static struct vs_input_s *g_input_worker_state;

static void vs_input_queue_reset(void)
{
  pthread_mutex_lock(&g_input_events.lock);
  g_input_events.read = 0;
  g_input_events.write = 0;
  g_input_events.count = 0;
  pthread_mutex_unlock(&g_input_events.lock);
}

static void vs_input_queue_push(const struct vs_input_event_s *event)
{
  pthread_mutex_lock(&g_input_events.lock);

  /* Progress is state, not an action.  If the UI is blocked on LCD I/O,
   * retain the newest progress value instead of filling the queue with stale
   * intermediate values. */
  if (event->type == VS_INPUT_PROGRESS ||
      event->type == VS_INPUT_COMBO_PROGRESS)
    {
      uint8_t index;
      uint8_t i;

      for (i = 0, index = g_input_events.read;
           i < g_input_events.count;
           i++, index = (index + 1) % VS_INPUT_EVENT_QUEUE_SIZE)
        {
          if (g_input_events.event[index].type == event->type &&
              g_input_events.event[index].key == event->key)
            {
              g_input_events.event[index] = *event;
              pthread_mutex_unlock(&g_input_events.lock);
              return;
            }
        }
    }

  if (g_input_events.count == VS_INPUT_EVENT_QUEUE_SIZE)
    {
      /* Keep action events by discarding the oldest queued event.  Progress
       * events are coalesced above and are safe to lose when saturated. */
      g_input_events.read =
        (g_input_events.read + 1) % VS_INPUT_EVENT_QUEUE_SIZE;
      g_input_events.count--;
    }

  g_input_events.event[g_input_events.write] = *event;
  g_input_events.write =
    (g_input_events.write + 1) % VS_INPUT_EVENT_QUEUE_SIZE;
  g_input_events.count++;
  pthread_mutex_unlock(&g_input_events.lock);
}

static bool vs_input_queue_pop(struct vs_input_event_s *event)
{
  bool available = false;

  pthread_mutex_lock(&g_input_events.lock);
  if (g_input_events.count != 0)
    {
      *event = g_input_events.event[g_input_events.read];
      g_input_events.read =
        (g_input_events.read + 1) % VS_INPUT_EVENT_QUEUE_SIZE;
      g_input_events.count--;
      available = true;
    }
  pthread_mutex_unlock(&g_input_events.lock);
  return available;
}

static int vs_input_worker(int argc, FAR char *argv[])
{
  struct vs_input_event_s event;

  (void)argc;
  (void)argv;
  while (g_input_worker_state != NULL)
    {
      if (vs_input_poll(g_input_worker_state, &event) > 0)
        vs_input_queue_push(&event);
      usleep(CONFIG_VS_INPUT_POLL_MS * 1000);
    }

  return 0;
}

static uint32_t vs_app_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

struct vs_runtime_s
{
  enum vs_page_e page;
  enum vs_history_view_e view;
  uint16_t index;
  uint8_t progress;
  bool history_blank;
  bool photo_context;
  bool voice_ending;

  /* Volume page state.  volume_level is the ring's 0..100 reading, which is
   * also what gets scaled to the driver's thousandths; volume_editing is
   * whether the browse keys currently move it or still turn pages.
   */

  uint8_t volume_level;
  bool volume_editing;

  /* Set when the level has moved since it was last written.  Volume is saved
   * when the user finishes adjusting rather than on every key press: one
   * completed action is one record, which is how the provisioning store is
   * used too, and it keeps a rename plus a sync off the path of a key the user
   * may be about to press again.
   */

  bool volume_dirty;

  /* A store write waiting for its progress frame to be painted, and the page
   * to return to once it completes.  See vs_request_save().
   */

  bool pending_save;
  enum vs_page_e save_resume_page;

  /* Set from the key press until VS_APP_EVENT_VOICE_LISTENING_READY says the
   * microphone is open.  The listening page shows a preparing state while it
   * is set, so the user is not asked to speak before anything can hear them. */

  bool voice_arming;
  enum vs_page_e social_entry_return_page;
  enum vs_page_e social_exit_return_page;
  enum vs_emotion_e emotion;
  uint32_t emotion_color;
  int error;
  char error_reason[VS_TEXT_LONG];
  bool error_retryable;
  enum vs_net_mode_e error_target_mode;
  enum vs_page_e error_return_page;
  bool network_busy;
  enum vs_net_mode_e network_target_mode;
  uint32_t wifi_retry_at_ms;
  uint32_t next_request_id;
  uint32_t active_request_id;
  bool api_ready;
  uint32_t response_until_ms;
  enum vs_key_e response_key;
  bool response_pending_visible;
  char response_text[VS_TEXT_SHORT];
  struct vs_net_status_s network;
  char alert_text[VS_TEXT_LONG];
  char result_text[VS_TEXT_LONG];
};

struct vs_network_worker_s
{
  struct vs_network_s *network;
  enum vs_net_mode_e mode;
};

int vs_app_post_event(const struct vs_app_event_s *event)
{
  if (event == NULL || event->type < VS_APP_EVENT_PHOTO_READY ||
      event->type > VS_APP_EVENT_NETWORK_FAILED)
    return -EINVAL;

  pthread_mutex_lock(&g_app_events.lock);
  if (g_app_events.count == VS_APP_EVENT_QUEUE_SIZE)
    {
      pthread_mutex_unlock(&g_app_events.lock);
      return -EAGAIN;
    }

  g_app_events.event[g_app_events.write] = *event;
  g_app_events.event[g_app_events.write].text[VS_TEXT_LONG - 1] = '\0';
  g_app_events.write = (g_app_events.write + 1) % VS_APP_EVENT_QUEUE_SIZE;
  g_app_events.count++;
  pthread_mutex_unlock(&g_app_events.lock);
  return 0;
}

uint32_t vs_app_current_request_id(void)
{
  uint32_t request_id;

  pthread_mutex_lock(&g_app_events.lock);
  request_id = g_active_request_id;
  pthread_mutex_unlock(&g_app_events.lock);
  return request_id;
}

static bool vs_app_pop_event(struct vs_app_event_s *event)
{
  bool available = false;

  pthread_mutex_lock(&g_app_events.lock);
  if (g_app_events.count != 0)
    {
      *event = g_app_events.event[g_app_events.read];
      g_app_events.read = (g_app_events.read + 1) % VS_APP_EVENT_QUEUE_SIZE;
      g_app_events.count--;
      available = true;
    }
  pthread_mutex_unlock(&g_app_events.lock);
  return available;
}

static struct vs_network_s *vs_take_network_result(void)
{
  struct vs_network_s *network;

  pthread_mutex_lock(&g_app_events.lock);
  network = g_network_result;
  g_network_result = NULL;
  pthread_mutex_unlock(&g_app_events.lock);
  return network;
}

static void *vs_network_start_worker(void *arg)
{
  struct vs_network_worker_s *worker = arg;
  struct vs_app_event_s event;
  struct vs_network_s *network = worker->network;
  int ret;

  memset(&event, 0, sizeof(event));
#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  if (network == NULL)
    ret = bk7258_wifi_wait_ready();
  else
    ret = 0;
#else
  ret = 0;
#endif
  if (ret == 0 && network == NULL)
    ret = vs_network_open(&network);
  if (ret == 0)
    ret = vs_network_request_mode(network, worker->mode);

  pthread_mutex_lock(&g_app_events.lock);
  g_network_result = network;
  pthread_mutex_unlock(&g_app_events.lock);
  event.type = ret < 0 ? VS_APP_EVENT_NETWORK_FAILED :
                         VS_APP_EVENT_NETWORK_READY;
  event.error = ret;
  while (vs_app_post_event(&event) == -EAGAIN)
    usleep(10000);
  free(worker);
  return NULL;
}

static int vs_start_network_worker(struct vs_runtime_s *runtime,
                                   struct vs_network_s *network,
                                   enum vs_net_mode_e mode)
{
  struct vs_network_worker_s *worker;
  pthread_t thread;
  pthread_attr_t attr;
  int ret;

  if (runtime->network_busy)
    return -EBUSY;

  worker = calloc(1, sizeof(*worker));
  if (worker == NULL)
    return -ENOMEM;

  worker->network = network;
  worker->mode = mode;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  ret = pthread_create(&thread, &attr, vs_network_start_worker, worker);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      free(worker);
      return -ret;
    }

  pthread_detach(thread);
  runtime->network_busy = true;
  runtime->network_target_mode = mode;
  if (mode == VS_NET_AP)
    runtime->wifi_retry_at_ms = 0;
  return 0;
}

static void vs_update_wifi_retry(struct vs_runtime_s *runtime)
{
  if (runtime->network.mode == VS_NET_STA &&
      runtime->network.state != VS_NET_STA_READY &&
      runtime->network.wifi_issue != VS_WIFI_ISSUE_PASSWORD &&
      runtime->network.error != -EINVAL &&
      runtime->network.error != -EBADMSG)
    {
      runtime->wifi_retry_at_ms = vs_app_now_ms() + VS_WIFI_RETRY_MS;
      printf("velasight: STA retry scheduled in %u ms\n",
             VS_WIFI_RETRY_MS);
    }
  else
    {
      runtime->wifi_retry_at_ms = 0;
    }
}

static uint32_t vs_begin_request(struct vs_runtime_s *runtime)
{
  runtime->next_request_id++;
  if (runtime->next_request_id == 0)
    runtime->next_request_id++;

  runtime->active_request_id = runtime->next_request_id;
  pthread_mutex_lock(&g_app_events.lock);
  g_active_request_id = runtime->active_request_id;
  pthread_mutex_unlock(&g_app_events.lock);
  return runtime->active_request_id;
}

static void vs_cancel_request(struct vs_runtime_s *runtime)
{
  runtime->active_request_id = 0;
  pthread_mutex_lock(&g_app_events.lock);
  g_active_request_id = 0;
  pthread_mutex_unlock(&g_app_events.lock);
}

/* Keep the request id live until the worker has persisted all completed
 * turns and posts a terminal event.  Dropping it here would make the normal
 * request-id filter discard that event and could leave a detached worker
 * running behind a page that already returned to history. */

static void vs_end_voice_conversation(struct vs_runtime_s *runtime)
{
  int ret;

  if (runtime->voice_ending)
    {
      return;
    }

  ret = vs_voice_end_conversation();
  if (ret < 0 && ret != -EINVAL)
    {
      printf("velasight: failed to request conversation end (%d)\n", ret);
    }

  /* -EINVAL can mean the worker has already completed and its terminal
   * event is queued.  Continue waiting with the request id intact. */

  runtime->voice_ending = true;
}

/* One key press of volume, as a percentage of full scale.
 *
 * Chosen against the driver's quantisation rather than for a round number of
 * steps: it maps 0..1000 onto six bits of digital gain, so one gain step is
 * about 1.6% and anything finer than that would give presses that change
 * nothing.  Five percent is three gain steps, which is always audible.
 */

#define VS_VOLUME_STEP 5

/* Used only if the driver will not say what the gain is.  Close to the 0 dB
 * point it powers up at, so the ring is not wildly wrong even then.
 */

#define VS_VOLUME_FALLBACK 70

static uint8_t vs_volume_round(unsigned int percent)
{
  if (percent > 100)
    percent = 100;

  /* Snapped to the step so that the first press moves a whole step instead of
   * first correcting an offset the user cannot see.
   */

  return (uint8_t)((percent + VS_VOLUME_STEP / 2) / VS_VOLUME_STEP *
                   VS_VOLUME_STEP);
}

/* A word for the level, so the left screen carries something the right screen
 * does not.  The specification forbids the two screens showing the same core
 * field, and a percentage is what the right screen already shows.
 */

static const char *vs_volume_word(unsigned int level)
{
  if (level == 0)
    return "已静音";
  if (level <= 30)
    return "较轻";
  if (level <= 60)
    return "适中";
  if (level < 100)
    return "较响";

  return "最大";
}

/* Push a level to the DAC and adopt whatever it actually settled on.
 *
 * The read-back matters: the driver quantises to six bits of gain, so the value
 * it keeps differs from the request by up to half a step, and a ring that
 * tracked requests would drift away from the hardware over a series of
 * presses.  Used for both a key press and a level restored from storage, so
 * the two cannot disagree about what the displayed number means.
 */

static void vs_apply_volume(struct vs_runtime_s *runtime, unsigned int level)
{
  unsigned int applied = 0;

  if (level > 100)
    level = 100;

  if (vs_audio_volume_set(AGENT_AUDIO_PLAYBACK_DEV, level * 10u) < 0)
    return;

  if (vs_audio_volume_get(AGENT_AUDIO_PLAYBACK_DEV, &applied) == 0)
    runtime->volume_level = vs_volume_round(applied / 10u);
  else
    runtime->volume_level = (uint8_t)level;
}

static void vs_adjust_volume(struct vs_runtime_s *runtime, bool louder)
{
  unsigned int level = runtime->volume_level;

  if (louder)
    level = level + VS_VOLUME_STEP > 100 ? 100 : level + VS_VOLUME_STEP;
  else
    level = level < VS_VOLUME_STEP ? 0 : level - VS_VOLUME_STEP;

  vs_apply_volume(runtime, level);
  runtime->volume_dirty = true;
}

/* Ask for the pending write to happen, and say where to go afterwards.
 *
 * Deliberately does not write anything.  This runs inside vs_handle_event(),
 * which only mutates runtime -- the frame it produces is not painted until
 * vs_display_tick() runs at the bottom of the main loop.  A rename plus a sync
 * on SD-NAND takes long enough to be seen, so doing it here would freeze the
 * page the user is trying to leave and then jump straight to the destination,
 * which is exactly the delay it looks like.
 *
 * Instead the page becomes VS_PAGE_SAVING now, the main loop paints it, and the
 * write happens on the following pass.  vs_flush_pending_save() then restores
 * the destination page.
 *
 * Callers must set the page they want to end up on before calling this, or pass
 * it as resume.
 */

static void vs_request_save(struct vs_runtime_s *runtime,
                            enum vs_page_e resume)
{
  if (!runtime->volume_dirty)
    {
      runtime->page = resume;
      return;
    }

  runtime->save_resume_page = resume;
  runtime->pending_save = true;
  runtime->page = VS_PAGE_SAVING;
}

/* Runs from the main loop, after the saving page has been painted.  This is the
 * only place that blocks on the store from the UI thread.
 */

static void vs_flush_pending_save(struct vs_runtime_s *runtime)
{
  int ret;

  if (!runtime->pending_save)
    return;

  runtime->pending_save = false;

  if (runtime->volume_dirty)
    {
      ret = vs_settings_save_volume(runtime->volume_level);
      if (ret < 0)
        printf("velasight: volume %u%% not saved (%d)\n",
               runtime->volume_level, ret);
      else
        runtime->volume_dirty = false;
    }

  runtime->page = runtime->save_resume_page;
}

static void vs_key_set(struct vs_ui_snapshot_s *snapshot,
                       enum vs_key_e key, const char *text)
{
  snapshot->softkey[key].visible = text != NULL && text[0] != '\0';
  snprintf(snapshot->softkey[key].text, sizeof(snapshot->softkey[key].text),
           "%s", text != NULL ? text : "");
}

static const char *vs_errno_reason(int error)
{
  switch (error < 0 ? -error : error)
    {
      case ETIMEDOUT:
        return "操作超时";
      case ENODEV:
        return "设备不可用";
      case ENOENT:
        return "配置不存在";
      case EINVAL:
        return "参数无效";
      case EBADMSG:
        return "返回数据为空或格式错误";
      case EIO:
        return "设备读写失败";
      case ENOMEM:
        return "内存不足";
      default:
        return "系统操作失败";
    }
}

/* vs_voice.c reports several failure modes vs_errno_reason() above was never
 * meant to cover (that table is shared with network/config errors, whose
 * ENOENT already means something else there).  This wraps it with the idle
 * assistant's own vocabulary so the on-screen reason matches what actually
 * failed instead of falling through to a generic "系统操作失败" plus a bare
 * errno number. */

static const char *vs_assistant_error_reason(int error)
{
  switch (error < 0 ? -error : error)
    {
      case ENOKEY:
        return "语音服务凭据未配置";
      case ENODATA:
        return "未听清，请重试";
      case EILSEQ:
        return "识别结果异常";
      case EMSGSIZE:
        return "记录内容过长";
      case ENOENT:
        return "记录读取失败";
      case EBUSY:
        return "上一次请求尚未结束";
      default:
        return vs_errno_reason(error);
    }
}

static void vs_snapshot(struct vs_runtime_s *runtime,
                        struct vs_ui_snapshot_s *snapshot)
{
  struct vs_history_index_s current;
  bool have_current;

  have_current = !runtime->history_blank &&
                 vs_history_get_index(VS_HISTORY_KIND_SOCIAL, runtime->index,
                                      &current) == 0;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->page = runtime->page;
  snapshot->history_view = runtime->view;
  snapshot->network = runtime->network;
  snapshot->history = NULL;
  snapshot->history_index = runtime->index;
  snapshot->history_count = vs_history_count(VS_HISTORY_KIND_SOCIAL);
  snapshot->history_is_blank = runtime->history_blank;
  snapshot->photo_context = runtime->photo_context;
  snapshot->progress = runtime->progress;
  snapshot->emotion = runtime->emotion;
  snapshot->response_active = runtime->response_until_ms != 0;
  snapshot->response_key = runtime->response_key;
  snapshot->error_retryable = runtime->error_retryable;
  snapshot->wifi_ready = runtime->network.state == VS_NET_STA_READY ||
                         runtime->network.state == VS_NET_AP_READY;
  snapshot->battery_present = false;
  /* UI snapshots are a latency-sensitive hot path.  Never read SD-NAND here;
   * persistent state must be loaded by startup or background event handling. */
  snapshot->api_ready = runtime->api_ready;
  snprintf(snapshot->error_reason, sizeof(snapshot->error_reason), "%s",
           runtime->error_reason);
  snapshot->emotion_color = runtime->emotion_color != 0 ?
                            runtime->emotion_color :
                            runtime->emotion == VS_EMOTION_TENSE ? 0xe85d5d :
                            runtime->emotion == VS_EMOTION_CONFUSED ? 0xe3ad4b :
                            runtime->emotion == VS_EMOTION_HAPPY ? 0x48c78e :
                            0xe8eef2;

  switch (runtime->page)
    {
      case VS_PAGE_PREPARING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在准备");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "VelaSight 即将就绪");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "启动");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "准备中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请稍等");
        snapshot->progress_kind = VS_PROGRESS_WAIT;
        break;

      case VS_PAGE_HISTORY:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "%s", have_current ? current.title : "");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "%s", have_current ? current.summary : "");
        snprintf(snapshot->content_meta, sizeof(snapshot->content_meta),
                 "%s", have_current ? current.date : "");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "历史");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "%02u/%02u", runtime->index + 1,
                 vs_history_count(VS_HISTORY_KIND_SOCIAL));
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                 "已保存");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "询问");
        vs_key_set(snapshot, VS_KEY_BACK, "上一条");
        vs_key_set(snapshot, VS_KEY_NEXT, "下一条");
        break;

      case VS_PAGE_HISTORY_BLANK:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "拍照提问");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "拍一张照片\n然后直接问我");
        snapshot->content_meta[0] = '\0';
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "AI");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "照片问答");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                 "准备好");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "拍照提问");
        vs_key_set(snapshot, VS_KEY_BACK, "上一条");
        vs_key_set(snapshot, VS_KEY_NEXT, "下一条");
        break;

      case VS_PAGE_VOLUME:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "音量");

        /* Middle layer describes what is being set and how loud it now is in
         * words.  It deliberately does not name the keys: the right screen's
         * footer is where key hints belong, and spelling them out in the body
         * duplicated them in a form nobody reads twice.
         */

        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "语音播报\n%s", vs_volume_word(runtime->volume_level));

        /* Lower rows: the address itself on the wider first row, its label on
         * the narrow second row.  The address is the longer meta field that
         * row exists for, and the label fits the four-character limit on the
         * row below it.
         */

        snprintf(snapshot->content_meta, sizeof(snapshot->content_meta),
                 "%s", runtime->network.address);
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "%s",
                 runtime->network.address[0] != '\0' ? "IP地址" : "未联网");

        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "音量");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "%u%%", runtime->volume_level);
        snapshot->progress_kind = VS_PROGRESS_LEVEL;
        snapshot->progress = runtime->volume_level;

        /* Next is louder, matching the direction the ring fills and the order
         * the two keys sit in on the footer. */

        if (runtime->volume_editing)
          {
            vs_key_set(snapshot, VS_KEY_CONFIRM, "完成");
            vs_key_set(snapshot, VS_KEY_BACK, "调小");
            vs_key_set(snapshot, VS_KEY_NEXT, "调大");
          }
        else
          {
            vs_key_set(snapshot, VS_KEY_CONFIRM, "调节");
            vs_key_set(snapshot, VS_KEY_BACK, "上一条");
            vs_key_set(snapshot, VS_KEY_NEXT, "下一条");
          }
        break;

      case VS_PAGE_SAVING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "保存设置");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "正在写入");
        snapshot->content_meta[0] = '\0';
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "设置");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "保存中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请稍等");

        /* No ring: the write is one indivisible operation with no progress to
         * report, and the specification forbids inventing a percentage.  WAIT
         * gives the left footer its dotted animation instead.
         */

        snapshot->progress_kind = VS_PROGRESS_WAIT;
        break;

      case VS_PAGE_SOCIAL_ENTER:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "进入社交");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "继续按住\n松开取消");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "社交");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "%u%%",
                 runtime->progress);
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "按住中");
        snapshot->progress_kind = VS_PROGRESS_HOLD;
        break;

      case VS_PAGE_SOCIAL_STARTING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "启动社交");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "正在建立会话");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "社交");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "连接中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请稍等");
        vs_key_set(snapshot, VS_KEY_BACK, "取消");
        break;

      case VS_PAGE_SOCIAL_RUNNING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "社交中");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "继续交流");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "社交中");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "%s", runtime->emotion == VS_EMOTION_NONE ? "观察中" :
                  runtime->emotion == VS_EMOTION_TENSE ? "情绪升高" : "观察中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "采集中");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "暂停");
        break;

      case VS_PAGE_SOCIAL_ALERT:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "情绪提醒");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body), "%s",
                 runtime->alert_text[0] != '\0' ? runtime->alert_text :
                 "放慢语速\n先听对方说完");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "情绪升高");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "提醒");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请留意");
        snapshot->emotion = runtime->emotion;
        vs_key_set(snapshot, VS_KEY_CONFIRM, "暂停");
        break;

      case VS_PAGE_SOCIAL_PAUSING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在暂停");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "正在停止采集");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "社交");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "请稍等");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "暂停中");
        snapshot->progress_kind = VS_PROGRESS_WAIT;
        break;

      case VS_PAGE_SOCIAL_PAUSED:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "已暂停");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "准备好后继续");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "社交");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "暂停");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "已暂停");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "继续");
        break;

      case VS_PAGE_SOCIAL_RESUMING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在继续");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "正在恢复采集");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "社交");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "请稍等");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "恢复中");
        snapshot->progress_kind = VS_PROGRESS_WAIT;
        break;

      case VS_PAGE_SOCIAL_EXITING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "结束交流");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "继续按住");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "结束");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "%u%%",
                 runtime->progress);
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "按住中");
        snapshot->progress_kind = VS_PROGRESS_HOLD;
        break;

      case VS_PAGE_SOCIAL_FINALIZING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "整理记录");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "正在生成建议");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "整理中");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "处理中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请稍等");
        snapshot->progress_kind = VS_PROGRESS_WAIT;
        break;

      case VS_PAGE_SOCIAL_RESULT:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "交流摘要");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body), "%s",
                 runtime->result_text[0] != '\0' ? runtime->result_text :
                 "交流记录已整理完成");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "结果");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "摘要已生成");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "待播报");
        vs_key_set(snapshot, VS_KEY_BACK, "返回");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "完成");
        break;

      case VS_PAGE_VOICE_LISTENING:
        if (runtime->voice_ending)
          {
            snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                     "正在结束");
            snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                     "保存已完成问答");
            snprintf(snapshot->status_title, sizeof(snapshot->status_title),
                     "会话");
            snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                     "请稍等");
            snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                     "写入历史");
            snapshot->progress_kind = VS_PROGRESS_WAIT;
            break;
          }

        if (runtime->voice_arming)
          {
            /* Not listening yet: the round is still loading the referenced
             * record and completing the TLS handshake to the ASR service.
             * Saying "请说话" here would lose the user's opening words. */

            snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                     "正在准备");
            snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                     "连接语音服务\n稍后再说话");
            snprintf(snapshot->status_title, sizeof(snapshot->status_title),
                     "聆听");
            snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                     "准备中");
            snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                     "请稍等");
            snapshot->progress_kind = VS_PROGRESS_WAIT;
            vs_key_set(snapshot, VS_KEY_BACK, "退出");
            break;
          }

        /* Pressing 说完 is the way an utterance ends, so the page says so
         * rather than describing a silence timer.  There is no local speech
         * detector any more: an energy threshold miscalibrated for this
         * board's microphone gain used to discard whole recordings, so the
         * keys drive the round and the listening window is only a bound on
         * how long a user who walked away is recorded. */

        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "请说话");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "说完按确认\n最长%u秒",
                 (unsigned int)(CONFIG_VS_VOICE_RECORD_MAX_MS / 1000));
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "聆听");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "%s", runtime->photo_context ? "照片问题" : "正在听");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "录音中");
        vs_key_set(snapshot, VS_KEY_BACK, "退出");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "说完");
        break;

      case VS_PAGE_VOICE_THINKING:
        if (runtime->voice_ending)
          {
            snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                     "正在结束");
            snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                     "保存已完成问答");
            snprintf(snapshot->status_title, sizeof(snapshot->status_title),
                     "会话");
            snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                     "请稍等");
            snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                     "写入历史");
            snapshot->progress_kind = VS_PROGRESS_WAIT;
            break;
          }

        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在思考");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "很快回答你");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "思考");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "请稍等");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "处理中");
        vs_key_set(snapshot, VS_KEY_BACK, "退出");
        break;

      case VS_PAGE_VOICE_SPEAKING:
        if (runtime->voice_ending)
          {
            snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                     "正在结束");
            snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                     "保存已完成问答");
            snprintf(snapshot->status_title, sizeof(snapshot->status_title),
                     "会话");
            snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                     "请稍等");
            snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                     "写入历史");
            snapshot->progress_kind = VS_PROGRESS_WAIT;
            break;
          }

        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "回答建议");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body), "%s",
                 runtime->result_text[0] != '\0' ? runtime->result_text :
                 "先听完对方\n再回应");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "回答");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "结果就绪");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                 "播报后继续聆听");
        vs_key_set(snapshot, VS_KEY_BACK, "退出");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "完成");
        break;

      case VS_PAGE_PHOTO_CAPTURE:
        if (runtime->voice_ending)
          {
            snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                     "正在结束");
            snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                     "保存已完成问答");
            snprintf(snapshot->status_title, sizeof(snapshot->status_title),
                     "会话");
            snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                     "请稍等");
            snprintf(snapshot->status_meta, sizeof(snapshot->status_meta),
                     "写入历史");
            snapshot->progress_kind = VS_PROGRESS_WAIT;
            break;
          }

        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在拍照");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "请看向目标");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "AI");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "拍摄中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请稍等");
        vs_key_set(snapshot, VS_KEY_BACK, "退出");
        break;

      case VS_PAGE_NET_SWITCHING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "切换网络");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "%s", runtime->network_busy ? "请稍等" :
                                                "继续按住\n松开取消");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "网络");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "%s",
                 runtime->network_busy ? "切换中" :
                  runtime->network.mode == VS_NET_STA ? "切换热点" :
                                                        "连接网络");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "%s",
                 runtime->network_busy ? "切换中" : "按住中");
        snapshot->progress_kind = runtime->network_busy ? VS_PROGRESS_WAIT :
                                                          VS_PROGRESS_HOLD;
        break;

      case VS_PAGE_SOFTAP:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "设备热点");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "密码: %s\n网页: %s", runtime->network.password,
                 runtime->network.address);
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "热点");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "%s",
                 runtime->network.ssid);
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "%s",
                 runtime->network.ap_client_count != 0 ? "已连接" :
                                                          "待连接");
        vs_key_set(snapshot, VS_KEY_BACK, "按住返回");
        break;

      case VS_PAGE_ERROR:
      default:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title), "出了问题");
         snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                  "%.80s\n%s", runtime->error_reason,
                  runtime->error_retryable ? "短按重试或返回" : "短按返回");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "错误");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "未完成");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请处理");
        vs_key_set(snapshot, VS_KEY_BACK, "返回");
        if (runtime->error_retryable)
          vs_key_set(snapshot, VS_KEY_CONFIRM, "重试");
        break;
    }

  if (snapshot->progress_kind == VS_PROGRESS_HOLD)
    snapshot->progress = runtime->progress;
  if (snapshot->response_active)
    {
      snapshot->softkey[runtime->response_key].visible = true;
      snapshot->softkey[runtime->response_key].highlighted = true;
      snprintf(snapshot->softkey[runtime->response_key].text,
               sizeof(snapshot->softkey[runtime->response_key].text), "%s",
               runtime->response_text);
    }
}

static void vs_render(struct vs_display_s *display,
                      struct vs_runtime_s *runtime)
{
  struct vs_ui_snapshot_s snapshot;

  vs_snapshot(runtime, &snapshot);
  (void)vs_display_render(display, &snapshot);
  if (runtime->response_pending_visible)
    {
      runtime->response_until_ms = vs_app_now_ms() + VS_RESPONSE_VISIBLE_MS;
      runtime->response_pending_visible = false;
    }
}

static void vs_expire_response(struct vs_runtime_s *runtime)
{
  if (runtime->response_until_ms != 0 &&
      (int32_t)(runtime->response_until_ms - vs_app_now_ms()) <= 0)
    {
      runtime->response_until_ms = 0;
      runtime->response_pending_visible = false;
    }
}

static void vs_set_response(struct vs_runtime_s *runtime, enum vs_key_e key,
                            const char *text)
{
  runtime->response_key = key;
  runtime->response_until_ms = vs_app_now_ms() + VS_RESPONSE_VISIBLE_MS;
  runtime->response_pending_visible = true;
  snprintf(runtime->response_text, sizeof(runtime->response_text), "%s", text);
}

static void vs_acknowledge(struct vs_runtime_s *runtime, enum vs_key_e key)
{
  struct vs_ui_snapshot_s snapshot;

  vs_snapshot(runtime, &snapshot);
  if (!snapshot.softkey[key].visible)
    return;

  vs_set_response(runtime, key, snapshot.softkey[key].text);
}

static void vs_set_error(struct vs_runtime_s *runtime, int error,
                         enum vs_page_e return_page, bool retryable);

static void vs_set_error_reason(struct vs_runtime_s *runtime, int error,
                                enum vs_page_e return_page, bool retryable,
                                const char *reason)
{
  vs_set_error(runtime, error, return_page, retryable);
  snprintf(runtime->error_reason, sizeof(runtime->error_reason), "%s (%d)",
           reason, runtime->error);
}

static void vs_switch_network(struct vs_display_s *display,
                              struct vs_runtime_s *runtime,
                              struct vs_network_s *network)
{
  enum vs_net_mode_e mode = runtime->network.mode == VS_NET_STA ?
                            VS_NET_AP : VS_NET_STA;
  int ret;

  runtime->progress = 100;
  runtime->page = VS_PAGE_NET_SWITCHING;
  ret = vs_start_network_worker(runtime, network, mode);
  if (ret < 0)
    {
      runtime->error_target_mode = mode;
      vs_set_error(runtime, ret,
                   runtime->network.mode == VS_NET_AP ? VS_PAGE_SOFTAP :
                   runtime->history_blank ? VS_PAGE_HISTORY_BLANK :
                                            VS_PAGE_HISTORY,
                   true);
    }
  else
    vs_render(display, runtime);
}

static void vs_set_error(struct vs_runtime_s *runtime, int error,
                         enum vs_page_e return_page, bool retryable)
{
  runtime->error = error != 0 ? error : -EIO;
  runtime->error_return_page = return_page;
  runtime->error_retryable = retryable;
  if (runtime->network.error == runtime->error &&
      runtime->network.error_reason[0] != '\0')
    snprintf(runtime->error_reason, sizeof(runtime->error_reason), "%s",
             runtime->network.error_reason);
  else
    snprintf(runtime->error_reason, sizeof(runtime->error_reason), "%s (%d)",
             vs_errno_reason(runtime->error), runtime->error);
  runtime->page = VS_PAGE_ERROR;
}

static void vs_handle_app_event(struct vs_runtime_s *runtime,
                                struct vs_network_s *network,
                                const struct vs_app_event_s *event)
{
  if (event->type < VS_APP_EVENT_NETWORK_READY &&
      (event->request_id == 0 ||
       event->request_id != runtime->active_request_id))
    {
      return;
    }

  switch (event->type)
    {
      case VS_APP_EVENT_PHOTO_READY:
        if (!runtime->voice_ending &&
            runtime->page == VS_PAGE_PHOTO_CAPTURE)
          {
            runtime->voice_arming = true;
            runtime->page = VS_PAGE_VOICE_LISTENING;
          }
        break;

      case VS_APP_EVENT_PHOTO_FAILED:
        runtime->voice_ending = false;
        vs_cancel_request(runtime);
        vs_set_error_reason(runtime, event->error, VS_PAGE_HISTORY_BLANK,
                            false,
                            vs_assistant_error_reason(event->error));
        break;

      case VS_APP_EVENT_VOICE_LISTENING_DONE:
        if (!runtime->voice_ending &&
            runtime->page == VS_PAGE_VOICE_LISTENING)
          {
            runtime->page = VS_PAGE_VOICE_THINKING;
          }
        break;

      case VS_APP_EVENT_VOICE_REPLY:
        if (!runtime->voice_ending &&
            runtime->page == VS_PAGE_VOICE_THINKING)
          {
            if (event->text[0] == '\0')
              {
                vs_end_voice_conversation(runtime);
              }
            else
              {
                snprintf(runtime->result_text, sizeof(runtime->result_text),
                         "%s", event->text);
                runtime->page = VS_PAGE_VOICE_SPEAKING;
              }
          }
        break;

      case VS_APP_EVENT_VOICE_LISTENING_AGAIN:
        if (!runtime->voice_ending &&
            (runtime->page == VS_PAGE_VOICE_SPEAKING ||
             runtime->page == VS_PAGE_VOICE_THINKING))
          {
            /* A follow-up round opens its own ASR session and microphone, so
             * it is armed here for the same reason the first round is. */

            runtime->voice_arming = true;
            runtime->page = VS_PAGE_VOICE_LISTENING;
          }
        break;

      case VS_APP_EVENT_VOICE_LISTENING_READY:
        runtime->voice_arming = false;
        break;

      case VS_APP_EVENT_VOICE_CONVERSATION_DONE:
        runtime->voice_ending = false;
        runtime->voice_arming = false;
        runtime->result_text[0] = '\0';
        vs_cancel_request(runtime);
        runtime->page = runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                VS_PAGE_HISTORY;
        break;

      case VS_APP_EVENT_VOICE_FAILED:
        runtime->voice_ending = false;
        runtime->voice_arming = false;
        vs_cancel_request(runtime);
        vs_set_error_reason(runtime, event->error,
                            runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                     VS_PAGE_HISTORY, false,
                            vs_assistant_error_reason(event->error));
        break;

      case VS_APP_EVENT_SOCIAL_STARTED:
        if (runtime->page == VS_PAGE_SOCIAL_STARTING)
          runtime->page = VS_PAGE_SOCIAL_RUNNING;
        break;

      case VS_APP_EVENT_SOCIAL_START_FAILED:
        if (runtime->page == VS_PAGE_SOCIAL_STARTING)
          {
            vs_cancel_request(runtime);
            vs_set_error_reason(runtime, event->error,
                                runtime->social_entry_return_page, false,
                                "社交会话启动失败");
          }
        break;

      case VS_APP_EVENT_SOCIAL_ALERT:
        if (runtime->page == VS_PAGE_SOCIAL_RUNNING ||
            runtime->page == VS_PAGE_SOCIAL_ALERT)
          {
            runtime->emotion = event->emotion;
            runtime->emotion_color = event->color;
            snprintf(runtime->alert_text, sizeof(runtime->alert_text), "%s",
                     event->text);
            runtime->page = VS_PAGE_SOCIAL_ALERT;
          }
        break;

      case VS_APP_EVENT_SOCIAL_ALERT_CLEARED:
        if (runtime->page == VS_PAGE_SOCIAL_ALERT ||
            runtime->page == VS_PAGE_SOCIAL_PAUSED)
          {
            runtime->emotion = VS_EMOTION_NONE;
            runtime->emotion_color = 0;
            runtime->alert_text[0] = '\0';
            if (runtime->page == VS_PAGE_SOCIAL_ALERT)
              runtime->page = VS_PAGE_SOCIAL_RUNNING;
          }
        break;

      case VS_APP_EVENT_SOCIAL_PAUSED:
        if (runtime->page == VS_PAGE_SOCIAL_PAUSING)
          runtime->page = VS_PAGE_SOCIAL_PAUSED;
        break;

      case VS_APP_EVENT_SOCIAL_RESUMED:
        if (runtime->page == VS_PAGE_SOCIAL_RESUMING)
          runtime->page = VS_PAGE_SOCIAL_RUNNING;
        break;

      case VS_APP_EVENT_SOCIAL_PAUSE_FAILED:
        if (runtime->page == VS_PAGE_SOCIAL_PAUSING ||
            runtime->page == VS_PAGE_SOCIAL_RESUMING)
          vs_set_error_reason(runtime, event->error, VS_PAGE_SOCIAL_RUNNING,
                              false, "社交会话操作失败");
        break;

      case VS_APP_EVENT_SOCIAL_RESULT:
        if (runtime->page == VS_PAGE_SOCIAL_FINALIZING)
          {
            if (event->text[0] == '\0')
              {
                vs_cancel_request(runtime);
                vs_set_error_reason(runtime, -EBADMSG, VS_PAGE_HISTORY, false,
                                    "社交摘要为空");
              }
            else
              {
                snprintf(runtime->result_text, sizeof(runtime->result_text),
                         "%s", event->text);
                runtime->page = VS_PAGE_SOCIAL_RESULT;
              }
          }
        break;

      case VS_APP_EVENT_SOCIAL_FINALIZE_FAILED:
        if (runtime->page == VS_PAGE_SOCIAL_FINALIZING)
          {
            vs_cancel_request(runtime);
            vs_set_error_reason(runtime, event->error, VS_PAGE_HISTORY, false,
                                "社交记录整理失败");
          }
        break;

      case VS_APP_EVENT_NETWORK_READY:
        runtime->network_busy = false;
        if (network != NULL)
          (void)vs_network_get_status(network, &runtime->network);
        runtime->api_ready = bk7258_ai_config_ready();
        runtime->error = 0;
        runtime->error_retryable = false;
        runtime->wifi_retry_at_ms = 0;
        if (runtime->page == VS_PAGE_PREPARING ||
            runtime->page == VS_PAGE_NET_SWITCHING ||
            runtime->page == VS_PAGE_SOFTAP)
          {
            runtime->page = runtime->network.mode == VS_NET_AP ?
                            VS_PAGE_SOFTAP :
                            runtime->history_blank ? VS_PAGE_HISTORY_BLANK :
                                                     VS_PAGE_HISTORY;
          }
        runtime->progress = 0;
        break;

      case VS_APP_EVENT_NETWORK_FAILED:
        runtime->network_busy = false;
        if (network != NULL)
          (void)vs_network_get_status(network, &runtime->network);
        runtime->api_ready = bk7258_ai_config_ready();
        runtime->error_target_mode = runtime->network_target_mode;
        if (runtime->network_target_mode == VS_NET_STA)
          {
            /* Failing to join the configured Wi-Fi is an offline state, not a
             * product error.  Keep the photo home usable and wait for the next
             * explicit network attempt instead of showing an error page. */

            runtime->error = event->error;
            runtime->error_retryable = false;
            runtime->error_reason[0] = '\0';
            if (network == NULL)
              runtime->network.error = event->error;
            vs_update_wifi_retry(runtime);
            if (runtime->page == VS_PAGE_PREPARING ||
                runtime->page == VS_PAGE_NET_SWITCHING)
              {
                runtime->page = runtime->history_blank ?
                                VS_PAGE_HISTORY_BLANK : VS_PAGE_HISTORY;
              }
            printf("velasight: STA unavailable (%d), continuing offline\n",
                   event->error);
          }
        else
          {
            vs_set_error(runtime, event->error,
                         runtime->network.mode == VS_NET_AP ? VS_PAGE_SOFTAP :
                         runtime->history_blank ? VS_PAGE_HISTORY_BLANK :
                                                  VS_PAGE_HISTORY,
                         network != NULL);
          }
        break;
    }
}

static void vs_handle_event(struct vs_display_s *display,
                            struct vs_runtime_s *runtime,
                            struct vs_network_s *network,
                            const struct vs_input_event_s *event)
{
  if (event->type == VS_INPUT_PRESS)
    return;

  if (event->type == VS_INPUT_COMBO_PROGRESS &&
      (runtime->page == VS_PAGE_HISTORY || runtime->page == VS_PAGE_HISTORY_BLANK ||
       (runtime->page == VS_PAGE_NET_SWITCHING && !runtime->network_busy)))
    {
      runtime->page = VS_PAGE_NET_SWITCHING;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_COMBO_CANCEL &&
      runtime->page == VS_PAGE_NET_SWITCHING && !runtime->network_busy)
    {
      runtime->page = runtime->network.mode == VS_NET_AP ? VS_PAGE_SOFTAP :
                      runtime->history_blank ? VS_PAGE_HISTORY_BLANK : VS_PAGE_HISTORY;
      runtime->progress = 0;
      return;
    }

  if (event->type == VS_INPUT_PROGRESS && event->key == VS_KEY_CONFIRM &&
      (runtime->page == VS_PAGE_HISTORY ||
       runtime->page == VS_PAGE_HISTORY_BLANK ||
       runtime->page == VS_PAGE_SOCIAL_ENTER))
    {
      if (runtime->page != VS_PAGE_SOCIAL_ENTER)
        runtime->social_entry_return_page = runtime->page;
      runtime->page = VS_PAGE_SOCIAL_ENTER;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_PROGRESS && event->key == VS_KEY_BACK &&
      (runtime->page == VS_PAGE_SOCIAL_RUNNING ||
       runtime->page == VS_PAGE_SOCIAL_PAUSED ||
       runtime->page == VS_PAGE_SOCIAL_ALERT ||
       runtime->page == VS_PAGE_SOCIAL_EXITING))
    {
      if (runtime->page != VS_PAGE_SOCIAL_EXITING)
        runtime->social_exit_return_page = runtime->page;
      runtime->page = VS_PAGE_SOCIAL_EXITING;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_PROGRESS && event->key == VS_KEY_BACK &&
      (runtime->page == VS_PAGE_SOFTAP ||
       (runtime->page == VS_PAGE_NET_SWITCHING &&
        runtime->network.mode == VS_NET_AP && !runtime->network_busy)))
    {
      runtime->page = VS_PAGE_NET_SWITCHING;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_CANCEL &&
      runtime->page == VS_PAGE_SOCIAL_ENTER)
    {
      runtime->page = runtime->social_entry_return_page;
      runtime->progress = 0;
      return;
    }

  if (event->type == VS_INPUT_CANCEL &&
      runtime->page == VS_PAGE_SOCIAL_EXITING)
    {
      runtime->page = runtime->social_exit_return_page;
      runtime->progress = 0;
      return;
    }

  if (event->type == VS_INPUT_CANCEL &&
      runtime->page == VS_PAGE_NET_SWITCHING && !runtime->network_busy)
    {
      runtime->page = runtime->network.mode == VS_NET_AP ? VS_PAGE_SOFTAP :
                      runtime->history_blank ? VS_PAGE_HISTORY_BLANK : VS_PAGE_HISTORY;
      runtime->progress = 0;
      return;
    }

  if (event->type == VS_INPUT_NET_TOGGLE &&
      runtime->page == VS_PAGE_NET_SWITCHING && !runtime->network_busy &&
      network != NULL)
    {
      vs_switch_network(display, runtime, network);
      return;
    }

  if (event->type == VS_INPUT_SHORT)
    {
      /* A short confirm first entered the hold candidate page through its
       * progress events.  Restore the source page so its visible short action
       * gets the response state before entering voice or photo mode. */
      if (runtime->page == VS_PAGE_SOCIAL_ENTER &&
          event->key == VS_KEY_CONFIRM)
        {
          runtime->page = runtime->social_entry_return_page;
          runtime->progress = 0;
        }

      /* Taken before the key is acted on, which is what makes it correct: the
       * label that flashes is the one the user pressed.  On the volume page
       * that means 确认 flashes "调节" and then becomes "完成", which is the
       * sequence that happened.  Only SOFTAP's back key opts out, because
       * there the short press has no action to acknowledge. */

      if (!(runtime->page == VS_PAGE_SOFTAP && event->key == VS_KEY_BACK))
        vs_acknowledge(runtime, event->key);
      switch (runtime->page)
        {
          case VS_PAGE_HISTORY:
            if (event->key == VS_KEY_CONFIRM)
              {
                struct vs_voice_request_s request;
                struct vs_history_index_s current;

                if (vs_history_get_index(VS_HISTORY_KIND_SOCIAL,
                                         runtime->index, &current) < 0)
                  {
                    runtime->history_blank = true;
                    runtime->index = 0;
                    runtime->page = VS_PAGE_HISTORY_BLANK;
                    break;
                  }

                /* Voice comes up on a background task at boot; until it is
                 * ready, show a brief hint rather than failing the start. */

                if (!vs_voice_ready())
                  {
                    vs_set_response(runtime, VS_KEY_CONFIRM, "准备中");
                    break;
                  }

                memset(&request, 0, sizeof(request));
                request.ctx = VS_VOICE_CTX_RECORD;
                snprintf(request.record_key, sizeof(request.record_key),
                         "%s", current.record_key);

                runtime->photo_context = false;
                runtime->voice_ending = false;
                runtime->result_text[0] = '\0';
                request.request_id = vs_begin_request(runtime);
                if (vs_voice_start(&request) == 0)
                  {
                    runtime->voice_arming = true;
                    runtime->page = VS_PAGE_VOICE_LISTENING;
                  }
                else
                  vs_cancel_request(runtime);
              }
            else if (event->key == VS_KEY_NEXT)
              {
                unsigned int count =
                  vs_history_count(VS_HISTORY_KIND_SOCIAL);

                /* Past the last record is the volume page, not a wrap: the
                 * ring is blank -> records -> volume -> blank. */

                if (count == 0 || runtime->index + 1u >= count)
                  {
                    runtime->index = count == 0 ? 0 : count - 1u;
                    runtime->page = VS_PAGE_VOLUME;
                    runtime->volume_editing = false;
                  }
                else
                  {
                    runtime->index++;
                  }
              }
            else if (event->key == VS_KEY_BACK)
              {
                unsigned int count =
                  vs_history_count(VS_HISTORY_KIND_SOCIAL);

                if (count == 0 || runtime->index == 0)
                  {
                    runtime->history_blank = true;
                    runtime->index = count == 0 ? 0 : count - 1u;
                    runtime->page = VS_PAGE_HISTORY_BLANK;
                  }
                else
                  {
                    runtime->index--;
                  }
              }
            break;

          case VS_PAGE_HISTORY_BLANK:
            if (event->key == VS_KEY_CONFIRM)
              {
                struct vs_voice_request_s request;

                if (!vs_voice_ready())
                  {
                    vs_set_response(runtime, VS_KEY_CONFIRM, "准备中");
                    break;
                  }

                memset(&request, 0, sizeof(request));
                request.ctx = VS_VOICE_CTX_PHOTO;

                runtime->photo_context = true;
                runtime->voice_ending = false;
                runtime->result_text[0] = '\0';
                request.request_id = vs_begin_request(runtime);
                if (vs_voice_start(&request) == 0)
                  runtime->page = VS_PAGE_PHOTO_CAPTURE;
                else
                  vs_cancel_request(runtime);
              }
            else if (event->key == VS_KEY_NEXT)
              {
                unsigned int count =
                  vs_history_count(VS_HISTORY_KIND_SOCIAL);

                if (count > 0)
                  {
                    runtime->history_blank = false;
                    runtime->index = 0;
                    runtime->page = VS_PAGE_HISTORY;
                  }
                else
                  {
                    /* With no records at all the ring is two pages wide. */

                    runtime->page = VS_PAGE_VOLUME;
                    runtime->volume_editing = false;
                  }
              }
            else if (event->key == VS_KEY_BACK)
              {
                /* Backwards from the first page is the last page, which is now
                 * the volume page rather than the newest record. */

                runtime->page = VS_PAGE_VOLUME;
                runtime->volume_editing = false;
              }
            break;

          case VS_PAGE_VOLUME:
            if (event->key == VS_KEY_CONFIRM)
              {
                /* Power toggles what the two browse keys mean.  Physically
                 * they are the volume rocker, so inside the editing state they
                 * are doing what is printed on them. */

                runtime->volume_editing = !runtime->volume_editing;
                if (!runtime->volume_editing)
                  vs_request_save(runtime, VS_PAGE_VOLUME);
              }
            else if (runtime->volume_editing)
              {
                vs_adjust_volume(runtime, event->key == VS_KEY_NEXT);
              }
            else if (event->key == VS_KEY_NEXT)
              {
                /* Forwards from the last page wraps to the first. */

                runtime->history_blank = true;
                vs_request_save(runtime, VS_PAGE_HISTORY_BLANK);
              }
            else if (event->key == VS_KEY_BACK)
              {
                unsigned int count =
                  vs_history_count(VS_HISTORY_KIND_SOCIAL);

                if (count > 0)
                  {
                    runtime->history_blank = false;
                    runtime->index = count - 1u;
                    vs_request_save(runtime, VS_PAGE_HISTORY);
                  }
                else
                  {
                    runtime->history_blank = true;
                    vs_request_save(runtime, VS_PAGE_HISTORY_BLANK);
                  }
              }
            break;

          case VS_PAGE_SOCIAL_RUNNING:
          case VS_PAGE_SOCIAL_ALERT:
            if (event->key == VS_KEY_CONFIRM)
              {
                runtime->page = VS_PAGE_SOCIAL_PAUSING;
              }
            break;

          case VS_PAGE_SOCIAL_STARTING:
            if (event->key == VS_KEY_BACK)
              {
                vs_cancel_request(runtime);
                runtime->page = runtime->social_entry_return_page;
              }
            break;

          case VS_PAGE_SOCIAL_PAUSED:
            if (event->key == VS_KEY_CONFIRM)
              {
                runtime->page = VS_PAGE_SOCIAL_RESUMING;
              }
            break;

          case VS_PAGE_VOICE_LISTENING:
            if (runtime->voice_ending)
              {
                break;
              }

            if (event->key == VS_KEY_CONFIRM)
              {
                /* Ignored while arming: the microphone is not open yet, so
                 * this would end a round that never recorded anything.  The
                 * page shows a preparing state and offers no 说完 key then,
                 * but a press queued just before the switch can still land
                 * here. */

                if (!runtime->voice_arming)
                  {
                    /* Finish only this utterance.  ASR still produces text
                     * and the multi-turn worker remains active. */
                    (void)vs_voice_stop_recording();
                  }
              }
            else if (event->key == VS_KEY_BACK)
              {
                vs_end_voice_conversation(runtime);
              }
            break;

          case VS_PAGE_VOICE_THINKING:
            if (!runtime->voice_ending && event->key == VS_KEY_BACK)
              {
                vs_end_voice_conversation(runtime);
              }
            break;

          case VS_PAGE_VOICE_SPEAKING:
            if (!runtime->voice_ending &&
                (event->key == VS_KEY_CONFIRM ||
                 event->key == VS_KEY_BACK))
              {
                vs_end_voice_conversation(runtime);
              }
            break;

          case VS_PAGE_SOCIAL_RESULT:
            if (event->key == VS_KEY_CONFIRM || event->key == VS_KEY_BACK)
              {
                vs_cancel_request(runtime);
                runtime->page = VS_PAGE_HISTORY;
              }
            break;

          case VS_PAGE_PHOTO_CAPTURE:
            if (!runtime->voice_ending && event->key == VS_KEY_BACK)
              {
                vs_end_voice_conversation(runtime);
              }
            break;

          case VS_PAGE_SOCIAL_EXITING:
            if (event->key == VS_KEY_BACK)
              {
                runtime->page = runtime->social_exit_return_page;
                runtime->progress = 0;
              }
            break;

          case VS_PAGE_NET_SWITCHING:
            if (event->key == VS_KEY_BACK && !runtime->network_busy)
              runtime->page = VS_PAGE_SOFTAP;
            break;

          case VS_PAGE_SOFTAP:
            break;

          case VS_PAGE_ERROR:
            if (event->key == VS_KEY_CONFIRM && runtime->error_retryable &&
                network != NULL)
              {
                enum vs_net_mode_e mode = runtime->error_target_mode;
                int ret;

                runtime->page = VS_PAGE_NET_SWITCHING;
                runtime->progress = 100;
                ret = vs_start_network_worker(runtime, network, mode);
                if (ret < 0)
                  {
                    runtime->error = ret;
                    snprintf(runtime->error_reason,
                             sizeof(runtime->error_reason),
                             "%s (%d)", vs_errno_reason(ret), ret);
                    runtime->page = VS_PAGE_ERROR;
                  }
              }
            else if (event->key == VS_KEY_BACK)
              runtime->page = runtime->error_return_page;
            break;

          default:
            break;
        }
    }
  else if (event->type == VS_INPUT_LONG)
    {
      if (runtime->page == VS_PAGE_SOCIAL_ENTER && event->key == VS_KEY_CONFIRM)
        {
          runtime->progress = 100;
          runtime->emotion = VS_EMOTION_NONE;
          runtime->emotion_color = 0;
          runtime->alert_text[0] = '\0';
          runtime->result_text[0] = '\0';
          (void)vs_begin_request(runtime);
          runtime->page = VS_PAGE_SOCIAL_STARTING;
        }
      else if ((runtime->page == VS_PAGE_SOCIAL_RUNNING ||
                runtime->page == VS_PAGE_SOCIAL_PAUSED ||
                runtime->page == VS_PAGE_SOCIAL_ALERT ||
                runtime->page == VS_PAGE_SOCIAL_EXITING) &&
               event->key == VS_KEY_BACK)
        {
          runtime->page = VS_PAGE_SOCIAL_FINALIZING;
          runtime->progress = 0;
        }
      else if ((runtime->page == VS_PAGE_SOFTAP ||
                (runtime->page == VS_PAGE_NET_SWITCHING &&
                 runtime->network.mode == VS_NET_AP &&
                 !runtime->network_busy)) &&
               event->key == VS_KEY_BACK && network != NULL)
        {
          vs_switch_network(display, runtime, network);
        }
    }
}

/* vs_voice_open() brings up config store, LLM/ASR/TTS backends and seeds
 * credentials -- seconds of SD-NAND and setup that nothing on the home screen
 * needs.  Running it on this task instead of inline lets the event loop start
 * as soon as history is loaded; readiness is published through
 * vs_voice_ready().
 */

static int vs_voice_open_task(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  vs_voice_open();
  return 0;
}

int vs_app_run(void)
{
  struct vs_display_s *display = NULL;
  struct vs_input_s *input = NULL;
  struct vs_network_s *network = NULL;
  struct vs_runtime_s runtime;
  struct vs_input_event_s event;
  struct vs_app_event_s app_event;
  int ret;

  memset(&event, 0, sizeof(event));
  memset(&runtime, 0, sizeof(runtime));
  runtime.page = VS_PAGE_PREPARING;
  runtime.view = VS_HISTORY_SUMMARY;
  runtime.history_blank = true;
  runtime.social_entry_return_page = VS_PAGE_HISTORY_BLANK;
  runtime.network.mode = VS_NET_STA;
  runtime.network.state = VS_NET_DOWN;

  ret = vs_display_open(&display);
  if (ret < 0)
    return ret;
  vs_render(display, &runtime);
  ret = vs_input_open(&input);
  if (ret < 0)
    goto fail;

  vs_input_queue_reset();
  g_input_worker_state = input;
  ret = task_create("velasight_input", VS_PRIORITY_INPUT, 2048,
                    vs_input_worker, NULL);
  if (ret < 0)
    {
      g_input_worker_state = NULL;
      printf("velasight: input worker unavailable (%d)\n", ret);
      goto fail;
    }

  ret = vs_start_network_worker(&runtime, NULL, VS_NET_STA);
  if (ret < 0)
    {
      runtime.error = ret;
      runtime.error_retryable = false;
      runtime.page = VS_PAGE_HISTORY_BLANK;
      printf("velasight: network worker unavailable (%d), continuing "
             "offline\n", ret);
    }

  /* The preparation page is a display state, not a network gate. */
  runtime.page = VS_PAGE_HISTORY_BLANK;
  runtime.progress = 0;
  runtime.network.wifi_issue = VS_WIFI_ISSUE_DISCONNECTED;
  vs_render(display, &runtime);

  /* Adopt the driver's current gain instead of imposing one.  The DAC comes
   * up at 0 dB, which reads back as 714 rather than 1000, so a hardcoded
   * starting value would show the user a number the hardware is not at.
   */

  {
    unsigned int permille = 0;

    if (vs_audio_volume_get(AGENT_AUDIO_PLAYBACK_DEV, &permille) == 0)
      runtime.volume_level = vs_volume_round(permille / 10u);
    else
      runtime.volume_level = VS_VOLUME_FALLBACK;
  }

  bk7258_nand_seed_agent_config();
  runtime.api_ready = bk7258_ai_config_ready();
  vs_history_open();

  /* Only now is /mnt/sdnand known to be mounted: SD-NAND comes up on a delayed
   * work item and vs_history_open() is what blocks for it.  Reading the volume
   * any earlier -- next to the driver query above, where it would read more
   * naturally -- gets ENOENT on every boot.
   */

  {
    uint8_t stored = 0;
    int stored_ret = vs_settings_load_volume(&stored);

    if (stored_ret == 0)
      {
        vs_apply_volume(&runtime, stored);
        printf("velasight: volume restored to %u%%\n", runtime.volume_level);
      }
    else if (stored_ret != -ENOENT)
      {
        /* A rejected record is worth saying out loud; a missing one is the
         * normal state of a device whose volume has never been changed, and
         * the driver's own level already stands in for it.
         */

        printf("velasight: stored volume unusable (%d), keeping %u%%\n",
               stored_ret, runtime.volume_level);
      }
  }

  /* Off the boot path: the home screen is fully usable without it, and it is
   * the single biggest thing that used to keep the UI frozen on the preparing
   * frame.  vs_voice_ready() gates the 询问/拍照 entry points until it lands.
   */

  ret = task_create("velasight_voiceinit", SCHED_PRIORITY_DEFAULT, 8192,
                    vs_voice_open_task, NULL);
  if (ret < 0)
    {
      printf("velasight: voice init task failed (%d), opening inline\n", ret);
      vs_voice_open();
    }

  vs_render(display, &runtime);
  for (;;)
    {
      if (runtime.response_until_ms != 0)
        {
          uint32_t now = vs_app_now_ms();

          if ((int32_t)(runtime.response_until_ms - now) <= 0)
            {
              vs_expire_response(&runtime);
              vs_render(display, &runtime);
            }
        }

      /* Input feedback never blocks its action.  SHORT updates the business
       * state immediately and carries its visual overlay onto the resulting
       * page. */
      {
        unsigned int input_count = 0;

        while (input_count < VS_INPUT_EVENTS_PER_FRAME &&
               vs_input_queue_pop(&event))
          {
            vs_handle_event(display, &runtime, network, &event);
            input_count++;
          }

        if (input_count != 0)
          {
            vs_render(display, &runtime);
          }
      }

      /* Each app event may synchronously push one or both full panels.  Handle
       * one per pass so a burst cannot keep an already queued press waiting. */
      if (vs_app_pop_event(&app_event))
        {
          if (app_event.type == VS_APP_EVENT_NETWORK_READY ||
              app_event.type == VS_APP_EVENT_NETWORK_FAILED)
            network = vs_take_network_result();
          vs_handle_app_event(&runtime, network, &app_event);
          vs_render(display, &runtime);
        }

      if (network != NULL && !runtime.network_busy)
        {
          ret = vs_network_process_events(network);
          if (ret != 0)
            {
              (void)vs_network_get_status(network, &runtime.network);
              runtime.api_ready = bk7258_ai_config_ready();
              if (ret < 0)
                {
                  runtime.error_target_mode = runtime.network.mode;
                  vs_set_error(
                      &runtime, ret,
                      runtime.network.mode == VS_NET_AP ? VS_PAGE_SOFTAP :
                      runtime.history_blank ? VS_PAGE_HISTORY_BLANK :
                                              VS_PAGE_HISTORY,
                      false);
                }
              else
                {
                  vs_update_wifi_retry(&runtime);
                }
              vs_render(display, &runtime);
            }
        }

      if (!runtime.network_busy &&
          runtime.wifi_retry_at_ms != 0 &&
          (int32_t)(vs_app_now_ms() - runtime.wifi_retry_at_ms) >= 0)
        {
          runtime.wifi_retry_at_ms = 0;
          printf("velasight: retrying STA connection\n");
          ret = vs_start_network_worker(&runtime, network, VS_NET_STA);
          if (ret < 0)
            {
              runtime.wifi_retry_at_ms = vs_app_now_ms() + VS_WIFI_RETRY_MS;
            }
          vs_render(display, &runtime);
        }

      vs_display_tick(display);

      /* After the tick, so the saving page is on the glass before the write
       * blocks.  Any future store write from the UI thread belongs here too:
       * request it with vs_request_save() and extend the flush.
       */

      if (runtime.pending_save)
        {
          /* Put the saving page on the glass before blocking on it. */

          vs_display_flush(display);
          vs_flush_pending_save(&runtime);
          vs_render(display, &runtime);
          vs_display_tick(display);
        }

      usleep(CONFIG_VS_INPUT_POLL_MS * 1000);
    }

fail:
  vs_voice_close();
  vs_network_close(network);
  vs_history_close();
  vs_input_close(input);
  vs_display_close(display);
  return ret;
}
