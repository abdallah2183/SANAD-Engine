"""One-shot rename: NOVAForge -> SANAD across the tracked tree.

Deterministic, ordered replacements (longest compound first so
NOVAForgeEditor is not half-renamed into SANADEditor by the bare pass),
plus file renames. Excludes tool scratch dirs (.kilo, .workbuddy-ai)
and build output (build/, dist/) which are not part of the repo.
"""
import os
import re
import sys
import subprocess

ROOT = r"C:\Users\abdal\OneDrive\Desktop\NOVAForge Engine"
SKIP_DIRS = {".git", ".kilo", ".workbuddy-ai", "build", "dist", "__pycache__"}
# This script itself (its name contains 'novaforge'); renaming it mid-run
# would be wrong and it is not part of the rename.
SKIP_FILES = {os.path.basename(__file__)}
# Binary extensions we must NOT touch as text.
BINARY_EXT = {
    ".jpg", ".jpeg", ".png", ".ico", ".gif", ".exe", ".dll", ".lib",
    ".pdb", ".zip", ".spv", ".wav", ".ogg", ".mp3", ".flac", ".ttf",
    ".otf", ".bin", ".obj", ".fbx", ".nfmesh", ".pdf", ".woff", ".woff2",
}

# Longest-first: compound identifiers before the bare name.
REPLACEMENTS = [
    ("NOVAForgeProjectLauncher", "SANADProjectLauncher"),
    ("NOVAForgeWindowClass", "SANADWindowClass"),
    ("NOVAForgeDockSpace", "SANADDockSpace"),
    ("NOVAForgeEditor", "SANADEditor"),
    ("NOVAForgeServer", "SANADServer"),
    ("NOVAForgeCook", "SANADCook"),
    ("NOVAForgePlayer", "SANADPlayer"),
    ("NOVAForge", "SANAD"),
    ("NovaForge", "SANAD"),
    ("novaforge", "sanad"),
]

# Files whose NAME contains novaforge/NOVAForge (to be renamed).
def new_name(name):
    for old, new in REPLACEMENTS:
        if old in name:
            name = name.replace(old, new)
    return name


def iter_text_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn in SKIP_FILES:
                continue
            ext = os.path.splitext(fn)[1].lower()
            if ext in BINARY_EXT:
                continue
            yield os.path.join(dirpath, fn)


def main():
    dry = "--dry" in sys.argv
    changed = 0
    renames = []
    for path in iter_text_files():
        try:
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
        except (UnicodeDecodeError, OSError):
            continue  # not a text file we can rewrite
        new = text
        for old, rep in REPLACEMENTS:
            if old in new:
                new = new.replace(old, rep)
        if new != text:
            changed += 1
            if not dry:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(new)
            print("rewrite:", os.path.relpath(path, ROOT))

    # File renames (name contains NOVAForge/novaforge)
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn in SKIP_FILES:
                continue
            nn = new_name(fn)
            if nn != fn:
                src = os.path.join(dirpath, fn)
                dst = os.path.join(dirpath, nn)
                renames.append((src, dst))
                if not dry:
                    os.rename(src, dst)
                print("rename:", os.path.relpath(src, ROOT), "->", nn)

    print(f"\n{'[DRY] ' if dry else ''}files rewritten: {changed}, files renamed: {len(renames)}")


if __name__ == "__main__":
    main()
