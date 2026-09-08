#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK_DIR=${TMPDIR:-/tmp}/velasight-font
FONT_VERSION=17.0.05
FONT_FILE="$WORK_DIR/unifont-$FONT_VERSION.otf"
FONT_URL="https://unifoundry.com/pub/unifont/unifont-$FONT_VERSION/font-builds/unifont-$FONT_VERSION.otf"
FONT_SHA256=85701ab9b1e251ee16f4df00b13f22eac311d72b7dab427a7d975fe7f5064702
CHARSET_BIN="$WORK_DIR/gb2312-charset.bin"
CHARSET_UTF8="$WORK_DIR/gb2312-charset.txt"
OUTPUT="$SCRIPT_DIR/velasight_font_16_ui.c"

mkdir -p "$WORK_DIR"
if [ ! -f "$FONT_FILE" ]; then
  curl --fail --location --output "$FONT_FILE" "$FONT_URL"
fi
printf '%s  %s\n' "$FONT_SHA256" "$FONT_FILE" | sha256sum --check --status

# GB2312 rows A1-A9 are the symbols and punctuation; rows B0-D7 are the 3,755
# level-one Chinese characters.
#
# The symbol rows were missing until now, and their absence was the whole of
# "every punctuation mark is a hollow box".  The level-one rows hold hanzi and
# nothing else, and --range 0x20-0x7f covers only ASCII -- so U+FF0C (，),
# U+3002 (。), U+3001 (、), U+FF1A (：), U+FF1F (？) and the quotation marks had
# no glyph at all.  Every one of those appears in the cloud's own text: the
# summaries, the extreme-emotion advice and the transcript sentences are ordinary
# Chinese prose and are punctuated with them.
#
# All nine rows rather than only the two that carry the common marks.  They are
# what GB2312 defines as its symbol space, so taking them whole is a line that
# does not have to be redrawn the first time the cloud emits a full-width digit
# (row A3), a circled numeral (A2) or a box-drawing rule (A9); the rows are
# sparse and the whole set costs a fraction of the hanzi.
#
# iconv -c drops the cells GB2312 leaves undefined, so the count is checked
# rather than assumed.
#
# The four --range arguments below cover the same ground a second way, and both
# are needed.  GB2312 is a byte encoding and this text is UTF-8, so a round trip
# through iconv lands on whichever code point its table chose: A1AA becomes
# U+2015 rather than U+2014, and A1A4 does not produce U+00B7 at all, so the em
# dash and the middle dot were still missing after taking all nine rows.  Naming
# the Unicode punctuation blocks directly -- U+00B7, U+2010-2027 dashes and
# quotes, U+3000-303F CJK punctuation, U+FF00-FFEF fullwidth forms -- closes
# that, and the overlap costs nothing because lv_font_conv merges the sets.
node - "$CHARSET_BIN" <<'NODE'
const fs = require('fs');
const output = process.argv[2];
const bytes = [];
const rows = [];
for (let row = 0xa1; row <= 0xa9; row++) rows.push(row);
for (let row = 0xb0; row <= 0xd7; row++) rows.push(row);
for (const row of rows) {
  for (let column = 0xa1; column <= 0xfe; column++) {
    bytes.push(row, column);
  }
}
fs.writeFileSync(output, Buffer.from(bytes));
NODE
iconv -c -f GB2312 -t UTF-8 -o "$CHARSET_UTF8" "$CHARSET_BIN"
CHAR_COUNT=$(wc -m < "$CHARSET_UTF8")

# 3755 level-one hanzi plus 682 defined symbol cells across rows A1-A9.
[ "$CHAR_COUNT" -eq 4437 ] || {
  printf 'unexpected GB2312 character count: %s (expected 4437)\n' \
    "$CHAR_COUNT" >&2
  exit 1
}

npx --yes lv_font_conv@1.5.3 \
  --font "$FONT_FILE" \
  --size 16 \
  --bpp 1 \
  --no-prefilter \
  --no-kerning \
  --range 0x20-0x7f \
  --range 0xb7 \
  --range 0x2010-0x2027 \
  --range 0x3000-0x303f \
  --range 0xff00-0xffef \
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
  ' * Source: GNU Unifont 17.0.05; ASCII + GB2312 rows A1-A9 symbols 682' +
  ' + level-one 3755 + U+8046; --bpp 1 --no-prefilter --no-kerning'
);
fs.writeFileSync(path, source.trimEnd() + '\n');
NODE
