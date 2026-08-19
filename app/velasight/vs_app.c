#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <velaclaw/client.h>

#include "include/vs_app.h"
#include "include/vs_display.h"
#include "include/vs_input.h"
#include "include/vs_network.h"

static const struct vs_history_item_s g_history[] =
{
  {"08/18 09:20", "MORNING", "CALM AND CLEAR", 55, 30, 15, false},
  {"08/17 16:40", "REVIEW", "NEEDS FOLLOW UP", 30, 20, 50, true},
  {"08/16 11:05", "DAILY NOTE", "GOOD PROGRESS", 65, 25, 10, false}
};

struct vs_runtime_s
{
  enum vs_page_e page;
  enum vs_history_view_e view;
  uint8_t index;
  uint8_t progress;
  bool social_exit_from_paused;
  bool error_back_progress;
  struct vs_net_status_s network;
  int error;
  velaclaw_client_t *agent;
};

static void vs_snapshot(struct vs_runtime_s *runtime,
                        struct vs_ui_snapshot_s *snapshot)
{
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->page = runtime->page;
  snapshot->history_view = runtime->view;
  snapshot->network = runtime->network;
  snapshot->history = &g_history[runtime->index];
  snapshot->history_index = runtime->index;
  snapshot->history_count = sizeof(g_history) / sizeof(g_history[0]);
  snapshot->progress = runtime->progress;
  snapshot->network_target_ap = runtime->network.mode == VS_NET_STA;
  snapshot->show_progress = runtime->error_back_progress;

  switch (runtime->page)
    {
      case VS_PAGE_HISTORY:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "HISTORY");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "SELECT");
        break;
      case VS_PAGE_SOCIAL_ENTER:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "SOCIAL");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "HOLD");
        break;
      case VS_PAGE_SOCIAL_RUNNING:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "SOCIAL");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "RUN");
        break;
      case VS_PAGE_SOCIAL_PAUSED:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "PAUSED");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "PRESS");
        break;
      case VS_PAGE_SOCIAL_EXITING:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "SOCIAL");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "EXIT");
        break;
      case VS_PAGE_VOICE_LISTENING:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "LISTEN");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "POWER");
        break;
      case VS_PAGE_VOICE_THINKING:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "THINK");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "WAIT");
        break;
      case VS_PAGE_VOICE_SPEAKING:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "SPEAK");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "ANSWER");
        break;
      case VS_PAGE_NET_SWITCHING:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "NETWORK");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "SWITCH");
        break;
      case VS_PAGE_SOFTAP:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "SOFTAP");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "READY");
        break;
      default:
        snprintf(snapshot->primary, sizeof(snapshot->primary), "ERROR");
        snprintf(snapshot->secondary, sizeof(snapshot->secondary), "RETRY");
        break;
    }
}

static void vs_render(struct vs_display_s *display,
                      struct vs_runtime_s *runtime)
{
  struct vs_ui_snapshot_s snapshot;

  vs_snapshot(runtime, &snapshot);
  (void)vs_display_render(display, &snapshot);
}

static void vs_switch_network(struct vs_display_s *display,
                              struct vs_runtime_s *runtime,
                              struct vs_network_s *network)
{
  enum vs_net_mode_e mode = runtime->network.mode == VS_NET_STA ?
                            VS_NET_AP : VS_NET_STA;

  runtime->error_back_progress = false;
  runtime->progress = 0;
  runtime->page = VS_PAGE_NET_SWITCHING;
  runtime->progress = 100;
  vs_render(display, runtime);

  if (vs_network_request_mode(network, mode) < 0)
    {
      (void)vs_network_get_status(network, &runtime->network);
      runtime->error = runtime->network.error;
      runtime->page = VS_PAGE_ERROR;
    }
  else
    {
      (void)vs_network_get_status(network, &runtime->network);
      runtime->page = runtime->network.mode == VS_NET_AP ?
                      VS_PAGE_SOFTAP : VS_PAGE_HISTORY;
    }
}

