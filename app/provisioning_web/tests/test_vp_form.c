/****************************************************************************
 * app/provisioning_web/tests/test_vp_form.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "vp_form.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                       \
  do                                                            \
    {                                                           \
      g_checks++;                                                \
      if (!(cond))                                              \
        {                                                       \
          g_failures++;                                         \
          printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);   \
        }                                                       \
    }                                                           \
  while (0)

static void test_url_decode(void)
{
  char out[64];

  CHECK(vp_url_decode("plain", 5, out, sizeof(out)) == 5 &&
        strcmp(out, "plain") == 0, "plain text passes through");

  CHECK(vp_url_decode("a+b", 3, out, sizeof(out)) == 3 &&
        strcmp(out, "a b") == 0, "plus becomes a space");

  CHECK(vp_url_decode("a%20b", 5, out, sizeof(out)) == 3 &&
        strcmp(out, "a b") == 0, "%20 becomes a space");

  CHECK(vp_url_decode("%2f%2F", 6, out, sizeof(out)) == 2 &&
        strcmp(out, "//") == 0, "hex is case insensitive");

  CHECK(vp_url_decode("caf%C3%A9", 9, out, sizeof(out)) == 5 &&
        memcmp(out, "caf\xc3\xa9", 5) == 0, "utf-8 survives byte for byte");

  CHECK(vp_url_decode("a%", 2, out, sizeof(out)) == -EINVAL,
        "a truncated escape is rejected");
  CHECK(vp_url_decode("a%2", 3, out, sizeof(out)) == -EINVAL,
        "a one-digit escape is rejected");
  CHECK(vp_url_decode("a%zz", 4, out, sizeof(out)) == -EINVAL,
        "a non-hex escape is rejected");
  CHECK(vp_url_decode("a%00b", 5, out, sizeof(out)) == -EINVAL,
        "an embedded NUL is rejected, not truncated");

  CHECK(vp_url_decode("", 0, out, sizeof(out)) == 0 && out[0] == '\0',
        "an empty component decodes to an empty string");

  CHECK(vp_url_decode("abcd", 4, out, 5) == 4 && strcmp(out, "abcd") == 0,
        "outlen counts the terminator");
  CHECK(vp_url_decode("abcd", 4, out, 4) == -E2BIG,
        "a buffer one byte short is refused, not truncated");
}

static void test_validate(void)
{
  struct velasight_prov_credentials_s cred;
  size_t i;

  memset(&cred, 0, sizeof(cred));
  strcpy(cred.ssid, "AIPC");
  strcpy(cred.password, "12345678");
  CHECK(vp_credentials_validate(&cred) == 0, "8-byte password is accepted");

  strcpy(cred.password, "1234567");
  CHECK(vp_credentials_validate(&cred) == -EINVAL,
        "7-byte password is rejected");

  cred.password[0] = '\0';
  cred.open_network = true;
  CHECK(vp_credentials_validate(&cred) == 0,
        "an empty password is an open network");

  cred.open_network = false;
  CHECK(vp_credentials_validate(&cred) == -EINVAL,
        "an empty password with open_network clear is inconsistent");

  memset(&cred, 0, sizeof(cred));
  cred.open_network = true;
  CHECK(vp_credentials_validate(&cred) == -EINVAL,
        "an empty SSID is rejected");

  memset(&cred, 0, sizeof(cred));
  for (i = 0; i < VELASIGHT_PROV_SSID_MAX; i++)
    {
      cred.ssid[i] = 'a';
    }

  cred.open_network = true;
  CHECK(vp_credentials_validate(&cred) == 0, "a 32-byte SSID is accepted");

  memset(&cred, 0, sizeof(cred));
  strcpy(cred.ssid, "bad\nssid");
  cred.open_network = true;
  CHECK(vp_credentials_validate(&cred) == -EINVAL,
        "a control character in the SSID is rejected");

  memset(&cred, 0, sizeof(cred));
  strcpy(cred.ssid, "ok");
  for (i = 0; i < VELASIGHT_PROV_PSK_MAX; i++)
    {
      cred.password[i] = 'p';
    }

  CHECK(vp_credentials_validate(&cred) == 0, "a 63-byte password fits");

  cred.password[0] = '\t';
  CHECK(vp_credentials_validate(&cred) == -EINVAL,
        "a control character in the password is rejected");
}

/* Parse a literal body without restating its length.
 *
 * Hand-counted lengths are their own bug source: a body that is one byte short
 * ends in a truncated escape, so the test fails for a reason that has nothing
 * to do with what it meant to check.  The cases that genuinely exercise a
 * length build it instead.
 */

