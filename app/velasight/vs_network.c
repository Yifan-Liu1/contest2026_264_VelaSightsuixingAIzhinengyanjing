#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netutils/dhcpd.h>
#include <netutils/netlib.h>
#include <wireless/wapi.h>
#include <nuttx/wireless/wireless.h>

#include <arch/board/board.h>
#include <arch/chip/bk7258_wifi.h>

#include "velasight_provisioning.h"
#include "include/vs_cloud.h"
#include "include/vs_config.h"
#include "include/vs_history.h"
#include "include/vs_network.h"
#include "include/vs_settings.h"
#include "include/vs_voice.h"


#define VS_DHCP_TRIES       3
#define VS_DHCP_RETRY_MS    1500
#define VS_STA_ASSOC_TIMEOUT_MS 15000

struct vs_network_s
{
  int sock;
  enum vs_net_mode_e mode;
  struct vs_net_status_s status;
  struct vs_wifi_config_s config;
  pthread_mutex_t event_lock;
  pthread_cond_t wifi_cond;
  bool dhcp_running;
  bool provision_running;
  uint32_t provision_pending_generation;
  uint32_t provision_applied_generation;
  bool provision_failure_pending;
  int provision_failure_status;
  bool wifi_event;
  bool sta_connected;
  bool sta_ipv4_ready;
  int sta_disconnect_reason;
  bool ignore_sta_disconnect;
  bool ap_client_event;
  uint8_t ap_client_count;

  /* A pending "draw a new SoftAP passphrase" request, outliving the config
   * reload that request_mode() may perform.  See
   * vs_network_reset_ap_password().  Only ever touched on the network worker
   * thread.
   */

  bool ap_password_reset;
};

static void vs_network_wifi_event(enum bk7258_wifi_event_e event,
                                  unsigned int value, void *arg)
{
  struct vs_network_s *network = arg;

  if (network == NULL)
    {
      return;
    }

  pthread_mutex_lock(&network->event_lock);
  if (event == BK7258_WIFI_EVENT_STA_CONNECTED ||
      event == BK7258_WIFI_EVENT_STA_DISCONNECTED)
    {
      if (event == BK7258_WIFI_EVENT_STA_DISCONNECTED &&
          network->ignore_sta_disconnect)
        {
          pthread_mutex_unlock(&network->event_lock);
          return;
        }

      network->wifi_event = true;
      network->sta_connected = event == BK7258_WIFI_EVENT_STA_CONNECTED;
      if (network->sta_connected)
        {
          network->sta_disconnect_reason = 0;
        }
      else
        {
          network->sta_ipv4_ready = false;
          if (network->sta_disconnect_reason !=
              (int)BK7258_WIFI_REASON_NO_AP_FOUND &&
              network->sta_disconnect_reason !=
              (int)BK7258_WIFI_REASON_WRONG_PASSWORD &&
              (value != 0 || network->sta_disconnect_reason == 0))
            {
              network->sta_disconnect_reason = (int)value;
            }
        }

      pthread_cond_signal(&network->wifi_cond);
    }
  else if (event == BK7258_WIFI_EVENT_AP_CLIENTS_CHANGED)
    {
      network->ap_client_event = true;
      network->ap_client_count = value > UINT8_MAX ? UINT8_MAX :
                                                       (uint8_t)value;
    }
  else if (event == BK7258_WIFI_EVENT_AP_STOPPED)
    {
      network->ap_client_event = true;
      network->ap_client_count = 0;
    }
  pthread_mutex_unlock(&network->event_lock);
}

static enum vs_wifi_issue_e vs_network_sta_issue(
    struct vs_network_s *network, enum vs_wifi_issue_e fallback)
{
  int reason;

  pthread_mutex_lock(&network->event_lock);
  reason = network->sta_disconnect_reason;
  pthread_mutex_unlock(&network->event_lock);
  if (reason == (int)BK7258_WIFI_REASON_NO_AP_FOUND)
    {
      return VS_WIFI_ISSUE_SSID_NOT_FOUND;
    }
  if (reason == (int)BK7258_WIFI_REASON_WRONG_PASSWORD)
    {
      return VS_WIFI_ISSUE_PASSWORD;
    }

