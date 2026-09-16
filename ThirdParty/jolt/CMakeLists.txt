# ThirdParty/jolt — Jolt Physics SDK (MIT, see LICENSE_JOLT.txt).
#
# Upstream: https://github.com/jrouwe/JoltPhysics
# Pinned: v5.6.0 (sparse checkout of Jolt/ + Jolt.cmake only; samples, unit
# tests, assets and docs are NOT vendored).
#
# NOVAForge usage: the static `jolt` target below feeds NFPhysics'
# JoltWorld wrapper (Engine/Physics). The engine's default solver stays
# first-party; Jolt is the advanced backend for complex meshes/vehicles.
#
# Build notes (why this file exists instead of upstream's root CMakeLists):
#   - Upstream's root build also compiles samples/tests/viewer; we only want
#     the library, driven by the upstream-maintained Jolt.cmake file list
#     (so file additions upstream merge cleanly).
#   - JPH_CROSS_PLATFORM_DETERMINISTIC keeps simulation results identical
#     across platforms, matching the engine's determinism contract.
#   - MSVC static runtime (MultiThreaded) matches every other target here.
#   - Jolt sources are warning-lenient by design (/W1, no /WX).

set(PHYSICS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(${CMAKE_CURRENT_SOURCE_DIR}/Jolt/Jolt.cmake)

# Jolt.cmake lists headers alongside sources; keep object files only.
set(JOLT_LIB_SOURCES "")
foreach(_f ${JOLT_PHYSICS_SRC_FILES})
    if(_f MATCHES "\\.cpp$")
        list(APPEND JOLT_LIB_SOURCES ${_f})
    endif()
endforeach()

add_library(jolt STATIC ${JOLT_LIB_SOURCES})
target_include_directories(jolt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(jolt PUBLIC
    JPH_CROSS_PLATFORM_DETERMINISTIC
)
# Jolt needs float contraction freedom for its SIMD paths; without it MSVC
# keeps intermediate precision and cross-platform determinism degrades.
if(MSVC)
    target_compile_options(jolt PRIVATE /W1 /fp:fast)
    set_property(TARGET jolt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_compile_features(jolt PUBLIC cxx_std_17)
