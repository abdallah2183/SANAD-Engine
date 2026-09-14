#!/bin/bash
# Scripts/build.sh — Configure + Build NOVAForge Engine
# Sets up the MSVC environment for cl.exe, rc.exe, mt.exe, then configures and builds.
#
# Usage:
#   bash Scripts/build.sh            # Debug configure + build
#   bash Scripts/build.sh rebuild     # Clean + configure + build
#   bash Scripts/build.sh release     # Release build

set -e

MSVC_VER="14.44.35207"
VS_ROOT="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
MSVC_BIN="${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/bin/Hostx64/x64"
WINSDK_BIN="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
MSVC_INC="${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/include"
MSVC_LIB="${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/lib/x64"
WINSDK_INC="C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0"
WINSDK_LIB="C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0"
CMAKE_EXE="${VS_ROOT}/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA_EXE="${VS_ROOT}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

# --- Environment that cl.exe/rc.exe/mt.exe/link.exe need at BUILD time ---
export INCLUDE="${MSVC_INC};${WINSDK_INC}/ucrt;${WINSDK_INC}/um;${WINSDK_INC}/shared"
export LIB="${MSVC_LIB};${WINSDK_LIB}/ucrt/x64;${WINSDK_LIB}/um/x64"
export VULKAN_SDK="C:/VulkanSDK/1.4.357.0"
export PATH="${MSVC_BIN}:${WINSDK_BIN}:${PATH}"

PROJECT_ROOT="C:/Users/abdal/OneDrive/Desktop/NOVAForge Engine"
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
        -DNF_BUILD_SAMPLES=ON
fi

echo "=== Building (${BUILD_DIR}) ==="
"${CMAKE_EXE}" --build "${BUILD_DIR}" "${PASSTHRU_ARGS[@]+"${PASSTHRU_ARGS[@]}"}"