  return fallback;
}

static int vs_network_wait_sta_association(struct vs_network_s *network)
{
  struct timespec deadline;
  bool connected;
  int reason;
  int ret;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += VS_STA_ASSOC_TIMEOUT_MS / 1000;
  deadline.tv_nsec += (VS_STA_ASSOC_TIMEOUT_MS % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L)
    {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }

  pthread_mutex_lock(&network->event_lock);
  while (!network->wifi_event)
    {
      ret = pthread_cond_timedwait(&network->wifi_cond,
                                   &network->event_lock, &deadline);
      if (ret != 0)
        {
          pthread_mutex_unlock(&network->event_lock);
          return ret == ETIMEDOUT ? -ETIMEDOUT : -ret;
        }
    }

  connected = network->sta_connected;
  reason = network->sta_disconnect_reason;
  network->wifi_event = false;
  pthread_mutex_unlock(&network->event_lock);

  if (connected)
    {
      return 0;
    }

  if (reason == (int)BK7258_WIFI_REASON_NO_AP_FOUND)
    {
      network->status.wifi_issue = VS_WIFI_ISSUE_SSID_NOT_FOUND;
      return -ENETUNREACH;
    }

  if (reason == (int)BK7258_WIFI_REASON_WRONG_PASSWORD)
    {
      network->status.wifi_issue = VS_WIFI_ISSUE_PASSWORD;
      return -EACCES;
    }

  network->status.wifi_issue = VS_WIFI_ISSUE_DISCONNECTED;
  return -ENOTCONN;
}

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

