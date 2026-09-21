# NFPhysics — rigid-body simulation.
#
# Jolt backend layout (Phase 16, hard-won — do NOT "simplify"):
#   - JoltWorld.cpp is the ONLY TU that touches Jolt headers/objects (chassis
#     Body*, VehicleConstraint, tester, RagdollSettings, JPH::Ragdoll...).
#     It opts out of /W4 /WX because Jolt headers are warning-noisy.
#   - JoltVehicle.cpp / JoltRagdoll.cpp / JoltCharacter.cpp are THIN Jolt-free
#     handles over JoltWorld wiring. Cross-TU Jolt allocation heap-corrupts
#     (Jolt's custom allocator + per-TU operator new/delete) — proven by a
#     0xC0000374 crash when the vehicle built its Body*/constraint in its own TU.

set(NFPHYSICS_SOURCES
    src/Shapes.cpp
    src/Broadphase.cpp
    src/Narrowphase.cpp
    src/PhysicsWorld.cpp
    src/CharacterController.cpp
    src/JoltWorld.cpp
    src/JoltVehicle.cpp
    src/JoltRagdoll.cpp
    src/JoltCharacter.cpp
    # The Jolt-backed IDebrisSink: Jolt-free (it goes through JoltWorld's API
    # only, like the three TUs above) so it stays under /W4 /WX.
    src/JoltDebrisSink.cpp
    # Cloth is first-party and deliberately Jolt-free: Jolt has no deformable
    # solver, so §38's Cloth entry is a PBD implementation over the engine's own
    # shapes. It includes no Jolt header — do not "unify" it with the Jolt TUs.
    src/Cloth.cpp
)

nf_engine_library(NFPhysics
    SOURCES ${NFPHYSICS_SOURCES}
    # NFDestruction is pure arithmetic behind the IDebrisSink seam; physics is
    # the layer that implements it for real (JoltDebrisSink). NFDestruction
    # depends on nothing but NFCore, so this edge cannot cycle.
    DEPENDS NFCore NFDestruction
)

# Jolt headers are warning-noisy: this TU alone opts out of /W4 /WX.
# (JoltVehicle.cpp / JoltRagdoll.cpp / JoltCharacter.cpp are Jolt-free by
# design: no opt-out.)
set_source_files_properties(src/JoltWorld.cpp PROPERTIES COMPILE_OPTIONS "/W1;/WX-")
target_link_libraries(NFPhysics PUBLIC Jolt)
