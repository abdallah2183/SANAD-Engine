#!/bin/bash
# build/scripts/configure_debug.sh
# Configures the NOVAForge Engine CMake project with MSVC + Ninja.
# This script sets up the necessary environment for cl.exe, rc.exe, mt.exe.

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

export INCLUDE="${MSVC_INC};${WINSDK_INC}/ucrt;${WINSDK_INC}/um;${WINSDK_INC}/shared"
export LIB="${MSVC_LIB};${WINSDK_LIB}/ucrt/x64;${WINSDK_LIB}/um/x64"
export VULKAN_SDK="C:/VulkanSDK/1.4.357.0"
export PATH="${MSVC_BIN}:${WINSDK_BIN}:${PATH}"

PROJECT_ROOT="C:/Users/abdal/OneDrive/Desktop/NOVAForge Engine"

rm -rf "${PROJECT_ROOT}/build/debug"

cd "${PROJECT_ROOT}"
"${CMAKE_EXE}" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="${NINJA_EXE}" \
  -DCMAKE_C_COMPILER="${MSVC_BIN}/cl.exe" \
  -DCMAKE_CXX_COMPILER="${MSVC_BIN}/cl.exe" \
  -DCMAKE_BUILD_TYPE=Debug \
  -B build/debug \
  -DNF_USE_VULKAN=ON \
  -DNF_BUILD_TESTS=ON \
  -DNF_BUILD_SAMPLES=ON "$@"