static int vs_network_failed(struct vs_network_s *network,
                             const char *step, int ret)
{
  if (network != NULL)
    {
      snprintf(network->status.error_reason,
               sizeof(network->status.error_reason), "%s (%d)", step, ret);
      network->status.error = ret;
    }

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

#ifdef CONFIG_VS_AP_RANDOM_PASSWORD
static int vs_network_random_ap_password(char *password, size_t size)
{
  static const char alphabet[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  const size_t alphabet_len = sizeof(alphabet) - 1;
  uint8_t byte;
  size_t index = 0;
  int fd;
  ssize_t n;

  if (password == NULL || size < 9)
    return -EINVAL;

  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return -errno;

  while (index < 8)
    {
      n = read(fd, &byte, sizeof(byte));
      if (n != (ssize_t)sizeof(byte))
        {
          int ret = n < 0 ? -errno : -EIO;
          close(fd);
          return ret;
        }

      /* Reject the incomplete final bucket to avoid modulo bias. */
      if (byte >= (uint8_t)(256u - (256u % alphabet_len)))
        continue;

      password[index++] = alphabet[byte % alphabet_len];
    }

  password[8] = '\0';
  close(fd);
  return 0;
}
#endif

static void vs_network_provision_saved(int status, uint32_t generation,
                                       void *arg)
{
  struct vs_network_s *network = arg;

  if (network == NULL)
    {
      return;
    }

  pthread_mutex_lock(&network->event_lock);
  if (status == 0 && generation != 0)
    {
      /* Successful callbacks are serialized and the store has one current
       * record, so only the latest unapplied generation is meaningful.  A
       * failure is kept independently and cannot erase this durable fact. */
      network->provision_pending_generation = generation;
    }
  else
    {
      network->provision_failure_pending = true;
      network->provision_failure_status = status < 0 ? status : -EBADMSG;
    }
  pthread_mutex_unlock(&network->event_lock);
}

static int vs_network_social_history_snapshot(
    unsigned int offset, struct velasight_prov_history_entry_s *out,
    size_t capacity, unsigned int *total, unsigned int *copied, void *arg)
{
  struct vs_history_index_s *native = NULL;
  unsigned int i;
  int ret;

  (void)arg;
  if (total == NULL || copied == NULL ||
      (capacity > 0 && out == NULL) ||
      capacity > VELASIGHT_PROV_HISTORY_MAX_ENTRIES)
    {
      return -EINVAL;
    }

  if (!vs_history_is_ready(VS_HISTORY_KIND_SOCIAL))
    {
      return -ENODEV;
    }

  if (capacity > 0)
    {
      native = calloc(capacity, sizeof(*native));
      if (native == NULL)
        {
          return -ENOMEM;
        }
    }

  ret = vs_history_snapshot(VS_HISTORY_KIND_SOCIAL, offset, native, capacity,
                            total, copied);
  if (ret < 0)
    {
      free(native);
      return ret;
    }

  for (i = 0; i < *copied; i++)
    {
      if (strlen(native[i].record_key) >
          VELASIGHT_PROV_HISTORY_KEY_MAX)
        {
          free(native);
          return -EBADMSG;
        }

      snprintf(out[i].record_key, sizeof(out[i].record_key), "%s",
               native[i].record_key);
      snprintf(out[i].date, sizeof(out[i].date), "%s", native[i].date);
      snprintf(out[i].title, sizeof(out[i].title), "%s", native[i].title);
      snprintf(out[i].summary, sizeof(out[i].summary), "%s",
               native[i].summary);
      out[i].calm = native[i].calm;
      out[i].happy = native[i].happy;
      out[i].tense = native[i].tense;
      out[i].incomplete = native[i].incomplete;
    }

  free(native);
  return 0;
}

static int vs_network_social_history_open(const char *record_key, int *fd,
                                          size_t *size, void *arg)
{
  (void)arg;
  return vs_history_open_full(VS_HISTORY_KIND_SOCIAL, record_key, fd, size);
}

static int vs_network_start_provisioning(struct vs_network_s *network)
{
  struct velasight_prov_config_s provision;
  int ret;

  memset(&provision, 0, sizeof(provision));
  provision.one_shot = false;
  provision.on_saved = vs_network_provision_saved;
  provision.cb_arg = network;
  provision.history.snapshot = vs_network_social_history_snapshot;
  provision.history.open = vs_network_social_history_open;
  ret = velasight_provisioning_start(&provision);
  if (ret == 0)
    {
      network->provision_running = true;
    }

  return ret;
}

/* Apply the newest durable successful save.  The callback-facing generation
 * is acknowledged only after the record has been loaded and all live mirrors
 * have accepted the refresh.  An equal compare under event_lock preserves a
 * newer callback that may arrive while SD-NAND I/O is in progress.
 *
 * Returns 1 when a save was applied, 0 when none is pending, or a negative
 * errno while leaving the successful generation pending for a later retry.
 */

static int vs_network_reload_saved(struct vs_network_s *network)
{
  struct vs_wifi_config_s config;
  uint32_t pending_generation;
  uint32_t loaded_generation = 0;
  int ret;

  pthread_mutex_lock(&network->event_lock);
  pending_generation = network->provision_pending_generation;
  pthread_mutex_unlock(&network->event_lock);
  if (pending_generation == 0)
    {
      return 0;
    }

  ret = vs_config_load_wifi(&config, &loaded_generation);
  if (ret < 0)
    {
      return ret;
    }

  if (loaded_generation == 0)
    {
      return -EBADMSG;
    }

  network->config = config;
  bk7258_nand_seed_agent_config();
  ret = vs_voice_reload_credentials();
  if (ret < 0)
    {
      return ret;
    }

  /* The social cloud endpoint is in the same record, so a save that moved it
   * has to reach vs_cloud the way a save that moved a credential reaches
   * vs_voice.
   *
   * -EBUSY is not a failure to propagate.  It means a social session is open
   * and vs_cloud has deferred the swap to the next session, which is the only
   * point where changing hosts cannot strand in-flight msgIds.  Returning it
   * here would leave the generation pending and make this whole reload run
   * again on the next pass, re-seeding the agent config and the voice
   * credentials each time for a change that is already recorded.
   */

  ret = vs_cloud_reload_endpoint();
  if (ret < 0 && ret != -EBUSY)
    {
      return ret;
    }

  pthread_mutex_lock(&network->event_lock);
  network->provision_applied_generation = loaded_generation;
  if (network->provision_pending_generation == loaded_generation)
    {
      network->provision_pending_generation = 0;
    }
  pthread_mutex_unlock(&network->event_lock);
  return 1;
}

static int vs_network_take_save_failure(struct vs_network_s *network)
{
  int status = 0;

  pthread_mutex_lock(&network->event_lock);
  if (network->provision_failure_pending)
    {
      status = network->provision_failure_status;
      network->provision_failure_pending = false;
    }
  pthread_mutex_unlock(&network->event_lock);
  return status;
}

static int vs_network_apply_sta(struct vs_network_s *network)
{
  struct wpa_wconfig_s wifi;
  struct in_addr address;
  unsigned int attempt;
  int ret;

  network->status.wifi_issue = VS_WIFI_ISSUE_DISCONNECTED;
  if (network->config.sta_ssid[0] == '\0')
    return vs_network_failed(network, "STA配置", -EBADMSG);

  pthread_mutex_lock(&network->event_lock);
  network->ignore_sta_disconnect = true;
  pthread_mutex_unlock(&network->event_lock);
  ret = wapi_set_ifdown(network->sock, "wlan0");
  pthread_mutex_lock(&network->event_lock);
  network->ignore_sta_disconnect = false;
  network->wifi_event = false;
  network->sta_ipv4_ready = false;
  network->sta_disconnect_reason = 0;
  pthread_mutex_unlock(&network->event_lock);
  if (ret < 0 && ret != -ENODEV && ret != -ETIMEDOUT)
    return vs_network_failed(network, "STA关闭接口", ret);
  if (ret == -ETIMEDOUT)
    printf("velasight: STA ifdown timed out; continuing interface reset\n");

  ret = wapi_set_ifup(network->sock, "wlan0");
  if (ret < 0)
     return vs_network_failed(network, "STA开启接口", ret);

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
     return vs_network_failed(network, "STA连接", ret);

  ret = vs_network_wait_sta_association(network);
  if (ret < 0)
    {
      (void)wapi_set_ifdown(network->sock, "wlan0");
      return vs_network_failed(network,
          network->status.wifi_issue == VS_WIFI_ISSUE_PASSWORD ?
          "WiFi密码错误" :
          network->status.wifi_issue == VS_WIFI_ISSUE_SSID_NOT_FOUND ?
          "SSID未扫描到" : "STA关联", ret);
    }

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
    {
      network->status.wifi_issue = vs_network_sta_issue(
          network, VS_WIFI_ISSUE_DISCONNECTED);
      (void)wapi_set_ifdown(network->sock, "wlan0");
      return vs_network_failed(network,
          network->status.wifi_issue == VS_WIFI_ISSUE_PASSWORD ?
          "WiFi密码错误" : "STA获取地址", ret);
    }

  memset(&address, 0, sizeof(address));
  ret = netlib_get_ipv4addr("wlan0", &address);
  if (ret < 0 || address.s_addr == htonl(INADDR_ANY))
    {
      if (ret >= 0)
        {
          ret = -EADDRNOTAVAIL;
        }
      (void)wapi_set_ifdown(network->sock, "wlan0");
      return vs_network_failed(network, "STA读取地址", ret);
    }

  if (inet_ntop(AF_INET, &address, network->status.address,
                sizeof(network->status.address)) == NULL)
    {
      ret = errno != 0 ? -errno : -EIO;
      (void)wapi_set_ifdown(network->sock, "wlan0");
      return vs_network_failed(network, "STA地址格式", ret);
    }

  ret = vs_network_start_provisioning(network);
  if (ret < 0)
    {
      (void)wapi_set_ifdown(network->sock, "wlan0");
      return vs_network_failed(network, "STA配网服务", ret);
    }

  network->mode = VS_NET_STA;
  network->status.mode = VS_NET_STA;
  network->status.state = VS_NET_STA_READY;
  network->status.wifi_issue = VS_WIFI_ISSUE_NONE;
  pthread_mutex_lock(&network->event_lock);
  network->sta_ipv4_ready = true;
  pthread_mutex_unlock(&network->event_lock);
  snprintf(network->status.ssid, sizeof(network->status.ssid), "%s",
           network->config.sta_ssid);
  network->status.password[0] = '\0';
  printf("velasight: STA ready, web http://%s/\n",
         network->status.address);
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
    return vs_network_failed(network, "AP配置", -EINVAL);

#ifdef CONFIG_VS_AP_RANDOM_PASSWORD

  /* Only when there is nothing to reuse.  ap_password_random is set either by
   * the boot-time load in vs_config_load_wifi() or by a previous pass through
   * here, so the ordinary case -- a device that has shown this hotspot before
   * -- takes neither the TRNG read nor the write below.  That matters because
   * this card's writes are slow enough to be felt, and the user gains nothing
   * from a password that changes behind their back while the one they typed
   * into a phone stops working.
   *
   * vs_network_reset_ap_password() clears the flag; that is the whole
   * mechanism behind the 按住重置 gesture.
   */

  if (!network->config.ap_password_random || network->ap_password_reset)
    {
      ret = vs_network_random_ap_password(network->config.ap_password,
                                         sizeof(network->config.ap_password));
      if (ret < 0)
        return vs_network_failed(network, "AP随机密码", ret);

      network->config.ap_password_random = true;

      /* Cleared only once a passphrase actually exists, so a TRNG failure
       * above leaves the request standing for the next attempt.
       */

      network->ap_password_reset = false;

      /* Persisted before the radio work rather than after it, so the value on
       * screen and the value on the card are the same thing even if
       * association then fails and the user retries.
       *
       * A failed write does not fail the AP: the hotspot still comes up and
       * the password is still readable on screen, it just will not survive a
       * reboot.  Refusing to start provisioning over a storage fault would
       * take away the one screen that can tell the user anything.
       */

      ret = vs_settings_save_ap_password(network->config.ap_password);
      if (ret < 0)
        printf("velasight: AP password not persisted (%d)\n", ret);
    }
#endif

   ret = wapi_set_ifdown(network->sock, "wlan0");
   if (ret < 0 && ret != -ENODEV && ret != -ETIMEDOUT)
     return vs_network_failed(network, "AP关闭接口", ret);
   if (ret == -ETIMEDOUT)
     printf("velasight: AP ifdown timed out; continuing cleanup\n");

  ret = wapi_set_ifup(network->sock, "wlan0");
  if (ret < 0)
    return vs_network_failed(network, "AP开启接口", ret);

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
       vs_network_failed(network, "AP启动", ret);
      goto fail;
    }

  address.s_addr = htonl(0xc0a80a01);
  netmask.s_addr = htonl(0xffffff00);
  ret = netlib_set_ipv4addr("wlan0", &address);
  if (ret < 0)
    {
       vs_network_failed(network, "AP地址", ret);
      goto fail;
    }

  ret = netlib_set_ipv4netmask("wlan0", &netmask);
  if (ret < 0)
    {
       vs_network_failed(network, "AP掩码", ret);
      goto fail;
    }

  ret = dhcpd_start("wlan0");
  if (ret < 0)
    {
       vs_network_failed(network, "AP DHCP", ret);
      goto fail;
    }

  network->dhcp_running = true;
  ret = vs_network_start_provisioning(network);
  if (ret < 0)
    {
       vs_network_failed(network, "AP配网服务", ret);
      goto fail;
    }

  network->mode = VS_NET_AP;
  network->status.mode = VS_NET_AP;
  network->status.state = VS_NET_AP_READY;
  network->status.wifi_issue = VS_WIFI_ISSUE_NONE;
  pthread_mutex_lock(&network->event_lock);
  network->status.ap_client_count = network->ap_client_count;
  pthread_mutex_unlock(&network->event_lock);
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

  ret = pthread_cond_init(&n->wifi_cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&n->event_lock);
      free(n);
      return -ret;
    }

  n->sock = wapi_make_socket();
  if (n->sock < 0)
    {
      ret = n->sock;
      pthread_cond_destroy(&n->wifi_cond);
      pthread_mutex_destroy(&n->event_lock);
      free(n);
      return ret;
    }

  ret = vs_config_load_wifi(&n->config, NULL);
  if (ret < 0)
    {
      close(n->sock);
      pthread_cond_destroy(&n->wifi_cond);
      pthread_mutex_destroy(&n->event_lock);
      free(n);
      return ret;
    }

  /* The social cloud endpoint's first read off SD-NAND, deliberately here
   * rather than in vs_cloud_init() on the startup path.  This function runs
   * on the network worker thread, the same place vs_config_load_wifi() just
   * did its own VFAT read -- not on vs_app_run()'s thread, which the
   * SD-NAND rule (docs/SD-NAND使用说明.md) forbids for reads with
   * near-second worst cases on this board's 1-bit PIO controller.
   *
   * A failure here is not fatal to bring-up: vs_cloud_init() already
   * installed the compiled-in default, so the module is usable either way.
   * It is worth one log line because it is the one case where the running
   * endpoint and whatever the setup page shows can disagree until the next
   * reload.
   */

  ret = vs_cloud_reload_endpoint();
  if (ret < 0 && ret != -EBUSY)
    {
      printf("velasight: social cloud endpoint not loaded (%d), keeping "
             "the factory default\n", ret);
    }

  n->mode = VS_NET_STA;
  n->status.mode = VS_NET_STA;
  n->status.state = VS_NET_DOWN;
  n->status.wifi_issue = VS_WIFI_ISSUE_DISCONNECTED;
  ret = bk7258_wifi_register_event_callback(vs_network_wifi_event, n);
  if (ret < 0)
    {
      close(n->sock);
      pthread_cond_destroy(&n->wifi_cond);
      pthread_mutex_destroy(&n->event_lock);
      free(n);
      return ret;
    }
  *network = n;
  return 0;
}

