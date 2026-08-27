/****************************************************************************
 * app/provisioning_web/vp_http.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "velasight_provisioning.h"
#include "vp_form.h"
#include "vp_http.h"

/* Sized for the setup page, which is the largest body built in one piece.
 * It lives on the listener stack rather than in .bss, so VP_THREAD_STACK in
 * vp_server.c has to stay ahead of it.
 */

#define VP_HTTP_BODY_BUILD 4608

static const char g_form_type[] = "application/x-www-form-urlencoded";

/* The reason phrase belongs on the wire, where the numbers and the English
 * are part of the protocol.  It is deliberately not what the page says: see
 * vp_plain_title().
 */

static const char *vp_reason(int status)
{
  switch (status)
    {
      case 200: return "OK";
      case 400: return "Bad Request";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 411: return "Length Required";
      case 413: return "Payload Too Large";
      case 415: return "Unsupported Media Type";
      case 431: return "Request Header Fields Too Large";
      case 500: return "Internal Server Error";
      case 503: return "Service Unavailable";
      default:  return "Error";
    }
}

/* What the person holding the phone sees instead.
 *
 * "405 Method Not Allowed" tells someone reading a protocol trace exactly
 * what happened and tells the owner of a pair of glasses nothing at all.  The
 * status code stays in the response line for anything that inspects it; the
 * heading is written for the reader.
 */

static const char *vp_plain_title(int status)
{
  switch (status)
    {
      case 400: return "填写的内容有问题";
      case 403: return "这次提交被拒绝了";
      case 404: return "找不到这个页面";
      case 405: return "不支持这样的操作";
      case 411: return "提交的内容不完整";
      case 413: return "提交的内容太多了";
      case 415: return "提交的格式不对";
      case 431: return "浏览器发来的内容太长了";
      case 500: return "设备内部出错了";
      case 503: return "设备暂时忙不过来";
      default:  return "出错了";
    }
}

static bool vp_ci_prefix(const char *line, size_t linelen, const char *name)
{
  size_t namelen = strlen(name);
  size_t i;

  if (linelen < namelen)
    {
      return false;
    }

  for (i = 0; i < namelen; i++)
    {
      char a = line[i];
      char b = name[i];

      if (a >= 'A' && a <= 'Z')
        {
          a = (char)(a - 'A' + 'a');
        }

      if (b >= 'A' && b <= 'Z')
        {
          b = (char)(b - 'A' + 'a');
        }

      if (a != b)
        {
          return false;
        }
    }

  return true;
}

static const char *vp_header_value(const char *line, size_t linelen,
                                   const char *name, size_t *valuelen)
{
  size_t namelen = strlen(name);
  size_t i;

  if (!vp_ci_prefix(line, linelen, name))
    {
      return NULL;
    }

  i = namelen;
  if (i >= linelen || line[i] != ':')
    {
      return NULL;
    }

  i++;
  while (i < linelen && (line[i] == ' ' || line[i] == '\t'))
    {
      i++;
    }

  *valuelen = linelen - i;
  return line + i;
}

static int vp_reject(struct vp_http_request_s *req, int status)
{
  req->action = VP_HTTP_ACTION_REJECT;
  req->status = status;
  return 0;
}

bool vp_http_history_key_valid(const char *key)
{
  size_t i;

  if (key == NULL || strlen(key) != VELASIGHT_PROV_HISTORY_KEY_MAX ||
      key[0] != 'R')
    {
      return false;
    }

  for (i = 1; i < VELASIGHT_PROV_HISTORY_KEY_MAX; i++)
    {
      if (key[i] < '0' || key[i] > '9')
        {
          return false;
        }
    }

  return true;
}

int vp_http_parse(const char *buf, size_t len,
                  struct vp_http_request_s *req)
{
  const char *end;
  const char *line;
  const char *sp1;
  const char *sp2;
  size_t linelen;
  size_t targetlen;
  size_t remaining;
  size_t headers_len;
  bool is_post;
  bool have_length = false;
  bool form_type = false;
  size_t content_length = 0;

  if (buf == NULL || req == NULL)
    {
      return -EINVAL;
    }

  memset(req, 0, sizeof(*req));
  req->status = 400;

  /* No CRLFCRLF yet means either "keep reading" or "this client is not going
   * to stop", and the cap is what tells them apart.
   */

