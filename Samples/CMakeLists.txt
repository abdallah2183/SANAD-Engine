# Samples CMakeLists.txt
# Sample projects and demos
#
# Shader compilation is configured by NFShaders.cmake at the top level, since
# the RHI tests share the same SPIR-V.

# --- Triangle — v0.1 milestone 1: Window → Device → Triangle ---------------
add_executable(NFSampleTriangle
    Triangle/main.cpp
)
target_link_libraries(NFSampleTriangle PRIVATE NFCore NFJobs NFPlatform NFRHI)
nf_target_set_warnings(NFSampleTriangle)

# Absolute path to the SPIR-V directory, so the sample works regardless of the
# working directory it is launched from.
target_compile_definitions(NFSampleTriangle PRIVATE
    "NF_TRIANGLE_SHADER_DIR=\"${NF_TRIANGLE_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleTriangle NFTriangleShaders)
endif()

# --- TexturedQuad — v0.1 milestone 2: buffers, textures, descriptors -------
# Now uses the Rendering layer (RenderGraph + Material + PipelineCache)
add_executable(NFSampleTexturedQuad
    TexturedQuad/main.cpp
)
target_link_libraries(NFSampleTexturedQuad PRIVATE NFCore NFJobs NFPlatform NFRHI NFRendering)
nf_target_set_warnings(NFSampleTexturedQuad)

target_compile_definitions(NFSampleTexturedQuad PRIVATE
    "NF_QUAD_SHADER_DIR=\"${NF_QUAD_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleTexturedQuad NFQuadShaders)
endif()

# --- Basic3D — first real 3D sample (Depth/GBuffer/Lighting/Tonemap) ---
add_executable(NFSampleBasic3D
    Basic3D/main.cpp
)
target_link_libraries(NFSampleBasic3D PRIVATE NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFRuntime)
nf_target_set_warnings(NFSampleBasic3D)

target_compile_definitions(NFSampleBasic3D PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleBasic3D NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DTonemapShaders)
endif()

add_executable(NFSampleRuntimeScene
    RuntimeScene/main.cpp
)
target_link_libraries(NFSampleRuntimeScene PRIVATE NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFAssets NFRuntime)
nf_target_set_warnings(NFSampleRuntimeScene)

target_compile_definitions(NFSampleRuntimeScene PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleRuntimeScene NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DTonemapShaders)
endif()

# --- SimpleGame — playable demo: Cube Collector (WASD, coins, enemy) ---
add_executable(NFSampleSimpleGame
    SimpleGame/main.cpp
)
target_link_libraries(NFSampleSimpleGame PRIVATE NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFRuntime)
nf_target_set_warnings(NFSampleSimpleGame)

target_compile_definitions(NFSampleSimpleGame PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleSimpleGame NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DTonemapShaders)
endif()

# MSVC: match the static runtime used by engine libraries
if(MSVC)
    set_property(TARGET NFSampleTriangle
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleTexturedQuad
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleBasic3D
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleRuntimeScene
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleSimpleGame
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
