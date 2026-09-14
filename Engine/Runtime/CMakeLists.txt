# NFRuntime — Application + Runtime + SceneLoader

set(NFRUNTIME_SOURCES
    src/Application.cpp
    src/Runtime.cpp
    src/RuntimeSceneLoader.cpp
)

nf_engine_library(NFRuntime
    SOURCES ${NFRUNTIME_SOURCES}
    DEPENDS NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFAssets
)

target_include_directories(NFRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(NFRuntime PRIVATE "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\"")
