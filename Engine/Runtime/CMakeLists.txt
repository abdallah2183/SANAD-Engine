# NFRuntime — Application + Runtime + SceneLoader

set(NFRUNTIME_SOURCES
    src/Application.cpp
    src/RunConfigResolver.cpp
    src/Runtime.cpp
    src/RuntimeSceneLoader.cpp
    src/SceneExtraction.cpp
    src/WorldStreamer.cpp
    src/SaveSystem.cpp
    src/InputLog.cpp
)

nf_engine_library(NFRuntime
    SOURCES ${NFRUNTIME_SOURCES}
    DEPENDS NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFAssets NFPhysics NFDestruction NFAnimation NFAudio NFGameplay
)

target_include_directories(NFRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(NFRuntime PRIVATE "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\"")
# The save format stamps the engine version into every slot's meta.txt. Taken
# from the CMake project version so the stamp cannot drift from the binary.
target_compile_definitions(NFRuntime PRIVATE "NF_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
