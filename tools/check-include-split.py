#!/usr/bin/env python3
"""Guardrail for docs/plans/compile-times/include-split.md (Phase 5).

Static (no build needed), so it runs as its own cheap CI job:
  1. The app core headers (AppCommon.h, AppShared.h, AppForward.h) include no
     node, DSP, arrangement or viewport header and not the Platform.h umbrella.
     Every app/ TU reads them, so one such include recompiles all ~78 of them
     whenever that header changes.
  2. Nothing outside src/platform/ includes the Platform.h umbrella - include
     the topic header(s) you use (platform/PlatformAudio.h, ...).
  3. AppShared.h's include closure stays within a budget of project headers.

Usage: python tools/check-include-split.py [--budget N]
Fix a failure by including the header in the .cpp (or app/App<Topic>.h) that
uses it, or by forward-declaring the type in app/AppForward.h.
"""
import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC_DIRS = ["src", "src/core", "src/nodes", "src/platform", "src/audio"]
CORE = ["src/app/AppCommon.h", "src/app/AppShared.h", "src/app/AppForward.h"]
BANNED = [
    (r"(^|/)nodes/", "node header"),
    (r"(^|/)audio/dsp/", "DSP kernel"),
    (r"(^|/)arrange/", "arrangement header (use app/AppArrange.h)"),
    (r"(^|/)NodeViewport\.h$", "viewport header (use app/AppViewports.h)"),
    (r"(^|/)Platform\.h$", "Platform.h umbrella (include the platform/Platform<Topic>.h you use)"),
    (r"(^|/)AudioEngine\.h$", "AudioEngine.h (include it where used)"),
]
# Field headers AppShared.h needs in full (inline device prompt, MainGraphHost).
ALLOWED = {"core/field/FieldDevice.h", "core/field/FieldGraphHost.h"}
DEFAULT_BUDGET = 64  # project headers in AppShared.h's closure; 55 when this was written, 63 after the design
                    # system (TokenColors.h, ActionButton.h: stable, generated tokens every UI file uses) joined it


def includes(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.M)


def resolve(inc, frm):
    for c in [os.path.join(os.path.dirname(frm), inc)] + [os.path.join(REPO, d, inc) for d in INC_DIRS]:
        if os.path.isfile(c):
            return os.path.normpath(c)
    return None


def closure(start):
    seen, todo = set(), [os.path.normpath(start)]
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        for inc in includes(f):
            r = resolve(inc, f)
            if r:
                todo.append(r)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    a = ap.parse_args()
    errors = []

    for core in CORE:
        p = os.path.join(REPO, core)
        for inc in includes(p):
            if inc in ALLOWED:
                continue
            for rx, what in BANNED:
                if re.search(rx, inc):
                    errors.append(f"{core} includes {inc}: {what}")

    umbrella = os.path.normpath(os.path.join(REPO, "src/platform/Platform.h"))
    pdir = os.path.normpath(os.path.join(REPO, "src/platform")) + os.sep
    for d, _, fs in os.walk(os.path.join(REPO, "src")):
        for f in fs:
            if not f.endswith((".cpp", ".mm", ".h", ".hpp")):
                continue
            p = os.path.normpath(os.path.join(d, f))
            if p.startswith(pdir):
                continue
            for inc in includes(p):
                if resolve(inc, p) == umbrella:
                    errors.append(f"{os.path.relpath(p, REPO)} includes the Platform.h umbrella; "
                                  f"include the platform/Platform<Topic>.h it uses")

    src = os.path.normpath(os.path.join(REPO, "src")) + os.sep
    shared = [h for h in closure(os.path.join(REPO, "src/app/AppShared.h")) if h.startswith(src)]
    if len(shared) > a.budget:
        errors.append(f"AppShared.h pulls in {len(shared)} project headers (budget {a.budget}); "
                      f"every app/ TU recompiles when any of them changes")

    for e in errors:
        print("FAIL:", e)
    print(f"AppShared.h closure: {len(shared)} project headers (budget {a.budget}); "
          f"{len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
