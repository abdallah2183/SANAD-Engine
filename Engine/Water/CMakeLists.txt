# NFWater — water surface (design doc 62): ocean, lakes, rivers.
#
# CPU-only and deterministic: the wave field is a pure function of position and
# time over a wave list, and the shore is an injected callback, so the logic
# tests without a GPU the way NFFoliage does. The one render-side type it
# touches is StaticMesh, which the mesh builder shares with every other mesh;
# the RHI is never reached from here.

set(NFWater_SOURCES
    src/Water.cpp
)

nf_engine_library(NFWater
    SOURCES ${NFWater_SOURCES}
    DEPENDS NFCore NFRendering
    DIR Water
)
