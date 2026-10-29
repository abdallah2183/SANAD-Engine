# NFVfx — CPU particle simulation (design doc Section 57).
#
# Deterministic emission/integration; the renderer consumes the packed
# particle array in a later phase (billboard pipeline).

set(NFVFX_SOURCES
    src/Particles.cpp
    src/ParticleEmitterShape.cpp
    src/ForceField.cpp
    src/RibbonTrail.cpp
)

nf_engine_library(NFVfx
    SOURCES ${NFVFX_SOURCES}
    DEPENDS NFCore
    DIR VFX
)
