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

int vs_config_load_wifi(struct vs_wifi_config_s *config);

#endif
