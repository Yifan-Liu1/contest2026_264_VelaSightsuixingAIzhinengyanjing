#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "include/vs_config.h"

int vs_config_load_wifi(struct vs_wifi_config_s *config)
{
  if (config == NULL)
    {
      return -EINVAL;
    }

  memset(config, 0, sizeof(*config));
  snprintf(config->sta_ssid, sizeof(config->sta_ssid), "%s",
           CONFIG_VS_STA_SSID);
  snprintf(config->sta_password, sizeof(config->sta_password), "%s",
           CONFIG_VS_STA_PASSWORD);
  snprintf(config->ap_ssid, sizeof(config->ap_ssid), "%s",
           CONFIG_VS_AP_SSID);
  snprintf(config->ap_password, sizeof(config->ap_password), "%s",
           CONFIG_VS_AP_PASSWORD);
  config->ap_channel = CONFIG_VS_AP_CHANNEL;
  return 0;
}
