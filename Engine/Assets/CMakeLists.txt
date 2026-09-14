# NFAssets — Virtual File System, Asset Registry, Asset Manager, Mesh Asset, Project Descriptor

set(NFASSETS_SOURCES
    src/VirtualFileSystem.cpp
    src/AssetRegistry.cpp
    src/AssetManager.cpp
    src/MeshAsset.cpp
    src/ProjectDescriptor.cpp
)

nf_engine_library(NFAssets
    SOURCES ${NFASSETS_SOURCES}
    DEPENDS NFCore NFJobs NFRendering
)

target_include_directories(NFAssets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
