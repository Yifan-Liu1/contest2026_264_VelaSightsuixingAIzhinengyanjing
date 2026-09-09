/****************************************************************************
 * app/web_tool/host/tests/nuttx/compiler.h
 *
 * Host-test shim for <nuttx/compiler.h>.
 *
 * The wire helpers under test are pure, but their headers spell FAR.  Four
 * lines here is cheaper and more honest than teaching the host build about
 * the real 1696-line header.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
#ifndef __NUTTX_COMPILER_H
#define __NUTTX_COMPILER_H
#define FAR
#endif
