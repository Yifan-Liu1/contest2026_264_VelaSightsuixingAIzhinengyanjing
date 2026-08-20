#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <velaclaw/client.h>
#include <arch/board/board.h>

#include "include/vs_app.h"
#include "include/vs_display.h"
#include "include/vs_input.h"
#include "include/vs_network.h"

static const struct vs_history_item_s g_history[] =
{
  {"08/18 09:20", "上午交流", "整体较平稳\n后段略有疑惑", 55, 30, 15, false},
  {"08/17 16:40", "项目讨论", "对方需要进一步确认", 30, 20, 50, true},
  {"08/16 11:05", "日常记录", "交流进展顺利", 65, 25, 10, false}
};

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

struct vs_runtime_s
{
  enum vs_page_e page;
  enum vs_history_view_e view;
  uint8_t index;
  uint8_t progress;
  bool history_blank;
  bool photo_context;
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
  uint32_t next_request_id;
  uint32_t active_request_id;
  uint8_t response_ticks;
  enum vs_key_e response_key;
  struct vs_net_status_s network;
  char alert_text[VS_TEXT_LONG];
  char result_text[VS_TEXT_LONG];
  velaclaw_client_t *agent;
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
  return 0;
}

static unsigned int vs_history_count(void)
{
  return sizeof(g_history) / sizeof(g_history[0]);
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

static void vs_snapshot(struct vs_runtime_s *runtime,
                        struct vs_ui_snapshot_s *snapshot)
{
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->page = runtime->page;
  snapshot->history_view = runtime->view;
  snapshot->network = runtime->network;
  snapshot->history = runtime->history_blank ? NULL : &g_history[runtime->index];
  snapshot->history_index = runtime->index;
  snapshot->history_count = vs_history_count();
  snapshot->history_is_blank = runtime->history_blank;
  snapshot->photo_context = runtime->photo_context;
  snapshot->progress = runtime->progress;
  snapshot->emotion = runtime->emotion;
  snapshot->response_active = runtime->response_ticks != 0;
  snapshot->response_key = runtime->response_key;
  snapshot->error_retryable = runtime->error_retryable;
  snapshot->wifi_ready = runtime->network.state == VS_NET_STA_READY ||
                         runtime->network.state == VS_NET_AP_READY;
  snapshot->battery_present = false;
#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  snapshot->api_ready = bk7258_ai_config_ready();
#endif
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
                 "%s", g_history[runtime->index].title);
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "%s", g_history[runtime->index].summary);
        snprintf(snapshot->content_meta, sizeof(snapshot->content_meta),
                 "%s", g_history[runtime->index].date);
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "历史");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "%02u/%02u", runtime->index + 1, vs_history_count());
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "已保存");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "询问");
        vs_key_set(snapshot, VS_KEY_BACK, "返回");
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
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "准备好");
        vs_key_set(snapshot, VS_KEY_BACK, "返回");
        vs_key_set(snapshot, VS_KEY_NEXT, "下一条");
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
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "请说话");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "说完自动结束");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "聆听");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value),
                 "%s", runtime->photo_context ? "照片问题" : "正在听");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "录音中");
        vs_key_set(snapshot, VS_KEY_BACK, "取消");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "结束");
        break;

      case VS_PAGE_VOICE_THINKING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在思考");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "很快回答你");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "思考");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "请稍等");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "处理中");
        vs_key_set(snapshot, VS_KEY_BACK, "取消");
        break;

      case VS_PAGE_VOICE_SPEAKING:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "回答建议");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body), "%s",
                 runtime->result_text[0] != '\0' ? runtime->result_text :
                 "先听完对方\n再回应");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "回答");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "结果就绪");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "播报中");
        vs_key_set(snapshot, VS_KEY_BACK, "取消");
        vs_key_set(snapshot, VS_KEY_CONFIRM, "完成");
        break;

      case VS_PAGE_PHOTO_CAPTURE:
        snprintf(snapshot->content_title, sizeof(snapshot->content_title),
                 "正在拍照");
        snprintf(snapshot->content_body, sizeof(snapshot->content_body),
                 "请看向目标");
        snprintf(snapshot->status_title, sizeof(snapshot->status_title), "AI");
        snprintf(snapshot->status_value, sizeof(snapshot->status_value), "拍摄中");
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "请稍等");
        vs_key_set(snapshot, VS_KEY_BACK, "取消");
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
        snprintf(snapshot->status_meta, sizeof(snapshot->status_meta), "待连接");
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
  if (runtime->response_ticks != 0)
    snapshot->softkey[runtime->response_key].highlighted = true;
}