static int parse_str(const char *body, struct vp_form_submit_s *s,
                     enum vp_form_field_e *which)
{
  return vp_form_parse(body, strlen(body), s, which);
}

static void test_form_parse(void)
{
  struct vp_form_submit_s s;
  enum vp_form_field_e which;
  char body[512];
  size_t used;
  size_t i;

  CHECK(parse_str("ssid=AIPC&password=secretpw", &s, &which) == 0 &&
        strcmp(s.cred.ssid, "AIPC") == 0 &&
        strcmp(s.cred.password, "secretpw") == 0 &&
        s.have_ssid && s.have_password && !s.open_requested,
        "both fields are parsed and marked present");

  CHECK(parse_str("password=secretpw&ssid=AIPC", &s, &which) == 0 &&
        strcmp(s.cred.ssid, "AIPC") == 0,
        "field order does not matter");

  /* The parser reports what arrived and nothing more.  Deciding that an empty
   * box means "this network has no password" is vp_form_resolve()'s job, and
   * doing it here is what used to erase a stored passphrase.
   */

  CHECK(parse_str("ssid=My+Net&password=", &s, &which) == 0 &&
        strcmp(s.cred.ssid, "My Net") == 0 && s.cred.password[0] == '\0' &&
        s.have_password && !s.open_requested && !s.cred.open_network,
        "an empty password box is recorded as present but empty");

  CHECK(parse_str("ssid=My%20Net", &s, &which) == 0 &&
        strcmp(s.cred.ssid, "My Net") == 0 && !s.have_password &&
        !s.cred.open_network,
        "an absent password field is recorded as absent");

  CHECK(parse_str("ssid=Open&no_password=on", &s, &which) == 0 &&
        s.open_requested, "the no-password box is parsed when ticked");

  CHECK(parse_str("ssid=Open&no_password=off", &s, &which) == 0 &&
        !s.open_requested,
        "a spelled-out off is not read as ticked");

  CHECK(parse_str("ssid=Open&no_password=", &s, &which) == 0 &&
        !s.open_requested,
        "an empty no-password value is not read as ticked");

  CHECK(parse_str("password=secretpw", &s, &which) == -EINVAL &&
        which == VP_FORM_FIELD_SSID, "a missing SSID is rejected by name");

  CHECK(parse_str("ssid=a&ssid=b&password=secretpw", &s, &which)
        == -EINVAL && which == VP_FORM_FIELD_SSID,
        "a duplicate SSID is rejected, not resolved");

  CHECK(parse_str("ssid=a&password=secretpw&password=other", &s,
                      &which) == -EINVAL &&
        which == VP_FORM_FIELD_PASSWORD, "a duplicate password is rejected");

  CHECK(parse_str("ssid=a&no_password=on&no_password=on", &s,
                      &which) == -EINVAL,
        "a duplicate no-password box is rejected");

  CHECK(parse_str("csrf=x&ssid=AIPC&extra=1&password=secretpw", &s, &which) == 0 && strcmp(s.cred.ssid, "AIPC") == 0,
        "unknown fields are ignored");

  CHECK(parse_str("", &s, &which) == -EINVAL,
        "an empty body is rejected");
  CHECK(parse_str("ssid", &s, &which) == -EINVAL &&
        which == VP_FORM_FIELD_BODY, "a field without = is rejected");
  CHECK(parse_str("ssid=a%2&password=secretpw", &s, &which)
        == -EINVAL, "a malformed escape fails the whole submit");

  /* A 7-byte passphrase is a rule, not a syntax error, so the parser accepts
   * it and vp_form_resolve() is the one that refuses it.  Keeping the two
   * apart is what lets the page say which field to fix.
   */

  CHECK(parse_str("ssid=ok&password=short", &s, &which) == 0,
        "a too-short password is not a parse failure");

  /* Pasted keys arrive with keyboard whitespace attached far more often than
   * not, and the platform's only answer to a token with a trailing newline is
   * an unexplained 401.
   */

  CHECK(parse_str("ssid=ok&mimo_apikey=+key+&volc_token=%09tok%0A", &s, &which) == 0 &&
        strcmp(s.cred.api_key, "key") == 0 &&
        strcmp(s.cred.volc_token, "tok") == 0,
        "surrounding whitespace is trimmed from the key fields");

  CHECK(parse_str("ssid=ok&password=+pass+word+", &s, &which) == 0 &&
        strcmp(s.cred.password, " pass word ") == 0,
        "the Wi-Fi password keeps its spaces, which WPA2 allows");

  /* An SSID one byte over the limit must fail as a length error rather than
   * arrive silently truncated to something the phone never typed.
   */

  used = 0;
  used += (size_t)snprintf(body + used, sizeof(body) - used, "ssid=");
  for (i = 0; i < VELASIGHT_PROV_SSID_MAX + 1; i++)
    {
      body[used++] = 'a';
    }

  body[used] = '\0';
  CHECK(vp_form_parse(body, used, &s, &which) == -E2BIG &&
        which == VP_FORM_FIELD_SSID,
        "a 33-byte SSID is refused as too long, by name");

  used = 0;
  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "ssid=ok&password=");
  for (i = 0; i < VELASIGHT_PROV_PSK_MAX + 1; i++)
    {
      body[used++] = 'p';
    }

  body[used] = '\0';
  CHECK(vp_form_parse(body, used, &s, &which) == -E2BIG &&
        which == VP_FORM_FIELD_PASSWORD,
        "a 64-byte password is refused as too long, by name");

  used = 0;
  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "ssid=ok&volc_appid=");
  for (i = 0; i < VELASIGHT_PROV_VOLC_APPID_MAX + 1; i++)
    {
      body[used++] = 'n';
    }

  body[used] = '\0';
  CHECK(vp_form_parse(body, used, &s, &which) == -E2BIG &&
        which == VP_FORM_FIELD_VOLC_APPID,
        "a 65-byte app id is refused as too long, by name");

  used = 0;
  for (i = 0; i < VP_FORM_MAX_FIELDS + 4; i++)
    {
      used += (size_t)snprintf(body + used, sizeof(body) - used,
                               "k%u=v&", (unsigned)i);
    }

  used += (size_t)snprintf(body + used, sizeof(body) - used, "ssid=AIPC");
  CHECK(vp_form_parse(body, used, &s, &which) == -E2BIG,
        "too many fields is refused instead of parsed forever");
}

