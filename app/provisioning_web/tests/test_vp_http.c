/****************************************************************************
 * app/provisioning_web/tests/test_vp_http.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vp_http.h"

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

static int parse(const char *request, struct vp_http_request_s *req)
{
  return vp_http_parse(request, strlen(request), req);
}

static void test_dispatch(void)
{
  struct vp_http_request_s req;

  CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_PAGE, "GET / serves the form");
  CHECK(parse("GET /index.html HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_PAGE, "GET /index.html serves the form");
  CHECK(parse("GET /?x=1 HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_PAGE, "a query string is ignored");

  CHECK(parse("GET /history HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_HISTORY_LIST,
        "GET /history serves the social-history list");
  CHECK(parse("GET /history/R0000000 HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_HISTORY_JSON &&
        strcmp(req.record_key, "R0000000") == 0,
        "a strict record key selects JSON preview");
  CHECK(parse("GET /history/R1234567/download?x=1 HTTP/1.1\r\n\r\n",
              &req) == 0 &&
        req.action == VP_HTTP_ACTION_HISTORY_DOWNLOAD &&
        strcmp(req.record_key, "R1234567") == 0,
        "a strict record key selects attachment download");
  CHECK(parse("GET /history/r0000000 HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "history keys are case sensitive");
  CHECK(parse("GET /history/R000000 HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "a short history key is rejected");
  CHECK(parse("GET /history/R00000000 HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "a long history key is rejected");
  CHECK(parse("GET /history/%520000000 HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "encoded history keys are not decoded into routes");
  CHECK(parse("GET /history/R0000000/ HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "a trailing history path segment is rejected");

  CHECK(parse("GET /secret HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "an unknown path is 404");
  CHECK(parse("GET /../etc/passwd HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "there is no file mapping to traverse");

  CHECK(parse("DELETE / HTTP/1.1\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 405,
        "an unsupported method is 405");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE && req.content_length == 27,
        "a well formed submit is accepted");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Sec-Fetch-Site: cross-site \t\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 403,
        "an explicit cross-site browser submit is refused");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "sEc-FeTcH-sItE: same-origin\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE && req.content_length == 27,
        "a same-origin browser submit remains accepted");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "content-type: application/x-www-form-urlencoded; charset=UTF-8"
              "\r\nCONTENT-LENGTH: 5\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE && req.content_length == 5,
        "header names are case insensitive and parameters are allowed");

  CHECK(parse("POST /elsewhere HTTP/1.1\r\nContent-Length: 5\r\n\r\n", &req)
        == 0 && req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "a POST to an unknown path is 404");
}

static void test_limits(void)
{
  struct vp_http_request_s req;
  char big[VP_HTTP_MAX_HEADERS + 256];
  size_t used;

  CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n", &req) == -EAGAIN,
        "incomplete headers ask for more");
  CHECK(parse("GE", &req) == -EAGAIN, "a partial request line asks for more");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n\r\n", &req)
        == 0 && req.action == VP_HTTP_ACTION_REJECT && req.status == 411,
        "a submit without Content-Length is 411");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 99999\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 413,
        "an oversized body is 413");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: abc\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 400,
        "a non-numeric Content-Length is 400");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 5\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 415,
        "a non-form content type is 415");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Transfer-Encoding: chunked\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 400,
        "a chunked body is refused, not half-read");

  CHECK(parse("GET\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 400,
        "a request line without a path is 400");

  used = (size_t)snprintf(big, sizeof(big), "GET / HTTP/1.1\r\nX: ");
  memset(big + used, 'a', VP_HTTP_MAX_HEADERS + 32);
  big[used + VP_HTTP_MAX_HEADERS + 32] = '\0';
  CHECK(parse(big, &req) == 0 && req.action == VP_HTTP_ACTION_REJECT &&
        req.status == 431, "headers past the cap are 431, not buffered");
}

static void test_header_len(void)
{
  static const char request[] =
    "POST /save HTTP/1.1\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 4\r\n\r\nssid";
  struct vp_http_request_s req;

  CHECK(vp_http_parse(request, sizeof(request) - 1, &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE &&
        req.header_len == sizeof(request) - 1 - 4,
        "header_len points at the first body byte");
}

static void test_escape(void)
{
  char out[64];

  CHECK(vp_html_escape("plain", out, sizeof(out)) == 5 &&
        strcmp(out, "plain") == 0, "plain text is unchanged");
  CHECK(vp_html_escape("<b>&\"'", out, sizeof(out)) > 0 &&
        strcmp(out, "&lt;b&gt;&amp;&quot;&#39;") == 0,
        "every markup character is escaped");
  CHECK(vp_html_escape("<<<<<<<<<<", out, 16) == -E2BIG,
        "escaping refuses to overflow");
}

static void test_pages(void)
{
  char buf[VP_HTTP_RESPONSE_MAX];
  size_t len;

  len = vp_http_form_page(buf, sizeof(buf), NULL);
  CHECK(len > 0 && len < sizeof(buf), "the form page is generated");

  /* Adding the two Volcengine fields pushed the form past one IOB
   * (1514 bytes); vp_write_all() loops until every byte is sent, so a
   * response spanning two TCP segments is a minor efficiency cost on a
   * SoftAP link, not a correctness issue.  The bound here still exists so
   * a future addition that meaningfully bloats the page (a stray debug
   * dump, an accidentally duplicated block) gets caught instead of silently
   * growing forever. */

  CHECK(len < 2200, "the form response stays within a couple of IOBs");
  CHECK(strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0,
        "the form page carries a 200 status line");
  CHECK(strstr(buf, "Content-Length: ") != NULL,
        "the form page declares its length");
  CHECK(strstr(buf, "name=\"ssid\"") != NULL &&
        strstr(buf, "name=\"password\"") != NULL &&
        strstr(buf, "name=\"mimo_apikey\"") != NULL &&
        strstr(buf, "name=\"volc_appid\"") != NULL &&
        strstr(buf, "name=\"volc_token\"") != NULL,
        "the form has Wi-Fi, MiMo and Volcengine inputs");
  CHECK(strstr(buf, "action=\"/save\"") != NULL &&
        strstr(buf, "method=\"post\"") != NULL,
        "the form posts to /save");
  CHECK(strstr(buf, "type=\"password\"") != NULL,
        "the passphrase field is masked in the browser");

  len = vp_http_form_page(buf, sizeof(buf), "密码长度不合法");
  CHECK(len > 0 && strstr(buf, "密码长度不合法") != NULL,
        "a notice is rendered on the form page");

  len = vp_http_saved_page(buf, sizeof(buf), "AIPC", 3, false);
  CHECK(len > 0 && strstr(buf, "AIPC") != NULL &&
         strstr(buf, "HTTP/1.1 200 OK") != NULL,
        "the saved form shows the SSID");
  CHECK(strstr(buf, "class=\"ok\">已保存") != NULL,
        "the saved form shows a green success notice");
  CHECK(strstr(buf, "name=\"password\"") != NULL &&
        strstr(buf, "href=\"/\"") == NULL,
        "saving stays on the input form without a return link");
  CHECK(len < 2200, "the saved response stays within a couple of IOBs");

  len = vp_http_saved_page(buf, sizeof(buf), "<script>", 1, true);
  CHECK(len > 0 && strstr(buf, "<script>") == NULL &&
        strstr(buf, "&lt;script&gt;") != NULL,
        "an SSID cannot inject markup into the success page");

  len = vp_http_status_page(buf, sizeof(buf), 413, "too large");
  CHECK(len > 0 && strncmp(buf, "HTTP/1.1 413 ", 13) == 0,
        "a status page uses the requested status");

  len = vp_http_status_page(buf, sizeof(buf), 403, "forbidden");
  CHECK(len > 0 &&
        strncmp(buf, "HTTP/1.1 403 Forbidden\r\n", 24) == 0,
        "a 403 status page uses the Forbidden reason phrase");
  CHECK(vp_http_status_page(buf, 8, 400, "bad") == 0,
        "a status page refuses a buffer it cannot fill");
}

