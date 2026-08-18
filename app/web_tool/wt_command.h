/****************************************************************************
 * app/web_tool/wt_command.h
 *
 * Structured command dispatch, and the shared state the handlers touch.
 *
 * The split between a structured command and `shell` passthrough follows one
 * test: does the front end have to *understand* the output?  If yes it is a
 * command here and answers with JSON; if it is only for a human to read it
 * goes through shell.exec and comes back as log lines.  Inventing a
 * structured command for output nobody parses would be work that buys
 * nothing, and screen-scraping output the page does need to parse is how you
 * get a tool that breaks whenever someone improves a message.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_WEB_TOOL_WT_COMMAND_H
#define __APP_WEB_TOOL_WT_COMMAND_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wt_queue.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WT_SHELL_CMD_MAX   256
#define WT_RSP_MAX         2048

/* The only geometries the camera driver programs.  It matches exactly and
 * rejects everything else, so the check belongs here where it can answer
 * EINVAL with the list rather than in the driver where it answers with a
 * failed ioctl.  480x480 is accepted but not offered as a default: the frame
 * is short at that size and the hardware encoder was measured recovering
 * (resets=1 err=1) on 2026-08-17.
 */

#define WT_CAM_DEFAULT_W   640
#define WT_CAM_DEFAULT_H   480

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Shared state.  One instance per web_tool process; handlers get a pointer.
 *
 * The volatile fields are written by one thread and read by another without a
 * lock on purpose: each is a single word used as a flag, and the cost of
 * getting the flag one iteration late is one more camera frame or one more
 * log line.  Anything with structure (the shell command line, the queue) goes
 * through a real lock.
 */

struct wt_ctx_s
{
  struct wt_queue_s   *queue;
  struct wt_logring_s *logring;

  /* Camera thread. */

  pthread_t            cam_thread;
  volatile bool        cam_running;
  volatile bool        cam_stop;
  int                  cam_width;
  int                  cam_height;

  /* Log streaming.  The ring fills whether or not anyone is subscribed --
   * that is what makes it possible to replay what happened before the host
   * connected.
   */

  volatile bool        log_on;

  /* Shell passthrough.  One at a time: passthrough means the operator can
   * start any app on the board, and without a gate a few clicks pile up a
   * dozen tasks.
   */

  pthread_t            shell_thread;
  volatile bool        shell_running;
  volatile bool        shell_kill;
  pthread_mutex_t      shell_lock;
  char                 shell_cmd[WT_SHELL_CMD_MAX];

  /* Set once a reboot has been answered.  The main loop reboots only after
   * the response has actually left the socket, so the front end does not need
   * a special "do not wait for a reply" path for this one command.
   */

  volatile bool        reboot_pending;

  /* Same treatment for wifi.connect, and for a sharper reason: re-associating
   * tears down the very TCP connection the answer has to travel on, so a
   * handler that applied first could never reply.  Observed on 2026-08-18 as
   * `wifi.connect 失败: closed by host` on the page -- the command had in fact
   * worked, and only the answer was lost.
   *
   * So the credentials are stored and acknowledged first; the association
   * happens after the response is out, the link drops, and the board dials
   * back in.
   */

  volatile bool        wifi_pending;
  char                 wifi_ssid[36];
  char                 wifi_psk[68];

  /* Monotonic milliseconds at start-up, so log timestamps are relative to the
   * service rather than to 1970 (this part has no RTC).
   */

  uint32_t             t0_ms;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: wt_command_dispatch
 *
 * Description:
 *   Handle one REQ payload.  Returns a malloc'd, NUL-terminated JSON response
 *   body that the caller owns and enqueues as an RSP, or NULL when out of
 *   memory.  Never blocks on the network.
 *
 ****************************************************************************/

char *wt_command_dispatch(struct wt_ctx_s *ctx, const char *json,
                          size_t len);

/****************************************************************************
 * Name: wt_now_ms
 *
 * Description:
 *   Monotonic milliseconds.  Used for log timestamps and for the frame rate
 *   the camera thread reports.
 *
 ****************************************************************************/

uint32_t wt_now_ms(void);

/****************************************************************************
 * Name: wt_json_escape
 *
 * Description:
 *   Copy in to out with the escapes JSON requires, truncating at outcap.
 *   Returns the number of bytes written, excluding the terminator.
 *
 *   Log lines carry whatever a driver decided to print, including quotes and
 *   control characters, and they arrive several times a second.  Escaping
 *   here rather than building a cJSON object per line keeps the hot path free
 *   of allocation.
 *
 ****************************************************************************/

size_t wt_json_escape(char *out, size_t outcap, const char *in);

/****************************************************************************
 * Name: wt_json_log / wt_json_dropped
 *
 * Description:
 *   Build the two EVT_LOG bodies: a line, and the notice that says how many
 *   lines were lost.  Return the body length.
 *
 ****************************************************************************/

int wt_json_log(char *out, size_t outcap, uint32_t t_ms, const char *line);
int wt_json_dropped(char *out, size_t outcap, uint32_t t_ms, uint32_t n);

/****************************************************************************
 * Name: wt_json_exit
 *
 * Description:
 *   The EVT_LOG that closes a shell.exec.  The front end needs it to stop
 *   showing the command as running.
 *
 *   known distinguishes "the command exited with this status" from "the command
 *   finished but the status could not be retrieved".  The second case is the
 *   normal one here: NuttX's pclose() returns ERROR when waitpid() cannot find
 *   the shell task any more, which happens routinely for a command that
 *   finishes quickly.  Reporting that as exit=-1 made every successful `free`
 *   look like a failure, which is exactly the kind of small lie that erodes
 *   trust in a debug tool.
 *
 ****************************************************************************/

int wt_json_exit(char *out, size_t outcap, uint32_t t_ms, int status,
                 bool known);

/****************************************************************************
 * Name: wt_kvdb_is_secret
 *
 * Description:
 *   True for keys ending in ".key" or ".psk".  Same rule as the `kvdb`
 *   command, deliberately duplicated rather than shared: the masking rule is
 *   two lines and having it in both places means neither can be changed by
 *   accident in only one of them.
 *
 ****************************************************************************/

bool wt_kvdb_is_secret(const char *key);

/****************************************************************************
 * Name: wt_mask_value
 *
 * Description:
 *   Render a secret as "sk-a...9f2c (37 bytes)" -- enough to recognise which
 *   credential is loaded, not enough to use it.  Returns true when the value
 *   was masked.
 *
 ****************************************************************************/

bool wt_mask_value(char *out, size_t outcap, const char *key,
                   const char *value, bool raw);

/****************************************************************************
 * Name: wt_wifi_apply_pending
 *
 * Description:
 *   Apply what wifi.connect stored.  Called by the sender after the response
 *   has left the socket, because associating drops that socket.
 *
 ****************************************************************************/

void wt_wifi_apply_pending(struct wt_ctx_s *ctx);

/* Camera geometry check, exported so the acceptance script and the tests can
 * assert on the same list the handler uses.
 */

bool wt_camera_geometry_ok(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEB_TOOL_WT_COMMAND_H */
