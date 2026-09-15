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
#include <NF/Animation/Components.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Gameplay/GameplayState.hpp>
#include <NF/Runtime/WorldStreamer.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>

#include <filesystem>
#include <memory>
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
    u64 m_gameplay_frame = 0;
    size_t m_gameplay_updates = 0;
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
    bool extract_camera(uint32_t target_width, uint32_t target_height, rendering::Camera& out);
    void extract_light();
    void build_render_world(rendering::RenderWorld& out);
    void fallback_clear(rhi::Texture& target, rhi::CommandBuffer& cmd, bool present_source);
};

} // namespace nf::runtime