int vs_network_request_mode(struct vs_network_s *network,
                            enum vs_net_mode_e mode)
{
  int ret;

  if (network == NULL)
    return -EINVAL;

  network->status.error = 0;
  network->status.error_reason[0] = '\0';
  network->status.state = VS_NET_SWITCHING;

  /* The wildcard listener is also active in STA mode.  Join it before any
   * wlan0 reset so no client or callback survives across an interface role
   * change; both AP and STA paths start a fresh listener after their address
   * is ready. */

  ret = vs_network_stop_provisioning(network);
  if (ret < 0)
    {
      snprintf(network->status.error_reason,
               sizeof(network->status.error_reason),
               "Web服务停止 (%d)", ret);
      network->status.state = VS_NET_ERROR;
      network->status.error = ret;
      return ret;
    }

  /* stop() joins the listener, so no save callback can race this reload.
   * A failed HTTP save already returned an error to its client and does not
   * invalidate the current config.  A successful save remains pending until
   * its durable generation and live providers have both been refreshed. */

  ret = vs_network_reload_saved(network);
  if (ret < 0)
    {
      snprintf(network->status.error_reason,
               sizeof(network->status.error_reason),
               "加载已保存配置 (%d)", ret);
      network->status.state = VS_NET_ERROR;
      network->status.error = ret;
      return ret;
    }

