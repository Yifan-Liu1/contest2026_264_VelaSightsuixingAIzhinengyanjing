/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_net_autostart.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_NET_AUTOSTART_H
#define __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_NET_AUTOSTART_H

#include <nuttx/config.h>

/****************************************************************************
 * Name: bk7258_net_autostart
 *
 * Description:
 *   Join the network named by the stored credentials, ask for an address, and
 *   start the development console service if it is in this image.
 *
 *   Call from a task, never from board bring-up: every step blocks either on
 *   the CP or on the network, and an AP that stops servicing the mailbox is
 *   reset by the CP's watchdog after 8 seconds.
 *
 ****************************************************************************/

void bk7258_net_autostart(void);

#endif /* __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_NET_AUTOSTART_H */
