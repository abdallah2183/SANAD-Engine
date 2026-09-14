# CMake toolchain file for MSVC + Ninja from Bash/PowerShell
# This sets up the MSVC compiler and Windows SDK paths explicitly.

set(MSVC_VER "14.44.35207")
set(VS_ROOT "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools")
set(MSVC_BIN "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/bin/Hostx64/x64")
set(WINSDK_BIN "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
set(WINSDK_INC "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0")
set(WINSDK_LIB "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0")
set(MSVC_INC "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/include")
set(MSVC_LIB "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/lib/x64")

# Compilers
set(CMAKE_C_COMPILER   "${MSVC_BIN}/cl.exe")
set(CMAKE_CXX_COMPILER "${MSVC_BIN}/cl.exe")

# Resource compiler and manifest tool — must be cache variables so CMake's
# internal vs_link_exe helper picks them up as --rc and --mt arguments
# instead of falling back to bare "rc" / "mt" which cmd.exe can't find.
set(CMAKE_RC_COMPILER "${WINSDK_BIN}/rc.exe" CACHE FILEPATH "Resource Compiler")
set(CMAKE_MT_COMPILER  "${WINSDK_BIN}/mt.exe" CACHE FILEPATH "Manifest Tool")
set(CMAKE_AR          "${MSVC_BIN}/lib.exe"  CACHE FILEPATH "Archiver")
set(CMAKE_LINKER      "${MSVC_BIN}/link.exe" CACHE FILEPATH "Linker")

# Environment variables for cl.exe (INCLUDE and LIB)
set(ENV{INCLUDE} "${MSVC_INC};${WINSDK_INC}/ucrt;${WINSDK_INC}/um;${WINSDK_INC}/shared")
set(ENV{LIB}     "${MSVC_LIB};${WINSDK_LIB}/ucrt/x64;${WINSDK_LIB}/um/x64")
set(ENV{PATH}    "${MSVC_BIN};${WINSDK_BIN};$ENV{PATH}")