/* The stored record every merge test starts from: a network with a password,
 * a key and both voice fields, so anything that goes missing is visible.
 */

static void stored_record(struct velasight_prov_credentials_s *out)
{
  memset(out, 0, sizeof(*out));
  strcpy(out->ssid, "HomeNet");
  strcpy(out->password, "oldpass123");
  strcpy(out->api_key, "old-mimo-key");
  strcpy(out->volc_appid, "old-appid");
  strcpy(out->volc_token, "old-token");
  out->generation = 7;
}

static void test_form_resolve(void)
{
  struct velasight_prov_credentials_s prev;
  struct vp_form_submit_s s;
  enum vp_form_field_e which;

  /* The regression this whole split exists for: someone opens the page to add
   * a key, the password box is blank because it is never pre-filled, and the
   * stored passphrase has to survive that.
   */

  stored_record(&prev);
  CHECK(parse_str("ssid=HomeNet&password=&mimo_apikey=new-key", &s,
                      &which) == 0 &&
        vp_form_resolve(&s, &prev, true, &which) == 0 &&
        strcmp(s.cred.password, "oldpass123") == 0 &&
        !s.cred.open_network &&
        strcmp(s.cred.api_key, "new-key") == 0 &&
        strcmp(s.cred.volc_appid, "old-appid") == 0 &&
        strcmp(s.cred.volc_token, "old-token") == 0,
        "a key-only resubmit keeps the stored password and voice fields");

  /* The flag has to travel with the passphrase.  Carrying one without the
   * other produces a record whose two halves disagree, which reads as a
   * different network depending on which field the reader trusts.
   */

  stored_record(&prev);
  prev.password[0] = '\0';
  prev.open_network = true;
  CHECK(parse_str("ssid=HomeNet&password=", &s, &which) == 0 &&
        vp_form_resolve(&s, &prev, true, &which) == 0 &&
        s.cred.password[0] == '\0' && s.cred.open_network,
        "carrying a stored open network over keeps both halves in step");

  stored_record(&prev);
  CHECK(parse_str("ssid=HomeNet&password=brandnew1", &s, &which)
        == 0 && vp_form_resolve(&s, &prev, true, &which) == 0 &&
        strcmp(s.cred.password, "brandnew1") == 0 && !s.cred.open_network,
        "a typed password replaces the stored one");

  /* Ticking the box is the only way to clear a stored passphrase, which is
   * what makes an accidental clear impossible rather than merely unlikely.
   */

  stored_record(&prev);
  CHECK(parse_str("ssid=HomeNet&password=&no_password=on", &s,
                      &which) == 0 &&
        vp_form_resolve(&s, &prev, true, &which) == 0 &&
        s.cred.password[0] == '\0' && s.cred.open_network,
        "ticking the no-password box clears the stored password");

  stored_record(&prev);
  CHECK(parse_str("ssid=HomeNet&password=brandnew1&no_password=on", &s, &which) == 0 &&
        vp_form_resolve(&s, &prev, true, &which) == -EINVAL &&
        which == VP_FORM_FIELD_PASSWORD,
        "the box and a typed password contradict each other");

  /* First-time setup has nothing to fall back on, so an empty box cannot mean
   * "leave it alone" and must not silently mean "no password" either.
   */

  CHECK(parse_str("ssid=FirstTime&password=", &s, &which) == 0 &&
        vp_form_resolve(&s, NULL, false, &which) == -EINVAL &&
        which == VP_FORM_FIELD_PASSWORD,
        "a blank password with nothing stored is refused, not assumed open");

  CHECK(parse_str("ssid=FirstTime&no_password=on", &s, &which) == 0 &&
        vp_form_resolve(&s, NULL, false, &which) == 0 &&
        s.cred.open_network,
        "first-time setup of a network with no password is allowed");

  CHECK(parse_str("ssid=FirstTime&password=goodpass1", &s, &which)
        == 0 && vp_form_resolve(&s, NULL, false, &which) == 0 &&
        strcmp(s.cred.password, "goodpass1") == 0 &&
        s.cred.api_key[0] == '\0',
        "first-time setup with a password needs no stored record");

  /* The name is pre-filled, so a blank box is a deliberate clear rather than
   * an omission, and there is nothing sensible to store for a nameless one.
   */

  stored_record(&prev);
  CHECK(parse_str("ssid=&password=", &s, &which) == 0 &&
        vp_form_resolve(&s, &prev, true, &which) == -EINVAL &&
        which == VP_FORM_FIELD_SSID,
        "a blanked network name is refused rather than carried over");

  stored_record(&prev);
  CHECK(parse_str("ssid=HomeNet&password=short", &s, &which) == 0 &&
        vp_form_resolve(&s, &prev, true, &which) == -EINVAL &&
        which == VP_FORM_FIELD_PASSWORD,
        "a too-short password is refused and named");

  CHECK(parse_str("ssid=HomeNet&password=goodpass1&volc_appid=%01", &s, &which) == 0 &&
        vp_form_resolve(&s, NULL, false, &which) == -EINVAL &&
        which == VP_FORM_FIELD_VOLC_APPID,
        "an unprintable byte in the app id is refused and named");
}

