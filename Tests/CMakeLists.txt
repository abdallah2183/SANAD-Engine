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
    # Pure arithmetic (splits, fit, tiling, bias) over NFRendering's cascade
    # math, so it runs without a GPU — the one suite that proves the committed
    # CSM work, which had been sitting unregistered and unrun.
    RHITests/test_shadow_cascades.cpp
    # Point/spot projectors: six cube faces and one cone fit, plus the
    # reconstruction rule the shader must agree with. CPU-only for the same
    # reason — the failures here look like soft or misplaced shadows, not
    # crashes, so they need pinning where a GPU can't.
    RHITests/test_local_shadows.cpp
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
        NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders
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
    AssetTests/test_gltf_character_import.cpp
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
    RuntimeTests/test_runtime_audio_import.cpp
    # The Rule 0 file: destruction's arithmetic world (DestructionTests) and its
    # Jolt sink (PhysicsTests) are both unit-tested in isolation. This one proves
    # a blast through the Runtime really spawns shards into a world update()
    # steps — the integration neither of the other two can see.
    RuntimeTests/test_runtime_destruction.cpp
    # The reachability file: the seam above is driven by hand. This one proves a
    # scene that merely declares a DestructibleComponent ends up cooked, bound,
    # shattered by a real contact and drawn — with no game code calling
    # enable_destruction or apply_damage. Before adopt_scene did this, the whole
    # Phase 19 chain was reachable only from tests.
    RuntimeTests/test_runtime_scene_destruction.cpp
    # VehicleSystem is header-only: this file is what makes the build
    # translate it at all (and pins the chassis pose it writes back).
    RuntimeTests/test_runtime_vehicle.cpp
    # Phase 10, W4: the record/replay round trip. The log's internals are
    # trivial; what has to be proved is that a captured session, replayed into a
    # freshly loaded scene with no live input, lands the entity in the same
    # place — which only Runtime::update() can show.
    RuntimeTests/test_runtime_input_log.cpp
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
    PhysicsTests/test_character.cpp
    PhysicsTests/test_jolt.cpp
    PhysicsTests/test_jolt_vehicle.cpp
    PhysicsTests/test_jolt_constraints.cpp
    PhysicsTests/test_jolt_queries.cpp
    PhysicsTests/test_jolt_character.cpp
    PhysicsTests/test_jolt_shapes.cpp
    # The Jolt-backed IDebrisSink: the first implementation of the destruction
    # seam, so this is where "the shard really became a body" gets asserted.
    PhysicsTests/test_jolt_debris_sink.cpp
    # Cloth is first-party (Jolt has no deformable solver), so it lives in its own
    # file rather than next to the Jolt cases.
    PhysicsTests/test_cloth.cpp
)
target_include_directories(PhysicsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(PhysicsTests PRIVATE NFCore NFPhysics)
nf_target_set_warnings(PhysicsTests)

# --- Animation tests (skeleton, clips, sampling, blending, player, state machine) ---
add_executable(AnimationTests
    TestFramework.cpp
    AnimationTests/test_animation.cpp
    AnimationTests/test_ik.cpp
    AnimationTests/test_retarget.cpp
    AnimationTests/test_root_motion.cpp
    AnimationTests/test_skinning.cpp
)
target_include_directories(AnimationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AnimationTests PRIVATE NFCore NFAnimation)
nf_target_set_warnings(AnimationTests)

# --- Audio tests (attenuation, 3D pan/gain, mixing, null device) ---
add_executable(AudioTests
    TestFramework.cpp
    AudioTests/test_audio.cpp
    AudioTests/test_audio_backend.cpp
    AudioTests/test_audio_buses.cpp
    AudioTests/test_audio_decode.cpp
    AudioTests/test_audio_music.cpp
    AudioTests/test_audio_occlusion.cpp
    AudioTests/test_audio_reverb.cpp
    AudioTests/test_audio_scene.cpp
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
    GameplayTests/test_dialogue.cpp
)
target_include_directories(GameplayTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(GameplayTests PRIVATE NFCore NFGameplay NFRuntime NFAssets NFScene NFEcs NFRHI NFRendering NFPhysics NFAnimation NFAudio)
nf_target_set_warnings(GameplayTests)

# --- Scripting tests (Lua VM, sandboxing, entity bindings, ticking, C#) ---
add_executable(ScriptTests
    TestFramework.cpp
    ScriptTests/test_scripting.cpp
    ScriptTests/test_csharp.cpp
    ScriptTests/test_ui_scripting.cpp
)
target_include_directories(ScriptTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ScriptTests PRIVATE NFCore NFEcs NFScene NFScripting NFUI)
if(NF_CSHARP_HOSTING)
    # Managed sandbox must exist before the hosting tests run; its absolute
    # dir is baked in so tests find the DLL + runtimeconfig without CWD games.
    add_dependencies(ScriptTests NFCSharpSandbox)
    target_compile_definitions(ScriptTests PRIVATE NF_CSHARP_ASM_DIR="${NF_CSHARP_OUT}")
    target_compile_definitions(ScriptTests PRIVATE NF_CSHARP_HOSTING=1)
else()
    target_compile_definitions(ScriptTests PRIVATE NF_CSHARP_HOSTING=0)
endif()
nf_target_set_warnings(ScriptTests)

# --- UI tests (Arabic shaping/bidi, localisation, runtime game UI) ---
add_executable(UITests
    TestFramework.cpp
    UITests/test_arabic.cpp
    UITests/test_localization.cpp
    UITests/test_gameui.cpp
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

# --- Input tests (action mapping, contexts, deadzones, rebinding) ---
add_executable(InputTests
    TestFramework.cpp
    InputTests/test_input.cpp
)
target_include_directories(InputTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(InputTests PRIVATE NFCore NFPlatform NFInput)
nf_target_set_warnings(InputTests)

# --- Network tests (UDP loopback transport, reliable channel, snapshots) ---
add_executable(NetworkTests
    TestFramework.cpp
    NetworkTests/test_udp.cpp
    NetworkTests/test_reliable.cpp
    NetworkTests/test_replication.cpp
    NetworkTests/test_vehicle_replication.cpp
    NetworkTests/test_constraint_replication.cpp
    # The dedicated server (design 81): in-process provisioning/refusal rules,
    # plus one test that spawns the real NOVAForgeServer binary and connects a
    # client to it over real UDP — the only place the headless host is proven
    # as a process, not just as a library.
    NetworkTests/test_dedicated_server.cpp
)
target_include_directories(NetworkTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(NetworkTests PRIVATE NFCore NFNetworking)
nf_target_set_warnings(NetworkTests)
# The binary-spawn test needs the built server exe's absolute path (spaces in
# the repo path are quoted by the test itself).
target_compile_definitions(NetworkTests PRIVATE "NF_SERVER_EXE=\"$<TARGET_FILE:NOVAForgeServer>\"")
add_dependencies(NetworkTests NOVAForgeServer)

# --- AI tests (grid pathfinding, behavior trees, perception, state machines) ---
add_executable(AITests
    TestFramework.cpp
    AITests/test_navgrid.cpp
    AITests/test_behavior_tree.cpp
    AITests/test_perception.cpp
    AITests/test_state_machine.cpp
    AITests/test_utility_ai.cpp
    AITests/test_ai_world.cpp
)
target_include_directories(AITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AITests PRIVATE NFCore NFAI)
nf_target_set_warnings(AITests)

# --- Scene2D tests (atlas, batcher, tilemap, 2D solver) ---
add_executable(Scene2DTests
    TestFramework.cpp
    Scene2DTests/test_scene2d.cpp
    Scene2DTests/test_systems.cpp
)
target_include_directories(Scene2DTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Scene2DTests PRIVATE NFCore NFScene2D)
nf_target_set_warnings(Scene2DTests)

# --- Destruction tests (convex geometry, fracture assets, determinism) ---
add_executable(DestructionTests
    TestFramework.cpp
    DestructionTests/test_fracture.cpp
    # The arithmetic half: budgets, stress accumulation, shard kinematics and the
    # mirror invariant between the component flags and the sink's live set.
    DestructionTests/test_destruction_world.cpp
)
target_include_directories(DestructionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(DestructionTests PRIVATE NFCore NFDestruction)
nf_target_set_warnings(DestructionTests)

# --- Perf tests (the G8 performance contract) ---
# A realistic scene (~2k meshes + a shadowed sun + live destructibles) is loaded
# through the real Runtime + Renderer3D path and frame time / draw calls /
# memory are compared against Tests/PerfTests/perf_baseline.csv: any metric more
# than its tolerance above baseline fails the suite (NF_PERF_GATE=1). The paths
# are absolute so the gate cannot be silently disarmed by the working directory.
add_executable(PerfTests
    TestFramework.cpp
    PerfTests/test_perf_scene.cpp
)
target_include_directories(PerfTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(PerfTests PRIVATE NFCore NFRuntime NFAssets NFScene NFEcs NFRendering NFRHI NFDestruction)
target_compile_definitions(PerfTests PRIVATE
    "NF_PERF_SCENE_PATH=\"${CMAKE_SOURCE_DIR}/Content/Scenes/Perf.nfscene\""
    "NF_PERF_BASELINE_PATH=\"${CMAKE_SOURCE_DIR}/Tests/PerfTests/perf_baseline.csv\""
)
nf_target_set_warnings(PerfTests)

# --- Foliage tests (deterministic scatter: density, gates, masks, LOD, wind) ---
add_executable(FoliageTests
    TestFramework.cpp
    FoliageTests/test_scatter.cpp
)
target_include_directories(FoliageTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(FoliageTests PRIVATE NFCore NFFoliage)
nf_target_set_warnings(FoliageTests)

# --- Water tests (the Gerstner field: height, displacement, normals, foam,
# reflection, and the baked grid's agreement with the live CPU surface) ---
add_executable(WaterTests
    TestFramework.cpp
    WaterTests/test_water.cpp
)
target_include_directories(WaterTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(WaterTests PRIVATE NFCore NFWater)
nf_target_set_warnings(WaterTests)

# --- VFX tests (deterministic particle simulation) ---
add_executable(VFXTests
    TestFramework.cpp
    VFXTests/test_particles.cpp
)
target_include_directories(VFXTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(VFXTests PRIVATE NFCore NFVfx)
nf_target_set_warnings(VFXTests)

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
    ToolTests/test_shipping.cpp
    ToolTests/test_model_importer_cli.cpp
    ToolTests/test_blender_preset.cpp
    # G7: the three game templates must scaffold into a project that actually
    # loads and plays, not just one that copies files.
    ToolTests/test_templates.cpp
)
target_include_directories(ToolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# NFRuntime is what owns load_scene_from_physical, so the template tests read the
# scaffolded Main.nfscene through the same loader the player uses.
target_link_libraries(ToolTests PRIVATE NFCore NFAssets NFProjectTool NFRendering NFRuntime)
target_compile_definitions(ToolTests PRIVATE
    "NF_TEMPLATE_DIR=\"${CMAKE_SOURCE_DIR}/Templates/Default\""
    "NF_BASIC3D_SHADER_DIR=\"${NF_BASIC3D_SHADER_DIR}\""
    # test_blender_preset.cpp pins the shipped add-on's exporter option names.
    "NF_BLENDER_PRESET=\"${CMAKE_SOURCE_DIR}/Templates/Blender/nf_gltf_export.py\""
    # test_templates.cpp scaffolds each game template from its real directory.
    "NF_TEMPLATE_THIRDPERSON_DIR=\"${CMAKE_SOURCE_DIR}/Templates/ThirdPerson\""
    "NF_TEMPLATE_FPSSTARTER_DIR=\"${CMAKE_SOURCE_DIR}/Templates/FPSStarter\""
    "NF_TEMPLATE_PLATFORMER2D_DIR=\"${CMAKE_SOURCE_DIR}/Templates/Platformer2D\""
)
nf_target_set_warnings(ToolTests)
if(NF_BUILD_TOOLS)
    # test_shipping.cpp launches the packaged player. Tools/Player is added to
    # the tree after Tests, so the path is a generator expression (resolved at
    # generate time, when NFPlayer exists) and the build-order dependency is
    # deferred to the end of the source tree.
    target_compile_definitions(ToolTests PRIVATE "NF_PLAYER_EXE=\"$<TARGET_FILE:NFPlayer>\"")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL add_dependencies ToolTests NFPlayer)
    # test_model_importer_cli.cpp launches NFModelImporter, same pattern.
    target_compile_definitions(ToolTests PRIVATE
        "NF_MODEL_IMPORTER_EXE=\"$<TARGET_FILE:NFModelImporter>\"")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL add_dependencies ToolTests NFModelImporter)
endif()
if(NF_GLSLC_EXECUTABLE)
    # The packager copies these, so they must exist before the tests run.
    add_dependencies(ToolTests NFBasic3DDepthShaders NFBasic3DGBufferShaders
        NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders NFBasic3DPickShaders)
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
        EditorTests/test_arabic_editor.cpp
        EditorTests/test_gizmo.cpp
        EditorTests/test_preview_pixels.cpp
        EditorTests/test_audio_preview.cpp
        EditorTests/test_profiler_session.cpp
        EditorTests/test_project_launcher.cpp
        EditorTests/test_editor_finishing.cpp
    )
    target_include_directories(EditorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(EditorTests PRIVATE NFCore NFEditorCore NFAssets NFRHI NFRendering NFGameplay NFUI)
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
        NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders)
endif()
if(MSVC)
    set_property(TARGET HeadlessProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# MSVC: match the static runtime used by engine libraries
if(MSVC)
    foreach(_t nf_test_framework CoreTests JobTests ECSTests AssetTests RuntimeTests RHITests PhysicsTests AnimationTests AudioTests GameplayTests SaveTests StreamingTests ToolTests HeadlessProbe ScriptTests UITests PlatformTests InputTests NetworkTests AITests VFXTests Scene2DTests DestructionTests PerfTests FoliageTests WaterTests)
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
add_test(NAME InputTests COMMAND InputTests)
add_test(NAME NetworkTests COMMAND NetworkTests)
add_test(NAME AITests COMMAND AITests)
add_test(NAME VFXTests COMMAND VFXTests)
add_test(NAME Scene2DTests COMMAND Scene2DTests)
add_test(NAME DestructionTests COMMAND DestructionTests)
add_test(NAME PerfTests COMMAND PerfTests)
add_test(NAME FoliageTests COMMAND FoliageTests)
add_test(NAME WaterTests COMMAND WaterTests)
if(NF_BUILD_EDITOR)
    add_test(NAME EditorTests COMMAND EditorTests)
endif()
