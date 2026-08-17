/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_status_screen.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_STATUS_SCREEN_H
#define __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_STATUS_SCREEN_H

#include <nuttx/config.h>
#include <nuttx/compiler.h>

/* The states the two modes go through, spelled as the product spec spells
 * them (social: IDLE/CONSENT/RECORDING/FINALIZING/RESULT_READY, voice:
 * LISTENING/TRANSCRIBING/THINKING/SPEAKING, plus CANCELLED and ERROR).
 */

enum bk7258_status_e
{
  BK7258_STATUS_IDLE = 0,
  BK7258_STATUS_CONSENT,
  BK7258_STATUS_RECORDING,
  BK7258_STATUS_FINALIZING,
  BK7258_STATUS_RESULT_READY,
  BK7258_STATUS_LISTENING,
  BK7258_STATUS_TRANSCRIBING,
  BK7258_STATUS_THINKING,
  BK7258_STATUS_SPEAKING,
  BK7258_STATUS_CANCELLED,
  BK7258_STATUS_ERROR,
  BK7258_STATUS_MAX
};

/* Both are no-ops when what they would draw is already on the glass, so a
 * caller may call them on every event without costing a 25ms push.
 */

int bk7258_status_screen_state(enum bk7258_status_e state);
int bk7258_status_screen_summary(FAR const char *line1,
                                 FAR const char *line2);
void bk7258_status_screen_invalidate(void);

#endif /* __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_STATUS_SCREEN_H */