static void vs_handle_event(struct vs_display_s *display,
                            struct vs_runtime_s *runtime,
                            struct vs_network_s *network,
                            const struct vs_input_event_s *event)
{
  if (event->type == VS_INPUT_COMBO_PROGRESS &&
      (runtime->page == VS_PAGE_HISTORY ||
       runtime->page == VS_PAGE_NET_SWITCHING))
    {
      runtime->page = VS_PAGE_NET_SWITCHING;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_COMBO_CANCEL &&
      runtime->page == VS_PAGE_NET_SWITCHING &&
      runtime->network.mode == VS_NET_STA)
    {
      runtime->page = VS_PAGE_HISTORY;
      runtime->progress = 0;
      return;
    }

  if (event->type == VS_INPUT_PROGRESS && event->key == VS_KEY_CONFIRM &&
      (runtime->page == VS_PAGE_HISTORY ||
       runtime->page == VS_PAGE_SOCIAL_ENTER))
    {
      runtime->page = VS_PAGE_SOCIAL_ENTER;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_PROGRESS && event->key == VS_KEY_BACK &&
      (runtime->page == VS_PAGE_SOCIAL_RUNNING ||
       runtime->page == VS_PAGE_SOCIAL_PAUSED ||
       runtime->page == VS_PAGE_SOCIAL_EXITING))
    {
      if (runtime->page != VS_PAGE_SOCIAL_EXITING)
        {
          runtime->social_exit_from_paused =
            runtime->page == VS_PAGE_SOCIAL_PAUSED;
        }

      runtime->page = VS_PAGE_SOCIAL_EXITING;
      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_PROGRESS && event->key == VS_KEY_BACK &&
       (runtime->page == VS_PAGE_SOFTAP ||
        (runtime->page == VS_PAGE_NET_SWITCHING &&
         runtime->network.mode == VS_NET_AP) ||
        runtime->page == VS_PAGE_ERROR))
    {
      if (runtime->page == VS_PAGE_ERROR)
        {
          runtime->error_back_progress = true;
        }
      else
        {
          runtime->page = VS_PAGE_NET_SWITCHING;
        }

      runtime->progress = event->progress;
      return;
    }

  if (event->type == VS_INPUT_CANCEL && event->key == VS_KEY_CONFIRM &&
      runtime->page == VS_PAGE_SOCIAL_ENTER)
    {
      runtime->page = VS_PAGE_HISTORY;
      runtime->progress = 0;
      return;
    }

  if (event->type == VS_INPUT_CANCEL && event->key == VS_KEY_BACK &&
      (runtime->page == VS_PAGE_NET_SWITCHING ||
       runtime->page == VS_PAGE_ERROR))
    {
      if (runtime->page == VS_PAGE_NET_SWITCHING)
        {
          runtime->page = VS_PAGE_SOFTAP;
        }

      runtime->progress = 0;
      runtime->error_back_progress = false;
      return;
    }

  if (event->type == VS_INPUT_NET_TOGGLE &&
      (runtime->page == VS_PAGE_HISTORY ||
       runtime->page == VS_PAGE_NET_SWITCHING) &&
      network != NULL)
    {
      vs_switch_network(display, runtime, network);
      return;
    }

  if (event->type == VS_INPUT_SHORT)
    {
      if (runtime->page == VS_PAGE_HISTORY)
        {
          if (event->key == VS_KEY_CONFIRM)
            {
              /* Keep a visible assistant entry page until the voice backend
               * is connected in the next implementation phase. */
              runtime->page = VS_PAGE_VOICE_LISTENING;
            }
          else if (event->key == VS_KEY_BACK)
            runtime->page = VS_PAGE_HISTORY;
          else if (event->key == VS_KEY_NEXT)
            {
              runtime->index = (runtime->index + 1) %
                               (sizeof(g_history) / sizeof(g_history[0]));
              runtime->view = VS_HISTORY_SUMMARY;
            }
        }
      else if (runtime->page == VS_PAGE_SOCIAL_RUNNING &&
               event->key == VS_KEY_CONFIRM)
        {
          runtime->page = VS_PAGE_SOCIAL_PAUSED;
        }
      else if (runtime->page == VS_PAGE_SOCIAL_PAUSED &&
               event->key == VS_KEY_CONFIRM)
        {
          runtime->page = VS_PAGE_SOCIAL_RUNNING;
        }
      else if (runtime->page == VS_PAGE_VOICE_LISTENING &&
               event->key == VS_KEY_CONFIRM)
        {
          runtime->page = VS_PAGE_VOICE_THINKING;
        }
      else if (runtime->page == VS_PAGE_SOCIAL_ENTER &&
               event->key == VS_KEY_CONFIRM)
        {
          runtime->page = VS_PAGE_VOICE_LISTENING;
          runtime->progress = 0;
        }
      else if (runtime->page == VS_PAGE_SOCIAL_EXITING &&
               event->key == VS_KEY_BACK)
        {
          runtime->page = runtime->social_exit_from_paused ?
                          VS_PAGE_SOCIAL_PAUSED : VS_PAGE_SOCIAL_RUNNING;
          runtime->progress = 0;
        }
      else if (runtime->page == VS_PAGE_NET_SWITCHING &&
               runtime->network.mode == VS_NET_AP &&
               event->key == VS_KEY_BACK)
        {
          runtime->page = VS_PAGE_SOFTAP;
          runtime->progress = 0;
        }
      else if (runtime->page == VS_PAGE_VOICE_SPEAKING)
        {
          runtime->page = VS_PAGE_HISTORY;
        }
      else if (event->key == VS_KEY_BACK &&
               runtime->page != VS_PAGE_SOCIAL_RUNNING &&
               runtime->page != VS_PAGE_SOCIAL_PAUSED &&
               runtime->page != VS_PAGE_SOFTAP &&
               runtime->page != VS_PAGE_NET_SWITCHING)
        {
          runtime->page = VS_PAGE_HISTORY;
          runtime->progress = 0;
          runtime->error_back_progress = false;
        }
    }
  else if (event->type == VS_INPUT_LONG)
    {
      if ((runtime->page == VS_PAGE_HISTORY ||
           runtime->page == VS_PAGE_SOCIAL_ENTER) &&
          event->key == VS_KEY_CONFIRM)
        {
          runtime->progress = 100;
          runtime->page = VS_PAGE_SOCIAL_RUNNING;
        }
      else if ((runtime->page == VS_PAGE_SOCIAL_RUNNING ||
                runtime->page == VS_PAGE_SOCIAL_PAUSED ||
                runtime->page == VS_PAGE_SOCIAL_EXITING) &&
               event->key == VS_KEY_BACK)
        {
          runtime->page = VS_PAGE_HISTORY;
        }
      else if (runtime->page == VS_PAGE_SOFTAP &&
               event->key == VS_KEY_CONFIRM)
        {
          /* Credential reset belongs to the future SoftAP service. */
          runtime->page = VS_PAGE_SOFTAP;
        }
      else if ((runtime->page == VS_PAGE_SOFTAP ||
                (runtime->page == VS_PAGE_NET_SWITCHING &&
                 runtime->network.mode == VS_NET_AP)) &&
               event->key == VS_KEY_BACK && network != NULL)
        {
          vs_switch_network(display, runtime, network);
        }
      else if (runtime->page == VS_PAGE_ERROR && event->key == VS_KEY_BACK)
        {
          runtime->page = VS_PAGE_HISTORY;
          runtime->progress = 0;
          runtime->error_back_progress = false;
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
  int ret;

  memset(&runtime, 0, sizeof(runtime));
  runtime.page = VS_PAGE_HISTORY;
  runtime.view = VS_HISTORY_SUMMARY;
  runtime.network.mode = VS_NET_STA;
  runtime.network.state = VS_NET_DOWN;

  ret = vs_display_open(&display);
  if (ret < 0)
    return ret;
  ret = vs_input_open(&input);
  if (ret < 0)
    goto fail;
  ret = vs_network_open(&network);
  if (ret < 0)
    {
      runtime.network.state = VS_NET_ERROR;
      runtime.network.error = ret;
    }

  runtime.agent = velaclaw_client_open("velasight");
  if (runtime.agent == NULL)
    {
      printf("velasight: ai_agent client unavailable\n");
    }

  vs_render(display, &runtime);
  for (;;)
    {
      if (vs_input_poll(input, &event) > 0)
        {
          enum vs_page_e previous = runtime.page;

          vs_handle_event(display, &runtime, network, &event);
          if (runtime.page != previous || event.type == VS_INPUT_LONG ||
              event.type == VS_INPUT_CANCEL)
            {
              printf("velasight: input type=%u key=%u progress=%u "
                     "page=%u->%u\n", event.type, event.key,
                     event.progress, previous, runtime.page);
            }

          vs_render(display, &runtime);
        }
      vs_display_tick(display);
      usleep(CONFIG_VS_INPUT_POLL_MS * 1000);
    }

fail:
  vs_network_close(network);
  vs_input_close(input);
  vs_display_close(display);
  return ret;
}
