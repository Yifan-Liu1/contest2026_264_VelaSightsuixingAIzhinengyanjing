/****************************************************************************
 * app/velasight/include/vs_settings.h
 *
 * Persistent device settings that belong to the device rather than to a
 * provisioning session.
 *
 * Why this is not part of the provisioning record
 * -----------------------------------------------
 * app/provisioning_web owns /mnt/sdnand/prov/wifi.bin, and that record is
 * replaced wholesale every time someone submits the web form.  A setting the
 * user changes from the device itself has a different lifetime and a different
 * writer, so putting it in the same file would mean either the web form
 * clearing it or the device having to read-modify-write a record it does not
 * own.
 *
 * Why not the agent config store
 * ------------------------------
 * claw_config_set() does persist, to /mnt/sdnand/ai_agent/config/config.json.
 * But bk7258_nand_seed_agent_config() rewrites that whole file with fopen(...,
 * "w") on every boot and after every provisioning change, emitting only the
 * four keys it knows about.  Anything else stored there is erased at the next
 * boot.
 *
 * Format and durability follow app/provisioning_web/vp_store.c: a fixed-size
 * record with a magic, a version and a CRC32, written to a fixed temporary
 * name in the same directory and renamed into place after fsync.  The names
 * stay inside 8.3 even though CONFIG_FAT_LFN is enabled, because the rest of
 * the project does.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_SETTINGS_H
#define __APP_VELASIGHT_INCLUDE_VS_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

/* Speaker volume as a percentage of full scale, the same 0..100 the volume
 * page shows and the same value vs_audio_volume_set() takes after scaling by
 * ten.
 *
 * load returns 0 and fills level only when a valid record exists.  -ENOENT
 * means nothing has been stored yet, which is not an error: the caller keeps
 * whatever the DAC powered up at.  -EBADMSG means a record was found and
 * rejected, which the caller should also treat as "use the default" but is
 * worth a log line.
 *
 * Neither call waits for the filesystem.  Call load only after something else
 * has established that /mnt/sdnand is mounted -- vs_history_open() blocks for
 * exactly that and is already on the startup path.
 */

int vs_settings_load_volume(uint8_t *level);

int vs_settings_save_volume(uint8_t level);

/* The SoftAP passphrase generated on this device
 * (CONFIG_VS_AP_RANDOM_PASSWORD).  Persisted so the hotspot keeps the same
 * password across reboots and across STA/AP switches: it is printed on the
 * device's own screen and typed into a phone, so drawing a new one every time
 * the AP starts would invalidate whatever the user already saved there, for
 * no security gain the reset gesture does not already provide.
 *
 * The whole point of storing it is to make the common path do no I/O at all.
 * load is called once, from vs_config_load_wifi() during vs_network_open(),
 * and the value it returns lives in struct vs_wifi_config_s for the rest of
 * the process; entering AP mode then reads nothing.  save is called only when
 * a new passphrase was actually drawn -- first ever AP entry, an unreadable
 * record, or an explicit reset -- so a device that is simply used never
 * writes here again.
 *
 * Both run on the network worker thread, never on the UI task, and neither
 * waits for the filesystem: call them only once something has established
 * that /mnt/sdnand is mounted.  vs_config_load_wifi() blocks for exactly that
 * on its first call and is already on the startup path.
 *
 * password must have room for 64 bytes.  load returns 0, -ENOENT when nothing
 * has been stored, or -EBADMSG when a record was found and rejected; the
 * caller treats both the same way -- draw a new passphrase -- but -EBADMSG is
 * worth a log line.  save returns 0 or a negative errno, and validates the
 * passphrase against the 8..63 printable-ASCII range WPA2 accepts.
 */

int vs_settings_load_ap_password(char *password, size_t size);

int vs_settings_save_ap_password(const char *password);

#endif /* __APP_VELASIGHT_INCLUDE_VS_SETTINGS_H */
