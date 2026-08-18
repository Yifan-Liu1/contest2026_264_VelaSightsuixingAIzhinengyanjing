/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_wifi_config.c
 *
 * Apply the stored Wi-Fi credentials, so the board joins its network after a
 * reset without anyone typing `wapi`.
 *
 * The store held wifi.ssid and wifi.psk from the start and nothing read them:
 * the keys were defined, documented and masked in `kvdb list`, and the actual
 * association was still the manual sequence from docs/WiFi使用说明.md.  This
 * file is the missing consumer, and it does exactly what those two `wapi`
 * commands do, through the same ioctls:
 *
 *   wapi psk wlan0 <passphrase> 3  ->  SIOCSIWENCODEEXT
 *   wapi essid wlan0 <ssid> 1      ->  SIOCSIWESSID with IW_ESSID_ON, which
 *                                      the upper half turns into essid(set)
 *                                      followed by connect()
 *                                      (netdev_upperhalf.c)
 *
 * What it deliberately does not do is DHCP.  There is no DHCP client on this
 * side of the build -- dhcpc lives in apps -- so an address still comes from
 * `renew wlan0`, which this configuration already ships.  Association is the
 * part that needed a human and a password; asking for an address does not.
 *
 * Called from the kvdb_loader task, never from bring-up: the association
 * request crosses to the CP and blocks, and bring-up must not block or the
 * mailbox stops being serviced (see bk7258_kvdb_init()).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>

#include <nuttx/wireless/wireless.h>

#include "bk7258_kvdb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_IFNAME       "wlan0"

/* The driver's own limits (bk7258_wifi.c g_wifi.ssid / g_wifi.password): a
 * 32-character SSID and a 64-character passphrase, each plus a terminator.
 */

#define WIFI_SSID_MAX     33
#define WIFI_PSK_MAX      65

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int wifi_config_ifup(int sock)
{
  struct ifreq ifr;
  int ret;

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, WIFI_IFNAME, IFNAMSIZ);

  ret = ioctl(sock, SIOCGIFFLAGS, (unsigned long)&ifr);
  if (ret < 0)
    {
      return -errno;
    }

  if ((ifr.ifr_flags & IFF_UP) != 0)
    {
      return OK;
    }

  ifr.ifr_flags |= IFF_UP;

  ret = ioctl(sock, SIOCSIFFLAGS, (unsigned long)&ifr);
  return ret < 0 ? -errno : OK;
}

static int wifi_config_passphrase(int sock, FAR const char *psk)
{
  /* The key follows the descriptor, which is why this is a byte buffer and
   * not just a struct: iw_encode_ext ends in key[0].
   */

  uint8_t buffer[sizeof(struct iw_encode_ext) + WIFI_PSK_MAX];
  FAR struct iw_encode_ext *ext = (FAR struct iw_encode_ext *)buffer;
  struct iwreq req;
  size_t len = strlen(psk);
  int ret;

  memset(buffer, 0, sizeof(buffer));
  memset(&req, 0, sizeof(req));

  ext->alg = IW_ENCODE_ALG_CCMP;
  ext->key_len = (uint16_t)len;
  memcpy(ext->key, psk, len);

  strlcpy(req.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  req.u.encoding.pointer = ext;
  req.u.encoding.length = sizeof(struct iw_encode_ext) + len;
  req.u.encoding.flags = IW_ENCODE_ALG_CCMP;

  ret = ioctl(sock, SIOCSIWENCODEEXT, (unsigned long)&req);
  return ret < 0 ? -errno : OK;
}

static int wifi_config_associate(int sock, FAR const char *ssid)
{
  struct iwreq req;
  int ret;

  memset(&req, 0, sizeof(req));
  strlcpy(req.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  req.u.essid.pointer = (FAR void *)ssid;
  req.u.essid.length = strlen(ssid);
  req.u.essid.flags = IW_ESSID_ON;

  ret = ioctl(sock, SIOCSIWESSID, (unsigned long)&req);
  return ret < 0 ? -errno : OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_kvdb_apply_wifi(void)
{
  char ssid[WIFI_SSID_MAX];
  char psk[WIFI_PSK_MAX];
  int sock;
  int ret;

  ret = bk7258_kvdb_get(BK7258_KVDB_KEY_WIFI_SSID, ssid, sizeof(ssid));
  if (ret <= 0)
    {
      /* Nothing stored, or stored empty.  Not a failure: the board simply has
       * no network configured yet and `wapi` still works.
       */

      return -ENOENT;
    }

  if (bk7258_kvdb_get(BK7258_KVDB_KEY_WIFI_PSK, psk, sizeof(psk)) <= 0)
    {
      psk[0] = '\0';           /* Open network */
    }

  sock = socket(PF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      return -errno;
    }

  ret = wifi_config_ifup(sock);
  if (ret < 0)
    {
      printf("kvdb: wlan0 would not come up (%d), Wi-Fi not configured\n",
             ret);
      goto out;
    }

  if (psk[0] != '\0')
    {
      ret = wifi_config_passphrase(sock, psk);
      if (ret < 0)
        {
          printf("kvdb: passphrase rejected (%d), Wi-Fi not configured\n",
                 ret);
          goto out;
        }
    }

  ret = wifi_config_associate(sock, ssid);
  if (ret < 0)
    {
      printf("kvdb: could not associate with %s (%d)\n", ssid, ret);
      goto out;
    }

  /* The SSID is not a secret and naming it is what makes this line useful; the
   * passphrase is never printed, only whether there was one.
   */

  printf("kvdb: associating with %s (%s) -- run `renew wlan0` for an "
         "address\n", ssid, psk[0] != '\0' ? "WPA2" : "open");

out:
  close(sock);
  return ret;
}
