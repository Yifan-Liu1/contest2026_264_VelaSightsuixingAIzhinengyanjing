/****************************************************************************
 * app/conv/conv_net.h
 *
 * The two things this application needs from off-board: the time, and the
 * model credentials.
 *
 * It used to also upload recordings over HTTP.  That went away with the
 * recordings themselves: a conversation record is now the transcript and the
 * expression analysis, both of which fit in a WebSocket frame, so there is
 * nothing left that needs a second transport.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_CONV_CONV_NET_H
#define __APP_CONV_CONV_NET_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Where the model credentials are kept.
 *
 * 8.3, like everything else on this card: FAT_LFN is off, and a longer name
 * would be silently mangled so that every path built from the constant missed.
 */

#define CONV_LLM_FILE     "/mnt/sdnand/ai_agent/conv/LLM.JSN"

#define CONV_KEY_MAX      128
#define CONV_HOST_MAX     64
#define CONV_MODEL_MAX    48

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: conv_clock_synced
 *
 * Description:
 *   Whether CLOCK_REALTIME holds a wall-clock time rather than seconds since
 *   boot.
 *
 *   Decided by magnitude, which is crude and sufficient: with no RTC the
 *   clock starts at zero and counts up, so it stays in the low thousands for
 *   any plausible uptime, while any real date is above 1.5 billion.  There is
 *   no flag to read -- clock_settime() leaves no record that it was called --
 *   so the value itself is the only evidence available.
 *
 ****************************************************************************/

bool conv_clock_synced(void);

/****************************************************************************
 * Name: conv_clock_report
 ****************************************************************************/

void conv_clock_report(void);

/****************************************************************************
 * Name: conv_clock_set
 *
 * Description:
 *   Set CLOCK_REALTIME from an epoch.
 *
 *   Setting the system clock rather than keeping an offset of our own is the
 *   whole point: with the clock set, time(NULL) is correct everywhere, so no
 *   call site has to remember to add anything.  An offset kept in this
 *   application would have to be applied by every writer of a timestamp, and
 *   the failure mode of forgetting one is a record with a silently wrong
 *   date -- which then corrupts every date query over it.
 *
 *   It does not survive a reboot.  There is no RTC, so this has to be redone
 *   each time the board starts.
 *
 ****************************************************************************/

int conv_clock_set(uint32_t epoch);

/****************************************************************************
 * Name: conv_clock_fetch
 *
 * Description:
 *   Ask the web console for the time over HTTP and set the clock from it.
 *
 *   GET /api/time, which answers {"epoch":1787037893,...}.  The reply is
 *   scanned for that one field rather than parsed as JSON: the board needs a
 *   single integer out of it, and a parser plus the RAM to run it is a poor
 *   trade for that.
 *
 ****************************************************************************/

int conv_clock_fetch(const char *host, int port);

/****************************************************************************
 * Name: conv_llm_set
 *
 * Description:
 *   Store the model host, name and API key on the card.
 *
 *   Three fields rather than just the key because that is the form the model
 *   actually needs: the project's own notes are explicit that the agent's
 *   built-in MiMo presets name models that have been withdrawn, so the
 *   host + model + key triple has to be given together and the presets
 *   skipped.  Storing only a key would leave the part most likely to be wrong
 *   unconfigurable.
 *
 *   Written in the clear.  There is no secure element and no filesystem
 *   encryption here, so this is a statement of fact rather than a choice --
 *   and a reason to treat the card as sensitive and to use a key that can be
 *   revoked.
 *
 * Returned Value:
 *   Zero on success, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_llm_set(const char *host, const char *model, const char *key);

/****************************************************************************
 * Name: conv_llm_report
 *
 * Description:
 *   Describe what is configured, without disclosing the key.
 *
 *   Only the first four characters and the length are shown, following what
 *   the agent's own config_show does.  A status display that echoes a
 *   credential in full turns every screenshot and every log paste into a
 *   leak.
 *
 * Input Parameters:
 *   out - receives a JSON object describing the state
 *   len - bytes available at out
 *
 * Returned Value:
 *   Zero on success, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_llm_report(char *out, size_t len);

#endif /* __APP_CONV_CONV_NET_H */
