#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK_DIR=${TMPDIR:-/tmp}/velasight-font
FONT_VERSION=17.0.05
FONT_FILE="$WORK_DIR/unifont-$FONT_VERSION.otf"
FONT_URL="https://unifoundry.com/pub/unifont/unifont-$FONT_VERSION/font-builds/unifont-$FONT_VERSION.otf"
FONT_SHA256=85701ab9b1e251ee16f4df00b13f22eac311d72b7dab427a7d975fe7f5064702
CHARSET_BIN="$WORK_DIR/gb2312-level1.bin"
CHARSET_UTF8="$WORK_DIR/gb2312-level1.txt"
OUTPUT="$SCRIPT_DIR/velasight_font_16_ui.c"

mkdir -p "$WORK_DIR"
if [ ! -f "$FONT_FILE" ]; then
  curl --fail --location --output "$FONT_FILE" "$FONT_URL"
fi
printf '%s  %s\n' "$FONT_SHA256" "$FONT_FILE" | sha256sum --check --status

# GB2312 rows B0-D7 are the 3,755 level-one Chinese characters.
node - "$CHARSET_BIN" <<'NODE'
const fs = require('fs');
const output = process.argv[2];
const bytes = [];
for (let row = 0xb0; row <= 0xd7; row++) {
  for (let column = 0xa1; column <= 0xfe; column++) {
    bytes.push(row, column);
  }
}
fs.writeFileSync(output, Buffer.from(bytes));
NODE
iconv -c -f GB2312 -t UTF-8 -o "$CHARSET_UTF8" "$CHARSET_BIN"
CHAR_COUNT=$(wc -m < "$CHARSET_UTF8")
[ "$CHAR_COUNT" -eq 3755 ] || {
  printf 'unexpected GB2312 level-one character count: %s\n' "$CHAR_COUNT" >&2
  exit 1
}

npx --yes lv_font_conv@1.5.3 \
  --font "$FONT_FILE" \
  --size 16 \
  --bpp 1 \
  --no-prefilter \
  --no-kerning \
  --range 0x20-0x7f \
  --symbols "$(tr -d '\n' < "$CHARSET_UTF8")聆" \
  --format lvgl \
  --output "$OUTPUT" \
  --lv-font-name velasight_font_16_ui \
  --force-fast-kern-format

# Keep generated metadata reviewable and independent of temporary paths.
node - "$OUTPUT" <<'NODE'
const fs = require('fs');
const path = process.argv[2];
let source = fs.readFileSync(path, 'utf8');
source = source.replace(
  /^ \* Opts:.*$/m,
  ' * Source: GNU Unifont 17.0.05; ASCII + GB2312 level-one 3755 + U+8046; --bpp 1 --no-prefilter --no-kerning'
);
fs.writeFileSync(path, source.trimEnd() + '\n');
NODE
