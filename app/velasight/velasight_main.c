#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/vs_app.h"
#include "include/vs_cloud.h"
#include "include/vs_history.h"
#include "include/vs_social.h"

/****************************************************************************
 * Name: velasight_event_name
 *
 * Description:
 *   The events the social session emits, for the headless subcommand's log.
 *   Only the social ones are named; anything else is printed by number,
 *   because nothing else can arrive on that path.
 *
 ****************************************************************************/

static const char *velasight_event_name(enum vs_app_event_e type)
{
  switch (type)
    {
      case VS_APP_EVENT_SOCIAL_STARTED:         return "STARTED";
      case VS_APP_EVENT_SOCIAL_START_FAILED:    return "START_FAILED";
      case VS_APP_EVENT_SOCIAL_ALERT:           return "ALERT";
      case VS_APP_EVENT_SOCIAL_ALERT_CLEARED:   return "ALERT_CLEARED";
      case VS_APP_EVENT_SOCIAL_PAUSED:          return "PAUSED";
      case VS_APP_EVENT_SOCIAL_RESUMED:         return "RESUMED";
      case VS_APP_EVENT_SOCIAL_PAUSE_FAILED:    return "PAUSE_FAILED";
      case VS_APP_EVENT_SOCIAL_RESULT:          return "RESULT";
      case VS_APP_EVENT_SOCIAL_FINALIZE_FAILED: return "FINALIZE_FAILED";
      default:                                  return NULL;
    }
}

/* Drain and print whatever the session has emitted.
 *
 * Draining is not cosmetic here.  A worker retries vs_app_post_event()
 * indefinitely when the queue is full, so a command that started a session and
 * never drained would wedge the session thread on the eighth event.
 *
 * Returns the terminal event's type, or -1 when none has arrived yet.
 */

static int velasight_drain_events(void)
{
  struct vs_app_event_s event;
  int terminal = -1;

  while (vs_app_take_event(&event) == 0)
    {
      const char *name = velasight_event_name(event.type);

      if (name == NULL)
        {
          printf("velasight: event %d\n", (int)event.type);
          continue;
        }

      printf("velasight: %s", name);
      if (event.error != 0)
        {
          printf(" err %d", event.error);
        }

      if (event.text[0] != '\0')
        {
          printf(" \"%s\"", event.text);
        }

      printf("\n");

      if (event.type == VS_APP_EVENT_SOCIAL_START_FAILED ||
          event.type == VS_APP_EVENT_SOCIAL_RESULT ||
          event.type == VS_APP_EVENT_SOCIAL_FINALIZE_FAILED)
        {
          terminal = (int)event.type;
        }
    }

  return terminal;
}

/****************************************************************************
 * Name: velasight_social_probe
 *
 * Description:
 *   Run one complete social session from the console: start, sample for a
 *   while, finalize, and print every event on the way.
 *
 *   This exists because the product path into a session is a long-press on a
 *   history page, which needs both displays, both keys and a person.  None of
 *   those are available in a bring-up rig, and the session is the part most
 *   worth exercising against a real cloud -- so without this, the only way to
 *   test the orchestration would be to test it by hand.
 *
 *   Unlike "cloudprobe", this uses the real camera and the real microphone.
 *   It is the whole pipeline, not a transport check.
 *
 ****************************************************************************/

