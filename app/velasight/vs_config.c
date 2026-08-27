#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/board.h>

#include "velasight_provisioning.h"
#include "include/vs_config.h"
#include "include/vs_settings.h"

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

int vs_config_load_wifi(struct vs_wifi_config_s *config,
                        uint32_t *generation)
{
  static bool first_load = true;
  bool use_defaults = false;

  if (config == NULL)
    {
      return -EINVAL;
    }

  memset(config, 0, sizeof(*config));
  if (generation != NULL)
    {
      *generation = 0;
    }

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
   * into KVDB.  A corrupt persisted record is never mistaken for an empty
   * but successful configuration.
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
        if (generation != NULL)
          {
            *generation = credentials.generation;
          }
      }
    else if (ret == -ENOENT)
      {
        use_defaults = true;
      }
    else
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

#ifdef CONFIG_VS_AP_RANDOM_PASSWORD
  /* The one read that makes the SoftAP passphrase survive a reboot.  It lands
   * in the caller's config, which vs_network_open() keeps for the life of the
   * process, so entering AP mode later touches no storage at all.
   *
   * The Kconfig value above is still written first and deliberately not
   * skipped: it is what the AP falls back to if this record is absent on a
   * build where generation somehow does not happen, and leaving the field
   * empty would silently turn the hotspot into an open network.
   *
   * Guarded by the option so a build without it does no extra I/O here.
   */

  {
    char stored[sizeof(config->ap_password)];
    int ret = vs_settings_load_ap_password(stored, sizeof(stored));

    if (ret == 0)
      {
        snprintf(config->ap_password, sizeof(config->ap_password), "%s",
                 stored);
        config->ap_password_random = true;
      }
    else if (ret != -ENOENT)
      {
        /* Not fatal, and not silent: the next AP entry will draw a fresh
         * passphrase and overwrite the bad record, but a card that keeps
         * producing this is worth seeing in the boot log.
         */

        printf("velasight: stored AP password rejected (%d)\n", ret);
      }
  }
#endif

  return 0;
}
