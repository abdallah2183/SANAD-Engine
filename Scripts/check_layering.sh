#!/usr/bin/env bash
#
# Scripts/check_layering.sh — enforces the module layering invariants.
#
# Phase 11 established these by moving code; this script is what keeps them from
# quietly coming back. The inversions it guards against appeared as ordinary
# `#include` lines that looked harmless in isolation, so an include-level check
# catches exactly the regression that created them.
#
# It is a grep guard, not a dependency-graph analysis. A dependency that arrives
# transitively through a link library would slip past — but so would nothing
# useful, because a CMake `DEPENDS` alone does not make a translation unit depend
# on a header. The header is the thing that actually couples the code, and the
# header is what this checks.
#
# Usage: bash Scripts/check_layering.sh   (run from the repo root; exits non-zero on violation)

set -u

FAILED=0

# check_no_dep <label> <directory> <pattern> <why>
check_no_dep() {
    local label="$1" dir="$2" pattern="$3" why="$4"
    local hits
    # Comment-only lines are stripped first. The comments in these files explain
    # *why* the dependency was removed, and they necessarily name the types that
    # were removed — flagging them would punish the documentation that stops the
    # regression from coming back. A trailing comment on a line of real code is
    # still caught, because the line does not *begin* with `//`.
    hits=$(grep -rnE "$pattern" "$dir" --include='*.hpp' --include='*.cpp' 2>/dev/null \
           | grep -vE ':[0-9]+:[[:space:]]*(//|\*)' || true)

    if [ -n "$hits" ]; then
        echo "LAYERING VIOLATION — $label"
        echo "  $why"
        echo "  $dir must not reference: $pattern"
        echo "$hits" | sed 's/^/    /'
        echo
        FAILED=1
    else
        echo "ok — $label"
    fi
}

# check_depends <target> <cmakelists> <forbidden target names>
check_depends() {
    local label="$1" file="$2" forbidden="$3"
    local line
    line=$(grep -E '^\s*DEPENDS' "$file" 2>/dev/null || true)

    local bad=0
    for t in $forbidden; do
        case " $line " in
            *" $t "*) bad=1 ;;
        esac
    done

    if [ "$bad" -ne 0 ]; then
        echo "LAYERING VIOLATION — $label"
        echo "  $file must not link: $forbidden"
        echo "    $line"
        echo
        FAILED=1
    else
        echo "ok — $label"
    fi
}

echo "=== NOVAForge layering invariants ==="
echo

# Phase 11, W2. The renderer consumes RenderWorld, which holds no game types.
# The ECS bridge lives in NF/Runtime/SceneExtraction.hpp — the layer that
# legitimately knows about both sides. Without this invariant the renderer cannot
# be built or used standalone, which blocks the dedicated-server path and any
# offscreen tooling.
check_no_dep "Rendering must not depend on ECS/Scene" \
    "Engine/Rendering" \
    'NF/(ECS|Scene)/|(^|[^_[:alnum:]])(ecs|scene)::' \
    "Rendering is a lower layer than the game world it draws."

check_depends "Rendering must not link NFEcs/NFScene" \
    "Engine/Rendering/CMakeLists.txt" \
    "NFEcs NFScene"

# Phase 11, W1. Assets is CPU-pure data: it reads bytes and parses them. The
# mesh type and the GPU are the renderer's business — the StaticMesh/asset
# conversions live in NFRendering (MeshUpload), which depends on NFAssets, never
# the other way around. Before W1, AssetManager held an IGraphicsDevice and
# duplicated the MeshLibrary upload path, which is what dragged both includes
# into this module.
check_no_dep "Assets must not depend on Rendering/RHI" \
    "Engine/Assets" 'NF/(Rendering|RHI)/' \
    "Assets is a lower layer than the renderer."

check_depends "Assets must not link NFRendering" \
    "Engine/Assets/CMakeLists.txt" "NFRendering NFRHI"

echo
if [ "$FAILED" -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
