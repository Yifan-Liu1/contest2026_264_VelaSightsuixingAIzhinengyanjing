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

#endif /* __APP_VELASIGHT_INCLUDE_VS_SETTINGS_H */
