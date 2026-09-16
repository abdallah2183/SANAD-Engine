# ThirdParty/jolt — Jolt Physics SDK (MIT, see LICENSE_JOLT.txt).
#
# Upstream: https://github.com/jrouwe/JoltPhysics
# Pinned: v5.6.0 (sparse checkout of Jolt/ + Jolt.cmake only; samples, unit
# tests, assets and docs are NOT vendored).
#
# NOVAForge usage: the upstream-maintained `Jolt` target below feeds
# NFPhysics' JoltWorld wrapper (Engine/Physics). The engine's default solver
# stays first-party; Jolt is the advanced backend for complex meshes/vehicles.
#
# Options are set explicitly (upstream's root CMakeLists, which normally sets
# them, is not vendored):
#   - CROSS_PLATFORM_DETERMINISTIC: identical results across platforms,
#     matching the engine's determinism contract (design doc Section 114).
#   - USE_STATIC_MSVC_RUNTIME_LIBRARY: /MT to match every other target here.
#   - USE_ASSERTS OFF: Jolt's internal asserts stay off (violations surface
#     as test failures through the wrapper, not hard aborts in games).

set(PHYSICS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/Jolt/Jolt.cmake)

# Upstream's own CMakeLists (not vendored) would map its
# USE_STATIC_MSVC_RUNTIME_LIBRARY option onto the target; without it the
# target builds /MD and LNK2038s against our /MT world. Forced here.
if(MSVC)
    set_property(TARGET Jolt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
