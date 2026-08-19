#!/usr/bin/env bash
# Render the spoken volume confirmation the board plays back.
#
# The clip is checked in, so this script is not part of the build -- it is here
# so the phrase can be changed, and so the one that is checked in can be
# reproduced rather than being a binary nobody can account for.
#
# Why edge-tts: this machine has no offline TTS at all (no espeak, no
# pico2wave, no festival) and installing one needs root, while edge-tts is a
# pip --user install and needs no API key.  It does need to reach Microsoft
# once, which is why the output is committed instead of rendered at build time.
#
#   python3 -m pip install --user edge-tts     # once
#   ./make_announce.sh                         # writes web/announce/VOLSET.PCM
#
# Format is dictated by the board: mono signed 16-bit little-endian at 8 kHz,
# no header, because that is what the DAC takes and there is no decoder in the
# path.  8 kHz rather than 16 keeps the clip well inside the protocol's 64 KB
# frame; see WT_ANNOUNCE_RATE in wt_command.c.

set -euo pipefail

TEXT="${1:-音量设置成功}"
VOICE="${VOICE:-zh-CN-XiaoxiaoNeural}"
RATE="${RATE:-8000}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$HERE/web/announce"
OUT="$OUT_DIR/VOLSET.PCM"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

EDGE_TTS="$(command -v edge-tts || echo "$HOME/.local/bin/edge-tts")"
if [ ! -x "$EDGE_TTS" ]; then
  echo "make_announce: edge-tts not found." >&2
  echo "  python3 -m pip install --user edge-tts" >&2
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "make_announce: ffmpeg not found (needed to resample to raw PCM)." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "make_announce: rendering \"$TEXT\" with $VOICE"
"$EDGE_TTS" --voice "$VOICE" --text "$TEXT" --write-media "$TMP/say.mp3"

# -af loudnorm would be the obvious thing here and is deliberately not used:
# the clip is what makes the *volume setting* audible, so any normalisation on
# this side would flatten exactly the difference the operator is listening for.
ffmpeg -hide_banner -loglevel error -y -i "$TMP/say.mp3" \
  -ar "$RATE" -ac 1 -f s16le -acodec pcm_s16le "$TMP/say.pcm"

mv "$TMP/say.pcm" "$OUT"

BYTES=$(stat -c%s "$OUT")
python3 - "$BYTES" "$RATE" <<'PY'
import sys
n, rate = int(sys.argv[1]), int(sys.argv[2])
print("make_announce: %d bytes = %.2f s at %d Hz mono s16le"
      % (n, n / 2 / rate, rate))
limit = 65536 - 8
if n > limit:
    print("make_announce: WARNING %d bytes exceeds the frame payload limit "
          "(%d); shorten the phrase or lower the rate" % (n, limit))
PY

echo "make_announce: wrote $OUT"
echo "make_announce: put it on the board once, at"
echo "               /mnt/sdnand/ai_agent/VOLSET.PCM"