  /* The browser already received any persistence failure.  Discard only the
   * independent failure notification during a mode switch; never the pending
   * success generation above. */
  (void)vs_network_take_save_failure(network);

  ret = vs_network_stop_dhcp(network);
  if (ret < 0)
    {
      snprintf(network->status.error_reason,
               sizeof(network->status.error_reason), "DHCP停止 (%d)", ret);
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
        if (network->status.error_reason[0] == '\0')
          snprintf(network->status.error_reason,
                   sizeof(network->status.error_reason),
                   "网络切换 (%d)", ret);
        network->status.state = VS_NET_ERROR;
      network->status.error = ret;
    }

  return ret;
}

int vs_network_reset_ap_password(struct vs_network_s *network)
{
#ifndef CONFIG_VS_AP_RANDOM_PASSWORD
  (void)network;

  /* Reported rather than silently ignored: the caller advertised a reset key
   * to the user, so a build where the passphrase is fixed has to say so.
   */

  return -ENOTSUP;
#else
  if (network == NULL)
    return -EINVAL;

  /* The passphrase only exists while wlan0 is a master.  Rejecting the STA
   * case here keeps this from becoming an accidental way to switch modes.
   */

  if (network->mode != VS_NET_AP)
    return -EPERM;

  /* Raising the flag *is* the request; vs_network_apply_ap() does the work.
   *
   * It lives on the network state rather than in config because request_mode()
   * may call reload_saved(), which replaces config wholesale from the store --
   * and the stored record contains a valid passphrase, so a request expressed
   * as config.ap_password_random = false would be quietly undone right after
   * being made.  Both this function and apply_ap() run on the network worker
   * thread, so the flag needs no lock.
   */

  printf("velasight: regenerating SoftAP password\n");
  network->ap_password_reset = true;
  return vs_network_request_mode(network, VS_NET_AP);
#endif
}

