# NFEcs — Entity Component System

set(NFECS_SOURCES
    src/ECS.cpp
    src/EntityCommandBuffer.cpp
)

nf_engine_library(NFEcs
    SOURCES ${NFECS_SOURCES}
    DEPENDS NFCore NFJobs
    # The default (strip the NF prefix) resolves to Engine/Ecs, but the real
    # directory is Engine/ECS. That only works on a case-insensitive
    # filesystem; on Linux/macOS the include path would not exist. DIR pins
    # the exact spelling, the way the VFX module does.
    DIR ECS
)

# Public headers are in include/NF/ECS
# No extra includes needed — nf_engine_library handles it
