/****************************************************************************
 * app/provisioning_web/vp_form.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "vp_form.h"

/* Long enough to tell "too long" from "just fits" for every field, including
 * the 512-byte API key: a value between its own limit and this cap is decoded
 * and then refused on length, instead of arriving silently truncated to
 * something legal that the phone never typed.
 */

#define VP_FORM_VALUE_MAX 640
#define VP_FORM_NAME_MAX  32

static int vp_hexval(char c)
{
  if (c >= '0' && c <= '9')
    {
      return c - '0';
    }

  if (c >= 'a' && c <= 'f')
    {
      return c - 'a' + 10;
    }

  if (c >= 'A' && c <= 'F')
    {
      return c - 'A' + 10;
    }

  return -1;
}

int vp_url_decode(const char *src, size_t srclen, char *out, size_t outlen)
{
  size_t i = 0;
  size_t o = 0;

  if (src == NULL || out == NULL || outlen == 0)
    {
      return -EINVAL;
    }

  while (i < srclen)
    {
      unsigned char byte;

      if (src[i] == '%')
        {
          int hi;
          int lo;

          if (srclen - i < 3)
            {
              return -EINVAL;
            }

          hi = vp_hexval(src[i + 1]);
          lo = vp_hexval(src[i + 2]);
          if (hi < 0 || lo < 0)
            {
              return -EINVAL;
            }

          byte = (unsigned char)((hi << 4) | lo);
          i += 3;
        }
      else if (src[i] == '+')
        {
          byte = (unsigned char)' ';
          i++;
        }
      else
        {
          byte = (unsigned char)src[i];
          i++;
        }

      /* %00 would otherwise cut the value short and leave the rest of it
       * unexamined, which is how a validated string stops matching the bytes
       * that were actually submitted.
       */

      if (byte == 0)
        {
          return -EINVAL;
        }

      if (o + 1 >= outlen)
        {
          return -E2BIG;
        }

      out[o++] = (char)byte;
    }

  out[o] = '\0';
  return (int)o;
}

static size_t vp_strnlen(const char *s, size_t maxlen)
{
  size_t n = 0;

  while (n < maxlen && s[n] != '\0')
    {
      n++;
    }

  return n;
}

static bool vp_ssid_char_ok(char c)
{
  unsigned char u = (unsigned char)c;

  /* Control characters only: an SSID is arbitrary bytes on the air, and a
   * UTF-8 name has to survive.
   */

  return u >= 0x20 && u != 0x7f;
}

static bool vp_psk_char_ok(char c)
{
  unsigned char u = (unsigned char)c;

  /* A WPA2 passphrase is printable ASCII.  Anything else could not have been
   * typed into the router either.
   */

  return u >= 0x20 && u <= 0x7e;
}

/* MiMo key, Volcengine app_id and Volcengine token share the same character
 * set (printable ASCII) and the same "empty means not configured yet"
 * treatment, so one helper checks all three instead of repeating the loop.
 */

static int vp_printable_field_validate(const char *field, size_t cap,
                                       size_t limit)
{
  size_t len = vp_strnlen(field, cap);
  size_t i;

  if (len > limit)
    {
      return -EINVAL;
    }

  for (i = 0; i < len; i++)
    {
      if ((unsigned char)field[i] < 0x20 || (unsigned char)field[i] > 0x7e)
        {
          return -EINVAL;
        }
    }

  return 0;
}

/* A DNS name or a dotted quad, with no scheme, no port and no path.
 *
 * Deliberately stricter than the printable-ASCII check the key fields use.
 * This value is interpolated into a Host header and into the target of a
 * connect(), so a space, a slash or a colon in it would either split the
 * header or silently change which host is reached.  Rejecting the character
 * outright is the only version of that check with no second interpretation.
 *
 * Empty is legal and means "use the compiled-in default".
 */

