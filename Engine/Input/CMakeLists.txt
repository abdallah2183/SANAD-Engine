# NFInput — Gameplay-facing action layer over the platform device state.
#
# Depends on NFPlatform for the raw InputState/key enums and on NFCore for
# containers and math. It must never reach back into rendering or gameplay —
# see the layering rules in design doc §103.

nf_engine_library(NFInput
    SOURCES
        src/InputAction.cpp
        src/InputContext.cpp
        src/InputMapper.cpp
        src/InputNames.cpp
    DEPENDS NFCore NFPlatform
)
