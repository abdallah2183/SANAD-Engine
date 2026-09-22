# Engine/Destruction — breakable meshes, fracture assets, chunk-based debris
# (design doc Section 41; Section 371 item 4).
#
# Increment 1 is geometry: convex hulls, plane cuts, the chunk tree. Increment
# 2 adds the live state and the damage/debris world, which is still pure
# arithmetic behind an IDebrisSink seam — no physics or GPU required to build or
# to test, so a machine with neither still verifies the budget and the shards.

nf_engine_library(NFDestruction
    SOURCES
        src/FractureMath.cpp
        src/FractureAsset.cpp
        src/FractureBuilder.cpp
        src/FractureMaterial.cpp
        src/DestructionWorld.cpp
    DEPENDS
        NFCore
)