  end = NULL;
  if (len >= 4)
    {
      size_t i;

      for (i = 0; i + 4 <= len; i++)
        {
          if (memcmp(buf + i, "\r\n\r\n", 4) == 0)
            {
              end = buf + i;
              break;
            }
        }
    }

  if (end == NULL)
    {
      if (len >= VP_HTTP_MAX_HEADERS)
        {
          return vp_reject(req, 431);
        }

      return -EAGAIN;
    }

  headers_len = (size_t)(end - buf) + 4;
  if (headers_len > VP_HTTP_MAX_HEADERS)
    {
      return vp_reject(req, 431);
    }

  req->header_len = headers_len;

  /* Request line */

  line = buf;
  {
    const char *crlf = memchr(buf, '\r', (size_t)(end - buf) + 1);

    if (crlf == NULL)
      {
        return vp_reject(req, 400);
      }

    linelen = (size_t)(crlf - buf);
  }

  sp1 = memchr(line, ' ', linelen);
  if (sp1 == NULL)
    {
      return vp_reject(req, 400);
    }

  remaining = linelen - (size_t)(sp1 + 1 - line);
  sp2 = memchr(sp1 + 1, ' ', remaining);
  targetlen = sp2 != NULL ? (size_t)(sp2 - (sp1 + 1)) : remaining;
  if (targetlen == 0)
    {
      return vp_reject(req, 400);
    }

  if ((size_t)(sp1 - line) == 3 && memcmp(line, "GET", 3) == 0)
    {
      is_post = false;
    }
  else if ((size_t)(sp1 - line) == 4 && memcmp(line, "POST", 4) == 0)
    {
      is_post = true;
    }
  else
    {
      return vp_reject(req, 405);
    }

  /* The query string is not used; stripping it keeps "/" and "/?x=1" from
   * being two different paths.
   */

  {
    const char *target = sp1 + 1;
    const char *query = memchr(target, '?', targetlen);
    size_t pathlen = query != NULL ? (size_t)(query - target) : targetlen;

    if (!is_post)
      {
        bool root = pathlen == 1 && target[0] == '/';
        bool index = pathlen == 11 &&
                     memcmp(target, "/index.html", 11) == 0;

        if (root || index)
          {
            req->action = VP_HTTP_ACTION_PAGE;
            return 0;
          }

        if (pathlen == 8 && memcmp(target, "/history", 8) == 0)
          {
            req->action = VP_HTTP_ACTION_HISTORY_LIST;
            return 0;
          }

        if ((pathlen == 17 || pathlen == 26) &&
            memcmp(target, "/history/", 9) == 0)
          {
            memcpy(req->record_key, target + 9,
                   VELASIGHT_PROV_HISTORY_KEY_MAX);
            req->record_key[VELASIGHT_PROV_HISTORY_KEY_MAX] = '\0';
            if (!vp_http_history_key_valid(req->record_key))
              {
                return vp_reject(req, 404);
              }

            if (pathlen == 17)
              {
                req->action = VP_HTTP_ACTION_HISTORY_JSON;
                return 0;
              }

            if (memcmp(target + 17, "/download", 9) == 0)
              {
                req->action = VP_HTTP_ACTION_HISTORY_DOWNLOAD;
                return 0;
              }
          }

        return vp_reject(req, 404);
      }

    /* The submit posts back to the page it came from rather than to a
     * separate endpoint.  There is no /save: a form that posted elsewhere left
     * the phone sitting on a URL that only existed as the answer to one
     * submit, so the address bar no longer matched the page and the tab bar
     * pointed away from where the user actually was.  Posting to "/" keeps the
     * whole service at one address.
     */

    {
      bool root = pathlen == 1 && target[0] == '/';
      bool index = pathlen == 11 && memcmp(target, "/index.html", 11) == 0;

      if (!root && !index)
        {
          return vp_reject(req, 404);
        }
    }
  }

  /* Headers.  Only three matter, and anything unexpected about them is a
   * refusal rather than a default, because this listener has no authentication
   * standing between it and the air.
   */

