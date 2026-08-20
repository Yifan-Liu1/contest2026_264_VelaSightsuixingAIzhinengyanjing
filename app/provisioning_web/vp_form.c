/****************************************************************************
 * app/provisioning_web/vp_form.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "vp_form.h"

/* Long enough to tell "too long" from "just fits": the largest legal value is
 * a 63-byte passphrase, so a 64..126 byte submit is decoded and then rejected
 * on length, instead of arriving truncated to something legal.
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

int vp_credentials_validate(
    const struct velasight_prov_credentials_s *cred)
{
  size_t len;
  size_t i;
  size_t api_len;

  if (cred == NULL)
    {
      return -EINVAL;
    }

  api_len = vp_strnlen(cred->api_key, sizeof(cred->api_key));
  if (api_len > VELASIGHT_PROV_API_KEY_MAX)
    return -EINVAL;
  for (i = 0; i < api_len; i++)
    {
      if ((unsigned char)cred->api_key[i] < 0x20 ||
          (unsigned char)cred->api_key[i] > 0x7e)
        return -EINVAL;
    }

  len = vp_strnlen(cred->ssid, sizeof(cred->ssid));
  if (len == 0 || len > VELASIGHT_PROV_SSID_MAX)
    {
      return -EINVAL;
    }

  for (i = 0; i < len; i++)
    {
      if (!vp_ssid_char_ok(cred->ssid[i]))
        {
          return -EINVAL;
        }
    }

  len = vp_strnlen(cred->password, sizeof(cred->password));
  if (len == 0)
    {
      /* The flag and the passphrase have to tell the same story, otherwise a
       * caller that trusts only one of them behaves differently from a caller
       * that trusts the other.
       */

      return cred->open_network ? 0 : -EINVAL;
    }

  if (cred->open_network)
    {
      return -EINVAL;
    }

  if (len < VELASIGHT_PROV_PSK_MIN || len > VELASIGHT_PROV_PSK_MAX)
    {
      return -EINVAL;
    }

  for (i = 0; i < len; i++)
    {
      if (!vp_psk_char_ok(cred->password[i]))
        {
          return -EINVAL;
        }
    }

  return 0;
}

int vp_form_parse(const char *body, size_t bodylen,
                  struct velasight_prov_credentials_s *cred)
{
  uint32_t generation;
  unsigned nfields = 0;
  bool have_ssid = false;
  bool have_psk = false;
  bool have_api_key = false;
  size_t pos = 0;

  if (body == NULL || cred == NULL)
    {
      return -EINVAL;
    }

  generation = cred->generation;
  memset(cred, 0, sizeof(*cred));
  cred->generation = generation;

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
          return -E2BIG;
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

          return -EINVAL;
        }

      namelen = (size_t)(sep - (body + pos));
      decoded = vp_url_decode(body + pos, namelen, name, sizeof(name));
      if (decoded == -EINVAL)
        {
          return -EINVAL;
        }

      if (decoded < 0)
        {
          /* A name too long to be one of ours cannot be one of ours. */

          goto next;
        }

      if (strcmp(name, "ssid") != 0 && strcmp(name, "password") != 0 &&
          strcmp(name, "mimo_apikey") != 0)
        {
          goto next;
        }

      decoded = vp_url_decode(sep + 1, fieldlen - namelen - 1,
                              value, sizeof(value));
      if (decoded < 0)
        {
          return -EINVAL;
        }

      if (strcmp(name, "ssid") == 0)
        {
          if (have_ssid)
            {
              return -EINVAL;
            }

          have_ssid = true;
          if ((size_t)decoded >= sizeof(cred->ssid))
            {
              return -EINVAL;
            }

          memcpy(cred->ssid, value, (size_t)decoded + 1);
        }
      else if (strcmp(name, "password") == 0)
        {
          if (have_psk)
            {
              return -EINVAL;
            }

          have_psk = true;
          if ((size_t)decoded >= sizeof(cred->password))
            {
              return -EINVAL;
            }

          memcpy(cred->password, value, (size_t)decoded + 1);
        }
      else
        {
          if (have_api_key || (size_t)decoded >= sizeof(cred->api_key))
            return -EINVAL;

          have_api_key = true;
          memcpy(cred->api_key, value, (size_t)decoded + 1);
        }

next:
      pos += fieldlen;
      if (pos < bodylen)
        {
          pos += 1; /* the '&' */
        }
    }

  if (!have_ssid)
    {
      return -EINVAL;
    }

  /* A form with no password field at all is how an open network is submitted
   * by a client that simply left the input out.
   */

  cred->open_network = cred->password[0] == '\0';
  return vp_credentials_validate(cred);
}
