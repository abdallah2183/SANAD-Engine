# NFAI — game AI: grid pathfinding, behavior trees, perception, blackboard,
# and the world-scale simulation budget that decides which actor runs which.
#
# CPU-only and deterministic (fixed neighbor order, sequenced heap
# tie-breaks, fixed sense evaluation order, no RNG): AI decisions replay
# bit-identically.

set(NFAI_SOURCES
    src/NavGrid.cpp
    src/BehaviorTree.cpp
    src/Perception.cpp
    src/StateMachine.cpp
    src/UtilityAI.cpp
    src/AIWorld.cpp
)

nf_engine_library(NFAI
    SOURCES ${NFAI_SOURCES}
    DEPENDS NFCore
    DIR AI
)
