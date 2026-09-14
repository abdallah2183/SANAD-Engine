#!/usr/bin/env bash
#
# Scripts/git_repair_ref.sh — repair the current branch ref after a commit.
#
# WHY THIS EXISTS
#
# This repository's main `.git` lives inside OneDrive
# (`C:/Users/abdal/OneDrive/Desktop/NOVAForge Engine/.git`). OneDrive's sync
# engine deletes newly-created *loose* ref files under `.git/refs/`, and the
# directory that holds them, within seconds of them being written.
#
# The symptom is alarming and misleading: `git commit` prints a success line with
# a hash, then `git log` says `your current branch '...' does not have any commits
# yet` and `git status` lists every file as a staged addition. Nothing is lost —
# the commit object and the reflog are intact — but the branch points nowhere.
#
# The durable fix is `git pack-refs`. A packed ref lives in the single existing
# file `.git/packed-refs`, which OneDrive leaves alone. Verified 2026-09-14: after
# packing, the loose file was deleted again within eight seconds and the branch
# still resolved.
#
# Every `git commit` writes a fresh loose ref, so this has to run after each one.
#
# WORKTREE / BRANCH AGNOSTIC
#
# The first version of this script hardcoded the branch it was written for
# (`workbuddy/main-e34f0fa2`). Run from any other worktree it reported "ok" while
# repairing nothing, which is the worst possible failure mode for a recovery
# tool. It now reads the branch from HEAD and resolves the ref and reflog through
# git, so it is correct in every worktree of this repository.
#
# `git symbolic-ref` is used rather than `git rev-parse --abbrev-ref HEAD`
# because it reads the HEAD file directly and therefore still answers after the
# branch ref has been deleted and the branch is "unborn" — which is exactly the
# state this script exists to recover from.
#
# Usage:
#   bash Scripts/git_repair_ref.sh          # repair + verify
#   bash Scripts/git_repair_ref.sh --check  # verify only, exit 1 if broken

set -u

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

# --- Resolve what we are repairing -----------------------------------------
BRANCH="$(git symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
if [ -z "$BRANCH" ]; then
    printf 'ERROR: HEAD is detached or unreadable — there is no branch ref to repair.\n' >&2
    printf '       This script repairs a branch ref, not a detached HEAD.\n' >&2
    exit 1
fi

# `--git-path` resolves through the worktree's `commondir`, so these point at the
# shared `.git` even when the script runs from a linked worktree. Building the
# paths by hand from `--git-common-dir` is the same thing with more ways to go
# wrong, so it is not done here.
REF_FILE="$(git rev-parse --git-path "refs/heads/${BRANCH}")"
REFLOG="$(git rev-parse --git-path "logs/refs/heads/${BRANCH}")"

verify() {
    if git rev-parse HEAD >/dev/null 2>&1; then
        printf 'ok  — %s: %s\n' "$BRANCH" "$(git log --oneline -1)"
        return 0
    fi
    printf 'BROKEN — %s does not resolve\n' "$BRANCH"
    return 1
}

if [ "$CHECK_ONLY" -eq 1 ]; then
    verify || exit 1
    exit 0
fi

if verify >/dev/null 2>&1; then
    verify
    # Even when it resolves it may be resolving from a loose ref that OneDrive is
    # about to delete. Pack it so the next read is from packed-refs.
    git pack-refs --all 2>/dev/null || true
    exit 0
fi

if [ ! -f "$REFLOG" ]; then
    printf 'ERROR: no reflog at %s — cannot recover the branch tip.\n' "$REFLOG" >&2
    printf '       The commit object may still be reachable via `git fsck --lost-found`.\n' >&2
    exit 1
fi

# The reflog survives the ref being deleted, so the last entry is the tip.
FULL=$(tail -1 "$REFLOG" | awk '{print $2}')
case "$FULL" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
    *) printf 'ERROR: reflog tail is not a sha: %s\n' "$FULL" >&2; exit 1 ;;
esac

if [ "${#FULL}" -ne 40 ]; then
    printf 'ERROR: reflog sha is %s chars, expected 40: %s\n' "${#FULL}" "$FULL" >&2
    exit 1
fi

printf 'recovering %s -> %s\n' "$BRANCH" "$FULL"

# `git update-ref` has been observed to report success while the file is deleted
# again immediately, so the ref is written directly. The sha MUST be the full 40
# characters: a short sha gives "warning: ignoring broken ref".
mkdir -p "$(dirname "$REF_FILE")"
printf '%s\n' "$FULL" > "$REF_FILE"

# The durable step.
git pack-refs --all

verify || exit 1
printf 'packed — the loose ref can be deleted again without consequence.\n'
