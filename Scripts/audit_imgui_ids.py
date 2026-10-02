#!/usr/bin/env python3
"""Find ImGui widget-ID collisions in the editor UI, statically.

Why this exists
---------------
In Dear ImGui a widget's ID is derived from its LABEL. Two widgets with the same
label submitted into the same window are the same item as far as ImGui is
concerned, and it answers with a red "Programmer error: N visible items with
conflicting ID!" popup plus a hit target that does nothing.

ImGui can only detect this itself WHILE THE POINTER HOVERS the item —
`ItemHoverable()` says so: "this is specifically done here by comparing on hover
because it allows us a detection of duplicates that is algorithmically extra
cheap". So no automated run finds one, and three separate collisions in this
editor were reported by a human looking at a screenshot. This script finds them
without a GPU, a window, or a mouse.

What it reports
---------------
Keys whose translation is used as a bare label (`AV("key")` with no `##suffix`)
by MORE THAN ONE non-menu widget in the same file. `##anything` makes the ID
unique regardless of the label, and `MenuItem` only ever lives in its own popup
window, so neither is flagged.

The check is a heuristic on purpose: it cannot know window boundaries, so a hit
is a CANDIDATE. Confirm each one by asking "can these two be visible at the same
time?" — e.g. two `Checkbox(AV("enabled"))` inside two default-open inspector
sections: yes, whenever an entity carries both components.

The definitive check is the runtime audit:
    cmake build/DebugNinja -DNF_IMGUI_ID_AUDIT=ON
which makes ImGui track every duplicate ID per frame (slow; audit only) and
main.cpp log each one — but it only sees what actually renders in that run, so
run it with the relevant panel/tab/section open.

Usage:  python Scripts/audit_imgui_ids.py [--all]
        (default: only same-window candidates; --all: every repeated key)
"""

import collections
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_FILES = [
    "Editor/src/ui/Panels.cpp",
    "Editor/src/ui/Toolbar.cpp",
    "Editor/src/ui/FileSystemPanel.cpp",
]

WIDGETS = (
    "Button|SmallButton|CollapsingHeader|Selectable|Checkbox|RadioButton|Combo|"
    "InputText|InputTextWithHint|MenuItem|TreeNode|TreeNodeEx|BeginMenu|"
    "BeginTabItem|BeginChild|Begin"
)
CALL = re.compile(rf"ImGui::({WIDGETS})\s*\(\s*([^;\n]*)")
AV_KEY = re.compile(r'AV\("([a-z0-9_]+)"\)')
# Menu items live in their own popup window, so a shared label cannot collide
# with anything in the panel behind it.
POPUP_ONLY = {"MenuItem"}


def scan(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    found = collections.defaultdict(list)
    for m in CALL.finditer(src):
        widget, args = m.group(1), m.group(2)
        key = AV_KEY.search(args)
        if key is None:
            continue
        found[key.group(1)].append(
            (src[: m.start()].count("\n") + 1, widget, "##" in args)
        )
    return found


def main():
    show_all = "--all" in sys.argv
    problems = 0
    for rel in UI_FILES:
        path = os.path.join(REPO, rel)
        if not os.path.exists(path):
            continue
        for key, hits in sorted(scan(path).items()):
            if len(hits) < 2:
                continue
            bare = [h for h in hits if not h[2]]
            risky = [h for h in bare if h[1] not in POPUP_ONLY]
            if show_all:
                print(f"{os.path.basename(rel)}  '{key}'  x{len(hits)}")
                for line, widget, suffixed in hits:
                    print(f"    line {line:5d}  {widget:18s} "
                          f"{'has ##suffix' if suffixed else 'BARE LABEL'}")
                continue
            if len(risky) > 1:
                problems += 1
                print(f"CANDIDATE  {os.path.basename(rel)}  key='{key}'")
                for line, widget, _ in risky:
                    print(f"    line {line:5d}  {widget}")
    if problems == 0:
        print("No same-window bare-label collisions found.")
    else:
        print(f"\n{problems} candidate(s) — confirm whether they can be visible "
              f"at the same time, then add a ##suffix to one.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
