#include <nuttx/config.h>

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netutils/dhcpd.h>
#include <netutils/netlib.h>
#include <wireless/wapi.h>
#include <nuttx/wireless/wireless.h>

#include "velasight_provisioning.h"
#include "include/vs_config.h"
#include "include/vs_network.h"

#define VS_DHCP_TRIES       3
#define VS_DHCP_RETRY_MS    1500

struct vs_network_s
{
  int sock;
  enum vs_net_mode_e mode;
  struct vs_net_status_s status;
  struct vs_wifi_config_s config;
  pthread_mutex_t event_lock;
  bool dhcp_running;
  bool provision_running;
  bool provision_event;
  int provision_status;
};

static int vs_network_stop_dhcp(struct vs_network_s *network)
{
  int ret;

  if (!network->dhcp_running)
    return 0;

  ret = dhcpd_stop();
  if (ret < 0)
    return ret;

  network->dhcp_running = false;
  return 0;
}

static int vs_network_failed(const char *step, int ret)
{
  printf("velasight: network %s failed: %d\n", step, ret);
  return ret;
}

static int vs_network_stop_provisioning(struct vs_network_s *network)
{
  int ret;

  if (!network->provision_running)
    return 0;

  ret = velasight_provisioning_stop();
  if (ret == 0 || ret == -EALREADY)
    {
      network->provision_running = false;
      return 0;
    }

  return ret;
}

static void vs_network_provision_saved(int status, uint32_t generation,
                                       void *arg)
{
  struct vs_network_s *network = arg;

  (void)generation;
  if (network != NULL)
    {
      pthread_mutex_lock(&network->event_lock);
      network->provision_status = status;
      network->provision_event = true;
      pthread_mutex_unlock(&network->event_lock);
    }
}

static int vs_network_consume_saved(struct vs_network_s *network,
                                    bool ignore_failure)
{
  struct vs_wifi_config_s config;
  bool pending;
  int status;
  int ret;

  pthread_mutex_lock(&network->event_lock);
  pending = network->provision_event;
  status = network->provision_status;
  network->provision_event = false;
  pthread_mutex_unlock(&network->event_lock);

  if (!pending)
    return -ENOENT;

  if (status < 0)
    return ignore_failure ? -ENOENT : status;

  ret = vs_config_load_wifi(&config);
  if (ret < 0)
    return ret;

  network->config = config;
  return 0;
}

static int vs_network_apply_sta(struct vs_network_s *network)
{
  struct wpa_wconfig_s wifi;
  unsigned int attempt;
  int ret;

  if (network->config.sta_ssid[0] == '\0')
    return -EBADMSG;

  ret = wapi_set_ifdown(network->sock, "wlan0");
  if (ret < 0 && ret != -ENODEV)
    return vs_network_failed("STA ifdown", ret);

  ret = wapi_set_ifup(network->sock, "wlan0");
  if (ret < 0)
    return vs_network_failed("STA ifup", ret);

  memset(&wifi, 0, sizeof(wifi));
  wifi.ifname = "wlan0";
  wifi.sta_mode = WAPI_MODE_MANAGED;
  if (!network->config.sta_open_network &&
      network->config.sta_password[0] != '\0')
    {
      wifi.auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
      wifi.cipher_mode = IW_AUTH_CIPHER_CCMP;
      wifi.alg = WPA_ALG_CCMP;
    }
  wifi.ssid = network->config.sta_ssid;
  wifi.ssidlen = strlen(wifi.ssid);
  wifi.passphrase = network->config.sta_password;
  wifi.phraselen = strlen(wifi.passphrase);

  ret = wpa_driver_wext_associate(&wifi);
  if (ret < 0)
    return vs_network_failed("STA associate", ret);

  ret = -ETIMEDOUT;
  for (attempt = 0; attempt < VS_DHCP_TRIES; attempt++)
    {
      ret = netlib_obtain_ipv4addr("wlan0");
      if (ret == 0)
        {
          break;
        }

      if (attempt + 1 < VS_DHCP_TRIES)
        {
          printf("velasight: STA DHCP attempt %u failed: %d, retrying\n",
                 attempt + 1, ret);
          usleep(VS_DHCP_RETRY_MS * 1000);
        }
    }

  if (ret < 0)
    return vs_network_failed("STA DHCP", ret);

  network->mode = VS_NET_STA;
  network->status.mode = VS_NET_STA;
  network->status.state = VS_NET_STA_READY;
  snprintf(network->status.ssid, sizeof(network->status.ssid), "%s",
           network->config.sta_ssid);
  network->status.password[0] = '\0';
  snprintf(network->status.address, sizeof(network->status.address),
           "STA READY");
  return 0;
}

