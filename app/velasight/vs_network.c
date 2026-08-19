#include <nuttx/config.h>

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netutils/dhcpd.h>
#include <netutils/netlib.h>
#include <wireless/wapi.h>
#include <nuttx/wireless/wireless.h>

#include "include/vs_config.h"
#include "include/vs_network.h"

struct vs_network_s
{
  int sock;
  enum vs_net_mode_e mode;
  struct vs_net_status_s status;
  struct vs_wifi_config_s config;
  bool dhcp_running;
};

static int vs_network_apply_sta(struct vs_network_s *network)
{
  struct wpa_wconfig_s wifi;
  int ret;

  ret = wapi_set_ifdown(network->sock, "wlan0");
  if (ret < 0 && ret != -ENODEV)
    return ret;

  ret = wapi_set_ifup(network->sock, "wlan0");
  if (ret < 0)
    return ret;

  memset(&wifi, 0, sizeof(wifi));
  wifi.ifname = "wlan0";
  wifi.sta_mode = WAPI_MODE_MANAGED;
  wifi.auth_wpa = WPA_VER_2;
  wifi.cipher_mode = IW_AUTH_CIPHER_CCMP;
  wifi.alg = WPA_ALG_CCMP;
  wifi.ssid = network->config.sta_ssid;
  wifi.ssidlen = strlen(wifi.ssid);
  wifi.passphrase = network->config.sta_password;
  wifi.phraselen = strlen(wifi.passphrase);

  ret = wpa_driver_wext_associate(&wifi);
  if (ret < 0)
    return ret;

  ret = netlib_obtain_ipv4addr("wlan0");
  if (ret < 0)
    return ret;

  network->mode = VS_NET_STA;
  network->status.mode = VS_NET_STA;
  network->status.state = VS_NET_STA_READY;
  snprintf(network->status.ssid, sizeof(network->status.ssid), "%s",
           network->config.sta_ssid);
  snprintf(network->status.address, sizeof(network->status.address),
           "STA READY");
  return 0;
}

static int vs_network_apply_ap(struct vs_network_s *network)
{
  struct wpa_wconfig_s wifi;
  struct in_addr address;
  struct in_addr netmask;
  int ret;

  if (network->config.ap_channel < 1 || network->config.ap_channel > 14 ||
      network->config.ap_ssid[0] == '\0')
    return -EINVAL;

  ret = wapi_set_ifdown(network->sock, "wlan0");
  if (ret < 0 && ret != -ENODEV)
    return ret;

  ret = wapi_set_ifup(network->sock, "wlan0");
  if (ret < 0)
    return ret;

  memset(&wifi, 0, sizeof(wifi));
  wifi.ifname = "wlan0";
  wifi.sta_mode = WAPI_MODE_MASTER;
  wifi.auth_wpa = WPA_VER_2;
  wifi.cipher_mode = IW_AUTH_CIPHER_CCMP;
  wifi.alg = WPA_ALG_CCMP;
  wifi.freq = network->config.ap_channel;
  wifi.flag = WAPI_FREQ_FIXED;
  wifi.ssid = network->config.ap_ssid;
  wifi.ssidlen = strlen(wifi.ssid);
  wifi.passphrase = network->config.ap_password;
  wifi.phraselen = strlen(wifi.passphrase);

  ret = wpa_driver_wext_associate(&wifi);
  if (ret < 0)
    return ret;

  address.s_addr = htonl(0xc0a80a01);
  netmask.s_addr = htonl(0xffffff00);
  ret = netlib_set_ipv4addr("wlan0", &address);
  if (ret < 0)
    return ret;
  ret = netlib_set_ipv4netmask("wlan0", &netmask);
  if (ret < 0)
    return ret;

  ret = dhcpd_start("wlan0");
  if (ret < 0)
    return ret;
  network->dhcp_running = true;
  network->mode = VS_NET_AP;
  network->status.mode = VS_NET_AP;
  network->status.state = VS_NET_AP_READY;
  snprintf(network->status.ssid, sizeof(network->status.ssid), "%s",
           network->config.ap_ssid);
  snprintf(network->status.address, sizeof(network->status.address),
           "192.168.10.1");
  return 0;
}

int vs_network_open(struct vs_network_s **network)
{
  struct vs_network_s *n;
  int ret;

  if (network == NULL)
    return -EINVAL;
  n = calloc(1, sizeof(*n));
  if (n == NULL)
    return -ENOMEM;
  n->sock = wapi_make_socket();
  if (n->sock < 0)
    {
      ret = n->sock;
      free(n);
      return ret;
    }
  ret = vs_config_load_wifi(&n->config);
  if (ret < 0)
    {
      close(n->sock);
      free(n);
      return ret;
    }
  n->status.mode = VS_NET_STA;
  n->status.state = VS_NET_DOWN;
  *network = n;
  return 0;
}

int vs_network_request_mode(struct vs_network_s *network,
                            enum vs_net_mode_e mode)
{
  int ret;

  if (network == NULL)
    return -EINVAL;

  network->status.state = VS_NET_SWITCHING;
  if (network->dhcp_running)
    {
      (void)dhcpd_stop();
      network->dhcp_running = false;
    }

  ret = mode == VS_NET_AP ? vs_network_apply_ap(network) :
                            vs_network_apply_sta(network);
  if (ret < 0)
    {
      network->status.state = VS_NET_ERROR;
      network->status.error = ret;
    }
  return ret;
}

int vs_network_get_status(struct vs_network_s *network,
                          struct vs_net_status_s *status)
{
  if (network == NULL || status == NULL)
    return -EINVAL;
  *status = network->status;
  return 0;
}

void vs_network_close(struct vs_network_s *network)
{
  if (network == NULL)
    return;
  if (network->dhcp_running)
    (void)dhcpd_stop();
  (void)wapi_set_ifdown(network->sock, "wlan0");
  close(network->sock);
  free(network);
}

int weak_function vs_softap_service_start(void)
{
  return -ENOSYS;
}

int weak_function vs_softap_service_stop(void)
{
  return -ENOSYS;
}