  {
    const char *cursor = buf + linelen + 2;

    while (cursor < end)
      {
        const char *crlf = memchr(cursor, '\r', (size_t)(end - cursor) + 1);
        const char *value;
        size_t valuelen;
        size_t thislen;

        if (crlf == NULL)
          {
            break;
          }

        thislen = (size_t)(crlf - cursor);
        if (thislen == 0)
          {
            break;
          }

        if (vp_header_value(cursor, thislen, "transfer-encoding",
                            &valuelen) != NULL)
          {
            return vp_reject(req, 400);
          }

        value = vp_header_value(cursor, thislen, "sec-fetch-site",
                                &valuelen);
        if (value != NULL)
          {
            while (valuelen > 0 &&
                   (value[valuelen - 1u] == ' ' ||
                    value[valuelen - 1u] == '\t'))
              {
                valuelen--;
              }

            /* Fetch Metadata is not authentication, but an explicit browser
             * cross-site navigation must never be allowed to mutate trusted-
             * LAN settings.  Missing headers remain accepted for captive
             * portals and older clients. */
            if (valuelen == sizeof("cross-site") - 1u &&
                vp_ci_prefix(value, valuelen, "cross-site"))
              {
                return vp_reject(req, 403);
              }
          }

        value = vp_header_value(cursor, thislen, "content-type", &valuelen);
        if (value != NULL)
          {
            form_type = valuelen >= strlen(g_form_type) &&
                        vp_ci_prefix(value, valuelen, g_form_type);
          }

        value = vp_header_value(cursor, thislen, "content-length", &valuelen);
        if (value != NULL)
          {
            size_t i;

            have_length = true;
            content_length = 0;
            if (valuelen == 0)
              {
                return vp_reject(req, 400);
              }

            for (i = 0; i < valuelen; i++)
              {
                if (value[i] < '0' || value[i] > '9')
                  {
                    return vp_reject(req, 400);
                  }

                if (content_length > (VP_HTTP_MAX_BODY + 1) / 10 + 1)
                  {
                    return vp_reject(req, 413);
                  }

                content_length = content_length * 10 +
                                 (size_t)(value[i] - '0');
              }
          }

        cursor = crlf + 2;
      }
  }

  if (!form_type)
    {
      return vp_reject(req, 415);
    }

  if (!have_length)
    {
      return vp_reject(req, 411);
    }

  if (content_length > VP_HTTP_MAX_BODY)
    {
      return vp_reject(req, 413);
    }

  if (content_length == 0)
    {
      return vp_reject(req, 400);
    }

  req->action = VP_HTTP_ACTION_SAVE;
  req->status = 200;
  req->content_length = content_length;
  return 0;
}

int vp_html_escape(const char *in, char *out, size_t outlen)
{
  size_t o = 0;
  size_t i;

  if (in == NULL || out == NULL || outlen == 0)
    {
      return -EINVAL;
    }

  for (i = 0; in[i] != '\0'; i++)
    {
      const char *rep;

      switch (in[i])
        {
          case '&':  rep = "&amp;";  break;
          case '<':  rep = "&lt;";   break;
          case '>':  rep = "&gt;";   break;
          case '"':  rep = "&quot;"; break;
          case '\'': rep = "&#39;";  break;
          default:   rep = NULL;     break;
        }

      if (rep != NULL)
        {
          size_t replen = strlen(rep);

          if (o + replen + 1 > outlen)
            {
              return -E2BIG;
            }

          memcpy(out + o, rep, replen);
          o += replen;
        }
      else
        {
          if (o + 2 > outlen)
            {
              return -E2BIG;
            }

          out[o++] = in[i];
        }
    }

  out[o] = '\0';
  return (int)o;
}

static size_t vp_wrap(char *buf, size_t buflen, int status,
                      const char *body, size_t bodylen)
{
  int header;

  header = snprintf(buf, buflen,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: %u\r\n"
                    "Cache-Control: no-store\r\n"
                    "%s"
                    "Connection: close\r\n"
                    "\r\n",
                    status, vp_reason(status), (unsigned)bodylen,
                    status == 405 ? "Allow: GET, POST\r\n" : "");

  if (header < 0 || (size_t)header + bodylen + 1 > buflen)
    {
      return 0;
    }

  memcpy(buf + header, body, bodylen);
  buf[(size_t)header + bodylen] = '\0';
  return (size_t)header + bodylen;
}

/* One stylesheet, no scripts and no external references: the phone that opens
 * this page has no route to the internet yet, so anything not inlined would
 * simply be a broken link.
 */

