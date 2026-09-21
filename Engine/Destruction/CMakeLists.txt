# Engine/Destruction — breakable meshes, fracture assets, chunk-based debris
# (design doc Section 41; Section 371 item 4).
#
# Increment 1 is geometry only: convex hulls, plane cuts, the chunk tree. It
# depends on nothing but Core math, so an asset can be built and verified on a
# machine with no physics and no GPU. ECS and Jolt arrive with Increment 2.

nf_engine_library(NFDestruction
    SOURCES
        src/FractureMath.cpp
        src/FractureAsset.cpp
        src/FractureBuilder.cpp
    DEPENDS
        NFCore
)
