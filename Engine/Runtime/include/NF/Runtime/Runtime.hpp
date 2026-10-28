#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/DynamicResolution.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/GpuPicker.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/CameraShake.hpp>
#include <NF/Rendering/FramePacingPolicy.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Runtime/CameraFollow.hpp>
#include <NF/Runtime/PlayerLook.hpp>
#include <NF/Runtime/PlayerSettings.hpp>
#include <NF/Runtime/CharacterComponent.hpp>
#include <NF/Runtime/Hitscan.hpp>
#include <NF/Runtime/DamageLedger.hpp>
#include <NF/Runtime/Impact.hpp>
#include <NF/Runtime/Shot.hpp>
#include <NF/Runtime/StatusEffect.hpp>
#include <NF/Runtime/CombatFeedback.hpp>
#include <NF/Runtime/Suppression.hpp>
#include <NF/Runtime/Projectile.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Physics/FixedTimestep.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/JoltDebrisSink.hpp>
#include <NF/Physics/VehicleSystem.hpp>
#include <NF/Destruction/DestructionWorld.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/AudioScene.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Gameplay/GameplayState.hpp>
#include <NF/Runtime/InputLog.hpp>
#include <NF/Runtime/WorldStreamer.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Vfx/Particles.hpp>
#include <NF/Vfx/Components.hpp>
#include <NF/AI/AIWorld.hpp>
#include <NF/AI/NavMesh.hpp>
#include <NF/AI/Perception.hpp>
#include <NF/AI/UtilityAI.hpp>
#include <NF/AI/StateMachine.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nf::runtime {

// Fallback material assignment when MeshComponent::material is empty.
inline constexpr const char* kDefaultMaterialPath = "content://Materials/Default";

struct RuntimeConfig {
    std::string scene_path = "content://Scenes/Example.nfscene";
    uint32_t max_frames = 0;
    bool validation = false;
    bool headless = false;
    uint32_t width = 1280;
    uint32_t height = 720;
};

