/****************************************************************************
 * app/provisioning_web/vp_form.h
 *
 * application/x-www-form-urlencoded decoding and credential validation.  No
 * NuttX includes on purpose: every edge case here (a truncated %, a duplicate
 * field, a 64-byte passphrase) is provoked by the host tests in milliseconds
 * instead of by retyping a form on a phone.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_PROVISIONING_WEB_VP_FORM_H
#define __APP_PROVISIONING_WEB_VP_FORM_H

#include <stddef.h>

#include "velasight_provisioning.h"

/* A provisioning form has two fields.  The cap exists so a body full of
 * unknown keys cannot keep the parser busy; it is not a promise about which
 * keys are understood.
 */

#define VP_FORM_MAX_FIELDS 16

/****************************************************************************
 * Name: vp_url_decode
 *
 * Description:
 *   Decode one form component.  '+' becomes a space, %xx becomes the byte.
 *   Returns the decoded length, -EINVAL for a malformed escape or an embedded
 *   NUL, or -E2BIG when the result does not fit with its terminator.
 *
 ****************************************************************************/

int vp_url_decode(const char *src, size_t srclen, char *out, size_t outlen);

/****************************************************************************
 * Name: vp_credentials_validate
 *
 * Description:
 *   SSID must be 1..32 bytes without control characters.  The password is
 *   either empty, meaning an open network, or 8..63 printable ASCII bytes,
 *   which is what WPA2 accepts.  Returns 0 or -EINVAL.
 *
 ****************************************************************************/

int vp_credentials_validate(
    const struct velasight_prov_credentials_s *cred);

/****************************************************************************
 * Name: vp_form_parse
 *
 * Description:
 *   Parse a submitted body into credentials.  ssid is required; a missing
 *   password field means an open network.  A repeated ssid or password is
 *   rejected rather than resolved, because either choice would be a guess.
 *   Unknown fields are ignored.  Returns 0, -EINVAL, or -E2BIG.
 *
 *   generation is left untouched: the store owns it.
 *
 ****************************************************************************/

int vp_form_parse(const char *body, size_t bodylen,
                  struct velasight_prov_credentials_s *cred);

#endif /* __APP_PROVISIONING_WEB_VP_FORM_H */