static void vs_render(struct vs_display_s *display,
                      struct vs_runtime_s *runtime)
{
  struct vs_ui_snapshot_s snapshot;

  vs_snapshot(runtime, &snapshot);
  (void)vs_display_render(display, &snapshot);
}

static void vs_set_response(struct vs_runtime_s *runtime, enum vs_key_e key)
{
  runtime->response_key = key;
  runtime->response_ticks = 1;
}

static void vs_acknowledge(struct vs_display_s *display,
                           struct vs_runtime_s *runtime,
                           enum vs_key_e key)
{
  struct vs_ui_snapshot_s snapshot;

  vs_snapshot(runtime, &snapshot);
  if (!snapshot.softkey[key].visible)
    return;

  vs_set_response(runtime, key);
  vs_render(display, runtime);
  usleep(120000);
  runtime->response_ticks = 0;
  vs_render(display, runtime);
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
        if (runtime->page == VS_PAGE_PHOTO_CAPTURE)
          {
            if (event->text[0] == '\0')
              {
                vs_cancel_request(runtime);
                vs_set_error_reason(runtime, -EBADMSG, VS_PAGE_HISTORY_BLANK,
                                    false, "拍照未返回照片");
              }
            else
              runtime->page = VS_PAGE_VOICE_LISTENING;
          }
        break;

      case VS_APP_EVENT_PHOTO_FAILED:
        if (runtime->page == VS_PAGE_PHOTO_CAPTURE)
          {
            vs_cancel_request(runtime);
            vs_set_error_reason(runtime, event->error, VS_PAGE_HISTORY_BLANK,
                                false, "拍照失败");
          }
        break;

      case VS_APP_EVENT_VOICE_REPLY:
        if (runtime->page == VS_PAGE_VOICE_THINKING)
          {
            if (event->text[0] == '\0')
              {
                vs_cancel_request(runtime);
                vs_set_error_reason(runtime, -EBADMSG,
                                    runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                             VS_PAGE_HISTORY, false,
                                    "AI未返回回答");
              }
            else
              {
                snprintf(runtime->result_text, sizeof(runtime->result_text),
                         "%s", event->text);
                runtime->page = VS_PAGE_VOICE_SPEAKING;
              }
          }
        break;

      case VS_APP_EVENT_VOICE_FAILED:
        if (runtime->page == VS_PAGE_VOICE_LISTENING ||
            runtime->page == VS_PAGE_VOICE_THINKING)
          {
            vs_cancel_request(runtime);
            vs_set_error_reason(runtime, event->error,
                                runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                         VS_PAGE_HISTORY, false,
                                "语音处理失败");
          }
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
        runtime->error = 0;
        runtime->error_retryable = false;
        runtime->page = runtime->network.mode == VS_NET_AP ? VS_PAGE_SOFTAP :
                        runtime->history_blank ? VS_PAGE_HISTORY_BLANK :
                                                 VS_PAGE_HISTORY;
        runtime->progress = 0;
        break;

