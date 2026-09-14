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
)

nf_engine_library(NFPhysics
    SOURCES ${NFPHYSICS_SOURCES}
    DEPENDS NFCore
)