class Runtime {
public:
    Runtime(assets::VirtualFileSystem& vfs, assets::AssetRegistry& registry, assets::AssetManager& manager,
            rhi::IGraphicsDevice& device, rhi::Swapchain* swapchain);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Load a scene from a logical path (e.g. content://Scenes/Example.nfscene)
    // Returns true on success (even if some assets are missing, it will log and continue)
    bool load_scene(const std::string& logical_path, std::string& out_error);

    /// Installs an already-built scene as the live one, running everything
    /// load_scene does *after* parsing: display naming, physics rebuild, gameplay
    /// init plus the on_scene_load hooks, mesh loads, and lazy renderer init.
    ///
    /// Used by the save system, whose scene comes from a slot directory rather
    /// than from a single file. `logical_path` is recorded as the scene's origin
    /// — for a loaded save that is the slot's own scene file, which keeps the
    /// "where did this scene come from" answer honest.
    void adopt_scene(std::unique_ptr<scene::Scene> scene, const std::string& logical_path);

    // Per-frame update: handles async asset GPU uploads, transform propagation, culling, etc.
    void update(float dt);

    // Render the current scene to the swapchain image at `image_index`
    // The caller is responsible for acquiring the image and presenting.
    // Records Depth → GBuffer → Lighting → Tonemap via Renderer3D.
    // On failure records a distinct fallback clear (magenta) and logs.
    //
    // `frame_slot` selects the renderer's per-slot frame resources; the caller
    // must have waited the GPU work previously recorded through the same slot
    // (see Renderer3D::render). Default 0 keeps serial callers unchanged.
    void render(uint32_t image_index, rhi::CommandBuffer& cmd, u32 frame_slot = 0);

    // For headless/offscreen rendering (tests).
    // Records the full Renderer3D pipeline into `cmd` targeting `target`
    // (no swapchain, no PRESENT layout). On failure records fallback clear.
    void render_offscreen(rhi::Texture& target, rhi::CommandBuffer& cmd, u32 frame_slot = 0);

    /// Presents an already-finished offscreen frame (the editor's viewport
    /// target) onto swapchain image `image_index` WITHOUT re-running the scene
    /// pipeline: a straight texture copy of `offscreen` into the swapchain,
    /// followed by nothing — the caller draws its UI over it. The editor used
    /// to render its whole scene a second time per frame to fill the swapchain
    /// behind the UI; this replaces that second render.
    /// Returns false (and records a fallback clear) when there is no scene or
    /// the renderer failed; the caller then owns the same fallback behaviour
    /// render() has.
    bool present_viewport(uint32_t image_index, rhi::CommandBuffer& cmd,
                          rhi::Texture& offscreen, rhi::TextureView& offscreen_view,
                          u32 frame_slot = 0);

    /// Builds and returns the render world for the current frame — the same one
    /// render() hands to Renderer3D, scene objects and debris together.
    ///
    /// Test-facing: this is how a test proves a subsystem's output reached the
    /// real draw path. A dedicated per-subsystem accessor would only prove the
    /// builder works; going through build_render_world() is what proves the
    /// builder is *called*, which is the claim worth asserting (Rule 0:
    /// unreachable code is dead code). Rebuilds on every call, so a game frame
    /// that already has one should not ask for another.
    rendering::RenderWorld render_world_snapshot();

    // Frees all GPU resources (renderer, mesh/material libraries, fallback
    // passes) after wait_idle. Must be called before device->shutdown().
    // Safe to call multiple times; subsequent renders will re-initialize.
    void shutdown();

    // Updates the swapchain pointer (after recreation on resize) and resizes
    // renderer resources when the resolution changed.
    void on_swapchain_resized(rhi::Swapchain* swapchain);

    bool has_scene() const { return m_scene_data_ptr && m_scene_data_ptr->scene; }
    size_t mesh_count() const { return m_mesh_library ? m_mesh_library->size() : 0; }
    /// The mesh library, for the callers that have to reach a mesh the
    /// runtime owns: a skinned character's vertices are rewritten every
    /// frame, and only the owner of the buffer can do that.
    rendering::MeshLibrary* mesh_library() { return m_mesh_library.get(); }
    const rendering::MeshLibrary* mesh_library() const { return m_mesh_library.get(); }

    // --- Physics ------------------------------------------------------------
    //
    // The world is created and populated from the scene when the scene loads,
    // and stepped on a fixed clock so the result does not depend on frame rate.
    // Null until a scene containing physics is loaded.
    physics::PhysicsWorld* physics_world() const { return m_physics.get(); }

    /// Advances physics by however many whole fixed steps `frame_delta` covers,
    /// then writes the results back into the scene's Transforms. Returns the
    /// number of steps taken.
    u32 step_physics(f32 frame_delta);

    /// Creates bodies for every entity that has a rigid body, a collider and a
    /// transform. Called on scene load; safe to call again to resynchronise.
    void rebuild_physics_from_scene();

    // --- World streaming ----------------------------------------------------
    //
    // The IO half of the WorldStreamer seam (the policy half is stream-tested
    // without a filesystem or GPU). Chunk files are ordinary .nfscene files
    // under a content directory, named chunk_<x>_<y>_<z>.nfscene; a load
    // merges the chunk into the live world (see merge_scene_into_world), an
    // unload destroys exactly the entities the load created. Off by default:
    // a runtime that never enables streaming never pays for it and behaves
    // exactly as before.
    //
    // After a load lands, meshes are kicked for async loading (like a fresh
    // scene load) and physics is rebuilt so merged bodies simulate. Both are
    // deliberately full passes, not incremental: loads are rare, capped
    // events, and an incremental path would have to mirror the rebuild's
    // invariants to stay correct.

    /// Turns streaming on with chunk files resolved under `chunk_dir_logical`
    /// (e.g. "content://Chunks") and the given initial volume. Missing chunk
    /// files fail their load and are retried on later updates — a hole in the
    /// region is not a reason to give up on it.
    void enable_streaming(const std::string& chunk_dir_logical, const scene::StreamingVolume& volume);
    /// Unloads every streamed chunk and turns streaming off.
    void disable_streaming();
    [[nodiscard]] bool streaming_enabled() const { return m_streaming_enabled; }
    /// Replaces the active volume (call every frame with a moving volume).
    void set_streaming_volume(const scene::StreamingVolume& volume);
    /// Advances the streamer; callers normally get this via update().
    /// No-op without a loaded scene. Returns chunks loaded this call.
    u32 step_streaming();
    [[nodiscard]] size_t streamed_chunk_count() const { return m_streamer.loaded_chunk_count(); }
    [[nodiscard]] size_t streamed_entity_count() const { return m_streamer.streamed_entity_count(); }
    /// Chunk loads currently staged on workers (parsed, not yet committed).
    /// Bounded by the in-flight cap; the observable that proves the cap.
    [[nodiscard]] size_t stream_in_flight_count() const;
    /// Upper bound on concurrent chunk-parse workers. Default 2.
    void set_max_stream_loads_in_flight(size_t cap) { m_max_stream_in_flight = cap; }

    // --- Animation (Phase 9) ------------------------------------------------
    //
    // Animation is a function of time, not of state, so unlike physics it needs
    // no fixed clock: it advances by the frame delta and is deterministic for
    // the same sequence of deltas.
    //
    // For every entity with an AnimationComponent this advances the player (or
    // the state machine), samples the clip into `last_local_pose`, computes
    // `last_world_pose`, and drives the entity's transform from the root bone
    // so the motion reaches the renderer and the editor.
    //
    // Runs after step_physics. An entity carrying both a rigid body and an
    // animation is driven by the animation (last writer wins) — documented
    // rather than accidental.
    u32 step_animation(f32 dt);

    /// Entities whose animation was advanced by the last step_animation().
    /// 0 when the scene has no animated entity.
    size_t animated_entity_count() const { return m_animated_entities; }

    /// Re-captures the placement an animated entity's pose is applied as an
    /// offset from. Call after moving an animated entity by hand: the animation
    /// owns the entity's transform, so without this the next step_animation()
    /// would undo the edit. No-op when the entity has no AnimationComponent.
    void rebase_animation(ecs::Entity entity);

    /// One animated entity's driven transform, in the same decomposed form the
    /// scene stores.
    struct AnimatedTransformSample {
        u32 entity_id = 0;
        Vec3 translation;
        Vec3 rotation_euler_degrees;
        Vec3 scale;
    };

    /// Snapshot of every animated entity's transform, ordered by entity id.
    ///
    /// This is what lets a caller tell "the animation ran" from "the animation
    /// produced a pose that never reached the transform". Those are different
    /// claims and only the second one is worth asserting: the phase's original
    /// defect was exactly a pose that was computed and then dropped, and a
    /// component-present check would have called that a pass.
    std::vector<AnimatedTransformSample> animated_transform_samples() const;

    // --- Audio (Phase 9) ----------------------------------------------------
    //
    // Builds the listener from the active camera, starts autoplay sources, takes
    // each source's world position from its entity transform, and mixes every
    // playing source through one `audio::AudioScene`.
    //
    // The scene is the whole mix, not just a router: buses and their Settings
    // volumes, the music/ambience layers and the reverb tail all run inside
    // `finalize`. Mixing component-by-component straight into the output buffers
    // (the old path) left every one of those systems built, tested and
    // unreachable from a shipped game.
    //
    // The device's own output is the base of the mix: the null backend
    // contributes silence, so a non-zero peak is the scene's audio and not the
    // backend's.
    u32 step_audio(f32 dt);

    /// Sources actually mixed by the last step_audio().
    size_t audio_sources_mixed() const { return m_audio_sources_mixed; }
    /// Largest absolute sample in the last mixed block. This is the observable
    /// that separates "the scene produced audio" from "the mixer ran and
    /// produced zeros" — an exit code cannot tell those apart.
    f32 audio_output_peak() const { return m_audio_output_peak; }
    /// Frames in the last mixed block (0 when the step covered no whole block).
    size_t audio_output_frames() const { return m_audio_left.size(); }
    const audio::AudioListener& audio_listener() const { return m_audio_listener; }
    /// The audio backend. Never null; a NullAudioDevice until one is installed.
    audio::AudioDevice* audio_device() const { return m_audio_device.get(); }
    /// The live mixer - the one object a Settings window binds to.
    ///
    /// This is the seam a game needs in order to ship its own audio settings
    /// without replacing the runtime: read/write `audio_scene().settings()` and
    /// the change applies to the next block. Also the way in to the music and
    /// ambience layers (`music()`) and the reverb zones.
    audio::AudioScene& audio_scene() { return m_audio_scene; }
    const audio::AudioScene& audio_scene() const { return m_audio_scene; }

    // --- Combat feedback (Phase 56) -----------------------------------------
    //
    // Shot/impact events are published every frame. This turns them into what a
    // player sees and hears, but only for a scene that opts in with a
    // `Feedback:` line: `m_feedback_bound` is false otherwise, and the render
    // and audio paths then do exactly nothing new. The director is exposed
    // read-only so a test (or a debug overlay) can tell "an event was
    // published" from "an effect actually spawned".
    [[nodiscard]] const runtime::CombatFeedback& combat_feedback() const { return m_feedback; }
    /// True when the loaded scene carries an enabled FeedbackComponent.
    [[nodiscard]] bool combat_feedback_bound() const { return m_feedback_bound; }

    // --- Perception (Phase 60) ----------------------------------------------
    //
    // The AI core's senses, bound to scene entities. `PerceptionComponent`
    // declares a sensor; the frame feeds it the scene's cast and its own shot and
    // impact events. Everything here is read-only from the outside: the state is
    // the sensor's memory, and a caller that could write it would be able to
    // forge awareness.
    //
    // `perception_of` is what the combat agent consults, and what a test reads to
    // tell "the event reached the sensor" from "the agent acted on it".
    [[nodiscard]] const ai::Perception* perception_of(ecs::Entity entity) const;
    /// Number of live sensors bound from the scene.
    [[nodiscard]] size_t perception_count() const { return m_perceptions.size(); }
    /// True when `sensor` currently knows about `target` (including the linger
    /// window after it went out of sight). False when either has no perception.
    [[nodiscard]] bool entity_aware_of(ecs::Entity sensor, ecs::Entity target) const;
    /// True when a sense received a stimulus from `target` THIS frame — strictly
    /// stronger than `entity_aware_of`.
    [[nodiscard]] bool entity_sensing(ecs::Entity sensor, ecs::Entity target) const;
    /// Where `sensor` last knew `target` to be. False when it never noticed it.
    [[nodiscard]] bool entity_last_known(ecs::Entity sensor, ecs::Entity target,
                                         Vec3& out_position) const;
    /// Drops one target from a sensor's memory. False when there was nothing to
    /// forget, or the entity has no perception.
    bool entity_forget(ecs::Entity sensor, ecs::Entity target);

    // --- Decision layer (Phase 61) ------------------------------------------
    //
    // Perception answers "what do I know"; the brain answers "what do I want to
    // do about it". The engine does not invent what "advance" means: it scores
    // the authored considerations, publishes the winner, and lets the game (or
    // Lua) act on the name. The one effect the engine does apply is the
    // authored `hold_fire`, because a decision the combat agent ignores is not
    // a decision.
    [[nodiscard]] size_t brain_count() const { return m_brains.size(); }
    /// The action that currently wins for `entity`: false when it has no brain
    /// or nothing has been decided yet.
    [[nodiscard]] bool brain_decision(ecs::Entity entity, std::string& out_name,
                                      f32& out_score) const;
    /// The value the frame computed for one input of one entity's brain. What a
    /// test (or a debug overlay) reads to prove the numbers are the world's and
    /// not the author's.
    [[nodiscard]] f32 brain_input_value(ecs::Entity entity, BrainInput input) const;
    /// True while the winning action asks the combat agent to hold its trigger.
    [[nodiscard]] bool brain_holds_fire(ecs::Entity entity) const;

    // --- State machines (Phase 62) -------------------------------------------
    //
    // The same inputs, the same publication policy: the engine reports which
    // state the entity is in and applies the authored `hold_fire`. It does not
    // invent what "patrol" means.
    [[nodiscard]] size_t state_machine_count() const { return m_machines.size(); }
    /// The state `entity` is currently in. False when it has no machine or the
    /// machine has not ticked yet.
    [[nodiscard]] bool entity_state(ecs::Entity entity, std::string& out_state) const;
    /// How many transitions the machine has fired. A replay asserts this with the
    /// state: the pair is the cheapest proof that two runs took the same path.
    [[nodiscard]] u32 entity_transition_count(ecs::Entity entity) const;

    // --- Gameplay modules (Phase 10) ----------------------------------------
    //
    // Modules come from GameplayModuleRegistry, so the Runtime only ever sees
    // the ones linked into the build. Each is instantiated once, given on_init,
    // and then stepped every frame by step_gameplay().
    //
    // Placement in the frame is deliberate: gameplay runs after physics,
    // animation and audio, and before transform propagation. A module therefore
    // observes this frame's simulation results, and an entity it repositions is
    // rendered at the new place in the same frame. A module that wants to steer
    // *physics* writes velocities, which take effect on the next fixed step —
    // the alternative (running gameplay first) would trade that for a module
    // that cannot see where the frame actually put anything.
    //
    // Ordering between modules is by update_priority(), lower first, ties broken
    // by name so the sequence does not depend on registration timing.
    u32 step_gameplay(f32 dt);

    /// Instantiates every registered module and calls on_init on each. Invoked
    /// by load_scene(); calling it again once modules exist is a no-op.
    void init_gameplay();

    /// Calls on_shutdown on every module and destroys them. Invoked by
    /// shutdown(); safe to call repeatedly.
    void shutdown_gameplay();

    [[nodiscard]] size_t gameplay_module_count() const { return m_gameplay_modules.size(); }

    /// Total on_update calls across all modules. The observable that separates
    /// "modules exist" from "modules are actually being driven" — a registry
    /// count alone cannot tell those apart.
    [[nodiscard]] size_t gameplay_updates() const { return m_gameplay_updates; }

    [[nodiscard]] gameplay::GameplayModule* find_gameplay_module(std::string_view name);

    /// Takes ownership of an already-built module. For a game that wants an
    /// instance the registry cannot construct, and for tests that need a probe
    /// with a known behaviour. Returns the module, or null if `module` was null.
    gameplay::GameplayModule* add_gameplay_module(std::unique_ptr<gameplay::GameplayModule> module);

    /// Copies each module's reflected state into its GameplayModuleComponent.
    /// Call before saving: the module holds live state, the component is the
    /// snapshot the scene file carries.
    void capture_gameplay_state();

    /// Reads each GameplayModuleComponent back into its module. Call after
    /// loading. A component naming a module this build does not have is left
    /// alone rather than dropped, so the scene keeps its data.
    void apply_gameplay_state();

    // --- Lua scripting (Phase 15 wired into the frame, Phase 24 editor) -----
    //
    // ScriptSystem owns one LuaVM and ticks every enabled
    // scripting::ScriptComponent in the scene. Placement is deliberate:
    // scripts run after gameplay and before destruction, so a script observes
    // this frame's simulation results (like a module) and damage it applies
    // still spawns shards in the same frame. Like gameplay, scripts only
    // advance on dt > 0 — the editor's edit-mode freeze (dt = 0) shows a
    // frozen authoring world, never a self-playing one.
    u32 step_script(f32 dt);
    /// Entities carrying a scripting::ScriptComponent right now.
    [[nodiscard]] size_t script_count() const;
    /// Enabled scripts with a non-empty source (the ones step_script ticks).
    [[nodiscard]] size_t scripts_runnable_count() const;
    /// Total script-entity ticks across all frames. The observable that
    /// separates "scripts exist" from "scripts are actually being driven".
    [[nodiscard]] size_t script_updates() const { return m_script_updates; }
    /// Forgets cached Lua environments (sources recompile lazily). Called on
    /// scene adopt so a previous scene's bytecode never serves the next one.
    void clear_script_cache();
    /// Loads every ScriptComponent path through the VFS into its source.
    /// Unresolvable paths keep an empty source with a warning — the scene
    /// keeps its path so a later save round-trips instead of dropping it.
    void resolve_scene_scripts();

    // --- Particles / cloth / AI (Phase 25 wiring) --------------------------
    //
    // Each owns live simulation objects for the scene's components and steps
    // them after scripts (they observe the same post-simulation world) and
    // before destruction (a script-triggered burst still spawns this frame).
    // Like scripts, all three only advance on dt > 0.
    u32 step_particles(f32 dt);
    u32 step_cloth(f32 dt);
    u32 step_ai_world(f32 dt);
    /// Live emitter/cloth/character counts (enabled components bound now).
    [[nodiscard]] size_t particle_emitter_count() const { return m_particles.size(); }
    /// Particles alive across every bound emitter.
    [[nodiscard]] size_t particles_alive() const;
    [[nodiscard]] size_t cloth_count() const { return m_cloths.size(); }
    [[nodiscard]] size_t character_count() const { return m_characters.size(); }
    /// Last grounded state written back by step_physics; false for unknown
    /// entities. Gameplay gates jumps on this instead of re-reading manifolds.
    [[nodiscard]] bool character_grounded(ecs::Entity e) const;
    /// The world-scale AI budget. Never null after construction; cleared on
    /// every scene adopt.
    ai::AIWorld* ai_world() { return m_ai_world.get(); }
    const ai::AIWorld* ai_world() const { return m_ai_world.get(); }
    /// Live simulation objects for gameplay/debug reads (dust effects, debug
    /// draws). Null when the entity has no bound object.
    const vfx::ParticleSystem* particle_system(ecs::Entity e) const;
    const physics::Cloth* cloth(ecs::Entity e) const;
    /// (Re)builds the live particle/cloth objects from the scene's enabled
    /// components. Called on scene adopt; characters are built separately in
    /// rebuild_physics_from_scene because they need the fresh physics world.
    void build_scene_particles();
    void build_scene_cloths();
    /// (Re)builds the scene's audio ENVIRONMENT — reverb zones, the music track
    /// and the ambience bed — into the live `AudioScene`. Called on scene adopt,
    /// like the particle and cloth builders above.
    ///
    /// The scene owns its environment, so this REPLACES the previous one: zones
    /// are cleared, and a theme or bed the new scene does not name is stopped.
    /// That is the same rule the particle and destructible builders follow, and
    /// the opposite of the post-process rule ("absent means unchanged") — the
    /// difference is that these are per-scene artefacts, not renderer state a
    /// caller might have configured directly.
    void build_scene_audio();

    /// Reads the first enabled FeedbackComponent into the director and binds
    /// the effect layer for this scene. Called from adopt_scene, so the
    /// previous scene's flashes, tracers, marks and voices never carry over.
    void bind_scene_feedback();

    // --- Perception (Phase 60) ----------------------------------------------
    //
    // `ai::Perception` has been CPU-only, deterministic and unit-tested since
    // Phase 22, and reachable from nothing: no scene could declare a sensor, no
    // frame fed it a cast or an event, no editor authored one. These four
    // methods are that connection. The same gap-closing shape Phase 25 used for
    // particles and cloth and Phase 19 used for destruction.
    //
    /// Binds every enabled PerceptionComponent into a live sensor. Called from
    /// adopt_scene: a scene change starts every NPC's memory empty, because the
    /// actors it remembered belong to the level that has just been replaced.
    void bind_scene_perception();
    /// Phase 68: shares the contacts each squad member personally senses with
    /// same-token members inside radio range. Runs at the top of the perception
    /// step, before any sensor integrates, so it reads the PREVIOUS frame's
    /// contacts and every receiver sees one snapshot (no same-frame relay).
    /// A report only sets awareness and the last known position — never a sense
    /// and never detection — so a receiver can hold/investigate but cannot fire
    /// at it and cannot pass it on.
    ///
    /// Phase 69: the same exchange delivers the lost-contact callouts queued at
    /// the END of the previous step, before the live snapshot, so a fresh sight
    /// always wins over an older last position. A lost callout is still a
    /// report: knowledge, never a sense, never a relay.
    void exchange_squad_radio();
    /// Feeds the sensors for this frame: rebuilds the cast, ages the noises
    /// still audible, routes this frame's pain to its victims, and steps every
    /// sensor. A no-op when the scene declared no sensor.
    void step_perception(f32 dt);
    /// True when the entity is something a sensor can notice: it has a Transform
    /// and something alive about it (hit points, a weapon, a brain or a side).
    /// Static scenery is deliberately not a target — being "aware of a wall" is
    /// a state with no meaning and every extra target costs every sensor a pass.
    [[nodiscard]] bool is_perception_target(const ecs::World& world, ecs::Entity entity) const;
    /// The scene-target id `entity` has this frame, or 0 when it is not one.
    [[nodiscard]] u32 perception_target_id(ecs::Entity entity) const;
    /// The id `target` has in SENSOR's own cast, or 0 when it is not in it.
    ///
    /// A sensor's cast is the shared cast minus the sensor itself, so every actor
    /// after it is numbered one lower. That shift is a property of the design
    /// (see the binding comment) and it is needed in four places — the queries,
    /// the stimulus attribution and the agent bridge — so it lives in exactly one
    /// function rather than in a table each caller rebuilds.
    [[nodiscard]] u32 perception_local_target_id(ecs::Entity sensor, ecs::Entity target) const;
    /// The inverse of `perception_local_target_id`: the SHARED cast id of a
    /// sensor's local id, or 0 when the local id is not one this cast contains.
    /// The radio speaks the shared language; a sensor's memory speaks its own.
    [[nodiscard]] u32 perception_shared_target_id(ecs::Entity sensor, u32 local) const;
    /// The one contact filter a radio report and a lost-contact callout share:
    /// alive target, not the reporter, not a same-token squadmate, and — when
    /// the reporter has a real team — hostile to it. Keeping both paths on this
    /// predicate means a callout can never name something a live report would
    /// have refused.
    [[nodiscard]] bool squad_contact_allowed(ecs::Entity reporter, ecs::Entity target,
                                             const std::string& token) const;
    // `track_squad_contacts` is declared with the perception state below, where
    // its `PerceptionBinding` parameter is complete.
    /// True when something solid stands between the two points. Only sight asks;
    /// hearing goes around corners and touch is contact by definition. `sensor`
    /// is skipped, because a ray that starts inside its own collider would
    /// otherwise report the sensor as its own wall.
    [[nodiscard]] bool perception_sight_blocked(ecs::Entity sensor, const Vec3& from,
                                                const Vec3& to);

    // --- Decision layer (Phase 61) ------------------------------------------
    /// Builds one live utility system per entity that authored a `Brain:` line.
    /// Called from adopt_scene: a decision belongs to the level that produced it.
    void bind_scene_brains();
    /// Feeds and ticks the brains for this frame. A no-op when the scene
    /// declared none.
    void step_brains(f32 dt);
    /// The inputs, filled from the world for one entity's brain. Deterministic
    /// and allocation-free: it reads what the frame already computed.
    void fill_brain_inputs(const ecs::World& world, ecs::Entity entity,
                           std::array<f32, kBrainInputCount>& out) const;
    /// Builds one live machine per entity that authored a `States:` line.
    void bind_scene_state_machines();
    /// Ticks the machines for this frame, feeding them the same inputs the brains
    /// get. A no-op when the scene declared none.
    void step_state_machines(f32 dt);

    // --- Navigation (Phase 28 — the voxel NavMesh, in the frame and the scene)
    //
    // The navmesh itself (nf::ai::NavMesh) is CPU-only, deterministic and was
    // unit-tested from the day it landed — and reachable from nothing: no scene
    // could name a volume, no frame stepped an agent, no editor authored either.
    // This closes that gap the same way Phase 25 did for particles and cloth:
    // the scene declares a volume and its walkers, the frame steps them, the
    // editor authors both.
    //
    // The bake reads the scene honestly: the floor is the volume's own floor
    // height (there is no terrain component to sample) and the obstacles are
    // the scene's STATIC BOX colliders, so a crate an author places is a step
    // the mesh knows about. Everything richer is a caller's `HeightSampler`
    // through build_scene_navmesh(), not a scene-format change.
    //
    // Placement in the frame is deliberate and mirrors AIWorld: agents step
    // after gameplay, scripts and the AI plan (a script that teleports an actor
    // this frame is already visible to its path), and before particles — a
    // walker's dust spawns where it now stands, not where it was.

    /// Bakes the mesh for the scene's first enabled `NavMeshComponent` and
    /// binds live path state to every enabled `NavAgentComponent`. A scene with
    /// no volume drops the previous scene's mesh and agents, like every other
    /// per-scene artefact here.
    void build_scene_navmesh();

    /// Perception gate for a patrol (Phase 65): true when the walker's senses
    /// currently know (`Aware`) or read (`Sensing`) a threat. A threat is
    /// another live perception target on a hostile team; a walker with no team
    /// cannot tell friend from foe, so anyone it notices counts. Reads the
    /// senses the previous perception step left behind — navigation runs before
    /// perception in the frame, and a one-frame delay is invisible to a walk.
    ///
    /// When `out_last_known` is non-null, it receives the strongest noticed
    /// threat's last known position (Phase 66's investigation destination).
    /// When `out_personally_perceived` is non-null, it reports whether that
    /// threat has ever been sensed by this walker itself (Phase 68): a search
    /// born only from a radio report must not open a squad call of its own.
    [[nodiscard]] bool nav_route_threat_perceived(ecs::Entity walker,
                                                  NavRouteHoldPolicy policy,
                                                  Vec3* out_last_known = nullptr,
                                                  bool* out_personally_perceived = nullptr) const;

    // --- Terrain (Phase 63) ------------------------------------------------
    //
    // The scene's `Terrain:` entities are one height source with three readers:
    // the mesh drawn on screen, the sampler the navigation bake walks on, and
    // the ground the physics world is told about. They all call
    // `terrain_height` with the same options, so they cannot disagree about
    // where the surface is — the whole point of authoring the field instead of
    // modelling it twice.

    /// Builds and uploads one mesh per enabled `Terrain:` entity. Called when a
    /// scene is adopted, before the navigation bake, because the bake samples
    /// the same field. A run with no device (headless, `--validate`) builds
    /// nothing and says so; the bake still follows the terrain, because the
    /// height source does not need a GPU.
    void build_scene_terrain();

    /// Terrains that produced a drawable mesh. Zero in a headless run, and zero
    /// for a scene that never authored one.
    [[nodiscard]] size_t terrain_mesh_count() const { return m_terrain_meshes.size(); }

    /// Steps every bound agent along its path, writing the result into its
    /// Transform. Returns the number of agents that moved this frame. Only
    /// advances on dt > 0 (callers go through update(), which enforces it).
    u32 step_navmesh(f32 dt);

    /// The live mesh, or null when the scene has no navigation volume. Const and
    /// non-const for the same reason `ai_world()` has both: the editor reads
    /// debug stats, a game may re-tune the bake in place.
    ai::NavMesh* navmesh() { return m_navmesh.get(); }
    const ai::NavMesh* navmesh() const { return m_navmesh.get(); }
    /// True when a volume was baked (false for a scene with no NavMesh line).
    [[nodiscard]] bool navmesh_built() const { return m_navmesh && m_navmesh->built(); }
    /// Polygons in the live mesh, 0 when there is none — the "the bake ran and
    /// produced something" observable.
    [[nodiscard]] size_t navmesh_polygon_count() const {
        return m_navmesh ? m_navmesh->polygon_count() : 0;
    }
    /// Enabled agents bound right now.
    [[nodiscard]] size_t nav_agent_count() const { return m_nav_agents.size(); }
    /// Agents whose last step drove them onto their goal. The difference
    /// between "walkers exist" and "walkers arrived". A patrolling agent counts
    /// while it waits at a route point; a non-looping route that reached its
    /// last point stays counted (see nav_route_state's `finished`).
    [[nodiscard]] size_t nav_agents_at_goal() const {
        size_t n = 0;
        for (const auto& [entity, state] : m_nav_agents) {
            (void)entity;
            if (state.at_goal) ++n;
        }
        return n;
    }
    /// The live cursor of an authored patrol route (Phase 64): point index,
    /// point count, seconds left in the wait at that point, and whether a
    /// non-looping route has ended at its last point. False when the entity has
    /// no enabled route of at least two points or no bound agent (no bake, or
    /// the agent/route is disabled). The points are scene data; this is the
    /// session state no file holds.
    ///
    /// `out_held` (Phase 65) is optional so every Phase 64 caller keeps its
    /// exact signature. It reports whether a perception gate is holding the
    /// patrol this frame. `out_investigating` (Phase 66) is optional for the
    /// same reason and reports the last-known-position detour.
    [[nodiscard]] bool nav_route_state(ecs::Entity entity, u32& out_index, u32& out_count,
                                       f32& out_wait, bool& out_finished,
                                       bool* out_held = nullptr,
                                       bool* out_investigating = nullptr) const;
    /// Obstacles the last bake carved (static box colliders seen at build time).
    [[nodiscard]] size_t navmesh_obstacle_count() const { return m_nav_obstacle_count; }

    // --- Vehicles (Step 11 — Jolt arcade cars in the frame and the scene) ---
    //
    // The deterministic solver cannot do vehicles (no VehicleConstraint), so
    // cars live in their own JoltWorld — the same "dedicated world" pattern
    // destruction uses for shards, for the same reason: one world's bodies
    // must never be simulated twice.
    //
    // The scene declares the tune (VehicleComponent::config); the frame steps
    // it; the editor authors it. Drive inputs are live-only (written straight
    // into the component by gameplay or the editor's debug drive, never
    // saved), like Character's wish_dir/jump.

    /// (Re)builds the vehicle world from the scene's enabled VehicleComponents:
    /// a fresh JoltWorld, the level's static colliders synced into it, and one
    /// JoltVehicle per car. Called on scene adopt; a scene with no enabled
    /// car tears everything down, like every other per-scene artefact here.
    /// Also the right call after restoring a pre-play snapshot (Stop), which
    /// is why it is public and idempotent rather than adopt-private.
    void bind_scene_vehicles();
    /// Steps every bound car on the fixed vehicle clock: drive inputs in, Jolt
    /// solves, chassis poses land in the entities' Transforms. Only advances
    /// on dt > 0 (callers go through update(), which enforces it).
    u32 step_vehicles(f32 dt);
    /// Uprights the car in place and zeroes its velocity (the R key in the
    /// demo). A recovered car must not keep rendering on its roof while the
    /// physics says it is upright — see VehicleSystem::reset.
    bool reset_vehicle(ecs::Entity e);
    /// Bound cars right now.
    [[nodiscard]] size_t vehicle_count() const;
    /// Moving-obstacle mirrors currently bound (one per simulated scene body in
    /// the vehicle world). A test-facing count, not a gameplay knob.
    [[nodiscard]] size_t vehicle_obstacle_count() const;

    // --- Vehicle network replication (Step 14) --------------------------------
    //
    // The engine-side half of the VehicleSnapshot pipeline. Runtime speaks
    // ENGINE types only (`physics::JoltBodyState`); the bridge to the wire —
    // net id ↔ net::VehicleNetState, encode/decode, sockets — is the session
    // owner's job (game layer), the same split that lets NFNetworking stay
    // free of engine worlds. Contract: a car WITHOUT a net id is not
    // replicated, and the wire never carries net id 0.

    /// One car's chassis on the wire: the engine solver's pose + velocity.
    struct VehicleNetRecord {
        u32 net_id = 0;
        physics::JoltBodyState state{};
    };

    /// Assigns the wire identity a car replicates under (a server uses its own
    /// entity ids; a client what the session manifest says). Rebinding a scene
    /// clears every assignment — sessions own ids, scenes do not.
    void assign_vehicle_net_id(ecs::Entity e, u32 net_id);

    /// One record per bound car that HAS a net id, read at the last step
    /// (`chassis_state()` is a read, so a collection costs nothing to the sim).
    /// The tick lives on the snapshot the session builds from these.
    bool collect_vehicle_net_states(std::vector<VehicleNetRecord>& out) const;

    /// Applies REMOTE chassis poses. A record is skipped when it carries no
    /// known net id, when its assignment died, when the target car is
    /// SIMULATED BY THIS WORLD (applying authority to a body this world owns
    /// is a fight this world loses), or when its tick is not newer than the
    /// last one accepted for that entity (`tick == 0` never applies). Writes
    /// the ghost's Transform exactly as VehicleSystem writes a simulated one.
    /// Returns the number of poses actually applied.
    u32 apply_vehicle_net_states(const std::vector<VehicleNetRecord>& in, u32 tick);
    /// Per-wheel render state for one car (pose, steer, spin, contact) — what
    /// wheel meshes and dust effects pose from. False when the car has no live
    /// vehicle.
    bool vehicle_wheel_states(ecs::Entity e, std::vector<physics::JoltWheelState>& out) const;
    /// The navigation debug snapshot: mesh outlines plus every bound agent's
    /// live route, copied out together so the overlay draws one consistent
    /// frame. Empty when the scene has no baked volume. Agents sorted by
    /// entity id (the live map is unordered); see NavDebugSnapshot.
    [[nodiscard]] NavDebugSnapshot nav_debug_snapshot() const;

    /// The input source handed to modules. Non-owning; null means no input, and
    /// modules are expected to check.
    void set_input_source(gameplay::IInputSource* source) {
        // Hand the pointer back before letting go of the source that owns it:
        // a captured cursor whose owner is gone is a hidden, clipped cursor
        // with nobody left to release it.
        if (m_input_source != nullptr && m_input_source != source) {
            m_input_source->set_pointer_capture(false);
        }
        m_input_source = source;
    }
    [[nodiscard]] gameplay::IInputSource* input_source() const { return m_input_source; }

    /// Whether the game is currently playing (vs. editing). Mirrored into
    /// every GameplayContext built from here: modules that own scene objects
    /// (notably the camera) must stay out while editing so editor gestures —
    /// viewport navigation writes the camera Transform every frame — are not
    /// overwritten on the next step. The editor sets this from its play state
    /// before each update; harness/tests default to editing (false).
    void set_playing(bool playing) { m_playing = playing; }
    [[nodiscard]] bool playing() const { return m_playing; }

    /// The yaw of the scene's active camera, in degrees; 0 when the scene has
    /// no active camera.
    ///
    /// Read from the component every call rather than cached: the camera is
    /// aimed once per frame and a script runs after it, so the number a
    /// script sees has to be the one the player just produced.
    [[nodiscard]] f32 camera_yaw() const;

    /// Adds camera shake, as trauma in [0, 1]. Scripts call it through
    /// `nf.shake(amount)`; gameplay calls it on an impact.
    ///
    /// Trauma decays on its own and the visible offset scales with its
    /// square, so two small hits do not add up to a big one (CameraShake.hpp
    /// explains the curve). The scene's CameraComponent trims the result with
    /// `shake_strength` and `shake_decay`, so a cutscene camera can opt out
    /// without the scripts knowing.
    void add_camera_shake(f32 amount);

    /// The current trauma — for a test, or a HUD that wants to show it.
    [[nodiscard]] f32 camera_trauma() const { return m_camera_shake.trauma(); }

    /// Adds recoil, in degrees: positive pitch raises the view, positive yaw
    /// turns it left. Scripts call it through `nf.recoil(pitch, yaw)`.
    ///
    /// It is an OFFSET on the view, not a change to the aim the player built:
    /// the kick recovers on its own and the view returns to exactly that aim.
    /// Clamped by the scene's `recoil_max`, so a burst cannot point the
    /// camera at the sky.
    void add_recoil(f32 pitch_deg, f32 yaw_deg);

    // --- Frame-window policy (Phase 42) -----------------------------------
    //
    // The governor above reacts to one smoothed frame; this reacts to the
    // WINDOW the host already measures. A scene opts in with
    // `frame_policy=true` on the post-process line, and the host feeds the
    // window each frame. Without a fed window the policy holds — it never
    // guesses from an empty statistic.
    void set_frame_window(const rendering::FramePacingStats& stats) { m_frame_window = stats; }
    [[nodiscard]] f32 frame_policy_scale() const { return m_frame_policy_state.scale; }
    [[nodiscard]] rendering::FramePolicyReason frame_policy_reason() const {
        return m_frame_policy_reason;
    }

    /// The live recoil offset, in degrees — for a test or a HUD.
    [[nodiscard]] f32 recoil_pitch() const { return m_recoil.pitch; }
    [[nodiscard]] f32 recoil_yaw() const { return m_recoil.yaw; }

    // --- Combat (Phase 44) -------------------------------------------------
    //
    // The gun is scene data (a `Weapon:` line on an entity) and the maths is
    // NF/Runtime/Weapon.hpp; this block is the third piece — the part that
    // needs a world, a camera and an input source.
    //
    // The rule that ties the three together: the entity the ACTIVE camera
    // follows is the player, and the player's weapon is the one the trigger
    // drives. Every other weapon in the scene still ticks (a reload finishes,
    // a cooldown expires) but fires only when a script asks it to, so an NPC's
    // rifle does not empty itself on the player's trigger.
    //
    // A shot is traced from the CAMERA through the crosshair, not from the
    // muzzle: a third-person camera that fired from the character's hip would
    // miss what the crosshair covers by the parallax between the two. The
    // direction includes this frame's recoil, so the crosshair and the bullet
    // agree about where the gun is pointing.

    /// What the player's weapon is doing right now.
    struct WeaponSnapshot {
        bool has_weapon = false;
        bool enabled = false;
        FireMode mode = FireMode::Semi;
        u32 ammo = 0u;
        u32 magazine = 0u;
        u32 reserve = 0u;
        bool reloading = false;
        /// 0 at the start of a reload, 1 when it completes. 0 when not reloading.
        f32 reload_progress = 0.0f;
        /// The cone the next shot would use, in degrees, bloom included.
        f32 spread_deg = 0.0f;
        /// Shots this runtime has fired, and how many of them hit something.
        u32 shots_fired = 0u;
        u32 hits_landed = 0u;
        /// Projectiles still in flight (Phase 46). Hitscan weapons always
        /// report zero, because their shot is resolved in the firing frame.
        u32 active_projectiles = 0u;
        /// Sum of the damage actually applied to hit points.
        f32 damage_dealt = 0.0f;
        /// Sum of the damage absorbed by armor (Phase 50). Kept separate from
        /// `damage_dealt` so existing HUDs that mean "health removed" keep
        /// their number, while a combat log can still show the full hit.
        f32 armor_damage_dealt = 0.0f;
        /// Radial blasts detonated and the extra entities they hit (Phase 51).
        u32 explosions = 0u;
        u32 splash_hits = 0u;
        /// Damage-over-time ticks and the health damage they applied (Phase 54).
        u32 status_ticks = 0u;
        f32 status_damage_dealt = 0.0f;
    };

    /// A copy of the player's weapon state. `has_weapon` is false when the
    /// scene has no active camera, or the camera follows an entity with no
    /// `WeaponComponent` — both are ordinary states, not errors.
    [[nodiscard]] WeaponSnapshot weapon_snapshot() const;

    /// Starts a reload on the player's weapon. Returns false when there is no
    /// weapon, the magazine is already full, or the reserve is empty.
    bool weapon_reload();

    /// Fires one volley now, bypassing the trigger: for a script that shoots
    /// on a timer, and for a test that cannot press a key. The fire-rate clock
    /// still applies, so this cannot be used to cheat the cadence. Returns the
    /// number of projectiles traced (0 when the weapon was not ready).
    u32 fire_weapon();

    /// The last projectile's result, for a HUD hit marker or a Lua script.
    struct WeaponHitInfo {
        bool hit = false;
        ecs::Entity entity{};
        /// Empty when the hit entity has no name, or when nothing was hit.
        std::string name;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 normal{0.0f, 0.0f, 0.0f};
        f32 distance = 0.0f;
        /// Damage after distance falloff, before the target's own rules
        /// (invulnerable, disabled) reduced it. `applied` is what landed.
        f32 damage = 0.0f;
        f32 applied = 0.0f;
        /// Points removed from armor in this hit (Phase 50).
        f32 armor_absorbed = 0.0f;
        /// Where on the collider the round landed, for a hit marker or a test.
        HitZone hit_zone = HitZone::Body;
        /// The type of damage the weapon carried (Phase 52).
        DamageType damage_type = DamageType::Ballistic;
        /// True when the shot struck an entity on the shooter's own team and
        /// was stopped without damage (Phase 46). The geometry hit; the social
        /// rules refused it.
        bool blocked_by_faction = false;
    };
    [[nodiscard]] const WeaponHitInfo& last_weapon_hit() const { return m_last_weapon_hit; }

    /// Every resolved impact from the frame that just ran (Phase 49). A host
    /// draws sparks/decals from this; a script reads the previous frame's list
    /// through `nf.impact_count()` / `nf.impact(i)`.
    [[nodiscard]] const std::vector<ImpactEvent>& impact_events() const {
        return m_impacts.current();
    }
    [[nodiscard]] const std::vector<ImpactEvent>& previous_impact_events() const {
        return m_impacts.previous();
    }
    /// Drops both frame slots. Used when a scene is adopted.
    void clear_impact_events() { m_impacts.clear(); }

    /// Every shot fired in the frame that just ran (Phase 53), and the
    /// completed frame a script reads. One event per pellet, with the spread
    /// direction that was actually traced.
    [[nodiscard]] const std::vector<ShotEvent>& shot_events() const {
        return m_shots.current();
    }
    [[nodiscard]] const std::vector<ShotEvent>& previous_shot_events() const {
        return m_shots.previous();
    }
    /// Drops both shot slots. Used when a scene is adopted.
    void clear_shot_events() { m_shots.clear(); }

    /// Applies damage to an entity's hit points. Returns what was actually
    /// taken (0 for an entity with no HealthComponent, an invulnerable one, or
    /// a dead one). Scripts call it through `nf.damage`.
    f32 apply_health_damage(ecs::Entity entity, f32 amount);

    /// The entity's hit points, or null when it has none.
    [[nodiscard]] const HealthComponent* health_of(ecs::Entity entity) const;

    /// Applies or refreshes a status effect by scene-format kind name
    /// ("burn", "bleed", "corrode"). Returns false for an unknown kind, an
    /// invalid dose/duration/tick, a dead entity or a disabled status set.
    bool apply_status(ecs::Entity entity, std::string_view kind, f32 damage_per_second,
                      f32 seconds, f32 tick_seconds = 0.5f);

    /// Seconds left for one kind, or 0 when absent.
    [[nodiscard]] f32 status_remaining(ecs::Entity entity, std::string_view kind) const;

    /// Removes one kind. Returns true when it was present.
    bool clear_status(ecs::Entity entity, std::string_view kind);

    /// Persistent damage memory for an entity, or null when it has never been
    /// hit (Phase 55).
    [[nodiscard]] const DamageLedgerComponent* damage_ledger_of(ecs::Entity entity) const;

    /// Current suppression level of an entity, or 0 when it is not an agent.
    [[nodiscard]] f32 suppression_of(ecs::Entity entity) const;

    // --- Player settings --------------------------------------------------
    //
    // Sensitivity, inverted-Y, FOV and shake are authored per scene, but they
    // are the player's taste, not the level's. This block is the override the
    // player owns; the scene keeps its authored values untouched, and a scale
    // of 1.0 / an FOV of 0 means "use the scene's".
    //
    // The file is optional. `load_scene` reads it from
    // `content://Settings/player.nfsettings` when it exists, and a file that
    // does not parse leaves the previous settings alone with a warning —
    // half-applied settings are worse than none, because the player cannot
    // tell which half took.
    static constexpr const char* kDefaultPlayerSettingsPath =
        "content://Settings/player.nfsettings";

    [[nodiscard]] bool load_player_settings(const std::string& logical_path,
                                            std::string& out_error);
    [[nodiscard]] bool save_player_settings(const std::string& logical_path,
                                            std::string& out_error) const;
    [[nodiscard]] const PlayerSettings& player_settings() const { return m_player_settings; }
    /// Replaces the settings and pushes the parts that are live engine state
    /// (the audio bus volumes) immediately, so a slider takes effect on the
    /// next mix block rather than on the next scene load.
    void set_player_settings(const PlayerSettings& settings);

    // --- Deterministic input record + replay --------------------------------
    //
    // A save restores what the scene looked like; an input log restores how it
    // got there. Recording sits over the live input source rather than beside
    // it, so what is captured is exactly what gameplay polled — an action no
    // module read cannot have changed the outcome, and is not in the log.
    //
    // The recorder and the replay are both IInputSources, which is what makes
    // the round trip honest: capture and replay go through the same code path
    // the real device does, so a difference between them is a difference in the
    // log, not in how it was sampled.

    /// Wraps the current input source (which may be null) in a recorder. Every
    /// read a module makes from now on lands in the log, one frame per step.
    void begin_input_capture();

    /// Removes the recorder and returns the log it gathered. The source that
    /// was live before capture is restored. No-op when capture was never begun.
    InputLog end_input_capture();

    /// Replays a log in place of the live source: frame `n` of the log answers
    /// every input query during step `n`. Call before the first update().
    void play_input_log(const InputLog& log);

    /// Removes the replay source and restores the input source captured over.
    void stop_input_replay();

    [[nodiscard]] bool input_capture_active() const { return m_input_recorder != nullptr; }
    [[nodiscard]] bool input_replay_active() const { return m_input_replay != nullptr; }

    // --- Destruction (Phase 19) ---------------------------------------------
    //
    // NFDestruction decides what breaks; this is the half that makes it happen
    // in a running game. Off by default, like streaming: a runtime that never
    // calls enable_destruction() never builds a Jolt world for it and pays
    // nothing.
    //
    // When enabled the Runtime owns a Jolt world dedicated to debris plus the
    // scene's *static* colliders, so a shard comes to rest on the level instead
    // of falling through it. The scene's dynamic bodies stay with the engine's
    // own solver — two solvers on one body would fight — which makes the debris
    // world a one-way street: the scene shatters into it, never the reverse.
    //
    // Fracture assets are registered once and referenced by index from a
    // destructible binding (the same table design DestructibleComponent's
    // asset_index presupposes), so two crates of the same mesh share one asset
    // and carry their own damage.

    struct DestructionConfig {
        destruction::DestructionBudget budget{};
        physics::DebrisSinkConfig    sink{};
    };

    /// Builds the debris world, the sink and the destruction world, and adds the
    /// scene's static colliders so shards land on the level. No-op when already
    /// enabled (call disable_destruction() first to reconfigure). Returns false
    /// when the Jolt world could not be created, in which case nothing is built.
    bool enable_destruction(const DestructionConfig& config = DestructionConfig{});

    /// Retires every shard, drops the bindings and tears the three objects down.
    /// The next enable_destruction() starts clean.
    void disable_destruction();
    [[nodiscard]] bool destruction_enabled() const { return m_destruction_enabled; }

    /// Takes a copy of a cooked asset and returns its index in the runtime's
    /// table, or kInvalidChunk when the asset is empty. That index is what
    /// bind_destructible() refers to.
    u32 register_fracture_asset(const destruction::FractureAsset& asset);
    [[nodiscard]] u32 fracture_asset_count() const { return static_cast<u32>(m_fracture_assets.size()); }

    /// Makes `entity` breakable using asset `asset_index`. Creates the live
    /// per-bond and per-chunk state the destruction world needs; rebinding the
    /// same asset keeps any damage already recorded. Returns false when the
    /// asset index is unknown or the entity has no Transform to place the
    /// asset in the world with — both are caller errors, not silent skips.
    bool bind_destructible(ecs::Entity entity, u32 asset_index);
    [[nodiscard]] size_t destructible_count() const { return m_destructibles.size(); }

    /// Applies one blast to one destructible. The entity's propagated world
    /// transform places the asset in the world, so call this after the scene's
    /// transforms have propagated (update() does this every frame). Returns the
    /// bonds that shattered, which is not the shard count: one bond can release
    /// a region that comes apart into several pieces.
    u32 apply_damage(ecs::Entity entity, const destruction::DamageEvent& event);

    /// Steps the debris world on a fixed clock (same contract as step_physics)
    /// and ages the shards against the budget. Called by update() when
    /// destruction is enabled; returns the fixed steps actually taken.
    u32 step_destruction(f32 frame_delta);

    /// One live shard's pose. `position` is the body's shape-local origin in
    /// world space, exactly the point the shard was spawned at — see
    /// JoltWorld::state() for why that is not Jolt's centre-of-mass position.
    /// A renderer posing the chunk mesh offsets it by the chunk's own local
    /// centroid from here; the runtime reports the body, not the render proxy.
    struct DebrisSample {
        u32  id = destruction::kInvalidDebris;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Quat rotation = Quat::identity();
        Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    };

    /// Every live shard, ordered by debris id. The order is the same on every
    /// run (ids come from a counter, spawns arrive in fracture order), which is
    /// what makes "the shards really fell" assertable rather than eyeballable.
    std::vector<DebrisSample> debris_samples() const;

    /// Shards alive right now, in both the destruction world and the sink.
    [[nodiscard]] size_t active_debris() const;
    /// Bonds shattered across every apply_damage() since enable_destruction().
    [[nodiscard]] u32 bonds_shattered_total() const;
    /// Shards the budget retired and still had no room for — a visible signal
    /// that the cap is too low for the scene rather than shards vanishing.
    [[nodiscard]] u32 shards_dropped_for_budget() const;
    /// Static colliders currently in the debris world.
    [[nodiscard]] size_t debris_static_count() const { return m_debris_statics.size(); }

    // Distinct meshes that failed to load. A failed asset is terminal, so this
    // is one entry per bad asset regardless of frame count.
    size_t failed_mesh_count() const { return m_failed_meshes.size(); }
    // How many times a mesh failure has been reported. This is the observable
    // that makes the once-per-asset contract assertable: the bug it guards
    // against re-reported the same dead asset on every frame (2 per frame, so
    // 32 after 16 frames), which a distinct-id count cannot detect because
    // re-inserting an existing id does not grow the set.
    size_t failed_mesh_reports() const { return m_failed_mesh_reports; }
    const rendering::Renderer3D* renderer() const { return m_renderer.get(); }

    // --- GPU profiling (Phase 39) ---
    //
    // One high-level switch for the two renderer toggles: frame timestamps and
    // the per-pass breakdown. It is a DEVELOPER setting, not scene data: with
    // it on, every frame records timestamp queries and the ping-pong result is
    // read one slot later. Per-pass timing also puts a BottomOfPipe between
    // passes, which serialises the GPU; that is acceptable for an editor or an
    // explicit diagnostic run, not for gameplay.
    //
    // The request is remembered even when it is made before the lazy renderer
    // exists (or across shutdown/re-init), so a scene that loads later still
    // gets the profiling state the caller asked for. `gpu_profiling_supported`
    // reports whether the device/queue can actually timestamp.
    void set_gpu_profiling_enabled(bool enabled);
    bool gpu_profiling_enabled() const { return m_gpu_profiling_enabled; }
    bool gpu_profiling_supported() const {
        return m_renderer != nullptr && m_renderer->gpu_timing_supported();
    }

    /// The renderer's exposure - the one renderer setting the editor is allowed
    /// to write, and the reason this exists.
    ///
    /// `renderer()` is const on purpose: a caller must not be able to reach into
    /// arbitrary renderer state from outside. That left the editor able to READ
    /// exposure and unable to write it, which is why the Settings > Rendering
    /// section shipped with shadows and no exposure, and why the localization key
    /// `exposure` has sat unused since Phase 15. A narrow pair rather than a
    /// mutable `renderer()` gives the editor exactly the control it needs and
    /// nothing more.
    ///
    /// Exposure is a DISPLAY preference, not scene data — deliberately not
    /// persisted per scene, because the editor's acceptance run pins its golden
    /// pixel count to the default. Setting it here therefore changes no scene and
    /// is not an undoable edit.
    ///
    /// A non-finite or non-positive value is REFUSED and the previous exposure
    /// kept: the renderer's own setter stores whatever it is handed, and a NaN
    /// exposure propagates through the tonemap to black or white every pixel of
    /// the frame. The editor's slider cannot produce one, but a game's settings
    /// screen is a caller too, and a bad value there must not be a frame-wide
    /// failure. Returns true when the value was applied.
    bool set_exposure(float exposure) {
        if (m_renderer == nullptr || !std::isfinite(exposure) || exposure <= 0.0f) {
            return false;
        }
        m_renderer->set_exposure(exposure);
        return true;
    }
    float exposure() const { return m_renderer != nullptr ? m_renderer->exposure() : 1.0f; }

    /// Image-based lighting on/off, exposed for the same reason `set_exposure`
    /// is: the editor's look-dev and capture flows (notably --no-ibl) need to
    /// compare the sky-baked look against the scalar-ambient look without
    /// touching scene data. No validation needed — any bool is meaningful.
    void set_ibl_enabled(bool enabled) {
        if (m_renderer != nullptr) {
            m_renderer->set_ibl_enabled(enabled);
        }
    }

    /// Screen-space ambient occlusion on/off, exposed for the same reason:
    /// capture flows and game settings screens need the compare switch
    /// without touching scene data.
    void set_ssao_enabled(bool enabled) {
        if (m_renderer != nullptr) {
            m_renderer->set_ssao_enabled(enabled);
        }
    }

    /// The renderer's tonemap operator, exposed for the same reason
    /// `set_exposure` is: `renderer()` is const by design, and a scene may
    /// author the operator per level (a stylized level wants ACES, a
    /// diagnostic capture wants Linear).
    ///
    /// The operator is a closed set, so anything outside it is refused rather
    /// than clamped: `TonemapMode` has no "next" to fall back to, and silently
    /// picking one would make a typo look like a working setting.
    bool set_tonemap_mode(rendering::TonemapMode mode) {
        if (m_renderer == nullptr) {
            return false;
        }
        switch (mode) {
            case rendering::TonemapMode::Exponential:
            case rendering::TonemapMode::ACES:
            case rendering::TonemapMode::Reinhard:
            case rendering::TonemapMode::Linear:
                break;
            default:
                return false;
        }
        m_renderer->set_tonemap_mode(mode);
        return true;
    }
    rendering::TonemapMode tonemap_mode() const {
        return m_renderer != nullptr ? m_renderer->tonemap_mode()
                                     : rendering::TonemapMode::Exponential;
    }

    // --- GPU picking ---
    //
    // Resolves the entity under a pixel of the last rendered frame, using an
    // id pass so occlusion is respected (unlike the CPU ray/AABB path, which
    // tests world bounds and cannot tell what is in front of what).
    //
    // `x`/`y` are in pixels, origin top-left, in the space of the rendered
    // target. Returns false — leaving `out_entity_id` untouched — when there is
    // no scene, the picker is unavailable, or no object covers that pixel, so
    // callers can fall back to the CPU path.
    bool pick_entity_gpu(uint32_t x, uint32_t y, uint32_t& out_entity_id);

    // True when GPU picking is usable (pick shaders present and loaded).
    bool gpu_picking_available() const { return m_picker != nullptr; }

    // --- Shared material assets (Phase 5) ---
    // MeshComponent::material names a .nfmat logical path; every entity with
    // the same path shares one renderer instance (one 64-byte UBO). Edits
    // rewrite that UBO in place — visible next frame, no pipeline touched.
    rendering::MaterialHandle material_for_path(const std::string& logical_path);
    bool set_material_params(const std::string& logical_path,
                             const rendering::PBRMaterialParams& params, std::string& out_error);
    bool material_params(const std::string& logical_path,
                         rendering::PBRMaterialParams& out) const;
    std::vector<std::string> known_material_paths() const;
    // Writes the entry's current params to dst_path (same-path save clears
    // the entry's dirty flag; copies leave it set).
    bool save_material(const std::string& src_path, const std::string& dst_path,
                       std::string& out_error);

    // --- Texture assets for material albedo (Phase 5) ---
    // Decoded once from content://, uploaded, and cached; materials reference
    // textures by path and rebind automatically on reload.
    bool ensure_texture(const std::string& logical_path, std::string& out_error);
    // Re-decodes and re-uploads (failure keeps the old texture live), then
    // rebinds every material using it. Hot Reload's texture path.
    bool reload_texture(const std::string& logical_path, std::string& out_error);
    // Reloads a mesh asset from its registry cooked file into the live mesh
    // library slot (handles stay valid; bounds follow the new geometry).
    // Failure keeps the old GPU copy. Hot Reload's mesh path.
    bool hot_reload_mesh(const assets::AssetId& id, std::string& out_error);
    // Re-reads a .nfmat file into the live instance (no dirty flag: the file
    // is the source of truth). Entries with unsaved UI edits are SKIPPED with
    // a warning — the user's work always wins over the disk.
    bool reload_material_file(const std::string& logical_path, std::string& out_error);
    // Binds (or, when empty, unbinds) a material's albedo. Marks dirty.
    bool set_material_albedo(const std::string& material_path, const std::string& texture_path,
                             std::string& out_error);
    std::string material_albedo(const std::string& material_path) const;
    // PBR maps, same contract as the albedo pair above (bind/unbind + dirty).
    // Keys in the .nfmat: "normal:", "mrough:", "occlusion:", "emissive_map:".
    bool set_material_normal(const std::string& material_path, const std::string& texture_path,
                             std::string& out_error);
    std::string material_normal(const std::string& material_path) const;
    bool set_material_mrough(const std::string& material_path, const std::string& texture_path,
                             std::string& out_error);
    std::string material_mrough(const std::string& material_path) const;
    bool set_material_occlusion(const std::string& material_path, const std::string& texture_path,
                                std::string& out_error);
    std::string material_occlusion(const std::string& material_path) const;
    bool set_material_emissive_map(const std::string& material_path, const std::string& texture_path,
                                   std::string& out_error);
    std::string material_emissive_map(const std::string& material_path) const;
    std::vector<std::string> known_texture_paths() const;
    // Sampler mip filtering per material ("none"|"nearest"|"linear" in the
    // .nfmat "mip:" key; Linear when the file is silent). Changing it rebinds
    // the material to the matching cached sampler — no re-upload. Marks dirty.
    bool set_material_mip_mode(const std::string& material_path, rhi::MipMapMode mode,
                               std::string& out_error);
    rhi::MipMapMode material_mip_mode(const std::string& material_path) const;
    bool material_dirty(const std::string& logical_path) const;
    bool any_material_dirty() const;

    // --- Editor hooks (the Editor owns no rendering; it edits this scene) ---
    // Mutable access to the loaded scene for outliner/inspector/commands.
    // The pointer stays valid until the next load_scene() or destruction.
    scene::Scene* edit_scene();
    const scene::Scene* scene() const;
    const std::string& loaded_scene_path() const { return m_loaded_scene_path; }
    // Persists the current scene via the standard .nfscene serializer.
    bool save_scene(const std::string& logical_path, std::string& out_error);
    // Notifies the runtime that the scene was edited externally:
    // re-propagates transforms and bumps scene_version() for viewport sync.
    void mark_scene_edited();
    uint64_t scene_version() const { return m_scene_version; }
    // Editor viewport overlays (ground grid) project through the same camera
    // the frame was rendered with; exposing the extractor keeps the overlay
    // from maintaining a second camera path that can desync.
    bool extract_camera(uint32_t target_width, uint32_t target_height, rendering::Camera& out);

    /// The camera's follow state, for the editor's viewport overlay and for
    /// the tests. `active` is false when no camera follows anything.
    const runtime::FollowCameraState& follow_camera_state() const { return m_follow_camera; }
    /// How far the characters' meshes have moved from their bind pose over
    /// the run. It is the character's version of the animated-transform
    /// deviation: a character's motion is in its mesh, not in its placement.
    float character_max_deviation() const { return m_character_max_deviation; }
    /// How many RIGGED characters the scene has and the engine loaded. Not
    /// the physics characters: those are controllers, these are the meshes
    /// with a skeleton.
    size_t rigged_character_count() const { return m_character_count; }
    bool follow_camera_active() const { return m_follow_active; }

private:
    /// Loads the characters the scene declares: a glTF with a skin and
    /// animations becomes a mesh, a skeleton, the clips and the skin data.
    ///
    /// Called after the scene is adopted and before the first frame, so a
    /// character is ready before anything draws it.
    void load_characters();

    /// Moves the characters: the speed drives the locomotion state machine,
    /// the pose becomes a skin palette, and the palette deforms the mesh.
    void step_characters(float dt);

    /// Places the follow camera for this frame: the pivot on the target, the
    /// orbit around it, the collision pull-in, and the smoothing.
    ///
    /// Called from update() so the camera moves with the same clock as
    /// everything else. A camera placed by the render path would be one frame
    /// behind the thing it follows.
    void update_follow_camera(float dt);

    /// Advances every weapon's clocks, fires the player's, and traces the
    /// shots. Runs BEFORE update_follow_camera so a shot's recoil is part of
    /// the frame the player sees, and after the characters have moved so the
    /// pivot the shot leaves from is this frame's.
    void step_weapons(float dt);

    /// Advances every projectile in flight and resolves its swept segment
    /// against the scene (Phase 46). Runs immediately after step_weapons, so a
    /// round spawned this frame already travels this frame. A projectile that
    /// hits an ally is removed without damage.
    void step_projectiles(float dt);

    /// Advances every status set and applies its ticks (Phase 54). Runs before
    /// the frame's new hits so a burn applied this frame starts next frame.
    void step_status_effects(float dt);

    /// Advances the quiet clock of every damage ledger (Phase 55).
    void step_damage_ledgers(float dt);

    /// Decays suppression and adds it for shots that passed near an agent
    /// (Phase 55). Runs after weapons/projectiles, when this frame's shots are
    /// in the shot buffer.
    void step_suppression(float dt);

    /// Records one accepted hit in the victim's ledger, creating the component
    /// lazily.
    void record_damage(ecs::Entity victim, ecs::Entity attacker, f32 health_damage,
                       f32 armor_damage, bool killed);

    /// Applies one already-traced hit: faction check, falloff, hit points and
    /// destruction. Shared by the hitscan and projectile paths so both agree
    /// about what a hit costs and when it is blocked.
    WeaponHitInfo resolve_weapon_hit(ecs::Entity shooter, const WeaponProfile& profile,
                                     const HitscanHit& hit, f32 distance_travelled);

    /// Applies a weapon's radial blast at `center` (Phase 51). Every entity in
    /// radius except `direct_victim` is considered; walls block the blast, the
    /// faction rules still apply, and each victim gets its own impact event.
    /// Returns the number of entities the blast reached.
    u32 apply_explosion(ecs::Entity shooter, const WeaponProfile& profile, const Vec3& center,
                        ecs::Entity direct_victim);

    /// Advances every combat agent's aim and trigger (Phase 45). Runs BEFORE
    /// step_weapons, which is what turns the agent's trigger into shots — one
    /// step per weapon per frame, so a cooldown cannot be charged twice.
    void step_combat_ai(float dt);

    /// The entity the active camera follows, or kInvalidEntity. The same
    /// lookup update_follow_camera does, so "the player" means one thing.
    ecs::Entity player_entity(ecs::World& world) const;

    /// Fires one volley from `weapon` (owned by `shooter`) along the camera's
    /// view, tracing every pellet in `step` and applying the hit. Returns the
    /// projectiles traced. Shared by the trigger path and by fire_weapon(), so
    /// a script and a key press cannot disagree about where a shot goes.
    u32 fire_volley(ecs::Entity shooter, WeaponComponent& weapon, const CameraComponent& cam,
                    const Vec3& pivot, const WeaponStep& step);

    /// The same trace, from an arbitrary origin and aim: the path an NPC's gun
    /// uses. Split from the camera version so "where the player looks" and
    /// "where an agent points" cannot drift apart in the hit code.
    u32 fire_volley_along(ecs::Entity shooter, WeaponComponent& weapon, const Vec3& origin,
                          f32 base_yaw, f32 base_pitch, const WeaponStep& step);

    /// Rebuilds m_hitscan_targets from the scene's colliders.
    void collect_hitscan_targets(ecs::World& world, ecs::Entity ignore);

    /// The scene's static box colliders, as world-space boxes. The camera's
    /// collision test uses the same list the navigation volume does, so the
    /// camera and the AI agree about what a wall is.
    struct CameraObstacle {
        Vec3 min{0.0f, 0.0f, 0.0f};
        Vec3 max{0.0f, 0.0f, 0.0f};
    };
    std::vector<CameraObstacle> collect_camera_obstacles(ecs::World& world) const;

    /// How far a sphere of `radius` travels from `origin` along `direction`
    /// before it hits one of the obstacles. -1 when nothing was hit.
    float camera_obstacle_hit(const std::vector<CameraObstacle>& obstacles, const Vec3& origin,
                               const Vec3& direction, float max_distance, float radius) const;

    struct SceneRuntimeData {
        std::unique_ptr<scene::Scene> scene;
    };
    assets::VirtualFileSystem& m_vfs;
    assets::AssetRegistry& m_registry;
    assets::AssetManager& m_manager;
    rhi::IGraphicsDevice& m_device;
    rhi::Swapchain* m_swapchain = nullptr;

    std::unique_ptr<SceneRuntimeData> m_scene_data_ptr;

    // Renderer3D integration (owns its own PipelineCache, RenderGraph, etc.)
    std::unique_ptr<rendering::Renderer3D> m_renderer;
    // Frame-time governor for the internal render scale (Phase 38). The scene
    // configures it; update(dt) advances it; extract_post_process() pushes its
    // scale (or the static authored scale when it is off). It is deliberately
    // owned here rather than by Renderer3D: the renderer has no frame clock.
    rendering::DynamicResolution m_dynamic_resolution;

    /// The window policy's state and config. Active only when a scene sets
    /// `frame_policy=true`; the governor is skipped then, so the two can never
    /// fight over the same scale.
    rendering::FramePolicyState m_frame_policy_state;
    rendering::FramePolicyConfig m_frame_policy_config;
    rendering::FramePacingStats m_frame_window;
    rendering::FramePolicyReason m_frame_policy_reason =
        rendering::FramePolicyReason::HoldNoSamples;
    bool m_frame_policy_active = false;
    // Built lazily on the first GPU pick, so a run that never picks never pays
    // for the id pass. Null when unavailable — callers fall back to CPU picking.
    std::unique_ptr<rendering::GpuPicker> m_picker;
    std::unique_ptr<physics::PhysicsWorld> m_physics;

    /// The follow camera's state: where it is, where it looks, and whether
    /// it has been placed at all. `m_follow_active` says whether any camera
    /// is following anything right now; without it a camera that stopped
    /// following would keep its last position forever.
    runtime::FollowCameraState m_follow_camera;
    bool m_follow_active = false;

    /// How far into the sights the camera is, 0..1. Stepped every frame from
    /// the `aim` action (PlayerLook.hpp's aim_blend_step), so releasing the
    /// button eases back out instead of snapping. Lives here rather than on
    /// the component because it is a per-frame animation, not scene data —
    /// a save must not record a half-raised sight.
    f32 m_ads_blend = 0.0f;

    /// Trauma-based camera shake. One instance for the whole runtime: the
    /// scene's camera trims it, and two cameras cannot shake independently
    /// because only one is active.
    rendering::CameraShake m_camera_shake;

    /// The live recoil offset, in degrees. Applied on top of the authored
    /// yaw/pitch when the camera is placed, so recovery lands back on the
    /// player's aim instead of on a number the kick overwrote.
    runtime::RecoilState m_recoil;

    // --- Combat (Phase 44) -------------------------------------------------
    //
    // Counters, not state: the weapon's own state lives in its component, and
    // these are the observables that let a test tell "the trigger was read"
    // from "a shot was traced and a target lost hit points". A single boolean
    // would let a broken hit path pass as a working trigger.
    u32 m_shots_fired_total = 0u;
    u32 m_hits_landed_total = 0u;
    f32 m_damage_dealt_total = 0.0f;
    f32 m_armor_damage_total = 0.0f;
    u32 m_explosions_total = 0u;
    u32 m_splash_hits_total = 0u;
    u32 m_status_ticks_total = 0u;
    f32 m_status_damage_total = 0.0f;
    WeaponHitInfo m_last_weapon_hit;

    /// Frame-local impact events (Phase 49): current and previous slots.
    ImpactBuffer m_impacts;

    /// Frame-local shot events (Phase 53): current and previous slots.
    ShotBuffer m_shots;

    /// Whether the reload action was down last frame. Edge detection for a
    /// button that must not restart a reload sixty times a second.
    bool m_reload_was_down = false;

    /// The scene's target boxes for this frame, rebuilt per volley rather than
    /// per pellet: a shotgun's eight pellets should not walk the scene eight
    /// times, and a box computed once is the same box for every pellet.
    std::vector<runtime::HitscanTarget> m_hitscan_targets;

    /// One round in flight (Phase 46). The profile is a snapshot taken at the
    /// muzzle: editing the weapon after firing must not change the bullet that
    /// is already in the air.
    struct ActiveProjectile {
        ProjectileBody body{};
        ecs::Entity owner{};
        WeaponProfile profile{};
    };

    /// Rounds in flight. A projectile is removed on impact, on expiry, or when
    /// the cap is exceeded (oldest first, deterministically).
    std::vector<ActiveProjectile> m_projectiles;

    /// Hard ceiling on simultaneous rounds. At 3600 rpm a three-second burst
    /// is 180 rounds, so 2048 is generous for gameplay and still a finite
    /// bound on a scene that spawns projectiles from a script every frame.
    static constexpr usize kMaxActiveProjectiles = 2048u;

    /// The player's overrides on the scene's camera taste. Defaults are all
    /// "no opinion", so a run that never loads a settings file behaves as the
    /// scene was written.
    runtime::PlayerSettings m_player_settings;
    /// Pushes the five bus volumes and the master dynamics into the live
    /// AudioScene. Separate from `set_player_settings` so the load path can
    /// call it without recursing.
    void apply_player_audio_settings();

    /// The skin palette of the character being stepped: one matrix per
    /// bone, recomputed every frame. It is scratch space, so it is a member
    /// rather than an allocation per frame.
    std::vector<Mat4> m_character_palette;
    /// The largest distance any character's vertices travelled from their bind
    /// pose over the run.
    float m_character_max_deviation = 0.0f;
    /// How many characters the scene declared and the runtime loaded.
    size_t m_character_count = 0;
    /// 1/60 s is the conventional choice: fine enough that a fast-moving body
    /// does not tunnel through a thin one, coarse enough to stay cheap.
    physics::FixedTimestep m_physics_clock{1.0f / 60.0f, 8};

    // Destruction (Phase 19). Three objects, owned in construction order: the
    // Jolt world that carries the shards, the sink that turns a DebrisSpawn into
    // a body in it, and the arithmetic world that decides what breaks. Torn
    // down in reverse, so the sink's bodies are gone before the world is.
    //
    // A second fixed clock rather than a shared one: the debris world steps
    // independently of the engine solver, and a shared accumulator would couple
    // two simulations whose budgets have nothing to do with each other.
    std::unique_ptr<physics::JoltWorld>                m_debris_world;
    std::unique_ptr<physics::JoltDebrisSink>           m_debris_sink;
    std::unique_ptr<destruction::DestructionWorld>     m_destruction;
    physics::FixedTimestep                             m_debris_clock{1.0f / 60.0f, 8};
    bool                                               m_destruction_enabled = false;

    // Vehicles (Step 11). Same dedicated-world pattern as destruction, for the
    // same reason: the deterministic solver has no VehicleConstraint, and one
    // world's bodies must never be simulated twice.
    //
    // Declaration order is the teardown contract (members destroy in reverse):
    // the world is declared FIRST so the system — whose JoltVehicles point
    // into it — dies before it does. The static bodies are plain handles, so
    // the vector may sit anywhere; it is cleared before the world on every
    // teardown path, like m_debris_statics.
    //
    // Own fixed clock, same 1/60 convention as the other two simulations: a
    // shared accumulator would couple budgets that have nothing to do with
    // each other.
    std::unique_ptr<physics::JoltWorld>                m_vehicle_world;
    std::unique_ptr<physics::VehicleSystem>            m_vehicle_system;
    std::vector<physics::JoltBody>                     m_vehicle_statics;
    /// Moving-obstacle mirrors (Step 13). One pair per scene body the engine
    /// solver simulates as Dynamic or Kinematic: the mirror is a KINEMATIC
    /// Jolt body in the vehicle world, teleported to the source body's solver
    /// pose every step. One-way coupling, stated plainly: the car bounces off
    /// the mirror, but the car can never push the source body — the source
    /// belongs to the deterministic engine solver and a second simulation of
    /// it ("push the mirror, mirror pushes back") is two bodies disagreeing,
    /// the same rule the statics comment below carries. The mirror integrates
    /// no motion of its own, so nothing here is simulated twice.
    struct VehicleObstacle {
        physics::BodyHandle source; // the engine solver's body (pose authority)
        physics::JoltBody   mirror; // kinematic shadow in the vehicle world
    };
    std::vector<VehicleObstacle>                       m_vehicle_obstacles;
    /// Replication (Step 14): wire id → car, and the last ACCEPTED tick per
    /// car (freshness gate, mirror of VehicleGhostClient's correction gate).
    /// Both die with the bind — a session's ids and a remote authority's tick
    /// stream have no meaning across a scene remap.
    std::unordered_map<ecs::Entity, u32>               m_vehicle_net_ids;
    std::unordered_map<ecs::Entity, u32>               m_vehicle_net_ticks;
    physics::FixedTimestep                             m_vehicle_clock{1.0f / 60.0f, 8};
    /// Integrated wheel-spin angle (radians) per vehicle and wheel. Jolt exposes
    /// the wheels' angular VELOCITY (JoltWheelState) but not an accumulated
    /// angle, so the visual roll is integrated on the runtime side — with the
    /// fixed-step h each step was solved at, which keeps the spin a function of
    /// simulated time, not of frame pacing. Visual-only state: like the chassis
    /// pose, nothing here is saved to a scene file, and a fresh bind resets it.
    ///
    /// The count is four because the physics rig is: two axles × two sides
    /// (JoltWorld's vehicle creation order [FL, FR, RL, RR]) — not a per-model
    /// axle table. A rig change would have to change this constant with it.
    struct WheelSpin {
        std::array<f32, 4> angle{};
    };
    std::unordered_map<ecs::Entity, WheelSpin>          m_vehicle_wheel_spin;
    /// One shared unit-cube wheel mesh, scaled per wheel by the vehicle's own
    /// radius/width in the pose matrix — the same flattened-cube tire the
    /// VehicleDemo draws. Built lazily on the first wheel frame (like
    /// shard_mesh) so headless runs simply render nothing rather than fail.
    rendering::StaticMeshHandle                        m_wheel_mesh;
    /// Emits the render objects for the bound vehicles' wheels, posed from
    /// `vehicle_wheel_states` like the demo poses its wheel entities. Called
    /// from build_render_world so wheels, chassis and debris are all drawn
    /// from the same world the renderer sees. Render-only, exactly like the
    /// shard path: no ECS entities, so the outliner, the PlayMode snapshot and
    /// the scene serializer never see wheels — and a save never gains them.
    void build_vehicle_wheels_render_world(rendering::RenderWorld& out);
    /// The combat feedback objects and decals for this frame (Phase 56).
    /// Render-only: ids are 0, so picking never reports a flash as an entity.
    /// A no-op unless the scene carries an enabled FeedbackComponent and the
    /// director has something alive.
    void build_combat_feedback_render_world(rendering::RenderWorld& out);
    /// Lazily creates the shared unit cube and the three emissive/scorch
    /// material instances. A headless runtime has no device: the mesh upload
    /// fails, the handles stay invalid, and feedback simply does not draw.
    void ensure_feedback_resources();
    /// Cooked assets, owned by the runtime (the "asset table" a
    /// DestructibleComponent's asset_index indexes).
    std::vector<destruction::FractureAsset>            m_fracture_assets;
    /// One entity's live breakable state. Keyed by entity rather than stored in
    /// a component so the runtime owns the destruction state the way it owns the
    /// physics world — a scene file carries no live damage either way.
    struct DestructibleBinding {
        u32                            asset_index = destruction::kInvalidChunk;
        destruction::DestructibleComponent component;
    };
    std::unordered_map<ecs::Entity, DestructibleBinding> m_destructibles;
    /// Static bodies added to the debris world so shards collide with the level.
    std::vector<physics::JoltBody>                     m_debris_statics;
    /// Which asset each live shard was carved out of. The sink carries the
    /// chunk id (the geometry to draw) but not the asset, because NFDestruction
    /// never learns about the asset table — the runtime owns that. This map is
    /// filled by differencing the sink's live ids across one apply_damage call,
    /// which is the only path that creates shards.
    std::map<u32, u32>                                  m_debris_asset;
    /// One uploaded mesh per (asset, chunk) pair, built on first use from the
    /// chunk's own vertices. A shard's geometry is shared by every blast that
    /// ever breaks that piece, so it is cached rather than rebuilt, and the
    /// key is stable across scenes because the asset table is.
    std::map<std::pair<u32, u32>, rendering::StaticMeshHandle> m_shard_meshes;
    /// Drops bindings and retires every shard; called on scene change so debris
    /// from the previous scene cannot outlive it.
    void reset_destruction_state();
    /// Re-adds the scene's static colliders to the debris world. Called on
    /// enable_destruction() and after a streaming resync, because a merged
    /// chunk's floor belongs in the debris world too.
    void rebuild_debris_statics();
    /// Uploads the mesh for `chunk` of `asset`, caching it. Returns an invalid
    /// handle when the piece has no geometry or the upload failed, in which
    /// case the shard is simply not drawn this frame.
    rendering::StaticMeshHandle shard_mesh(u32 asset_index, u32 chunk_id);
    /// Emits the render objects for live shards. Called from build_render_world
    /// so a shard drawn this frame is posed from the same state the renderer
    /// sees for every scene object.
    void build_debris_render_world(rendering::RenderWorld& out);
    /// Cooks a fracture asset for every enabled DestructibleComponent in the
    /// scene and binds it. Called from adopt_scene, so a scene that declares a
    /// breakable object is breakable the moment it opens — no game code has to
    /// remember to turn the feature on. Enables destruction on the first
    /// bindable object it finds rather than unconditionally: a scene with
    /// nothing breakable must not spin up a second physics world.
    void bind_scene_destructibles();
    /// Turns last step's contacts into damage. Runs straight after step_physics
    /// so the manifolds are fresh, and before step_destruction so a hit landed
    /// this frame spawns its shards this frame. A resting contact delivers only
    /// weight·dt per step; DestructibleComponent::damage_threshold is what
    /// separates that from a slam.
    void step_impact_damage();
    /// Retires an entity that has come apart: pulls its engine body before the
    /// components go (a static body left behind is an invisible box the rest of
    /// the level keeps colliding with), drops the binding, and rebuilds the
    /// debris statics so the shards do not land on the box they were carved
    /// out of.
    void retire_shattered_entity(ecs::Entity e);

    // World streaming (off unless enable_streaming ran). m_streaming_known is
    // the loaded-chunk count as of the last resync: any change means the
    // world membership moved, so meshes get kicked and physics rebuilt.
    WorldStreamer m_streamer;
    bool m_streaming_enabled = false;
    std::string m_streaming_chunk_dir;
    size_t m_streaming_known_loaded = 0;
    void resync_after_streaming();
    bool streaming_load_chunk(const scene::ChunkCoord& coord, std::vector<ecs::Entity>& out_created,
                              std::string& out_error);
    void streaming_unload_chunk(const scene::ChunkCoord& coord,
                                const std::vector<ecs::Entity>& created);
    // One chunk staged on a worker: read + parsed, waiting for a main-thread
    // commit. Owned jointly by the map entry and the worker lambda, so an
    // in-flight job can never dangle even if streaming is disabled or the
    // Runtime is destroyed first (the worker touches only this block and the
    // physical path it was given — never the VFS, the live world, or `this`).
    struct StreamLoadJob {
        std::mutex mutex;
        bool finished = false;
        bool ok = false;
        std::string error;
        std::shared_ptr<scene::Scene> scene;
    };
    std::unordered_map<scene::ChunkCoord, std::shared_ptr<StreamLoadJob>> m_stream_jobs;
    size_t m_max_stream_in_flight = 2;
    // True when the coordinator still wants the chunk (a job that finished
    // after its chunk left the wanted set is discarded, never committed).
    bool stream_coord_still_wanted(const scene::ChunkCoord& coord) const;

    // Gameplay modules (Phase 10). Sorted by (priority, name) at init so the
    // per-frame loop is a straight walk.
    //
    // `initialized` is tracked per module rather than as a single flag for the
    // whole set. A module added after the first scene load still has to receive
    // on_init, and a set-wide flag would silently skip it — which is exactly the
    // "registered but never driven" failure this phase exists to avoid.
    struct GameplaySlot {
        std::unique_ptr<gameplay::GameplayModule> module;
        bool initialized = false;
    };
    std::vector<GameplaySlot> m_gameplay_modules;
    gameplay::IInputSource* m_input_source = nullptr;

    // A recorder installs itself over m_input_source and forwards every query,
    // so a module reading input goes on reading it while capture is on. The
    // replay replaces m_input_source outright: nothing forwards, the log is the
    // device. The two are mutually exclusive by construction — installing a
    // replay while recording would record the log back into itself.
    std::unique_ptr<InputRecorder> m_input_recorder;
    std::unique_ptr<InputReplay>   m_input_replay;
    // Owned here so the recorder's reference stays valid for the whole capture:
    // a caller's local would dangle the moment begin_input_capture() returns.
    InputLog m_capture_log;
    // The source to restore when capture or replay ends. Both fields exist
    // because a replay can be started over an active capture and must still
    // unwind to the original device.
    gameplay::IInputSource* m_input_prior = nullptr;

    u64 m_gameplay_frame = 0;
    size_t m_gameplay_updates = 0;
    // Lua scripts (Phase 15 engine, Phase 24 frame wiring). One VM for the
    // whole scene, matching ScriptSystem's own design (per-source sandboxed
    // environments inside, not one VM per entity).
    std::unique_ptr<scripting::ScriptSystem> m_scripts;
    size_t m_script_updates = 0;
    // Particles / cloth / characters (Phase 25 wiring). One live simulation
    // object per enabled scene component, built on scene adopt and stepped
    // every frame; the scene persists configs, never live state. Characters
    // additionally need the physics world, so their controllers are (re)built
    // inside rebuild_physics_from_scene rather than on adopt.
    std::unordered_map<ecs::Entity, vfx::ParticleSystem> m_particles;
    std::unordered_map<ecs::Entity, std::unique_ptr<physics::Cloth>> m_cloths;
    std::unordered_map<ecs::Entity, std::unique_ptr<physics::CharacterController>> m_characters;
    // The scene's music and ambience samples, OWNED BY THE RUNTIME rather than
    // pointed at inside the ECS world. `MusicSystem` holds the buffer pointer
    // across frames, and the editor creates entities between them — a world
    // reallocation would leave it pointing at freed memory. One buffer each
    // because a level has one theme and one bed.
    audio::AudioBuffer m_scene_music_buffer;
    audio::AudioBuffer m_scene_ambience_buffer;
    // World-scale AI budget (Phase 25): stepped every frame via plan(). Games
    // register actors and foci through ai_world(); the population is cleared
    // on scene adopt so a previous scene's crowd never governs the next one.
    std::unique_ptr<ai::AIWorld> m_ai_world;
    // Navigation (Phase 28). The mesh is a per-scene artefact like the particle
    // emitters: rebuilt on adopt, dropped with the scene that made it. Agents
    // carry their live path beside the component rather than inside it, because
    // the path is session state — the scene persists the goal, never the route.
    struct NavAgentState {
        std::vector<Vec3> path;
        usize waypoint = 0;         ///< Index into `path` the agent is walking to.
        Vec3 goal{};                ///< The goal this path was found for.
        bool has_path = false;
        bool at_goal = false;
        u64 mesh_generation = 0;    ///< Bake this path was planned against.
        // Patrol routes (Phase 64). The authored points are scene data; where
        // the walker is on them is session state, beside the path for the same
        // reason. `route_point` is the point being walked to or waited at.
        bool on_route = false;
        usize route_point = 0;
        i32 route_direction = 1;    ///< +1 forward, -1 backward (ping-pong).
        f32 route_wait_remaining = 0.0f;
        bool route_finished = false; ///< Non-looping route reached its last point.
        // Perception-gated patrol (Phase 65). `route_held` is this frame's
        // answer; `route_hold_remaining` is the post-sighting hysteresis
        // (`resume=`) still owed before walking continues. Both session state.
        bool route_held = false;
        f32 route_hold_remaining = 0.0f;
        // Last-known-position investigation (Phase 66). A sighting leaves a
        // point to search; the walker leaves the route, scans there for
        // `investigate=` seconds, then continues. `advance_after` remembers
        // that the hold interrupted a waypoint pause, so the search replaces
        // that pause instead of walking back to a point already visited.
        bool route_investigating = false;
        bool route_investigate_pending = false;
        bool route_investigate_advance_after = false;
        bool route_investigate_arrived = false;
        Vec3 route_investigate_point{};
        f32 route_investigate_remaining = 0.0f;
        // Squad-shared investigation (Phase 67). `search_id` names the search
        // this walker is running — mates that answer one broadcast share its
        // id. Only a search born from this walker's own senses is broadcast;
        // `shared` marks the ones it answered. `answered_id` is the last
        // broadcast it consumed, so finishing a scan does not make it re-join
        // a call that is still running beside it.
        u64 route_investigate_search_id = 0;
        u64 route_investigate_answered_id = 0;
        bool route_investigate_shared = false;
        bool route_investigate_pending_shared = false;
        /// Phase 68: the pending search was armed by this walker's OWN senses,
        /// not by a radio report. Only a personal sighting opens a new call; a
        /// radio-born search runs silently (search_id stays 0).
        bool route_investigate_pending_personal = false;
        // The route shape the live cursor belongs to. A shape change restarts
        // the patrol at the first point instead of indexing a point that moved.
        usize route_point_count = 0;
        bool route_loop = false;
        bool route_ping_pong = false;
    };
    std::unordered_map<ecs::Entity, NavAgentState> m_nav_agents;
    /// One live search call per investigating squad member, rebuilt at the top
    /// of every navigation step from the PREVIOUS frame's state. A mate reads
    /// it with a one-frame latency, which is invisible next to a walk.
    struct SquadSearchBroadcast {
        std::string squad;
        Vec3 position{};
        f32 remaining = 0.0f;
        u64 id = 0;
    };
    std::vector<SquadSearchBroadcast> m_squad_searches;
    /// Monotonic per-scene serial. A new own search gets a new id, so a mate
    /// that answered an old call can answer a later one for the same squad.
    u64 m_squad_search_serial = 0;
    std::unique_ptr<ai::NavMesh> m_navmesh;
    /// Bumped on every bake, so a rebuilt mesh invalidates every agent's path
    /// without the agents knowing anything about the bake.
    u64 m_nav_generation = 0;
    size_t m_nav_obstacle_count = 0;
    /// Terrain (Phase 63): the meshes built for the scene's `Terrain:` entities,
    /// keyed by entity id. Render-only state — the height source itself is
    /// rebuilt on demand by `terrain_state_for`, so a scene edit never has to
    /// invalidate a cache it cannot see.
    std::unordered_map<u32, rendering::StaticMeshHandle> m_terrain_meshes;
    std::unordered_map<u32, rendering::MaterialHandle> m_terrain_materials;
    /// Edit-vs-play mode, mirrored into GameplayContext::playing (see
    /// set_playing). Defaults to editing so a harness that never sets it gets
    /// the editor-safe behaviour, not gameplay driving the scene.
    bool m_playing = false;
    void sort_gameplay_modules();
    /// Fills a context from the current scene/physics/audio state.
    gameplay::GameplayContext build_gameplay_context(f32 dt);
    /// Entity that owns the GameplayModuleComponent for `name`, or an invalid
    /// Entity when the scene has none.
    ecs::Entity find_gameplay_owner(std::string_view name) const;
    /// Module name -> enabled, read from the scene's components in one pass so
    /// the step loop does not re-query the world per module.
    std::unordered_map<std::string, bool> collect_gameplay_enabled() const;

    // Animation (Phase 9). `m_anim_clip_table` is a scratch view the state
    // machine needs (name -> clip*); rebuilt per entity because the state
    // machine takes the table by reference, not a lookup.
    size_t m_animated_entities = 0;
    std::unordered_map<std::string, const animation::AnimationClip*> m_anim_clip_table;

    // Audio (Phase 9). The device is a NullAudioDevice until a real backend is
    // installed behind the seam, which keeps headless runs and CI silent but
    // still exercises the whole mixing path.
    std::unique_ptr<audio::AudioDevice> m_audio_device;
    /// The one mixer. Owns the bus tree, the Settings volumes, the music and
    /// ambience layers and the reverb tail — everything a shipped level needs.
    audio::AudioScene m_audio_scene;
    /// Live emitters, one per playing source, kept between frames so the
    /// playback cursors and the occlusion filter's state survive the frame.
    ///
    /// The state cannot live in a stack temporary inside step_audio: the filter
    /// is what makes a wall's muffle continuous, and a per-frame reset would
    /// click on every block boundary. Keyed by entity, so a source that is
    /// removed from the scene drops out on the next rebuild.
    std::unordered_map<ecs::Entity, audio::Emitter> m_audio_emitters;
    /// Entities seen in the last step_audio(). Compared against the emitter map
    /// each block to retire sources whose entity no longer carries audio,
    /// instead of leaking one entry per despawned emitter.
    std::vector<ecs::Entity> m_audio_emitter_seen;
    audio::AudioListener m_audio_listener;
    std::vector<f32> m_audio_left;
    std::vector<f32> m_audio_right;
    /// Seconds owed to the mixer. The device runs in fixed-size blocks while
    /// frames arrive at arbitrary deltas, so the remainder carries over; without
    /// it the mixed block length would track the frame time and a fast machine
    /// would silently mix less audio per second than a slow one.
    f32 m_audio_accumulator = 0.0f;
    size_t m_audio_sources_mixed = 0;
    f32 m_audio_output_peak = 0.0f;

    // --- Combat feedback (Phase 56) ----------------------------------------
    //
    // The director is pure arithmetic. This block is the bridge from its lists
    // to the two things that own resources: RenderWorld (one shared cube and
    // three materials) and AudioScene (one Emitter per live cue). Nothing here
    // runs for a scene whose `Feedback:` line is absent.
    runtime::CombatFeedback m_feedback;
    bool m_feedback_bound = false;
    rendering::StaticMeshHandle m_feedback_mesh{};
    rendering::MaterialHandle m_feedback_flash_material{};
    rendering::MaterialHandle m_feedback_tracer_material{};
    rendering::MaterialHandle m_feedback_impact_material{};
    /// Cues the director produced but no mix block has consumed yet. step_audio
    /// can return 0 blocks for a short frame while the director clears its own
    /// list on the next update; holding them here is what makes a shot heard
    /// when several frames fit inside one fixed-size audio block.
    std::vector<runtime::FeedbackSound> m_feedback_pending;
    /// One live voice per started cue. The Emitter owns the playback cursor,
    /// so it must survive across blocks; a non-looping source stops itself when
    /// the buffer ends and is retired on the next mix.
    std::vector<audio::Emitter> m_feedback_voices;
    /// Procedural one-shots, built once the device rate is known. The voice
    /// emitters point into these, so the buffers must outlive them and never be
    /// rebuilt while a voice is live. Phase 59 added one impact buffer per
    /// damage channel, indexed by `DamageType` (Ballistic/Explosive/Fire).
    audio::AudioBuffer m_feedback_shot_buffer;
    audio::AudioBuffer m_feedback_impact_buffers[3];
    u32 m_feedback_buffer_rate = 0;

    // --- Perception state (Phase 60) -----------------------------------------
    //
    // One live sensor per entity that authored a `Perception:` line. The
    // component is authored data and round-trips through the file; the memory
    // below is session state, keyed to the scene and dropped with it, exactly
    // like a baked navmesh or a cloth piece.
    struct PerceptionBinding {
        ecs::Entity entity{};
        ai::Perception perception{};
        /// This sensor's own cast: the shared cast MINUS the sensor itself,
        /// plus the pain addressed to it. Built per frame, and the reason ids
        /// stay stable is that the entity order does: the same actors are added
        /// in the same order every frame, and the one entity that is dropped is
        /// always this sensor.
        ///
        /// A sensor is deliberately not in its own cast. `sight_strength` has no
        /// "skip self" rule, and at zero distance it reports a full-strength
        /// stimulus — a guard would spend half a second detecting itself and
        /// `most_threatening()` would answer "me".
        ai::PerceptionScene cast;

        /// A contact this sensor personally sensed last step, with the last
        /// position it held, tracked so the step the contact falls out of the
        /// senses can call it out once (Phase 69). Keyed on the ENTITY, not the
        /// cast id: the shared cast is rebuilt every frame, and destroying an
        /// actor in front of the target renumbers everyone behind it. The vector
        /// is replaced each step, not searched: an AI cast is tens of actors,
        /// and linear scans keep the update order deterministic.
        struct SquadContact {
            ecs::Entity target{};
            Vec3 last_position{};
        };
        std::vector<SquadContact> squad_contacts;
    };
    std::vector<PerceptionBinding> m_perceptions;

    /// One lost-contact callout waiting for the next radio exchange (Phase 69).
    /// Queued at the end of the step that noticed the loss and delivered at the
    /// start of the next one, exactly like a live report: same one-frame
    /// latency, same knowledge-only contract. Delivered once, then dropped.
    struct SquadLostCallout {
        ecs::Entity reporter{};
        ecs::Entity target{};
        Vec3 position{};
    };
    std::vector<SquadLostCallout> m_perception_lost_callouts;

    /// Phase 69: books the contacts this sensor PERSONALLY sensed this step and
    /// queues exactly one callout for each contact that fell out of its senses.
    /// Radio-born memories never enter this bookkeeping, so a report can never
    /// spawn a callout and the anti-relay rule keeps its teeth.
    void track_squad_contacts(PerceptionBinding& binding);

    /// The cast for THIS frame. Rebuilt every frame so the target ids a sensor's
    /// memory is keyed on stay stable: `PerceptionScene::add_target` hands out
    /// 1, 2, 3... in insertion order, and the insertion order is
    /// `all_entities()`, which is ECS insertion order. Reusing one scene and
    /// clearing it would NOT work — `clear()` deliberately keeps the id counter,
    /// so every frame would issue fresh ids and no sensor would ever recognise
    /// the actor it has been watching.
    ai::PerceptionScene m_perception_scene;
    /// Parallel to `m_perception_scene.targets()`: index i is the entity whose
    /// scene id is i+1. What turns an entity into the id a sensor remembers.
    std::vector<ecs::Entity> m_perception_target_entities;

    /// A noise still audible this frame. A gunshot is a *moment*, but a stimulus
    /// that existed for one frame would deliver one tick of detection
    /// (`gain * stim * dt`, 0.03 s at 60 Hz) and no NPC could ever act on it, so
    /// the noise is held for a lifetime and re-emitted while it lasts — the same
    /// job `PerceptionScene::age` does for a scene that is not rebuilt.
    struct PerceptionNoise {
        Vec3 position{};
        f32 strength = 1.0f;
        u32 source_target = 0; // 0 = unattributed (nobody to blame)
        f32 remaining = 0.0f;
    };
    std::vector<PerceptionNoise> m_perception_noises;

    /// One frame's worth of pain, addressed to a single victim. The damage sense
    /// is neither attenuated nor gradual, so a shared stimulus would make every
    /// sensor in the level aware of whoever punched one of them — the victim's
    /// own copy is the only honest place for it.
    struct PerceptionPain {
        ecs::Entity victim{};
        u32 source_target = 0;
        Vec3 position{};
    };
    std::vector<PerceptionPain> m_perception_pain;

    /// World boxes for the sight ray. Deliberately NOT the weapon's
    /// `m_hitscan_targets`: perception runs before the combat step, and filling
    /// the weapon's list here would leave that step testing a stale cast.
    std::vector<runtime::HitscanTarget> m_perception_sight_targets;

    // --- Decision state (Phase 61) -------------------------------------------
    //
    // One live utility system per entity with a `Brain:` line, plus the memory of
    // what it decided. The considerations are authored data and round-trip
    // through the file; the score, the inputs and the winner are this frame's.
    struct BrainBinding {
        ecs::Entity entity{};
        ai::UtilitySystem system;
        ai::Blackboard board;
        /// Action id is insertion order starting at 1, so this is indexed by id-1.
        /// It is how a winning id becomes the name the game reads.
        std::vector<std::string> action_names;
        std::vector<bool> action_holds_fire;
        /// Seconds since the last evaluation; the brain re-decides when it
        /// reaches the authored rate.
        f32 clock = 0.0f;
        std::string decision_name;
        f32 decision_score = 0.0f;
        u32 decision_duration = 0;
        /// The input values as the frame computed them, indexed by BrainInput.
        std::array<f32, kBrainInputCount> inputs{};        bool holds_fire = false;
    };
    std::vector<BrainBinding> m_brains;

    /// One live machine per entity with a `States:` line. Parallel to the brain
    /// binding in shape, and deliberately separate: an entity may carry either,
    /// and the two are different questions ("what do I want" and "what am I
    /// doing"), not two implementations of one.
    struct MachineBinding {
        ecs::Entity entity{};
        /// Held by pointer because `ai::StateMachine` owns its states through
        /// `unique_ptr` and is therefore not copyable; a binding that had to be
        /// movable would otherwise have to copy one.
        std::unique_ptr<ai::StateMachine> machine;
        ai::Blackboard board;
        /// State id -> name, and state id -> effect. Ids are insertion order.
        std::vector<std::string> state_names;
        std::vector<bool> state_holds_fire;
        f32 clock = 0.0f;
        std::string current_name;
        u32 transitions_fired = 0;
        bool holds_fire = false;
    };
    std::vector<MachineBinding> m_machines;
    std::unique_ptr<rendering::MeshLibrary> m_mesh_library;
    std::unique_ptr<rendering::MaterialLibrary> m_material_library;
    std::unique_ptr<rendering::PipelineCache> m_pipeline_cache;
    bool m_renderer_initialized = false;
    // Requested developer profiling state. Kept here, not only in Renderer3D,
    // because scene loads call shutdown() and init() again; applying this after
    // every successful init is what keeps the user's request from silently
    // disappearing with the old renderer resources.
    bool m_gpu_profiling_enabled = false;
    // Phase 40: a scene that asks for GPU-aware dynamic resolution needs frame
    // timestamps even when the developer did not request profiling. This is a
    // second reason to keep frame timing on; it never enables the per-pass
    // breakdown, which is the toggle that serialises the GPU.
    bool m_gpu_frame_timing_required = false;
    // Last Renderer3D::Stats::gpu_sample_serial consumed by the governor. A
    // repeated stats read must fall back to CPU rather than count the same GPU
    // frame twice when update() runs more often than render().
    u64 m_last_gpu_sample_serial = 0;
    // One warning per renderer lifetime for "asked for timestamps, device has
    // none"; neither path should spam the log every frame.
    bool m_gpu_timing_warning_shown = false;
    std::unordered_map<assets::AssetId, rendering::StaticMeshHandle> m_mesh_handles;
    // Mesh ids whose load already failed and was reported. A Failed handle is
    // terminal in AssetManager, so without this the sync loop re-queried and
    // re-warned for the same dead asset on every pass — twice per frame, forever
    // (a scene with one uncooked mesh produced 120 identical warnings over 60
    // frames). Erased on hot reload and on scene load so a re-cooked asset is
    // picked up again.
    std::unordered_set<assets::AssetId> m_failed_meshes;
    // Total failure reports emitted. Kept separate from the set above because
    // re-inserting an existing id does not change the set's size, so the set
    // alone cannot distinguish "reported once" from "reported every frame".
    size_t m_failed_mesh_reports = 0;
    rendering::MaterialHandle m_default_material{};
    std::string m_loaded_scene_path;
    uint64_t m_scene_version = 0;

    // Scratch render world reused across frames: build_render_world() clears
    // and refills it, so a 5000-object scene stops allocating 5000 RenderObjects
    // through the heap on every frame. render_world_snapshot()/picking still
    // build into locals — they are not per-frame paths.
    rendering::RenderWorld m_render_world;

    // Path -> renderer instance (includes gray fallbacks for missing files, so
    // file IO happens at most once per path; Hot Reload drops entries).
    std::unordered_map<std::string, rendering::MaterialHandle> m_material_instances;
    std::unordered_map<std::string, bool> m_material_dirty;
    // Material path -> albedo texture path ("" = scalar only).
    std::unordered_map<std::string, std::string> m_material_albedo;
    // Material path -> PBR map paths ("" = scalar only). Same contract as the
    // albedo map above; all four ride the same sampler/mip mode.
    std::unordered_map<std::string, std::string> m_material_normal;
    std::unordered_map<std::string, std::string> m_material_mrough;
    std::unordered_map<std::string, std::string> m_material_occlusion;
    std::unordered_map<std::string, std::string> m_material_emissive_map;
    // Material path -> sampler mip mode (Linear when the file is silent).
    std::unordered_map<std::string, rhi::MipMapMode> m_material_mip;

    // Owned GPU texture + full-mip view + one sampler per mip mode (at most
    // three, created lazily). Destruction order matters (samplers/views before
    // texture); reverse destruction frees samplers, then the view, then the
    // texture. All destroyed after wait_idle.
    struct TextureObjects {
        std::unique_ptr<rhi::Texture> texture;
        std::unique_ptr<rhi::TextureView> view;
        u32 mip_levels = 1;
        // Indexed by static_cast<int>(rhi::MipMapMode): None=0, Nearest=1, Linear=2.
        std::unique_ptr<rhi::Sampler> samplers[3];
    };
    std::unordered_map<std::string, std::unique_ptr<TextureObjects>> m_textures;
    // The colour-grading LUT the scene currently names, and the VIEW into the
    // cached texture above. The path is what makes the resolve once-per-change;
    // the view is what the renderer binds.
    std::string m_lut_path;
    const rhi::TextureView* m_lut_view = nullptr;
    // Sampler for (texture entry, mode), creating and caching on demand.
    // max_lod follows the texture's real chain (or 0 when mode is None).
    rhi::Sampler* sampler_for_mode(TextureObjects& entry, rhi::MipMapMode mode);
    // (Re)binds a material instance to its recorded maps+mode, if any.
    // Returns false only when the renderer is down.
    bool rebind_material_sampling(const std::string& material_path, rendering::MaterialHandle handle,
                                  std::string& out_error);
    // Shared body of the four set_material_* map setters (normal, mrough,
    // occlusion, emissive_map): record-then-rebind, mirroring set_material_albedo.
    bool set_material_map(const std::string& material_path, const std::string& texture_path,
                          std::unordered_map<std::string, std::string>& slot_map,
                          void (rendering::MaterialLibrary::*clear)(rendering::MaterialHandle),
                          std::string& out_error);

    static std::string normalize_material_path(const std::string& p);
    static rendering::PBRMaterialParams default_material_params();
    bool load_material_file(const std::string& path, rendering::PBRMaterialParams& out) const;
    std::unique_ptr<TextureObjects> upload_texture_objects(const std::string& logical_path,
                                                           const rendering::DecodedImage& img,
                                                           std::string& out_error);

    // Fallback clear resources: kept alive until shutdown() so the command
    // buffer that references them stays valid past cmd->end()/submit.
    // Only used when Renderer3D fails; the success path uses no transients.
    std::vector<std::unique_ptr<rhi::RenderPass>> m_fallback_rps;
    std::vector<std::unique_ptr<rhi::Framebuffer>> m_fallback_fbs;

    bool ensure_renderer_initialized();
    bool ensure_renderer_initialized_for(uint32_t width, uint32_t height);
    /// Applies the two renderer toggles from the stored developer request and
    /// the scene's dynamic-resolution timing request. Frame timestamps are on
    /// when EITHER asks for them; the per-pass breakdown needs the developer
    /// switch, so GPU-aware dynamic resolution never pays the per-pass drains.
    void apply_gpu_timing_state();
    /// Locates the compiled renderer shaders (also used by the GPU picker).
    std::filesystem::path resolve_shader_dir() const;
    void sync_meshes_from_assets();
    void ensure_default_material();
    void extract_light();
    /// Advance the scene's TimeOfDay clock by `dt` and write the new hour back
    /// into the component. A scene with no component, or one with `enabled=false`
    /// or `day_length_seconds <= 0` (frozen), leaves the authored state alone.
    void advance_time_of_day(f32 dt);
    /// The scene's enabled TimeOfDay, or nullptr. Shared by extract_light and
    /// extract_sky so the sun and the sky can never be driven by two different
    /// hours.
    static const TimeOfDayComponent* find_time_of_day(const ecs::World& world);
    /// The mutable twin of the above, for the one caller that advances the clock.
    static TimeOfDayComponent* find_time_of_day_mutable(ecs::World& world);
    /// Pushes the scene's point/spot lights into the renderer (Phase 26).
    /// Rebuilt every frame, like the directional light: a lamp destroyed by
    /// gameplay has to stop lighting the scene this frame, and the renderer's
    /// lights are an array indexed by slot, which cannot express "removed".
    void extract_local_lights();
    /// Adds the director's transient muzzle/blast lights (Phase 59) on top of
    /// the scene's own point lights. Called immediately after
    /// extract_local_lights in every render path: the scene fills the slots and
    /// the flash takes whatever is left, so a level that already uses all eight
    /// keeps its authored lighting and the flash is simply not lit.
    void apply_combat_feedback_lights();
    void extract_sky();
    /// Pushes the scene's post-processing block (design §206) into the renderer.
    ///
    /// Separate from extract_sky even though both walk the world once per frame:
    /// a scene may want a glow without a procedural sky (an interior), and a
    /// scene may want a sky with no post at all. Folding them together would make
    /// the sky's presence a precondition for the post stack, which is a coupling
    /// nothing in the design asks for.
    ///
    /// With no component in the scene the renderer keeps its own defaults, so a
    /// level authored before this existed renders identically.
    void extract_post_process();
    void build_render_world(rendering::RenderWorld& out);
    void fallback_clear(rhi::Texture& target, rhi::CommandBuffer& cmd, bool present_source);

    // One-shot latch for the "the scene asked for more lights than the frame can
    // draw" warning. It re-arms on scene load, because the next scene is a
    // different question.
    bool m_local_light_warning_shown = false;
};

} // namespace nf::runtime
