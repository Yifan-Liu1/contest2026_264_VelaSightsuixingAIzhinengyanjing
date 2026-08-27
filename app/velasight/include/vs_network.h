#ifndef __APP_VELASIGHT_INCLUDE_VS_NETWORK_H
#define __APP_VELASIGHT_INCLUDE_VS_NETWORK_H

#include "vs_types.h"

struct vs_network_s;

int vs_network_open(struct vs_network_s **network);
int vs_network_request_mode(struct vs_network_s *network,
                            enum vs_net_mode_e mode);

/* Restart the SoftAP so it comes back up with a newly generated passphrase,
 * and persist that passphrase so it survives the next boot.
 *
 * A WPA2 passphrase cannot be changed on a running AP, so this is the same
 * blocking sequence as entering AP mode -- stop the provisioning server and
 * DHCP, re-associate wlan0 as master, start them again -- and it takes the
 * same few seconds.  Any station currently associated is dropped; that is
 * inherent to the operation, not a shortcut.  Call it off the UI task.
 *
 * This is the only path that draws a passphrase on a device that already has
 * one: an ordinary switch into AP mode reuses the value cached at startup and
 * touches no storage.  So this call is also the only one that writes, and a
 * write that fails is logged rather than propagated -- the hotspot comes up
 * either way, the password just would not outlive a reboot.
 *
 * On success the new passphrase is readable through vs_network_get_status().
 *
 * Returns 0, or a negative errno:
 *   -ENOTSUP  CONFIG_VS_AP_RANDOM_PASSWORD is not enabled, so the AP would
 *             come back with the same fixed CONFIG_VS_AP_PASSWORD
 *   -EINVAL   network is NULL
 *   -EPERM    not currently in AP mode
 *   (other)   whatever the restart failed with; status.error_reason is set
 */

int vs_network_reset_ap_password(struct vs_network_s *network);
int vs_network_process_events(struct vs_network_s *network);
int vs_network_get_status(struct vs_network_s *network,
                          struct vs_net_status_s *status);
void vs_network_close(struct vs_network_s *network);

#endif