static bool vp_cloud_host_ok(const char *host)
{
  size_t len = vp_strnlen(host, VELASIGHT_PROV_CLOUD_HOST_MAX + 1);
  size_t i;

  if (len > VELASIGHT_PROV_CLOUD_HOST_MAX)
    {
      return false;
    }

  if (len == 0)
    {
      return true;
    }

  /* A leading or trailing dot is not a name any resolver accepts as written,
   * and is what a half-typed value looks like.
   */

  if (host[0] == '.' || host[0] == '-' || host[len - 1] == '.' ||
      host[len - 1] == '-')
    {
      return false;
    }

  for (i = 0; i < len; i++)
    {
      char c = host[i];

      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.')
        {
          continue;
        }

      return false;
    }

  return true;
}

/* The prefix that sits in front of /contest/v1.
 *
 * Empty is legal and means the endpoints are at the document root, which is
 * what the interface document's own examples assume.  A non-empty value must
 * start with '/' and must not end with one, so the consumer can concatenate
 * prefix + "/contest/v1/session" and get exactly one slash at the join
 * without testing for it.  Enforcing the shape here rather than normalising
 * it means the value stored is the value shown, and a user who typed a
 * trailing slash is told so instead of having it silently removed.
 */

static bool vp_cloud_path_ok(const char *path)
{
  size_t len = vp_strnlen(path, VELASIGHT_PROV_CLOUD_PATH_MAX + 1);
  size_t i;

  if (len > VELASIGHT_PROV_CLOUD_PATH_MAX)
    {
      return false;
    }

  if (len == 0)
    {
      return true;
    }

  if (path[0] != '/' || path[len - 1] == '/')
    {
      return false;
    }

  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)path[i];

      /* Unreserved URI characters plus '/' and '%', and nothing else.
       *
       * Two separate reasons to be this narrow.  '?', '#' and '\' would turn
       * a prefix into a query, a fragment or a Windows path.  '<', '>', '&',
       * '"' and '\'' are the five characters HTML escaping expands, and this
       * value is rendered twice per page -- once in the stored-settings box
       * and once as the input's value attribute -- so allowing them would
       * multiply the page's worst case by six for a character no path prefix
       * needs.  Percent-encoding covers anything genuinely required.
       */

      if (c == '/' || c == '%' || c == '-' || c == '_' || c == '.' ||
          c == '~' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9'))
        {
          /* Accepted. */
        }
      else
        {
          return false;
        }

      if (i > 0 && path[i] == '/' && path[i - 1] == '/')
        {
          return false;
        }
    }

  return true;
}

/* Strip ASCII whitespace from both ends, in place.
 *
 * Applied to the three key fields only.  Phone keyboards and clipboard
 * managers routinely add a trailing space or newline to a pasted token, and
 * the platform then answers 401 with nothing on the device to explain why.
 * The Wi-Fi password is deliberately left alone: a space is a legal WPA2
 * passphrase byte, so trimming it would break a working network to fix a
 * cosmetic problem.
 */

static void vp_trim_ascii_space(char *s)
{
  size_t len;
  size_t start = 0;

  if (s == NULL)
    {
      return;
    }

  len = strlen(s);
  while (len > start && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                         s[len - 1] == '\r' || s[len - 1] == '\n'))
    {
      len--;
    }

  while (start < len && (s[start] == ' ' || s[start] == '\t' ||
                         s[start] == '\r' || s[start] == '\n'))
    {
      start++;
    }

  if (start > 0)
    {
      memmove(s, s + start, len - start);
    }

  s[len - start] = '\0';
}

static int vp_fail(enum vp_form_field_e *which, enum vp_form_field_e field,
                   int err)
{
  if (which != NULL)
    {
      *which = field;
    }

  return err;
}

int vp_credentials_validate_detail(
    const struct velasight_prov_credentials_s *cred,
    enum vp_form_field_e *which)
{
  size_t len;
  size_t i;

  if (which != NULL)
    {
      *which = VP_FORM_FIELD_NONE;
    }

  if (cred == NULL)
    {
      return vp_fail(which, VP_FORM_FIELD_BODY, -EINVAL);
    }

  if (vp_printable_field_validate(cred->api_key, sizeof(cred->api_key),
                                  VELASIGHT_PROV_API_KEY_MAX) < 0)
    {
      return vp_fail(which, VP_FORM_FIELD_API_KEY, -EINVAL);
    }

