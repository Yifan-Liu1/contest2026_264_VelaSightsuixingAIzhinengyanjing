/****************************************************************************
 * app/provisioning_web/vp_http.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "velasight_provisioning.h"
#include "vp_http.h"

#define VP_HTTP_BODY_BUILD 2048

static const char g_form_type[] = "application/x-www-form-urlencoded";

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

    if (pathlen != 5 || memcmp(target, "/save", 5) != 0)
      {
        return vp_reject(req, 404);
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
  "<title>VelaSight 配网</title><style>"                                \
  "body{font-family:system-ui;margin:0;padding:20px;background:#f5f5f7}" \
  "main{max-width:22rem;margin:auto;background:#fff;padding:20px;"       \
  "border-radius:12px}h1{font-size:1.25rem;margin:0 0 12px}"            \
  "label{display:block;margin:10px 0 4px}input,button{width:100%%;"       \
  "box-sizing:border-box;padding:10px;font-size:1rem;border-radius:8px}" \
  "input{border:1px solid #bbb}button{margin-top:18px;border:0;"         \
  "background:#0676df;color:#fff}.note{font-size:.85rem;color:#666}"    \
  ".err{color:#b00020}.ok{color:#16833b;font-weight:600}"               \
  "</style></head><body><main>"

#define VP_PAGE_TAIL "</main></body></html>"

static size_t vp_http_form_page_notice(char *buf, size_t buflen,
                                       const char *notice,
                                       const char *current_ssid,
                                       bool saved,
                                       bool history_enabled)
{
  char escaped[256];
  char escaped_ssid[VELASIGHT_PROV_SSID_MAX * 6 + 1];
  char ssid_attr[VELASIGHT_PROV_SSID_MAX * 6 + 16];
  char body[VP_HTTP_BODY_BUILD];
  int bodylen;

  if (buf == NULL)
    {
      return 0;
    }

  escaped[0] = '\0';
  escaped_ssid[0] = '\0';
  ssid_attr[0] = '\0';
  if (current_ssid != NULL)
    {
      if (vp_html_escape(current_ssid, escaped_ssid, sizeof(escaped_ssid)) >= 0)
        snprintf(ssid_attr, sizeof(ssid_attr), "value=\"%s\" ",
                 escaped_ssid);
    }
  if (notice != NULL && vp_html_escape(notice, escaped, sizeof(escaped)) < 0)
    {
      escaped[0] = '\0';
    }

  bodylen = snprintf(body, sizeof(body),
                     VP_PAGE_HEAD
                      "<h1>设备配网</h1>"
                      "<p class=\"note\">当前存储的 Wi-Fi：<strong>%s</strong></p>"
                      "%s%s%s"
                     "<form action=\"/save\" method=\"post\">"
                     "<label for=\"ssid\">网络名称（SSID）</label>"
                      "<input id=\"ssid\" name=\"ssid\" type=\"text\" "
                      "maxlength=\"32\" %sautocapitalize=\"off\" required>"
                     "<label for=\"password\">密码（开放网络留空）</label>"
                      "<input id=\"password\" name=\"password\" "
                      "type=\"password\" maxlength=\"63\">"
                       "<label for=\"mimo_apikey\">MiMo API key（可选）</label>"
                      "<input id=\"mimo_apikey\" name=\"mimo_apikey\" "
                      "type=\"password\" maxlength=\"512\" "
                      "autocomplete=\"off\">"
                       "<label for=\"volc_appid\">语音 App ID（可选）</label>"
                      "<input id=\"volc_appid\" name=\"volc_appid\" "
                      "type=\"text\" maxlength=\"64\" "
                      "autocomplete=\"off\" autocapitalize=\"off\">"
                       "<label for=\"volc_token\">语音 Token（可选）</label>"
                      "<input id=\"volc_token\" name=\"volc_token\" "
                      "type=\"password\" maxlength=\"128\" "
                      "autocomplete=\"off\">"
                     "<button type=\"submit\">保存</button>"
                     "</form>"
                      "<p class=\"note\">保存后可在设备上长按返回。</p>"
                      "%s"
                      VP_PAGE_TAIL,
                      escaped_ssid[0] != '\0' ? escaped_ssid : "未配置",
                      escaped[0] != '\0' ? saved ? "<p class=\"ok\">" :
                                                   "<p class=\"err\">" : "",
                      escaped,
                      escaped[0] != '\0' ? "</p>" : "",
                      ssid_attr,
                      history_enabled ?
                        "<p><a href=\"/history\">浏览社交历史记录</a></p>" :
                        "");

  if (bodylen < 0 || (size_t)bodylen >= sizeof(body))
    {
      return 0;
    }

  return vp_wrap(buf, buflen, 200, body, (size_t)bodylen);
}

size_t vp_http_form_page_with_ssid_history(char *buf, size_t buflen,
                                           const char *notice,
                                           const char *current_ssid,
                                           bool history_enabled)
{
  return vp_http_form_page_notice(buf, buflen, notice, current_ssid, false,
                                  history_enabled);
}

size_t vp_http_form_page_with_ssid(char *buf, size_t buflen,
                                   const char *notice,
                                   const char *current_ssid)
{
  return vp_http_form_page_with_ssid_history(buf, buflen, notice,
                                             current_ssid, false);
}

size_t vp_http_form_page(char *buf, size_t buflen, const char *notice)
{
  return vp_http_form_page_with_ssid(buf, buflen, notice, NULL);
}

size_t vp_http_saved_page(char *buf, size_t buflen, const char *ssid,
                          uint32_t generation, bool open_network)
{
  (void)generation;
  (void)open_network;
  return vp_http_form_page_notice(buf, buflen, "已保存", ssid, true, false);
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
                     "<h1>%d %s</h1><p>%s</p>"
                     "<p class=\"note\"><a href=\"/\">返回配网页面</a></p>"
                     VP_PAGE_TAIL,
                     status, vp_reason(status), escaped);

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
               "<h1>社交历史记录</h1>"
               "<p class=\"note\">按时间从新到旧，共 %u 条。</p>"
               "%s",
               count,
               count == 0 ? "<p>暂无可读取的社交历史记录。</p>" : "");
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
               "<p><a href=\"/history/%s\">预览 JSON</a> · "
               "<a href=\"/history/%s/download\">下载</a></p>"
               "</article>",
               title, date, key, entry->incomplete ? " · 未完整" : "",
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

  n = snprintf(buf, buflen,
               "<p class=\"note\"><a href=\"/\">返回配网页面</a></p>"
               VP_PAGE_TAIL);
  return n < 0 || (size_t)n >= buflen ? 0 : (size_t)n;
}
