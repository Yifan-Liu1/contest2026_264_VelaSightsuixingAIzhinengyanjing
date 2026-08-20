#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/board.h>

#include "velasight_provisioning.h"
#include "include/vs_config.h"

static int vs_config_wait_for_store(void)
{
#ifdef CONFIG_BK7258_SDIO
  char source[32];
  bool mounted;
  unsigned int attempt;

  /* Automount may be delayed by up to 60 seconds, followed by the card probe
   * timeout.  Keep this bounded but long enough for every legal Kconfig value.
   */
  for (attempt = 0; attempt < 700; attempt++)
    {
      if (bk7258_mmcsd_status(&mounted, source, sizeof(source)) == 0 &&
          mounted)
        {
          return 0;
        }

      usleep(100000);
    }

  return -ETIMEDOUT;
#else
  return -ENODEV;
#endif
}

int vs_config_load_wifi(struct vs_wifi_config_s *config)
{
  static bool first_load = true;
  bool use_defaults = false;

  if (config == NULL)
    {
      return -EINVAL;
    }

  memset(config, 0, sizeof(*config));

  if (first_load)
    {
      int ret;

      ret = vs_config_wait_for_store();
      if (ret < 0)
        {
          return ret;
        }

      first_load = false;
    }

  /* The provisioning record is the runtime source of network credentials.
   * Kconfig is only the first-boot fallback; no Wi-Fi credential is mirrored
   * into KVDB.
   */
  {
    struct velasight_prov_credentials_s credentials;
    int ret = velasight_provisioning_load(&credentials);

    if (ret == 0)
      {
        snprintf(config->sta_ssid, sizeof(config->sta_ssid), "%s",
                 credentials.ssid);
        snprintf(config->sta_password, sizeof(config->sta_password), "%s",
                 credentials.password);
        config->sta_open_network = credentials.open_network;
      }
    else if (ret == -ENOENT)
      {
        use_defaults = true;
      }
    else if (ret != -EBADMSG)
      {
        return ret;
      }
  }

  if (use_defaults)
    {
      snprintf(config->sta_ssid, sizeof(config->sta_ssid), "%s",
               CONFIG_VS_STA_SSID);
      snprintf(config->sta_password, sizeof(config->sta_password), "%s",
               CONFIG_VS_STA_PASSWORD);
      config->sta_open_network = config->sta_password[0] == '\0';
    }

  snprintf(config->ap_ssid, sizeof(config->ap_ssid), "%s",
           CONFIG_VS_AP_SSID);
  snprintf(config->ap_password, sizeof(config->ap_password), "%s",
           CONFIG_VS_AP_PASSWORD);
  config->ap_channel = CONFIG_VS_AP_CHANNEL;
  return 0;
}
