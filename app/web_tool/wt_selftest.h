/****************************************************************************
 * app/web_tool/wt_selftest.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_WEB_TOOL_WT_SELFTEST_H
#define __APP_WEB_TOOL_WT_SELFTEST_H

/****************************************************************************
 * Name: wt_selftest_run
 *
 * Description:
 *   Drive an already-running web_tool from the board itself, over loopback,
 *   and print what was measured.  Returns 0 when every check passed.
 *
 *   This exists because the only wireless network available to this board is
 *   an open guest SSID that blocks traffic to and from the development
 *   machine's wired subnet, so without it the board-side code would reach
 *   acceptance having only ever run against a mock.
 *
 ****************************************************************************/

int wt_selftest_run(const char *host, int port);

#endif /* __APP_WEB_TOOL_WT_SELFTEST_H */
