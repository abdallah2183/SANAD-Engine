#!/usr/bin/env bash
# G8 — the performance contract, driven from one place.
#
# The perf scene (Content/Scenes/Perf.nfscene: ~2k meshes, a shadowed sun, live
# destructibles) is loaded through the real Runtime + Renderer3D path and frame
# time / draw calls / memory are compared against Tests/PerfTests/perf_baseline.csv.
# Any metric more than 10% above its baseline row fails loudly, so a regression
# cannot reach a player unnoticed.
#
# Usage:
#   bash Tests/PerfTests/perf_gate.sh                  # measure AND enforce (exit != 0 on a regression)
#   bash Tests/PerfTests/perf_gate.sh --record         # refresh the baseline CSV from a real run
#   bash Tests/PerfTests/perf_gate.sh --self-test      # prove the gate rejects a deliberate regression
#   bash Tests/PerfTests/perf_gate.sh --binary PATH    # use a specific PerfTests binary
#
# Environment:
#   NF_PERF_MACHINE   machine tag for the machine-dependent rows (frame time,
#                     update time, total working set). Default: win11-x64-dev.
#                     A CI runner records and enforces its own tag — see
#                     .github/workflows/perf-gate.yml.
#   NF_PERF_BASELINE  baseline CSV to read/write. Default: perf_baseline.csv
#                     beside this script.
#
# Exit codes: 0 = the contract holds, 1 = a metric regressed (or the harness
# itself is misconfigured — e.g. the binary or the baseline is missing).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

BASELINE="${NF_PERF_BASELINE:-$HERE/perf_baseline.csv}"
MACHINE="${NF_PERF_MACHINE:-win11-x64-dev}"

MODE="gate"
BINARY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --record)    MODE="record" ;;
    --self-test) MODE="self-test" ;;
    --binary)    shift; BINARY="${1:-}" ;;
    --help|-h)   sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "perf_gate.sh: unknown argument '$1'" >&2; exit 1 ;;
  esac
  shift
done

find_binary() {
  if [ -n "$BINARY" ]; then
    [ -x "$BINARY" ] || [ -f "$BINARY" ] || { echo "perf_gate.sh: no binary at '$BINARY'" >&2; return 1; }
    printf '%s\n' "$BINARY"
    return 0
  fi
  local candidate
  for candidate in \
      "${NF_PERF_BINARY:-}" \
      build/DebugNinja/bin/PerfTests.exe \
      build/ci/bin/PerfTests.exe \
      build/DebugNinja/bin/PerfTests \
      build/ci/bin/PerfTests; do
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  echo "perf_gate.sh: PerfTests binary not found. Build it first:" >&2
  echo "  bash .workbuddy-ai/nfb.sh --target PerfTests    # or: cmake --build build/ci --target PerfTests" >&2
  return 1
}

# The suite is a native Windows binary: it cannot open a Git Bash path like
# /c/Users/... — MSVC's ifstream would just report "file not found" and every
# metric would look unbaselined. Hand it a native path.
to_native() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1"
  else
    printf '%s\n' "$1"
  fi
}

# Run the suite in gate mode. The suite prints PERF_METRIC / PERF_GATE lines as
# it goes, so the log is the evidence, not just a pass/fail bit.
run_gate() {
  local binary="$1" baseline="$2" machine="$3"
  NF_PERF_GATE=1 \
  NF_PERF_BASELINE="$(to_native "$baseline")" \
  NF_PERF_MACHINE="$machine" \
  "$binary"
}

# The suite reads NF_PERF_GATE itself, so a plain run measures without enforcing.
run_measure() {
  local binary="$1" machine="$2" record_to="$3"
  NF_PERF_GATE=0 \
  NF_PERF_RECORD="$(to_native "$record_to")" \
  NF_PERF_MACHINE="$machine" \
  "$binary"
}

BIN="$(find_binary)" || exit 1

