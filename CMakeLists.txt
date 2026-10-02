cmake_minimum_required(VERSION 3.24)

# The single source of truth for the engine version. Everything that shows a
# version derives from here and nothing keeps a second copy:
#   * NF_VERSION_STRING           -> the launcher sidebar, project cards, hero line
#   * Editor/resources/version.rc -> the exe's icon tooltip and Explorer
#                                   Properties (File version / Product version)
# A hand-maintained literal in a .cpp is what made the launcher keep advertising
# 0.1.0 after every build; the fix is that there is nowhere left to put one.
#
# 0.2.0-beta.1: the first tagged beta. It carries the full transform-hierarchy
# upgrade (world rotation and scale now propagate and reach the renderer), asset
# delete/rename, view framing, and the Godot-style FileSystem dock.
project(NOVAForge
    VERSION 0.2.0
    LANGUAGES C CXX
    DESCRIPTION "NOVAForge Engine - High-performance cross-platform game engine"
)

# project(VERSION) accepts NUMBERS only, so a pre-release tag cannot live there.
# It gets its own line next to the version instead of being smeared into a
# hand-copied string somewhere, and NF_VERSION_STRING composes the two. Set it
# empty for a final release; the "-suffix" disappears with it.
set(NF_VERSION_SUFFIX "beta.1")
if(NF_VERSION_SUFFIX STREQUAL "")
    set(NF_VERSION_STRING "${PROJECT_VERSION}")
else()
    set(NF_VERSION_STRING "${PROJECT_VERSION}-${NF_VERSION_SUFFIX}")
endif()

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
add_subdirectory(Engine/UI)
add_subdirectory(Engine/Jobs)
add_subdirectory(Engine/Platform)
add_subdirectory(Engine/Input)
add_subdirectory(Engine/RHI)
add_subdirectory(Engine/ECS)
add_subdirectory(Engine/Scene)
add_subdirectory(Engine/Rendering)
# Water shares StaticMesh with the render path but is otherwise pure arithmetic,
# so it follows Rendering in the link order and reaches no RHI type itself.
add_subdirectory(Engine/Water)
add_subdirectory(Engine/Assets)
# Destruction must precede Physics: NFPhysics implements the destruction
# IDebrisSink seam (JoltDebrisSink), so it links NFDestruction, and a module
# can only link one defined before it. NFDestruction itself depends on nothing
# but NFCore, so it is free to sit here.
add_subdirectory(Engine/Destruction)
add_subdirectory(Engine/Physics)
add_subdirectory(Engine/Animation)
add_subdirectory(Engine/Audio)
add_subdirectory(Engine/Gameplay)
add_subdirectory(Engine/Scripting)
add_subdirectory(Engine/Networking)
add_subdirectory(Engine/AI)
add_subdirectory(Engine/VFX)
add_subdirectory(Engine/Foliage)
add_subdirectory(Engine/Scene2D)
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
    add_subdirectory(Tools/ModelImporter)
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