  if (vp_printable_field_validate(cred->volc_appid,
                                  sizeof(cred->volc_appid),
                                  VELASIGHT_PROV_VOLC_APPID_MAX) < 0)
    {
      return vp_fail(which, VP_FORM_FIELD_VOLC_APPID, -EINVAL);
    }

  if (vp_printable_field_validate(cred->volc_token,
                                  sizeof(cred->volc_token),
                                  VELASIGHT_PROV_VOLC_TOKEN_MAX) < 0)
    {
      return vp_fail(which, VP_FORM_FIELD_VOLC_TOKEN, -EINVAL);
    }

  if (!vp_cloud_host_ok(cred->cloud_host))
    {
      return vp_fail(which, VP_FORM_FIELD_CLOUD_HOST, -EINVAL);
    }

  if (!vp_cloud_path_ok(cred->cloud_path))
    {
      return vp_fail(which, VP_FORM_FIELD_CLOUD_PATH, -EINVAL);
    }

  /* cloud_port needs no range check: it is a uint16_t, so every value it can
   * hold is a legal port or the 0 that means "use the default".  The parser
   * is where a number outside that range is refused, because that is where
   * the typed text still exists to be refused.
   */

  len = vp_strnlen(cred->ssid, sizeof(cred->ssid));
  if (len == 0 || len > VELASIGHT_PROV_SSID_MAX)
    {
      return vp_fail(which, VP_FORM_FIELD_SSID, -EINVAL);
    }

  for (i = 0; i < len; i++)
    {
      if (!vp_ssid_char_ok(cred->ssid[i]))
        {
          return vp_fail(which, VP_FORM_FIELD_SSID, -EINVAL);
        }
    }

  len = vp_strnlen(cred->password, sizeof(cred->password));
  if (len == 0)
    {
      /* The flag and the passphrase have to tell the same story, otherwise a
       * caller that trusts only one of them behaves differently from a caller
       * that trusts the other.
       */

      return cred->open_network ? 0 :
             vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
    }

  if (cred->open_network)
    {
      return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
    }

  if (len < VELASIGHT_PROV_PSK_MIN || len > VELASIGHT_PROV_PSK_MAX)
    {
      return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
    }

  for (i = 0; i < len; i++)
    {
      if (!vp_psk_char_ok(cred->password[i]))
        {
          return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
        }
    }

  return 0;
}

int vp_credentials_validate(
    const struct velasight_prov_credentials_s *cred)
{
  return vp_credentials_validate_detail(cred, NULL);
}

/* An HTML checkbox is absent when clear and carries "on" when set.  The
 * explicit "off"/"0"/"false" cases are for hand-written clients: a body that
 * spells out "not ticked" must not be read as ticked, because that would
 * clear a stored password.
 */

