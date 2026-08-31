/****************************************************************************
 * app/velasight/include/vs_social.h
 *
 * The social session: what happens between "long-press Power on the history
 * page" and "the summary is on screen".
 *
 * This is the orchestration layer.  It owns no protocol knowledge (vs_cloud.c
 * has all of it) and no device knowledge (vs_media.c and vs_audio.c do); what
 * it owns is the schedule -- when to sample, when to upload, when to poll,
 * when an emotion is worth interrupting the user for, and how a session ends
 * without stranding anything.
 *
 * Shape
 * -----
 * Four threads, because the alternative is a capture cadence that stutters
 * whenever the network does:
 *
 *   capture   holds /dev/video0 for the session, grabs a frame on a fixed
 *             interval, pushes it into a bounded ring.  No network I/O.
 *   audio     reads PCM from vs_audio's staging ring, accumulates one chunk,
 *             encodes Ogg Opus, pushes it into the same ring.  No network I/O.
 *   upload    pops from the ring, registers and transfers, records the msgId.
 *             The only thread that blocks on a socket in the steady state.
 *   session   open, the poll loop, finalize, and the minutes.  Blocks on
 *             sockets too, but only at a low rate.
 *
 * The ring is bounded and drops its oldest entry when full.  That is the
 * intended behaviour under a stalled network, not a fallback: the session
 * wants the most recent face and the most recent speech, and a backlog of
 * stale frames is worth less than the frame arriving now.  The integration
 * plan's rule -- capture threads never do network I/O -- is what the ring
 * exists to satisfy.
 *
 * Events
 * ------
 * Everything reaches the UI through vs_app_post_event(), never by touching UI
 * state.  Two kinds, and the distinction is load-bearing:
 *
 *   VS_APP_EVENT_SOCIAL_ALERT is an in-session emotion.  It updates a colour
 *   and a short line.  It never writes history and never speaks.
 *   VS_APP_EVENT_SOCIAL_RESULT is the end of the session.  It is the only one
 *   that persists anything.
 *
 * Every event carries the request_id it was started with, so a session the
 * user walked away from cannot repaint a page that has moved on.
 *
 * Threading
 * ---------
 * Every function here is called from the UI thread and returns immediately.
 * None of them block on the network.  Exactly one session may be in flight.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_SOCIAL_H
#define __APP_VELASIGHT_INCLUDE_VS_SOCIAL_H

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Name: vs_social_start
 *
 * Description:
 *   Begin a session.  Non-blocking: the cloud handshake, the camera open and
 *   the microphone open all happen on the session thread, and the UI learns
 *   the outcome through VS_APP_EVENT_SOCIAL_STARTED or
 *   VS_APP_EVENT_SOCIAL_START_FAILED.
 *
 *   The started event is deliberately not posted until capture is actually
 *   running.  Posting it at the end of the cloud handshake would show "社交中"
 *   while the camera was still negotiating a format, and the first seconds of
 *   the conversation would be missing from the timeline with nothing on screen
 *   to suggest it.
 *
 * Input Parameters:
 *   request_id - from vs_begin_request(); stamped on every event this session
 *                emits
 *
 * Returned Value:
 *   0 when the session thread started.  -EBUSY when one is already in flight,
 *   -ENODATA when no cloud endpoint is configured, -EAGAIN when the thread
 *   could not be created.  A negative return means no event will arrive, so
 *   the caller has to handle the page transition itself.
 *
 ****************************************************************************/

int vs_social_start(uint32_t request_id);

/****************************************************************************
 * Name: vs_social_pause / vs_social_resume
 *
 * Description:
 *   Stop and restart sampling without closing the session.
 *
 *   Pause stops the capture and audio threads from producing.  It does not
 *   stop the poll loop: results for data already uploaded are still coming,
 *   and a user who paused specifically to read an alert should still see the
 *   advice that alert triggered.
 *
 *   The camera is not STREAMOFF'd and the microphone is not closed.  Both
 *   would have to renegotiate on resume, which takes long enough to be
 *   visible, and neither costs anything meaningful while idle.
 *
 * Returned Value:
 *   0 when the request was accepted; the UI learns the outcome through
 *   VS_APP_EVENT_SOCIAL_PAUSED / _RESUMED / _PAUSE_FAILED.  -EINVAL when no
 *   session is running or it is already in the requested state.
 *
 ****************************************************************************/

int vs_social_pause(void);
int vs_social_resume(void);

/****************************************************************************
 * Name: vs_social_finalize
 *
 * Description:
 *   End the session and produce its minutes.  Non-blocking.
 *
 *   Stops sampling, uploads the tail -- the partial audio chunk that has not
 *   reached its full duration is sent rather than discarded, because it is the
 *   end of the conversation -- then closes the session and polls for the
 *   minutes until they arrive or the timeout expires.
 *
 *   On success the minutes are persisted with vs_history_append() before
 *   VS_APP_EVENT_SOCIAL_RESULT is posted, so a record on screen is a record on
 *   the card.  On failure VS_APP_EVENT_SOCIAL_FINALIZE_FAILED carries the
 *   errno.
 *
 * Input Parameters:
 *   request_id - the id the UI is now waiting on.  Normally the same one the
 *                session started with; passed again rather than remembered so
 *                a mismatch is visible here instead of producing an event the
 *                UI silently drops.
 *
 * Returned Value:
 *   0 when the request was accepted, -EINVAL when no session is running.
 *
 ****************************************************************************/

int vs_social_finalize(uint32_t request_id);

/****************************************************************************
 * Name: vs_social_abort
 *
 * Description:
 *   Give up on the session without asking for minutes.  This is the "user
 *   pressed back while it was still starting" path.
 *
 *   Still closes the session with the cloud, on a detached thread so the UI
 *   does not wait.  That matters: one deviceId may hold one live session, so
 *   walking away from an open one would make the next attempt fail with -EBUSY
 *   and report a problem that has nothing to do with what went wrong.
 *
 *   Posts no event.  The caller has already moved the page.
 *
 ****************************************************************************/

void vs_social_abort(void);

/* True while a session is running, including while it is finalizing.  For the
 * UI to avoid offering a second start, and for shutdown.
 */

bool vs_social_active(void);

/* Wait for a running session to finish and release everything.  Blocking; for
 * shutdown only.  Safe when nothing is running.
 */

void vs_social_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_SOCIAL_H */