case "$MODE" in
  gate)
    [ -f "$BASELINE" ] || { echo "perf_gate.sh: no baseline at '$BASELINE' — run --record first" >&2; exit 1; }
    echo "perf_gate: $BIN"
    echo "perf_gate: baseline=$BASELINE machine=$MACHINE"
    echo "perf_gate: rows enforced"
    grep -v '^#' "$BASELINE" | grep -v '^[[:space:]]*$' | sed 's/^/  /'
    echo "---"
    run_gate "$BIN" "$BASELINE" "$MACHINE"
    code=$?
    if [ "$code" -ne 0 ]; then
      echo "---"
      echo "PERF GATE FAILED (exit $code): the performance contract was violated."
      echo "If the regression is real, fix it. If the baseline is genuinely obsolete,"
      echo "re-record it with: bash Tests/PerfTests/perf_gate.sh --record"
      exit 1
    fi
    echo "---"
    echo "PERF GATE PASSED: every metric is within 10% of its baseline."
    ;;

  record)
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp" >/dev/null 2>&1 || true' EXIT
    echo "perf_gate: recording a fresh baseline from $BIN (machine tag '$MACHINE')"
    run_measure "$BIN" "$MACHINE" "$tmp/metrics.csv"
    code=$?
    if [ "$code" -ne 0 ]; then
      echo "perf_gate: the measuring run failed (exit $code) — refusing to record a baseline from it" >&2
      exit 1
    fi
    # One line per metric, last writer wins, sorted so the file is stable in
    # version control and a diff shows exactly which numbers moved.
    awk -F, '!/^#/ && NF>=3 { row[$1]=$0 } END { for (m in row) print row[m] }' \
      "$tmp/metrics.csv" | sort > "$tmp/sorted.csv"
    {
      echo "# G8 performance contract — baseline for Tests/PerfTests/test_perf_scene.cpp"
      echo "#"
      echo "# Recorded by: bash Tests/PerfTests/perf_gate.sh --record"
      echo "# Machine tag: $MACHINE  (machine-dependent rows are enforced only on this tag)"
      echo "# Format: metric,machine,value,tolerance_pct"
      echo "# A row tagged 'any' is enforced on every machine; 'any' rows are the"
      echo "# deterministic counters and the scene-attributable memory delta."
      echo "#"
      echo "# metric,machine,value,tolerance_pct"
      cat "$tmp/sorted.csv"
    } > "$BASELINE"
    echo "perf_gate: wrote $BASELINE"
    cat "$BASELINE"
    ;;

  self-test)
    # The negative control. A gate nobody has seen reject anything is a
    # decoration, so this deliberately makes the baseline unachievable and
    # asserts the gate goes red. It runs on any machine: the rows it leans on
    # are the 'any' counters, which are enforced everywhere.
    [ -f "$BASELINE" ] || { echo "perf_gate.sh: no baseline at '$BASELINE' — run --record first" >&2; exit 1; }
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp" >/dev/null 2>&1 || true' EXIT

    echo "=== self-test 1/3: the real baseline must PASS ==="
    if ! run_gate "$BIN" "$BASELINE" "$MACHINE" > "$tmp/pass.log" 2>&1; then
      echo "self-test FAILED: the gate rejects the committed baseline." >&2
      tail -30 "$tmp/pass.log" >&2
      exit 1
    fi
    grep -E 'PERF_GATE ok|PERF_GATE report-only' "$tmp/pass.log" | sed 's/^/  /'
    echo "  -> PASS, as expected."

    echo
    echo "=== self-test 2/3: a deliberate 2x regression on EVERY metric must FAIL ==="
    # Halving every baseline value means the measured numbers are now 2x over
    # the line — exactly the shape of "the engine got twice as slow".
    awk -F, 'BEGIN{OFS=","} /^#/ { print; next } NF>=3 { $3=$3*0.5; print }' \
      "$BASELINE" > "$tmp/regressed.csv"
    if run_gate "$BIN" "$tmp/regressed.csv" "$MACHINE" > "$tmp/fail.log" 2>&1; then
      echo "self-test FAILED: a 2x regression was ACCEPTED — the gate does not bite." >&2
      exit 1
    fi
    rejected=$(grep -c 'PERF GATE FAILED' "$tmp/fail.log" || true)
    echo "  metrics rejected: $rejected"
    grep 'PERF GATE FAILED' "$tmp/fail.log" | head -10 | sed 's/^/  /'
    echo "  -> FAILED loudly, as expected."

    echo
    echo "=== self-test 3/3: a frame-time-only regression must FAIL on frame_time_ms ==="
    # Step 2 trips on the first metric each test happens to emit, so it does not
    # by itself prove the frame-time row bites. Here only the timing rows are
    # halved: every counter still passes, and the run must die on frame_time_ms.
    awk -F, 'BEGIN{OFS=","} /^#/ { print; next }
             NF>=3 && ($1=="frame_time_ms" || $1=="update_ms") { $3=$3*0.5; print; next }
             NF>=3 { print }' "$BASELINE" > "$tmp/slow.csv"
    if run_gate "$BIN" "$tmp/slow.csv" "$MACHINE" > "$tmp/slow.log" 2>&1; then
      echo "self-test FAILED: a frame-time regression was ACCEPTED." >&2
      exit 1
    fi
    if ! grep -q 'PERF GATE FAILED: frame_time_ms' "$tmp/slow.log"; then
      echo "self-test FAILED: the run went red, but not on frame_time_ms:" >&2
      grep 'FAILED' "$tmp/slow.log" | head -5 >&2
      exit 1
    fi
    grep 'PERF GATE FAILED: frame_time_ms' "$tmp/slow.log" | sed 's/^/  /'
    echo "  -> FAILED on the frame-time row, as expected."
    echo
    echo "SELF-TEST PASSED: the gate accepts a matching run and rejects regressions in"
    echo "the counters and in frame time."
    ;;

  *)
    echo "perf_gate.sh: unknown mode '$MODE'" >&2
    exit 1
    ;;
esac