#define VP_PAGE_HEAD                                                   \
  "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"  \
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" \
  "<title>VelaSight 联网设置</title><style>"                            \
  "body{font-family:system-ui;margin:0;padding:20px 20px 76px;"          \
  "background:#f5f5f7}"                                                  \
  "main{max-width:22rem;margin:auto;background:#fff;padding:20px;"       \
  "border-radius:12px}h1{font-size:1.25rem;margin:0 0 12px}"            \
  "label{display:block;margin:14px 0 4px;font-weight:600}"              \
  "input,button{width:100%%;"                                            \
  "box-sizing:border-box;padding:10px;font-size:1rem;border-radius:8px}" \
  "input{border:1px solid #bbb}button{margin-top:20px;border:0;"         \
  "background:#0676df;color:#fff}.note{font-size:.85rem;color:#666;"    \
  "margin:4px 0 0}.err{color:#b00020;font-weight:600}"                  \
  ".ok{color:#16833b;font-weight:600}.warn{color:#a35200;font-weight:600}" \
  ".box{background:#f1f3f5;border-radius:8px;padding:12px;margin:0}"    \
  ".box p{margin:2px 0}"                                                 \
  ".chk{display:flex;align-items:center;gap:8px;margin:12px 0 0;"        \
  "font-weight:400}.chk input{width:auto;margin:0}"                      \
  ".tabs{position:fixed;left:0;right:0;bottom:0;display:flex;"           \
  "background:#fff;border-top:1px solid #d8d8dc}"                        \
  ".tab{flex:1;text-align:center;padding:14px 0;font-size:.95rem;"       \
  "color:#666;text-decoration:none}"                                     \
  ".tab.on{color:#0676df;font-weight:600;box-shadow:inset 0 2px 0 #0676df}" \
  "</style></head><body><main>"

/* Split so the tab bar can sit between the card and the end of the document.
 * It is positioned against the viewport, so it must not be a child of the
 * card, whose own background and rounded corners would otherwise be the thing
 * a fixed element is measured against on some engines.
 */

#define VP_PAGE_MAIN_END "</main>"
#define VP_PAGE_END      "</body></html>"
#define VP_PAGE_TAIL     VP_PAGE_MAIN_END VP_PAGE_END

/* The bottom tab bar, and the only way between the two pages.
 *
 * It replaces a link that sat inside the setup page, which meant it was only
 * reachable from one of the two places a user could be.  A bar pinned to the
 * viewport is present on both pages at the same coordinates, so switching does
 * not depend on having scrolled to the right part of the right page.
 *
 * Both tabs are rendered only when a history provider is configured; a
 * one-item tab bar would be a control that does nothing.
 */

#define VP_PAGE_NAV(setup_on, history_on)                                \
  "<nav class=\"tabs\">"                                                 \
  "<a class=\"tab" setup_on "\" href=\"/\">联网设置</a>"                 \
  "<a class=\"tab" history_on "\" href=\"/history\">聊天记录</a>"        \
  "</nav>"

#define VP_PAGE_NAV_SETUP   VP_PAGE_NAV(" on", "")
#define VP_PAGE_NAV_HISTORY VP_PAGE_NAV("", " on")

/* The one place the input names are written down.  Both the setup page and
 * the confirmation page carry the same form, so a user who mistyped one field
 * never has to navigate back to fix it.
 */

#define VP_PAGE_FORM                                                     \
  "<form action=\"/\" method=\"post\">"                                  \
  "<label for=\"ssid\">Wi-Fi 名称</label>"                               \
  "<input id=\"ssid\" name=\"ssid\" type=\"text\" maxlength=\"32\" "     \
  "%sautocapitalize=\"off\" autocomplete=\"off\" required>"              \
  "<p class=\"note\">要和路由器上的名称完全一致，区分大小写。"           \
  "设备只能连 2.4G 的 Wi-Fi。</p>"                                       \
  "<label for=\"password\">Wi-Fi 密码</label>"                           \
  "<input id=\"password\" name=\"password\" type=\"password\" "          \
  "maxlength=\"63\" autocomplete=\"off\">"                               \
  "<p class=\"note\">留空表示不改动原来的密码。</p>"                     \
  "<label class=\"chk\"><input type=\"checkbox\" name=\"no_password\" "  \
  "value=\"on\">这个 Wi-Fi 没有密码</label>"                             \
  "<p class=\"note\">只有连不需要密码的 Wi-Fi 时才勾选，"                \
  "勾选时请把上面的密码栏留空。</p>"                                     \
  "<label for=\"mimo_apikey\">MiMo API key</label>"                      \
  "<input id=\"mimo_apikey\" name=\"mimo_apikey\" type=\"password\" "    \
  "maxlength=\"512\" autocomplete=\"off\">"                              \
  "<p class=\"note\">留空表示不改动。少了这一项，设备无法回答问题。</p>" \
  "<label for=\"volc_appid\">语音 App ID</label>"                        \
  "<input id=\"volc_appid\" name=\"volc_appid\" type=\"text\" "          \
  "maxlength=\"64\" autocomplete=\"off\" autocapitalize=\"off\">"        \
  "<label for=\"volc_token\">语音 Token</label>"                         \
  "<input id=\"volc_token\" name=\"volc_token\" type=\"password\" "      \
  "maxlength=\"128\" autocomplete=\"off\">"                              \
  "<p class=\"note\">这两项留空表示不改动，需要一起填写，"               \
  "设备靠它们听懂和说话。</p>"                                           \
  "<button type=\"submit\">保存</button>"                                \
  "</form>"                                                              \
  "<p class=\"note\">保存后在设备上长按返回键即可退出设置。</p>"

static const char *vp_filled(bool set)
{
  return set ? "已填写" : "还没填写";
}

/* The stored-settings summary.  Secrets appear only as "已填写"; the record is
 * never rendered, which is what makes echoing one impossible rather than
 * merely unintended.
 */

static int vp_state_box(char *out, size_t outlen,
                        const struct vp_http_state_s *state,
                        const char *escaped_ssid)
{
  if (state == NULL || !state->have_record)
    {
      return snprintf(out, outlen,
                      "<p class=\"note\">设备还没有记住任何 Wi-Fi，"
                      "请在下面填写后保存。</p>");
    }

  return snprintf(out, outlen,
                  "<div class=\"box\">"
                  "<p class=\"note\"><strong>设备现在记住的设置</strong></p>"
                  "<p class=\"note\">Wi-Fi 名称：%s</p>"
                  "<p class=\"note\">Wi-Fi 密码：%s</p>"
                  "<p class=\"note\">MiMo API key：%s</p>"
                  "<p class=\"note\">语音 App ID：%s</p>"
                  "<p class=\"note\">语音 Token：%s</p>"
                  "<p class=\"note\">已经保存过 %u 次</p>"
                  "</div>",
                  escaped_ssid[0] != '\0' ? escaped_ssid : "（空）",
                  state->open_network ? "这个 Wi-Fi 没有密码" : "已设置",
                  vp_filled(state->have_api_key),
                  vp_filled(state->have_volc_appid),
                  vp_filled(state->have_volc_token),
                  (unsigned int)state->generation);
}

static size_t vp_http_page(char *buf, size_t buflen,
                           const struct vp_http_state_s *state,
                           const char *notice, bool notice_ok,
                           const char *extra)
{
  char escaped[256];
  char escaped_ssid[VELASIGHT_PROV_SSID_MAX * 6 + 1];
  char escaped_form[VELASIGHT_PROV_SSID_MAX * 6 + 1];
  char ssid_attr[VELASIGHT_PROV_SSID_MAX * 6 + 16];
  char box[768];
  char body[VP_HTTP_BODY_BUILD];
  const char *form_ssid;
  int bodylen;
  int n;

  if (buf == NULL)
    {
      return 0;
    }

  escaped[0] = '\0';
  escaped_ssid[0] = '\0';
  escaped_form[0] = '\0';
  ssid_attr[0] = '\0';

  if (state != NULL && state->ssid != NULL && state->ssid[0] != '\0')
    {
      (void)vp_html_escape(state->ssid, escaped_ssid, sizeof(escaped_ssid));
    }

  /* The box reports what is stored; the input carries what should be typed
   * next.  They are the same thing on a plain GET and deliberately different
   * on a refused submit.
   */

  form_ssid = state == NULL ? NULL :
              state->form_ssid != NULL ? state->form_ssid : state->ssid;

  if (form_ssid != NULL && form_ssid[0] != '\0' &&
      vp_html_escape(form_ssid, escaped_form, sizeof(escaped_form)) >= 0)
    {
      snprintf(ssid_attr, sizeof(ssid_attr), "value=\"%s\" ", escaped_form);
    }

  if (notice != NULL && vp_html_escape(notice, escaped, sizeof(escaped)) < 0)
    {
      escaped[0] = '\0';
    }

  n = vp_state_box(box, sizeof(box), state, escaped_ssid);
  if (n < 0 || (size_t)n >= sizeof(box))
    {
      return 0;
    }

  bodylen = snprintf(body, sizeof(body),
                     VP_PAGE_HEAD
                     "<h1>联网设置</h1>"
                     "%s%s%s%s%s"
                     VP_PAGE_FORM
                     VP_PAGE_MAIN_END
                     "%s"
                     VP_PAGE_END,
                     box,
                     escaped[0] != '\0' ? notice_ok ? "<p class=\"ok\">" :
                                                      "<p class=\"err\">" : "",
                     escaped,
                     escaped[0] != '\0' ? "</p>" : "",
                     extra != NULL ? extra : "",
                     ssid_attr,
                     state != NULL && state->history_enabled ?
                       VP_PAGE_NAV_SETUP : "");

  if (bodylen < 0 || (size_t)bodylen >= sizeof(body))
    {
      return 0;
    }

  return vp_wrap(buf, buflen, notice_ok || notice == NULL ? 200 : 400,
                 body, (size_t)bodylen);
}

size_t vp_http_setup_page(char *buf, size_t buflen,
                          const struct vp_http_state_s *state,
                          const char *notice)
{
  return vp_http_page(buf, buflen, state, notice, false, NULL);
}

size_t vp_http_saved_page(char *buf, size_t buflen,
                          const struct vp_http_state_s *state,
                          unsigned int changed)
{
  static const struct
  {
    unsigned int bit;
    const char *name;
  }
  g_names[] =
  {
    { VP_FORM_CHANGED_SSID,       "Wi-Fi 名称"   },
    { VP_FORM_CHANGED_PASSWORD,   "Wi-Fi 密码"   },
    { VP_FORM_CHANGED_API_KEY,    "MiMo API key" },
    { VP_FORM_CHANGED_VOLC_APPID, "语音 App ID"  },
    { VP_FORM_CHANGED_VOLC_TOKEN, "语音 Token"   },
  };

  char names[128];
  char extra[512];
  size_t used = 0;
  size_t i;
  int n;

  names[0] = '\0';
  for (i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++)
    {
      if ((changed & g_names[i].bit) == 0)
        {
          continue;
        }

      n = snprintf(names + used, sizeof(names) - used, "%s%s",
                   used > 0 ? "、" : "", g_names[i].name);
      if (n < 0 || (size_t)n >= sizeof(names) - used)
        {
          break;
        }

      used += (size_t)n;
    }

  /* A network stored without a password is either exactly what was asked for
   * or the one mistake that leaves the device unable to connect and gives no
   * hint why, so it is called out rather than merely recorded.
   */

  n = snprintf(extra, sizeof(extra), "%s<p class=\"note\">%s%s%s</p>",
               state != NULL && state->open_network ?
                 "<p class=\"warn\">注意：这个 Wi-Fi 记成了没有密码。"
                 "如果它其实需要密码，请在下面重新填写再保存一次。</p>" : "",
               used > 0 ? "这次改动了：" : "这次没有改动任何设置。",
               used > 0 ? names : "",
               used > 0 ? "。" : "");
  if (n < 0 || (size_t)n >= sizeof(extra))
    {
      extra[0] = '\0';
    }

  return vp_http_page(buf, buflen, state, "已保存", true, extra);
}

size_t vp_http_status_page(char *buf, size_t buflen, int status,
                           const char *message)
{
  char escaped[256];
  char body[VP_HTTP_BODY_BUILD];
  int bodylen;

  if (buf == NULL)
    {
      return 0;
    }

  escaped[0] = '\0';
  if (message != NULL && vp_html_escape(message, escaped,
                                        sizeof(escaped)) < 0)
    {
      escaped[0] = '\0';
    }

  bodylen = snprintf(body, sizeof(body),
                     VP_PAGE_HEAD
                     "<h1>%s</h1><p>%s</p>"
                     "<p class=\"note\"><a href=\"/\">返回设置页</a></p>"
                     VP_PAGE_TAIL,
                     vp_plain_title(status), escaped);

  if (bodylen < 0 || (size_t)bodylen >= sizeof(body))
    {
      return 0;
    }

  return vp_wrap(buf, buflen, status, body, (size_t)bodylen);
}

size_t vp_http_response_header(char *buf, size_t buflen, int status,
                               const char *content_type,
                               size_t content_length,
                               const char *disposition,
                               const char *record_key)
{
  int n;

  if (buf == NULL || buflen == 0 || content_type == NULL)
    {
      return 0;
    }

  if (disposition != NULL)
    {
      if ((strcmp(disposition, "inline") != 0 &&
           strcmp(disposition, "attachment") != 0) ||
          !vp_http_history_key_valid(record_key))
        {
          return 0;
        }

      n = snprintf(buf, buflen,
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Content-Disposition: %s; filename=\"%s.json\"\r\n"
                   "Cache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n",
                   status, vp_reason(status), content_type, content_length,
                   disposition, record_key);
    }
  else
    {
      n = snprintf(buf, buflen,
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n",
                   status, vp_reason(status), content_type, content_length);
    }

  return n < 0 || (size_t)n >= buflen ? 0 : (size_t)n;
}

size_t vp_http_history_head_fragment(char *buf, size_t buflen,
                                     unsigned int count)
{
  int n;

  if (buf == NULL || buflen == 0)
    {
      return 0;
    }

  n = snprintf(buf, buflen,
               VP_PAGE_HEAD
               "<h1>聊天记录</h1>"
               "<p class=\"note\">按时间从新到旧，共 %u 条。</p>"
               "%s",
               count,
               count == 0 ? "<p>设备上还没有聊天记录。</p>" : "");
  return n < 0 || (size_t)n >= buflen ? 0 : (size_t)n;
}

size_t vp_http_history_entry_fragment(
    char *buf, size_t buflen,
    const struct velasight_prov_history_entry_s *entry)
{
  char key[(VELASIGHT_PROV_HISTORY_KEY_MAX * 6) + 1];
  char date[(VELASIGHT_PROV_HISTORY_DATE_MAX * 6) + 1];
  char title[(VELASIGHT_PROV_HISTORY_TITLE_MAX * 6) + 1];
  char summary[(VELASIGHT_PROV_HISTORY_SUMMARY_MAX * 6) + 1];
  int n;

  if (buf == NULL || buflen == 0 || entry == NULL ||
      memchr(entry->record_key, '\0', sizeof(entry->record_key)) == NULL ||
      memchr(entry->date, '\0', sizeof(entry->date)) == NULL ||
      memchr(entry->title, '\0', sizeof(entry->title)) == NULL ||
      memchr(entry->summary, '\0', sizeof(entry->summary)) == NULL ||
      !vp_http_history_key_valid(entry->record_key) ||
      entry->calm > 100 || entry->happy > 100 || entry->tense > 100)
    {
      return 0;
    }

  if (vp_html_escape(entry->record_key, key, sizeof(key)) < 0 ||
      vp_html_escape(entry->date, date, sizeof(date)) < 0 ||
      vp_html_escape(entry->title, title, sizeof(title)) < 0 ||
      vp_html_escape(entry->summary, summary, sizeof(summary)) < 0)
    {
      return 0;
    }

  n = snprintf(buf, buflen,
               "<article style=\"border-top:1px solid #ddd;padding:14px 0\">"
               "<h2 style=\"font-size:1.05rem;margin:0 0 4px\">%s</h2>"
               "<p class=\"note\">%s · %s%s</p>"
               "<p style=\"white-space:pre-wrap\">%s</p>"
               "<p class=\"note\">平静 %u%% · 开心 %u%% · 紧张 %u%%</p>"
               "<p><a href=\"/history/%s\">查看内容</a> · "
               "<a href=\"/history/%s/download\">下载文件</a></p>"
               "</article>",
               title, date, key,
               entry->incomplete ? " · 这条记录不完整" : "",
               summary, (unsigned int)entry->calm,
               (unsigned int)entry->happy, (unsigned int)entry->tense,
               entry->record_key, entry->record_key);
  return n < 0 || (size_t)n >= buflen ? 0 : (size_t)n;
}

size_t vp_http_history_tail_fragment(char *buf, size_t buflen)
{
  int n;

  if (buf == NULL || buflen == 0)
    {
      return 0;
    }

  /* The tab bar is unconditional here: this page only exists when a provider
   * is configured, so the history tab always has somewhere to point.
   */

  n = snprintf(buf, buflen,
               VP_PAGE_MAIN_END VP_PAGE_NAV_HISTORY VP_PAGE_END);
  return n < 0 || (size_t)n >= buflen ? 0 : (size_t)n;
}
