#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
    struct velasight_prov_credentials_s *credentials;
    int ret;

    /* Heap rather than a 976-byte local.  This function runs on the network
     * worker pthread (CONFIG_PTHREAD_STACK_DEFAULT, 4 KiB) and on vs_app_run()
     * itself early in bring-up; see vp_store.c's vp_record_decode() for the
     * incident that made this struct's stack footprint worth caring about.
     */

    credentials = malloc(sizeof(*credentials));
    if (credentials == NULL)
      {
        return -ENOMEM;
      }

    ret = velasight_provisioning_load(credentials);

    if (ret == 0)
      {
        snprintf(config->sta_ssid, sizeof(config->sta_ssid), "%s",
                 credentials->ssid);
        snprintf(config->sta_password, sizeof(config->sta_password), "%s",
                 credentials->password);
        config->sta_open_network = credentials->open_network;
        if (generation != NULL)
          {
            *generation = credentials->generation;
          }
      }
    else if (ret == -ENOENT || ret == -EBADMSG)
      {
        /* -ENOENT is "never provisioned"; -EBADMSG is "provisioned, but not
         * in a shape this build can read" -- the record format has changed
         * three times now (v2->v3 added the Volcengine fields, v3->v4 added
         * the social cloud endpoint) and each change is deliberately
         * breaking: vp_record_decode() refuses rather than guesses at a
         * layout it does not recognise.  A device that was provisioned
         * under an older version therefore reads back exactly like one that
         * was never provisioned at all.
         *
         * Both fall back to the Kconfig default rather than failing
         * vs_config_load_wifi() outright.  The one thing that must not
         * happen on a version mismatch is what an early return here used to
         * do: fail before ap_ssid/ap_password/ap_channel below are ever
         * filled in, which left the AP hotspot with an empty SSID and no
         * way to reach the setup page to fix it -- the one recovery path a
         * device in this state actually needs.  A user who sees "connecting
         * to the same network as before" fail and has to re-enter Wi-Fi
         * credentials is a solved problem; a device that cannot be
         * provisioned at all is not.
         */

        if (ret == -EBADMSG)
          {
            printf("velasight: provisioning record unreadable (%d, "
                   "format mismatch), falling back to Kconfig Wi-Fi "
                   "defaults\n", ret);
          }

        use_defaults = true;
      }
    else
      {
        free(credentials);
        return ret;
      }

    free(credentials);
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
