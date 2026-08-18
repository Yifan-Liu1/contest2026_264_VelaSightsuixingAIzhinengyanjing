/****************************************************************************
 * app/provisioning_web/vp_http.h
 *
 * The smallest HTTP surface that can serve one form and accept one submit.
 * Everything it does not recognise is refused with a status rather than
 * tolerated: this listener sits on an open SoftAP, so "be liberal in what you
 * accept" buys nothing and costs attack surface.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_PROVISIONING_WEB_VP_HTTP_H
#define __APP_PROVISIONING_WEB_VP_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Request headers and body caps.  A provisioning form needs a few hundred
 * bytes; anything larger is a mistake or a probe.
 */

#define VP_HTTP_MAX_HEADERS 2048
#define VP_HTTP_MAX_BODY    512

/* Enough for the form page plus the largest status page. */

#define VP_HTTP_RESPONSE_MAX 4096

enum vp_http_action_e
{
  VP_HTTP_ACTION_PAGE = 0, /* GET / -- serve the form */
  VP_HTTP_ACTION_SAVE,     /* POST /save -- body is a form submit */
  VP_HTTP_ACTION_REJECT    /* answer with .status and close */
};

struct vp_http_request_s
{
  enum vp_http_action_e action;
  int                   status;         /* Only meaningful when REJECT */
  size_t                header_len;     /* Includes the blank line */
  size_t                content_length; /* 0 unless SAVE */
};

/****************************************************************************
 * Name: vp_http_parse
 *
 * Description:
 *   Parse whatever has arrived so far.  Returns -EAGAIN while the headers are
 *   incomplete and 0 once a decision has been made, with action saying what
 *   to do.  Rejections carry the status to answer with:
 *
 *     405  a method other than GET or POST
 *     404  an unknown path
 *     411  POST without Content-Length
 *     413  a body over VP_HTTP_MAX_BODY
 *     415  a submit that is not form-urlencoded
 *     431  headers over VP_HTTP_MAX_HEADERS
 *     400  anything malformed, including chunked bodies
 *
 ****************************************************************************/

int vp_http_parse(const char *buf, size_t len,
                  struct vp_http_request_s *req);

/****************************************************************************
 * Name: vp_html_escape
 *
 * Description:
 *   Escape &, <, >, " and ' for insertion into element text or an attribute.
 *   Returns the escaped length or -E2BIG.  The SSID comes from the network,
 *   so it is untrusted even though it is short.
 *
 ****************************************************************************/

int vp_html_escape(const char *in, char *out, size_t outlen);

/* Response builders.  Each returns the number of bytes written, or 0 when the
 * buffer is too small.  No password is ever part of a response body, which
 * the host tests assert directly rather than trust.
 */

size_t vp_http_form_page(char *buf, size_t buflen, const char *notice);

size_t vp_http_saved_page(char *buf, size_t buflen, const char *ssid,
                          uint32_t generation, bool open_network);

size_t vp_http_status_page(char *buf, size_t buflen, int status,
                           const char *message);

#endif /* __APP_PROVISIONING_WEB_VP_HTTP_H */