static void test_history_pages(void)
{
  struct velasight_prov_history_entry_s entry;
  char buf[VP_HTTP_RESPONSE_MAX];
  size_t len;

  CHECK(vp_http_history_key_valid("R0000000"),
        "the canonical history key is valid");
  CHECK(!vp_http_history_key_valid("R00000000") &&
        !vp_http_history_key_valid("r0000000") &&
        !vp_http_history_key_valid("R00000/0"),
        "non-canonical history keys are invalid");

  len = vp_http_form_page_with_ssid_history(buf, sizeof(buf), NULL,
                                            "AIPC", true);
  CHECK(len > 0 && strstr(buf, "href=\"/history\"") != NULL,
        "the configured form exposes the history entry point");
  len = vp_http_form_page_with_ssid_history(buf, sizeof(buf), NULL,
                                            "AIPC", false);
  CHECK(len > 0 && strstr(buf, "href=\"/history\"") == NULL,
        "a form without a provider has no dead history link");

  memset(&entry, 0, sizeof(entry));
  snprintf(entry.record_key, sizeof(entry.record_key), "R0000000");
  snprintf(entry.date, sizeof(entry.date), "<date>");
  snprintf(entry.title, sizeof(entry.title), "<script>alert(1)</script>");
  snprintf(entry.summary, sizeof(entry.summary), "A&B \"quoted\"");
  entry.calm = 60;
  entry.happy = 30;
  entry.tense = 10;
  entry.incomplete = true;

  len = vp_http_history_head_fragment(buf, sizeof(buf), 1);
  CHECK(len > 0 && strstr(buf, "共 1 条") != NULL,
        "the history head shows its record count");
  len = vp_http_history_entry_fragment(buf, sizeof(buf), &entry);
  CHECK(len > 0 && strstr(buf, "<script>") == NULL &&
        strstr(buf, "&lt;script&gt;") != NULL &&
        strstr(buf, "A&amp;B &quot;quoted&quot;") != NULL,
        "history metadata is HTML escaped");
  CHECK(strstr(buf, "href=\"/history/R0000000\"") != NULL &&
        strstr(buf, "href=\"/history/R0000000/download\"") != NULL,
        "each history entry links preview and download");
  CHECK(vp_http_history_tail_fragment(buf, sizeof(buf)) > 0 &&
        strstr(buf, "href=\"/\"") != NULL,
        "the history page links back to provisioning");

  len = vp_http_response_header(buf, sizeof(buf), 200,
                                "application/json", 8193,
                                "attachment", "R0000000");
  CHECK(len > 0 && strstr(buf, "Content-Length: 8193\r\n") != NULL &&
        strstr(buf, "Content-Disposition: attachment; "
                    "filename=\"R0000000.json\"") != NULL,
        "a download header carries exact length and safe filename");
  CHECK(vp_http_response_header(buf, sizeof(buf), 200,
                                "application/json", 1,
                                "attachment", "../bad") == 0,
        "an unsafe download filename is refused");
}

static void test_password_never_echoed(void)
{
  static const char secret[] = "VelaSecret2026";
  char buf[VP_HTTP_RESPONSE_MAX];
  size_t len;

  /* The password only ever reaches the page builders as part of an SSID by
   * mistake, so the assertion is blunt on purpose: no builder output may
   * contain it.
   */

  len = vp_http_saved_page(buf, sizeof(buf), "AIPC", 1, false);
  CHECK(len > 0 && strstr(buf, secret) == NULL,
        "the success page contains no passphrase");

  len = vp_http_form_page(buf, sizeof(buf), NULL);
  CHECK(len > 0 && strstr(buf, secret) == NULL &&
        strstr(buf, "value=\"") == NULL,
        "the form page pre-fills nothing");

  len = vp_http_status_page(buf, sizeof(buf), 400, "invalid credentials");
  CHECK(len > 0 && strstr(buf, secret) == NULL,
        "an error page contains no passphrase");
}

int main(void)
{
  test_dispatch();
  test_limits();
  test_header_len();
  test_escape();
  test_pages();
  test_history_pages();
  test_password_never_echoed();

  printf("%s: %d checks, %d failures\n",
         g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
