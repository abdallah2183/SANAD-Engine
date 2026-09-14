# NFEcs — Entity Component System

set(NFECS_SOURCES
    src/ECS.cpp
)

nf_engine_library(NFEcs
    SOURCES ${NFECS_SOURCES}
    DEPENDS NFCore NFJobs
)

# Public headers are in include/NF/ECS
# No extra includes needed — nf_engine_library handles it
