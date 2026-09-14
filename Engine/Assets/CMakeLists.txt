# NFAssets — Virtual File System, Asset Registry, Asset Manager, Mesh Asset, Project Descriptor
#
# CPU-pure since Phase 11 W1: no NFRendering, no NFRHI. The StaticMesh/asset
# conversions live in NFRendering (MeshUpload.cpp), which depends on NFAssets —
# never the other way around.

set(NFASSETS_SOURCES
    src/VirtualFileSystem.cpp
    src/AssetRegistry.cpp
    src/AssetManager.cpp
    src/MeshAsset.cpp
    src/ProjectDescriptor.cpp
)

nf_engine_library(NFAssets
    SOURCES ${NFASSETS_SOURCES}
    DEPENDS NFCore NFJobs
)

target_include_directories(NFAssets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