static void test_changed(void)
{
  struct velasight_prov_credentials_s prev;
  struct velasight_prov_credentials_s now;
  unsigned int changed;

  stored_record(&prev);
  now = prev;
  CHECK(vp_credentials_changed(&now, &prev, true) == 0,
        "an identical record reports nothing changed");

  now = prev;
  strcpy(now.api_key, "new");
  changed = vp_credentials_changed(&now, &prev, true);
  CHECK(changed == VP_FORM_CHANGED_API_KEY,
        "only the field that moved is reported");

  now = prev;
  now.password[0] = '\0';
  now.open_network = true;
  CHECK((vp_credentials_changed(&now, &prev, true) &
         VP_FORM_CHANGED_PASSWORD) != 0,
        "dropping the password counts as a password change");

  now = prev;
  strcpy(now.ssid, "Other");
  strcpy(now.volc_token, "t2");
  CHECK(vp_credentials_changed(&now, &prev, true) ==
        (VP_FORM_CHANGED_SSID | VP_FORM_CHANGED_VOLC_TOKEN),
        "two moved fields are both reported");

  memset(&now, 0, sizeof(now));
  strcpy(now.ssid, "Fresh");
  now.open_network = true;
  CHECK(vp_credentials_changed(&now, NULL, false) ==
        (VP_FORM_CHANGED_SSID | VP_FORM_CHANGED_PASSWORD),
        "with nothing stored every field that has a value is new");
}

int main(void)
{
  test_url_decode();
  test_validate();
  test_form_parse();
  test_form_resolve();
  test_changed();

  printf("%s: %d checks, %d failures\n",
         g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