static int velasight_social_probe(unsigned int seconds)
{
  uint32_t request_id = 1;
  unsigned int elapsed = 0;
  int terminal;
  int ret;

  /* History has to be open before the session can persist its minutes, and it
   * is also what blocks for the SD-NAND mount.
   */

  vs_history_open();

  ret = vs_cloud_init();
  if (ret < 0 && ret != -ENODATA)
    {
      printf("velasight: cloud init failed (%d)\n", ret);
      return 1;
    }

  /* vs_cloud_init() only installs the compiled-in default now -- see its own
   * comment for why it no longer touches SD-NAND.  This command runs from an
   * interactive nsh prompt, long after boot and with no heartbeat deadline to
   * respect, so reading the provisioning record here is safe; it is what
   * picks up a host actually configured through the setup page instead of
   * silently probing the factory default every time.
   */

  ret = vs_cloud_reload_endpoint();
  if (ret < 0 && ret != -EBUSY)
    {
      printf("velasight: provisioning record not read (%d), using the "
             "factory default endpoint\n", ret);
    }

  if (!vs_cloud_configured())
    {
      printf("velasight: no cloud endpoint (%d)\n", ret);
      return 1;
    }

  {
    const char *host = "";
    const char *base = "";
    uint16_t port = 0;
    bool tls = false;

    vs_cloud_endpoint(&host, &port, &base, &tls);
    printf("velasight: social probe against %s://%s:%u%s/contest/v1, "
           "%u s, device %s\n", tls ? "https" : "http", host,
           (unsigned int)port, base, seconds, vs_cloud_device_id());
  }

  ret = vs_social_start(request_id);
  if (ret < 0)
    {
      printf("velasight: session refused: %d\n", ret);
      return 1;
    }

  /* Sample for the requested time, draining events as they arrive so the
   * session thread is never blocked on the queue.
   */

  while (elapsed < seconds * 1000u)
    {
      terminal = velasight_drain_events();
      if (terminal == VS_APP_EVENT_SOCIAL_START_FAILED)
        {
          vs_social_close();
          return 1;
        }

      usleep(200000);
      elapsed += 200;
    }

  printf("velasight: asking for the minutes\n");
  ret = vs_social_finalize(request_id);
  if (ret < 0)
    {
      printf("velasight: finalize refused: %d\n", ret);
      vs_social_close();
      return 1;
    }

  /* The finalize timeout bounds this, so the wait cannot be unbounded even if
   * the cloud never answers.  One extra interval of slack so the terminal
   * event has somewhere to land after that timeout fires.
   */

  elapsed = 0;
  terminal = -1;
  while (elapsed < CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS + 5000u)
    {
      terminal = velasight_drain_events();
      if (terminal == VS_APP_EVENT_SOCIAL_RESULT ||
          terminal == VS_APP_EVENT_SOCIAL_FINALIZE_FAILED)
        {
          break;
        }

      if (!vs_social_active())
        {
          /* The session thread is gone.  One more drain, in case the terminal
           * event landed between the check above and this one, then stop
           * waiting for something that has nothing left to send it.
           */

          terminal = velasight_drain_events();
          break;
        }

      usleep(200000);
      elapsed += 200;
    }

  vs_social_close();
  (void)velasight_drain_events();

  if (terminal == VS_APP_EVENT_SOCIAL_RESULT)
    {
      printf("velasight: social probe completed\n");
      return 0;
    }

  printf("velasight: social probe did not produce minutes\n");
  return 1;
}

int main(int argc, FAR char *argv[])
{
  /* "velasight cloudprobe" drives one complete session against the
   * configured /contest/v1 endpoint and prints what happened, without
   * opening the displays or taking the input keys.
   *
   * It is a subcommand rather than its own application because the cloud
   * client is compiled into this program: a second PROGNAME would mean a
   * second link of the same objects. It exists because the cloud client has
   * to be verifiable on its own -- separating a protocol problem from an
   * orchestration one is much easier when each can be run by itself.
   */

  if (argc > 1 && strcmp(argv[1], "cloudprobe") == 0)
    {
      return vs_cloud_probe() < 0 ? 1 : 0;
    }

  /* "velasight social [seconds]" runs the real session -- camera, microphone,
   * uploads, polling, minutes -- without the displays or the keys.  The
   * default duration is long enough to cover several audio chunks and a few
   * poll rounds, which is what makes the emotion path observable at all.
   */

  if (argc > 1 && strcmp(argv[1], "social") == 0)
    {
      unsigned int seconds = 15;

      if (argc > 2)
        {
          int given = atoi(argv[2]);

          if (given < 1 || given > 600)
            {
              printf("velasight: social duration must be 1..600 seconds\n");
              return 1;
            }

          seconds = (unsigned int)given;
        }

      return velasight_social_probe(seconds);
    }

  if (argc > 1)
    {
      printf("Usage: velasight [cloudprobe | social [seconds]]\n"
             "  (no argument)      run the product UI on both displays\n"
             "  cloudprobe         one cloud round trip, synthetic payload\n"
             "  social [seconds]   one real session, camera and microphone\n");
      return 1;
    }

  printf("velasight: taking ownership of both displays\n");
  return vs_app_run();
}

static int velasight_task(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  return vs_app_run();
}

int velasight_autostart(void)
{
  int pid;

  pid = task_create("velasight", SCHED_PRIORITY_DEFAULT, 8192,
                    velasight_task, NULL);
  return pid < 0 ? pid : 0;
}
