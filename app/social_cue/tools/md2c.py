#!/usr/bin/env python3
"""Generate a C header holding a skill document as a string literal.

The skill text has to exist twice: once as Markdown that a human (or a judge)
reads, and once inside the firmware, because the board has no way to receive a
file from the host.  Rather than maintain both by hand and let them drift, the
Markdown is the source and this script produces the header.

Usage, from the repository root:

    python3 app/social_cue/tools/md2c.py \\
        board/beken/boards/bk7258/bk7258-ap/ai_agent/skills/social-cue-assistant.md \\
        app/social_cue/social_cue_skill.h SOCIAL_CUE_SKILL_MD

The check that they have not drifted is to re-run it and see that git reports
no change.
"""

import pathlib
import sys


def c_escape(line: str) -> str:
    out = line.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{out}\\n"'


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    macro = sys.argv[3]

    text = src.read_text(encoding="utf-8")
    body = " \\\n    ".join(c_escape(l) for l in text.split("\n"))

    guard = "__" + dst.name.upper().replace(".", "_").replace("-", "_")
    header = f"""/****************************************************************************
 * {dst.as_posix()}
 *
 * GENERATED FILE -- do not edit.  Source of truth:
 *   {src.as_posix()}
 * Regenerate with:
 *   python3 app/social_cue/tools/md2c.py \\
 *       {src.as_posix()} \\
 *       {dst.as_posix()} {macro}
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef {guard}
#define {guard}

#define {macro} \\
    {body}

#endif /* {guard} */
"""
    dst.write_text(header, encoding="utf-8")
    print(f"md2c: {src} -> {dst} ({len(text)} bytes of Markdown)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
