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

  /* True when ap_password holds a passphrase this device generated and
   * persisted, false when it is still the CONFIG_VS_AP_PASSWORD fallback.
   *
   * This is what keeps entering AP mode free of I/O.  vs_network_apply_ap()
   * draws a new passphrase only when the flag is clear, so the normal case --
   * a device that has been in AP mode before -- reuses the cached value and
   * writes nothing.  Clearing the flag is how the reset gesture asks for a
   * new one.  Always false without CONFIG_VS_AP_RANDOM_PASSWORD.
   */

  bool ap_password_random;
  uint8_t ap_channel;
};

/* Load Wi-Fi settings.  When generation is non-NULL it receives the
 * generation of the durable provisioning record, or 0 when first-boot
 * Kconfig defaults were used. */

int vs_config_load_wifi(struct vs_wifi_config_s *config,
                        uint32_t *generation);

#endif
