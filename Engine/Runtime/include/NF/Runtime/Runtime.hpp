#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/GpuPicker.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Physics/FixedTimestep.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/JoltDebrisSink.hpp>
#include <NF/Destruction/DestructionWorld.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Gameplay/GameplayState.hpp>
#include <NF/Runtime/InputLog.hpp>
#include <NF/Runtime/WorldStreamer.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>

#include <filesystem>
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
    void render(uint32_t image_index, rhi::CommandBuffer& cmd);

    // For headless/offscreen rendering (tests).
    // Records the full Renderer3D pipeline into `cmd` targeting `target`
    // (no swapchain, no PRESENT layout). On failure records fallback clear.
    void render_offscreen(rhi::Texture& target, rhi::CommandBuffer& cmd);

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
    // playing source through the master bus.
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

    /// The input source handed to modules. Non-owning; null means no input, and
    /// modules are expected to check.
    void set_input_source(gameplay::IInputSource* source) { m_input_source = source; }
    [[nodiscard]] gameplay::IInputSource* input_source() const { return m_input_source; }

    /// Whether the game is currently playing (vs. editing). Mirrored into
    /// every GameplayContext built from here: modules that own scene objects
    /// (notably the camera) must stay out while editing so editor gestures —
    /// viewport navigation writes the camera Transform every frame — are not
    /// overwritten on the next step. The editor sets this from its play state
    /// before each update; harness/tests default to editing (false).
    void set_playing(bool playing) { m_playing = playing; }
    [[nodiscard]] bool playing() const { return m_playing; }

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
    // the same path shares one renderer instance (one 48-byte UBO). Edits
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

private:
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
    // Built lazily on the first GPU pick, so a run that never picks never pays
    // for the id pass. Null when unavailable — callers fall back to CPU picking.
    std::unique_ptr<rendering::GpuPicker> m_picker;
    std::unique_ptr<physics::PhysicsWorld> m_physics;
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
    audio::AudioBus m_audio_bus;
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
    std::unique_ptr<rendering::MeshLibrary> m_mesh_library;
    std::unique_ptr<rendering::MaterialLibrary> m_material_library;
    std::unique_ptr<rendering::PipelineCache> m_pipeline_cache;
    bool m_renderer_initialized = false;
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

    // Path -> renderer instance (includes gray fallbacks for missing files, so
    // file IO happens at most once per path; Hot Reload drops entries).
    std::unordered_map<std::string, rendering::MaterialHandle> m_material_instances;
    std::unordered_map<std::string, bool> m_material_dirty;
    // Material path -> albedo texture path ("" = scalar only).
    std::unordered_map<std::string, std::string> m_material_albedo;
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
    // Sampler for (texture entry, mode), creating and caching on demand.
    // max_lod follows the texture's real chain (or 0 when mode is None).
    rhi::Sampler* sampler_for_mode(TextureObjects& entry, rhi::MipMapMode mode);
    // (Re)binds a material instance to its recorded albedo+mode, if any.
    // Returns false only when the renderer is down.
    bool rebind_material_sampling(const std::string& material_path, rendering::MaterialHandle handle,
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
    /// Locates the compiled renderer shaders (also used by the GPU picker).
    std::filesystem::path resolve_shader_dir() const;
    void sync_meshes_from_assets();
    void ensure_default_material();
    void extract_light();
    void extract_sky();
    void build_render_world(rendering::RenderWorld& out);
    void fallback_clear(rhi::Texture& target, rhi::CommandBuffer& cmd, bool present_source);
};

} // namespace nf::runtime
