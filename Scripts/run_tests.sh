#!/bin/bash
# Scripts/run_tests.sh — run every SANAD test suite and report one summary.
#
# Usage:
#   bash Scripts/run_tests.sh [build-dir]     # default: build/debug
#
# Exits non-zero if any suite fails. GPU suites report SKIPPED (not PASS) on a
# machine without a Vulkan device, so a green run never means "verified nothing".

set -u

BUILD_DIR="${1:-build/debug}"
BIN="${BUILD_DIR}/bin"

if [ ! -d "${BIN}" ]; then
    echo "ERROR: no bin directory at '${BIN}'. Build first, or pass the build dir:" >&2
    echo "       bash Scripts/run_tests.sh build/DebugNinja" >&2
    exit 1
fi

# The suite list used to be a hardcoded array, and it had fallen a phase
# behind: twelve built suites (AITests, DestructionTests, Scene2DTests,
# ScriptTests, UITests, VFXTests, NetworkTests, PerfTests, ...) were silently
# never run — the same "registered but not driven" failure this project keeps
# finding elsewhere, at the runner level. Discovery is the fix: whatever the
# build produced is what gets run, and a suite added later cannot be forgotten.
#
# NF_TEST_SUITES lets a caller add suites that are not on this disk (a remote
# run, a suite renamed during a refactor).
SUITES=()
if [ -d "${BIN}" ]; then
    for exe in "${BIN}"/*Tests.exe "${BIN}"/*Tests; do
        [ -f "${exe}" ] || continue
        name=$(basename "${exe}")
        name=${name%.exe}
        case " ${SUITES[*]:-} " in
            *" ${name} "*) ;;
            *) SUITES+=("${name}") ;;
        esac
    done
fi
for extra in ${NF_TEST_SUITES:-}; do
    case " ${SUITES[*]:-} " in
        *" ${extra} "*) ;;
        *) SUITES+=("${extra}") ;;
    esac
done

TOTAL_PASSED=0
TOTAL_FAILED=0
TOTAL_SKIPPED=0
ANY_FAILURE=0

for suite in "${SUITES[@]}"; do
    exe="${BIN}/${suite}.exe"
    [ -f "${exe}" ] || exe="${BIN}/${suite}"

    if [ ! -f "${exe}" ]; then
        printf 'SKIP  %-14s (not built)\n' "${suite}"
        continue
    fi

    output=$("${exe}" 2>&1)
    code=$?

    summary=$(printf '%s\n' "${output}" | grep -E '^Passed:' | tail -1)
    p=$(printf '%s' "${summary}" | sed -n 's/.*Passed: \([0-9]*\).*/\1/p')
    f=$(printf '%s' "${summary}" | sed -n 's/.*Failed: \([0-9]*\).*/\1/p')
    s=$(printf '%s' "${summary}" | sed -n 's/.*Skipped: \([0-9]*\).*/\1/p')
    TOTAL_PASSED=$((TOTAL_PASSED + ${p:-0}))
    TOTAL_FAILED=$((TOTAL_FAILED + ${f:-0}))
    TOTAL_SKIPPED=$((TOTAL_SKIPPED + ${s:-0}))

    if [ ${code} -eq 0 ]; then
        printf 'PASS  %-14s %s\n' "${suite}" "${summary:-no summary}"
    else
        printf 'FAIL  %-14s exit=%s  %s\n' "${suite}" "${code}" "${summary:-no summary}"
        printf '%s\n' "${output}" | grep -E 'FAILED|SKIPPED|ERROR' | head -20
        ANY_FAILURE=1
    fi
done

echo "------------------------------------------------------------"
printf 'TOTAL  passed=%s failed=%s skipped=%s\n' "${TOTAL_PASSED}" "${TOTAL_FAILED}" "${TOTAL_SKIPPED}"
echo "------------------------------------------------------------"

if [ ${ANY_FAILURE} -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi

if [ ${TOTAL_SKIPPED} -ne 0 ]; then
    echo "RESULT: PASS (${TOTAL_SKIPPED} skipped — see above)"
else
    echo "RESULT: PASS"
fi
exit 0
