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

#define VP_HTTP_BODY_BUILD 3072

static const char g_form_type[] = "application/x-www-form-urlencoded";

static const char *vp_reason(int status)
{
  switch (status)
    {
      case 200: return "OK";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 411: return "Length Required";
      case 413: return "Payload Too Large";
      case 415: return "Unsupported Media Type";
      case 431: return "Request Header Fields Too Large";
      case 500: return "Internal Server Error";
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

        if (!root && !index)
          {
            return vp_reject(req, 404);
          }

        req->action = VP_HTTP_ACTION_PAGE;
        return 0;
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
  "<!DOCTYPE html><html lang=\"zh-CN\"><head>"                         \
  "<meta charset=\"utf-8\">"                                           \
  "<meta name=\"viewport\" content=\"width=device-width,"               \
  "initial-scale=1\">"                                                 \
  "<title>VelaSight 配网</title><style>"                                \
  "body{font-family:system-ui,sans-serif;margin:0;padding:24px;"        \
  "background:#f5f5f7;color:#1d1d1f}"                                   \
  "main{max-width:22rem;margin:0 auto;background:#fff;border-radius:"   \
  "12px;padding:20px}"                                                  \
  "h1{font-size:1.25rem;margin:0 0 16px}"                               \
  "label{display:block;margin:12px 0 4px;font-size:.9rem}"              \
  "input{width:100%%;box-sizing:border-box;padding:10px;font-size:1rem;" \
  "border:1px solid #c7c7cc;border-radius:8px}"                         \
  "button{width:100%%;margin-top:20px;padding:12px;font-size:1rem;"      \
  "border:0;border-radius:8px;background:#0071e3;color:#fff}"           \
  "p.note{font-size:.85rem;color:#6e6e73;margin-top:16px}"              \
  "p.err{color:#b00020;font-size:.9rem;margin:0 0 8px}"                 \
  "</style></head><body><main>"

#define VP_PAGE_TAIL "</main></body></html>"

size_t vp_http_form_page(char *buf, size_t buflen, const char *notice)
{
  char escaped[256];
  char body[VP_HTTP_BODY_BUILD];
  int bodylen;

  if (buf == NULL)
    {
      return 0;
    }

  escaped[0] = '\0';
  if (notice != NULL && vp_html_escape(notice, escaped, sizeof(escaped)) < 0)
    {
      escaped[0] = '\0';
    }

  bodylen = snprintf(body, sizeof(body),
                     VP_PAGE_HEAD
                     "<h1>连接 Wi-Fi</h1>"
                     "%s%s%s"
                     "<form action=\"/save\" method=\"post\">"
                     "<label for=\"ssid\">网络名称（SSID）</label>"
                     "<input id=\"ssid\" name=\"ssid\" type=\"text\" "
                     "maxlength=\"32\" autocapitalize=\"off\" required>"
                     "<label for=\"password\">密码（开放网络留空）</label>"
                     "<input id=\"password\" name=\"password\" "
                     "type=\"password\" maxlength=\"63\">"
                     "<button type=\"submit\">保存</button>"
                     "</form>"
                     "<p class=\"note\">凭据保存在设备的持久存储中，"
                     "本页面不会切换 Wi-Fi 模式。</p>"
                     VP_PAGE_TAIL,
                     escaped[0] != '\0' ? "<p class=\"err\">" : "",
                     escaped,
                     escaped[0] != '\0' ? "</p>" : "");

  if (bodylen < 0)
    {
      return 0;
    }

  return vp_wrap(buf, buflen, 200, body, (size_t)bodylen);
}

size_t vp_http_saved_page(char *buf, size_t buflen, const char *ssid,
                          uint32_t generation, bool open_network)
{
  char escaped[VELASIGHT_PROV_SSID_MAX * 6 + 1];
  char body[VP_HTTP_BODY_BUILD];
  int bodylen;

  if (buf == NULL || ssid == NULL)
    {
      return 0;
    }

  if (vp_html_escape(ssid, escaped, sizeof(escaped)) < 0)
    {
      return 0;
    }

  bodylen = snprintf(body, sizeof(body),
                     VP_PAGE_HEAD
                     "<h1>已保存</h1>"
                     "<p>网络：<strong>%s</strong></p>"
                     "<p>加密：%s</p>"
                     "<p>第 %u 次保存</p>"
                     "<p class=\"note\">凭据已写入设备持久存储。"
                     "切换回普通 Wi-Fi 模式由应用完成，此热点可能随之断开。"
                     "</p>"
                     VP_PAGE_TAIL,
                     escaped, open_network ? "开放网络" : "有密码",
                     (unsigned)generation);

  if (bodylen < 0)
    {
      return 0;
    }

  return vp_wrap(buf, buflen, 200, body, (size_t)bodylen);
}

size_t vp_http_status_page(char *buf, size_t buflen, int status,
                           const char *message)
{
  char escaped[256];
  char body[1024];
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

  if (bodylen < 0)
    {
      return 0;
    }

  return vp_wrap(buf, buflen, status, body, (size_t)bodylen);
}