static bool vp_checkbox_set(const char *value)
{
  return value[0] != '\0' && strcmp(value, "off") != 0 &&
         strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

int vp_form_parse(const char *body, size_t bodylen,
                  struct vp_form_submit_s *submit,
                  enum vp_form_field_e *which)
{
  unsigned nfields = 0;
  bool have_open_flag = false;
  size_t pos = 0;

  if (which != NULL)
    {
      *which = VP_FORM_FIELD_NONE;
    }

  if (body == NULL || submit == NULL)
    {
      return vp_fail(which, VP_FORM_FIELD_BODY, -EINVAL);
    }

  memset(submit, 0, sizeof(*submit));

  while (pos < bodylen)
    {
      char name[VP_FORM_NAME_MAX + 1];
      char value[VP_FORM_VALUE_MAX];
      const char *sep;
      const char *amp;
      size_t fieldlen;
      size_t namelen;
      int decoded;

      amp = memchr(body + pos, '&', bodylen - pos);
      fieldlen = amp != NULL ? (size_t)(amp - (body + pos)) : bodylen - pos;

      if (++nfields > VP_FORM_MAX_FIELDS)
        {
          return vp_fail(which, VP_FORM_FIELD_BODY, -E2BIG);
        }

      if (fieldlen == 0)
        {
          pos += 1;
          continue;
        }

      sep = memchr(body + pos, '=', fieldlen);
      if (sep == NULL)
        {
          /* "ssid" without a value is not an empty SSID, it is a body this
           * code does not understand.
           */

          return vp_fail(which, VP_FORM_FIELD_BODY, -EINVAL);
        }

      namelen = (size_t)(sep - (body + pos));
      decoded = vp_url_decode(body + pos, namelen, name, sizeof(name));
      if (decoded == -EINVAL)
        {
          return vp_fail(which, VP_FORM_FIELD_BODY, -EINVAL);
        }

      if (decoded < 0)
        {
          /* A name too long to be one of ours cannot be one of ours. */

          goto next;
        }

      if (strcmp(name, "ssid") != 0 && strcmp(name, "password") != 0 &&
          strcmp(name, "no_password") != 0 &&
          strcmp(name, "mimo_apikey") != 0 &&
          strcmp(name, "volc_appid") != 0 &&
          strcmp(name, "volc_token") != 0 &&
          strcmp(name, "cloud_host") != 0 &&
          strcmp(name, "cloud_port") != 0 &&
          strcmp(name, "cloud_path") != 0)
        {
          goto next;
        }

      decoded = vp_url_decode(sep + 1, fieldlen - namelen - 1,
                              value, sizeof(value));
      if (decoded < 0)
        {
          /* Even at this cap the value cannot belong to any known field, so
           * there is no field to name.
           */

          return vp_fail(which, VP_FORM_FIELD_BODY,
                         decoded == -E2BIG ? -E2BIG : -EINVAL);
        }

      if (strcmp(name, "ssid") == 0)
        {
          if (submit->have_ssid)
            {
              return vp_fail(which, VP_FORM_FIELD_SSID, -EINVAL);
            }

          submit->have_ssid = true;
          if ((size_t)decoded >= sizeof(submit->cred.ssid))
            {
              return vp_fail(which, VP_FORM_FIELD_SSID, -E2BIG);
            }

          memcpy(submit->cred.ssid, value, (size_t)decoded + 1);
        }
      else if (strcmp(name, "password") == 0)
        {
          if (submit->have_password)
            {
              return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
            }

          submit->have_password = true;
          if ((size_t)decoded >= sizeof(submit->cred.password))
            {
              return vp_fail(which, VP_FORM_FIELD_PASSWORD, -E2BIG);
            }

          memcpy(submit->cred.password, value, (size_t)decoded + 1);
        }
      else if (strcmp(name, "no_password") == 0)
        {
          if (have_open_flag)
            {
              return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
            }

          have_open_flag = true;
          submit->open_requested = vp_checkbox_set(value);
        }
      else if (strcmp(name, "mimo_apikey") == 0)
        {
          if (submit->have_api_key)
            {
              return vp_fail(which, VP_FORM_FIELD_API_KEY, -EINVAL);
            }

          submit->have_api_key = true;
          vp_trim_ascii_space(value);
          if (strlen(value) >= sizeof(submit->cred.api_key))
            {
              return vp_fail(which, VP_FORM_FIELD_API_KEY, -E2BIG);
            }

          strcpy(submit->cred.api_key, value);
        }
      else if (strcmp(name, "volc_appid") == 0)
        {
          if (submit->have_volc_appid)
            {
              return vp_fail(which, VP_FORM_FIELD_VOLC_APPID, -EINVAL);
            }

          submit->have_volc_appid = true;
          vp_trim_ascii_space(value);
          if (strlen(value) >= sizeof(submit->cred.volc_appid))
            {
              return vp_fail(which, VP_FORM_FIELD_VOLC_APPID, -E2BIG);
            }

          strcpy(submit->cred.volc_appid, value);
        }
      else if (strcmp(name, "cloud_host") == 0)
        {
          if (submit->have_cloud_host)
            {
              return vp_fail(which, VP_FORM_FIELD_CLOUD_HOST, -EINVAL);
            }

          submit->have_cloud_host = true;
          vp_trim_ascii_space(value);
          if (strlen(value) >= sizeof(submit->cred.cloud_host))
            {
              return vp_fail(which, VP_FORM_FIELD_CLOUD_HOST, -E2BIG);
            }

          strcpy(submit->cred.cloud_host, value);
        }
      else if (strcmp(name, "cloud_path") == 0)
        {
          if (submit->have_cloud_path)
            {
              return vp_fail(which, VP_FORM_FIELD_CLOUD_PATH, -EINVAL);
            }

          submit->have_cloud_path = true;
          vp_trim_ascii_space(value);
          if (strlen(value) >= sizeof(submit->cred.cloud_path))
            {
              return vp_fail(which, VP_FORM_FIELD_CLOUD_PATH, -E2BIG);
            }

          strcpy(submit->cred.cloud_path, value);
        }
      else if (strcmp(name, "cloud_port") == 0)
        {
          if (submit->have_cloud_port)
            {
              return vp_fail(which, VP_FORM_FIELD_CLOUD_PORT, -EINVAL);
            }

          submit->have_cloud_port = true;
          vp_trim_ascii_space(value);

          /* Empty stays 0, which resolve() reads as "back to the default".
           * A non-empty value must be entirely digits and in range: strtol
           * would accept "80abc" and "0x50", and a port the user did not
           * type is worse than a rejection they can see.
           */

          if (value[0] != '\0')
            {
              unsigned long port = 0;
              size_t i;

              for (i = 0; value[i] != '\0'; i++)
                {
                  if (value[i] < '0' || value[i] > '9' || i >= 5)
                    {
                      return vp_fail(which, VP_FORM_FIELD_CLOUD_PORT,
                                     -EINVAL);
                    }

                  port = port * 10u + (unsigned long)(value[i] - '0');
                }

              if (port == 0 || port > 65535u)
                {
                  return vp_fail(which, VP_FORM_FIELD_CLOUD_PORT, -EINVAL);
                }

              submit->cred.cloud_port = (uint16_t)port;
            }
        }
      else
        {
          if (submit->have_volc_token)
            {
              return vp_fail(which, VP_FORM_FIELD_VOLC_TOKEN, -EINVAL);
            }

          submit->have_volc_token = true;
          vp_trim_ascii_space(value);
          if (strlen(value) >= sizeof(submit->cred.volc_token))
            {
              return vp_fail(which, VP_FORM_FIELD_VOLC_TOKEN, -E2BIG);
            }

          strcpy(submit->cred.volc_token, value);
        }

next:
      pos += fieldlen;
      if (pos < bodylen)
        {
          pos += 1; /* the '&' */
        }
    }

  if (!submit->have_ssid)
    {
      return vp_fail(which, VP_FORM_FIELD_SSID, -EINVAL);
    }

  return 0;
}

int vp_form_resolve(struct vp_form_submit_s *submit,
                    const struct velasight_prov_credentials_s *previous,
                    bool have_previous,
                    enum vp_form_field_e *which)
{
  struct velasight_prov_credentials_s *cred;

  if (which != NULL)
    {
      *which = VP_FORM_FIELD_NONE;
    }

  if (submit == NULL || (have_previous && previous == NULL))
    {
      return vp_fail(which, VP_FORM_FIELD_BODY, -EINVAL);
    }

  cred = &submit->cred;

  /* The form pre-fills the network name, so an empty box is a deliberate
   * clear rather than "leave it alone".  There is nothing sensible to store
   * for a nameless network, so this is where it stops.
   */

  if (cred->ssid[0] == '\0')
    {
      return vp_fail(which, VP_FORM_FIELD_SSID, -EINVAL);
    }

  if (submit->open_requested)
    {
      if (cred->password[0] != '\0')
        {
          /* Ticking the box and typing a password are two different
           * intentions; picking one for the user would be a guess.
           */

          return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
        }

      cred->open_network = true;
    }
  else if (cred->password[0] != '\0')
    {
      cred->open_network = false;
    }
  else if (have_previous)
    {
      /* Carry the stored password and its flag over as a pair.  Moving one
       * without the other breaks the agreement that
       * vp_credentials_validate() enforces, and a record where they disagree
       * behaves differently depending on which field a reader trusts.
       */

      memcpy(cred->password, previous->password, sizeof(cred->password));
      cred->open_network = previous->open_network;
    }
  else
    {
      /* First-time setup has nothing to fall back on, so silence here would
       * quietly create a passwordless network the user never asked for.
       */

      return vp_fail(which, VP_FORM_FIELD_PASSWORD, -EINVAL);
    }

  if (have_previous)
    {
      if (cred->api_key[0] == '\0')
        {
          memcpy(cred->api_key, previous->api_key, sizeof(cred->api_key));
        }

      if (cred->volc_appid[0] == '\0')
        {
          memcpy(cred->volc_appid, previous->volc_appid,
                 sizeof(cred->volc_appid));
        }

      if (cred->volc_token[0] == '\0')
        {
          memcpy(cred->volc_token, previous->volc_token,
                 sizeof(cred->volc_token));
        }

      /* The endpoint fields carry over only when the field was *absent*
       * from the body, not when it arrived empty.  The form pre-fills them,
       * so an empty box is the user clearing a custom endpoint back to the
       * built-in default, and carrying the old value over would make that
       * impossible.  An absent field, by contrast, means this client never
       * offered the box at all -- an older cached form, or a hand-written
       * request -- and silently resetting the endpoint for it would be a
       * change nobody asked for.
       */

      if (!submit->have_cloud_host)
        {
          memcpy(cred->cloud_host, previous->cloud_host,
                 sizeof(cred->cloud_host));
        }

      if (!submit->have_cloud_path)
        {
          memcpy(cred->cloud_path, previous->cloud_path,
                 sizeof(cred->cloud_path));
        }

      if (!submit->have_cloud_port)
        {
          cred->cloud_port = previous->cloud_port;
        }
    }

  return vp_credentials_validate_detail(cred, which);
}

unsigned int vp_credentials_changed(
    const struct velasight_prov_credentials_s *now,
    const struct velasight_prov_credentials_s *previous,
    bool have_previous)
{
  unsigned int changed = 0;

  if (now == NULL)
    {
      return 0;
    }

  if (!have_previous || previous == NULL)
    {
      changed |= VP_FORM_CHANGED_SSID;
      if (now->password[0] != '\0' || now->open_network)
        {
          changed |= VP_FORM_CHANGED_PASSWORD;
        }

      if (now->api_key[0] != '\0')
        {
          changed |= VP_FORM_CHANGED_API_KEY;
        }

      if (now->volc_appid[0] != '\0')
        {
          changed |= VP_FORM_CHANGED_VOLC_APPID;
        }

      if (now->volc_token[0] != '\0')
        {
          changed |= VP_FORM_CHANGED_VOLC_TOKEN;
        }

      if (now->cloud_host[0] != '\0')
        {
          changed |= VP_FORM_CHANGED_CLOUD_HOST;
        }

      if (now->cloud_path[0] != '\0')
        {
          changed |= VP_FORM_CHANGED_CLOUD_PATH;
        }

      if (now->cloud_port != 0)
        {
          changed |= VP_FORM_CHANGED_CLOUD_PORT;
        }

      return changed;
    }

  if (strcmp(now->ssid, previous->ssid) != 0)
    {
      changed |= VP_FORM_CHANGED_SSID;
    }

  if (strcmp(now->password, previous->password) != 0 ||
      now->open_network != previous->open_network)
    {
      changed |= VP_FORM_CHANGED_PASSWORD;
    }

  if (strcmp(now->api_key, previous->api_key) != 0)
    {
      changed |= VP_FORM_CHANGED_API_KEY;
    }

  if (strcmp(now->volc_appid, previous->volc_appid) != 0)
    {
      changed |= VP_FORM_CHANGED_VOLC_APPID;
    }

  if (strcmp(now->volc_token, previous->volc_token) != 0)
    {
      changed |= VP_FORM_CHANGED_VOLC_TOKEN;
    }

  if (strcmp(now->cloud_host, previous->cloud_host) != 0)
    {
      changed |= VP_FORM_CHANGED_CLOUD_HOST;
    }

  if (strcmp(now->cloud_path, previous->cloud_path) != 0)
    {
      changed |= VP_FORM_CHANGED_CLOUD_PATH;
    }

  if (now->cloud_port != previous->cloud_port)
    {
      changed |= VP_FORM_CHANGED_CLOUD_PORT;
    }

  return changed;
}
