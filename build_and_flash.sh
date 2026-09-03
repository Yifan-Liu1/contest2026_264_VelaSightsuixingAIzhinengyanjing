#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
CONTEST_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
SDK_ROOT="$CONTEST_ROOT/bk_avdk_smp"
OPENVELA_ROOT="$CONTEST_ROOT/contest"
AP_CONFIG="vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent"
AP_OUT="$OPENVELA_ROOT/cmake_out/bk7258-ap_ai_agent"
PACKAGE="$SDK_ROOT/projects/app_ab/build/bk7258/app_ab/package/all-app.bin"
AP_BIN="$AP_OUT/nuttx.bin"
AP_INPUT="$SDK_ROOT/build/openvela-ap.bin"
AP_PACKED="$SDK_ROOT/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin"
LOG_DIR="$SCRIPT_DIR/logs/runtime"
EXTERNAL_PREPARE="$SCRIPT_DIR/external/prepare.sh"
EXTERNAL_MANIFEST="$SCRIPT_DIR/external/manifest.tsv"
CP_CONFIG_SEED="$SCRIPT_DIR/external/bk_avdk_smp/projects/app_ab/cp/config/bk7258/config"
CP_CONFIG="$SDK_ROOT/projects/app_ab/cp/config/bk7258/config"
GENERATED_CP_CONFIG="$SDK_ROOT/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h"
ARMINO_IMAGE=${ARMINO_IMAGE:-localhost/bekencorp/armino-idk:1.5}

PORT=${VELASIGHT_PORT:-/dev/ttyUSB0}
PORT_NUM=${VELASIGHT_PORT_NUM:-}
FLASH_BAUD=${VELASIGHT_FLASH_BAUD:-1500000}
CONSOLE_BAUD=${VELASIGHT_CONSOLE_BAUD:-115200}
BOOT_LOG_SECONDS=${VELASIGHT_BOOT_LOG_SECONDS:-25}
DO_FLASH=0
PREPARE_OVERLAY=0
CP_CONFIG_ARMED=0

die()
{
  printf 'error: %s\n' "$*" >&2
  exit 1
}

usage()
{
  printf '%s\n' \
    'usage: build_and_flash.sh [--prepare-overlay] [--flash] [-p port] [-n loader-port]' \
    '       [--boot-log-seconds seconds]' \
    'Checks the complete external overlay by default. --prepare-overlay safely installs it.' \
    'Builds ai_agent, regenerates the 4148K-aligned AP layout, packages all-app.bin,' \
    'and optionally flashes it with the shell-based autoflash workflow.'
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prepare-overlay) PREPARE_OVERLAY=1 ;;
    --flash) DO_FLASH=1 ;;
    -p)
      shift
      [ "$#" -gt 0 ] || die 'missing -p value'
      PORT=$1
      ;;
    -n)
      shift
      [ "$#" -gt 0 ] || die 'missing -n value'
      PORT_NUM=$1
      ;;
    --boot-log-seconds)
      shift
      [ "$#" -gt 0 ] || die 'missing --boot-log-seconds value'
      BOOT_LOG_SECONDS=$1
      ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

for tool in podman sha256sum cmp awk; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done

test -f "$EXTERNAL_PREPARE" || die "missing overlay tool: $EXTERNAL_PREPARE"
test -f "$EXTERNAL_MANIFEST" || die "missing overlay manifest: $EXTERNAL_MANIFEST"
test -f "$CP_CONFIG_SEED" || die "missing CP config seed: $CP_CONFIG_SEED"

PREPARE_MODE=check
if [ "$PREPARE_OVERLAY" -eq 1 ]; then
  PREPARE_MODE=install
fi
sh "$EXTERNAL_PREPARE" "$PREPARE_MODE" \
  --openvela-root "$OPENVELA_ROOT" \
  --bk-avdk-root "$SDK_ROOT"

