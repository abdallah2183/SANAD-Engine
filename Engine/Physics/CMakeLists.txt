# NFPhysics — rigid-body simulation.
#
# Self-contained on purpose. The design document plans a Jolt integration, and
# that is the right long-term answer, but vendoring a large third-party solver is
# a decision for the project owner rather than a side effect of a phase. What
# this module commits to is the *interface*: the rest of the engine talks to
# PhysicsWorld and to generation-checked body handles, never to a solver detail,
# so a Jolt backend can be dropped in behind the same surface.

set(NFPHYSICS_SOURCES
    src/Shapes.cpp
    src/Broadphase.cpp
    src/Narrowphase.cpp
    src/PhysicsWorld.cpp
    src/CharacterController.cpp
    src/JoltWorld.cpp
)

nf_engine_library(NFPhysics
    SOURCES ${NFPHYSICS_SOURCES}
    DEPENDS NFCore
)

# Jolt headers are warning-noisy: this TU alone opts out of /W4 /WX.
set_source_files_properties(src/JoltWorld.cpp PROPERTIES COMPILE_OPTIONS "/W1;/WX-")
target_link_libraries(NFPhysics PUBLIC Jolt)