      case VS_APP_EVENT_NETWORK_FAILED:
        runtime->network_busy = false;
        if (network != NULL)
          (void)vs_network_get_status(network, &runtime->network);
        runtime->error_target_mode = runtime->network_target_mode;
        if (runtime->network_target_mode == VS_NET_STA)
          {
            /* Failing to join the configured Wi-Fi is an offline state, not a
             * product error.  Keep the photo home usable and wait for the next
             * explicit network attempt instead of showing an error page. */

            runtime->error = event->error;
            runtime->error_retryable = false;
            runtime->error_reason[0] = '\0';
            runtime->page = runtime->history_blank ? VS_PAGE_HISTORY_BLANK :
                                                     VS_PAGE_HISTORY;
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

      if (!(runtime->page == VS_PAGE_SOFTAP && event->key == VS_KEY_BACK))
        vs_acknowledge(display, runtime, event->key);
      switch (runtime->page)
        {
          case VS_PAGE_HISTORY:
            if (event->key == VS_KEY_CONFIRM)
              {
                runtime->photo_context = false;
                (void)vs_begin_request(runtime);
                runtime->page = VS_PAGE_VOICE_LISTENING;
              }
            else if (event->key == VS_KEY_NEXT)
              {
                runtime->index++;
                runtime->history_blank = runtime->index >= vs_history_count();
                if (runtime->history_blank)
                  {
                    runtime->index = vs_history_count() - 1;
                    runtime->page = VS_PAGE_HISTORY_BLANK;
                  }
              }
            else if (event->key == VS_KEY_BACK)
              runtime->page = VS_PAGE_HISTORY;
            break;

          case VS_PAGE_HISTORY_BLANK:
            if (event->key == VS_KEY_CONFIRM)
              {
                runtime->photo_context = true;
                (void)vs_begin_request(runtime);
                runtime->page = VS_PAGE_PHOTO_CAPTURE;
              }
            else if (event->key == VS_KEY_NEXT)
              {
                runtime->history_blank = false;
                runtime->index = 0;
                runtime->page = VS_PAGE_HISTORY;
              }
            else if (event->key == VS_KEY_BACK)
              {
                runtime->history_blank = false;
                runtime->index = vs_history_count() - 1;
                runtime->page = VS_PAGE_HISTORY;
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
            if (event->key == VS_KEY_CONFIRM)
              {
                runtime->page = VS_PAGE_VOICE_THINKING;
              }
            else if (event->key == VS_KEY_BACK)
              {
                vs_cancel_request(runtime);
                runtime->page = runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                        VS_PAGE_HISTORY;
              }
            break;

          case VS_PAGE_VOICE_THINKING:
            if (event->key == VS_KEY_BACK)
              {
                vs_cancel_request(runtime);
                runtime->page = runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                        VS_PAGE_HISTORY;
              }
            break;

          case VS_PAGE_VOICE_SPEAKING:
            if (event->key == VS_KEY_CONFIRM || event->key == VS_KEY_BACK)
              {
                vs_cancel_request(runtime);
                runtime->page = runtime->photo_context ? VS_PAGE_HISTORY_BLANK :
                                                        VS_PAGE_HISTORY;
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
            if (event->key == VS_KEY_BACK)
              {
                vs_cancel_request(runtime);
                runtime->page = VS_PAGE_HISTORY_BLANK;
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

int vs_app_run(void)
{
  struct vs_display_s *display = NULL;
  struct vs_input_s *input = NULL;
  struct vs_network_s *network = NULL;
  struct vs_runtime_s runtime;
  struct vs_input_event_s event;
  struct vs_app_event_s app_event;
  int ret;

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

  ret = vs_start_network_worker(&runtime, NULL, VS_NET_STA);
  if (ret < 0)
    {
      runtime.error = ret;
      runtime.error_retryable = false;
      runtime.page = VS_PAGE_HISTORY_BLANK;
      printf("velasight: network worker unavailable (%d), continuing "
             "offline\n", ret);
    }

  bk7258_nand_seed_agent_config();
  runtime.agent = velaclaw_client_open("velasight");
  if (runtime.agent == NULL)
    printf("velasight: ai_agent client unavailable\n");

  vs_render(display, &runtime);
  for (;;)
    {
      while (vs_app_pop_event(&app_event))
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
              if (ret < 0)
                {
                  runtime.error_target_mode = VS_NET_STA;
                  vs_set_error(&runtime, ret, VS_PAGE_SOFTAP, false);
                }
              else
                {
                  runtime.page = VS_PAGE_NET_SWITCHING;
                  runtime.progress = 100;
                  ret = vs_start_network_worker(&runtime, network, VS_NET_STA);
                  if (ret < 0)
                    {
                      runtime.error_target_mode = VS_NET_STA;
                      vs_set_error(&runtime, ret, VS_PAGE_SOFTAP, true);
                    }
                }
              vs_render(display, &runtime);
            }
        }

      if (vs_input_poll(input, &event) > 0)
        {
          enum vs_page_e previous = runtime.page;
          vs_handle_event(display, &runtime, network, &event);
          if (runtime.page != previous || event.type == VS_INPUT_LONG ||
              event.type == VS_INPUT_CANCEL)
            printf("velasight: input type=%u key=%u progress=%u page=%u->%u\n",
                   event.type, event.key, event.progress, previous, runtime.page);
          vs_render(display, &runtime);
        }
      vs_display_tick(display);
      usleep(CONFIG_VS_INPUT_POLL_MS * 1000);
    }

fail:
  if (runtime.agent != NULL)
    velaclaw_client_close(runtime.agent);
  vs_network_close(network);
  vs_input_close(input);
  vs_display_close(display);
  return ret;
}
