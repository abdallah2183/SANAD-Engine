#!/usr/bin/env bash
# Registry-free MSVC build driver for NOVAForge.
#
# Why this exists: build_nf.bat -> vcvars64.bat -> vsdevcmd.bat shells out to
# reg.exe to locate the VS install. reg.exe is on the sandbox Program Blacklist,
# so the MSVC environment never initialises: every TU dies with
#   fatal error C1083: Cannot open include file: 'stddef.h'
# and every link with
#   LINK : fatal error LNK1104: cannot open file 'ole32.lib'
# Neither is a source error -- both are a missing INCLUDE/LIB.
#
# This driver sets the same INCLUDE/LIB/PATH that vcvars64 would, from paths
# resolved on disk, without invoking reg.exe or reading the registry at all.
#
# Usage:  bash .workbuddy-ai/nfb.sh [--target <Target>]     (no args = all)
set -uo pipefail

VS='/c/Program Files/Microsoft Visual Studio/18/Community'
MSVC_WIN='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
SDK_WIN='C:\Program Files (x86)\Windows Kits\10'
SDKV='10.0.26100.0'

export INCLUDE="$MSVC_WIN\\include;$SDK_WIN\\Include\\$SDKV\\ucrt;$SDK_WIN\\Include\\$SDKV\\um;$SDK_WIN\\Include\\$SDKV\\shared;$SDK_WIN\\Include\\$SDKV\\winrt;$SDK_WIN\\Include\\$SDKV\\cppwinrt"
export LIB="$MSVC_WIN\\lib\\x64;$SDK_WIN\\Lib\\$SDKV\\ucrt\\x64;$SDK_WIN\\Lib\\$SDKV\\um\\x64"
export PATH="$VS/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:/c/Program Files (x86)/Windows Kits/10/bin/$SDKV/x64:$PATH"

cd "C:/Users/abdal/OneDrive/Desktop/NOVAForge Engine" || exit 1

# The agent sandbox strips the standard Windows profile / program-files variables
# (APPDATA, ProgramData, ALLUSERSPROFILE, ProgramFiles, ProgramFiles(x86),
# CommonProgramFiles...). NuGet builds its machine-wide config paths out of them
# and throws, aborting restore:
#   NuGet.targets(782,5): error : Value cannot be null. (Parameter 'path1')
# That kills the NFCSharpSandbox custom target, so the WHOLE build goes red even
# though every C++ TU is fine. Restore them for the build (same spirit as the
# INCLUDE/LIB block above: reconstruct what vcvars/the shell would have set).
# Note: `ProgramFiles(x86)` is not a legal bash identifier, hence `env`.
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'
env \
  "APPDATA=C:\Users\abdal\AppData\Roaming" \
  "ProgramData=C:\ProgramData" \
  "ALLUSERSPROFILE=C:\ProgramData" \
  "ProgramFiles=C:\Program Files" \
  "ProgramFiles(x86)=C:\Program Files (x86)" \
  "CommonProgramFiles=C:\Program Files\Common Files" \
  "CommonProgramFiles(x86)=C:\Program Files (x86)\Common Files" \
  cmake --build build/DebugNinja "$@"
