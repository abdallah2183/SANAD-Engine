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
    CoreTests/test_testframework.cpp
    CoreTests/test_geometry.cpp
    CoreTests/test_eventbus.cpp
    CoreTests/test_ring_allocator.cpp
    CoreTests/test_object_pool.cpp
    CoreTests/test_curve_and_easing.cpp
    CoreTests/test_tween.cpp
    CoreTests/test_state_machine.cpp
    CoreTests/test_string_id.cpp
    CoreTests/test_bitset.cpp
    CoreTests/test_timer_manager.cpp
    CoreTests/test_compression_and_commandline.cpp
    CoreTests/test_procedural_noise.cpp
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
    ECSTests/test_octree.cpp
    ECSTests/test_entity_command_buffer.cpp
    ECSTests/test_scene_query.cpp
    ECSTests/test_spline.cpp
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
    RHITests/test_skyenv.cpp
    RHITests/test_timeofday.cpp
    RHITests/test_terrain.cpp
    RHITests/test_procedural_mesh.cpp
    RHITests/test_postfx_shake.cpp
    # The §206 post stack: bloom chain, colour grade, sharpen. CPU mirrors for
    # every stage (so the maths, the neutral values and the stage ORDER are
    # pinned without a GPU) plus three GPU tests that assert the chain is
    # recorded and that its result reaches the pixels.
    RHITests/test_post_stack.cpp
    # Internal render scale (Phase 37): the clamp/rounding contract without a
    # GPU, plus a renderer test that a 32x32 output still renders after the
    # internal targets are rebuilt at 64x64 (2x SSAA) and 16x16.
    RHITests/test_render_scale.cpp
    # Dynamic resolution (Phase 38): pure CPU governor. Cooldown, hysteresis,
    # step direction, clamps, reset, illegal config and hitch handling, with no
    # GPU required — the renderer integration is covered by RuntimeTests.
    RHITests/test_dynamic_resolution.cpp
    # Frame pacing (Phase 41): the rolling window, average/1% low/jitter, ring
    # wrap, GPU-sample ageing and the bound verdict. Pure arithmetic, no GPU.
    RHITests/test_frame_pacing.cpp
    # Frame-pacing POLICY (Phase 42): render scale decided from a finished
    # window. The asymmetry (1% low lowers, average + 1% low raise), the
    # cooldown, the band and the clamps — all arithmetic, no GPU.
    RHITests/test_frame_pacing_policy.cpp
    # Pure arithmetic (splits, fit, tiling, bias) over NFRendering's cascade
    # math, so it runs without a GPU — the one suite that proves the committed
    # CSM work, which had been sitting unregistered and unrun.
    RHITests/test_shadow_cascades.cpp
    # Point/spot projectors: six cube faces and one cone fit, plus the
    # reconstruction rule the shader must agree with. CPU-only for the same
    # reason — the failures here look like soft or misplaced shadows, not
    # crashes, so they need pinning where a GPU can't.
    RHITests/test_local_shadows.cpp
    # Projected screen-space decals (Phase 36): the CPU size/inverse/rect/sort
    # contract, plus GPU proofs that the blend lands in GBuffer0/2, preserves
    # the normal/emissive attachments and the emission-strength alpha, and that
    # disabled/offscreen/backfacing decals are bit-exact no-ops.
    RHITests/test_decals.cpp
    # GPU timestamps (Phase 39): the raw query path, non-blocking availability
    # polling and RenderGraph's per-pass brackets. The Renderer3D integration
    # lives in test_3d_renderer.cpp with the rest of the scene pipeline.
    RHITests/test_gpu_timing.cpp
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
        NFBasic3DDepthShaders NFBasic3DGBufferShaders NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders NFBasic3DFXAAShaders
        NFBasic3DBloomShaders NFBasic3DSSAOShaders NFBasic3DSSAOBlurShaders NFBasic3DPickShaders
        NFBasic3DDecalShaders)
    # The present pass is its own shader target (present_vert.spv /
    # present_frag.spv). Renderer3D::present_texture() — the path
    # Runtime::update() takes every frame — loads those two files, and
    # without them the present stage loses its pipeline and the frame never
    # reaches the screen. Editor/CMakeLists.txt already depends on this
    # target; the test suites did not, so a suite that renders could be
    # pointed at a shader directory with no present pass in it.
    if(TARGET NFBasic3DPresentShaders)
        add_dependencies(RHITests NFBasic3DPresentShaders)
    endif()
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
    AssetTests/test_mesh_import.cpp
    AssetTests/test_package_archive.cpp
)
target_include_directories(AssetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AssetTests PRIVATE NFCore NFAssets NFRHI NFRendering NFProjectTool)
nf_target_set_warnings(AssetTests)

