# NFRendering — RenderGraph + Material System (layer above RHI)

set(NFRENDERING_SOURCES
    src/RenderGraph.cpp
    src/Material.cpp
    src/MaterialAsset.cpp
    src/StbImage.cpp
    src/MaterialLibrary.cpp
    src/PipelineCache.cpp
    src/ShaderReflection.cpp
    src/Shader.cpp
    src/ShaderManager.cpp
    src/RenderWorld.cpp
    src/Extraction.cpp
    src/StaticMesh.cpp
    src/MeshAsset.cpp
    src/MeshLibrary.cpp
    src/Renderer3D.cpp
    src/GpuPicker.cpp
    src/Camera.cpp
    src/Culling.cpp
    src/ResourceState.cpp
)

nf_engine_library(NFRendering
    SOURCES ${NFRENDERING_SOURCES}
    DEPENDS NFCore NFRHI NFJobs NFEcs NFScene
)

target_include_directories(NFRendering
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_SOURCE_DIR}/ThirdParty/stb
)

if(MSVC)
    set_property(TARGET NFRendering PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
