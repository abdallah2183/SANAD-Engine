# NFAnimation — skeletal animation.
#
# Self-contained. The design document plans Ozz or a similar third-party
# runtime as a long-term option; this module commits to the engine-facing API
# (skeleton, clips, player, state machine, blend) behind a seam that a future
# backend can satisfy without touching the ECS component, the serialization,
# the editor, or the runtime.

set(NFANIMATION_SOURCES
    src/Skeleton.cpp
    src/AnimationClip.cpp
    src/AnimationPlayer.cpp
    src/AnimationStateMachine.cpp
)

nf_engine_library(NFAnimation
    SOURCES ${NFANIMATION_SOURCES}
    DEPENDS NFCore
)
