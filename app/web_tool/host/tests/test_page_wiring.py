#!/usr/bin/env python3
"""Every element app.js reaches for must exist in index.html.

Why this is a test and not a code review item: `$('id').onclick = ...` on a
missing element throws, and because all the wiring happens in one wire() call,
that single throw leaves *every later button* dead.  The failure looks like
"some buttons do not work" and gives no clue which change caused it -- which is
exactly the report that prompted writing this.
"""

import os
import re
import sys

WEB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "web")

checks = 0
failures = []


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("  FAIL %s" % what)


def main() -> int:
    js = open(os.path.join(WEB, "app.js"), encoding="utf-8").read()
    html = open(os.path.join(WEB, "index.html"), encoding="utf-8").read()

    used = set(re.findall(r"\$\('([^']+)'\)", js))
    # setLamp() takes the id as a plain string argument.
    used |= set(re.findall(r"setLamp\('([^']+)'", js))
    have = set(re.findall(r'id="([^"]+)"', html))

    print("page wiring: %d ids referenced, %d present" % (len(used), len(have)))
    missing = sorted(used - have)
    check(not missing, "app.js references ids that index.html does not have: %s"
          % missing)

    # The module the page imports must be served from the same directory.
    for mod in re.findall(r"from '/static/([^']+)'", js):
        check(os.path.exists(os.path.join(WEB, mod)),
              "imported module %s exists" % mod)

    # Every button that carries a command must carry a non-empty one.
    for val in re.findall(r'data-cmd="([^"]*)"', html):
        check(bool(val.strip()), "data-cmd is not empty")

    # index.html must load app.js as a module, or the import fails at runtime.
    check('type="module"' in html and "app.js" in html,
          "app.js is loaded as an ES module")

    # A one-shot must actually enable host capture, and the backend's exact
    # path must be rendered in the NSH-style web console.  These source-level
    # checks complement test_e2e.py, which proves the event and file but does
    # not execute app.js in a browser DOM.
    grab = re.search(r"\$\('btn-grab'\)\.onclick\s*=\s*async\s*\(\)\s*=>\s*\{(.*?)\n  \};",
                     js, re.S)
    check(grab is not None and
          "op: 'capture', action: 'start'" in grab.group(1) and
          "op: 'capture', action: 'stop'" in grab.group(1),
          "one-shot capture starts and stops host disk capture")
    check(re.search(r"msg\.type\s*===\s*'capture\.saved'.*?addConsole\([^;]*msg\.path",
                    js, re.S) is not None,
          "capture.saved prints the backend path in the web console")

    print("%d checks, %d failure(s)" % (checks, len(failures)))
    for f in failures:
        print("  - %s" % f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
