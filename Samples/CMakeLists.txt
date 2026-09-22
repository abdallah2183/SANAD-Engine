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
    add_dependencies(NFSampleBasic3D NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
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
    add_dependencies(NFSampleRuntimeScene NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
endif()

# --- SimpleGame — playable demo: Cube Collector (WASD, coins, enemy) ---
add_executable(NFSampleSimpleGame
    SimpleGame/main.cpp
)
target_link_libraries(NFSampleSimpleGame PRIVATE NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFRuntime NFScripting)
nf_target_set_warnings(NFSampleSimpleGame)

target_compile_definitions(NFSampleSimpleGame PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleSimpleGame NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
endif()

# --- VehicleDemo — drivable Jolt car: chassis + 4 posed wheels, obstacles, gates ---
add_executable(NFSampleVehicleDemo
    VehicleDemo/main.cpp
)
target_link_libraries(NFSampleVehicleDemo PRIVATE NFCore NFJobs NFPlatform NFInput NFRHI NFRendering NFEcs NFScene NFRuntime NFPhysics)
nf_target_set_warnings(NFSampleVehicleDemo)

target_compile_definitions(NFSampleVehicleDemo PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleVehicleDemo NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
endif()

# --- WaterDemo — animated Gerstner surface, shore surf, and a buoy riding the CPU field ---
# Reuses the Basic3D shader set: the demo draws nothing those materials cannot
# shade. The wave field is re-baked per frame at the clock's time, replaced in
# the mesh library, and re-uploaded, so the surface moves without touching the
# renderer or its shaders (the foam/reflection channel is requested, not
# assumed — see COORDINATION.md R1).
add_executable(NFSampleWaterDemo
    WaterDemo/main.cpp
)
target_link_libraries(NFSampleWaterDemo PRIVATE NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFRuntime NFWater)
nf_target_set_warnings(NFSampleWaterDemo)

target_compile_definitions(NFSampleWaterDemo PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleWaterDemo NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
endif()

# --- MedievalVillage — third-person collection game on the MegaKit assets ---
# Exercises the full vertical: cooked kit meshes + BaseColor maps through the
# runtime asset path, a physics character among measured-AABB colliders, the
# gameplay quest/inventory layer, and the engine's ui::GameFlow drawn by the
# sample's own ImGui-backed overlay (the shipped-game UI backend NF/UI lacks).
# Reuses the Basic3D shader set for the world and the editor's imgui shaders
# for the HUD.
add_executable(NFSampleMedievalVillage
    MedievalVillage/main.cpp
    MedievalVillage/KitAssets.cpp
    MedievalVillage/UiOverlay.cpp
    MedievalVillage/Village.cpp
)
target_link_libraries(NFSampleMedievalVillage PRIVATE
    NFCore NFJobs NFPlatform NFRHI NFRendering NFEcs NFScene NFAssets
    NFRuntime NFPhysics NFGameplay NFUI imgui)
nf_target_set_warnings(NFSampleMedievalVillage)

target_compile_definitions(NFSampleMedievalVillage PRIVATE
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
    "NF_EDITOR_IMGUI_SHADER_DIR=\"${NF_EDITOR_IMGUI_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(NFSampleMedievalVillage NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
    add_dependencies(NFSampleMedievalVillage NFEditorImGuiShaders)
endif()

# --- AnimationRetarget — G4 acceptance demo: two characters, one graph ------
# Headless and GPU-less on purpose: it links NFCore + NFAnimation only, so the
# shared-locomotion-graph demo runs anywhere the engine builds. It exits 0 when
# every check holds, so it doubles as a smoke test.
add_executable(NFSampleAnimationRetarget
    AnimationRetarget/main.cpp
)
target_link_libraries(NFSampleAnimationRetarget PRIVATE NFCore NFAnimation)
nf_target_set_warnings(NFSampleAnimationRetarget)

# --- GameUI — G3 acceptance sample: the shipped-game UI spine --------------
# Headless and GPU-less on purpose: it links NFCore + NFUI + NFScripting only,
# so the main -> play -> pause -> game-over flow runs anywhere the engine
# builds, driven once from C++ and once from Lua through nf.ui.*. Exits 0 when
# both walks agree, so it doubles as a smoke test.
add_executable(NFSampleGameUI
    GameUI/main.cpp
)
target_link_libraries(NFSampleGameUI PRIVATE NFCore NFUI NFScripting)
nf_target_set_warnings(NFSampleGameUI)

# --- AudioWalkthrough — G5 acceptance demo: cave echoes, wall muffle, menu
# music and volume sliders, all from a settings file ------------------------
# Headless and GPU-less on purpose: it links NFCore + NFAudio only, so the
# walkthrough runs anywhere the engine builds. It renders the sequence to a WAV
# (so the cave can actually be heard on a machine with no sound card) and exits
# 0 when every check holds, so it doubles as a smoke test.
add_executable(NFSampleAudioWalkthrough
    AudioWalkthrough/main.cpp
)
target_link_libraries(NFSampleAudioWalkthrough PRIVATE NFCore NFAudio)
nf_target_set_warnings(NFSampleAudioWalkthrough)

# --- Server — NOVAForgeServer: the dedicated, GPU-less host (design §81) ---
# Links NFCore/NFJobs/NFNetworking only. Adding RHI or Rendering here would
# defeat the point: this binary must build and run on a machine with no GPU,
# because a server does not render.
add_executable(NOVAForgeServer
    Server/main.cpp
)
target_link_libraries(NOVAForgeServer PRIVATE NFCore NFJobs NFNetworking)
nf_target_set_warnings(NOVAForgeServer)

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
    set_property(TARGET NFSampleVehicleDemo
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleWaterDemo
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleMedievalVillage
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleAnimationRetarget
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleGameUI
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NFSampleAudioWalkthrough
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET NOVAForgeServer
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