add_executable(RuntimeTests
    TestFramework.cpp
    RuntimeTests/test_runtime_scene.cpp
    RuntimeTests/test_camera_follow.cpp
    RuntimeTests/test_player_look.cpp
    RuntimeTests/test_player_settings.cpp
    # Scene validation (Phase 43): the rules that only exist BETWEEN entities —
    # duplicate names, a camera following nothing, a scale the renderer would
    # reject. Pure: a scene built in memory, no device.
    RuntimeTests/test_scene_validator.cpp
    # The gun and hit points (Phase 44): the trigger machine, the magazine, the
    # deterministic cone, the ray and the scene line. Pure — no device, no
    # window — because every one of those is arithmetic before it is a game.
    RuntimeTests/test_weapon.cpp
    # The NPC half of the same fight (Phase 45): notice, turn, decide, hold
    # fire between bursts — and one integration case where an agent actually
    # takes hit points off a player it can see.
    RuntimeTests/test_combat_ai.cpp
    # Time of flight and sides (Phase 46): a projectile is a body integrated
    # frame by frame, and a faction decides whether a hit is allowed at all.
    RuntimeTests/test_ballistics.cpp
    # Frame-local impact events (Phase 49): current/previous slots, deterministic
    # order, and a hard cap. Pure: no world, no device.
    RuntimeTests/test_impact_events.cpp
    # Layered damage (Phase 50): hit-zone geometry, the armor pool, penetration,
    # the scene line, and a head hit through the real runtime.
    RuntimeTests/test_damage.cpp
    # Radial blasts (Phase 51): linear falloff, nearest-point distance, the
    # scene keys, and one runtime blast that hits a second target.
    RuntimeTests/test_explosion.cpp
    # The shot itself (Phase 53): the frame-local buffer a game reads to draw a
    # muzzle flash and a tracer, separate from the impact it may produce.
    RuntimeTests/test_shot_events.cpp
    # Damage over time (Phase 54): refresh, tick cadence, partial final tick,
    # removal and the `Status:` scene line. Pure arithmetic, no device.
    RuntimeTests/test_status_effects.cpp
    # Combat memory and suppression (Phase 55): who hit me, and what a near
    # miss does to aim and trigger discipline. Pure arithmetic.
    RuntimeTests/test_damage_ledger.cpp
    RuntimeTests/test_suppression.cpp
    # Terrain and its links (Phase 63): the `Terrain:`/`NavLink:` lines round
    # trip, the validator names the links the bake would drop, and a scene that
    # declares a terrain gets a navigation mesh that follows it.
    RuntimeTests/test_terrain.cpp
    RuntimeTests/test_input_bindings.cpp
    RuntimeTests/test_character.cpp
    RuntimeTests/test_runtime_anim_audio.cpp
    RuntimeTests/test_runtime.cpp
    RuntimeTests/test_runtime_3b.cpp
    RuntimeTests/test_run_config.cpp
    RuntimeTests/test_runtime_streaming.cpp
    RuntimeTests/test_runtime_sky.cpp
    # The §206 post stack as scene data: the `PostProcess:` line round-trips, an
    # absent component writes no line, and the runtime really pushes the block
    # into the renderer (the integration half, without which the component is a
    # library nobody drives).
    RuntimeTests/test_runtime_post_process.cpp
    # The exposure route the editor's Settings > Rendering section writes
    # through: Runtime::renderer() is const by design, so without this narrow
    # setter the editor could read exposure and never set it. Pins that the value
    # reaches the renderer, and that a value which would poison the tonemap is
    # refused rather than stored.
    RuntimeTests/test_runtime_exposure.cpp
    # The scene's audio environment: reverb zones, the level's music and its
    # ambience bed. AudioScene and MusicSystem are unit-tested in isolation;
    # this file proves the three `.nfscene` lines reach the LIVE mixer and that
    # a new scene replaces the previous one's environment.
    RuntimeTests/test_runtime_scene_audio.cpp
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
    # Phase 24: Lua reachability from a scene. ScriptSystem is unit-tested in
    # isolation; this file proves a `Script:` line is resolved and stepped by
    # update() with no game code driving the seam.
    RuntimeTests/test_runtime_scene_script.cpp
    # Phase 25: the wired systems. Particles, cloth, the character mover and
    # the AI budget are unit-tested in isolation; this file proves a scene
    # that merely declares them is built and stepped by update().
    RuntimeTests/test_runtime_scene_systems.cpp
    # Phase 28: navigation. NavMesh is unit-tested in AITests (19 tests over
    # the voxel pipeline); this file is the Rule 0 half — a scene that merely
    # declares `NavMesh:` / `NavAgent:` is baked, walked by update() and
    # observable, with no game code calling the navmesh directly.
    RuntimeTests/test_runtime_nav_agents.cpp
    # Step 11: Jolt arcade cars. JoltVehicle and the JoltWorld vehicle API are
    # unit-tested in PhysicsTests; this file is the Rule 0 half — a scene that
    # merely declares `Vehicle:` is bound, driven by update() from live inputs
    # and written back into its Transform, with no game code spawning bodies.
    RuntimeTests/test_runtime_vehicles.cpp
    # Phase 36: projected decals. The CPU box/projection contract and the GPU
    # composite live in RHITests/test_decals.cpp; this file is the scene half —
    # `Decal:` round-trips, refuses an impossible size by name, and the Runtime
    # really extracts enabled decals with the entity Transform and authored size.
    RuntimeTests/test_runtime_decals.cpp
    # Phase 56: shot/impact events become visible and audible. The director's
    # arithmetic, the `Feedback:` scene line, and one real fired shot through
    # the Runtime into RenderWorld objects, a decal and the mixer.
    RuntimeTests/test_combat_feedback.cpp
    # Phase 60: the AI core's senses bound to entities and fed by the frame.
    # The `Perception:` line, the validator's cross-field rule, and the frame
    # itself: hearing a gunshot, sight needing the cone and a clear line, pain
    # waking only the victim, a sensor never detecting itself, and an agent that
    # only engages what it has actually noticed.
    RuntimeTests/test_perception_runtime.cpp
    # Phase 61: the decision layer. The `Brain:` and `BrainAction:` lines, the
    # validator's list-level rules, the frame's own inputs, and the choices they
    # produce — including the one effect the engine applies (`hold_fire`).
    RuntimeTests/test_brain_runtime.cpp
    # Phase 62: the state machine half of the same trio. The `States:` / `State:`
    # / `Transition:` lines, the cross-reference rules only the whole machine can
    # check, and the frame's side: authored order decides a tie, the rate holds a
    # transition, and a `hold_fire` state vetoes the trigger.
    RuntimeTests/test_state_machine_runtime.cpp
)
target_include_directories(RuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(RuntimeTests PRIVATE NFCore NFRuntime NFAssets NFScene NFEcs NFRendering NFRHI NFPhysics NFAnimation NFAudio NFScripting NFVfx NFAI NFNetworking)
# The character the engine ships, so the character tests exercise the real
# asset rather than a fixture that could drift away from it.
target_compile_definitions(RuntimeTests PRIVATE
    "NF_CHARACTER_PATH=\"${CMAKE_SOURCE_DIR}/Games/TreeWorld/Content/Characters/Explorer.gltf\""
    # Phase 46's shipped sample: parsed and validated from source so a broken
    # example cannot sit in Content/ without a test noticing.
    "NF_BALLISTICS_SCENE_PATH=\"${CMAKE_SOURCE_DIR}/Content/Scenes/Ballistics.nfscene\""
    # Phase 51's sample: the scene that shows armor, hit zones and a blast in
    # one place. It is loaded from source, so the example cannot drift.
    "NF_COMBAT_DAMAGE_SCENE_PATH=\"${CMAKE_SOURCE_DIR}/Content/Scenes/CombatDamage.nfscene\"")
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
    PhysicsTests/test_collision_materials_and_buoyancy.cpp
    PhysicsTests/test_physics_query_and_triggers.cpp
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
    AnimationTests/test_blend_space_and_fabrik.cpp
    AnimationTests/test_notifies_and_montage.cpp
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
AudioTests/test_audio_game_settings.cpp
    AudioTests/test_audio_decode.cpp
    AudioTests/test_audio_music.cpp
    AudioTests/test_audio_occlusion.cpp
    AudioTests/test_audio_reverb.cpp
    AudioTests/test_audio_scene.cpp
    # Master-bus dynamics: the compressor's static curve and times, the
    # limiter's brickwall and latency, the neutral/bit-exact contract, and the
    # AudioScene integration that puts both stages after the bus volumes.
    AudioTests/test_audio_dynamics.cpp
    # Phase 56: the procedural gunshot/impact one-shots the feedback layer
    # plays. Fixed-seed noise, bounded, decaying, deterministic.
    AudioTests/test_feedback_audio.cpp
    AudioTests/test_dsp_and_soundcue.cpp
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
    GameplayTests/test_attributes_and_savegame.cpp
    GameplayTests/test_abilities.cpp
    GameplayTests/test_gameplay_effects.cpp
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
    UITests/test_font_packaging.cpp
)
target_include_directories(UITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(UITests PRIVATE NFCore NFUI)
# The editor's Arabic companion font, so the test can assert the file EXISTS and
# resolves. A packaged build that ships without it renders its whole UI as tofu,
# which no other test in the suite can see.
target_compile_definitions(UITests PRIVATE
    "NF_FONT_DIR=\"${CMAKE_SOURCE_DIR}/Resources/fonts\""
    "NF_FONT_FILE=\"Amiri-Regular.ttf\""
)
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
    # plus one test that spawns the real SANADServer binary and connects a
    # client to it over real UDP — the only place the headless host is proven
    # as a process, not just as a library.
    NetworkTests/test_dedicated_server.cpp
    NetworkTests/test_bitstream_and_rpc.cpp
)
target_include_directories(NetworkTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(NetworkTests PRIVATE NFCore NFNetworking)
nf_target_set_warnings(NetworkTests)
# The binary-spawn test needs the built server exe's absolute path (spaces in
# the repo path are quoted by the test itself).
#
# SANADServer is a SAMPLE target, so it only exists when the samples are
# built. Referencing a non-existent target in a generator expression is a
# configure-time error, not a warning: with -DNF_BUILD_SAMPLES=OFF the whole
# configure failed with "Error evaluating generator expression". The spawn
# test is skipped when the path is empty, which is what the sample-less
# configuration needs.
if(TARGET SANADServer)
    target_compile_definitions(NetworkTests PRIVATE "NF_SERVER_EXE=\"$<TARGET_FILE:SANADServer>\"")
    add_dependencies(NetworkTests SANADServer)
else()
    target_compile_definitions(NetworkTests PRIVATE "NF_SERVER_EXE=\"\"")
endif()

# --- AI tests (grid pathfinding, behavior trees, perception, state machines) ---
add_executable(AITests
    AITests/test_navmesh.cpp
    TestFramework.cpp
    AITests/test_navgrid.cpp
    AITests/test_behavior_tree.cpp
    AITests/test_perception.cpp
    AITests/test_state_machine.cpp
    AITests/test_utility_ai.cpp
    AITests/test_ai_world.cpp
    AITests/test_eqs_and_advanced_bt.cpp
    AITests/test_bt_services_and_blackboard.cpp
)
target_include_directories(AITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AITests PRIVATE NFCore NFAI)
nf_target_set_warnings(AITests)

# --- Scene2D tests (atlas, batcher, tilemap, 2D solver) ---
add_executable(Scene2DTests
    TestFramework.cpp
    Scene2DTests/test_scene2d.cpp
    Scene2DTests/test_systems.cpp
    Scene2DTests/test_parallax_and_nineslice.cpp
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
    VFXTests/test_vfx_advanced.cpp
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
    ToolTests/test_build_tool.cpp
    ToolTests/test_model_importer_cli.cpp
    ToolTests/test_blender_preset.cpp
    ToolTests/test_script_assets.cpp
    # G7: the three game templates must scaffold into a project that actually
    # loads and plays, not just one that copies files.
    ToolTests/test_templates.cpp
)
target_include_directories(ToolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# NFRuntime is what owns load_scene_from_physical, so the template tests read the
# scaffolded Main.nfscene through the same loader the player uses.
target_link_libraries(ToolTests PRIVATE NFCore NFAssets NFProjectTool NFRendering NFRuntime)
target_compile_definitions(ToolTests PRIVATE
    # The crash-report test asserts the version NFPlayer was built with, so it
    # must read the same single source of truth instead of a hand-copied literal
    # that goes stale on every release bump.
    "NF_ENGINE_VERSION=\"${NF_VERSION_STRING}\""
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
    target_compile_definitions(ToolTests PRIVATE
        "NF_PLAYER_EXE=\"$<TARGET_FILE:NFPlayer>\""
        # test_build_tool.cpp exercises the real template selector and nf run path.
        "NF_CLI_EXE=\"$<TARGET_FILE:nf>\"")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL add_dependencies ToolTests NFPlayer nf)
    # test_model_importer_cli.cpp launches NFModelImporter, same pattern.
    target_compile_definitions(ToolTests PRIVATE
        "NF_MODEL_IMPORTER_EXE=\"$<TARGET_FILE:NFModelImporter>\"")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL add_dependencies ToolTests NFModelImporter)
endif()
if(NF_GLSLC_EXECUTABLE)
    # The packager copies these, so they must exist before the tests run.
    add_dependencies(ToolTests NFBasic3DDepthShaders NFBasic3DGBufferShaders
        NFBasic3DLightingShaders NFBasic3DForwardShaders NFBasic3DTonemapShaders NFBasic3DFXAAShaders NFBasic3DBloomShaders NFBasic3DSSAOShaders NFBasic3DSSAOBlurShaders NFBasic3DPickShaders)
    # The present pass too: a packaged scene is driven through the Runtime,
    # whose frame ends in Renderer3D::present_texture(), and the packaged
    # shader set has to carry present_vert.spv / present_frag.spv with it.
    if(TARGET NFBasic3DPresentShaders)
        add_dependencies(ToolTests NFBasic3DPresentShaders)
    endif()
endif()

# --- Editor tests (window-independent editor logic + headless GPU viewport) ---
if(NF_BUILD_EDITOR)
    add_executable(EditorTests
        TestFramework.cpp
        EditorTests/test_outliner.cpp
        EditorTests/test_inspector.cpp
        # The editor path for the scene audio environment (reverb zone / music /
        # ambience): command-level cases with no device, plus the EditorApp case
        # that proves a music path is resolved through the VFS and that an
        # unchanged path keeps the samples already decoded.
        EditorTests/test_audio_env_editor.cpp
        EditorTests/test_undo_redo.cpp
        EditorTests/test_delete_selection.cpp
        EditorTests/test_duplicate.cpp
        EditorTests/test_fog_scene.cpp
        EditorTests/test_recovery.cpp
        EditorTests/test_asset_filter.cpp
        EditorTests/test_drag_drop.cpp
        EditorTests/test_save_load.cpp
        EditorTests/test_playmode.cpp
        EditorTests/test_viewport.cpp
        EditorTests/test_viewport_drag.cpp
        EditorTests/test_view_framing.cpp
        EditorTests/test_imgui_id_uniqueness.cpp
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
        EditorTests/test_script_editor.cpp
        EditorTests/test_wiring_editor.cpp
        EditorTests/test_ide_open.cpp
        EditorTests/test_transform_gizmo.cpp
        # Phase 28: navigation authoring (attach / validated edit / detach +
        # the save/load round trip), same contract as test_wiring_editor.cpp.
        EditorTests/test_nav_editor.cpp
        # Step 11: vehicle authoring (attach / validated tune / live-only debug
        # drive / detach), plus the play/stop preservation the PlayMode
        # copiers now guarantee for vehicles and navigation alike.
        EditorTests/test_vehicle_editor.cpp
    )
    target_include_directories(EditorTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # test_project_launcher.cpp pins the New Project gallery against the
        # CLI's own template allow-list (NF/Project/ProjectScaffold.hpp) so the
        # two cannot drift into offering different content. Header-only, so the
        # include path is all that is needed — no link to NFProjectTool.
        "${CMAKE_SOURCE_DIR}/Tools/ProjectTool/include")
    target_link_libraries(EditorTests PRIVATE NFCore NFEditorCore NFAssets NFRHI NFRendering NFGameplay NFScripting NFVfx NFUI)
    target_compile_definitions(EditorTests PRIVATE
        "NF_EDITOR_IMGUI_SHADER_DIR=\"${NF_EDITOR_IMGUI_SHADER_DIR}\""
        # For the ImGui duplicate-ID lint. It reads the panel sources because a
        # duplicate ID is only observable at runtime with a GPU, a live EditorApp
        # and a real window — which is exactly why one shipped and reached a
        # user as a modal error popup instead of a failing test.
        "NF_EDITOR_UI_SRC_DIR=\"${CMAKE_SOURCE_DIR}/Editor/src/ui\""
    )
    nf_target_set_warnings(EditorTests)
    if(NF_GLSLC_EXECUTABLE)
        add_dependencies(EditorTests NFEditorImGuiShaders)
    endif()
endif()

# MSVC: match the static runtime used by engine libraries
if(MSVC)
    foreach(_t nf_test_framework CoreTests JobTests ECSTests AssetTests RuntimeTests RHITests PhysicsTests AnimationTests AudioTests GameplayTests SaveTests StreamingTests ToolTests ScriptTests UITests PlatformTests InputTests NetworkTests AITests VFXTests Scene2DTests DestructionTests PerfTests FoliageTests WaterTests)
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
