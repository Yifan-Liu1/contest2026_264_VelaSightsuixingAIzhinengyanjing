/****************************************************************************
 * app/provisioning_web/vp_form.h
 *
 * application/x-www-form-urlencoded decoding and credential validation.  No
 * NuttX includes on purpose: every edge case here (a truncated %, a duplicate
 * field, a 64-byte passphrase) is provoked by the host tests in milliseconds
 * instead of by retyping a form on a phone.
 *
 * The parse is deliberately split from the merge and the merge from the
 * validation.  A submitted-but-empty field and an absent field mean different
 * things -- "clear this" versus "leave this alone" -- and the old single-pass
 * design could not tell them apart, which is how a resubmit that only added
 * an API key silently erased the stored Wi-Fi password.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_PROVISIONING_WEB_VP_FORM_H
#define __APP_PROVISIONING_WEB_VP_FORM_H

#include <stdbool.h>
#include <stddef.h>

#include "velasight_provisioning.h"

/* The cap exists so a body full of unknown keys cannot keep the parser busy;
 * it is not a promise about which keys are understood.
 */

#define VP_FORM_MAX_FIELDS 16

/* Which field a failure belongs to, so the page can say what to fix instead
 * of reciting every rule at once.  VP_FORM_FIELD_BODY means the body itself
 * did not decode and no single field can be blamed.
 */

enum vp_form_field_e
{
  VP_FORM_FIELD_NONE = 0,
  VP_FORM_FIELD_BODY,
  VP_FORM_FIELD_SSID,
  VP_FORM_FIELD_PASSWORD,
  VP_FORM_FIELD_API_KEY,
  VP_FORM_FIELD_VOLC_APPID,
  VP_FORM_FIELD_VOLC_TOKEN,
  VP_FORM_FIELD_CLOUD_HOST,
  VP_FORM_FIELD_CLOUD_PORT,
  VP_FORM_FIELD_CLOUD_PATH
};

/* What one submit actually changed, for the confirmation page.  Only field
 * names are ever reported, never values.
 */

#define VP_FORM_CHANGED_SSID       (1u << 0)
#define VP_FORM_CHANGED_PASSWORD   (1u << 1)
#define VP_FORM_CHANGED_API_KEY    (1u << 2)
#define VP_FORM_CHANGED_VOLC_APPID (1u << 3)
#define VP_FORM_CHANGED_VOLC_TOKEN (1u << 4)
#define VP_FORM_CHANGED_CLOUD_HOST (1u << 5)
#define VP_FORM_CHANGED_CLOUD_PORT (1u << 6)
#define VP_FORM_CHANGED_CLOUD_PATH (1u << 7)

/* The decoded submit plus which fields were present in it.
 *
 * cred.open_network is not meaningful until vp_form_resolve() has run: the
 * parser records what arrived, and only the merge can decide whether an empty
 * password box means "this network has none" or "do not touch the stored one".
 */

struct vp_form_submit_s
{
  struct velasight_prov_credentials_s cred;
  bool have_ssid;
  bool have_password;
  bool have_api_key;
  bool have_volc_appid;
  bool have_volc_token;
  /* The three endpoint fields behave like the SSID rather than like the
   * keys: the form pre-fills them, so an empty box is a deliberate clear.
   * Clearing them means "go back to the compiled-in default", which is the
   * only way to undo a custom endpoint from the phone.  A field absent from
   * the body entirely still carries the stored value over, so a client
   * posting an older form cannot silently reset the endpoint.
   *
   * They are addresses, not secrets, which is what makes pre-filling them
   * acceptable when pre-filling a passphrase would not be.
   */

  bool have_cloud_host;
  bool have_cloud_port;
  bool have_cloud_path;

  bool open_requested;   /* the "this Wi-Fi has no password" box was ticked */
};

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
 * Name: vp_credentials_validate / vp_credentials_validate_detail
 *
 * Description:
 *   SSID must be 1..32 bytes without control characters.  The password is
 *   either empty, meaning a network with no password, or 8..63 printable
 *   ASCII bytes, which is what WPA2 accepts; the open_network flag and the
 *   password have to agree.  The three key fields are printable ASCII within
 *   their own limits, empty meaning not configured.
 *
 *   Returns 0 or -EINVAL.  The _detail form also reports which field failed,
 *   so the caller can name it.  which may be NULL.
 *
 ****************************************************************************/

int vp_credentials_validate(
    const struct velasight_prov_credentials_s *cred);

int vp_credentials_validate_detail(
    const struct velasight_prov_credentials_s *cred,
    enum vp_form_field_e *which);

/****************************************************************************
 * Name: vp_form_parse
 *
 * Description:
 *   Decode a submitted body.  This is syntax and capacity only: duplicate
 *   fields are refused rather than resolved, unknown fields are ignored, and
 *   a value too long for its destination is refused rather than truncated.
 *   Nothing here decides what an empty field means -- that is
 *   vp_form_resolve()'s job.
 *
 *   Returns 0, -EINVAL for a body that does not decode, or -E2BIG for too
 *   many fields or an over-long value.  which reports the offending field
 *   when one can be named.  which may be NULL.
 *
 *   cred.generation is left at zero: the store owns it.
 *
 ****************************************************************************/

int vp_form_parse(const char *body, size_t bodylen,
                  struct vp_form_submit_s *submit,
                  enum vp_form_field_e *which);

/****************************************************************************
 * Name: vp_form_resolve
 *
 * Description:
 *   Turn a decoded submit into the record to store, then validate it.
 *
 *   previous is the currently stored record and have_previous says whether it
 *   was readable.  The store is not consulted from here: the caller passes it
 *   in, which keeps this file free of storage dependencies and lets the host
 *   tests drive every branch without a filesystem.
 *
 *   The Wi-Fi name is required; blanking it is an error, not an omission,
 *   because the form pre-fills it and clearing it is therefore deliberate.
 *
 *   The password is three-state:
 *
 *     box ticked, password empty   -> no password, and this is the only way
 *                                     to clear a stored one
 *     box ticked, password given   -> -EINVAL, the two contradict each other
 *     password given               -> use it
 *     empty, a record exists       -> carry the stored password AND its
 *                                     open_network flag over as a pair
 *     empty, no record yet         -> -EINVAL, nothing to carry over
 *
 *   The three key fields carry over from previous when submitted empty, so a
 *   Wi-Fi-only resubmit does not erase them.
 *
 *   Returns 0 or -EINVAL, with which naming the field to fix.
 *
 ****************************************************************************/

int vp_form_resolve(struct vp_form_submit_s *submit,
                    const struct velasight_prov_credentials_s *previous,
                    bool have_previous,
                    enum vp_form_field_e *which);

/****************************************************************************
 * Name: vp_credentials_changed
 *
 * Description:
 *   The VP_FORM_CHANGED_* bits for the fields that differ between now and
 *   previous.  With have_previous false everything non-empty counts as new.
 *   Used only to list field names on the confirmation page.
 *
 ****************************************************************************/

unsigned int vp_credentials_changed(
    const struct velasight_prov_credentials_s *now,
    const struct velasight_prov_credentials_s *previous,
    bool have_previous);

#endif /* __APP_PROVISIONING_WEB_VP_FORM_H */
