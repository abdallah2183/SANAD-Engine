---
name: onedrive-git-recovery
description: >
  Diagnose and repair this repo's .git after OneDrive sync corruption — stale
  main ref, missing commit objects, broken pack files, or a fossil index.
  Use when git suddenly shows an OLD tip, "fatal: bad object HEAD",
  "unable to read <sha>", "not a valid object", fetch fails with
  "did not send all necessary objects", or fsck reports invalid sha1
  pointers / failed pack loads. Also covers the do-not list while corrupted.
---

# OneDrive Git Recovery — NOVAForge Engine

This workspace lives under OneDrive Desktop. OneDrive sync races can restore
stale `.git` refs from the cloud, clobber loose objects, and corrupt pack
files. The 2026-09-22 incident destroyed the metadata of ~14 unpushed commits
(ff418d0..33a08a0) while the working tree survived intact — the recipes below
are what recovered it, plus the rules that prevent a repeat.

## Hard rules (while corrupted, and always)

1. **NEVER `git stash` in this workspace.** The incident was triggered by a
   `git stash push` racing OneDrive. For A/B experiments, copy files aside
   explicitly and restore them by copy.
2. **NEVER `git reset --hard`, `git gc`, `git repack`, or `git prune`** while
   corrupted — the working tree is the only surviving copy of recent work.
3. **Push immediately after every commit.** Unpushed commits are one sync
   race from oblivion; the remote is the real backup.
4. Diagnose before touching anything: is the ref stale, are objects missing,
   or are packs corrupt? Each has its own repair.

## Symptom → cause map

- `git log` shows an OLD tip while `git status` shows huge diffs →
  OneDrive restored a stale `refs/heads/main`.
- `fatal: bad object HEAD` / `cat-file -t <sha>` says not valid for recent
  commits → loose objects clobbered.
- `git fetch` fails "did not send all necessary objects" after naming some
  ref → that ref is dead and poisons fetch negotiation.
- `git reset` fails "unable to read <sha>" while `read-tree HEAD` succeeds →
  the index's cache-tree references never-pushed objects (fossil index).
- fsck: "failed to load pack in position N" → pack file corrupted.

## Recovery order (verified 2026-09-22)

1. Inventory, touch nothing yet:
   - `git log --oneline -3`, `git status --short`
   - `tail .git/logs/HEAD` (real last position of HEAD)
   - `git ls-remote origin main` (what the remote actually has)
   - `git cat-file -t <sha>` for each reflog tip → which are missing
2. Delete every dead ref — each one poisons fetch negotiation:
   ```bash
   git for-each-ref --format='%(refname) %(objectname)' | while read r sha; do
     git cat-file -e "$sha" 2>/dev/null || git update-ref -d "$r"
   done
   ```
   (Incident dead refs: `refs/cline/checkpoints/*`, `refs/heads/workbuddy/*`.)
3. If `refs/heads/main` points at an object that is missing but the REMOTE
   has a newer tip → the ref is stale; fetch will reconcile. If the remote
   is OLDER than the lost local tip → those commits are gone (metadata);
   plan a recovery commit.
4. Quarantine corrupt packs (do NOT delete — OneDrive cloud version history
   may hold intact copies):
   ```bash
   mkdir -p .git/objects/pack_quarantine
   mv .git/objects/pack/pack-<id>* .git/objects/pack_quarantine/
   git fetch origin        # re-downloads clean objects over the network
   ```
5. Rebuild the index when plain `git reset` fails on an unreadable cache-tree
   object: `git read-tree HEAD && git reset`.
6. Verify: `git fsck --connectivity-only` (stale HEAD reflog entries are
   harmless noise), `git status --short` (expect only real changes),
   `git log --oneline -1`.
7. Re-record lost work as ONE recovery commit describing the incident and
   what it contains: `git add -A && git commit`, then
   `bash Scripts/git_repair_ref.sh` (mandatory under OneDrive), then push.
8. If anything is unclear, stop and check OneDrive web (Recycle bin +
   version history on `.git/objects/**`) BEFORE recommitting — original
   objects restorable there beat a squashed re-record.

## Calibration data from the 2026-09-22 incident

- Trigger: `git stash push -- Samples/Basic3D/shaders/brdf.glsl` during an
  active OneDrive sync.
- Lost: commit objects for ff418d0..33a08a0 + both pack files; refs restored
  to a stale tip (0499102, CSM-era).
- Survived: entire working tree, reflog file, remote.
- Recovery: dead-ref purge → pack quarantine → fetch → read-tree + reset →
  recovery commit bb27b1f → repair script → push.
- Post-recovery sweep: 1542 passed / 0 failed / 2 skipped, 26 suites.
