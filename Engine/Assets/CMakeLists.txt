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
    src/GltfImport.cpp
    src/AnimationImport.cpp
    src/MeshExport.cpp
    src/CgltfImpl.cpp
)

nf_engine_library(NFAssets
    SOURCES ${NFASSETS_SOURCES}
    DEPENDS NFCore NFJobs NFAnimation
)

target_link_libraries(NFAssets PUBLIC cgltf)

# The vendored cgltf implementation is third-party code: warning-lenient TU.
set_source_files_properties(src/CgltfImpl.cpp PROPERTIES COMPILE_OPTIONS "/W1;/WX-;/wd4244")

target_include_directories(NFAssets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
