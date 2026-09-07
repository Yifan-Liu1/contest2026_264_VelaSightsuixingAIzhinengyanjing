/****************************************************************************
 * app/velasight/include/vs_tts.h
 *
 * One owner for "speak this WAV file", shared by the two places that need it.
 *
 * Why a module rather than a function
 * ----------------------------------
 * Two callers want the same thing from opposite sides of the threading rules.
 * The social session, having just downloaded the spoken minutes, wants to
 * start playback and get on with tearing itself down.  The history page wants
 * to start playback of whatever record the user has settled on, and to stop it
 * the moment they move -- and the history page *is* the UI thread, which
 * vs_audio.h forbids from doing anything but stop() on a playback handle.
 *
 * Neither can be served by a blocking call.  So the file reading and the DAC
 * feeding live on one worker thread here, and both callers only ever hand over
 * a path or ask for silence.
 *
 * It also makes the mutual exclusion real.  There is one DAC, one worker and
 * one file at a time, so a session's minutes and a browsed record cannot end
 * up interleaved on the speaker -- which is exactly what two independent
 * copies of the same loop would have allowed.
 *
 * Restarting the same file is deliberately not restarting
 * ------------------------------------------------------
 * vs_tts_play() with the path that is already playing does nothing and reports
 * success.  That is what makes the two callers compose: a session posts its
 * result, playback starts, and the user immediately backs out of the result
 * page onto the history entry that session just wrote.  The dwell timer then
 * asks for that record's audio -- the same file -- and without this rule the
 * user would hear the first two seconds again.
 *
 * Threading
 * ---------
 * Every function here is safe to call from the UI thread and returns without
 * waiting for audio.  vs_tts_close() is the one exception and is for shutdown.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_TTS_H
#define __APP_VELASIGHT_INCLUDE_VS_TTS_H

#include <stdbool.h>
#include <stddef.h>

/* Long enough for the history store's own paths, which are the only ones this
 * is asked to play: VS_HISTORY_PATH_MAX is 96.
 */

#define VS_TTS_PATH_MAX 128

/****************************************************************************
 * Name: vs_tts_open
 *
 * Description:
 *   Start the playback worker.  Opens no device -- the DAC is opened per file,
 *   because the sample rate comes out of the file's own header -- so this is
 *   cheap and safe on the startup path.
 *
 * Returned Value:
 *   0, or a negative errno when the thread could not be created.  A failure
 *   is not fatal to the application: every later call becomes a no-op that
 *   reports -ENODEV, and the only thing lost is spoken output.
 *
 ****************************************************************************/

int vs_tts_open(void);

/****************************************************************************
 * Name: vs_tts_play
 *
 * Description:
 *   Play one RIFF/WAVE file, replacing whatever is playing.  Returns as soon
 *   as the request is handed to the worker; the file is opened, parsed and
 *   fed to the DAC there.
 *
 *   Asking for the file that is already playing is a no-op -- see the header
 *   comment for why that rule exists rather than being an optimisation.
 *
 *   Only 16-bit PCM at 8000, 16000 or 32000 Hz can be played; this board's
 *   audio block cannot reach other rates (see vs_audio.h).  A file at any
 *   other rate is refused by name in the log rather than played at the wrong
 *   pitch.
 *
 * Returned Value:
 *   0 when accepted or already playing.  -ENODEV when the worker is not
 *   running, -EINVAL for an empty path, -ENAMETOOLONG when the path does not
 *   fit VS_TTS_PATH_MAX.  Anything wrong with the *file* is reported in the
 *   log rather than here, because by the time it is discovered this call has
 *   long returned.
 *
 ****************************************************************************/

int vs_tts_play(const char *path);

/****************************************************************************
 * Name: vs_tts_stop
 *
 * Description:
 *   Silence the speaker and forget the request.  Safe to call when nothing is
 *   playing, and safe to call repeatedly -- the history page does exactly that
 *   on every navigation.
 *
 ****************************************************************************/

void vs_tts_stop(void);

/* True while a file is open and being fed to the DAC.  For the UI to show that
 * something is speaking, and for shutdown to know whether it is waiting on
 * anything.
 */

bool vs_tts_busy(void);

/* Copy the path being played into out, or leave it empty when idle.  Lets a
 * caller decide whether the file it is about to ask for is already the one on
 * the speaker without depending on this module's own comparison.
 */

void vs_tts_current(char *out, size_t len);

/* Stop playback, join the worker and release everything.  Blocking; for
 * shutdown only.  Safe when vs_tts_open() failed or was never called.
 */

void vs_tts_close(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_TTS_H */
