cmake_minimum_required(VERSION 3.24)

project(NOVAForge
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "NOVAForge Engine - High-performance cross-platform game engine"
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(NF_BUILD_TESTS       "Build unit tests"            ON)
option(NF_BUILD_SAMPLES    "Build sample projects"        ON)
option(NF_BUILD_EDITOR     "Build the editor"             ON)
option(NF_BUILD_TOOLS      "Build standalone tools"       OFF)
option(NF_USE_VULKAN      "Enable Vulkan RHI backend"     ON)
option(NF_USE_D3D12       "Enable D3D12 RHI backend"      OFF)
option(NF_WARNINGS_AS_ERRORS "Treat warnings as errors"   ON)

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------

if(WIN32)
    set(NF_PLATFORM_WINDOWS ON)
elseif(APPLE)
    set(NF_PLATFORM_MACOS ON)
else()
    set(NF_PLATFORM_LINUX ON)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

include(NFCompilerFlags)

# Shader compilation is configured here rather than under Samples/ because
# both the samples and the RHI tests consume the same SPIR-V.
include(NFShaders)

# ---------------------------------------------------------------------------
# Third-party
# ---------------------------------------------------------------------------

add_subdirectory(ThirdParty)

# ---------------------------------------------------------------------------
# Engine modules
# ---------------------------------------------------------------------------

add_subdirectory(Engine/Core)
add_subdirectory(Engine/Jobs)
add_subdirectory(Engine/Platform)
add_subdirectory(Engine/RHI)
add_subdirectory(Engine/ECS)
add_subdirectory(Engine/Scene)
add_subdirectory(Engine/Rendering)
add_subdirectory(Engine/Assets)
add_subdirectory(Engine/Physics)
add_subdirectory(Engine/Animation)
add_subdirectory(Engine/Audio)
add_subdirectory(Engine/Runtime)

# ---------------------------------------------------------------------------
# Project tooling library
#
# Deliberately NOT gated behind NF_BUILD_TOOLS: this is a library, and both the
# asset cooker and the `nf` CLI are thin front ends over it. The test suite links
# it too, so it must exist whenever tests are built.
# ---------------------------------------------------------------------------

add_subdirectory(Tools/ProjectTool)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(NF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# ---------------------------------------------------------------------------
# Samples
# ---------------------------------------------------------------------------

if(NF_BUILD_SAMPLES)
    add_subdirectory(Samples)
endif()

if(NF_BUILD_TOOLS)
    # NFProjectTool is added above, unconditionally.
    add_subdirectory(Tools/AssetCooker)
    add_subdirectory(Tools/BuildTool)
    # NFPlayer is what packaging copies into a package, so it is built alongside
    # the tool that needs it.
    add_subdirectory(Tools/Player)
endif()

# ---------------------------------------------------------------------------
# Editor
# ---------------------------------------------------------------------------

if(NF_BUILD_EDITOR)
    add_subdirectory(Editor)
endif()
