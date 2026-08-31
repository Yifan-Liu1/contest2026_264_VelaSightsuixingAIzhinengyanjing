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

#include "velasight_provisioning.h"

/* Request headers and body caps.
 *
 * The body cap has to cover the worst legal submit, not the typical one: a
 * 512-byte API key, a 128-byte token, a 64-byte app id, a 63-byte passphrase
 * and a 32-byte network name, plus field names and separators, is about 850
 * bytes of plain ASCII.  Percent-encoding is where that stops being the
 * answer -- a Chinese network name costs 9 bytes per character once encoded,
 * and a key containing reserved characters triples -- so the earlier 1024
 * byte cap turned a legal submit into an unexplained refusal.
 */

#define VP_HTTP_MAX_HEADERS 2048
#define VP_HTTP_MAX_BODY    2048

/* Enough for the setup page, which is the largest response built in one
 * piece, plus its header.  History listings stream instead, one fragment at
 * a time.
 *
 * The worst case is measured, not estimated: a 32-byte network name of pure
 * markup characters expands sixfold when escaped, and the confirmation page
 * adds a warning plus every field name on top of that.  The host tests build
 * exactly that page and assert it still fits, because the failure mode of
 * getting this wrong is a builder returning zero and the phone receiving an
 * empty response with nothing to explain it.
 *
 * Raised from 5120 when the three social cloud endpoint boxes arrived.  They
 * cost more than their own length because each is rendered twice -- once in
 * the stored-settings box and once as the input's value attribute -- so a
 * 96-byte host and a 64-byte path add roughly 320 bytes between them, and the
 * two new labels, placeholders and the note add a fixed few hundred more.
 * Neither the host nor the path can contain a character HTML escaping
 * expands: vp_cloud_host_ok() and vp_cloud_path_ok() reject all five, which
 * is what keeps this a linear cost rather than a sixfold one.
 */

#define VP_HTTP_RESPONSE_MAX 6144

enum vp_http_action_e
{
  VP_HTTP_ACTION_PAGE = 0,       /* GET / -- serve the form */
  VP_HTTP_ACTION_SAVE,           /* POST / -- body is a form submit */
  VP_HTTP_ACTION_HISTORY_LIST,   /* GET /history */
  VP_HTTP_ACTION_HISTORY_JSON,   /* GET /history/<key> */
  VP_HTTP_ACTION_HISTORY_DOWNLOAD, /* GET /history/<key>/download */
  VP_HTTP_ACTION_REJECT          /* answer with .status and close */
};

struct vp_http_request_s
{
  enum vp_http_action_e action;
  int                   status;         /* Only meaningful when REJECT */
  size_t                header_len;     /* Includes the blank line */
  size_t                content_length; /* 0 unless SAVE */
  char                  record_key[VELASIGHT_PROV_HISTORY_KEY_MAX + 1];
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
 *     403  an explicit Sec-Fetch-Site: cross-site submit
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

/****************************************************************************
 * What the setup page tells the user the device already has.
 *
 * The three key fields and the passphrase are booleans on purpose.  A page
 * that could render a stored secret would eventually render it over an open
 * SoftAP; carrying only "is it set" makes that impossible rather than merely
 * unintended.  The host tests assert the absence directly.
 ****************************************************************************/

struct vp_http_state_s
{
  bool        have_record;      /* A readable record exists */
  const char *ssid;             /* Stored name; NULL when nothing is stored */
  bool        open_network;     /* The stored network has no password */
  bool        have_api_key;
  bool        have_volc_appid;
  bool        have_volc_token;

  /* The social cloud endpoint, rendered in full rather than as a boolean.
   *
   * That is the one deliberate exception to the rule above, and it is not an
   * inconsistency: an address is not a secret.  Showing it is also the only
   * way the page can be useful -- a user debugging why social mode cannot
   * reach the cloud needs to see which host the device is actually using,
   * and "已填写" would tell them nothing.
   *
   * NULL or empty means nothing is stored and the built-in default applies;
   * cloud_port 0 means the same for the port.
   */

  const char *cloud_host;
  const char *cloud_path;
  uint16_t    cloud_port;
  uint32_t    generation;       /* Times saved; 0 when unknown */
  bool        history_enabled;

  /* What to put back in the name box, when that differs from what is stored.
   *
   * A refused submit has to come back carrying what the user typed, not what
   * the device remembers.  Pre-filling the stored name instead would quietly
   * revert their edit: they would fix the password, press save, and store the
   * old network under a new passphrase without ever seeing the substitution.
   * NULL means "use ssid", which is the ordinary GET case.
   */

  const char *form_ssid;
};

/* Response builders.  Each returns the number of bytes written, or 0 when the
 * buffer is too small.  No password is ever part of a response body, which
 * the host tests assert directly rather than trust.
 */

/****************************************************************************
 * Name: vp_http_setup_page
 *
 * Description:
 *   The setup form, answered for GET / and for a refused submit.  state may
 *   be NULL, which reads as "nothing stored yet".  notice, when given, is
 *   rendered as a problem to fix; it must be short enough to survive
 *   escaping into a 256-byte field, which is why the long explanations live
 *   in the static help text instead.
 *
 *   The status follows the notice: without one this is a plain 200, with one
 *   it is a 400 that still carries the whole form.  A refused submit that
 *   answered a bare status page would cost the user everything they had
 *   already typed, which is how a single mistyped character turns into
 *   retyping a 512-byte key.
 *
 ****************************************************************************/

size_t vp_http_setup_page(char *buf, size_t buflen,
                          const struct vp_http_state_s *state,
                          const char *notice);

/****************************************************************************
 * Name: vp_http_saved_page
 *
 * Description:
 *   The confirmation, carrying the state that was just written.  changed is
 *   a mask of VP_FORM_CHANGED_* so the page can name the fields it altered
 *   without echoing any of their values, and state->open_network drives an
 *   explicit warning: a network saved without a password is either what the
 *   user meant or the one mistake they most need told about.
 *
 ****************************************************************************/

size_t vp_http_saved_page(char *buf, size_t buflen,
                          const struct vp_http_state_s *state,
                          unsigned int changed);

size_t vp_http_status_page(char *buf, size_t buflen, int status,
                           const char *message);

/* Streaming history response helpers.  response_header writes headers only;
 * history_*_fragment write body fragments only. */

bool vp_http_history_key_valid(const char *key);

size_t vp_http_response_header(char *buf, size_t buflen, int status,
                               const char *content_type,
                               size_t content_length,
                               const char *disposition,
                               const char *record_key);

size_t vp_http_history_head_fragment(char *buf, size_t buflen,
                                     unsigned int count);
size_t vp_http_history_entry_fragment(
    char *buf, size_t buflen,
    const struct velasight_prov_history_entry_s *entry);
size_t vp_http_history_tail_fragment(char *buf, size_t buflen);

#endif /* __APP_PROVISIONING_WEB_VP_HTTP_H */
