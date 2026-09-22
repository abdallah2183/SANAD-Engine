# NFFoliage — foliage scatter (design doc 61): grass, rocks, trees placed on a
# surface by density, slope/height gates, and a painted mask.
#
# CPU-only and deterministic: integer cell indices and an integer-lattice hash,
# no RNG state, and no float ever enters the hash. A cell's instances depend
# only on the cell, so a chunk scattered on demand (design doc 60) is identical
# to the same chunk scattered as part of a larger rect.
#
# Deliberately depends on nothing but NFCore: the ground and the mask are
# injected callbacks, so this module never pulls in the RHI or the terrain, and
# it builds and tests while the render path is in flight elsewhere.

set(NFFoliage_SOURCES
    src/Foliage.cpp
)

nf_engine_library(NFFoliage
    SOURCES ${NFFoliage_SOURCES}
    DEPENDS NFCore
    DIR Foliage
)
