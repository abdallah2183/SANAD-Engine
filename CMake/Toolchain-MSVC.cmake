# CMake toolchain file for MSVC + Ninja from Bash/PowerShell
#
# Locates the Visual Studio install, the MSVC toolset, and the Windows SDK
# automatically (newest of each). Previously these were hardcoded, so upgrading
# Visual Studio silently broke the build until someone edited this file.
# Override the VS root with the NF_VS_ROOT environment variable.

# --- Visual Studio root -----------------------------------------------------
if(DEFINED ENV{NF_VS_ROOT} AND NOT "$ENV{NF_VS_ROOT}" STREQUAL ""
   AND EXISTS "$ENV{NF_VS_ROOT}/VC/Tools/MSVC")
    set(VS_ROOT "$ENV{NF_VS_ROOT}")
else()
    set(_nf_vs_candidates
        "C:/Program Files/Microsoft Visual Studio/18/Community"
        "C:/Program Files/Microsoft Visual Studio/18/Professional"
        "C:/Program Files/Microsoft Visual Studio/18/Enterprise"
        "C:/Program Files/Microsoft Visual Studio/18/BuildTools"
        "C:/Program Files/Microsoft Visual Studio/2022/Community"
        "C:/Program Files/Microsoft Visual Studio/2022/Professional"
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools")
    set(VS_ROOT "")
    foreach(_nf_cand IN LISTS _nf_vs_candidates)
        if(EXISTS "${_nf_cand}/VC/Tools/MSVC")
            set(VS_ROOT "${_nf_cand}")
            break()
        endif()
    endforeach()
endif()

if(NOT VS_ROOT)
    message(FATAL_ERROR
        "[NF] No Visual Studio install found. Set NF_VS_ROOT to your VS root, e.g.\n"
        "     NF_VS_ROOT='C:/Program Files/Microsoft Visual Studio/18/Community'")
endif()

# --- Newest MSVC toolset under that install ---------------------------------
file(GLOB _nf_toolsets LIST_DIRECTORIES true "${VS_ROOT}/VC/Tools/MSVC/*")
set(MSVC_VER "")
foreach(_nf_dir IN LISTS _nf_toolsets)
    get_filename_component(_nf_name "${_nf_dir}" NAME)
    if(_nf_name MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        if(NOT MSVC_VER OR _nf_name VERSION_GREATER MSVC_VER)
            set(MSVC_VER "${_nf_name}")
        endif()
    endif()
endforeach()
if(NOT MSVC_VER)
    message(FATAL_ERROR "[NF] No MSVC toolset found under ${VS_ROOT}/VC/Tools/MSVC")
endif()

# --- Newest Windows SDK -----------------------------------------------------
set(_nf_sdk_inc_root "C:/Program Files (x86)/Windows Kits/10/Include")
file(GLOB _nf_sdks LIST_DIRECTORIES true "${_nf_sdk_inc_root}/*")
set(WINSDK_VER "")
foreach(_nf_dir IN LISTS _nf_sdks)
    get_filename_component(_nf_name "${_nf_dir}" NAME)
    if(_nf_name MATCHES "^10\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
        if(NOT WINSDK_VER OR _nf_name VERSION_GREATER WINSDK_VER)
            set(WINSDK_VER "${_nf_name}")
        endif()
    endif()
endforeach()
if(NOT WINSDK_VER)
    message(FATAL_ERROR "[NF] No Windows 10/11 SDK found under ${_nf_sdk_inc_root}")
endif()

message(STATUS "[NF] Toolchain: VS=${VS_ROOT}  MSVC=${MSVC_VER}  SDK=${WINSDK_VER}")

set(MSVC_BIN   "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/bin/Hostx64/x64")
set(WINSDK_BIN "C:/Program Files (x86)/Windows Kits/10/bin/${WINSDK_VER}/x64")
set(MSVC_INC   "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/include")
set(MSVC_LIB   "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VER}/lib/x64")
set(WINSDK_INC "${_nf_sdk_inc_root}/${WINSDK_VER}")
set(WINSDK_LIB "C:/Program Files (x86)/Windows Kits/10/Lib/${WINSDK_VER}")

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