int vs_network_process_events(struct vs_network_s *network)
{
  bool ap_client_event;
  bool sta_connected;
  bool sta_ipv4_ready;
  bool wifi_event;
  uint8_t ap_client_count;
  bool changed = false;
  int disconnect_reason;
  int ret;

  if (network == NULL)
    return 0;

  pthread_mutex_lock(&network->event_lock);
  wifi_event = network->wifi_event;
  sta_connected = network->sta_connected;
  sta_ipv4_ready = network->sta_ipv4_ready;
  disconnect_reason = network->sta_disconnect_reason;
  network->wifi_event = false;
  ap_client_event = network->ap_client_event;
  ap_client_count = network->ap_client_count;
  network->ap_client_event = false;
  pthread_mutex_unlock(&network->event_lock);

  if (network->mode == VS_NET_STA && wifi_event)
    {
      if (sta_connected)
        {
          network->status.state = sta_ipv4_ready ? VS_NET_STA_READY :
                                                   VS_NET_DOWN;
          network->status.wifi_issue = sta_ipv4_ready ?
                                       VS_WIFI_ISSUE_NONE :
                                       VS_WIFI_ISSUE_DISCONNECTED;
          if (sta_ipv4_ready)
            {
              network->status.error = 0;
              network->status.error_reason[0] = '\0';
            }
          changed = true;
        }
      else
        {
          network->status.state = VS_NET_DOWN;
          network->status.wifi_issue =
              disconnect_reason == (int)BK7258_WIFI_REASON_NO_AP_FOUND ?
              VS_WIFI_ISSUE_SSID_NOT_FOUND :
              disconnect_reason == (int)BK7258_WIFI_REASON_WRONG_PASSWORD ?
              VS_WIFI_ISSUE_PASSWORD : VS_WIFI_ISSUE_DISCONNECTED;
          changed = true;
        }
    }

  if (network->mode == VS_NET_AP && ap_client_event)
    {
      network->status.ap_client_count = ap_client_count;
      changed = true;
    }

  ret = vs_network_reload_saved(network);
  if (ret < 0)
    {
      /* Do not acknowledge the generation on a transient load/provider
       * failure.  The current link stays alive and a later poll retries. */
      network->status.error = ret;
      snprintf(network->status.error_reason,
               sizeof(network->status.error_reason),
               "加载保存配置 (%d)", ret);
      return ret;
    }

  if (ret > 0)
    {
      changed = true;
      network->status.error = 0;
      network->status.error_reason[0] = '\0';
    }

  ret = vs_network_take_save_failure(network);
  if (ret < 0)
    {
      /* Persistence errors belong to the Web operation, not the radio.  Keep
       * the real AP/STA/DOWN state instead of fabricating AP_READY while the
       * device may be connected as a station. */
      network->status.error = ret;
      snprintf(network->status.error_reason,
               sizeof(network->status.error_reason),
               "保存配置失败 (%d)", ret);
      return ret;
    }

  /* Keep the current listener and link alive after a save.  New Wi-Fi
   * credentials are used by the next explicit switch or disconnected-STA
   * retry; MiMo/Volcengine live credentials are already refreshed or queued. */
  return changed ? 1 : 0;
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

  bk7258_wifi_unregister_event_callback(vs_network_wifi_event, network);
  (void)vs_network_stop_provisioning(network);
  if (vs_network_stop_dhcp(network) < 0)
    printf("velasight: DHCP server did not stop cleanly\n");

  (void)wapi_set_ifdown(network->sock, "wlan0");
  close(network->sock);
  pthread_cond_destroy(&network->wifi_cond);
  pthread_mutex_destroy(&network->event_lock);
  free(network);
}