EXPECTED_IMAGE_ID=$(awk -F '\t' \
  '$1 == "image" && $2 == "armino_idk" { print $5; exit }' \
  "$EXTERNAL_MANIFEST")
test -n "$EXPECTED_IMAGE_ID" || die 'Armino image ID is not pinned in external/manifest.tsv'
ACTUAL_IMAGE_ID=$(podman image inspect "$ARMINO_IMAGE" --format '{{.Id}}' 2>/dev/null) \
  || die "missing Armino image: $ARMINO_IMAGE (see external/README.md)"
EXPECTED_IMAGE_ID=${EXPECTED_IMAGE_ID#sha256:}
ACTUAL_IMAGE_ID=${ACTUAL_IMAGE_ID#sha256:}
[ "$ACTUAL_IMAGE_ID" = "$EXPECTED_IMAGE_ID" ] \
  || die "Armino image ID mismatch: $ACTUAL_IMAGE_ID != $EXPECTED_IMAGE_ID"

cd "$OPENVELA_ROOT"
./build.sh "$AP_CONFIG" --cmake distclean
./build.sh "$AP_CONFIG" --cmake -j8

test -s "$AP_BIN" || die "missing AP binary: $AP_BIN"
grep -q '^CONFIG_LV_TXT_ENC_UTF8=y$' "$AP_OUT/.config" || die 'UTF-8 is disabled'
grep -q '^# CONFIG_LV_FONT_FMT_TXT_LARGE is not set$' "$AP_OUT/.config" || die 'large LVGL font format wastes flash'
grep -q '^CONFIG_VS_SHORT_PRESS_MAX_MS=500$' "$AP_OUT/.config" || die 'short-press limit is not 500 ms'
# The analog capture gain is tuned against hardware, so assert the value that
# reached .config rather than trusting the Kconfig default to have been picked
# up: editing Kconfig without a distclean silently keeps the cached old value,
# and the symptom (clipped audio the recognizer refuses) looks like a code bug.
grep -q '^CONFIG_VS_AUDIO_MIC_GAIN=6$' "$AP_OUT/.config" || die "MIC gain is not 6: $(grep '^CONFIG_VS_AUDIO_MIC_GAIN=' "$AP_OUT/.config")"
# The playback ring must hold a whole spoken answer. Anything smaller lets a
# blocked write stop the TTS socket being read, which stalls the server and
# truncates the reply mid-sentence -- observed at the previous 128 KiB.
grep -q '^CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES=655360$' "$AP_OUT/.config" || die "playback ring is not 655360: $(grep '^CONFIG_VS_AUDIO_PLAYBACK_RING_BYTES=' "$AP_OUT/.config")"
# Playback must not start on the first chunk. Synthesis arrives slower than
# real time, so a player with no pre-buffer runs the ring dry between bursts.
grep -q '^CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS=1000$' "$AP_OUT/.config" || die "playback pre-buffer is not 1000 ms: $(grep '^CONFIG_VS_AUDIO_PLAYBACK_PREBUFFER_MS=' "$AP_OUT/.config")"
grep -q 'vs_settings_save_volume' "$AP_OUT/System.map" || die 'volume persistence is not linked'
# The network buffer pool feeds TCP read-ahead. A 16 KiB TLS record needs
# eleven buffers before mbedtls can decrypt it; the pool cannot move to PSRAM
# because iob_initialize() runs before the CP powers PSRAM up.
#
# Raised from 40 to 60, with the throttle halved to 4, because the pool also
# sets the receive window: tcp_get_recvwindow() advertises iob_navail(true) *
# CONFIG_IOB_BUFSIZE. A measured download bottomed out at 9 free of 40, which
# is 1 after the old throttle, and the window collapsed to 1094 bytes while the
# far end sat waiting on it for 20% of the transfer.
#
# The 30 KB this needs comes from JPEG_FAST_BITS in bk7258_jpeg_entropy.c,
# dropped from 12 to 8 so the four Huffman acceleration tables cost 2 KB
# instead of 32 KB. The trade is deliberate: those tables only speed up
# realigning a captured camera frame and have a correctness-preserving
# fallback, while the IOB pool decides how fast every download runs and cannot
# be moved to PSRAM at all.
grep -q '^CONFIG_IOB_NBUFFERS=60$' "$AP_OUT/.config" || die "IOB pool is not 60: $(grep '^CONFIG_IOB_NBUFFERS=' "$AP_OUT/.config")"
grep -q '^CONFIG_IOB_THROTTLE=4$' "$AP_OUT/.config" || die "IOB throttle is not 4: $(grep '^CONFIG_IOB_THROTTLE=' "$AP_OUT/.config")"
for app in KVDB_TOOL AGENT_CAMERA AUDIO_TEST CONV CAMERA_PREVIEW CTRLC_TEST \
           HELLO_SCREEN PERIPH_SELFTEST SOCIAL_CUE SDNAND_INIT WEB_TOOL; do
  grep -q "^CONFIG_LVX_USE_DEMO_CONTEST2026_264_${app}=y$" \
    "$AP_OUT/.config" || die "restored app is not enabled: $app"
done
grep -q 'velasight_font_16_ui' "$AP_OUT/System.map" || die 'UI font is not linked'
grep -q '^ \* Bpp: 1$' "$SCRIPT_DIR/app/velasight/velasight_font_16_ui.c" || die 'UI font is not 1bpp'
grep -q -- '--no-prefilter' "$SCRIPT_DIR/app/velasight/velasight_font_16_ui.c" || die 'UI font is not the pixel-font build'
grep -q 'vs_voice_start' "$AP_OUT/System.map" || die 'idle voice assistant worker is not linked'
grep -q 'vs_audio_volume_set' "$AP_OUT/System.map" || die 'volume page control is not linked'
if grep -q 'voice_vad_process' "$AP_OUT/System.map"; then
  die 'VAD is linked (it was removed; the round ends on the keys, the listening window and the recognizer)'
fi
grep -q 'llm_chat_vision_raw' "$AP_OUT/System.map" || die 'vision model call is not linked'
if grep -qw 'velaclaw_ask' "$AP_OUT/System.map"; then
  die 'velaclaw_ask is linked (decision was to bypass it, see vs_voice.c)'
fi

AP_SIZE=$(stat -c%s -- "$AP_BIN")
AP_LIMIT=$((3904 * 1024))
[ "$AP_SIZE" -le "$AP_LIMIT" ] || die "AP binary exceeds MPU flash region: $AP_SIZE > $AP_LIMIT"

mkdir -p "$(dirname -- "$AP_INPUT")"
cp -- "$AP_BIN" "$AP_INPUT"

restore_cp_config()
{
  if [ "$CP_CONFIG_ARMED" -eq 1 ]; then
    local parent tmp
    parent=$(dirname -- "$CP_CONFIG")
    if ! tmp=$(mktemp "$parent/.velasight-cp-config.XXXXXX"); then
      printf 'error: cannot create CP config restore temporary file\n' >&2
      return 1
    fi
    if ! cp -- "$CP_CONFIG_SEED" "$tmp" \
      || ! chmod --reference="$CP_CONFIG_SEED" "$tmp" \
      || ! mv -f -- "$tmp" "$CP_CONFIG"; then
      rm -f -- "$tmp"
      printf 'error: failed to atomically restore CP config seed: %s\n' "$CP_CONFIG" >&2
      return 1
    fi
    CP_CONFIG_ARMED=0
    printf 'restored CP config seed: %s\n' "$CP_CONFIG"
  fi
}

restore_on_exit()
{
  local status=$?
  trap - EXIT HUP INT TERM
  if ! restore_cp_config; then
    [ "$status" -ne 0 ] || status=1
  fi
  exit "$status"
}

exit_for_signal()
{
  exit "$1"
}

cmp -s "$CP_CONFIG_SEED" "$CP_CONFIG" \
  || die 'CP config is not the authoritative minimal overlay seed'
CP_CONFIG_ARMED=1
trap restore_on_exit EXIT
trap 'exit_for_signal 129' HUP
trap 'exit_for_signal 130' INT
trap 'exit_for_signal 143' TERM

cd "$SDK_ROOT"
make -C projects/app_ab clean
podman run --rm --userns=keep-id \
  -v "$SDK_ROOT:/armino" \
  -w /armino \
  "$ARMINO_IMAGE" \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin

test -s "$GENERATED_CP_CONFIG" || die 'generated CP sdkconfig.h is missing'
grep -q '^#define CONFIG_OPENVELA_AP_CPU1_480M 1$' "$GENERATED_CP_CONFIG" \
  || die 'CP CPU1 480 MHz setting is missing'
grep -q '^#define CONFIG_WIFI_VNET_AP_IPV4 1$' "$GENERATED_CP_CONFIG" \
  || die 'CP OpenVela IPv4 setting is missing'
grep -q '^#define CONFIG_GPIO_DEFAULT_SET_SUPPORT 1$' "$GENERATED_CP_CONFIG" \
  || die 'CP GPIO default support is missing'

restore_cp_config
trap - EXIT HUP INT TERM
sh "$EXTERNAL_PREPARE" check \
  --openvela-root "$OPENVELA_ROOT" \
  --bk-avdk-root "$SDK_ROOT"

PARTITIONS="$SDK_ROOT/projects/app_ab/build/bk7258/app_ab/partitions/partitions.txt"
test -s "$PACKAGE" || die 'all-app.bin is missing'
test -s "$AP_PACKED" || die 'packed AP input is missing'
grep -q 'primary_ap_app.*4148K' "$PARTITIONS" || die 'AP partition is not 4148K'
cmp -s "$AP_BIN" "$AP_INPUT" || die 'external AP copy differs'
cmp -s "$AP_BIN" "$AP_PACKED" || die 'packed AP input differs'

IMAGE_SIZE=$(stat -c%s -- "$PACKAGE")
IMAGE_SHA=$(sha256sum -- "$PACKAGE" | cut -d' ' -f1)
printf 'all-app.bin: %s bytes sha256=%s\n' "$IMAGE_SIZE" "$IMAGE_SHA"

if [ "$DO_FLASH" -eq 1 ]; then
  cd "$SCRIPT_DIR"
  FLASH_ARGS=(-b "$FLASH_BAUD" -B "$CONSOLE_BAUD" -p "$PORT")
  if [ -n "$PORT_NUM" ]; then
    FLASH_ARGS+=(-n "$PORT_NUM")
  fi
  AUTOFLASH_EXPECTED_SIZE="$IMAGE_SIZE" \
    ./autoflash.sh "${FLASH_ARGS[@]}"

  mkdir -p "$LOG_DIR"
  BOOT_LOG="$LOG_DIR/velasight-final-boot-$(date +%Y%m%d-%H%M%S).log"
  stty -F "$PORT" "$CONSOLE_BAUD" cs8 -cstopb -parenb raw -echo -crtscts
  timeout --foreground "$BOOT_LOG_SECONDS" tee "$BOOT_LOG" < "$PORT" >/dev/null &
  CAPTURE_PID=$!
  sleep 0.5
  printf '\035' > "$PORT"
  sleep 0.05
  printf '.' > "$PORT"
  sleep 0.05
  printf '\r\n' > "$PORT"
  sleep 0.05
  printf 'reboot\r\n' > "$PORT"
  wait "$CAPTURE_PID" || true
  printf 'boot log: %s\n' "$BOOT_LOG"
  tr '\r' '\n' < "$BOOT_LOG" | tee "${BOOT_LOG%.log}.normalized.log"
fi
