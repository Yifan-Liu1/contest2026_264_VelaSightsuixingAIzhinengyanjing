#ifndef __APP_VELASIGHT_INCLUDE_VS_CONFIG_H
#define __APP_VELASIGHT_INCLUDE_VS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

struct vs_wifi_config_s
{
  char sta_ssid[33];
  char sta_password[64];
  bool sta_open_network;
  char ap_ssid[33];
  char ap_password[64];
  uint8_t ap_channel;
};

/* Load Wi-Fi settings.  When generation is non-NULL it receives the
 * generation of the durable provisioning record, or 0 when first-boot
 * Kconfig defaults were used. */

int vs_config_load_wifi(struct vs_wifi_config_s *config,
                        uint32_t *generation);

#endif
