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
# THE STALE-PACKED-REF TRAP
#
# "Does HEAD resolve?" is NOT sufficient to detect the failure, and this script
# originally got that wrong. Once a branch has been packed once, deleting the new
# loose ref leaves the *previous* tip in `packed-refs`, so HEAD keeps resolving —
# to the old commit. The check reported `ok` while the commit that had just been
# made was unreferenced, which is the one outcome a recovery tool must never
# produce. The comparison is therefore against the reflog, which is written before
# the ref and is the only record of the true tip.
#
# WORKTREE / BRANCH AGNOSTIC
#
# An earlier version hardcoded the branch it was written for
# (`workbuddy/main-e34f0fa2`), so from any other worktree it repaired nothing.
# The branch is read from HEAD and the ref and reflog are resolved through git.
#
# `git symbolic-ref` is used rather than `git rev-parse --abbrev-ref HEAD`
# because it reads the HEAD file directly and therefore still answers after the
# branch ref has been deleted and the branch is "unborn".
#
# Usage:
#   bash Scripts/git_repair_ref.sh          # repair + verify
#   bash Scripts/git_repair_ref.sh --check  # verify only, exit 1 if broken or stale

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
# shared `.git` even when the script runs from a linked worktree.
REF_FILE="$(git rev-parse --git-path "refs/heads/${BRANCH}")"
REFLOG="$(git rev-parse --git-path "logs/refs/heads/${BRANCH}")"

# The tip the branch is supposed to have. The reflog is written before the ref and
# survives the ref being deleted, so its last entry is the authority.
reflog_tip() {
    [ -f "$REFLOG" ] || return 1
    tail -1 "$REFLOG" | awk '{print $2}'
}

verify() {
    local expected current
    expected="$(reflog_tip || true)"

    if ! current="$(git rev-parse HEAD 2>/dev/null)"; then
        printf 'BROKEN — %s does not resolve\n' "$BRANCH"
        return 1
    fi

    # Resolving is not enough: a stale packed ref resolves too. See the
    # STALE-PACKED-REF TRAP note at the top of this file.
    if [ -n "$expected" ] && [ "$expected" != "$current" ]; then
        printf 'STALE — %s resolves to %s but the reflog tip is %s\n' \
            "$BRANCH" "${current:0:12}" "${expected:0:12}"
        return 1
    fi

    printf 'ok  — %s: %s\n' "$BRANCH" "$(git log --oneline -1)"
    return 0
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

FULL="$(reflog_tip)"
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

# The durable step. A loose ref alone wins over the stale packed one, but only
# until OneDrive deletes it; packing folds it into the file OneDrive leaves alone.
git pack-refs --all

verify || exit 1
printf 'packed — the loose ref can be deleted again without consequence.\n'
