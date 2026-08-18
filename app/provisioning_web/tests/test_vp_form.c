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

static void test_form_parse(void)
{
  struct velasight_prov_credentials_s cred;
  char body[512];
  size_t used;
  size_t i;

  CHECK(vp_form_parse("ssid=AIPC&password=secretpw", 27, &cred) == 0 &&
        strcmp(cred.ssid, "AIPC") == 0 &&
        strcmp(cred.password, "secretpw") == 0 && !cred.open_network,
        "both fields are parsed");

  CHECK(vp_form_parse("password=secretpw&ssid=AIPC", 27, &cred) == 0 &&
        strcmp(cred.ssid, "AIPC") == 0,
        "field order does not matter");

  CHECK(vp_form_parse("ssid=My+Net&password=", 21, &cred) == 0 &&
        strcmp(cred.ssid, "My Net") == 0 && cred.password[0] == '\0' &&
        cred.open_network, "an empty password means an open network");

  CHECK(vp_form_parse("ssid=My%20Net", 13, &cred) == 0 &&
        strcmp(cred.ssid, "My Net") == 0 && cred.open_network,
        "a missing password field means an open network");

  CHECK(vp_form_parse("password=secretpw", 17, &cred) == -EINVAL,
        "a missing SSID is rejected");

  CHECK(vp_form_parse("ssid=a&ssid=b&password=secretpw", 31, &cred)
        == -EINVAL, "a duplicate SSID is rejected, not resolved");

  CHECK(vp_form_parse("ssid=a&password=secretpw&password=other", 39, &cred)
        == -EINVAL, "a duplicate password is rejected");

  CHECK(vp_form_parse("csrf=x&ssid=AIPC&extra=1&password=secretpw", 42,
                      &cred) == 0 && strcmp(cred.ssid, "AIPC") == 0,
        "unknown fields are ignored");

  CHECK(vp_form_parse("", 0, &cred) == -EINVAL, "an empty body is rejected");
  CHECK(vp_form_parse("ssid", 4, &cred) == -EINVAL,
        "a field without = is rejected");
  CHECK(vp_form_parse("ssid=a%2&password=secretpw", 26, &cred) == -EINVAL,
        "a malformed escape fails the whole submit");

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
  CHECK(vp_form_parse(body, used, &cred) == -EINVAL,
        "a 33-byte SSID is rejected");

  used = 0;
  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "ssid=ok&password=");
  for (i = 0; i < VELASIGHT_PROV_PSK_MAX + 1; i++)
    {
      body[used++] = 'p';
    }

  body[used] = '\0';
  CHECK(vp_form_parse(body, used, &cred) == -EINVAL,
        "a 64-byte password is rejected");

  used = 0;
  for (i = 0; i < VP_FORM_MAX_FIELDS + 4; i++)
    {
      used += (size_t)snprintf(body + used, sizeof(body) - used,
                               "k%u=v&", (unsigned)i);
    }

  used += (size_t)snprintf(body + used, sizeof(body) - used, "ssid=AIPC");
  CHECK(vp_form_parse(body, used, &cred) == -E2BIG,
        "too many fields is refused instead of parsed forever");
}

int main(void)
{
  test_url_decode();
  test_validate();
  test_form_parse();

  printf("%s: %d checks, %d failures\n",
         g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