static int vs_network_apply_ap(struct vs_network_s *network)
{
  struct wpa_wconfig_s wifi;
  struct in_addr address;
  struct in_addr netmask;
  struct velasight_prov_config_s provision;
  int ret;

  if (network->config.ap_channel < 1 || network->config.ap_channel > 14 ||
      network->config.ap_ssid[0] == '\0')
    return -EINVAL;

  ret = wapi_set_ifdown(network->sock, "wlan0");
  if (ret < 0 && ret != -ENODEV)
    return vs_network_failed("AP ifdown", ret);

  ret = wapi_set_ifup(network->sock, "wlan0");
  if (ret < 0)
    return vs_network_failed("AP ifup", ret);

  memset(&wifi, 0, sizeof(wifi));
  wifi.ifname = "wlan0";
  wifi.sta_mode = WAPI_MODE_MASTER;
  if (network->config.ap_password[0] != '\0')
    {
      wifi.auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
      wifi.cipher_mode = IW_AUTH_CIPHER_CCMP;
      wifi.alg = WPA_ALG_CCMP;
    }
  wifi.freq = network->config.ap_channel;
  wifi.flag = WAPI_FREQ_FIXED;
  wifi.ssid = network->config.ap_ssid;
  wifi.ssidlen = strlen(wifi.ssid);
  wifi.passphrase = network->config.ap_password;
  wifi.phraselen = strlen(wifi.passphrase);

  ret = wpa_driver_wext_associate(&wifi);
  if (ret < 0)
    {
      vs_network_failed("AP start", ret);
      goto fail;
    }

  address.s_addr = htonl(0xc0a80a01);
  netmask.s_addr = htonl(0xffffff00);
  ret = netlib_set_ipv4addr("wlan0", &address);
  if (ret < 0)
    {
      vs_network_failed("AP IPv4 address", ret);
      goto fail;
    }

  ret = netlib_set_ipv4netmask("wlan0", &netmask);
  if (ret < 0)
    {
      vs_network_failed("AP netmask", ret);
      goto fail;
    }

  ret = dhcpd_start("wlan0");
  if (ret < 0)
    {
      vs_network_failed("AP DHCP server", ret);
      goto fail;
    }

  network->dhcp_running = true;
  memset(&provision, 0, sizeof(provision));
  provision.one_shot = true;
  provision.on_saved = vs_network_provision_saved;
  provision.cb_arg = network;
  ret = velasight_provisioning_start(&provision);
  if (ret < 0)
    {
      vs_network_failed("AP provisioning server", ret);
      goto fail;
    }

  network->provision_running = true;

  network->mode = VS_NET_AP;
  network->status.mode = VS_NET_AP;
  network->status.state = VS_NET_AP_READY;
  snprintf(network->status.ssid, sizeof(network->status.ssid), "%s",
           network->config.ap_ssid);
  snprintf(network->status.password, sizeof(network->status.password), "%s",
           network->config.ap_password);
  printf("velasight: SoftAP ready, ssid=%s channel=%u\n",
         network->status.ssid, network->config.ap_channel);
  snprintf(network->status.address, sizeof(network->status.address),
           "192.168.10.1");
  return 0;

fail:
  (void)vs_network_stop_provisioning(network);
  (void)vs_network_stop_dhcp(network);
  (void)wapi_set_ifdown(network->sock, "wlan0");
  return ret;
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

  ret = pthread_mutex_init(&n->event_lock, NULL);
  if (ret != 0)
    {
      free(n);
      return -ret;
    }

  n->sock = wapi_make_socket();
  if (n->sock < 0)
    {
      ret = n->sock;
      pthread_mutex_destroy(&n->event_lock);
      free(n);
      return ret;
    }

  ret = vs_config_load_wifi(&n->config);
  if (ret < 0)
    {
      close(n->sock);
      pthread_mutex_destroy(&n->event_lock);
      free(n);
      return ret;
    }

  n->mode = VS_NET_STA;
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

  if (network->mode == VS_NET_AP && mode == VS_NET_STA)
    {
      ret = vs_network_stop_provisioning(network);
      if (ret < 0)
        {
          network->status.state = VS_NET_ERROR;
          network->status.error = ret;
          return ret;
        }

      /* stop() joins the listener, so no callback can race with this consume.
       * Apply a completed save even when manual exit won the UI race; discard
       * a failed save because the user explicitly chose to leave AP.
       */
      ret = vs_network_consume_saved(network, true);
      if (ret < 0 && ret != -ENOENT)
        {
          network->status.state = VS_NET_ERROR;
          network->status.error = ret;
          return ret;
        }
    }

  ret = vs_network_stop_dhcp(network);
  if (ret < 0)
    {
      network->status.state = VS_NET_ERROR;
      network->status.error = ret;
      return ret;
    }

  /* AP resources are gone before STA association begins.  If association or
   * DHCP then fails, the next network toggle must be able to enter AP again.
   */
  if (mode == VS_NET_STA)
    {
      network->mode = VS_NET_STA;
      network->status.mode = VS_NET_STA;
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

int vs_network_process_events(struct vs_network_s *network)
{
  int ret;

  if (network == NULL)
    return 0;

  ret = vs_network_consume_saved(network, false);
  if (ret == -ENOENT)
    return 0;

  if (ret < 0)
    {
      network->status.error = ret;
      network->status.state = VS_NET_AP_READY;
      return ret;
    }

  ret = vs_network_request_mode(network, VS_NET_STA);
  return ret < 0 ? ret : 1;
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

  (void)vs_network_stop_provisioning(network);
  if (vs_network_stop_dhcp(network) < 0)
    printf("velasight: DHCP server did not stop cleanly\n");

  (void)wapi_set_ifdown(network->sock, "wlan0");
  close(network->sock);
  pthread_mutex_destroy(&network->event_lock);
  free(network);
}
