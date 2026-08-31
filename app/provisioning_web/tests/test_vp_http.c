/****************************************************************************
 * app/provisioning_web/tests/test_vp_http.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vp_form.h"
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

  CHECK(parse("POST / HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE && req.content_length == 27,
        "a well formed submit is accepted");

  /* The submit posts back to the page it came from, so the whole service lives
   * at one address and the tab bar always points at where the user is.
   */

  CHECK(parse("POST /index.html HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE,
        "the submit is accepted at the same two paths the page is served at");

  CHECK(parse("POST /save HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 404,
        "the separate save endpoint is gone rather than kept as an alias");

  CHECK(parse("POST /?x=1 HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE,
        "a query string on the submit target is stripped, not routed on");

  CHECK(parse("POST / HTTP/1.1\r\n"
              "Sec-Fetch-Site: cross-site \t\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 403,
        "an explicit cross-site browser submit is refused");

  CHECK(parse("POST / HTTP/1.1\r\n"
              "sEc-FeTcH-sItE: same-origin\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 27\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_SAVE && req.content_length == 27,
        "a same-origin browser submit remains accepted");

  CHECK(parse("POST / HTTP/1.1\r\n"
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

  CHECK(parse("POST / HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n\r\n", &req)
        == 0 && req.action == VP_HTTP_ACTION_REJECT && req.status == 411,
        "a submit without Content-Length is 411");

  CHECK(parse("POST / HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 99999\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 413,
        "an oversized body is 413");

  CHECK(parse("POST / HTTP/1.1\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: abc\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 400,
        "a non-numeric Content-Length is 400");

  CHECK(parse("POST / HTTP/1.1\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 5\r\n\r\n", &req) == 0 &&
        req.action == VP_HTTP_ACTION_REJECT && req.status == 415,
        "a non-form content type is 415");

  CHECK(parse("POST / HTTP/1.1\r\n"
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
    "POST / HTTP/1.1\r\n"
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

static unsigned int count_occurrences(const char *haystack,
                                      const char *needle)
{
  unsigned int n = 0;
  const char *p = haystack;

  while ((p = strstr(p, needle)) != NULL)
    {
      n++;
      p += strlen(needle);
    }

  return n;
}

/* A device that has been set up once: a network with a password, a key and
 * both voice fields.  Every credential the page may say something about is a
 * boolean, so no secret reaches the renderer.  The endpoint is the deliberate
 * exception -- an address is not a secret and the page renders it in full --
 * so it is left empty here, which is what "the factory default applies" looks
 * like, and set explicitly by the tests that care.
 */

static void configured_state(struct vp_http_state_s *state)
{
  memset(state, 0, sizeof(*state));
  state->have_record      = true;
  state->ssid             = "AIPC";
  state->open_network     = false;
  state->have_api_key     = true;
  state->have_volc_appid  = true;
  state->have_volc_token  = false;
  state->generation       = 3;
  state->history_enabled  = false;
}

static void test_pages(void)
{
  struct vp_http_state_s state;
  char buf[VP_HTTP_RESPONSE_MAX];
  size_t len;

  len = vp_http_setup_page(buf, sizeof(buf), NULL, NULL);
  CHECK(len > 0 && len < sizeof(buf), "the setup page is generated");

  /* The page already spans more than one IOB (1514 bytes); vp_write_all()
   * loops until every byte is sent, so several TCP segments on a SoftAP link
   * are an efficiency cost rather than a correctness issue.  The bound exists
   * so a future addition that meaningfully bloats the page -- a stray debug
   * dump, an accidentally duplicated block -- is caught instead of silently
   * growing until it no longer fits the response buffer at all.
   *
   * Raised from 3600 with the endpoint boxes.  Worth knowing which way the
   * cost fell: a device with *nothing* stored is the larger page, not the
   * smaller one, because the box then spells out the factory default host and
   * path instead of a short stored value.
   */

  CHECK(len < 4608, "the setup response stays inside its buffer with room");
  CHECK(strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0,
        "the setup page carries a 200 status line");
  CHECK(strstr(buf, "Content-Length: ") != NULL,
        "the setup page declares its length");
  CHECK(strstr(buf, "name=\"ssid\"") != NULL &&
        strstr(buf, "name=\"password\"") != NULL &&
        strstr(buf, "name=\"no_password\"") != NULL &&
        strstr(buf, "name=\"mimo_apikey\"") != NULL &&
        strstr(buf, "name=\"volc_appid\"") != NULL &&
        strstr(buf, "name=\"volc_token\"") != NULL,
        "the form has Wi-Fi, the no-password box, MiMo and voice inputs");
  CHECK(strstr(buf, "action=\"/\"") != NULL &&
        strstr(buf, "method=\"post\"") != NULL,
        "the form posts back to the page it came from");
  CHECK(strstr(buf, "type=\"password\"") != NULL,
        "the passphrase field is masked in the browser");
  CHECK(strstr(buf, "设备还没有记住任何 Wi-Fi") != NULL,
        "an unconfigured device says so in plain words");

  /* Every user-visible string is ordinary Chinese.  A protocol reason phrase
   * or an errno on this page would be unreadable to the person it is for, so
   * their absence is asserted rather than assumed.
   */

  CHECK(strstr(buf, "SSID") == NULL && strstr(buf, "PSK") == NULL &&
        strstr(buf, "开放网络") == NULL && strstr(buf, "配网") == NULL &&
        strstr(buf, "errno") == NULL,
        "the setup page uses no jargon for the Wi-Fi fields");

  /* The key fields keep their product names, which is the one place a proper
   * noun is the clearest thing to write.
   */

  CHECK(strstr(buf, "MiMo API key") != NULL &&
        strstr(buf, "语音 App ID") != NULL &&
        strstr(buf, "语音 Token") != NULL,
        "the key fields are still named the way the consoles name them");

  /* Help text, not just labels: "leave blank to keep it" is the whole reason
   * an empty box is safe now, so the page has to say it.
   */

  CHECK(strstr(buf, "留空表示不改动") != NULL,
        "the page explains that a blank box changes nothing");

  len = vp_http_setup_page(buf, sizeof(buf), NULL, "请填写 Wi-Fi 名称。");
  CHECK(len > 0 && strstr(buf, "请填写 Wi-Fi 名称。") != NULL,
        "a notice is rendered on the setup page");
  CHECK(strncmp(buf, "HTTP/1.1 400 ", 13) == 0,
        "a setup page carrying a problem answers 400");
  CHECK(strstr(buf, "class=\"err\">") != NULL,
        "the problem notice is styled as a problem");
  CHECK(strstr(buf, "name=\"ssid\"") != NULL,
        "a refused submit still gets the whole form back");

  configured_state(&state);
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(len > 0 && strstr(buf, "AIPC") != NULL,
        "the setup page names the stored network");
  CHECK(strstr(buf, "value=\"AIPC\"") != NULL,
        "the stored network name is pre-filled so it need not be retyped");

  /* The box and the input answer different questions.  On a refused submit the
   * box still reports what the device remembers while the input carries what
   * the user typed, so fixing one field cannot silently revert another.
   */

  state.form_ssid = "JustTyped";
  len = vp_http_setup_page(buf, sizeof(buf), &state, "请填写 Wi-Fi 密码。");
  CHECK(len > 0 && strstr(buf, "value=\"JustTyped\"") != NULL &&
        strstr(buf, "Wi-Fi 名称：AIPC") != NULL,
        "the input keeps what was typed while the box reports what is stored");
  CHECK(strstr(buf, "value=\"AIPC\"") == NULL,
        "the stored name does not overwrite what was typed");
  state.form_ssid = NULL;
  CHECK(strstr(buf, "已经保存过 3 次") != NULL,
        "the save count is shown in plain words, not as a generation");
  CHECK(count_occurrences(buf, "已填写") == 2 &&
        count_occurrences(buf, "还没填写") == 1,
        "each key field reports only whether it is set");
  CHECK(strstr(buf, "Wi-Fi 密码：已设置") != NULL,
        "a stored passphrase is reported as set, never shown");

  state.open_network = true;
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(len > 0 && strstr(buf, "这个 Wi-Fi 没有密码") != NULL,
        "a stored network with no password says so");

  configured_state(&state);
  len = vp_http_saved_page(buf, sizeof(buf), &state,
                           VP_FORM_CHANGED_PASSWORD |
                           VP_FORM_CHANGED_VOLC_TOKEN);
  CHECK(len > 0 && strstr(buf, "AIPC") != NULL &&
        strstr(buf, "HTTP/1.1 200 OK") != NULL,
        "the saved page shows the network that was saved");
  CHECK(strstr(buf, "class=\"ok\">已保存") != NULL,
        "the saved page shows a green success notice");
  CHECK(strstr(buf, "这次改动了：Wi-Fi 密码、语音 Token。") != NULL,
        "the saved page names the fields it changed, in order");
  CHECK(strstr(buf, "MiMo API key。") == NULL,
        "a field that did not move is not listed as changed");
  CHECK(strstr(buf, "name=\"password\"") != NULL &&
        strstr(buf, "href=\"/\"") == NULL,
        "saving stays on the input form without a return link");
  CHECK(len < 4608, "the saved response stays inside its buffer with room");

  len = vp_http_saved_page(buf, sizeof(buf), &state, 0);
  CHECK(len > 0 && strstr(buf, "这次没有改动任何设置。") != NULL,
        "a submit that changed nothing says so instead of implying a change");

  /* Saving a network without a password is either exactly what was meant or
   * the one mistake that leaves the device unable to connect with no hint
   * why, so the page has to say it out loud.
   */

  state.open_network = true;
  len = vp_http_saved_page(buf, sizeof(buf), &state,
                           VP_FORM_CHANGED_PASSWORD);
  CHECK(len > 0 && strstr(buf, "class=\"warn\">注意") != NULL &&
        strstr(buf, "记成了没有密码") != NULL,
        "saving a network with no password raises an explicit warning");

  configured_state(&state);
  len = vp_http_saved_page(buf, sizeof(buf), &state, 0);
  CHECK(len > 0 && strstr(buf, "class=\"warn\">") == NULL,
        "a network with a password raises no warning");

  configured_state(&state);
  state.ssid = "<script>";
  len = vp_http_saved_page(buf, sizeof(buf), &state, 0);
  CHECK(len > 0 && strstr(buf, "<script>") == NULL &&
        strstr(buf, "&lt;script&gt;") != NULL,
        "a network name cannot inject markup into the success page");

  /* The endpoint is the one stored value rendered in full rather than as a
   * boolean, so both places it appears have to be checked: the
   * stored-settings box and the input's pre-filled value attribute.  Without
   * the second one, clearing a custom endpoint from the phone would be
   * impossible -- an empty box would look the same as an unsubmitted one.
   */

  configured_state(&state);
  state.cloud_host = "10.192.225.223";
  state.cloud_path = "/mock";
  state.cloud_port = 18080;
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(len > 0 && strstr(buf, "社交云地址：10.192.225.223") != NULL &&
        strstr(buf, "端口：18080") != NULL &&
        strstr(buf, "路径前缀：/mock") != NULL,
        "the stored endpoint is shown in full, not as a boolean");
  CHECK(strstr(buf, "value=\"10.192.225.223\"") != NULL &&
        strstr(buf, "value=\"18080\"") != NULL &&
        strstr(buf, "value=\"/mock\"") != NULL,
        "the endpoint boxes are pre-filled so clearing them is possible");
  CHECK(strstr(buf, "name=\"cloud_host\"") != NULL &&
        strstr(buf, "name=\"cloud_port\"") != NULL &&
        strstr(buf, "name=\"cloud_path\"") != NULL,
        "the form carries the three endpoint inputs");

  /* Nothing stored has to read as "the default applies" rather than as a
   * missing setting, and the placeholder has to name the default the firmware
   * would actually use -- the page and vs_cloud_init() share one macro for
   * exactly this reason.
   */

  configured_state(&state);
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(len > 0 &&
        strstr(buf, "社交云地址：默认 "
                    VELASIGHT_PROV_CLOUD_HOST_DEFAULT) != NULL &&
        strstr(buf, "路径前缀：默认 "
                    VELASIGHT_PROV_CLOUD_PATH_DEFAULT) != NULL,
        "an unset endpoint is reported as the factory default, named");
  CHECK(strstr(buf, "placeholder=\"" VELASIGHT_PROV_CLOUD_HOST_DEFAULT "\"")
        != NULL,
        "the address box offers the factory default as placeholder text");
  CHECK(strstr(buf, "value=\"10.192.225.223\"") == NULL,
        "an unset endpoint leaves the boxes empty rather than pre-filled");

  /* The largest page the builders can be asked for.  Every variable part is
   * at its maximum at once: a 32-byte network name made entirely of markup
   * characters, which sextuples when escaped, the longest problem notice,
   * every field name listed as changed, a full-length endpoint host and path
   * rendered twice each, the no-password warning and the history link.
   * Getting the buffer wrong makes a builder return zero and the phone
   * receive an empty response, so the bound is tested rather than measured
   * once and trusted.
   */

  {
    static const char worst_ssid[] =
        "\"'<&>\"'<&>\"'<&>\"'<&>\"'<&>\"'<&>ab";
    static const char worst_notice[] =
        "Wi-Fi 密码需要 8 到 63 个字符；想保留原来的密码请把这一栏留空；"
        "这个 Wi-Fi 确实没有密码请勾选下面那一项。";
    char worst_host[VELASIGHT_PROV_CLOUD_HOST_MAX + 1];
    char worst_path[VELASIGHT_PROV_CLOUD_PATH_MAX + 1];
    size_t worst_setup;
    size_t worst_saved;

    /* Legal at full length: vp_cloud_host_ok() allows only letters, digits,
     * '-' and '.', so this is the longest host that can actually be stored.
     * Using markup characters here instead would test a value the validator
     * rejects, and would size the buffer for a page that cannot exist.
     */

    memset(worst_host, 'a', sizeof(worst_host) - 1);
    worst_host[sizeof(worst_host) - 1] = '\0';
    worst_path[0] = '/';
    memset(worst_path + 1, 'b', sizeof(worst_path) - 2);
    worst_path[sizeof(worst_path) - 1] = '\0';

    configured_state(&state);
    state.ssid            = worst_ssid;
    state.have_volc_token = true;
    state.open_network    = true;
    state.generation      = 4294967295u;
    state.history_enabled = true;
    state.cloud_host      = worst_host;
    state.cloud_path      = worst_path;
    state.cloud_port      = 65535;

    worst_setup = vp_http_setup_page(buf, sizeof(buf), &state, worst_notice);
    CHECK(worst_setup > 0 && strstr(buf, "&quot;&#39;&lt;&amp;&gt;") != NULL,
          "the worst-case setup page is built, not silently refused");

    worst_saved = vp_http_saved_page(buf, sizeof(buf), &state, 0xffffffffu);
    CHECK(worst_saved > 0,
          "the worst-case confirmation page is built, not silently refused");
    CHECK(worst_saved < VP_HTTP_RESPONSE_MAX - 512,
          "the worst-case page leaves room for further copy edits");
  }

  len = vp_http_status_page(buf, sizeof(buf), 413, "内容太多了");
  CHECK(len > 0 && strncmp(buf, "HTTP/1.1 413 ", 13) == 0,
        "a status page uses the requested status");

  len = vp_http_status_page(buf, sizeof(buf), 403, "被拒绝了");
  CHECK(len > 0 &&
        strncmp(buf, "HTTP/1.1 403 Forbidden\r\n", 24) == 0,
        "a 403 status line keeps the protocol reason phrase");

  /* The number and the English belong on the wire; the heading belongs to
   * whoever is holding the phone.
   */

  CHECK(strstr(buf, "<h1>这次提交被拒绝了</h1>") != NULL &&
        strstr(buf, "<h1>403") == NULL &&
        strstr(buf, "Forbidden</h1>") == NULL,
        "the status page heading is plain Chinese, not the reason phrase");
  CHECK(strstr(buf, "返回设置页") != NULL,
        "the status page offers a way back in plain words");
  CHECK(vp_http_status_page(buf, 8, 400, "bad") == 0,
        "a status page refuses a buffer it cannot fill");
}

static void test_history_pages(void)
{
  struct velasight_prov_history_entry_s entry;
  struct vp_http_state_s state;
  char buf[VP_HTTP_RESPONSE_MAX];
  size_t len;

  CHECK(vp_http_history_key_valid("R0000000"),
        "the canonical history key is valid");
  CHECK(!vp_http_history_key_valid("R00000000") &&
        !vp_http_history_key_valid("r0000000") &&
        !vp_http_history_key_valid("R00000/0"),
        "non-canonical history keys are invalid");

  configured_state(&state);
  state.history_enabled = true;
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(len > 0 &&
        strstr(buf, "class=\"tab on\" href=\"/\">联网设置") != NULL &&
        strstr(buf, "class=\"tab\" href=\"/history\">聊天记录") != NULL,
        "the setup page carries the tab bar with its own tab marked");
  CHECK(strstr(buf, "</main><nav") != NULL,
        "the tab bar sits outside the card it is pinned in front of");

  /* The confirmation is the same page, so it keeps the same bar.  Losing the
   * way to the history vanished for exactly as long as the user was looking at
   * the page that told them the save had worked.
   */

  len = vp_http_saved_page(buf, sizeof(buf), &state, 0);
  CHECK(len > 0 &&
        strstr(buf, "class=\"tab on\" href=\"/\">联网设置") != NULL &&
        strstr(buf, "href=\"/history\"") != NULL,
        "the confirmation keeps the tab bar, still on the setup tab");

  state.history_enabled = false;
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(len > 0 && strstr(buf, "href=\"/history\"") == NULL &&
        strstr(buf, "<nav") == NULL,
        "without a provider there is no bar rather than a one-item bar");

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
  CHECK(strstr(buf, "查看内容") != NULL &&
        strstr(buf, "下载文件") != NULL && strstr(buf, "JSON") == NULL,
        "the entry links are labelled without naming a file format");
  CHECK(strstr(buf, "这条记录不完整") != NULL,
        "an incomplete record says so in a full sentence");
  /* The tab bar is how you leave this page, and it has to say which of the two
   * pages you are currently on -- a bar where neither tab is marked is a bar
   * that tells you nothing about where you are.
   */

  CHECK(vp_http_history_tail_fragment(buf, sizeof(buf)) > 0 &&
        strstr(buf, "class=\"tab\" href=\"/\">联网设置") != NULL &&
        strstr(buf, "class=\"tab on\" href=\"/history\">聊天记录") != NULL,
        "the history page carries the tab bar with its own tab marked");
  CHECK(strstr(buf, "</main><nav") != NULL,
        "the tab bar sits outside the card it is pinned in front of");

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
  struct vp_http_state_s state;
  char buf[VP_HTTP_RESPONSE_MAX];
  size_t len;

  /* The builders now take a summary rather than a record, so a secret cannot
   * reach them at all except through the one string that is still a string:
   * the network name.  The assertion stays blunt on purpose.
   */

  configured_state(&state);
  len = vp_http_saved_page(buf, sizeof(buf), &state, 0xffffffffu);
  CHECK(len > 0 && strstr(buf, secret) == NULL,
        "the success page contains no passphrase");

  len = vp_http_setup_page(buf, sizeof(buf), NULL, NULL);
  CHECK(len > 0 && strstr(buf, secret) == NULL,
        "the setup page contains no passphrase");

  /* Nothing is pre-filled except the network name, and with no record there
   * is not even that.  The checkbox's own value="on" is the only attribute of
   * that shape the page is allowed to carry.
   */

  CHECK(count_occurrences(buf, "value=\"") == 1 &&
        strstr(buf, "value=\"on\"") != NULL,
        "an unconfigured setup page pre-fills nothing but the checkbox");

  configured_state(&state);
  state.ssid = secret;
  len = vp_http_setup_page(buf, sizeof(buf), &state, NULL);
  CHECK(count_occurrences(buf, "value=\"") == 2,
        "a configured setup page pre-fills only the network name");

  len = vp_http_status_page(buf, sizeof(buf), 400, "填写的内容有问题");
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
