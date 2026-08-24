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

PORT=${VELASIGHT_PORT:-/dev/ttyUSB0}
PORT_NUM=${VELASIGHT_PORT_NUM:-}
FLASH_BAUD=${VELASIGHT_FLASH_BAUD:-1500000}
CONSOLE_BAUD=${VELASIGHT_CONSOLE_BAUD:-115200}
BOOT_LOG_SECONDS=${VELASIGHT_BOOT_LOG_SECONDS:-25}
DO_FLASH=0

die()
{
  printf 'error: %s\n' "$*" >&2
  exit 1
}

usage()
{
  printf '%s\n' \
    'usage: build_and_flash.sh [--flash] [-p port] [-n loader-port]' \
    '       [--boot-log-seconds seconds]' \
    'Builds ai_agent, regenerates the 4148K-aligned AP layout, packages all-app.bin,' \
    'and optionally flashes it with the shell-based autoflash workflow.'
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --flash) DO_FLASH=1 ;;
    -p) shift; PORT=$1 ;;
    -n) shift; PORT_NUM=$1 ;;
    --boot-log-seconds) shift; BOOT_LOG_SECONDS=$1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

command -v podman >/dev/null 2>&1 || die 'podman is required'
command -v sha256sum >/dev/null 2>&1 || die 'sha256sum is required'
command -v cmp >/dev/null 2>&1 || die 'cmp is required'

cd "$OPENVELA_ROOT"
./build.sh "$AP_CONFIG" --cmake distclean
./build.sh "$AP_CONFIG" --cmake -j8

test -s "$AP_BIN" || die "missing AP binary: $AP_BIN"
grep -q '^CONFIG_LV_TXT_ENC_UTF8=y$' "$AP_OUT/.config" || die 'UTF-8 is disabled'
grep -q '^# CONFIG_LV_FONT_FMT_TXT_LARGE is not set$' "$AP_OUT/.config" || die 'large LVGL font format wastes flash'
grep -q '^CONFIG_VS_SHORT_PRESS_MAX_MS=500$' "$AP_OUT/.config" || die 'short-press limit is not 500 ms'
for app in KVDB_TOOL AGENT_CAMERA AUDIO_TEST CONV CAMERA_PREVIEW CTRLC_TEST \
           HELLO_SCREEN PERIPH_SELFTEST SOCIAL_CUE SDNAND_INIT WEB_TOOL; do
  grep -q "^CONFIG_LVX_USE_DEMO_CONTEST2026_264_${app}=y$" \
    "$AP_OUT/.config" || die "restored app is not enabled: $app"
done
grep -q 'velasight_font_16_ui' "$AP_OUT/System.map" || die 'UI font is not linked'
grep -q '^ \* Bpp: 1$' "$SCRIPT_DIR/app/velasight/velasight_font_16_ui.c" || die 'UI font is not 1bpp'
grep -q -- '--no-prefilter' "$SCRIPT_DIR/app/velasight/velasight_font_16_ui.c" || die 'UI font is not the pixel-font build'
grep -q 'voice_vad_process' "$AP_OUT/System.map" || die 'VAD is not linked'
grep -q 'vs_voice_start' "$AP_OUT/System.map" || die 'idle voice assistant worker is not linked'
grep -q 'llm_chat_vision_raw' "$AP_OUT/System.map" || die 'vision model call is not linked'
if grep -qw 'velaclaw_ask' "$AP_OUT/System.map"; then
  die 'velaclaw_ask is linked (decision was to bypass it, see vs_voice.c)'
fi

AP_SIZE=$(stat -c%s -- "$AP_BIN")
AP_LIMIT=$((3904 * 1024))
[ "$AP_SIZE" -le "$AP_LIMIT" ] || die "AP binary exceeds MPU flash region: $AP_SIZE > $AP_LIMIT"

mkdir -p "$(dirname -- "$AP_INPUT")"
cp -- "$AP_BIN" "$AP_INPUT"

cd "$SDK_ROOT"
make -C projects/app_ab clean
podman run --rm --userns=keep-id \
  -v "$SDK_ROOT:/armino" \
  -w /armino \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin

PARTITIONS="$SDK_ROOT/projects/app_ab/build/bk7258/app_ab/partitions/partitions.txt"
test -s "$PACKAGE" || die 'all-app.bin is missing'
test -s "$AP_PACKED" || die 'packed AP input is missing'
grep -q 'primary_ap_app.*4148K' "$PARTITIONS" || die 'AP partition is not 4148K'
cmp -s "$AP_BIN" "$AP_INPUT" || die 'external AP copy differs'
cmp -s "$AP_BIN" "$AP_PACKED" || die 'packed AP input differs'

IMAGE_SIZE=$(stat -c%s -- "$PACKAGE")
IMAGE_SHA=$(sha256sum -- "$PACKAGE" | cut -d' ' -f1)

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
