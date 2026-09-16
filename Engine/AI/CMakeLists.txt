# NFAI — game AI: grid pathfinding, behavior trees, blackboard.
#
# CPU-only and deterministic (fixed neighbor order, sequenced heap
# tie-breaks, no RNG): AI decisions replay bit-identically.

set(NFAI_SOURCES
    src/NavGrid.cpp
    src/BehaviorTree.cpp
)

nf_engine_library(NFAI
    SOURCES ${NFAI_SOURCES}
    DEPENDS NFCore
    DIR AI
)
