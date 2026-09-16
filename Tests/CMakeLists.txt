# Tests CMakeLists.txt
# Unit tests for all engine modules

# Lightweight test framework — header-only registration, shared main()
add_library(nf_test_framework STATIC
    TestFramework.cpp
)
target_include_directories(nf_test_framework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nf_test_framework PUBLIC NFCore)
nf_target_set_warnings(nf_test_framework)

# Core tests
add_executable(CoreTests
    TestFramework.cpp
    CoreTests/test_math.cpp
    CoreTests/test_memory.cpp
    CoreTests/test_containers.cpp
    CoreTests/test_uuid.cpp
    CoreTests/test_hash.cpp
    CoreTests/test_time.cpp
    CoreTests/test_filesystem.cpp
    CoreTests/test_reflection.cpp
    CoreTests/test_profiler.cpp
    CoreTests/test_crash_handler.cpp
)
target_include_directories(CoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CoreTests PRIVATE NFCore)
nf_target_set_warnings(CoreTests)

# Job tests
add_executable(JobTests
    TestFramework.cpp
    JobTests/test_jobs.cpp
)
target_include_directories(JobTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(JobTests PRIVATE NFCore NFJobs)
nf_target_set_warnings(JobTests)

# --- ECS / Scene / Prefab tests ---------------------------------------------
add_executable(ECSTests
    TestFramework.cpp
    ECSTests/test_ecs.cpp
    ECSTests/test_transform.cpp
    ECSTests/test_scene.cpp
    ECSTests/test_prefab.cpp
    ECSTests/test_benchmarks.cpp
)
target_include_directories(ECSTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ECSTests PRIVATE NFCore NFEcs NFScene NFJobs)
nf_target_set_warnings(ECSTests)

# --- RHI tests -------------------------------------------------------------
# These exercise the real Vulkan backend on whatever GPU is present and skip
# themselves when there is none, so they are safe to run anywhere.

add_executable(RHITests
    TestFramework.cpp
    RHITests/test_rhi_triangle.cpp
    RHITests/test_rhi_quad.cpp
    RHITests/test_rhi_push.cpp
    RHITests/test_rhi_resources.cpp
    RHITests/test_rendergraph.cpp
    RHITests/test_rhi_hardening.cpp
    RHITests/test_rendering_pipeline.cpp
    RHITests/test_render_world.cpp
    RHITests/test_3d_renderer.cpp
    RHITests/test_gpu_picking.cpp
    RHITests/test_benchmarks_3d.cpp
    RHITests/test_lod.cpp
    RHITests/test_lod_gen.cpp
    RHITests/test_sky.cpp
    RHITests/test_timeofday.cpp
    RHITests/test_terrain.cpp
    RHITests/test_postfx_shake.cpp
)
target_include_directories(RHITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# NFScene/NFEcs are explicit now: NFRendering no longer provides them (Phase 11, W2).
# NFRuntime provides the game-world -> render-world bridge, which is what these
# tests are actually exercising.
target_link_libraries(RHITests PRIVATE NFCore NFRHI NFRendering NFScene NFEcs NFRuntime)
nf_target_set_warnings(RHITests)

# The RHI tests consume the same compiled SPIR-V as the samples, so the shaders
# are built once (NFShaders.cmake, included from the top level) and shared.
target_compile_definitions(RHITests PRIVATE
    "NF_RHI_TEST_SHADER_DIR=\"${NF_TRIANGLE_SHADER_DIR}\""
    "NF_RHI_TEST_QUAD_SHADER_DIR=\"${NF_QUAD_SHADER_DIR}\""
    "NF_RHI_PUSH_SHADER_DIR=\"${NF_RHI_PUSH_SHADER_DIR}\""
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)

if(NF_GLSLC_EXECUTABLE)
    add_dependencies(RHITests NFTriangleShaders NFQuadShaders NFRHIPushShaders
        NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DTonemapShaders
        NFBasic3DPickShaders)
endif()

add_executable(AssetTests
    TestFramework.cpp
    AssetTests/test_vfs.cpp
    AssetTests/test_registry.cpp
    AssetTests/test_asset_manager.cpp
    AssetTests/test_project_descriptor.cpp
    AssetTests/test_project_cooker.cpp
    AssetTests/test_gltf_import.cpp
)
target_include_directories(AssetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AssetTests PRIVATE NFCore NFAssets NFRHI NFRendering NFProjectTool)
nf_target_set_warnings(AssetTests)

add_executable(RuntimeTests
    TestFramework.cpp
    RuntimeTests/test_runtime_scene.cpp
    RuntimeTests/test_runtime_anim_audio.cpp
    RuntimeTests/test_runtime.cpp
    RuntimeTests/test_runtime_3b.cpp
    RuntimeTests/test_run_config.cpp
    RuntimeTests/test_runtime_streaming.cpp
    RuntimeTests/test_runtime_sky.cpp
)
target_include_directories(RuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(RuntimeTests PRIVATE NFCore NFRuntime NFAssets NFScene NFEcs NFRendering NFRHI NFPhysics NFAnimation NFAudio)
nf_target_set_warnings(RuntimeTests)

# --- Physics tests (shapes, broadphase, narrowphase, solver, determinism) ---
add_executable(PhysicsTests
    TestFramework.cpp
    PhysicsTests/test_shapes.cpp
    PhysicsTests/test_broadphase.cpp
    PhysicsTests/test_narrowphase.cpp
    PhysicsTests/test_solver.cpp
    PhysicsTests/test_determinism.cpp
)
target_include_directories(PhysicsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(PhysicsTests PRIVATE NFCore NFPhysics)
nf_target_set_warnings(PhysicsTests)

# --- Animation tests (skeleton, clips, sampling, blending, player, state machine) ---
add_executable(AnimationTests
    TestFramework.cpp
    AnimationTests/test_animation.cpp
)
target_include_directories(AnimationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AnimationTests PRIVATE NFCore NFAnimation)
nf_target_set_warnings(AnimationTests)

# --- Audio tests (attenuation, 3D pan/gain, mixing, null device) ---
add_executable(AudioTests
    TestFramework.cpp
    AudioTests/test_audio.cpp
    AudioTests/test_audio_backend.cpp
    AudioTests/test_audio_decode.cpp
)
target_include_directories(AudioTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AudioTests PRIVATE NFCore NFAudio)
nf_target_set_warnings(AudioTests)

# --- Gameplay tests (module registry, state bridge, Runtime integration) ---
# Links NFRuntime because the interesting half of the suite is whether the
# Runtime actually instantiates and steps modules — a registry test alone cannot
# see that, and that is precisely how Phase 9 shipped a dead subsystem.
add_executable(GameplayTests
    TestFramework.cpp
    GameplayTests/test_gameplay.cpp
    GameplayTests/test_gameplay_serialization.cpp
    GameplayTests/test_tags_quest_inventory.cpp
)
target_include_directories(GameplayTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(GameplayTests PRIVATE NFCore NFGameplay NFRuntime NFAssets NFScene NFEcs NFRHI NFRendering NFPhysics NFAnimation NFAudio)
nf_target_set_warnings(GameplayTests)

# --- Scripting tests (Lua VM, sandboxing, entity bindings, ticking) ---
add_executable(ScriptTests
    TestFramework.cpp
    ScriptTests/test_scripting.cpp
)
target_include_directories(ScriptTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ScriptTests PRIVATE NFCore NFEcs NFScene NFScripting)
nf_target_set_warnings(ScriptTests)

# --- UI tests (Arabic shaping/bidi, localisation) ---
add_executable(UITests
    TestFramework.cpp
    UITests/test_arabic.cpp
    UITests/test_localization.cpp
)
target_include_directories(UITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(UITests PRIVATE NFCore NFUI)
nf_target_set_warnings(UITests)

# --- Platform tests (input recording/playback; window tests stay manual) ---
add_executable(PlatformTests
    TestFramework.cpp
    PlatformTests/test_replay.cpp
)
target_include_directories(PlatformTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(PlatformTests PRIVATE NFCore NFPlatform)
nf_target_set_warnings(PlatformTests)

# --- Save tests (slots, async save, autosave, schema migration) ---
add_executable(SaveTests
    TestFramework.cpp
    SaveTests/test_save.cpp
)
target_include_directories(SaveTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(SaveTests PRIVATE NFCore NFGameplay NFRuntime NFAssets NFScene NFEcs NFRHI NFRendering NFPhysics NFAnimation NFAudio NFJobs)
nf_target_set_warnings(SaveTests)

# --- Streaming tests (world streaming grid + load/unload policy) ---
add_executable(StreamingTests
    TestFramework.cpp
    StreamingTests/test_streaming.cpp
    StreamingTests/test_stream_merge.cpp
)
target_include_directories(StreamingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(StreamingTests PRIVATE NFCore NFScene NFEcs NFRuntime NFAssets)
nf_target_set_warnings(StreamingTests)

# --- Tool tests (project scaffold, cooker, packager) ---
add_executable(ToolTests
    TestFramework.cpp
    ToolTests/test_project_scaffold.cpp
    ToolTests/test_project_packager.cpp
)
target_include_directories(ToolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ToolTests PRIVATE NFCore NFAssets NFProjectTool NFRendering)
target_compile_definitions(ToolTests PRIVATE
    "NF_TEMPLATE_DIR=\"${CMAKE_SOURCE_DIR}/Templates/Default\""
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)
nf_target_set_warnings(ToolTests)
if(NF_GLSLC_EXECUTABLE)
    # The packager copies these, so they must exist before the tests run.
    add_dependencies(ToolTests NFBasic3DDepthShaders NFBasic3DGBufferShaders
        NFBasic3DLightingShaders NFBasic3DTonemapShaders NFBasic3DPickShaders)
endif()

# --- Editor tests (window-independent editor logic + headless GPU viewport) ---
if(NF_BUILD_EDITOR)
    add_executable(EditorTests
        TestFramework.cpp
        EditorTests/test_outliner.cpp
        EditorTests/test_inspector.cpp
        EditorTests/test_undo_redo.cpp
        EditorTests/test_delete_selection.cpp
        EditorTests/test_asset_filter.cpp
        EditorTests/test_drag_drop.cpp
        EditorTests/test_save_load.cpp
        EditorTests/test_playmode.cpp
        EditorTests/test_viewport.cpp
        EditorTests/test_viewport_drag.cpp
        EditorTests/test_console.cpp
        EditorTests/test_ui_render.cpp
        EditorTests/test_materials.cpp
        EditorTests/test_import.cpp
        EditorTests/test_hot_reload.cpp
        EditorTests/test_prefabs.cpp
        EditorTests/test_reflected_inspector.cpp
        EditorTests/test_gameplay_editor.cpp
        EditorTests/test_orbit_camera_module.cpp
    )
    target_include_directories(EditorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(EditorTests PRIVATE NFCore NFEditorCore NFAssets NFRHI NFRendering NFGameplay)
    target_compile_definitions(EditorTests PRIVATE
        "NF_EDITOR_IMGUI_SHADER_DIR=\"${NF_EDITOR_IMGUI_SHADER_DIR}\""
    )
    nf_target_set_warnings(EditorTests)
    if(NF_GLSLC_EXECUTABLE)
        add_dependencies(EditorTests NFEditorImGuiShaders)
    endif()
endif()

# Temporary crash probe (remove before closing the milestone)
add_executable(HeadlessProbe EXCLUDE_FROM_ALL
    HeadlessProbe.cpp
)
target_include_directories(HeadlessProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(HeadlessProbe PRIVATE NFCore NFRHI NFRendering NFEcs NFScene NFRuntime)
target_compile_definitions(HeadlessProbe PRIVATE
    "NF_RHI_TEST_SHADER_DIR=\"${NF_TRIANGLE_SHADER_DIR}\""
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
)
if(NF_GLSLC_EXECUTABLE)
    add_dependencies(HeadlessProbe NFTriangleShaders NFBasic3DDepthShaders
        NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DTonemapShaders)
endif()
if(MSVC)
    set_property(TARGET HeadlessProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# MSVC: match the static runtime used by engine libraries
if(MSVC)
    foreach(_t nf_test_framework CoreTests JobTests ECSTests AssetTests RuntimeTests RHITests PhysicsTests AnimationTests AudioTests GameplayTests SaveTests StreamingTests ToolTests HeadlessProbe ScriptTests UITests PlatformTests)
        set_property(TARGET ${_t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
    if(NF_BUILD_EDITOR)
        set_property(TARGET EditorTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_test(NAME CoreTests COMMAND CoreTests)
add_test(NAME JobTests  COMMAND JobTests)
add_test(NAME ECSTests  COMMAND ECSTests)
add_test(NAME AssetTests COMMAND AssetTests)
add_test(NAME RuntimeTests COMMAND RuntimeTests)
add_test(NAME RHITests  COMMAND RHITests)
add_test(NAME PhysicsTests COMMAND PhysicsTests)
add_test(NAME AnimationTests COMMAND AnimationTests)
add_test(NAME AudioTests COMMAND AudioTests)
add_test(NAME GameplayTests COMMAND GameplayTests)
add_test(NAME SaveTests COMMAND SaveTests)
add_test(NAME StreamingTests COMMAND StreamingTests)
add_test(NAME ToolTests  COMMAND ToolTests)
add_test(NAME ScriptTests COMMAND ScriptTests)
add_test(NAME UITests COMMAND UITests)
add_test(NAME PlatformTests COMMAND PlatformTests)
if(NF_BUILD_EDITOR)
    add_test(NAME EditorTests COMMAND EditorTests)
endif()
