#!/bin/bash
# Scripts/build.sh — Configure + Build NOVAForge Engine
# Sets up the MSVC environment for cl.exe, rc.exe, mt.exe, then configures and builds.
#
# Usage:
#   bash Scripts/build.sh            # Debug configure + build
#   bash Scripts/build.sh rebuild     # Clean + configure + build
#   bash Scripts/build.sh release     # Release build
#
# The project root is derived from this script's location, so running it from a
# git worktree builds THAT worktree. It used to be hardcoded to the main
# checkout, which meant running it from a worktree silently built a different
# tree and reported a green result for it.
#
# The Visual Studio install, MSVC toolset, and Windows SDK are auto-detected
# (newest of each). Override with NF_VS_ROOT if detection picks the wrong one.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Path helpers -----------------------------------------------------------
# PATH must use POSIX form: colon-separated Windows paths like "C:/x:y" are
# mis-parsed by MSYS, which makes cl.exe undiscoverable.
to_posix() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -u "$1"
    else
        printf '/%s%s' "$(printf '%s' "${1:0:1}" | tr 'A-Z' 'a-z')" "${1:2}"
    fi
}

# Newest version-sorted entry in a directory. Usage: newest_dir "<parent dir>"
# The glob must be expanded INSIDE the function: passing a pattern in as a
# string word-splits it, and the Visual Studio path contains spaces.
newest_dir() {
    local parent="$1"
    local best="" name d
    for d in "$parent"/*/; do
        [ -d "${d}" ] || continue
        name="$(basename "${d}")"
        if [ -z "${best}" ] || [ "$(printf '%s\n%s\n' "${best}" "${name}" | sort -V | tail -1)" = "${name}" ]; then
            best="${name}"
        fi
    done
    [ -n "${best}" ] && printf '%s' "${best}"
    return 0
}

# --- Locate the toolchain ---------------------------------------------------
find_vs_root() {
    if [ -n "${NF_VS_ROOT:-}" ] && [ -d "${NF_VS_ROOT}/VC/Tools/MSVC" ]; then
        printf '%s' "${NF_VS_ROOT}"
        return 0
    fi
    local candidates=(
        "C:/Program Files/Microsoft Visual Studio/18/Community"
        "C:/Program Files/Microsoft Visual Studio/18/Professional"
        "C:/Program Files/Microsoft Visual Studio/18/Enterprise"
        "C:/Program Files/Microsoft Visual Studio/18/BuildTools"
        "C:/Program Files/Microsoft Visual Studio/2022/Community"
        "C:/Program Files/Microsoft Visual Studio/2022/Professional"
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
    )
    local c
    for c in "${candidates[@]}"; do
        if [ -d "${c}/VC/Tools/MSVC" ]; then
            printf '%s' "${c}"
            return 0
        fi
    done
    return 1
}

if ! VS_ROOT="$(find_vs_root)"; then
    echo "ERROR: no Visual Studio install found." >&2
    echo "       Set NF_VS_ROOT to your VS root, e.g." >&2
    echo "       NF_VS_ROOT='C:/Program Files/Microsoft Visual Studio/18/Community' bash Scripts/build.sh" >&2
    exit 1
fi

MSVC_VER="$(newest_dir "${VS_ROOT}/VC/Tools/MSVC")"
WINSDK_INC_ROOT="C:/Program Files (x86)/Windows Kits/10/Include"
WINSDK_VER="$(newest_dir "${WINSDK_INC_ROOT}")"

if [ -z "${MSVC_VER}" ] || [ -z "${WINSDK_VER}" ]; then
    echo "ERROR: could not detect an MSVC toolset or Windows SDK." >&2
    echo "       VS_ROOT=${VS_ROOT}  MSVC_VER=${MSVC_VER:-<none>}  WINSDK_VER=${WINSDK_VER:-<none>}" >&2
    exit 1
fi

MSVC_BIN="${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/bin/Hostx64/x64"
WINSDK_BIN="C:/Program Files (x86)/Windows Kits/10/bin/${WINSDK_VER}/x64"
MSVC_INC="${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/include"
MSVC_LIB="${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/lib/x64"
WINSDK_INC="${WINSDK_INC_ROOT}/${WINSDK_VER}"
WINSDK_LIB="C:/Program Files (x86)/Windows Kits/10/Lib/${WINSDK_VER}"
CMAKE_EXE="${VS_ROOT}/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA_EXE="${VS_ROOT}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
export VULKAN_SDK="${VULKAN_SDK:-C:/VulkanSDK/1.4.357.0}"

if [ ! -x "${CMAKE_EXE}" ]; then
    echo "ERROR: cmake not found at ${CMAKE_EXE}" >&2
    echo "       Install the 'C++ CMake tools for Windows' component, or set NF_VS_ROOT." >&2
    exit 1
fi

# --- Environment that cl.exe/rc.exe/mt.exe/link.exe need at BUILD time -------
export INCLUDE="${MSVC_INC};${WINSDK_INC}/ucrt;${WINSDK_INC}/um;${WINSDK_INC}/shared;${VULKAN_SDK}/Include"
export LIB="${MSVC_LIB};${WINSDK_LIB}/ucrt/x64;${WINSDK_LIB}/um/x64;${VULKAN_SDK}/Lib"
export PATH="$(to_posix "${MSVC_BIN}"):$(to_posix "${WINSDK_BIN}"):$(to_posix "${VULKAN_SDK}/Bin"):${PATH}"

echo "=== Toolchain ==="
echo "  project : ${PROJECT_ROOT}"
echo "  VS      : ${VS_ROOT}"
echo "  MSVC    : ${MSVC_VER}"
echo "  SDK     : ${WINSDK_VER}"

BUILD_TYPE="Debug"
BUILD_DIR="build/debug"
DO_CLEAN=0
PASSTHRU_ARGS=()

# Parse args
for arg in "$@"; do
    case "$arg" in
        release)    BUILD_TYPE="Release"; BUILD_DIR="build/release" ;;
        rebuild)   DO_CLEAN=1 ;;
        *)         PASSTHRU_ARGS+=("$arg") ;;
    esac
done

cd "${PROJECT_ROOT}"

if [ "$DO_CLEAN" = "1" ] || [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "=== Configuring (${BUILD_TYPE}) ==="
    rm -rf "${BUILD_DIR}"
    "${CMAKE_EXE}" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="${NINJA_EXE}" \
        -DCMAKE_TOOLCHAIN_FILE="CMake/Toolchain-MSVC.cmake" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -B "${BUILD_DIR}" \
        -DNF_USE_VULKAN=ON \
        -DNF_BUILD_TESTS=ON \
        -DNF_BUILD_SAMPLES=ON \
        -DNF_BUILD_EDITOR=ON \
        -DNF_BUILD_TOOLS=ON
fi

echo "=== Building (${BUILD_DIR}) ==="
"${CMAKE_EXE}" --build "${BUILD_DIR}" "${PASSTHRU_ARGS[@]+"${PASSTHRU_ARGS[@]}"}"
