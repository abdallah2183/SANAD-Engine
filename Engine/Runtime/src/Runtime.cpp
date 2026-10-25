#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/BuiltinModules.hpp>
#include <NF/Runtime/PlayerLook.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/MaterialAsset.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Profiler.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/TimeOfDay.hpp>

#include <algorithm>
#include <iterator>
#include <array>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nf::runtime {

namespace {

// Scale ratio for applying an animated pose as an offset on top of an authored
// transform. A rest scale of zero is not a meaningful rig, but it is reachable
// from a hand-edited scene, and dividing by it would poison the entity's
// transform with inf/NaN for the rest of the session. Treat it as "no change".
f32 safe_scale_ratio(f32 posed, f32 rest) {
    if (std::abs(rest) < 1e-6f) {
        return 1.0f;
    }
    return posed / rest;
}

/// Distance in the XZ plane from `p` to the segment `a`→`b`. Navigation is a
/// ground-plane problem, so a height difference never counts as "off the
/// corridor" — an agent that falls and lands is still on its path.
f32 xz_distance_to_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const f32 abx = b.x - a.x;
    const f32 abz = b.z - a.z;
    const f32 ab2 = abx * abx + abz * abz;
    if (ab2 < 1e-12f) {
        const f32 dx = p.x - a.x;
        const f32 dz = p.z - a.z;
        return std::sqrt(dx * dx + dz * dz);
    }
    f32 t = ((p.x - a.x) * abx + (p.z - a.z) * abz) / ab2;
    t = nf::clamp(t, 0.0f, 1.0f);
    const f32 dx = p.x - (a.x + abx * t);
    const f32 dz = p.z - (a.z + abz * t);
    return std::sqrt(dx * dx + dz * dz);
}

/// The fracture piece for a box collider, in the asset's own local space. The
/// collider is the only description of an object's shape a scene carries —
/// there is no fracture-asset import pipeline — so the cut starts from it.
/// NFDestruction deliberately knows nothing about physics shapes ("nothing here
/// touches physics or rendering"), so the conversion lives in this integration
/// layer. The winding matches the destruction suite's unit cube.
destruction::FracturePiece box_fracture_piece(const Vec3& half) {
    destruction::FracturePiece piece;
    piece.vertices = {
        Vec3{-half.x, -half.y, -half.z}, Vec3{ half.x, -half.y, -half.z},
        Vec3{ half.x,  half.y, -half.z}, Vec3{-half.x,  half.y, -half.z},
        Vec3{-half.x, -half.y,  half.z}, Vec3{ half.x, -half.y,  half.z},
        Vec3{ half.x,  half.y,  half.z}, Vec3{-half.x,  half.y,  half.z},
    };
    piece.indices = {
        0u, 2u, 1u,  0u, 3u, 2u,   1u, 6u, 5u,  1u, 2u, 6u,
        5u, 7u, 4u,  5u, 6u, 7u,   4u, 3u, 0u,  4u, 7u, 3u,
        4u, 0u, 1u,  4u, 1u, 5u,   3u, 6u, 2u,  3u, 7u, 6u,
    };
    return piece;
}

} // namespace

Runtime::Runtime(assets::VirtualFileSystem& vfs, assets::AssetRegistry& registry, assets::AssetManager& manager,
                 rhi::IGraphicsDevice& device, rhi::Swapchain* swapchain)
    : m_vfs(vfs), m_registry(registry), m_manager(manager), m_device(device), m_swapchain(swapchain) {
    m_mesh_library = std::make_unique<rendering::MeshLibrary>();
    m_material_library = std::make_unique<rendering::MaterialLibrary>(m_device);
    m_pipeline_cache = std::make_unique<rendering::PipelineCache>(m_device);
    m_renderer = std::make_unique<rendering::Renderer3D>();
    m_scripts = std::make_unique<scripting::ScriptSystem>();
    // Default open-field grid; games resize expectations through ai_world()
    // (set_budget/set_tier_radii) or by replacing the population per scene.
    m_ai_world = std::make_unique<ai::AIWorld>(32.0f, 8, 8);
}

Runtime::~Runtime() {
    // Best effort: free GPU resources while the device is still alive.
    // If the device was already shut down this is a no-op (guards inside).
    // The explicit shutdown() path is preferred (called before device shutdown).
    // The gameplay modules go first: a module's shutdown hook may flush state
    // or release a device resource, and a Runtime destroyed without an
    // explicit shutdown() (an exception path, a tool, a test) would otherwise
    // drop them without their hook ever running.
    shutdown_gameplay();
    disable_destruction();
    if (m_renderer_initialized && m_renderer) {
        // Do not call device.wait_idle() here: the device may already be gone.
        // Renderer3D::shutdown() itself waits when the device is alive.
        m_renderer->shutdown();
        m_renderer_initialized = false;
    }
}

void Runtime::shutdown() {
    // Gameplay first: a module may still want to write through the scene, and
    // the scene outlives the GPU resources below.
    shutdown_gameplay();
    // Debris next: its bodies live in the runtime's own Jolt world, which has
    // to go while the Runtime still owns every object that references it.
    disable_destruction();
    m_device.wait_idle();
    // The picker owns its own targets/pipeline/framebuffer; drop them while the
    // device is still alive. Rebuilt lazily if a pick happens after shutdown.
    m_picker.reset();
    if (m_renderer && m_renderer_initialized) {
        m_renderer->shutdown();
    }
    m_renderer_initialized = false;
    m_fallback_fbs.clear();
    m_fallback_rps.clear();
    m_mesh_handles.clear();
    m_failed_meshes.clear();
    m_failed_mesh_reports = 0;
    // Renderer-owned material instances (and their raw albedo view pointers)
    // die with m_renderer->shutdown() above, so freeing the textures here is
    // safe: nothing references these views anymore.
    m_material_instances.clear();
    m_material_dirty.clear();
    m_material_albedo.clear();
    m_material_normal.clear();
    m_material_mrough.clear();
    m_material_occlusion.clear();
    m_material_emissive_map.clear();
    m_textures.clear();
    // The cached texture views just died with the map, so the LUT pointer into
    // one of them must go with it.
    m_lut_path.clear();
    m_lut_view = nullptr;
    m_default_material = rendering::kInvalidMaterialHandle;
    // Feedback resources died with the renderer above (materials) and with the
    // mesh library (the cube). Drop the handles so the next render rebuilds
    // them instead of drawing through dead ones.
    m_feedback_flash_material = rendering::kInvalidMaterialHandle;
    m_feedback_tracer_material = rendering::kInvalidMaterialHandle;
    m_feedback_impact_material = rendering::kInvalidMaterialHandle;
    m_feedback_mesh = rendering::StaticMeshHandle{};
    // The voices point into the procedural buffers below and are per-scene
    // artefacts; a mix after shutdown starts from silence.
    m_feedback_voices.clear();
    m_feedback_pending.clear();
    // Free GPU-backed libraries while the device is still alive.
    // Recreate empty ones so the Runtime stays usable after shutdown.
    m_mesh_library = std::make_unique<rendering::MeshLibrary>();
    m_material_library = std::make_unique<rendering::MaterialLibrary>(m_device);
    m_pipeline_cache = std::make_unique<rendering::PipelineCache>(m_device);
    m_device.wait_idle();
}

void Runtime::on_swapchain_resized(rhi::Swapchain* swapchain) {
    m_swapchain = swapchain;
    if (m_renderer && m_renderer_initialized && m_swapchain) {
        const auto w = m_swapchain->width();
        const auto h = m_swapchain->height();
        if (w != 0 && h != 0 && (w != m_renderer->width() || h != m_renderer->height())) {
            m_renderer->resize(w, h);
            NF_LOG_INFO(LogCategory::Core, "Runtime: renderer resized to {}x{}", w, h);
        }
    }
}

void Runtime::set_gpu_profiling_enabled(bool enabled) {
    m_gpu_profiling_enabled = enabled;
    apply_gpu_timing_state();
}

void Runtime::apply_gpu_timing_state() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    const bool frame_timing = m_gpu_profiling_enabled || m_gpu_frame_timing_required;
    m_renderer->set_gpu_timing_enabled(frame_timing);
    m_renderer->set_gpu_pass_timing_enabled(m_gpu_profiling_enabled);
    if (!frame_timing) {
        m_gpu_timing_warning_shown = false;
        return;
    }
    if (!m_renderer->gpu_timing_supported() && !m_gpu_timing_warning_shown) {
        if (m_gpu_profiling_enabled) {
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: GPU profiling requested but this device/queue cannot "
                        "write timestamps - falling back to the submit-to-fence estimate");
        } else {
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: GPU-aware dynamic resolution requested but this "
                        "device/queue cannot write timestamps - falling back to CPU "
                        "frame time");
        }
        m_gpu_timing_warning_shown = true;
    }
}

bool Runtime::ensure_renderer_initialized() {
    uint32_t width = 1280;
    uint32_t height = 720;
    if (m_swapchain && m_swapchain->width() != 0 && m_swapchain->height() != 0) {
        width = m_swapchain->width();
        height = m_swapchain->height();
    }
    return ensure_renderer_initialized_for(width, height);
}

std::filesystem::path Runtime::resolve_shader_dir() const {
    // A project may declare a shaders:// mount, and that is the ONLY path that
    // works for a packaged game: everything below resolves against the engine
    // source tree or a build directory, neither of which ships. Consulted first
    // so a project always wins.
    {
        auto via_project = m_vfs.resolve("shaders://Basic3D");
        if (via_project.ok && std::filesystem::exists(via_project.value)) {
            return via_project.value;
        }
    }
    // A shipped package is self-contained and keeps Shaders/ beside the player.
    // Prefer that relative path before any compile-time build-tree path, because
    // a developer may have the original build directory on the same machine.
    const std::filesystem::path packaged("Shaders/Basic3D");
    if (std::filesystem::exists(packaged)) {
        return packaged;
    }
    // In-repo fallback: the compile-time define (set by the build), then paths
    // relative to the working directory, then content://, then the two known
    // build trees. Extracted from ensure_renderer_initialized_for so the GPU
    // picker can find the same shaders without duplicating this search.
#ifdef NF_BASIC3D_SHADER_DIR
    {
        std::filesystem::path p = NF_BASIC3D_SHADER_DIR;
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
#endif
    const std::array<std::filesystem::path, 6> candidates{
        std::filesystem::path("Samples/Basic3D/shaders"),
        std::filesystem::path("../Samples/Basic3D/shaders"),
        std::filesystem::path("Shaders/Basic3D"),
        std::filesystem::path("../Shaders/Basic3D"),
        std::filesystem::path("build/DebugNinja/Shaders/Basic3D"),
        std::filesystem::path("build/debug/Shaders/Basic3D"),
    };
    for (const auto& cand : candidates) {
        if (std::filesystem::exists(cand)) {
            return cand;
        }
    }
    auto r = m_vfs.resolve("content://Shaders/Basic3D");
    if (r.ok && std::filesystem::exists(r.value)) {
        return r.value;
    }
    std::filesystem::path fallback("build/DebugNinja/Shaders/Basic3D");
    if (!std::filesystem::exists(fallback)) {
        fallback = std::filesystem::path("build/debug/Shaders/Basic3D");
    }
    return fallback;
}

bool Runtime::ensure_renderer_initialized_for(uint32_t width, uint32_t height) {
    if (m_renderer_initialized) {
        if (m_renderer && (m_renderer->width() != width || m_renderer->height() != height)) {
            if (width != 0 && height != 0) {
                m_renderer->resize(width, height);
            }
        }
        if (m_default_material.valid()) {
            return true;
        }
        ensure_default_material();
        return m_renderer_initialized;
    }
    if (!m_renderer) {
        m_renderer = std::make_unique<rendering::Renderer3D>();
    }
    if (width == 0 || height == 0) {
        width = 1280;
        height = 720;
    }
    const std::filesystem::path shader_dir = resolve_shader_dir();
    if (!m_renderer->init(m_device, shader_dir, width, height)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: failed to init Renderer3D (shader_dir='{}')", shader_dir.string());
        return false;
    }
    m_renderer->set_mesh_library(m_mesh_library.get());
    // Sensible defaults; per-frame extraction overrides the directional light.
    m_renderer->set_directional_light(rendering::DirectionalLight{
        Vec3{-0.5f, -1.0f, -0.3f},
        Vec3{1.0f, 1.0f, 1.0f},
        1.0f,
        true,
    });
    m_renderer->set_ambient(0.2f);
    m_renderer->set_exposure(1.0f);
    m_renderer_initialized = true;
    // Re-apply the requested developer/scene state after every init: shutdown()
    // during a scene change destroys the timestamp pools, and a renderer that
    // comes back with profiling silently off is the failure this guards.
    m_gpu_timing_warning_shown = false;
    apply_gpu_timing_state();
    ensure_default_material();
    NF_LOG_INFO(LogCategory::Core, "Runtime: Renderer3D initialized ({}x{}, shader_dir='{}')", width, height,
                shader_dir.string());
    return true;
}

void Runtime::ensure_default_material() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (m_default_material.valid()) {
        return;
    }
    rendering::Material* gbuffer_mat = m_renderer->gbuffer_material();
    if (gbuffer_mat == nullptr || !gbuffer_mat->valid()) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: gbuffer material unavailable");
        return;
    }
    rendering::PBRMaterialParams params{};
    params.base_color[0] = 0.8f;
    params.base_color[1] = 0.8f;
    params.base_color[2] = 0.8f;
    params.base_color[3] = 1.0f;
    params.metallic = 0.0f;
    params.roughness = 0.4f;
    params.ao = 1.0f;
    m_default_material = m_renderer->materials().create_instance(*gbuffer_mat, params, "runtime_default");
    if (!m_default_material.valid()) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: failed to create default material");
    }
}

void Runtime::sync_meshes_from_assets() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!m_mesh_library) {
        m_mesh_library = std::make_unique<rendering::MeshLibrary>();
        if (m_renderer) {
            m_renderer->set_mesh_library(m_mesh_library.get());
        }
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<MeshComponent>()) {
        auto* comp = world.get<MeshComponent>(e);
        if (comp == nullptr || !comp->mesh_id.valid()) {
            continue;
        }
        if (m_mesh_handles.find(comp->mesh_id) != m_mesh_handles.end()) {
            continue;
        }
        // Already reported as unloadable: skip in silence instead of re-logging
        // the identical failure on every frame.
        if (m_failed_meshes.find(comp->mesh_id) != m_failed_meshes.end()) {
            continue;
        }
        auto handle = m_manager.find(comp->mesh_id);
        if (!handle) {
            // Kick off an async load; the mesh will be picked up next frame.
            m_manager.load_mesh(comp->mesh_id);
            continue;
        }
        if (handle->state == assets::AssetState::Loading) {
            continue;
        }
        if (handle->state == assets::AssetState::Failed) {
            // Missing/corrupt asset. Failed is terminal in AssetManager, so this
            // would otherwise re-fire on every sync pass (twice per frame). Record
            // it and warn exactly once; render proceeds without this object (safe
            // fallback). Cleared on hot reload and scene load so a re-cooked asset
            // is picked up again.
            m_failed_meshes.insert(comp->mesh_id);
            ++m_failed_mesh_reports;
            NF_LOG_WARN(LogCategory::Core, "Runtime: mesh asset {} failed: {}", comp->mesh_id.to_string(),
                        handle->error);
            continue;
        }
        if (handle->state == assets::AssetState::Ready && handle->asset) {
            // Phase 11 W1: the conversion lives on the rendering side now
            // (Assets is CPU-pure and must not know the mesh type).
            auto static_mesh = rendering::make_static_mesh(*handle->asset, handle->asset->logical_path);
            if (!static_mesh) {
                NF_LOG_WARN(LogCategory::Core, "Runtime: failed to convert mesh asset {}",
                            comp->mesh_id.to_string());
                continue;
            }
            if (!static_mesh->is_uploaded()) {
                // GPU upload happens on the main/render thread (this thread).
                if (!static_mesh->upload(m_device)) {
                    NF_LOG_WARN(LogCategory::Core, "Runtime: failed to upload mesh asset {}",
                                comp->mesh_id.to_string());
                    continue;
                }
            }
            auto h = m_mesh_library->add(std::move(static_mesh));
            if (!h.valid()) {
                NF_LOG_WARN(LogCategory::Core, "Runtime: MeshLibrary rejected asset {}",
                            comp->mesh_id.to_string());
                continue;
            }
            m_mesh_handles[comp->mesh_id] = h;
            NF_LOG_INFO(LogCategory::Core, "Runtime: mesh asset {} uploaded (handle {})",
                        comp->mesh_id.to_string(), h.id);
        }
    }
}

bool Runtime::load_scene(const std::string& logical_path, std::string& out_error) {
    auto result = load_scene_from_vfs(m_vfs, logical_path);
    if (!result.success) {
        out_error = result.error;
        NF_LOG_ERROR(LogCategory::Core, "Runtime: failed to load scene '{}': {}", logical_path, result.error);
        return false;
    }
    if (!result.missing_assets.empty()) {
        for (const auto& a : result.missing_assets) {
            NF_LOG_WARN(LogCategory::Core, "Runtime: scene '{}' missing asset: {}", logical_path, a);
        }
    }
    for (const auto& w : result.warnings) {
        NF_LOG_WARN(LogCategory::Core, "Runtime: scene '{}' warning: {}", logical_path, w);
    }
    // The player's overrides, if this machine has any. Absence is the normal
    // case (a fresh install) and a broken file must not stop a level from
    // loading, so both are warnings at most — the scene's authored taste is
    // still a complete answer on its own.
    {
        const auto settings_exist = m_vfs.exists(kDefaultPlayerSettingsPath);
        if (settings_exist.ok && settings_exist.value) {
            std::string settings_error;
            if (!load_player_settings(kDefaultPlayerSettingsPath, settings_error)) {
                NF_LOG_WARN(LogCategory::Core, "Runtime: player settings ignored — {}",
                            settings_error);
            }
        }
    }
    // A previous scene's modules get a chance to release what they hold while
    // the world they were pointing at is still alive.
    if (m_scene_data_ptr && m_scene_data_ptr->scene && !m_gameplay_modules.empty()) {
        gameplay::GameplayContext unload_ctx = build_gameplay_context(0.0f);
        for (GameplaySlot& slot : m_gameplay_modules) {
            slot.module->on_scene_unload(unload_ctx);
        }
    }
    adopt_scene(std::move(result.scene), logical_path);
    return true;
}

void Runtime::adopt_scene(std::unique_ptr<scene::Scene> scene, const std::string& logical_path) {
    m_scene_data_ptr = std::make_unique<SceneRuntimeData>();
    m_scene_data_ptr->scene = std::move(scene);
    m_loaded_scene_path = logical_path;
    // A scene built in code normally sets local transforms only. The runtime
    // (weapons, AI visibility, blasts) reads WORLD space, so publish the world
    // transform here rather than waiting for the first update - the first shot
    // can be fired before any update runs.
    scene::propagate_transforms(m_scene_data_ptr->scene->world());
    // A round in the old world must not appear in the new one. Projectiles
    // carry entity handles and profile snapshots from the scene they were
    // fired in, so a scene change is the one place they cannot be repaired.
    m_projectiles.clear();
    m_impacts.clear();
    m_shots.clear();
    // The characters load here, before the first frame: a character is a
    // glTF with a skin and animations, and the mesh it needs has to exist
    // before anything tries to draw it.
    load_characters();
    // A freshly loaded scene gets a fresh attempt at every mesh. Failure was
    // recorded per-asset, and the asset may have been cooked since it last
    // failed, so the previous scene's verdicts must not carry over.
    m_failed_meshes.clear();
    m_failed_mesh_reports = 0;
    // The next scene is allowed to overflow the light slots too, and saying so
    // again is a fact about this scene, not a fact about the run.
    m_local_light_warning_shown = false;
    // A scene-owned GPU timing request does not carry across a scene change.
    // extract_post_process() re-enables it before the first render if the new
    // scene asks. The developer's profiling switch is untouched.
    m_gpu_frame_timing_required = false;
    m_last_gpu_sample_serial = 0;
    apply_gpu_timing_state();
    // Assign display names to entities that have none (older scenes predate
    // NameComponent). Keeps the outliner meaningful without touching the file.
    {
        auto& world = m_scene_data_ptr->scene->world();
        for (auto e : world.all_entities()) {
            if (world.has<scene::NameComponent>(e)) {
                continue;
            }
            std::string label;
            if (world.has<CameraComponent>(e)) {
                label = "Camera";
            } else if (world.has<DirectionalLight>(e)) {
                label = "DirectionalLight";
            } else if (world.has<PointLightComponent>(e)) {
                label = "PointLight";
            } else if (world.has<SpotLightComponent>(e)) {
                label = "SpotLight";
            } else if (world.has<MeshComponent>(e)) {
                label = "Mesh";
            } else if (world.has<scripting::ScriptComponent>(e)) {
                label = "Script";
            } else if (world.has<vfx::ParticleComponent>(e)) {
                label = "Particles";
            } else if (world.has<physics::ClothComponent>(e)) {
                label = "Cloth";
            } else if (world.has<physics::CharacterComponent>(e)) {
                label = "Character";
            } else if (world.has<NavMeshComponent>(e)) {
                label = "NavMesh";
            } else if (world.has<NavAgentComponent>(e)) {
                label = "NavAgent";
            } else {
                label = "Entity " + std::to_string(e.id);
            }
            world.add<scene::NameComponent>(e, scene::NameComponent{label});
        }
    }
    // Build the physics world from the scene that was just loaded, so a scene
    // containing physics is live as soon as it opens.
    rebuild_physics_from_scene();
    // Scripts: a fresh scene gets a fresh VM cache and its file-backed
    // sources resolved from the VFS, so the previous scene's bytecode never
    // serves the next one and a path-only component becomes runnable.
    clear_script_cache();
    m_script_updates = 0;
    resolve_scene_scripts();
    // Particles/cloth: live objects are per-scene artefacts like fracture
    // assets — the previous scene's emitters and sheets do not carry over.
    build_scene_particles();
    build_scene_cloths();
    // Audio environment: reverb zones, the level's theme and its ambience bed.
    // Same rule as the two above — the scene owns them, so the previous scene's
    // do not carry over.
    build_scene_audio();
    // Combat feedback: the first enabled FeedbackComponent owns the level's
    // tuning, and a scene without one draws and mixes nothing new.
    bind_scene_feedback();
    // AI population is game-registered per scene; a previous scene's crowd
    // must not govern the next one.
    if (m_ai_world) {
        m_ai_world->clear();
    }
    // Navigation is a per-scene artefact like the crowd: the previous scene's
    // mesh and its walkers are dropped with it, and the new one bakes its own
    // (a no-op for a scene with no NavMesh line).
    m_nav_agents.clear();
    build_scene_navmesh();
    // Vehicles are a per-scene artefact on the same terms: the previous
    // scene's Jolt world, statics and cars die with it, and the new one binds
    // its own (a teardown for a scene with no Vehicle line).
    bind_scene_vehicles();
    // The previous scene's bindings and shards do not carry over. Assets stay
    // (the game registered them), but which entities are breakable and what is
    // flying around belongs to this scene.
    reset_destruction_state();
    // A scene that declares breakable objects gets them bound here, so the
    // feature is live the moment the level opens. Until this existed the whole
    // Phase 19 chain — enable, cook, bind, damage, step, draw — was reachable
    // only from tests: no game entry point called any of it.
    bind_scene_destructibles();
    // Gameplay: instantiate the registered modules on the first load, let the
    // scene's components restore their state, then announce the new scene.
    // apply_gameplay_state() must follow init_gameplay() — there is nothing to
    // restore into until the module instances exist.
    // A freshly loaded scene starts a fresh simulation timeline: step N of this
    // scene is frame N. The counter is not reset on shutdown_gameplay alone,
    // because a scene loaded without re-adding modules still has to number its
    // steps from zero — an input log's frame index is only meaningful relative
    // to the scene it was recorded against.
    m_gameplay_frame = 0;
    init_gameplay();
    apply_gameplay_state();
    {
        gameplay::GameplayContext ctx = build_gameplay_context(0.0f);
        for (GameplaySlot& slot : m_gameplay_modules) {
            slot.module->on_scene_load(ctx);
        }
    }
    ++m_scene_version;
    NF_LOG_INFO(LogCategory::Core, "Runtime: scene loaded '{}' with {} entities", logical_path,
                m_scene_data_ptr->scene->world().alive_entity_count());
    // Scene → AssetManager: kick off loads for every referenced mesh.
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<MeshComponent>()) {
        auto* comp = world.get<MeshComponent>(e);
        if (comp != nullptr && comp->mesh_id.valid()) {
            m_manager.load_mesh(comp->mesh_id);
        }
    }
    // Pump once so synchronously-available assets are Ready immediately.
    // Async (JobSystem) assets complete over the next frames via update().
    m_manager.update();
    sync_meshes_from_assets();
    // Renderer init is lazy per target size, but try now so load failures surface early.
    (void)ensure_renderer_initialized();
}

scene::Scene* Runtime::edit_scene() {
    return (m_scene_data_ptr && m_scene_data_ptr->scene) ? m_scene_data_ptr->scene.get() : nullptr;
}

const scene::Scene* Runtime::scene() const {
    return (m_scene_data_ptr && m_scene_data_ptr->scene) ? m_scene_data_ptr->scene.get() : nullptr;
}

bool Runtime::save_scene(const std::string& logical_path, std::string& out_error) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        out_error = "No scene loaded";
        return false;
    }
    if (!save_scene_to_vfs(m_vfs, logical_path, *m_scene_data_ptr->scene, out_error)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: failed to save scene '{}': {}", logical_path, out_error);
        return false;
    }
    m_loaded_scene_path = logical_path;
    NF_LOG_INFO(LogCategory::Core, "Runtime: scene saved '{}'", logical_path);
    return true;
}

void Runtime::mark_scene_edited() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    scene::propagate_transforms(m_scene_data_ptr->scene->world());
    ++m_scene_version;
}

void Runtime::update(float dt) {
    // Frame-local combat events: the completed frame becomes the script-visible
    // `previous` slot, and `current` starts empty for this frame's shots.
    m_impacts.begin_frame();
    m_shots.begin_frame();
    // The input source refreshes once, here, before anything reads it: a
    // pointer reports a DELTA, and a delta that two readers each consumed
    // would make the camera and the script disagree about how far the mouse
    // moved. Sources with no pointer do nothing.
    if (m_input_source != nullptr) {
        m_input_source->begin_frame();
    }
    // Streaming first: it moves world membership (merge/unload chunks), and
    // everything below — asset sync, physics, animation — must see the
    // post-stream world. No-op unless enable_streaming ran.
    step_streaming();
    // AssetManager.update(): finalize async loads finished on workers
    // (Loading -> Ready). The GPU upload is sync_meshes_from_assets' business
    // via rendering::MeshLibrary, since Phase 11 W1.
    m_manager.update();
    sync_meshes_from_assets();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    // Edit-mode freeze: the editor drives the Runtime with dt = 0 while it is
    // NOT playing, so the viewport shows a frozen authoring world. Without
    // this, physics / animation / gameplay / audio / destruction all advance
    // every editor frame and the scene "plays itself" before Play is pressed —
    // the exact "models move while not playing, Play button does nothing"
    // report. dt <= 0 still propagates transforms (editor moves stay visible)
    // but steps no simulation. Packaged games and tests always pass dt > 0,
    // so they are unaffected.
    if (!(dt > 0.0f)) {
        auto& world = m_scene_data_ptr->scene->world();
        scene::propagate_transforms(world);
        return;
    }
    // Combat feedback consumes the PREVIOUS frame's events, which begin_frame
    // just published. That is what makes a fire_weapon() call made between
    // updates (a host, a test, a Lua script's own shot) visible: the events are
    // ingested exactly once, effects start at age 0, and the render path below
    // sees them this frame. Unbound scenes do no work at all.
    if (m_feedback_bound) {
        m_feedback.update(dt, m_shots.previous(), m_impacts.previous());
    }
    // Quiet clocks advance once per frame, before new damage can reset them.
    step_damage_ledgers(dt);
    // Physics runs on its own fixed clock, so the scene's motion is the same
    // whether the frame took 5 ms or 50. The render path then only reads the
    // transforms physics produced.
    step_physics(dt);
    // Cars simulate in their own Jolt world on the same fixed-clock terms:
    // drive inputs armed, Jolt solves, chassis poses land in the Transforms
    // before anything downstream reads them. No-op without a bound car.
    step_vehicles(dt);
    // Contacts are read straight after the step they belong to: the solver's
    // manifolds are only valid until the next step, and a hit landed this frame
    // has to be able to spawn its shards in the step_destruction below. A
    // no-op unless the scene carries a bound destructible.
    step_impact_damage();
    // Animation advances on the frame delta — it is a function of time, not of
    // state, so it needs no fixed clock. Runs after physics, so an entity that
    // carries both is driven by its animation.
    step_animation(dt);
    // The day/night clock advances here, before the render extracts the light and
    // sky, so a frame's sun is the one this frame's dt produced. Advancing it in
    // render() instead would make the hour depend on how often the frame was
    // drawn, which is not the same thing as how long it took.
    advance_time_of_day(dt);
    // Gameplay runs after simulation, so a module observes where this frame
    // actually put everything, and before propagation, so whatever it writes
    // is what gets rendered rather than a frame late.
    step_gameplay(dt);
    // Scripts run after gameplay and before destruction, so a script observes
    // the same post-simulation world a module does, and damage it applies
    // still spawns shards in the same frame.
    step_script(dt);
    // World-scale AI plans next: a plan only assigns tiers and grants, so a
    // script that moved an actor this frame is already visible to it.
    step_ai_world(dt);
    // Navigation walks last of the movers: an agent sees this frame's
    // simulation results and the AI plan, and writes its transform before
    // propagation below — so the walk renders where it ends up this frame.
    step_navmesh(dt);
    // Particles and cloth simulate after decisions, before debris, so a burst
    // or gust triggered this frame is already integrated when it renders.
    step_particles(dt);
    step_cloth(dt);
    // Debris simulates in its own Jolt world, after gameplay so a module that
    // applies damage this frame sees its shards spawn this frame. Shards are
    // posed by the Jolt world rather than by scene Transforms, so propagation
    // below does not touch them. No-op unless enable_destruction() ran.
    step_destruction(dt);
    auto& world = m_scene_data_ptr->scene->world();
    scene::propagate_transforms(world);
    // The characters move AFTER the animation has been sampled (the pose is
    // what the skinning reads) and after the transforms are final (the speed
    // that drives the locomotion state machine is measured from them).
    step_characters(dt);
    // Damage over time ticks before this frame's new hits. A burn applied by a
    // shot below therefore begins next frame, never in the same instant it was
    // applied, even if its tick interval is shorter than one frame.
    step_status_effects(dt);
    // The gun fires after the player has moved, so the pivot a shot leaves
    // from is this frame's, and BEFORE the camera is placed, so the recoil the
    // shot adds is part of the frame the player sees rather than the next one.
    // The agents decide first: step_weapons reads their trigger, so each weapon
    // is stepped exactly once per frame.
    // No-op unless an entity carries a WeaponComponent and an active camera
    // follows it.
    step_combat_ai(dt);
    step_weapons(dt);
    // Rounds in flight advance immediately after the trigger that spawned
    // them, so a projectile fired this frame already travels this frame. Its
    // swept segment is traced against the same colliders a hitscan shot uses.
    step_projectiles(dt);
    // This frame's shots are now in the buffer; near misses can suppress the
    // agents they passed before the camera is placed.
    step_suppression(dt);
    // The camera is placed AFTER the transforms are final, so it follows the
    // character where the character actually IS this frame and not where it
    // was. It is also after the scripts, so a script that moved the player
    // is followed in the same frame.
    update_follow_camera(dt);
    // Audio mixes after the transforms AND the camera are final for this frame.
    // The listener is the camera, so doing this before placement would pan a
    // source against last frame's view; feedback one-shots generated this frame
    // are also mixed with the listener they will actually be heard from.
    step_audio(dt);
    // Dynamic resolution consumes the same dt the simulation did. It advances
    // after the frame's work so the sample describes the frame that just ran,
    // and the next render applies the new scale. Disabled by default; a scene
    // has to opt in with `dynamic_resolution=true`.
    //
    // Phase 40: a GPU source uses the last completed timestamp sample, not the
    // submit-to-fence estimate and not the same sample twice. The renderer
    // publishes a serial with every completed readback; consuming it here is
    // what keeps an editor that updates more often than it renders from
    // treating one GPU frame as evidence for many. No new sample (or no
    // timestamp support) means a CPU fallback inside the governor.
    // Phase 42: a scene that opted into the window policy owns the scale, and
    // the per-frame governor is skipped — two policies on one number would
    // fight, and the loser would be whichever ran last.
    if (m_frame_policy_active) {
        const auto decision = rendering::frame_policy_step(m_frame_window, m_frame_policy_config,
                                                           m_frame_policy_state, dt);
        if (decision.changed) {
            NF_LOG_INFO(LogCategory::Render, "Frame policy: {} -> render scale {:.2f}",
                        rendering::frame_policy_reason_name(decision.reason), decision.scale);
        }
        m_frame_policy_reason = decision.reason;
    } else if (m_dynamic_resolution.enabled()) {
        f32 gpu_frame_s = 0.0f;
        bool gpu_frame_valid = false;
        if (m_dynamic_resolution.config().timing ==
                rendering::DynamicResolutionTiming::Gpu &&
            m_renderer != nullptr) {
            const auto& stats = m_renderer->last_stats();
            if (stats.gpu_timing_valid && stats.gpu_sample_serial != 0 &&
                stats.gpu_sample_serial != m_last_gpu_sample_serial &&
                std::isfinite(stats.gpu_frame_ms) && stats.gpu_frame_ms > 0.0) {
                gpu_frame_s = static_cast<f32>(stats.gpu_frame_ms / 1000.0);
                gpu_frame_valid = true;
                m_last_gpu_sample_serial = stats.gpu_sample_serial;
            }
        }
        m_dynamic_resolution.update(dt, gpu_frame_s, gpu_frame_valid);
    }
}

u32 Runtime::step_animation(float dt) {
    m_animated_entities = 0;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }

    auto& world = m_scene_data_ptr->scene->world();
    u32 stepped = 0;

    for (auto e : world.query<animation::AnimationComponent>()) {
        auto* anim = world.get<animation::AnimationComponent>(e);
        if (anim == nullptr || anim->skeleton.bones.empty()) {
            continue;
        }
        auto* transform = world.get<scene::Transform>(e);

        // Capture the authored placement on the first run. Doing it lazily
        // rather than at load keeps whatever the editor set before the first
        // frame, and keeps the base and the transform consistent.
        if (!anim->has_base_transform && transform != nullptr) {
            anim->base_translation =
                Vec3(transform->local_x, transform->local_y, transform->local_z);
            anim->base_rotation = scene::quat_from_euler_xyz_degrees(
                transform->rot_x, transform->rot_y, transform->rot_z);
            anim->base_scale = Vec3(transform->scale_x, transform->scale_y, transform->scale_z);
            anim->has_base_transform = true;
        }

        if (anim->use_state_machine) {
            if (!anim->paused) {
                m_anim_clip_table.clear();
                for (const auto& entry : anim->clips) {
                    m_anim_clip_table[entry.first] = &entry.second;
                }
                anim->state_machine.update(dt, anim->skeleton, m_anim_clip_table,
                                           anim->last_local_pose);
            }
            if (anim->last_local_pose.size() != anim->skeleton.bones.size()) {
                // Never produced a pose (paused from the start, or a machine
                // with no states). Fall back to rest so the world pose below is
                // well-defined instead of stale or empty.
                animation::AnimationClip rest;
                rest.sample(0.0f, anim->skeleton, anim->last_local_pose);
            }
        } else {
            const auto it = anim->clips.find(anim->player.clip_name());
            if (it == anim->clips.end() || it->second.tracks.empty()) {
                // No clip data. The loader already warned about this; stepping
                // would just re-sample the rest pose every frame.
                continue;
            }
            const animation::AnimationClip& clip = it->second;
            // A paused or stopped player returns its current time unchanged, so
            // this is the same call in every state.
            const f32 sample_time = anim->player.update(dt, clip.duration);
            clip.sample(sample_time, anim->skeleton, anim->last_local_pose);
        }

        animation::compute_world_transforms(anim->skeleton, anim->last_local_pose,
                                            anim->last_world_pose);
        ++stepped;

        // Drive the entity from the root bone so the motion reaches the renderer
        // and the editor. The delta is taken from the rest pose, so a clip that
        // leaves the root alone leaves the entity exactly where it was authored.
        //
        // A CHARACTER is the exception: its motion IS the mesh, and the mesh
        // is skinned from the same pose. Driving the entity from the root bone
        // as well would move the character twice -- once through the skinning
        // and once through its placement -- and a character's placement
        // belongs to the game (a script, a controller, the player), not to
        // the clip.
        if (transform != nullptr && anim->has_base_transform && !anim->last_local_pose.empty() &&
            !world.has<CharacterComponent>(e)) {
            const animation::LocalPose& bone = anim->last_local_pose[0];
            const animation::Bone& rest = anim->skeleton.bones[0];

            const Vec3 delta_t = bone.translation - rest.rest_translation;
            const Quat delta_q = bone.rotation * rest.rest_rotation.inverse();
            const Vec3 delta_s = {
                safe_scale_ratio(bone.scale.x, rest.rest_scale.x),
                safe_scale_ratio(bone.scale.y, rest.rest_scale.y),
                safe_scale_ratio(bone.scale.z, rest.rest_scale.z),
            };

            // The delta lives in the model's space, so the authored rotation
            // has to be applied to it before it is added to the placement.
            const Vec3 offset = anim->base_rotation.rotate(delta_t);
            transform->local_x = anim->base_translation.x + offset.x;
            transform->local_y = anim->base_translation.y + offset.y;
            transform->local_z = anim->base_translation.z + offset.z;

            const Quat composed = (anim->base_rotation * delta_q).normalized();
            scene::euler_xyz_degrees_from_quat(composed, transform->rot_x, transform->rot_y,
                                               transform->rot_z);

            transform->scale_x = anim->base_scale.x * delta_s.x;
            transform->scale_y = anim->base_scale.y * delta_s.y;
            transform->scale_z = anim->base_scale.z * delta_s.z;

            transform->dirty = true;
        }
    }

    m_animated_entities = stepped;
    return stepped;
}

std::vector<Runtime::AnimatedTransformSample> Runtime::animated_transform_samples() const {
    std::vector<AnimatedTransformSample> samples;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return samples;
    }

    const auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<animation::AnimationComponent>()) {
        const auto* t = world.get<scene::Transform>(e);
        if (t == nullptr) {
            continue;
        }
        // A CHARACTER's animation does not drive its transform: its motion is
        // in its mesh, and its placement belongs to the game. Reporting the
        // transform here would say "this entity never moved" about a character
        // that is walking perfectly well.
        if (world.has<CharacterComponent>(e)) {
            continue;
        }
        AnimatedTransformSample s;
        s.entity_id = e.id;
        s.translation = Vec3(t->local_x, t->local_y, t->local_z);
        s.rotation_euler_degrees = Vec3(t->rot_x, t->rot_y, t->rot_z);
        s.scale = Vec3(t->scale_x, t->scale_y, t->scale_z);
        samples.push_back(s);
    }

    // query() walks the component pool, whose order is insertion order and so
    // depends on how the scene was built. Sorting makes the snapshot comparable
    // across runs and across load/save.
    std::sort(samples.begin(), samples.end(),
              [](const AnimatedTransformSample& a, const AnimatedTransformSample& b) {
                  return a.entity_id < b.entity_id;
              });
    return samples;
}

void Runtime::rebase_animation(ecs::Entity entity) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    auto* anim = world.get<animation::AnimationComponent>(entity);
    auto* transform = world.get<scene::Transform>(entity);
    if (anim == nullptr || transform == nullptr) {
        return;
    }
    anim->base_translation = Vec3(transform->local_x, transform->local_y, transform->local_z);
    anim->base_rotation =
        scene::quat_from_euler_xyz_degrees(transform->rot_x, transform->rot_y, transform->rot_z);
    anim->base_scale = Vec3(transform->scale_x, transform->scale_y, transform->scale_z);
    anim->has_base_transform = true;
}

u32 Runtime::step_audio(float dt) {
    m_audio_sources_mixed = 0;
    m_audio_output_peak = 0.0f;
    m_audio_left.clear();
    m_audio_right.clear();

    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    if (!m_audio_device) {
        // Best available output: real hardware where a backend opens one,
        // silence otherwise (headless/CI). Never null.
        m_audio_device = audio::create_output_device();
    }
    if (dt <= 0.0f) {
        return 0;
    }

    auto& world = m_scene_data_ptr->scene->world();

    // Combat feedback cues (Phase 56). A cue can arrive on a frame that mixes
    // no whole block (step_audio returns 0 below); the director clears its own
    // list on the next update, so copy into a pending list and start an Emitter
    // now. The Emitter owns the playback cursor and survives until the buffer
    // ends, which is what makes a shot audible even when two frames fit inside
    // one fixed-size audio block.
    if (m_feedback_bound) {
        const u32 rate = m_audio_device->sample_rate();
        if (rate != 0 && rate != m_feedback_buffer_rate) {
            // New rate: old voices point at buffers built for the old clock.
            m_feedback_voices.clear();
            m_feedback_shot_buffer = audio::make_gunshot_buffer(rate);
            m_feedback_impact_buffers[static_cast<usize>(runtime::DamageType::Ballistic)] =
                audio::make_impact_buffer(rate, audio::ImpactSound::Ballistic);
            m_feedback_impact_buffers[static_cast<usize>(runtime::DamageType::Explosive)] =
                audio::make_impact_buffer(rate, audio::ImpactSound::Explosive);
            m_feedback_impact_buffers[static_cast<usize>(runtime::DamageType::Fire)] =
                audio::make_impact_buffer(rate, audio::ImpactSound::Fire);
            m_feedback_buffer_rate = rate;
        }
        constexpr usize kMaxPendingCues = 64;
        for (const runtime::FeedbackSound& cue : m_feedback.sounds()) {
            if (m_feedback_pending.size() >= kMaxPendingCues) {
                m_feedback_pending.erase(m_feedback_pending.begin());
            }
            m_feedback_pending.push_back(cue);
        }
        m_feedback.clear_sounds();

        constexpr usize kMaxFeedbackVoices = 24;
        for (const runtime::FeedbackSound& cue : m_feedback_pending) {
            // Phase 59: one buffer per damage channel. The cue already chose the
            // channel (the director maps DamageType to it); the runtime only owns
            // the buffers, so the two never disagree about which sound a blast is.
            const audio::AudioBuffer* buffer = &m_feedback_shot_buffer;
            switch (cue.kind) {
            case runtime::FeedbackSound::Kind::ImpactBallistic:
                buffer = &m_feedback_impact_buffers[static_cast<usize>(
                    runtime::DamageType::Ballistic)];
                break;
            case runtime::FeedbackSound::Kind::ImpactExplosive:
                buffer = &m_feedback_impact_buffers[static_cast<usize>(
                    runtime::DamageType::Explosive)];
                break;
            case runtime::FeedbackSound::Kind::ImpactFire:
                buffer = &m_feedback_impact_buffers[static_cast<usize>(
                    runtime::DamageType::Fire)];
                break;
            case runtime::FeedbackSound::Kind::Shot:
            default:
                break;
            }
            if (buffer->samples.empty()) {
                continue;
            }
            if (m_feedback_voices.size() >= kMaxFeedbackVoices) {
                m_feedback_voices.erase(m_feedback_voices.begin());
            }
            audio::Emitter voice{};
            voice.buffer = buffer;
            voice.position = cue.position;
            voice.volume = cue.volume;
            voice.pitch = cue.pitch;
            voice.playing = true;
            voice.looping = false;
            voice.spatial = true;
            // Gunfire and impacts are world sounds: a wall between the listener
            // and the hit should muffle them like any other source.
            voice.occluded = true;
            voice.bus = audio::BusId::Sfx;
            m_feedback_voices.push_back(voice);
        }
        m_feedback_pending.clear();
    }

    // The listener follows the active camera, so a source pans relative to what
    // the player is actually looking at.
    {
        rendering::Camera cam{};
        if (extract_camera(0, 0, cam)) {
            const Vec3 cam_pos = cam.position;
            m_audio_listener.position = cam_pos;
            const Vec3 forward = cam.target - cam_pos;
            const f32 len = forward.length();
            if (len > 1e-6f) {
                m_audio_listener.forward = forward * (1.0f / len);
            }
            m_audio_listener.up = cam.up;
        }
    }

    // The device runs in fixed-size blocks while frames arrive at arbitrary
    // deltas, so the remainder carries over. Without the accumulator a 240 fps
    // machine would mix a quarter of the audio a 60 fps machine does for the
    // same wall-clock second.
    m_audio_accumulator += dt;
    const f32 block_seconds =
        static_cast<f32>(audio::kDefaultBufferFrames) / static_cast<f32>(audio::kDefaultSampleRate);
    u32 blocks = 0;
    while (m_audio_accumulator >= block_seconds && blocks < 4) {
        m_audio_accumulator -= block_seconds;
        ++blocks;
    }
    if (blocks == 0) {
        return 0;
    }

    const usize frames = static_cast<usize>(blocks) * audio::kDefaultBufferFrames;
    m_audio_left.assign(frames, 0.0f);
    m_audio_right.assign(frames, 0.0f);

    // The backend's own output is the base of the mix. The null device writes
    // silence, so anything non-zero afterwards is the scene's audio.
    m_audio_device->request_buffer(m_audio_left.data(), m_audio_right.data(), frames);

    // One scene mixes the whole block, in the order AudioScene documents:
    // begin_block sizes it, mix_emitter runs each source, finalize adds music,
    // ambience, the reverb tail and the bus volumes onto the buffers above.
    m_audio_scene.set_listener(m_audio_listener);
    m_audio_scene.begin_block(frames, m_audio_device->sample_rate());

    m_audio_emitter_seen.clear();
    for (auto e : world.query<audio::AudioComponent>()) {
        auto* comp = world.get<audio::AudioComponent>(e);
        if (comp == nullptr) {
            continue;
        }
        const audio::AudioBuffer* buffer = comp->resolved_buffer();
        if (buffer == nullptr) {
            continue;
        }

        if (comp->autoplay && !comp->playing) {
            comp->playing = true;
        }
        if (!comp->playing) {
            continue;
        }

        // Take the source's position from the entity, so a moving emitter
        // spatialises correctly.
        Vec3 position = Vec3(0.0f, 0.0f, 0.0f);
        if (const auto* t = world.get<scene::Transform>(e)) {
            position = Vec3(t->world_x, t->world_y, t->world_z);
        }

        // The emitter is persistent state, not a per-frame temporary: its
        // cursors and its occlusion filter have to survive the frame or the
        // playback position truncates every block and the muffle clicks.
        audio::Emitter& emitter = m_audio_emitters[e];
        emitter.buffer = buffer;
        emitter.volume = comp->volume;
        emitter.pitch = comp->pitch;
        emitter.looping = comp->looping;
        emitter.playing = true;
        emitter.spatial = comp->spatial;
        emitter.position = position;
        emitter.spatial_settings = comp->spatial_settings;
        emitter.occluded = comp->occluded;
        emitter.bus = comp->bus;

        m_audio_scene.mix_emitter(emitter);
        m_audio_emitter_seen.push_back(e);

        // Both cursor fields go back to the component. Persisting only
        // sample_cursor would re-seek a rate-converted buffer to a truncated
        // position every block and drift audibly away from where it should be.
        comp->sample_cursor = emitter.sample_cursor;
        comp->sample_position = emitter.sample_position;
        comp->playing = emitter.playing;
        ++m_audio_sources_mixed;
    }

    // Retire sources that stopped playing or lost their component, so the map
    // tracks the scene instead of growing once per emitter ever created.
    for (auto it = m_audio_emitters.begin(); it != m_audio_emitters.end();) {
        const bool live =
            std::find(m_audio_emitter_seen.begin(), m_audio_emitter_seen.end(),
                      it->first) != m_audio_emitter_seen.end();
        it = live ? std::next(it) : m_audio_emitters.erase(it);
    }

    // Feedback one-shots mix after the scene's own sources, through the same
    // buses and reverb. The mixer clears `playing` when a non-looping buffer
    // reaches its end; retiring here is what keeps a fight from growing one
    // voice per shot for the whole level.
    for (audio::Emitter& voice : m_feedback_voices) {
        if (!voice.playing || voice.buffer == nullptr) {
            continue;
        }
        m_audio_scene.mix_emitter(voice);
        ++m_audio_sources_mixed;
    }
    m_feedback_voices.erase(
        std::remove_if(m_feedback_voices.begin(), m_feedback_voices.end(),
                       [](const audio::Emitter& voice) {
                           return !voice.playing || voice.buffer == nullptr;
                       }),
        m_feedback_voices.end());

    m_audio_scene.finalize(m_audio_left.data(), m_audio_right.data());

    for (usize i = 0; i < frames; ++i) {
        m_audio_output_peak = std::max(m_audio_output_peak, std::abs(m_audio_left[i]));
        m_audio_output_peak = std::max(m_audio_output_peak, std::abs(m_audio_right[i]));
    }
    // Real-output backends consume the final mix on their own clock; the null
    // backend ignores this (its contract ends at request_buffer).
    if (m_audio_device->accepts_push()) {
        m_audio_device->submit_mix(m_audio_left.data(), m_audio_right.data(), frames);
    }

    return blocks;
}

// --- Gameplay modules (Phase 10) --------------------------------------------

void Runtime::sort_gameplay_modules() {
    std::stable_sort(
        m_gameplay_modules.begin(), m_gameplay_modules.end(),
        [](const GameplaySlot& a, const GameplaySlot& b) {
            const f32 pa = a.module->update_priority();
            const f32 pb = b.module->update_priority();
            if (pa != pb) return pa < pb;
            // Tie-break on name, so the order is a property of the modules
            // rather than of registration or link order — both of which vary.
            return std::string_view(a.module->name()) < std::string_view(b.module->name());
        });
}

void Runtime::init_gameplay() {
    // Engine-shipped modules (PlayerController, OrbitCamera) must be registered
    // before the registry walk: registration through static initialisers alone
    // is at the linker's mercy (an unreferenced object in a static library is
    // dropped, and the module silently vanishes from the binary). Naming the
    // modules in BuiltinModules.cpp both registers them and pins their objects
    // into every binary that links NFRuntime.
    gameplay::register_builtin_modules();

    auto& registry = gameplay::GameplayModuleRegistry::instance();
    for (const std::string& name : registry.names()) {
        // Do NOT create a second instance of a module that is already loaded.
        // init_gameplay() runs on every scene load (adopt_scene), and the
        // modules are meant to SURVIVE a reload (they get on_scene_unload and
        // then on_scene_load). Appending unconditionally left N copies of
        // every module after N scene loads: they were stepped N times per
        // frame, and find_gameplay_module() resolves a name to the FIRST
        // match — so the scene's gameplay state was written into the
        // instance belonging to the FIRST scene while the live instance kept
        // its defaults.
        if (find_gameplay_module(name) != nullptr) continue;
        if (auto module = registry.create(name)) {
            m_gameplay_modules.push_back(GameplaySlot{std::move(module), false});
        }
    }
    sort_gameplay_modules();

    // Only modules that have not been through on_init yet. A module handed in by
    // add_gameplay_module() after the first load is picked up here rather than
    // skipped, and a second call is a no-op for the ones already initialised.
    gameplay::GameplayContext ctx = build_gameplay_context(0.0f);
    u32 newly_initialised = 0;
    for (GameplaySlot& slot : m_gameplay_modules) {
        if (slot.initialized) continue;
        slot.module->on_init(ctx);
        slot.initialized = true;
        ++newly_initialised;
    }

    if (newly_initialised != 0) {
        NF_LOG_INFO(LogCategory::Core, "Runtime: {} gameplay module(s) initialised ({})",
                    newly_initialised, m_gameplay_modules.size());
    }
}

void Runtime::shutdown_gameplay() {
    if (m_gameplay_modules.empty()) {
        return;
    }
    gameplay::GameplayContext ctx = build_gameplay_context(0.0f);
    for (GameplaySlot& slot : m_gameplay_modules) {
        slot.module->on_shutdown(ctx);
    }
    m_gameplay_modules.clear();
    m_gameplay_updates = 0;
    m_gameplay_frame = 0;
}

gameplay::GameplayModule* Runtime::find_gameplay_module(std::string_view name) {
    for (GameplaySlot& slot : m_gameplay_modules) {
        if (name == slot.module->name()) return slot.module.get();
    }
    return nullptr;
}

gameplay::GameplayModule* Runtime::add_gameplay_module(
    std::unique_ptr<gameplay::GameplayModule> module) {
    if (!module) return nullptr;
    gameplay::GameplayModule* raw = module.get();
    m_gameplay_modules.push_back(GameplaySlot{std::move(module), false});
    sort_gameplay_modules();
    return raw;
}

gameplay::GameplayContext Runtime::build_gameplay_context(f32 dt) {
    gameplay::GameplayContext ctx{};
    ctx.dt      = dt;
    ctx.frame   = m_gameplay_frame;
    ctx.playing = m_playing;
    ctx.input   = m_input_source;
    ctx.audio   = &m_audio_scene;
    ctx.physics = m_physics.get();
    if (m_scene_data_ptr && m_scene_data_ptr->scene) {
        ctx.scene         = m_scene_data_ptr->scene.get();
        ctx.world         = &m_scene_data_ptr->scene->world();
        ctx.scene_version = static_cast<u32>(m_scene_version);
    }
    return ctx;
}

void Runtime::begin_input_capture() {
    // Capturing over an existing recorder would nest them: the outer one would
    // record the inner one's answers, doubling the log. Restore the real source
    // first so the new recorder wraps the device, not the old recorder.
    if (m_input_recorder != nullptr) end_input_capture();

    m_input_prior = m_input_source;
    m_capture_log.clear();
    m_input_recorder = std::make_unique<InputRecorder>(m_input_source, m_capture_log);
    m_input_source = m_input_recorder.get();
}

InputLog Runtime::end_input_capture() {
    if (m_input_recorder == nullptr) return InputLog{};

    // The recorder holds the live source; the replay holds the log, not the
    // source, so stopping a replay does not disturb this restore.
    m_input_source = m_input_recorder->real_source();
    m_input_prior = m_input_source;
    InputLog log = std::move(m_capture_log);
    m_input_recorder.reset();
    return log;
}

void Runtime::play_input_log(const InputLog& log) {
    // A replay replaces the source rather than wrapping it, so an in-flight
    // capture would otherwise be left pointing at a dead recorder. Its log is
    // sealed and handed back first — the caller asked for a replay, not for the
    // capture to be discarded.
    if (m_input_recorder != nullptr) end_input_capture();

    if (m_input_replay == nullptr) m_input_prior = m_input_source;
    m_input_replay = std::make_unique<InputReplay>(log);
    m_input_source = m_input_replay.get();
}

void Runtime::stop_input_replay() {
    if (m_input_replay == nullptr) return;
    m_input_source = m_input_prior;
    m_input_replay.reset();
}

ecs::Entity Runtime::find_gameplay_owner(std::string_view name) const {    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) return ecs::Entity{};
    const ecs::World& world = m_scene_data_ptr->scene->world();
    for (ecs::Entity e : world.query<gameplay::GameplayModuleComponent>()) {
        const auto* comp = world.get<gameplay::GameplayModuleComponent>(e);
        if (comp != nullptr && comp->module_name == name) return e;
    }
    return ecs::Entity{};
}

std::unordered_map<std::string, bool> Runtime::collect_gameplay_enabled() const {
    std::unordered_map<std::string, bool> enabled;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) return enabled;

    const ecs::World& world = m_scene_data_ptr->scene->world();
    for (ecs::Entity e : world.query<gameplay::GameplayModuleComponent>()) {
        const auto* comp = world.get<gameplay::GameplayModuleComponent>(e);
        if (comp == nullptr) continue;
        // Two components naming one module is contradictory; disabled wins,
        // because silently running a module the scene asked to switch off is
        // the more surprising of the two failures.
        const auto it = enabled.find(comp->module_name);
        if (it == enabled.end()) {
            enabled.emplace(comp->module_name, comp->enabled);
        } else {
            it->second = it->second && comp->enabled;
        }
    }
    return enabled;
}

u32 Runtime::step_gameplay(float dt) {
    if (m_gameplay_modules.empty()) {
        return 0;
    }

    const std::unordered_map<std::string, bool> enabled = collect_gameplay_enabled();

    // The replay advances before the modules read, not after: frame N of the log
    // has to answer the queries of step N. Sealing the recorder comes after the
    // loop for the mirror-image reason — the frame has to describe what this
    // step actually polled. Both are anchored to m_gameplay_frame and both are
    // behind the same empty-modules early return, so a log's frame indices line
    // up with the steps the runtime actually took.
    if (m_input_replay != nullptr) m_input_replay->advance(m_gameplay_frame);

    gameplay::GameplayContext ctx = build_gameplay_context(dt);

    u32 stepped = 0;
    for (GameplaySlot& slot : m_gameplay_modules) {
        const auto it = enabled.find(slot.module->name());
        if (it != enabled.end() && !it->second) continue;
        slot.module->on_update(ctx);
        ++stepped;
    }

    if (m_input_recorder != nullptr) m_input_recorder->seal(m_gameplay_frame);

    ++m_gameplay_frame;
    m_gameplay_updates += stepped;
    return stepped;
}

u32 Runtime::step_script(float dt) {
    if (!m_scripts || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    auto& world = m_scene_data_ptr->scene->world();
    // Count runnable scripts first so the counter stays meaningful even when
    // every script is disabled or empty (0 stepped, but the call happened).
    size_t runnable = 0;
    for (auto e : world.query<scripting::ScriptComponent>()) {
        const auto* comp = world.get<scripting::ScriptComponent>(e);
        if (comp != nullptr && comp->enabled && !comp->source.empty()) {
            ++runnable;
        }
    }
    if (runnable == 0) {
        return 0;
    }
    // Scripts read named actions, not devices: the Runtime owns whichever input
    // source is active (keyboard, replay log) and adapts it to two callbacks.
    // The callbacks capture the source pointer, which is stable for this update.
    scripting::ScriptInput input;
    if (m_input_source != nullptr) {
        gameplay::IInputSource* source = m_input_source;
        Runtime* self = this;
        input.pressed = [source](std::string_view action) {
            return source->action_pressed(action);
        };
        input.axis = [source](std::string_view action) {
            return source->action_axis(action);
        };
        // The camera is aimed before the scripts run (update() places it last
        // in the frame, but the aim is written to the component at the top of
        // that call), so a script asking "which way is the camera looking"
        // gets this frame's answer, not the previous frame's.
        input.camera_yaw = [self]() { return self->camera_yaw(); };
        input.camera_move = [self, source](f32& out_x, f32& out_z) {
            move_relative(source->action_axis("move_x"), source->action_axis("move_z"),
                          self->camera_yaw(), out_x, out_z);
        };
        // Trauma is a number the script owns; the runtime owns the clock.
        input.shake = [self](f32 amount) { self->add_camera_shake(amount); };
        input.recoil = [self](f32 pitch, f32 yaw) { self->add_recoil(pitch, yaw); };
    }
    // The gun and hit points need no device, so they are wired OUTSIDE the
    // input branch: a headless run still answers "how much ammo", and a
    // scripted turret still fires on its own timer. Each hook reads the state
    // the runtime already stepped this frame, never a copy that could drift.
    {
        Runtime* self = this;
        input.weapon_ammo = [self]() { return static_cast<f32>(self->weapon_snapshot().ammo); };
        input.weapon_reserve = [self]() { return static_cast<f32>(self->weapon_snapshot().reserve); };
        input.weapon_reloading = [self]() { return self->weapon_snapshot().reloading; };
        input.weapon_reload = [self]() { return self->weapon_reload(); };
        input.weapon_fire = [self]() { return static_cast<f32>(self->fire_weapon()); };
        input.last_hit = [self](bool& hit, f32& x, f32& y, f32& z, f32& distance) {
            const Runtime::WeaponHitInfo& info = self->last_weapon_hit();
            hit = info.hit;
            x = info.position.x;
            y = info.position.y;
            z = info.position.z;
            distance = info.distance;
        };
        input.impact_count = [self]() {
            return static_cast<u32>(self->previous_impact_events().size());
        };
        input.impact_at = [self](u32 index, scripting::ScriptImpact& out) {
            const auto& events = self->previous_impact_events();
            if (static_cast<usize>(index) >= events.size()) {
                return false;
            }
            const ImpactEvent& event = events[index];
            out.shooter_id = event.shooter.id;
            out.shooter_generation = event.shooter.generation;
            out.target_id = event.victim.id;
            out.target_generation = event.victim.generation;
            out.target_name = event.victim_name;
            out.x = event.position.x;
            out.y = event.position.y;
            out.z = event.position.z;
            out.nx = event.normal.x;
            out.ny = event.normal.y;
            out.nz = event.normal.z;
            out.distance = event.distance;
            out.damage = event.damage;
            out.applied = event.applied;
            out.hit_zone = hit_zone_name(event.hit_zone);
            out.armor_absorbed = event.armor_absorbed;
            out.damage_type = damage_type_name(event.damage_type);
            out.blocked_by_faction = event.blocked_by_faction;
            out.killed = event.killed;
            out.splash = event.splash;
            out.status = event.status;
            return true;
        };
        input.shot_count = [self]() {
            return static_cast<u32>(self->previous_shot_events().size());
        };
        input.shot_at = [self](u32 index, scripting::ScriptShot& out) {
            const auto& shots = self->previous_shot_events();
            if (static_cast<usize>(index) >= shots.size()) {
                return false;
            }
            const ShotEvent& shot = shots[index];
            out.shooter_id = shot.shooter.id;
            out.shooter_generation = shot.shooter.generation;
            out.x = shot.origin.x;
            out.y = shot.origin.y;
            out.z = shot.origin.z;
            out.dx = shot.direction.x;
            out.dy = shot.direction.y;
            out.dz = shot.direction.z;
            out.damage = shot.damage;
            out.pellet = shot.pellet;
            out.pellets = shot.pellets;
            out.seed = shot.seed;
            out.projectile = shot.projectile;
            return true;
        };
        input.health_of = [self](u32 id, u32 gen, f32& current, f32& maximum) {
            const HealthComponent* h = self->health_of(ecs::Entity{id, gen});
            if (h == nullptr) {
                current = 0.0f;
                maximum = 0.0f;
                return;
            }
            current = h->health;
            maximum = h->max_health;
        };
        input.damage = [self](u32 id, u32 gen, f32 amount) {
            return self->apply_health_damage(ecs::Entity{id, gen}, amount);
        };
        input.apply_status = [self](u32 id, u32 gen, std::string_view kind, f32 dps,
                                    f32 seconds, f32 tick) {
            return self->apply_status(ecs::Entity{id, gen}, kind, dps, seconds, tick);
        };
        input.status_remaining = [self](u32 id, u32 gen, std::string_view kind) {
            return self->status_remaining(ecs::Entity{id, gen}, kind);
        };
        input.clear_status = [self](u32 id, u32 gen, std::string_view kind) {
            return self->clear_status(ecs::Entity{id, gen}, kind);
        };
        input.last_attacker = [self](u32 id, u32 gen, u32& attacker_id, u32& attacker_gen) {
            const DamageLedgerComponent* ledger =
                self->damage_ledger_of(ecs::Entity{id, gen});
            if (ledger == nullptr || !ledger->ledger.has_damage ||
                !ledger->ledger.last_attacker.valid()) {
                return false;
            }
            attacker_id = ledger->ledger.last_attacker.id;
            attacker_gen = ledger->ledger.last_attacker.generation;
            return true;
        };
        input.killer = [self](u32 id, u32 gen, u32& killer_id, u32& killer_gen) {
            const DamageLedgerComponent* ledger =
                self->damage_ledger_of(ecs::Entity{id, gen});
            if (ledger == nullptr || !ledger->ledger.killed ||
                !ledger->ledger.killer.valid()) {
                return false;
            }
            killer_id = ledger->ledger.killer.id;
            killer_gen = ledger->ledger.killer.generation;
            return true;
        };
        input.damage_taken = [self](u32 id, u32 gen, f32& health, f32& armor, u32& hits,
                                    f32& seconds) {
            const DamageLedgerComponent* ledger =
                self->damage_ledger_of(ecs::Entity{id, gen});
            if (ledger == nullptr) {
                return;
            }
            health = ledger->ledger.health_damage;
            armor = ledger->ledger.armor_damage;
            hits = ledger->ledger.hits;
            seconds = ledger->ledger.seconds_since_damage;
        };
        input.suppression = [self](u32 id, u32 gen) {
            return self->suppression_of(ecs::Entity{id, gen});
        };
    }
    m_scripts->set_input_source(input);
    m_scripts->update(world, dt);
    m_script_updates += runnable;
    return static_cast<u32>(runnable);
}

size_t Runtime::script_count() const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    return m_scene_data_ptr->scene->world().query<scripting::ScriptComponent>().size();
}

size_t Runtime::scripts_runnable_count() const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    const auto& world = m_scene_data_ptr->scene->world();
    size_t runnable = 0;
    for (auto e : world.query<scripting::ScriptComponent>()) {
        const auto* comp = world.get<scripting::ScriptComponent>(e);
        if (comp != nullptr && comp->enabled && !comp->source.empty()) {
            ++runnable;
        }
    }
    return runnable;
}

void Runtime::clear_script_cache() {
    if (m_scripts) {
        m_scripts->clear_cache();
    }
}

void Runtime::resolve_scene_scripts() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<scripting::ScriptComponent>()) {
        auto* comp = world.get<scripting::ScriptComponent>(e);
        if (comp == nullptr || comp->path.empty() || !comp->source.empty()) {
            continue;
        }
        auto bytes = m_vfs.read_bytes(comp->path);
        if (!bytes.ok) {
            NF_LOG_WARN(LogCategory::Core, "Runtime: script '{}' not found in VFS; the entity will run nothing",
                        comp->path);
            continue;
        }
        comp->source.assign(reinterpret_cast<const char*>(bytes.value.data()), bytes.value.size());
    }
}

void Runtime::build_scene_particles() {
    m_particles.clear();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<vfx::ParticleComponent>()) {
        const auto* comp = world.get<vfx::ParticleComponent>(e);
        const auto* transform = world.get<scene::Transform>(e);
        if (comp == nullptr || transform == nullptr || !comp->enabled) {
            continue;
        }
        // The emitter follows its entity: the authored origin is the local
        // offset, the transform is where it stands in the world.
        vfx::EmitterConfig cfg = comp->config;
        cfg.origin.x += transform->local_x;
        cfg.origin.y += transform->local_y;
        cfg.origin.z += transform->local_z;
        m_particles.emplace(e, vfx::ParticleSystem(cfg));
    }
}

void Runtime::build_scene_cloths() {
    m_cloths.clear();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<physics::ClothComponent>()) {
        const auto* comp = world.get<physics::ClothComponent>(e);
        const auto* transform = world.get<scene::Transform>(e);
        if (comp == nullptr || transform == nullptr || !comp->enabled) {
            continue;
        }
        // Sheet placement comes from the entity: the config's origin is kept
        // as the authored local offset, exactly like particle emitters.
        physics::ClothConfig cfg = comp->config;
        cfg.origin.x += transform->local_x;
        cfg.origin.y += transform->local_y;
        cfg.origin.z += transform->local_z;
        auto cloth = std::make_unique<physics::Cloth>(cfg);
        m_cloths.emplace(e, std::move(cloth));
    }
}

void Runtime::build_scene_audio() {
    // The scene owns its audio environment, so the previous scene's zones are
    // dropped and its theme/bed are stopped unless this scene names them. A
    // level change must not leave the old cave echoing under the new one.
    m_audio_scene.clear_zones();
    m_scene_music_buffer = audio::AudioBuffer{};
    m_scene_ambience_buffer = audio::AudioBuffer{};
    bool music_from_scene = false;
    bool ambience_from_scene = false;

    if (m_scene_data_ptr && m_scene_data_ptr->scene) {
        auto& world = m_scene_data_ptr->scene->world();

        // Reverb zones: the zone's position is the ENTITY's, so placing the
        // object places the echo. A zone on an entity with no transform sits at
        // the origin, which is the same thing a default Transform would give.
        for (auto e : world.query<audio::ReverbZoneComponent>()) {
            const auto* rz = world.get<audio::ReverbZoneComponent>(e);
            if (rz == nullptr || !rz->enabled) {
                continue;
            }
            const auto* t = world.get<scene::Transform>(e);
            audio::ReverbZone zone{};
            if (t != nullptr) {
                zone.position = Vec3{t->world_x, t->world_y, t->world_z};
            }
            zone.radius = rz->radius;
            zone.inner_radius = rz->inner_radius;
            zone.wet_gain = rz->wet_gain;
            zone.decay_seconds = rz->decay_seconds;
            zone.pre_delay_seconds = rz->pre_delay_seconds;
            zone.echo_spacing_seconds = rz->echo_spacing_seconds;
            m_audio_scene.add_zone(zone);
        }

        // One theme per level: the first enabled component that resolved a
        // buffer wins. Order is the world's entity order, which is insertion
        // order, so it is deterministic.
        for (auto e : world.query<audio::MusicComponent>()) {
            const auto* mus = world.get<audio::MusicComponent>(e);
            if (mus == nullptr || !mus->enabled || mus->resolved_buffer() == nullptr) {
                continue;
            }
            m_scene_music_buffer = *mus->resolved_buffer();
            audio::MusicTrack track{};
            track.buffer = &m_scene_music_buffer;
            track.base_volume = mus->volume;
            m_audio_scene.music().play(track, std::max(0.0f, mus->fade_in_seconds));
            music_from_scene = true;
            break;
        }
        for (auto e : world.query<audio::AmbienceComponent>()) {
            const auto* amb = world.get<audio::AmbienceComponent>(e);
            if (amb == nullptr || !amb->enabled || amb->resolved_buffer() == nullptr) {
                continue;
            }
            m_scene_ambience_buffer = *amb->resolved_buffer();
            m_audio_scene.music().set_ambience(&m_scene_ambience_buffer,
                                               std::max(0.0f, amb->fade_in_seconds));
            ambience_from_scene = true;
            break;
        }
    }

    // Nothing named it, so nothing should be playing. A game that wants music
    // across a level change starts it again after the load — the alternative is
    // the previous level's theme surviving into one that never asked for it.
    if (!music_from_scene) {
        m_audio_scene.music().stop(0.0f);
    }
    if (!ambience_from_scene) {
        m_audio_scene.music().clear_ambience(0.0f);
    }
}

void Runtime::bind_scene_feedback() {
    // A scene change ends the old fight: its flashes, tracers, scorch marks,
    // pending cues and live voices all belong to the level that produced them.
    // configure({}) puts the director back on its defaults too, so a scene with
    // no Feedback line cannot inherit the previous scene's tuning.
    m_feedback.reset();
    m_feedback.configure(runtime::CombatFeedbackConfig{});
    m_feedback_bound = false;
    m_feedback_pending.clear();
    m_feedback_voices.clear();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<FeedbackComponent>()) {
        const auto* fb = world.get<FeedbackComponent>(e);
        if (fb == nullptr || !fb->enabled) {
            continue;
        }
        runtime::CombatFeedbackConfig config;
        config.flash_seconds = fb->flash_seconds;
        config.flash_size = fb->flash_size;
        config.tracer_seconds = fb->tracer_seconds;
        config.tracer_length = fb->tracer_length;
        config.tracer_width = fb->tracer_width;
        config.impact_seconds = fb->impact_seconds;
        config.impact_size = fb->impact_size;
        config.shot_volume = fb->shot_volume;
        config.impact_volume = fb->impact_volume;
        config.audio = fb->audio;
        // Phase 59: the blast flash, the per-channel gains, the muzzle light and
        // all five caps are scene data now.
        config.blast_flash_seconds = fb->blast_flash_seconds;
        config.blast_flash_size = fb->blast_flash_size;
        config.impact_volume_explosive = fb->impact_volume_explosive;
        config.impact_volume_fire = fb->impact_volume_fire;
        config.flash_light = fb->flash_light;
        config.flash_light_intensity = fb->flash_light_intensity;
        config.flash_light_radius = fb->flash_light_radius;
        config.flash_light_color =
            Vec3{fb->flash_light_r, fb->flash_light_g, fb->flash_light_b};
        config.max_flashes = static_cast<u32>(fb->max_flashes);
        config.max_tracers = static_cast<u32>(fb->max_tracers);
        config.max_impacts = static_cast<u32>(fb->max_impacts);
        config.max_sounds = static_cast<u32>(fb->max_sounds);
        config.max_lights = static_cast<u32>(fb->max_lights);
        m_feedback.configure(config);
        m_feedback_bound = true;
        break;
    }
}

u32 Runtime::step_particles(float dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    auto& world = m_scene_data_ptr->scene->world();
    u32 stepped = 0;
    for (auto& [entity, system] : m_particles) {
        if (!world.is_alive(entity)) {
            continue;
        }
        const auto* comp = world.get<vfx::ParticleComponent>(entity);
        const auto* transform = world.get<scene::Transform>(entity);
        if (comp == nullptr || transform == nullptr || !comp->enabled) {
            continue;
        }
        vfx::EmitterConfig cfg = comp->config;
        cfg.origin.x += transform->local_x;
        cfg.origin.y += transform->local_y;
        cfg.origin.z += transform->local_z;
        system.set_config(cfg);
        system.update(dt);
        ++stepped;
    }
    return stepped;
}

u32 Runtime::step_cloth(float dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    auto& world = m_scene_data_ptr->scene->world();
    // Colliders are re-submitted every step: a collider driven by a rigid
    // body moves, and the cloth module's contract is that moving colliders
    // are re-submitted (its AABBs are hoisted at set_colliders time).
    std::vector<physics::ClothCollider> colliders;
    for (auto e : world.query<physics::ColliderComponent>()) {
        const auto* col = world.get<physics::ColliderComponent>(e);
        const auto* transform = world.get<scene::Transform>(e);
        if (col == nullptr || transform == nullptr) {
            continue;
        }
        physics::ClothCollider cc;
        cc.shape = col->shape;
        cc.position = Vec3(transform->local_x, transform->local_y, transform->local_z);
        cc.orientation =
            scene::quat_from_euler_xyz_degrees(transform->rot_x, transform->rot_y, transform->rot_z);
        colliders.push_back(cc);
    }
    u32 stepped = 0;
    for (auto& [entity, cloth] : m_cloths) {
        if (cloth == nullptr || !world.is_alive(entity)) {
            continue;
        }
        const auto* comp = world.get<physics::ClothComponent>(entity);
        if (comp == nullptr || !comp->enabled) {
            continue;
        }
        cloth->set_colliders(colliders);
        cloth->step(dt);
        ++stepped;
    }
    return stepped;
}

u32 Runtime::step_ai_world(float dt) {
    if (!m_ai_world || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    m_ai_world->plan(dt);
    u32 ticking = 0;
    for (const ai::AIActor& actor : m_ai_world->actors()) {
        if (m_ai_world->wants_tick(actor.id)) {
            ++ticking;
        }
    }
    return ticking;
}

// --- Navigation (Phase 28) ---------------------------------------------------
//
// The bake reads three things from the scene and invents nothing else:
//   * the volume's own Transform origin as the area's minimum corner,
//   * a FLAT floor sampler at the volume's floor height — terrain is not
//     scene-authorable, so a sampler that invented hills would describe a
//     world the physics does not simulate, and
//   * the scene's static box colliders as obstacles.
// A richer sampler is a caller's extension through ai::NavMesh::build (it takes
// any callable); this is the honest v1 a scene can author.

void Runtime::build_scene_navmesh() {
    // The scene owns its navigation: dropping the previous scene's mesh and
    // walkers is the same rule the particle, cloth and audio-environment
    // builders follow.
    m_navmesh.reset();
    m_nav_agents.clear();
    m_nav_obstacle_count = 0;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();

    ecs::Entity volume = ecs::kInvalidEntity;
    const NavMeshComponent* spec = nullptr;
    for (auto e : world.query<NavMeshComponent>()) {
        const auto* comp = world.get<NavMeshComponent>(e);
        if (comp != nullptr && comp->enabled) {
            volume = e;
            spec = comp;
            break;
        }
    }
    if (spec == nullptr || volume == ecs::kInvalidEntity) {
        NF_LOG_INFO(LogCategory::Core,
                    "Runtime: scene carries no enabled navigation volume; no navmesh baked");
        return;
    }

    const auto* vt = world.get<scene::Transform>(volume);
    const Vec3 origin =
        vt != nullptr ? Vec3{vt->local_x, vt->local_y, vt->local_z} : Vec3{};
    const Vec3 area_min = origin;
    const Vec3 area_max{origin.x + spec->area_x, origin.y + spec->area_y,
                        origin.z + spec->area_z};

    std::vector<ai::NavObstacle> obstacles;
    for (auto e : world.query<physics::ColliderComponent>()) {
        if (e == volume) {
            continue; // the volume is not an obstacle to itself
        }
        const auto* col = world.get<physics::ColliderComponent>(e);
        const auto* rb = world.get<physics::RigidBodyComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (col == nullptr || t == nullptr) {
            continue;
        }
        // Static scenery only: a dynamic body is simulated by the engine
        // solver, and an obstacle that moves every frame would need a re-bake
        // to mean anything (callers get that through set_obstacles on the mesh).
        if (rb != nullptr && rb->type != physics::BodyType::Static) {
            continue;
        }
        // A walker is not its own wall — an agent's collider would carve a hole
        // exactly where it stands.
        if (world.has<NavAgentComponent>(e)) {
            continue;
        }
        // The voxeliser is AABB-based; other shapes are skipped rather than
        // approximated, because a sphere baked as its bounding box is a wall
        // that is wider than the thing an author placed and can see.
        if (col->shape.type != physics::ShapeType::Box) {
            continue;
        }
        const Vec3 centre{t->local_x, t->local_y, t->local_z};
        const Vec3 half{col->shape.box.half_extents.x * t->scale_x,
                        col->shape.box.half_extents.y * t->scale_y,
                        col->shape.box.half_extents.z * t->scale_z};
        ai::NavObstacle obs;
        obs.min = Vec3{centre.x - half.x, centre.y - half.y, centre.z - half.z};
        obs.max = Vec3{centre.x + half.x, centre.y + half.y, centre.z + half.z};
        obstacles.push_back(obs);
    }

    ai::NavMesh::Config cfg;
    cfg.cell_size = spec->cell_size;
    cfg.cell_height = spec->cell_height;
    cfg.walkable_slope_deg = spec->walkable_slope_deg;
    cfg.walkable_climb = spec->walkable_climb;
    cfg.walkable_height = spec->walkable_height;
    cfg.min_region_area = spec->min_region_area;
    cfg.agent_radius = spec->agent_radius;
    cfg.jump_distance = spec->jump_distance;
    cfg.jump_height = spec->jump_height;
    cfg.max_verts_per_poly = spec->max_verts_per_poly;

    const f32 floor_y = area_min.y;
    auto sampler = [floor_y](f32, f32) { return floor_y; };

    auto mesh = std::make_unique<ai::NavMesh>();
    if (!mesh->build(cfg, area_min, area_max, sampler, obstacles)) {
        NF_LOG_ERROR(LogCategory::Core,
                     "Runtime: navigation bake failed for the volume at ({}, {}, {}) "
                     "with extents ({}, {}, {}) — check the area, the cell size and the floor",
                     origin.x, origin.y, origin.z, spec->area_x, spec->area_y, spec->area_z);
        return;
    }
    // Bumped only on success, and before the agents bind, so a freshly bound
    // agent's path is stamped with the bake it was planned against.
    ++m_nav_generation;
    m_navmesh = std::move(mesh);
    m_nav_obstacle_count = obstacles.size();

    for (auto e : world.query<NavAgentComponent>()) {
        const auto* comp = world.get<NavAgentComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (comp == nullptr || t == nullptr || !comp->enabled) {
            continue;
        }
        NavAgentState state;
        state.goal = Vec3{comp->goal_x, comp->goal_y, comp->goal_z};
        state.mesh_generation = m_nav_generation;
        m_nav_agents.emplace(e, std::move(state));
    }

    NF_LOG_INFO(LogCategory::Core,
                "Runtime: navigation baked {} polygons / {} regions from {} obstacles "
                "({} agents bound)",
                m_navmesh->polygon_count(), m_navmesh->region_count(), obstacles.size(),
                m_nav_agents.size());
}

u32 Runtime::step_navmesh(float dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || m_navmesh == nullptr ||
        !m_navmesh->built()) {
        return 0;
    }
    // The edit-mode freeze: a viewport that is not playing shows a posed crowd,
    // never a self-walking one. update() already returns early on dt <= 0, so
    // this guard is for a direct caller.
    if (!(dt > 0.0f)) {
        return 0;
    }
    auto& world = m_scene_data_ptr->scene->world();
    // Re-path tolerance: how far off its corridor an agent may sit before its
    // path is replanned from where it now stands. A cell, so a waypoint the
    // agent is still walking towards never reads as "dragged away".
    const f32 repath_tolerance = std::max(m_navmesh->cell_size(), 0.05f);

    u32 moved = 0;
    for (auto e : world.query<NavAgentComponent>()) {
        auto* comp = world.get<NavAgentComponent>(e);
        auto* t = world.get<scene::Transform>(e);
        if (comp == nullptr || t == nullptr || !comp->enabled) {
            continue;
        }
        auto it = m_nav_agents.find(e);
        if (it == m_nav_agents.end()) {
            continue; // bound at bake time; an agent added later re-bakes
        }
        NavAgentState& st = it->second;
        const Vec3 start{t->local_x, t->local_y, t->local_z};
        const Vec3 goal{comp->goal_x, comp->goal_y, comp->goal_z};

        bool repath = !st.has_path || st.path.size() < 2 ||
                      !st.goal.nearly_equals(goal, 1e-4f) ||
                      st.mesh_generation != m_nav_generation;
        if (!repath) {
            // Off the corridor? Then the agent was moved by something other
            // than this step (an editor drag, a script teleport), and the old
            // path starts somewhere the agent is not.
            const Vec3 a = st.path[st.waypoint > 0 ? st.waypoint - 1 : 0];
            const Vec3 b = st.path[std::min(st.waypoint, st.path.size() - 1)];
            if (xz_distance_to_segment(start, a, b) > repath_tolerance) {
                repath = true;
            }
        }
        if (repath) {
            st.at_goal = false;
            std::vector<Vec3> path;
            if (m_navmesh->find_path(start, goal, path) && path.size() >= 2) {
                st.path = std::move(path);
                st.waypoint = 1; // waypoint 0 is where the agent already stands
                st.goal = goal;
                st.has_path = true;
                st.mesh_generation = m_nav_generation;
            } else {
                // Off the mesh or unreachable: hold position and keep the goal,
                // so walking back onto the volume resumes without a re-bake.
                st.path.clear();
                st.has_path = false;
                st.goal = goal;
                continue;
            }
        }
        if (st.at_goal) {
            continue;
        }

        const Vec3 target = st.path[st.waypoint];
        const Vec3 delta = target - start;
        const f32 dist = delta.length();
        const f32 step_len = comp->speed * dt;
        if (dist <= std::max(step_len, comp->arrive_radius)) {
            // Snap to the waypoint rather than stepping past it: overshooting
            // would cut a corner the corridor was built to avoid.
            t->local_x = target.x;
            t->local_y = target.y;
            t->local_z = target.z;
            ++st.waypoint;
            if (st.waypoint >= st.path.size()) {
                st.at_goal = true;
            }
        } else if (step_len > 0.0f) {
            const f32 inv = 1.0f / dist;
            t->local_x = start.x + delta.x * inv * step_len;
            t->local_y = start.y + delta.y * inv * step_len;
            t->local_z = start.z + delta.z * inv * step_len;
        } else {
            continue; // speed 0 and not there yet: parked, not arrived
        }
        t->dirty = true;
        ++moved;
    }
    return moved;
}

void Runtime::bind_scene_vehicles() {
    // Teardown first, unconditionally: the system before the world (its
    // JoltVehicles point into it), the static handles before the world (same
    // order destruction tears down in). Stale component views are cleared
    // with the bindings — a view of a dead JoltVehicle is exactly the
    // dangling-view bug VehicleSystem's ownership rules exist to prevent.
    m_vehicle_system.reset();
    if (m_vehicle_world) {
        for (const physics::JoltBody& body : m_vehicle_statics) {
            m_vehicle_world->remove_body(body);
        }
        for (const VehicleObstacle& obstacle : m_vehicle_obstacles) {
            m_vehicle_world->remove_body(obstacle.mirror);
        }
    }
    m_vehicle_statics.clear();
    m_vehicle_obstacles.clear();
    m_vehicle_world.reset();
    m_vehicle_clock.reset();
    // Spin belongs to the binding, not the scene: a wheel that spun carrying one
    // world's simulation must not surface in the next one, and a fresh bind
    // starts visuals from parked. (Like the clock: reset on every path.)
    m_vehicle_wheel_spin.clear();
    m_wheel_mesh = rendering::StaticMeshHandle{};
    // Same for replication state: a session's ids and tick stream end here.
    m_vehicle_net_ids.clear();
    m_vehicle_net_ticks.clear();
    if (m_scene_data_ptr && m_scene_data_ptr->scene) {
        auto& w = m_scene_data_ptr->scene->world();
        for (auto e : w.query<physics::VehicleComponent>()) {
            if (auto* vc = w.get<physics::VehicleComponent>(e)) {
                vc->vehicle = nullptr;
            }
        }
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    bool any_enabled = false;
    for (auto e : world.query<physics::VehicleComponent>()) {
        const auto* vc = world.get<physics::VehicleComponent>(e);
        if (vc != nullptr && vc->enabled) {
            any_enabled = true;
            break;
        }
    }
    if (!any_enabled) {
        return; // no car: no Jolt world, no cost, no statics
    }
    auto jolt = std::make_unique<physics::JoltWorld>();
    if (!jolt->valid()) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: vehicles disabled, Jolt world unavailable");
        return;
    }
    // Level geometry the cars drive on: the scene's STATIC rigid bodies, in
    // world space (propagated first, so parented platforms land correctly).
    // Bodies the ENGINE SOLVER simulates (Dynamic, Kinematic) stay with the
    // engine — a body simulated twice is two bodies disagreeing about where it
    // is (same rule as the debris statics). What they get here instead is a
    // KINEMATIC MIRROR (below): a solid obstacle the car collides with, driven
    // from the solver's pose, integrating no motion of its own. Entities
    // carrying a VehicleComponent are skipped: the chassis is its own Jolt
    // body, and a second shell on top of it is a wall the car can never leave.
    scene::propagate_transforms(world);
    for (auto e : world.query<physics::RigidBodyComponent>()) {
        const auto* rb = world.get<physics::RigidBodyComponent>(e);
        const auto* collider = world.get<physics::ColliderComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (rb == nullptr || collider == nullptr || t == nullptr) {
            continue;
        }
        if (world.has<physics::VehicleComponent>(e)) {
            continue;
        }
        if (rb->type == physics::BodyType::Static) {
            physics::BodyDesc desc;
            desc.type = physics::BodyType::Static;
            desc.shape = collider->shape;
            desc.position = Vec3(t->world_x, t->world_y, t->world_z);
            desc.orientation = scene::quat_from_euler_xyz_degrees(t->rot_x, t->rot_y, t->rot_z);
            const physics::JoltBody body = jolt->add_body(desc);
            if (body.valid()) {
                m_vehicle_statics.push_back(body);
            }
            continue;
        }
        // Dynamic / Kinematic scene bodies: solid moving obstacles. The pose
        // AUTHORITY is the engine solver's body (rb->body), not the Transform:
        // a mirror spawned from a Transform that the solver has already moved
        // starts a frame behind, opening a one-step tunnel the car can slip
        // through. Bodies the solver never got (rebuild failed or ran without
        // physics) have no pose authority, so they mirror nothing.
        if (!rb->body.valid() || !m_physics) {
            continue;
        }
        const physics::BodyState state = m_physics->state(rb->body);
        physics::BodyDesc desc;
        desc.type = physics::BodyType::Kinematic;
        desc.shape = collider->shape;
        desc.position = state.position;
        desc.orientation = state.orientation;
        desc.friction = rb->friction;
        desc.restitution = rb->restitution;
        const physics::JoltBody mirror = jolt->add_body(desc);
        if (mirror.valid()) {
            m_vehicle_obstacles.push_back(VehicleObstacle{rb->body, mirror});
        }
    }
    m_vehicle_world = std::move(jolt);
    m_vehicle_system = std::make_unique<physics::VehicleSystem>(*m_vehicle_world);
    m_vehicle_system->startup(world);
    NF_LOG_INFO(LogCategory::Core,
                "Runtime: vehicles bound ({} cars, {} static colliders, {} obstacle mirrors)",
                m_vehicle_system->vehicle_count(), m_vehicle_statics.size(),
                m_vehicle_obstacles.size());
}

u32 Runtime::step_vehicles(float dt) {
    if (!m_vehicle_world || !m_vehicle_system || !m_vehicle_world->valid() ||
        !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    // The edit-mode freeze: a viewport that is not playing shows parked cars,
    // never self-driving ones. update() already returns early on dt <= 0, so
    // this guard is for a direct caller.
    if (!(dt > 0.0f)) {
        return 0;
    }
    auto& world = m_scene_data_ptr->scene->world();
    const u32 steps = m_vehicle_clock.advance(dt);
    for (u32 i = 0; i < steps; ++i) {
        const float h = m_vehicle_clock.step();
        // Obstacle mirrors follow their solver bodies BEFORE the step: the car
        // contacts what the engine world says is true now, at the pose the
        // solver reports after its own step (step_physics precedes
        // step_vehicles in update()), so a crate falling this frame is solid
        // exactly where it landed. Velocities ride along so contacts push the
        // car with the obstacle's motion instead of treating a translating
        // slab as a teleporting wall. One-way: nothing here writes back into
        // m_physics, so the solver's determinism is untouched.
        if (m_physics) {
            for (const VehicleObstacle& obstacle : m_vehicle_obstacles) {
                const physics::BodyState src = m_physics->state(obstacle.source);
                m_vehicle_world->set_pose(obstacle.mirror, src.position, src.orientation);
                m_vehicle_world->set_linear_velocity(obstacle.mirror, src.linear_velocity);
            }
        }
        // Drive inputs in, then Jolt solves — the VehicleDemo's own protocol
        // (update() before step()).
        m_vehicle_system->update(world);
        m_vehicle_world->step(h);
        // Wheel-spin integration, on the simulation clock: Jolt exposes the
        // wheels' angular velocity but never an accumulated angle, so the roll
        // the render world shows is integrated here with the SAME h each step
        // was solved at. Integrate against the states AFTER the step: one step
        // of lag on a visual is invisible; spinning before Jolt moved the
        // bodies would let the wheel appear to roll where the chassis is not.
        for (auto e : world.query<physics::VehicleComponent>()) {
            const auto* vc = world.get<physics::VehicleComponent>(e);
            if (vc == nullptr || vc->vehicle == nullptr) {
                continue;
            }
            WheelSpin& spin = m_vehicle_wheel_spin[e];
            const std::vector<physics::JoltWheelState> states =
                vc->vehicle->wheel_states();
            for (usize w = 0; w < states.size() && w < spin.angle.size(); ++w) {
                spin.angle[w] += states[w].angular_velocity * h;
            }
        }
    }
    if (steps == 0) {
        return 0;
    }
    // One trailing sync so the Transforms render where the last step ended up,
    // not one step behind it. drive() only arms driver inputs, so re-applying
    // them here is idempotent — the poses are what this pass is for.
    m_vehicle_system->update(world);
    return steps;
}

bool Runtime::reset_vehicle(ecs::Entity e) {
    if (!m_vehicle_system || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    auto& world = m_scene_data_ptr->scene->world();
    if (!world.is_alive(e)) {
        return false;
    }
    const auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        return false;
    }
    // In place: upright where it stands. The demo resets to a remembered
    // spawn, but the scene persists no spawn pose — the Transform is the only
    // placement the format knows.
    m_vehicle_system->reset(e, world, Vec3{t->local_x, t->local_y, t->local_z});
    return true;
}

size_t Runtime::vehicle_count() const {
    return m_vehicle_system ? m_vehicle_system->vehicle_count() : 0u;
}

size_t Runtime::vehicle_obstacle_count() const {
    return m_vehicle_obstacles.size();
}

// --- Vehicle network replication (Step 14) ------------------------------------

void Runtime::assign_vehicle_net_id(ecs::Entity e, u32 net_id) {
    // Net id 0 is unwireable (decode_vehicle_snapshot rejects it), so refusing
    // the assignment here keeps collect/apply from ever minting a record the
    // format itself would bounce later.
    if (!e.valid() || net_id == 0u) {
        return;
    }
    m_vehicle_net_ids[e] = net_id;
}

bool Runtime::collect_vehicle_net_states(std::vector<VehicleNetRecord>& out) const {
    out.clear();
    if (!m_vehicle_world || !m_vehicle_system || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false; // no bind: nothing in this world is authoritative
    }
    const auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<physics::VehicleComponent>()) {
        const auto* vc = world.get<physics::VehicleComponent>(e);
        if (vc == nullptr || vc->vehicle == nullptr || !world.is_alive(e)) {
            continue;
        }
        const auto it = m_vehicle_net_ids.find(e);
        if (it == m_vehicle_net_ids.end()) {
            continue; // a car without a wire identity is not replicated
        }
        VehicleNetRecord rec;
        rec.net_id = it->second;
        rec.state = vc->vehicle->chassis_state();
        out.push_back(rec);
    }
    return true;
}

u32 Runtime::apply_vehicle_net_states(const std::vector<VehicleNetRecord>& in, u32 tick) {
    u32 applied = 0;
    // Tick 0 is "nothing has stepped yet" on every side of the protocol
    // (snapshot tick defaults to 0) — it can never be a NEWER correction.
    if (tick == 0u || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (const VehicleNetRecord& rec : in) {
        if (rec.net_id == 0u) {
            continue;
        }
        // Assignments are entity -> net id; apply walks the other way. Car
        // counts are table-small, so the reverse scan is free.
        ecs::Entity target = ecs::kInvalidEntity;
        for (const auto& [entity, net] : m_vehicle_net_ids) {
            if (net == rec.net_id && world.is_alive(entity)) {
                target = entity;
                break;
            }
        }
        if (!target.valid()) {
            continue;
        }
        auto* vc = world.get<physics::VehicleComponent>(target);
        if (vc == nullptr || vc->vehicle != nullptr) {
            // Not a ghost here: a car this world simulates writes its own pose
            // every step, and foreign authority arriving through a packet
            // would fight it to a visible standstill.
            continue;
        }
        u32& last = m_vehicle_net_ticks[target];
        if (last >= tick) {
            continue; // stale or duplicate correction
        }
        auto* tf = world.get<scene::Transform>(target);
        if (tf == nullptr) {
            continue;
        }
        // Exactly the writeback VehicleSystem::update does for a locally
        // simulated chassis: position into the transform's local slot, heading
        // into euler degrees. The renderer only reads transforms, so a remote
        // car and a local one are indistinguishable two frames later.
        tf->local_x = rec.state.position.x;
        tf->local_y = rec.state.position.y;
        tf->local_z = rec.state.position.z;
        scene::euler_xyz_degrees_from_quat(rec.state.rotation, tf->rot_x, tf->rot_y, tf->rot_z);
        tf->dirty = true;
        last = tick;
        ++applied;
    }
    return applied;
}

bool Runtime::vehicle_wheel_states(ecs::Entity e, std::vector<physics::JoltWheelState>& out) const {
    out.clear();
    if (!m_vehicle_system || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    const auto& world = m_scene_data_ptr->scene->world();
    if (!world.is_alive(e)) {
        return false;
    }
    const auto* vc = world.get<physics::VehicleComponent>(e);
    if (vc == nullptr || vc->vehicle == nullptr) {
        return false;
    }
    out = vc->vehicle->wheel_states();
    return true;
}

std::vector<Vec3> nav_debug_mesh_edge_points(const ai::NavMesh& mesh) {
    std::vector<Vec3> out;
    const u32 n = mesh.polygon_count();
    for (u32 i = 0; i < n; ++i) {
        const ai::NavPoly& poly = mesh.polygon(i);
        const usize vcount = poly.verts.size();
        // A polygon with fewer than 3 vertices is degenerate input the
        // polygoniser should never have produced; skipping it keeps a corrupt
        // bake from drawing nonsense rather than crashing on wraparound.
        if (vcount < 3) {
            continue;
        }
        for (usize v = 0; v < vcount; ++v) {
            out.push_back(poly.verts[v]);
            out.push_back(poly.verts[(v + 1) % vcount]);
        }
    }
    return out;
}

NavDebugSnapshot Runtime::nav_debug_snapshot() const {
    NavDebugSnapshot snap;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || m_navmesh == nullptr ||
        !m_navmesh->built()) {
        return snap; // no volume: the overlay draws nothing, not stale data
    }
    snap.has_mesh = true;
    snap.mesh_generation = m_nav_generation;
    const u32 n = m_navmesh->polygon_count();
    snap.mesh_polygons.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        snap.mesh_polygons.push_back(m_navmesh->polygon(i).verts);
    }
    const auto& world = m_scene_data_ptr->scene->world();
    snap.agents.reserve(m_nav_agents.size());
    for (const auto& [entity, st] : m_nav_agents) {
        NavDebugAgent a;
        a.entity = entity;
        if (const auto* t = world.get<scene::Transform>(entity); t != nullptr) {
            a.position = Vec3{t->local_x, t->local_y, t->local_z};
        }
        if (const auto* comp = world.get<NavAgentComponent>(entity); comp != nullptr) {
            a.goal = Vec3{comp->goal_x, comp->goal_y, comp->goal_z};
        }
        // The route from the waypoint currently walked to: the travelled legs
        // are history, and drawing them would paint the path the agent has
        // already consumed as if it still had to walk it.
        if (st.has_path && !st.path.empty()) {
            const usize from = std::min(st.waypoint > 0 ? st.waypoint - 1 : usize{0}, st.path.size() - 1);
            a.path.assign(st.path.begin() + static_cast<std::ptrdiff_t>(from), st.path.end());
        }
        a.has_path = st.has_path && !a.path.empty();
        a.at_goal = st.at_goal;
        snap.agents.push_back(std::move(a));
    }
    // The live map is unordered: sort by entity id, the one stable key every
    // agent has, so the overlay draws — and a test reads — the same order
    // every frame. (The destruction retire list is sorted for the same reason.)
    std::sort(snap.agents.begin(), snap.agents.end(), [](const NavDebugAgent& a, const NavDebugAgent& b) {
        if (a.entity.id != b.entity.id) return a.entity.id < b.entity.id;
        return a.entity.generation < b.entity.generation;
    });
    return snap;
}

size_t Runtime::particles_alive() const {
    size_t alive = 0;
    for (const auto& [entity, system] : m_particles) {
        (void)entity;
        alive += system.alive();
    }
    return alive;
}

bool Runtime::character_grounded(ecs::Entity e) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    const auto* comp = m_scene_data_ptr->scene->world().get<physics::CharacterComponent>(e);
    return comp != nullptr && comp->grounded;
}

const vfx::ParticleSystem* Runtime::particle_system(ecs::Entity e) const {
    const auto it = m_particles.find(e);
    return it != m_particles.end() ? &it->second : nullptr;
}

const physics::Cloth* Runtime::cloth(ecs::Entity e) const {
    const auto it = m_cloths.find(e);
    return (it != m_cloths.end()) ? it->second.get() : nullptr;
}

void Runtime::capture_gameplay_state() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) return;
    ecs::World& world = m_scene_data_ptr->scene->world();

    for (GameplaySlot& slot : m_gameplay_modules) {
        const gameplay::GameplayStateBinding binding = slot.module->state();
        if (!binding.valid()) continue;

        std::unordered_map<std::string, std::string> properties;
        gameplay::capture_state(binding, properties);

        ecs::Entity owner = find_gameplay_owner(slot.module->name());
        if (!owner.valid()) {
            // A module added at runtime has no component yet. Create one so its
            // state has somewhere to live, otherwise the save would silently
            // drop it.
            owner = world.create_entity();
            world.add<gameplay::GameplayModuleComponent>(
                owner, gameplay::GameplayModuleComponent{std::string(slot.module->name()), {}, true});
            world.add<scene::NameComponent>(
                owner, scene::NameComponent{std::string(slot.module->name()) + " (gameplay)"});
        }
        if (auto* comp = world.get<gameplay::GameplayModuleComponent>(owner)) {
            comp->properties = std::move(properties);
        }
    }
    ++m_scene_version;
}

void Runtime::apply_gameplay_state() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) return;
    const ecs::World& world = m_scene_data_ptr->scene->world();

    for (ecs::Entity e : world.query<gameplay::GameplayModuleComponent>()) {
        const auto* comp = world.get<gameplay::GameplayModuleComponent>(e);
        if (comp == nullptr) continue;

        gameplay::GameplayModule* module = find_gameplay_module(comp->module_name);
        if (module == nullptr) {
            // Not an error: a scene authored with a module this build does not
            // have keeps its data and simply does not run that module.
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: scene references gameplay module '{}', which is not registered",
                        comp->module_name);
            continue;
        }
        (void)gameplay::apply_state(module->state(), comp->properties);
    }
}

void Runtime::enable_streaming(const std::string& chunk_dir_logical,
                               const scene::StreamingVolume& volume) {
    if (m_streaming_enabled) {
        disable_streaming();
    }
    // ECS component type ids are process-global mutable state on first touch
    // (a function-local static fed by a registering singleton). The chunk
    // parser below builds Worlds on workers now, so every component type it
    // can add is touched here, on the main thread, first. Without this, a
    // first-touch registration on a worker races the main thread's own adds.
    (void)ecs::component_type_id<scene::Transform>();
    (void)ecs::component_type_id<scene::NameComponent>();
    (void)ecs::component_type_id<scene::PrefabLinkComponent>();
    (void)ecs::component_type_id<MeshComponent>();
    (void)ecs::component_type_id<DirectionalLight>();
    (void)ecs::component_type_id<CameraComponent>();
    (void)ecs::component_type_id<physics::RigidBodyComponent>();
    (void)ecs::component_type_id<physics::ColliderComponent>();
    (void)ecs::component_type_id<animation::AnimationComponent>();
    (void)ecs::component_type_id<audio::AudioComponent>();
    (void)ecs::component_type_id<audio::ReverbZoneComponent>();
    (void)ecs::component_type_id<audio::MusicComponent>();
    (void)ecs::component_type_id<audio::AmbienceComponent>();
    (void)ecs::component_type_id<gameplay::GameplayModuleComponent>();
    (void)ecs::component_type_id<scripting::ScriptComponent>();
    (void)ecs::component_type_id<vfx::ParticleComponent>();
    (void)ecs::component_type_id<physics::ClothComponent>();
    (void)ecs::component_type_id<physics::CharacterComponent>();
    m_streaming_chunk_dir = chunk_dir_logical;
    m_streaming_enabled = true;
    m_streamer.set_handlers(
        [this](const scene::ChunkCoord& coord, std::vector<ecs::Entity>& out_created,
               std::string& out_error) { return streaming_load_chunk(coord, out_created, out_error); },
        [this](const scene::ChunkCoord& coord, const std::vector<ecs::Entity>& created) {
            streaming_unload_chunk(coord, created);
        });
    m_streamer.set_volume(volume);
    m_streaming_known_loaded = m_streamer.loaded_chunk_count();
}

void Runtime::disable_streaming() {
    if (!m_streaming_enabled) {
        return;
    }
    m_streamer.clear();
    m_streaming_enabled = false;
    m_streaming_chunk_dir.clear();
    // Abandoned workers touch only their staged block (never the world or
    // `this`), so dropping the map entries is safe; a late completion finds
    // no entry and its scene is simply destroyed with the block.
    m_stream_jobs.clear();
    resync_after_streaming();
}

void Runtime::set_streaming_volume(const scene::StreamingVolume& volume) {
    m_streamer.set_volume(volume);
}

u32 Runtime::step_streaming() {
    if (!m_streaming_enabled || !has_scene()) {
        return 0;
    }
    const u32 loaded = m_streamer.update();
    if (m_streamer.loaded_chunk_count() != m_streaming_known_loaded) {
        resync_after_streaming();
    }
    return loaded;
}

size_t Runtime::stream_in_flight_count() const {
    return m_stream_jobs.size();
}

bool Runtime::stream_coord_still_wanted(const scene::ChunkCoord& coord) const {
    for (const scene::ChunkCoord& c : m_streamer.wanted_chunks()) {
        if (c == coord) {
            return true;
        }
    }
    return false;
}

bool Runtime::streaming_load_chunk(const scene::ChunkCoord& coord,
                                   std::vector<ecs::Entity>& out_created, std::string& out_error) {
    if (!has_scene()) {
        out_error = "No scene loaded";
        return false;
    }
    // Stage one: a finished worker waits for its main-thread commit. False
    // here does NOT mean failure — the streamer retries the coord on a later
    // update, which is exactly the non-blocking contract.
    auto it = m_stream_jobs.find(coord);
    if (it != m_stream_jobs.end()) {
        std::shared_ptr<StreamLoadJob> job = it->second;
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            finished = job->finished;
        }
        if (!finished) {
            return false;
        }
        m_stream_jobs.erase(it);
        if (!job->ok || !job->scene) {
            out_error = job->error.empty() ? ("Cannot load streamed chunk") : job->error;
            return false;
        }
        if (!stream_coord_still_wanted(coord)) {
            // The volume moved on while the worker parsed: the temp scene is
            // destroyed with the job block, nothing reaches the live world.
            return false;
        }
        const std::vector<ecs::Entity> roots =
            merge_loaded_scene_into_world(*job->scene, m_scene_data_ptr->scene->world());
        auto& world = m_scene_data_ptr->scene->world();
        for (ecs::Entity root : roots) {
            if (!world.is_alive(root)) {
                continue;
            }
            std::vector<ecs::Entity> members{root};
            for (size_t i = 0; i < members.size(); ++i) {
                for (ecs::Entity kid : scene::get_children(world, members[i])) {
                    if (world.is_alive(kid)) {
                        members.push_back(kid);
                    }
                }
            }
            out_created.insert(out_created.end(), members.begin(), members.end());
        }
        NF_LOG_INFO(LogCategory::Core, "Runtime: streamed in chunk ({},{},{}) ({} entities)", coord.x,
                    coord.y, coord.z, out_created.size());
        return true;
    }
    // Stage zero: dispatch, bounded by the in-flight cap. Path resolution
    // stays on the main thread (VFS); the worker gets a physical path and
    // parses with plain file IO, touching neither the VFS nor the live
    // world — the ImportQueue split, applied to chunks.
    if (m_stream_jobs.size() >= m_max_stream_in_flight) {
        return false;
    }
    const std::string path = m_streaming_chunk_dir + "/chunk_" + std::to_string(coord.x) + "_" +
                             std::to_string(coord.y) + "_" + std::to_string(coord.z) + ".nfscene";
    auto resolved = m_vfs.resolve(path);
    if (!resolved.ok) {
        out_error = resolved.error;
        return false;
    }
    // Existence is checked HERE, on the main thread, with a cheap stat —
    // not by spending a worker proving the file absent on every update.
    // Resolve only maps the path; it does not promise the file is there, and
    // without this every missing chunk in range would burn a worker slot per
    // update (the common case: a sparse region). A TOCTOU gap remains (the
    // file may vanish before the worker opens it); the worker fails that
    // gracefully, and an appearing file is picked up on a later update.
    std::error_code ec;
    if (!std::filesystem::exists(resolved.value, ec)) {
        out_error = "Chunk file not found: " + path;
        return false;
    }
    auto job = std::make_shared<StreamLoadJob>();
    auto run_parse = [job, physical = resolved.value] {
        SceneLoadResult result;
        try {
            result = load_scene_from_physical(physical);
        } catch (...) {
            result.error = "Exception while parsing streamed chunk";
        }
        std::lock_guard<std::mutex> lock(job->mutex);
        job->ok = result.success && result.scene != nullptr;
        job->scene = std::move(result.scene);
        job->error = result.error;
        job->finished = true;
    };
    if (JobSystem::instance().is_initialized()) {
        // The task is passed BY VALUE, so the copy here is what the job
        // system owns. If it refuses the job, nothing will ever set
        // job->finished and a caller waiting for that flag would wait
        // forever -- so the work is done here instead. The call deliberately
        // does NOT move run_parse: a moved-from std::function is empty, and
        // calling it throws.
        if (!JobSystem::instance().enqueue(run_parse)) {
            run_parse();
        }
    } else {
        // No workers (tests, tools): deterministic inline parse. Same
        // contract — this call still returns false; the commit lands on the
        // next update, so both paths are exercised identically.
        run_parse();
    }
    m_stream_jobs.emplace(coord, std::move(job));
    return false;
}

void Runtime::streaming_unload_chunk(const scene::ChunkCoord& coord,
                                     const std::vector<ecs::Entity>& created) {
    if (!has_scene()) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    size_t destroyed = 0;
    for (ecs::Entity root : created) {
        if (!world.is_alive(root)) {
            continue;
        }
        // Destroy the whole subtree, children first: destroy_entity removes
        // components but does not cascade, and a member destroyed before its
        // children would strand their parent links.
        std::vector<ecs::Entity> members{root};
        for (size_t i = 0; i < members.size(); ++i) {
            for (ecs::Entity kid : scene::get_children(world, members[i])) {
                if (world.is_alive(kid)) {
                    members.push_back(kid);
                }
            }
        }
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            world.destroy_entity(*it);
            ++destroyed;
        }
    }
    NF_LOG_INFO(LogCategory::Core, "Runtime: streamed out chunk ({},{},{}) ({} entities)", coord.x,
                coord.y, coord.z, destroyed);
}

void Runtime::resync_after_streaming() {
    m_streaming_known_loaded = m_streamer.loaded_chunk_count();
    if (!has_scene()) {
        return;
    }
    // Merged entities reference meshes by stable AssetId exactly like a fresh
    // load, so the same kick starts their async loads; the frame's asset sync
    // uploads whatever is ready.
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<MeshComponent>()) {
        auto* comp = world.get<MeshComponent>(e);
        if (comp != nullptr && comp->mesh_id.valid()) {
            m_manager.load_mesh(comp->mesh_id);
        }
    }
    // Full physics rebuild: merged bodies get created, unloaded bodies are
    // gone with the fresh world. Loads are rare capped events, so the simple
    // correct pass beats an incremental one that must mirror its invariants.
    rebuild_physics_from_scene();
    // Same reasoning for the debris world: the level geometry a shard lands on
    // changed, and stale bindings would point at entities the unload removed.
    reset_destruction_state();
    mark_scene_edited();
}

void Runtime::rebuild_physics_from_scene() {
    // Controllers hold a raw PhysicsWorld pointer and body handles into it:
    // they must die before the world they point at is replaced.
    m_characters.clear();
    m_physics = std::make_unique<physics::PhysicsWorld>();
    m_physics_clock.reset();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }

    auto& world = m_scene_data_ptr->scene->world();
    size_t created = 0;
    for (auto e : world.query<physics::RigidBodyComponent>()) {
        auto* rb = world.get<physics::RigidBodyComponent>(e);
        auto* collider = world.get<physics::ColliderComponent>(e);
        auto* transform = world.get<scene::Transform>(e);
        if (rb == nullptr || collider == nullptr || transform == nullptr) {
            // An entity missing one of the three cannot be simulated. Leave its
            // handle invalid rather than inventing a default: the editor shows
            // the gap, and a silently-placed body is harder to explain.
            continue;
        }

        physics::BodyDesc desc;
        desc.type = rb->type;
        desc.shape = collider->shape;
        desc.position = Vec3(transform->local_x, transform->local_y, transform->local_z);
        desc.orientation = scene::quat_from_euler_xyz_degrees(
            transform->rot_x, transform->rot_y, transform->rot_z);
        desc.linear_velocity = rb->linear_velocity;
        desc.angular_velocity = rb->angular_velocity;
        desc.mass = rb->mass;
        desc.friction = rb->friction;
        desc.restitution = rb->restitution;
        desc.linear_damping = rb->linear_damping;
        desc.angular_damping = rb->angular_damping;
        desc.allow_sleep = rb->allow_sleep;

        rb->body = m_physics->add_body(desc);
        ++created;
    }
    // Characters own a dynamic body each through their controller (spawn pose
    // from the entity Transform, like rigid bodies above). An entity missing
    // its Transform is left without a controller, same rule as bodies.
    for (auto e : world.query<physics::CharacterComponent>()) {
        auto* ch = world.get<physics::CharacterComponent>(e);
        auto* transform = world.get<scene::Transform>(e);
        if (ch == nullptr || transform == nullptr || !ch->enabled) {
            continue;
        }
        const Vec3 spawn{transform->local_x, transform->local_y, transform->local_z};
        auto controller =
            std::make_unique<physics::CharacterController>(*m_physics, ch->config, spawn);
        m_characters.emplace(e, std::move(controller));
        ++created;
    }
    if (created > 0) {
        NF_LOG_INFO(LogCategory::Core, "Runtime: physics world created with {} bodies", created);
    }
}

bool Runtime::enable_destruction(const DestructionConfig& config) {
    if (m_destruction_enabled) {
        return true;
    }
    // Built in dependency order and assigned only when every piece is valid, so
    // a failure leaves nothing half-constructed: the world owns the sink's
    // bodies, the sink owns the destruction world's debris, and the destructor
    // tears them down in reverse.
    auto world = std::make_unique<physics::JoltWorld>();
    if (!world->valid()) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: destruction disabled, Jolt world unavailable");
        return false;
    }
    auto sink = std::make_unique<physics::JoltDebrisSink>(*world, config.sink);
    auto destruction = std::make_unique<destruction::DestructionWorld>(*sink, config.budget);

    m_debris_world = std::move(world);
    m_debris_sink = std::move(sink);
    m_destruction = std::move(destruction);
    m_debris_clock.reset();
    m_destruction_enabled = true;
    // The level's statics go in now so the first blast of the game lands on
    // something. Dynamic bodies stay with the engine solver — see the header.
    rebuild_debris_statics();
    NF_LOG_INFO(LogCategory::Core, "Runtime: destruction enabled (cap {} shards, {} static colliders)",
                config.budget.max_active_debris, m_debris_statics.size());
    return true;
}

void Runtime::disable_destruction() {
    if (!m_destruction_enabled) {
        return;
    }
    // Reverse dependency order: the destruction world holds a reference to the
    // sink, and the sink holds bodies in the world, so each must go before the
    // thing it references.
    m_destruction.reset();
    m_debris_sink.reset();
    m_debris_statics.clear();
    m_debris_world.reset();
    m_destructibles.clear();
    m_debris_asset.clear();
    m_shard_meshes.clear();
    m_debris_clock.reset();
    m_destruction_enabled = false;
}

void Runtime::reset_destruction_state() {
    // Bindings and shards are scene-scoped; the fracture assets are registered
    // by the game and outlive a scene change, so they stay.
    m_destructibles.clear();
    if (m_debris_sink) {
        m_debris_sink->clear();
    }
    // Every shard id is now retired, so its asset entry is dead too. The shard
    // meshes stay: they are keyed by (asset, chunk), which does not change with
    // the scene, and a second scene that breaks the same asset reuses them.
    m_debris_asset.clear();
    m_debris_clock.reset();
    if (m_destruction_enabled) {
        // The new scene has its own level geometry.
        rebuild_debris_statics();
    }
}

void Runtime::rebuild_debris_statics() {
    if (!m_debris_world) {
        return;
    }
    for (const physics::JoltBody& body : m_debris_statics) {
        m_debris_world->remove_body(body);
    }
    m_debris_statics.clear();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<physics::RigidBodyComponent>()) {
        const auto* rb = world.get<physics::RigidBodyComponent>(e);
        const auto* collider = world.get<physics::ColliderComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (rb == nullptr || collider == nullptr || t == nullptr) {
            continue;
        }
        // Statics only. A dynamic body here would be simulated twice — once by
        // the engine solver, once by Jolt — and the two would disagree about
        // where it is.
        if (rb->type != physics::BodyType::Static) {
            continue;
        }
        physics::BodyDesc desc;
        desc.type = physics::BodyType::Static;
        desc.shape = collider->shape;
        desc.position = Vec3(t->world_x, t->world_y, t->world_z);
        desc.orientation = scene::quat_from_euler_xyz_degrees(t->rot_x, t->rot_y, t->rot_z);
        const physics::JoltBody body = m_debris_world->add_body(desc);
        if (body.valid()) {
            m_debris_statics.push_back(body);
        }
    }
}

u32 Runtime::register_fracture_asset(const destruction::FractureAsset& asset) {
    if (asset.empty()) {
        return destruction::kInvalidChunk;
    }
    const u32 index = static_cast<u32>(m_fracture_assets.size());
    m_fracture_assets.push_back(asset);
    return index;
}

bool Runtime::bind_destructible(ecs::Entity entity, u32 asset_index) {
    if (asset_index >= m_fracture_assets.size()) {
        return false;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }
    // Without a Transform there is nowhere to place the asset in the world, and
    // inventing the identity pose would spawn shards at the origin when the
    // object is clearly somewhere else.
    if (!m_scene_data_ptr->scene->world().has<scene::Transform>(entity)) {
        return false;
    }
    DestructibleBinding binding;
    binding.asset_index = asset_index;
    // resize_for is idempotent, so rebinding the same asset keeps whatever
    // damage the object has already taken.
    binding.component.resize_for(m_fracture_assets[asset_index]);
    m_destructibles[entity] = std::move(binding);
    return true;
}

u32 Runtime::apply_damage(ecs::Entity entity, const destruction::DamageEvent& event) {
    if (!m_destruction_enabled || !m_destruction || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }
    const auto it = m_destructibles.find(entity);
    if (it == m_destructibles.end()) {
        return 0;
    }
    const scene::Transform* t = m_scene_data_ptr->scene->world().get<scene::Transform>(entity);
    if (t == nullptr) {
        return 0;
    }
    // The propagated world transform: apply_damage works in world space, and a
    // destructible that is a child of a moving platform has to break where the
    // platform actually is. Callers that set a transform by hand must propagate
    // first (update() does this every frame before this can run).
    const Mat4 world = scene::compose_trs_mat4(t->world_x, t->world_y, t->world_z,
                                               t->rot_x, t->rot_y, t->rot_z,
                                               t->scale_x, t->scale_y, t->scale_z);
    // apply_damage is the only path that creates shards, and the sink hands out
    // fresh ids monotonically, so the ids present afterwards but not before are
    // exactly this call's new shards. Recording the asset here — rather than in
    // the sink — keeps NFDestruction ignorant of the asset table, which is the
    // seam the whole module is built on; the sink carries the chunk id because
    // the spawn already carries it.
    const std::vector<u32> before = m_debris_sink ? m_debris_sink->live_ids() : std::vector<u32>{};
    const u32 shattered = m_destruction->apply_damage(m_fracture_assets[it->second.asset_index],
                                                      it->second.component, world, event);
    if (m_debris_sink) {
        size_t next = 0u;
        const std::vector<u32> after = m_debris_sink->live_ids();
        for (const u32 id : after) {
            // Both lists are ascending, so a single sweep matches them.
            while (next < before.size() && before[next] < id) {
                ++next;
            }
            if (next < before.size() && before[next] == id) {
                continue;
            }
            m_debris_asset[id] = it->second.asset_index;
        }
    }
    return shattered;
}

u32 Runtime::step_destruction(f32 frame_delta) {
    if (!m_destruction_enabled || !m_debris_world || !m_destruction) {
        return 0;
    }
    // Same fixed-clock contract as step_physics: the trajectories a shard
    // follows must not depend on the frame rate the machine happened to deliver.
    const u32 steps = m_debris_clock.advance(frame_delta);
    for (u32 i = 0; i < steps; ++i) {
        m_debris_world->step(m_debris_clock.step());
    }
    if (steps > 0) {
        // Aging after the step, so a shard that came to rest this step is
        // sleeping before the budget decides whether to retire it.
        m_destruction->tick(m_debris_clock.step() * static_cast<f32>(steps));
    }
    return steps;
}

void Runtime::bind_scene_destructibles() {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    u32 bound = 0u;
    for (auto e : world.query<DestructibleComponent>()) {
        const auto* d = world.get<DestructibleComponent>(e);
        const auto* collider = world.get<physics::ColliderComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (d == nullptr || !d->enabled) {
            continue;
        }
        if (collider == nullptr || t == nullptr) {
            // No collider means no shape to cut, no transform means nowhere to
            // put the pieces. Reported rather than defaulted: an invented
            // one-unit box is not what the author placed.
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: entity {} is destructible but has no collider or transform; it will not break",
                        e.id);
            continue;
        }
        destruction::FracturePiece piece;
        if (collider->shape.type == physics::ShapeType::Box) {
            piece = box_fracture_piece(collider->shape.box.half_extents);
        } else {
            // Only the box for v0.1. Substituting a box for another shape would
            // fracture something that does not look like what the player sees.
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: entity {} is destructible but its collider is not a box; it will not break",
                        e.id);
            continue;
        }

        destruction::FractureParams params;
        params.seed = d->seed;
        params.target_chunks = d->chunks;
        params.strength_per_area = d->strength;
        destruction::FractureAsset asset;
        std::string build_error;
        build_fracture_asset(piece, params, asset, &build_error);
        if (asset.empty() || asset.leaf_count() < 2u) {
            // Fewer than two leaves means no bonds, so no blast can ever take
            // the object apart. Binding it would advertise a breakable that
            // cannot break.
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: entity {} fracture asset has nothing to split ({}); it will not break",
                        e.id, build_error);
            continue;
        }

        // Enabled on the first bindable object, not once per object. A failure
        // here bails out of the whole scene: if the debris world cannot be
        // built, none of the remaining objects can break either.
        if (!m_destruction_enabled && !enable_destruction(DestructionConfig{})) {
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: scene's destructibles will not break (destruction unavailable)");
            return;
        }
        const u32 asset_index = register_fracture_asset(asset);
        if (asset_index == destruction::kInvalidChunk) {
            continue;
        }
        if (bind_destructible(e, asset_index)) {
            ++bound;
        }
    }
    if (bound > 0u) {
        NF_LOG_INFO(LogCategory::Core, "Runtime: scene carries {} destructible object(s)", bound);
    }
}

void Runtime::step_impact_damage() {
    if (!m_destruction_enabled || !m_physics || !m_destruction || !m_scene_data_ptr) {
        return;
    }
    if (m_destructibles.empty()) {
        return;
    }
    const std::vector<physics::Manifold>& manifolds = m_physics->last_manifolds();
    if (manifolds.empty()) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();

    // Retiring an object erases its map entry, so a hit found while walking the
    // bindings is collected and acted on afterwards rather than in place.
    std::vector<ecs::Entity> shattered;
    for (auto& [entity, binding] : m_destructibles) {
        const auto* rb = world.get<physics::RigidBodyComponent>(entity);
        if (rb == nullptr || !rb->body.valid()) {
            continue;
        }
        const auto* d = world.get<DestructibleComponent>(entity);
        for (const physics::Manifold& manifold : manifolds) {
            if (manifold.body_a != rb->body.index && manifold.body_b != rb->body.index) {
                continue;
            }
            if (manifold.point_count == 0u) {
                continue;
            }
            // The accumulated normal impulse across the step is the impact's
            // actual magnitude, not a speed heuristic: a slam is mass × Δv
            // delivered in one step, a resting contact only weight × dt. The
            // contact point is the mean of the manifold's, which keeps a
            // face-on hit centred and an edge hit where the edge is.
            f32 impulse = 0.0f;
            Vec3 point{0.0f, 0.0f, 0.0f};
            for (u32 i = 0u; i < manifold.point_count; ++i) {
                impulse += manifold.points[i].normal_impulse;
                point = point + manifold.points[i].position;
            }
            point = point * (1.0f / static_cast<f32>(manifold.point_count));
            const f32 threshold = (d != nullptr) ? d->damage_threshold : 8.0f;
            if (impulse <= threshold) {
                continue;
            }
            destruction::DamageEvent event;
            event.world_point = point;
            event.radius = (d != nullptr) ? d->blast_radius : 2.0f;
            event.impulse = impulse;
            static_cast<void>(apply_damage(entity, event));
            // Retire only when something actually came loose: apply_damage can
            // sever bonds while every chunk stays attached to the root, and
            // removing that object would delete a crate the player can still
            // see and stand on.
            if (binding.component.emitted_count() > 0u) {
                shattered.push_back(entity);
            }
            // One blast per object per frame; the manifold that mattered has
            // been found.
            break;
        }
    }
    // unordered_map iteration order is not stable across runs, so the retire
    // order is fixed here: two objects broken in the same frame must come
    // apart in the same order every run, or their shard ids would differ and
    // the determinism guarantee (§114) would not hold.
    std::sort(shattered.begin(), shattered.end(),
              [](const ecs::Entity& a, const ecs::Entity& b) {
                  return (a.id != b.id) ? (a.id < b.id) : (a.generation < b.generation);
              });
    for (ecs::Entity e : shattered) {
        retire_shattered_entity(e);
    }
}

void Runtime::retire_shattered_entity(ecs::Entity e) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    if (!world.is_alive(e)) {
        // Already gone (a second manifold, or a streaming unload raced the
        // retire). Drop the binding so a recycled id does not inherit it.
        m_destructibles.erase(e);
        return;
    }
    // The engine body goes before the components do: destroy_entity drops the
    // RigidBodyComponent and with it the handle, and a static body left in the
    // solver is an invisible box the rest of the level keeps colliding with
    // long after the player watched it explode.
    if (auto* rb = world.get<physics::RigidBodyComponent>(e)) {
        if (rb->body.valid() && m_physics) {
            m_physics->remove_body(rb->body);
        }
    }
    m_destructibles.erase(e);
    world.destroy_entity(e);
    // The debris world holds its own copy of the same static, and the shards
    // would land on it. Rebuilding from the surviving scene picks the floor up
    // and leaves the crate out. (Streaming is off by default; a level that
    // streams would need the same treatment on resync, which reset_destruction
    // already gives.)
    rebuild_debris_statics();
    NF_LOG_INFO(LogCategory::Core, "Runtime: destructible entity {} came apart", e.id);
}

std::vector<Runtime::DebrisSample> Runtime::debris_samples() const {
    std::vector<DebrisSample> samples;
    if (!m_debris_sink || !m_debris_world) {
        return samples;
    }
    const std::vector<u32> ids = m_debris_sink->live_ids();
    samples.reserve(ids.size());
    for (const u32 id : ids) {
        const physics::JoltBody body = m_debris_sink->body_of(id);
        if (!body.valid()) {
            continue;
        }
        const physics::JoltBodyState state = m_debris_world->state(body);
        samples.push_back(DebrisSample{id, state.position, state.rotation, state.linear_velocity});
    }
    return samples;
}

size_t Runtime::active_debris() const {
    return m_destruction ? m_destruction->active_debris() : 0u;
}

u32 Runtime::bonds_shattered_total() const {
    return m_destruction ? m_destruction->bonds_shattered_total() : 0u;
}

u32 Runtime::shards_dropped_for_budget() const {
    return m_destruction ? m_destruction->shards_dropped_for_budget() : 0u;
}

rendering::StaticMeshHandle Runtime::shard_mesh(u32 asset_index, u32 chunk_id) {
    if (asset_index >= m_fracture_assets.size()) {
        return rendering::StaticMeshHandle{};
    }
    const auto cached = m_shard_meshes.find({asset_index, chunk_id});
    if (cached != m_shard_meshes.end()) {
        return cached->second;
    }
    const destruction::FractureAsset& asset = m_fracture_assets[asset_index];
    if (chunk_id >= asset.chunks.size()) {
        return rendering::StaticMeshHandle{};
    }
    const destruction::FracturePiece& piece = asset.chunks[chunk_id].piece;
    if (piece.vertices.empty() || piece.indices.empty()) {
        return rendering::StaticMeshHandle{};
    }

    // The chunk's hull is the collision shape; the mesh drawn for it is the
    // same vertices and indices, so the shard's silhouette is the fracture
    // itself rather than an approximation of it. Flat normals: a convex hull
    // has no smooth seams to blend across, so per-triangle normals are the
    // honest answer, and they cost nothing to compute here.
    // The constructor already pushes an empty default LOD 0 — fill it rather
    // than appending a second LOD, or bounds()/bounding_sphere() (which read
    // m_lods[0]) keep describing the empty one and every shard culls and draws
    // from a zero-size box. This is the same entry point Terrain uses.
    auto mesh = std::make_unique<rendering::StaticMesh>("debris shard");
    rendering::MeshLOD& lod = mesh->lod(0);
    lod.vertices.reserve(piece.vertices.size());
    for (const Vec3& p : piece.vertices) {
        rendering::Vertex v{};
        v.position[0] = p.x;
        v.position[1] = p.y;
        v.position[2] = p.z;
        lod.vertices.push_back(v);
    }
    for (usize t = 0u; t + 2u < piece.indices.size(); t += 3u) {
        const Vec3& a = piece.vertices[piece.indices[t + 0u]];
        const Vec3& b = piece.vertices[piece.indices[t + 1u]];
        const Vec3& c = piece.vertices[piece.indices[t + 2u]];
        Vec3 n = { (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y),
                   (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z),
                   (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) };
        const f32 len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
        if (len_sq > 1e-12f) {
            const f32 inv = 1.0f / std::sqrt(len_sq);
            n = { n.x * inv, n.y * inv, n.z * inv };
        } else {
            n = { 0.0f, 1.0f, 0.0f }; // degenerate triangle: pick something safe
        }
        for (usize k = 0u; k < 3u; ++k) {
            rendering::Vertex& v = lod.vertices[piece.indices[t + k]];
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
        }
        lod.indices.push_back(piece.indices[t + 0u]);
        lod.indices.push_back(piece.indices[t + 1u]);
        lod.indices.push_back(piece.indices[t + 2u]);
    }
    rendering::SubMesh sm;
    sm.index_offset = 0;
    sm.index_count = static_cast<u32>(lod.indices.size());
    sm.vertex_offset = 0;
    sm.vertex_count = static_cast<u32>(lod.vertices.size());
    sm.material_slot = 0;
    lod.submeshes.push_back(sm);
    lod.material_slots.push_back(rendering::MaterialSlot{"default"});
    rendering::StaticMesh::compute_lod_bounds(lod);
    // Submesh bounds follow the LOD bounds, as compute_bounds would have done.
    lod.submeshes[0].bounds = lod.bounds;
    lod.submeshes[0].sphere = lod.sphere;

    // Upload on the render thread like every other mesh: shards appear this
    // frame, not next, which matters for the one frame the player is watching.
    if (!mesh->upload(m_device)) {
        return rendering::StaticMeshHandle{};
    }
    const rendering::StaticMeshHandle handle = m_mesh_library->add(std::move(mesh));
    if (!handle.valid()) {
        return rendering::StaticMeshHandle{};
    }
    m_shard_meshes[{asset_index, chunk_id}] = handle;
    return handle;
}

void Runtime::build_debris_render_world(rendering::RenderWorld& out) {
    if (!m_destruction_enabled || !m_debris_world || !m_debris_sink || !m_debris_world->valid()) {
        return;
    }
    if (!m_mesh_library) {
        return;
    }
    ensure_default_material();
    const std::vector<u32> ids = m_debris_sink->live_ids();
    for (const u32 id : ids) {
        const physics::JoltBody body = m_debris_sink->body_of(id);
        if (!body.valid()) {
            continue;
        }
        const auto asset_it = m_debris_asset.find(id);
        if (asset_it == m_debris_asset.end()) {
            continue;
        }
        const u32 chunk_id = m_debris_sink->chunk_of(id);
        if (chunk_id == destruction::kInvalidChunk) {
            continue;
        }
        const rendering::StaticMeshHandle handle = shard_mesh(asset_it->second, chunk_id);
        if (!handle.valid()) {
            continue;
        }
        const rendering::StaticMesh* mesh = m_mesh_library->get(handle);
        if (mesh == nullptr || !mesh->is_uploaded()) {
            continue;
        }

        const physics::JoltBodyState state = m_debris_world->state(body);
        // The state's position is the shape-local origin in world space — the
        // same origin the hull points are relative to — so this pose puts the
        // shard's geometry exactly where the physics body is. This is the same
        // contract as the JoltBodyState read-back for scene bodies, and it is
        // why writing GetCenterOfMassPosition() here would offset every shard
        // by its own centroid (see JoltWorld::state).
        //
        // rotation * translate, NOT translate * rotation: the engine's matrices
        // apply rightmost-first (v * M = ((v * R) * T), the same order
        // compose_trs_mat4 bakes in), so rotation must be on the LEFT. Swapping
        // them still rotates the shard correctly but rotates its POSITION about
        // the world origin as well — a tumbling shard at (32, 86, 35) renders
        // somewhere entirely else, while a nearly-still one is off by
        // millimetres and the bug looks like a tolerance issue.
        const Mat4 world = state.rotation.to_matrix() * Mat4::translate(state.position);

        rendering::RenderObject ro{};
        ro.id = 0; // not an entity: picking a shard reports no object, by design
        ro.visible = true;
        ro.world = world;
        ro.mesh_handle = handle;
        ro.material_handle = m_default_material;
        ro.lod = 0;
        // A shard tumbles, so its bounds cannot be translated the way the
        // scene's are: the eight corners of the local AABB go through the
        // rotation, and the result is re-boxed. Larger than the true rotated
        // box by construction, which is exactly what a cull test wants.
        const rendering::AABB lb = mesh->bounds();
        Vec3 corners[8] = {
            { lb.min_x, lb.min_y, lb.min_z }, { lb.max_x, lb.min_y, lb.min_z },
            { lb.min_x, lb.max_y, lb.min_z }, { lb.max_x, lb.max_y, lb.min_z },
            { lb.min_x, lb.min_y, lb.max_z }, { lb.max_x, lb.min_y, lb.max_z },
            { lb.min_x, lb.max_y, lb.max_z }, { lb.max_x, lb.max_y, lb.max_z },
        };
        Vec3 mn = state.position;
        Vec3 mx = state.position;
        for (const Vec3& c : corners) {
            const Vec3 w = world.transform_point(c);
            mn.x = std::min(mn.x, w.x); mn.y = std::min(mn.y, w.y); mn.z = std::min(mn.z, w.z);
            mx.x = std::max(mx.x, w.x); mx.y = std::max(mx.y, w.y); mx.z = std::max(mx.z, w.z);
        }
        ro.bounds = rendering::AABB{ mn.x, mn.y, mn.z, mx.x, mx.y, mx.z };
        const rendering::BoundingSphere ls = mesh->bounding_sphere();
        const Vec3 sphere_centre = world.transform_point({ ls.cx, ls.cy, ls.cz });
        ro.sphere = rendering::BoundingSphere{ sphere_centre.x, sphere_centre.y, sphere_centre.z, ls.radius };
        out.objects.push_back(std::move(ro));
    }
}

void Runtime::build_vehicle_wheels_render_world(rendering::RenderWorld& out) {
    if (!m_vehicle_world || !m_vehicle_system || !m_vehicle_world->valid() ||
        !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!m_mesh_library) {
        return;
    }
    ensure_default_material();

    // One shared unit cube for every wheel of every car, scaled in the pose
    // matrix — the flattened-cube tire the VehicleDemo draws, so the scene car
    // and the demo car look related. Built lazily on the first wheel frame (the
    // shard_mesh pattern): a headless runtime has no device, upload fails, the
    // handle stays invalid and wheels simply do not draw instead of crashing.
    if (!m_wheel_mesh.valid()) {
        auto mesh = rendering::StaticMesh::create_cube(1.0f);
        if (mesh == nullptr || !mesh->upload(m_device)) {
            return;
        }
        const rendering::StaticMeshHandle handle = m_mesh_library->add(std::move(mesh));
        if (!handle.valid()) {
            return;
        }
        m_wheel_mesh = handle;
    }
    const rendering::StaticMesh* wheel_mesh = m_mesh_library->get(m_wheel_mesh);
    if (wheel_mesh == nullptr || !wheel_mesh->is_uploaded()) {
        return;
    }

    constexpr float kRadToDeg = 57.2957795f;
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<scene::Transform, physics::VehicleComponent>()) {
        const auto* vc = world.get<physics::VehicleComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (vc == nullptr || !vc->enabled || vc->vehicle == nullptr || t == nullptr) {
            continue;
        }
        const std::vector<physics::JoltWheelState> states = vc->vehicle->wheel_states();
        if (states.empty()) {
            continue;
        }
        // The chassis orientation, re-read from the Transform the vehicle sync
        // wrote this frame — the wheels must roll with a pitched or rocking
        // body, not stay axis-aligned to the world while the car rotates.
        const Quat chassis = scene::quat_from_euler_xyz_degrees(t->rot_x, t->rot_y, t->rot_z);
        const WheelSpin* spin = nullptr;
        if (const auto it = m_vehicle_wheel_spin.find(e); it != m_vehicle_wheel_spin.end()) {
            spin = &it->second;
        }
        for (usize w = 0; w < states.size(); ++w) {
            const physics::JoltWheelState& st = states[w];
            // Steer first, then spin (R = Ry * Rx, the convention
            // quat_from_euler_xyz_degrees applies to (x=spin, y=steer)): a
            // steered wheel rolls around the STEERED axis, which is exactly
            // euler-x euler-y nesting. Same orientation the demo parks on its
            // wheel entities — spin on X, steer on Y — read from the same
            // JoltWheelState, so the scene car rolls like the demo car.
            const f32 spin_deg =
                (spin != nullptr && w < spin->angle.size()) ? spin->angle[w] * kRadToDeg : 0.0f;
            const Quat wheel_local = scene::quat_from_euler_xyz_degrees(
                spin_deg, st.steer_angle * kRadToDeg, 0.0f);
            // width along X (the spin axis), diameter along Y and Z: the cube
            // spans +/-0.5 per axis, so scale = the tire's real extents.
            const f32 radius = vc->config.wheel_radius;
            const f32 width = vc->config.wheel_width;
            // compose_quat_scale_mat4 = T * R * S, the one canonical transform
            // to a row-vector Mat4; the state's position is the wheel CENTER
            // in world space (suspension travel already applied), so no
            // chassis offset is added here.
            const Mat4 m = scene::compose_quat_scale_mat4(
                st.position, chassis * wheel_local, Vec3{width, radius * 2.0f, radius * 2.0f});

            rendering::RenderObject ro{};
            ro.id = 0; // not an entity: wheels are render-only, picking reports none
            ro.visible = true;
            ro.world = m;
            ro.mesh_handle = m_wheel_mesh;
            ro.material_handle = m_default_material;
            ro.lod = 0;
            // Culling bounds through the same matrix the draw will use — the
            // wheels spin every frame, so the cube's corners are transformed
            // and re-boxed (the debris path's corner walk, unchanged).
            const rendering::AABB lb = wheel_mesh->bounds();
            Vec3 corners[8] = {
                { lb.min_x, lb.min_y, lb.min_z }, { lb.max_x, lb.min_y, lb.min_z },
                { lb.min_x, lb.max_y, lb.min_z }, { lb.max_x, lb.max_y, lb.min_z },
                { lb.min_x, lb.min_y, lb.max_z }, { lb.max_x, lb.min_y, lb.max_z },
                { lb.min_x, lb.max_y, lb.max_z }, { lb.max_x, lb.max_y, lb.max_z },
            };
            Vec3 mn = st.position;
            Vec3 mx = st.position;
            for (const Vec3& c : corners) {
                const Vec3 wpos = m.transform_point(c);
                mn.x = std::min(mn.x, wpos.x); mn.y = std::min(mn.y, wpos.y); mn.z = std::min(mn.z, wpos.z);
                mx.x = std::max(mx.x, wpos.x); mx.y = std::max(mx.y, wpos.y); mx.z = std::max(mx.z, wpos.z);
            }
            ro.bounds = rendering::AABB{ mn.x, mn.y, mn.z, mx.x, mx.y, mx.z };
            const rendering::BoundingSphere ls = wheel_mesh->bounding_sphere();
            const Vec3 sc = m.transform_point({ ls.cx, ls.cy, ls.cz });
            ro.sphere = rendering::BoundingSphere{ sc.x, sc.y, sc.z,
                ls.radius * std::max({width, radius * 2.0f}) };
            out.objects.push_back(std::move(ro));
        }
    }
}

void Runtime::ensure_feedback_resources() {
    if (!m_feedback_bound || !m_renderer || !m_renderer_initialized || !m_mesh_library) {
        return;
    }
    rendering::Material* gbuffer = m_renderer->gbuffer_material();
    if (gbuffer == nullptr || !gbuffer->valid()) {
        return;
    }
    // The flash and the tracer are emissive so they read as light rather than
    // as plastic cubes. The values are deliberately above 1: with bloom enabled
    // a shot glows, and with bloom off it still saturates to white.
    if (!m_feedback_flash_material.valid()) {
        rendering::PBRMaterialParams params{};
        params.base_color[0] = 0.35f;
        params.base_color[1] = 0.22f;
        params.base_color[2] = 0.08f;
        params.base_color[3] = 1.0f;
        params.metallic = 0.0f;
        params.roughness = 1.0f;
        params.ao = 1.0f;
        params.emission[0] = 1.0f;
        params.emission[1] = 0.62f;
        params.emission[2] = 0.22f;
        params.emission_strength = 14.0f;
        m_feedback_flash_material =
            m_renderer->materials().create_instance(*gbuffer, params, "feedback_flash");
    }
    if (!m_feedback_tracer_material.valid()) {
        rendering::PBRMaterialParams params{};
        params.base_color[0] = 0.30f;
        params.base_color[1] = 0.18f;
        params.base_color[2] = 0.06f;
        params.base_color[3] = 1.0f;
        params.metallic = 0.0f;
        params.roughness = 1.0f;
        params.ao = 1.0f;
        params.emission[0] = 1.0f;
        params.emission[1] = 0.42f;
        params.emission[2] = 0.12f;
        params.emission_strength = 9.0f;
        m_feedback_tracer_material =
            m_renderer->materials().create_instance(*gbuffer, params, "feedback_tracer");
    }
    // A scorch mark is not emissive: it is a soot-dark patch that the scene's
    // own lights still shade. No texture, so the decal's material is complete
    // without the asset pipeline and a hit always leaves something visible.
    if (!m_feedback_impact_material.valid()) {
        rendering::PBRMaterialParams params{};
        params.base_color[0] = 0.025f;
        params.base_color[1] = 0.022f;
        params.base_color[2] = 0.020f;
        params.base_color[3] = 1.0f;
        params.metallic = 0.0f;
        params.roughness = 0.95f;
        params.ao = 1.0f;
        params.emission_strength = 0.0f;
        m_feedback_impact_material =
            m_renderer->materials().create_instance(*gbuffer, params, "feedback_scorch");
    }
    // One shared cube for every flash and every beam segment. Lazily uploaded
    // exactly like the wheel mesh: a headless device has no GPU, so the upload
    // fails once and the layer simply does not draw instead of crashing.
    if (!m_feedback_mesh.valid()) {
        auto mesh = rendering::StaticMesh::create_cube(1.0f);
        if (mesh == nullptr || !mesh->upload(m_device)) {
            return;
        }
        const rendering::StaticMeshHandle handle = m_mesh_library->add(std::move(mesh));
        if (!handle.valid()) {
            return;
        }
        m_feedback_mesh = handle;
    }
}

void Runtime::build_combat_feedback_render_world(rendering::RenderWorld& out) {
    if (!m_feedback_bound || m_feedback.empty()) {
        return;
    }
    ensure_feedback_resources();
    if (!m_feedback_mesh.valid() || !m_mesh_library) {
        return;
    }
    const rendering::StaticMesh* mesh = m_mesh_library->get(m_feedback_mesh);
    if (mesh == nullptr || !mesh->is_uploaded()) {
        return;
    }

    // The one orientation problem this layer has: the cube/beam's local +Z must
    // point along a world direction, and Quat has no from_to. Cross product and
    // angle is exact, with the antiparallel case handled explicitly (its cross
    // product is zero and acos would otherwise return a degenerate axis).
    const auto align_z = [](const Vec3& direction) {
        Vec3 dir = direction.normalized();
        if (dir.length_sq() < 1.0e-8f) {
            dir = Vec3{0.0f, 0.0f, 1.0f};
        }
        const f32 dot = std::clamp(dir.z, -1.0f, 1.0f);
        if (dot > 0.99999f) {
            return Quat::identity();
        }
        if (dot < -0.99999f) {
            return Quat::from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, PI);
        }
        const Vec3 axis = Vec3{0.0f, 0.0f, 1.0f}.cross(dir);
        return Quat::from_axis_angle(axis, std::acos(dot));
    };

    const auto push_cube = [&](const Mat4& world, rendering::MaterialHandle material) {
        rendering::RenderObject ro{};
        ro.id = 0; // not an entity: a flash is never a pick result
        ro.visible = true;
        ro.world = world;
        ro.mesh_handle = m_feedback_mesh;
        ro.material_handle = material;
        ro.lod = 0;
        ro.bounds = rendering::transform_aabb(mesh->bounds(), world);
        ro.sphere = rendering::transform_sphere(mesh->bounding_sphere(), world);
        out.objects.push_back(std::move(ro));
    };

    if (m_feedback_flash_material.valid()) {
        for (const runtime::FeedbackFlash& flash : m_feedback.flashes()) {
            // Strength shrinks the cube as it dies, so the last visible frame is
            // a small bright point instead of a full-size block vanishing.
            const f32 s = std::max(0.001f, flash.size * (0.35f + 0.65f * flash.strength));
            const Mat4 world = Mat4::scale(Vec3{s, s, s * 0.85f}) *
                               align_z(flash.direction).to_matrix() *
                               Mat4::translate(flash.position);
            push_cube(world, m_feedback_flash_material);
        }
    }

    if (m_feedback_tracer_material.valid()) {
        for (const runtime::FeedbackTracer& tracer : m_feedback.tracers()) {
            const Vec3 delta = tracer.end - tracer.start;
            const f32 length = delta.length();
            if (length < 1.0e-4f) {
                continue;
            }
            const f32 width =
                std::max(0.0005f, tracer.width * (0.55f + 0.45f * tracer.strength));
            const Vec3 mid = (tracer.start + tracer.end) * 0.5f;
            const Mat4 world = Mat4::scale(Vec3{width, width, length}) *
                               align_z(delta).to_matrix() * Mat4::translate(mid);
            push_cube(world, m_feedback_tracer_material);
        }
    }

    if (m_feedback_impact_material.valid()) {
        for (const runtime::FeedbackImpact& mark : m_feedback.impacts()) {
            // Steady for the first three quarters, then a linear fade. Using
            // the director's linear strength here would make a six-second mark
            // faint almost immediately.
            f32 opacity = 1.0f;
            const f32 fade = mark.life * 0.25f;
            if (fade > 0.0f && mark.age > mark.life - fade) {
                opacity = (mark.life - mark.age) / fade;
            }
            opacity = std::clamp(opacity, 0.0f, 1.0f);
            if (opacity <= 0.0f) {
                continue;
            }
            const f32 roll = static_cast<f32>(mark.seed % 360u) * DEG_TO_RAD;
            // Row-vector composition is rightmost-first: M(q1*q2) =
            // M(q2)*M(q1), so `align * roll` rolls around the decal's own +Z
            // FIRST and only then aims that axis at the surface normal. The
            // other order would spin the mark around world Z and tilt it off
            // the wall for any normal that is not +Z.
            const Quat rotation =
                align_z(mark.normal) * Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, roll);
            const Mat4 entity_world = rotation.to_matrix() * Mat4::translate(mark.position);
            const Mat4 world = rendering::decal_world_matrix(
                entity_world, Vec3{mark.size, mark.size, std::max(0.02f, mark.size)});
            out.decals.push_back(
                rendering::make_decal(0u, world, m_feedback_impact_material, opacity));
        }
    }
}

u32 Runtime::step_physics(float frame_delta) {
    if (!m_physics || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }

    const u32 steps = m_physics_clock.advance(frame_delta);
    // Character tick protocol (order matters): move() sets velocities, the
    // world solves, post_step() refreshes grounded from the fresh manifolds.
    // Sorted by handle so the sequence never depends on hash order.
    std::vector<ecs::Entity> characters;
    characters.reserve(m_characters.size());
    for (const auto& [entity, controller] : m_characters) {
        (void)controller;
        characters.push_back(entity);
    }
    std::sort(characters.begin(), characters.end(), [](ecs::Entity a, ecs::Entity b) {
        return (a.id != b.id) ? (a.id < b.id) : (a.generation < b.generation);
    });
    auto& world_for_characters = m_scene_data_ptr->scene->world();
    for (u32 i = 0; i < steps; ++i) {
        const float h = m_physics_clock.step();
        for (ecs::Entity e : characters) {
            auto it = m_characters.find(e);
            if (it == m_characters.end() || it->second == nullptr) {
                continue;
            }
            if (!world_for_characters.is_alive(e)) {
                continue;
            }
            const auto* comp = world_for_characters.get<physics::CharacterComponent>(e);
            if (comp == nullptr || !comp->enabled) {
                continue;
            }
            it->second->move(comp->wish_dir, comp->jump, h);
        }
        m_physics->step(h);
        for (ecs::Entity e : characters) {
            auto it = m_characters.find(e);
            if (it == m_characters.end() || it->second == nullptr) {
                continue;
            }
            if (world_for_characters.is_alive(e)) {
                it->second->post_step();
            }
        }
    }
    if (steps == 0) {
        return 0;
    }

    // Write the simulation back into the scene so rendering and the editor see
    // it. Velocities go back too, so a body's state survives a save/load and the
    // inspector can show it.
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<physics::RigidBodyComponent>()) {
        auto* rb = world.get<physics::RigidBodyComponent>(e);
        auto* transform = world.get<scene::Transform>(e);
        if (rb == nullptr || transform == nullptr || !m_physics->is_alive(rb->body)) {
            continue;
        }
        const physics::BodyState state = m_physics->state(rb->body);
        transform->local_x = state.position.x;
        transform->local_y = state.position.y;
        transform->local_z = state.position.z;
        scene::euler_xyz_degrees_from_quat(state.orientation, transform->rot_x,
                                           transform->rot_y, transform->rot_z);
        transform->dirty = true;

        rb->linear_velocity = state.linear_velocity;
        rb->angular_velocity = state.angular_velocity;
    }
    // Characters write back the same way: physics owns the position, the
    // transform follows, and grounded lands in the component for gameplay.
    for (ecs::Entity e : characters) {
        auto it = m_characters.find(e);
        auto* comp = world.get<physics::CharacterComponent>(e);
        auto* transform = world.get<scene::Transform>(e);
        if (it == m_characters.end() || it->second == nullptr || comp == nullptr ||
            transform == nullptr) {
            continue;
        }
        const Vec3 pos = it->second->position();
        transform->local_x = pos.x;
        transform->local_y = pos.y;
        transform->local_z = pos.z;
        transform->dirty = true;
        comp->grounded = it->second->grounded();
    }
    return steps;
}

bool Runtime::extract_camera(uint32_t target_width, uint32_t target_height, rendering::Camera& out) {
    out = rendering::Camera{};
    out.position = {0.0f, 2.0f, 5.0f};
    out.target = {0.0f, 0.0f, 0.0f};
    out.up = {0.0f, 1.0f, 0.0f};
    out.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    out.near_plane = 0.1f;
    out.far_plane = 1000.0f;
    out.aspect = (target_height != 0) ? (static_cast<float>(target_width) / static_cast<float>(target_height))
                                      : 16.0f / 9.0f;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        rendering::update_camera(out);
        return false;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<CameraComponent>()) {
        auto* c = world.get<CameraComponent>(e);
        if (c == nullptr || !c->is_active) {
            continue;
        }
        // The player's FOV, when they set one, replaces the scene's — a wider
        // view is a comfort setting for some players and a competitive one for
        // others, and neither is the level author's call. 0 means "the scene".
        const f32 fov_deg = m_player_settings.fov_y > 0.0f ? m_player_settings.fov_y : c->fov_y;
        out.fov_y_rad = fov_deg * 3.14159265359f / 180.0f;
        out.near_plane = c->near_plane;
        out.far_plane = c->far_plane;
        // Aspect follows the actual target so offscreen/windowed renders are correct
        // even when the serialized aspect is stale.
        if (target_width != 0 && target_height != 0) {
            out.aspect = static_cast<float>(target_width) / static_cast<float>(target_height);
        } else if (c->aspect > 0.0f) {
            out.aspect = c->aspect;
        }
        if (const auto* t = world.get<scene::Transform>(e)) {
            out.position = {t->world_x, t->world_y, t->world_z};
        }
        // A follow camera decides for itself where it is and what it looks
        // at. Its Transform is the pivot of the ORBIT, not the camera: the
        // camera's position is the RESULT of the follow, and reading it back
        // from the Transform would make the camera chase the position it was
        // placed in last frame.
        if (m_follow_active) {
            // Camera shake is applied to the RESULT of the follow, never to
            // its state: the follow keeps easing towards the unshaken pose,
            // so a long shake cannot walk the camera away from the player.
            // The positional part moves the eye; the rotational part turns
            // the view direction, which is what makes a hit read as a hit
            // rather than as the world sliding sideways.
            const Vec3 offset =
                m_camera_shake.position_offset() * (c->shake_strength * m_player_settings.shake_scale);
            const Vec3 rotation = m_camera_shake.rotation_offset_deg() *
                                  (c->shake_strength * m_player_settings.shake_scale);
            out.position = m_follow_camera.position + offset;
            Vec3 direction = m_follow_camera.target - m_follow_camera.position;
            if (rotation.x != 0.0f || rotation.y != 0.0f || rotation.z != 0.0f) {
                const Quat q =
                    scene::quat_from_euler_xyz_degrees(rotation.x, rotation.y, rotation.z);
                direction = q.rotate(direction);
            }
            out.target = out.position + direction;
        } else {
            out.target = {0.0f, 0.0f, 0.0f};
            out.up = {0.0f, 1.0f, 0.0f};
            // A camera serialized at the origin (e.g. the Example scene default)
            // would sit inside the mesh; fall back to a sensible orbit position so the
            // scene is actually visible instead of degenerate.
            const float dx = out.position.x;
            const float dy = out.position.y;
            const float dz = out.position.z;
            const float dist2 = dx * dx + dy * dy + dz * dz;
            if (dist2 < 0.25f) {
                out.position = {0.0f, 2.0f, 5.0f};
            }
        }
        rendering::update_camera(out);
        return true;
    }
    // No active camera: keep the default orbit camera.
    rendering::update_camera(out);
    return false;
}

namespace {

// The living entity with this name, or kInvalidEntity.
//
// The same lookup the Lua binding does (nf.find_entity), so a camera that
// follows "Player" and a script that finds "Player" agree on what "Player"
// is.
ecs::Entity find_entity_by_name(ecs::World& world, const std::string& name) {
    if (name.empty()) {
        return ecs::kInvalidEntity;
    }
    for (ecs::Entity e : world.all_entities()) {
        if (!world.is_alive(e)) {
            continue;
        }
        const scene::NameComponent* n = world.get<scene::NameComponent>(e);
        if (n != nullptr && n->name == name) {
            return e;
        }
    }
    return ecs::kInvalidEntity;
}

} // namespace

std::vector<Runtime::CameraObstacle> Runtime::collect_camera_obstacles(ecs::World& world) const {
    std::vector<CameraObstacle> out;
    for (auto e : world.query<physics::ColliderComponent>()) {
        const auto* col = world.get<physics::ColliderComponent>(e);
        const auto* rb = world.get<physics::RigidBodyComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (col == nullptr || t == nullptr) {
            continue;
        }
        // Static scenery only: a dynamic body moves, and a wall that moved
        // every frame would be a wall the camera kept missing.
        if (rb != nullptr && rb->type != physics::BodyType::Static) {
            continue;
        }
        // The voxeliser is AABB-based and the camera's test is the same
        // shape, so anything else is skipped rather than approximated.
        if (col->shape.type != physics::ShapeType::Box) {
            continue;
        }
        CameraObstacle box;
        const Vec3 centre{t->world_x, t->world_y, t->world_z};
        const Vec3 half{col->shape.box.half_extents.x * std::abs(t->world_scale_x),
                        col->shape.box.half_extents.y * std::abs(t->world_scale_y),
                        col->shape.box.half_extents.z * std::abs(t->world_scale_z)};
        box.min = Vec3{centre.x - half.x, centre.y - half.y, centre.z - half.z};
        box.max = Vec3{centre.x + half.x, centre.y + half.y, centre.z + half.z};
        out.push_back(box);
    }
    return out;
}

float Runtime::camera_obstacle_hit(const std::vector<CameraObstacle>& obstacles, const Vec3& origin,
                                   const Vec3& direction, float max_distance, float radius) const {
    // A swept sphere against a box, one axis at a time: the slab test gives
    // the entry and exit parameters, and the sphere is the box grown by the
    // radius.
    //
    // Growing the box is an approximation (a sphere is not a box), and it is
    // the approximation a camera wants: the camera keeps its distance from
    // the wall, and the corner case is a corner the camera slides past.
    float closest = max_distance;
    bool hit = false;
    for (const CameraObstacle& box : obstacles) {
        const Vec3 lo{box.min.x - radius, box.min.y - radius, box.min.z - radius};
        const Vec3 hi{box.max.x + radius, box.max.y + radius, box.max.z + radius};
        const float origin_axis[3] = {origin.x, origin.y, origin.z};
        const float dir_axis[3] = {direction.x, direction.y, direction.z};
        const float lo_axis[3] = {lo.x, lo.y, lo.z};
        const float hi_axis[3] = {hi.x, hi.y, hi.z};
        float t_enter = 0.0f;
        float t_exit = closest;
        bool miss = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir_axis[axis]) < 1e-8f) {
                // Parallel to this slab: a miss when the origin is outside it.
                if (origin_axis[axis] < lo_axis[axis] || origin_axis[axis] > hi_axis[axis]) {
                    miss = true;
                    break;
                }
                continue;
            }
            const float inv = 1.0f / dir_axis[axis];
            float t0 = (lo_axis[axis] - origin_axis[axis]) * inv;
            float t1 = (hi_axis[axis] - origin_axis[axis]) * inv;
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            t_enter = std::max(t_enter, t0);
            t_exit = std::min(t_exit, t1);
            if (t_enter > t_exit) {
                miss = true;
                break;
            }
        }
        if (miss) {
            continue;
        }
        if (t_enter >= 0.0f && t_enter < closest) {
            closest = t_enter;
            hit = true;
        }
    }
    return hit ? closest : -1.0f;
}

f32 Runtime::camera_yaw() const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0.0f;
    }
    const ecs::World& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<CameraComponent>()) {
        const auto* c = world.get<CameraComponent>(e);
        if (c != nullptr && c->is_active) {
            return c->yaw;
        }
    }
    return 0.0f;
}

void Runtime::add_camera_shake(f32 amount) {
    // Clamped by CameraShake itself; the guard here is for the non-finite
    // case, which a script can produce with a division and which would
    // otherwise poison the noise clock for the rest of the run.
    if (!std::isfinite(amount) || amount <= 0.0f) {
        return;
    }
    m_camera_shake.add_trauma(amount);
}

void Runtime::add_recoil(f32 pitch_deg, f32 yaw_deg) {    // The clamp is the ACTIVE camera's: a scene can widen or narrow the kick
    // without the script knowing, and a camera-less scene keeps the default.
    f32 max_abs = 12.0f;
    if (m_scene_data_ptr && m_scene_data_ptr->scene) {
        auto& world = m_scene_data_ptr->scene->world();
        for (auto e : world.query<CameraComponent>()) {
            const auto* c = world.get<CameraComponent>(e);
            if (c != nullptr && c->is_active) {
                max_abs = c->recoil_max;
                break;
            }
        }
    }
    runtime::recoil_add(m_recoil, pitch_deg, yaw_deg, max_abs);
}

bool Runtime::load_player_settings(const std::string& logical_path, std::string& out_error) {
    const auto text = m_vfs.read_text(logical_path);
    if (!text.ok) {
        out_error = text.error;
        return false;
    }
    runtime::PlayerSettings parsed;
    std::string parse_error;
    if (!runtime::parse_player_settings(text.value, parsed, parse_error)) {
        out_error = logical_path + ": " + parse_error;
        return false;
    }
    m_player_settings = parsed;
    apply_player_audio_settings();
    return true;
}

void Runtime::set_player_settings(const PlayerSettings& settings) {
    m_player_settings = settings;
    apply_player_audio_settings();
}

void Runtime::apply_player_audio_settings() {
    // The mixer reads its volumes every block, so this is the whole of "the
    // slider moved" — no reconfiguration, no reload. A scene that never wrote
    // a settings file gets the shipped defaults, because PlayerSettings starts
    // from AudioVolumeSettings' own defaults rather than a copy of them.
    audio::AudioVolumeSettings& live = m_audio_scene.settings();
    for (u32 i = 0; i < audio::kBusCount; ++i) {
        const auto bus = static_cast<audio::BusId>(i);
        live.set_volume(bus, m_player_settings.volumes.volume(bus));
    }

    // Dynamics are the same kind of binding: the processor reads its settings
    // from the scene's member, and set_settings intentionally keeps the live
    // envelopes so moving a threshold slider during play does not click.
    m_audio_scene.set_dynamics(m_player_settings.dynamics);
}

bool Runtime::save_player_settings(const std::string& logical_path, std::string& out_error) const {    const auto written =
        m_vfs.write_text(logical_path, runtime::serialize_player_settings(m_player_settings));
    if (!written.ok) {
        out_error = written.error;
        return false;
    }
    return true;
}

void Runtime::update_follow_camera(float dt) {
    m_follow_active = false;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        if (m_input_source != nullptr) {
            m_input_source->set_pointer_capture(false);
        }
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    CameraComponent* cam = nullptr;
    for (auto e : world.query<CameraComponent>()) {
        auto* c = world.get<CameraComponent>(e);
        if (c == nullptr || !c->is_active) {
            continue;
        }
        cam = c;
        break;
    }

    // Whether the pointer belongs to the game this frame. Decided ONCE and
    // applied ONCE: asking for capture and then giving it back every frame
    // would clip and unclip the cursor sixty times a second, which is both
    // pointless and visible.
    const bool aiming = cam != nullptr && !cam->follow_target.empty() && m_playing &&
                        cam->mouse_look && m_input_source != nullptr;
    if (m_input_source != nullptr) {
        m_input_source->set_pointer_capture(aiming);
    }

    if (cam == nullptr || cam->follow_target.empty()) {
        // No camera, or a camera that follows nothing: the scene's own
        // camera (its Transform, looking at the origin) stays in charge.
        return;
    }

    // --- Aiming ---------------------------------------------------------
    //
    // The orbit used to be whatever the scene authored. Now the player
    // turns it, and the component is where the turn lives: play mode clones
    // the scene, so a camera aimed during play is thrown away when play
    // stops, and an authored angle is never overwritten by a mouse that
    // happened to be moving in the editor.
    // --- Aiming down sights ----------------------------------------------
    //
    // The blend is stepped EVERY frame, not only while aiming: releasing the
    // button has to ease back out, and a paused frame (dt <= 0) must not
    // advance it. `aim` is an ordinary action, so a scene can put it on a
    // mouse button, a pad trigger or a key without the engine knowing which.
    const bool wants_aim = m_playing && cam->ads_enabled && m_input_source != nullptr &&
                           m_input_source->action_pressed("aim");
    m_ads_blend = runtime::aim_blend_step(m_ads_blend, wants_aim, cam->ads_speed, dt);
    // The player's ADS trim rides on top of the scene's scale, so a player can
    // slow the sight picture further without the level knowing.
    const f32 aim_scale = runtime::aim_sensitivity_scale(
        cam->ads_sensitivity * m_player_settings.ads_scale, m_ads_blend);
    const f32 aim_dist = runtime::aim_distance(cam->distance, cam->ads_distance, m_ads_blend);

    // The shake clock advances with the frame, whether or not anything is
    // being aimed: trauma has to decay even while the player stands still.
    m_camera_shake.set_decay_per_second(cam->shake_decay);
    m_camera_shake.update(dt);

    // Recoil recovers every frame, whether or not the player is aiming: a
    // burst fired from the hip has to settle too. The offset is applied to
    // the VIEW below and never written into cam->yaw/pitch, which is what
    // makes recovery land on the aim the player built.
    m_recoil = runtime::recoil_recover(m_recoil, cam->recoil_recover, dt);
    const f32 view_yaw = runtime::look_wrap_deg(cam->yaw + m_recoil.yaw);
    const f32 view_pitch =
        std::clamp(cam->pitch + m_recoil.pitch, cam->pitch_min, cam->pitch_max);

    if (aiming) {
        LookSettings look;
        // While aiming, the same hand movement turns the camera less — that
        // is the whole point of a sight picture, and the scene says by how
        // much (1.0 = no change). The player's own scale multiplies the
        // scene's, so "faster mouse" applies to both states at once.
        look.mouse_sensitivity = cam->look_sensitivity * aim_scale * m_player_settings.look_scale;
        look.stick_sensitivity = cam->stick_sensitivity * aim_scale * m_player_settings.stick_scale;
        look.pitch_min = cam->pitch_min;
        look.pitch_max = cam->pitch_max;
        // Three-state on purpose: "the player has no opinion" must not read as
        // "the player wants it off", so the scene's choice survives.
        switch (m_player_settings.invert_y) {
        case runtime::InvertY::On: look.invert_y = true; break;
        case runtime::InvertY::Off: look.invert_y = false; break;
        case runtime::InvertY::Scene:
        default: look.invert_y = cam->invert_look_y; break;
        }

        LookState state{cam->yaw, cam->pitch};
        bool aimed = false;

        f32 dx = 0.0f;
        f32 dy = 0.0f;
        if (m_input_source->look_delta(dx, dy) && (dx != 0.0f || dy != 0.0f)) {
            state = look_apply_mouse(state, dx, dy, look);
            aimed = true;
        }
        const f32 stick_x = m_input_source->action_axis("look_x");
        const f32 stick_y = m_input_source->action_axis("look_y");
        if (stick_x != 0.0f || stick_y != 0.0f) {
            state = look_apply_stick(state, stick_x, stick_y, dt, look);
            aimed = true;
        }

        const f32 wheel = m_input_source->wheel_delta();
        if (wheel != 0.0f) {
            cam->distance = follow_zoom(cam->distance, wheel, cam->zoom_step, cam->zoom_min,
                                        cam->zoom_max);
        }

        // Auto-align: the camera drifts back behind the character while it
        // runs and the player is not aiming. Only when the player IS moving:
        // a camera that recentres while standing still fights the player's
        // last deliberate look.
        if (cam->align_speed > 0.0f && !aimed) {
            const f32 mx = m_input_source->action_axis("move_x");
            const f32 mz = m_input_source->action_axis("move_z");
            if (mx * mx + mz * mz > 0.04f) {
                f32 world_x = 0.0f;
                f32 world_z = 0.0f;
                move_relative(mx, mz, state.yaw, world_x, world_z);
                const f32 wanted = look_yaw_for_direction(world_x, world_z, state.yaw);
                state.yaw = look_align(state.yaw, wanted, cam->align_speed, dt);
            }
        }

        cam->yaw = state.yaw;
        cam->pitch = state.pitch;
    }

    const ecs::Entity target = find_entity_by_name(world, cam->follow_target);
    if (target == ecs::kInvalidEntity) {
        // The named entity is not there. The camera keeps its last position
        // instead of snapping to the origin: a camera that teleports because
        // a name is wrong is worse than one that waits for the entity.
        m_follow_active = m_follow_camera.initialised;
        return;
    }
    const scene::Transform* t = world.get<scene::Transform>(target);
    if (t == nullptr) {
        m_follow_active = m_follow_camera.initialised;
        return;
    }

    // The pivot: the target's position plus the offset, in the target's own
    // space, so the pivot stays on the character's head when the character
    // turns.
    const Vec3 offset{cam->pivot_x, cam->pivot_y, cam->pivot_z};
    const Vec3 rotated = t->world_rot.rotate(offset);
    const Vec3 pivot{t->world_x + rotated.x, t->world_y + rotated.y, t->world_z + rotated.z};

    // Where the camera wants to be, and where it looks. The distance is the
    // ADS-blended one: aiming pulls the camera in without changing the
    // authored `distance`, so releasing the button returns to exactly the
    // framing the player had before.
    Vec3 desired = runtime::follow_camera_position(pivot, view_yaw, view_pitch, aim_dist);

    // The collision pull-in: something solid between the pivot and the camera
    // moves the camera in front of it. Without this the camera ends up
    // inside a tree and the player sees the inside of a trunk.
    if (cam->avoid_collision && cam->collision_radius > 0.0f) {
        const Vec3 delta = desired - pivot;
        const f32 length = delta.length();
        if (length > 1e-4f) {
            const Vec3 direction = delta / length;
            const auto obstacles = collect_camera_obstacles(world);
            const f32 hit = camera_obstacle_hit(obstacles, pivot, direction, length,
                                                cam->collision_radius);
            if (hit >= 0.0f) {
                desired = runtime::follow_camera_avoid(pivot, desired, hit, cam->collision_radius,
                                                      cam->collision_radius * 2.0f);
            }
        }
    }

    const f32 blend = runtime::follow_camera_blend(cam->smoothing, dt);
    if (!m_follow_camera.initialised) {
        // The first frame: snap. A camera that eases in from the origin
        // spends the first second of every game flying across the map.
        m_follow_camera.position = desired;
        m_follow_camera.target = pivot;
        m_follow_camera.initialised = true;
    } else {
        const Vec3 position = m_follow_camera.position + (desired - m_follow_camera.position) * blend;
        const Vec3 look = m_follow_camera.target + (pivot - m_follow_camera.target) * blend;
        m_follow_camera.position = position;
        m_follow_camera.target = look;
    }
    m_follow_active = true;
}

void Runtime::advance_time_of_day(f32 dt) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene || dt <= 0.0f) {
        return;
    }
    // Only the enabled component drives, matching the "first one wins" rule every
    // other singleton component here follows (Sky, DirectionalLight). A scene
    // with two would silently fight over the same clock.
    auto& world = m_scene_data_ptr->scene->world();
    TimeOfDayComponent* tod = find_time_of_day_mutable(world);
    if (tod == nullptr) {
        return;
    }
    // The clock is rebuilt from the component's authored state each frame rather
    // than kept as a live object, so the value the scene holds and the value
    // driving the render can never drift apart — there is only one of them. The
    // component is the single source of truth, which is also why a save
    // mid-cycle round-trips the exact hour.
    rendering::TimeOfDay clock;
    clock.set_time_hours(tod->time_hours);
    clock.set_day_length_seconds(tod->day_length_seconds);
    clock.advance(dt);
    tod->time_hours = clock.time_hours();
}

const TimeOfDayComponent* Runtime::find_time_of_day(const ecs::World& world) {
    // The enabled component, or null when the scene has none or has it switched
    // off. One lookup shared by extract_light and extract_sky so the two cannot
    // disagree about which clock — and therefore which hour — is in force.
    for (auto e : world.query<TimeOfDayComponent>()) {
        const auto* tod = world.get<TimeOfDayComponent>(e);
        if (tod != nullptr && tod->enabled) {
            return tod;
        }
    }
    return nullptr;
}

TimeOfDayComponent* Runtime::find_time_of_day_mutable(ecs::World& world) {
    // Same rule as the const overload, so the clock that advances is always the
    // one that renders. Duplicated rather than const_cast on purpose: casting
    // away constness here would be a lie about what advance_time_of_day does.
    for (auto e : world.query<TimeOfDayComponent>()) {
        auto* tod = world.get<TimeOfDayComponent>(e);
        if (tod != nullptr && tod->enabled) {
            return tod;
        }
    }
    return nullptr;
}

void Runtime::extract_light() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    // A TimeOfDay component, when enabled and driving the light, supplies the sun
    // instead of the authored DirectionalLight — but only its colour, intensity
    // and direction. The shadow tuning stays the authored one: a scene that
    // authored cascade count and shadow distance chose those for its own
    // geometry, and a day/night cycle has no opinion about them.
    const TimeOfDayComponent* tod = find_time_of_day(world);
    for (auto e : world.query<DirectionalLight>()) {
        const auto* l = world.get<DirectionalLight>(e);
        if (l == nullptr) {
            continue;
        }
        rendering::DirectionalLight rl{};
        rl.direction = {l->dir_x, l->dir_y, l->dir_z};
        rl.color = {l->color_r, l->color_g, l->color_b};
        rl.intensity = l->intensity;
        if (tod != nullptr && tod->drive_light) {
            rendering::TimeOfDay clock;
            clock.set_time_hours(tod->time_hours);
            clock.set_day_length_seconds(tod->day_length_seconds);
            const rendering::DirectionalLight sun = clock.make_light();
            rl.direction = sun.direction;
            rl.color = sun.color;
            rl.intensity = sun.intensity;
            // The sun below the horizon must not light the scene from underneath.
            // TimeOfDay already dims it toward moonlight; this stops the light
            // itself entirely once the elevation is negative, which is what keeps
            // a night scene from showing a shadow cast upward.
            rl.enabled = clock.sun_elevation() > 0.0f;
        }
        rl.shadows_enabled = l->cast_shadows;
        rl.shadow_strength = l->shadow_strength;
        rl.shadow_bias = l->shadow_bias;
        rl.shadow_cascades = l->shadow_cascades;
        rl.shadow_distance = l->shadow_distance;
        m_renderer->set_directional_light(rl);
        return;
    }
    // No light in the scene: keep the renderer's default directional light.
}

void Runtime::extract_local_lights() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    const LocalLights lights = collect_local_lights(m_scene_data_ptr->scene->world());
    // Cleared and refilled every frame. The renderer keeps lights in a slot
    // array, so a light that vanished from the world has to be taken out of the
    // array by putting the whole set back; and a per-frame rebuild is also what
    // lets a lamp destroyed by gameplay or switched off in the inspector stop
    // lighting the scene on the very next frame.
    m_renderer->clear_point_lights();
    m_renderer->clear_spot_lights();
    for (const rendering::PointLight& pl : lights.points) {
        m_renderer->add_point_light(pl);
    }
    for (const rendering::SpotLight& sl : lights.spots) {
        m_renderer->add_spot_light(sl);
    }
    if (lights.dropped_points != 0 || lights.dropped_spots != 0) {
        // Once per scene, not once per frame: this repeats every frame while the
        // level author watches, and 90 identical lines a second buries the
        // warning along with everything else in the log.
        if (!m_local_light_warning_shown) {
            m_local_light_warning_shown = true;
            NF_LOG_WARN(LogCategory::Core,
                        "Runtime: scene asks for {} more point and {} more spot light(s) than the "
                        "renderer has slots for; those lights are not drawn this frame",
                        lights.dropped_points, lights.dropped_spots);
        }
    }
}

void Runtime::apply_combat_feedback_lights() {
    if (!m_feedback_bound || !m_renderer || !m_renderer_initialized) {
        return;
    }
    const std::vector<runtime::FeedbackLight>& lights = m_feedback.lights();
    if (lights.empty()) {
        return;
    }
    // The scene's authored lights fill the slots first — extract_local_lights
    // ran immediately before this — so a level that already uses all eight keeps
    // the lighting its author placed. A flash that loses the race is a missing
    // highlight on a wall, not a level that renders wrong; it is not a warning
    // either, because a muzzle flash is a per-frame transient and a log line per
    // shot would bury the console.
    for (const runtime::FeedbackLight& light : lights) {
        if (m_renderer->point_light_count() >= rendering::Renderer3D::kMaxPointLights) {
            return;
        }
        rendering::PointLight point{};
        point.position = light.position;
        point.color = light.color;
        point.intensity = light.intensity;
        point.radius = light.radius;
        // A flash that casts shadows would cost six depth renders for five
        // hundredths of a second, and the shadow map would not even be sampled
        // before the light is gone.
        point.shadows_enabled = false;
        m_renderer->add_point_light(point);
    }
}

void Runtime::extract_sky() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    // An enabled TimeOfDay supplies the whole palette: the cycle exists to move
    // the sky, and leaving the authored colours in place would make it drive the
    // sun across a noon-blue sky — the one result that looks broken rather than
    // merely wrong.
    const TimeOfDayComponent* tod = find_time_of_day(world);
    if (tod != nullptr) {
        rendering::TimeOfDay clock;
        clock.set_time_hours(tod->time_hours);
        clock.set_day_length_seconds(tod->day_length_seconds);
        m_renderer->set_sky(clock.make_sky());
        return;
    }
    for (auto e : world.query<SkyComponent>()) {
        const auto* s = world.get<SkyComponent>(e);
        if (s == nullptr) {
            continue;
        }
        rendering::SkyParams sky;
        sky.zenith = {s->zenith_r, s->zenith_g, s->zenith_b};
        sky.horizon = {s->horizon_r, s->horizon_g, s->horizon_b};
        sky.ground = {s->ground_r, s->ground_g, s->ground_b};
        sky.clear = {s->clear_r, s->clear_g, s->clear_b};
        sky.sun_disk = s->sun_disk;
        sky.sun_glow = s->sun_glow;
        sky.enabled = s->enabled;
        m_renderer->set_sky(sky);
        return;
    }
    // No sky in the scene: keep the renderer's default sky.
}

void Runtime::extract_post_process() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<PostProcessComponent>()) {
        const auto* pp = world.get<PostProcessComponent>(e);
        if (pp == nullptr) {
            continue;
        }
        rendering::PostFxParams params;
        params.bloom.enabled = pp->bloom_enabled;
        params.bloom.threshold = pp->bloom_threshold;
        params.bloom.knee = pp->bloom_knee;
        params.bloom.intensity = pp->bloom_intensity;
        params.bloom.radius = pp->bloom_radius;
        params.grade.enabled = pp->grade_enabled;
        params.grade.contrast = pp->grade_contrast;
        params.grade.pivot = pp->grade_pivot;
        params.grade.temperature = pp->grade_temperature;
        params.grade.tint = pp->grade_tint;
        params.grade.gamma = pp->grade_gamma;
        params.sharpen.enabled = pp->sharpen_enabled;
        params.sharpen.amount = pp->sharpen_amount;
        params.sharpen.radius = pp->sharpen_radius;
        params.saturation = pp->saturation;
        params.vignette = pp->vignette;
        params.lens.enabled = pp->lens_enabled;
        params.lens.distortion = pp->lens_distortion;
        params.lens.chromatic_aberration = pp->lens_chromatic_aberration;
        params.dof.enabled = pp->dof_enabled;
        params.dof.focus_distance = pp->dof_focus_distance;
        params.dof.focus_range = pp->dof_focus_range;
        params.dof.max_radius = pp->dof_max_radius;
        params.motion.enabled = pp->motion_enabled;
        params.motion.intensity = pp->motion_intensity;
        params.motion.max_length = pp->motion_max_length;
        params.lut_strength = pp->lut_strength;
        m_renderer->set_postfx(params);
        // Anti-aliasing is not part of `PostFxParams` either: it is a separate
        // pass on the tonemapped image, not a stage inside the tonemap shader.
        // Written unconditionally from the component so toggling the box off
        // turns the pass off on the next frame, which is the behaviour an
        // inspector checkbox promises.
        m_renderer->set_fxaa_enabled(pp->fxaa_enabled);
        m_renderer->set_fxaa_thresholds(pp->fxaa_threshold, pp->fxaa_threshold_min);
        // Internal 3D resolution (Phase 37). Applied every frame because the
        // editor can edit the component live; set_render_scale is a no-op when
        // the value did not change, so this does not rebuild targets per frame.
        //
        // Phase 38: when the scene enables the governor, `render_scale` is the
        // CEILING and the governor's current scale is what the renderer gets.
        // Configuring here (rather than on load) means an inspector edit takes
        // effect on the next frame, and a scene without a PostProcess component
        // leaves the previous scale alone. The governor itself is advanced by
        // Runtime::update(dt), which owns the frame clock.
        // Phase 40: `dynamic_resolution_timing=gpu` asks the governor to use
        // real timestamps. That is the only scene-owned reason to turn frame
        // timing on; the per-pass breakdown stays a developer switch because it
        // drains at every pass boundary.
        const bool wants_gpu_timing =
            pp->dynamic_resolution &&
            pp->dynamic_resolution_timing == kDynamicResolutionTimingGpu;
        if (wants_gpu_timing != m_gpu_frame_timing_required) {
            m_gpu_frame_timing_required = wants_gpu_timing;
            // Require a sample published AFTER the switch. The current stats
            // may describe a frame rendered while the CPU clock was selected.
            m_last_gpu_sample_serial =
                m_renderer ? m_renderer->last_stats().gpu_sample_serial : 0;
            apply_gpu_timing_state();
            NF_LOG_INFO(LogCategory::Core, "Dynamic resolution timing: {}",
                        wants_gpu_timing
                            ? "GPU frame timestamps (CPU fallback until a sample is ready)"
                            : "CPU frame time");
        }
        rendering::DynamicResolutionConfig dynamic{};
        dynamic.enabled = pp->dynamic_resolution;
        dynamic.max_scale = pp->render_scale;
        dynamic.min_scale = pp->min_render_scale;
        dynamic.target_frame_ms =
            pp->target_fps > 0.0f ? 1000.0f / pp->target_fps : 16.6667f;
        dynamic.timing = pp->dynamic_resolution_timing == kDynamicResolutionTimingGpu
                             ? rendering::DynamicResolutionTiming::Gpu
                             : rendering::DynamicResolutionTiming::Cpu;
        m_dynamic_resolution.configure(dynamic);

        // The window policy reuses the same ceiling/floor/target as the
        // governor: `render_scale` is where it may return to, `min_render_scale`
        // is where it stops, and `target_fps` is what the 1% low is measured
        // against. Nothing new to author, and a scene can switch sources
        // without re-tuning.
        m_frame_policy_active = pp->frame_policy;
        if (m_frame_policy_active) {
            rendering::FramePolicyConfig policy{};
            policy.target_fps = pp->target_fps > 0.0f ? pp->target_fps : 60.0f;
            policy.min_scale = pp->min_render_scale;
            policy.max_scale = pp->render_scale;
            m_frame_policy_config = policy;
            // A scene authored at 0.8 that enables the policy starts at 0.8,
            // not at the state's default 1.0.
            m_frame_policy_state.scale =
                std::clamp(m_frame_policy_state.scale, policy.min_scale, policy.max_scale);
        }
        const f32 requested_scale =
            m_frame_policy_active
                ? m_frame_policy_state.scale
                : (pp->dynamic_resolution ? m_dynamic_resolution.scale() : pp->render_scale);
        if (requested_scale != m_renderer->render_scale()) {
            NF_LOG_INFO(LogCategory::Render, "Render scale {:.2f} -> {:.2f}{}",
                        m_renderer->render_scale(), requested_scale,
                        pp->dynamic_resolution ? " (dynamic resolution)" : "");
        }
        m_renderer->set_render_scale(requested_scale);
        // Exposure and the tonemap operator are OPT-IN, and they are applied
        // AFTER the block because they are not part of `PostFxParams` — the
        // renderer holds them separately, and a scene that does not name them
        // must leave them exactly as they were.
        if (pp->exposure > 0.0f) {
            m_renderer->set_exposure(pp->exposure);
        }
        if (pp->tonemap >= 0) {
            m_renderer->set_tonemap_mode(static_cast<rendering::TonemapMode>(pp->tonemap));
        }
        // The colour LUT is a TEXTURE rather than a number, so it is resolved
        // through the same path-keyed cache every other texture uses — and only
        // when the path CHANGES, because this runs every frame.
        if (pp->lut_path != m_lut_path) {
            m_lut_path = pp->lut_path;
            m_lut_view = nullptr;
            if (!m_lut_path.empty()) {
                std::string lut_err;
                if (!ensure_texture(m_lut_path, lut_err)) {
                    NF_LOG_WARN(LogCategory::Core,
                                "Runtime: colour LUT '{}' unusable ({}); the LUT stage is off",
                                m_lut_path, lut_err);
                } else {
                    const auto it = m_textures.find(m_lut_path);
                    if (it != m_textures.end() && it->second && it->second->view) {
                        // The shader reads a kLutSize^3 cube laid out as a strip.
                        // A wrong-sized image would be sampled out of range, so
                        // it is refused with a warning rather than bound.
                        const rhi::TextureView* view = it->second->view.get();
                        if (view->width() == rendering::kLutSize * rendering::kLutSize &&
                            view->height() == rendering::kLutSize) {
                            m_lut_view = view;
                        } else {
                            NF_LOG_WARN(LogCategory::Core,
                                        "Runtime: colour LUT '{}' is {}x{}; a {}x{} strip is required",
                                        m_lut_path, view->width(), view->height(),
                                        rendering::kLutSize * rendering::kLutSize,
                                        rendering::kLutSize);
                        }
                    }
                }
            }
        }
        // Distance fog is part of the same authored block even though the
        // renderer applies it in the lighting pass. Clamp the band into order:
        // a start at or past the end would make the fade divide by zero, and a
        // hand-edited file is allowed to say something the shader must survive.
        rendering::Renderer3D::FogParams fog{};
        fog.enabled = pp->fog_enabled;
        fog.color = {pp->fog_r, pp->fog_g, pp->fog_b};
        fog.start = pp->fog_start;
        fog.end = std::max(pp->fog_start + 0.01f, pp->fog_end);
        m_renderer->set_fog(fog);
        // Bound every frame: the pointer is a plain store, and it must be
        // re-established after the renderer is recreated.
        m_renderer->set_color_lut(m_lut_view);
        return;
    }
    // No post block in the scene: keep the renderer's neutral defaults. Not
    // "reset to neutral" — a caller that configured the renderer directly
    // (a sample) must not have that configuration erased by a scene that simply
    // says nothing about post-processing.
}

void Runtime::build_render_world(rendering::RenderWorld& out) {
    out.clear();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!m_mesh_library) {
        return;
    }
    ensure_default_material();
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<scene::Transform, MeshComponent>()) {
        const auto* tr = world.get<scene::Transform>(e);
        const auto* mc = world.get<MeshComponent>(e);
        if (tr == nullptr || mc == nullptr) {
            continue;
        }
        rendering::StaticMeshHandle mesh_handle;
        if (mc->mesh_id.valid()) {
            auto it = m_mesh_handles.find(mc->mesh_id);
            if (it == m_mesh_handles.end()) {
                // Asset still loading or failed: skip safely (no crash).
                continue;
            }
            mesh_handle = it->second;
        } else {
            // A RIGGED CHARACTER: its geometry comes from the rig, not from an
            // asset id, and its mesh is its OWN (a skinned mesh deforms
            // differently every frame, so it cannot share the library's copy
            // of the same file). Without this the character is loaded,
            // animated and never drawn.
            const auto* ch = world.get<CharacterComponent>(e);
            if (ch == nullptr || !ch->loaded || !ch->mesh.valid()) {
                continue;
            }
            mesh_handle = ch->mesh;
        }
        const rendering::StaticMesh* mesh = m_mesh_library->get(mesh_handle);
        if (mesh == nullptr || !mesh->is_uploaded()) {
            continue;
        }
        rendering::RenderObject ro{};
        ro.id = e.id;
        ro.visible = true;
        ro.transform.x = tr->world_x;
        ro.transform.y = tr->world_y;
        ro.transform.z = tr->world_z;
        // The FULL world matrix, from the propagated world pose — not
        // world_pos with the LOCAL rotation and scale bolted on. Those agree
        // for a root and disagree for a child: a prefab instance under a rotated
        // parent was drawn with the parent's translation but the child's own
        // unrotated-by-the-parent orientation, so the mesh visibly detached from
        // its parent when the parent turned. world_matrix() is the one place a
        // Transform becomes a matrix, so this path, extract_render_objects, the
        // editor's bounds and the gizmo all agree.
        ro.world = scene::world_matrix(*tr);
        ro.mesh_handle = mesh_handle;
        // Shared material assignment: same path shares one renderer instance.
        ro.material_handle = material_for_path(mc->material);
        if (!ro.material_handle.valid()) {
            ro.material_handle = m_default_material;
        }
        ro.lod = 0;
        // World-space bounds for frustum culling, through the SAME matrix the
        // draw uses. The previous translation-only box under-reported every
        // rotated and scaled object, and both the camera frustum and the shadow
        // cascades reject on it — so a scaled ground plane was culled while
        // filling the screen. The matrix forms re-fit the AABB around the 8
        // transformed corners and scale the sphere by the largest axis, both
        // conservative: the volume can only grow, so nothing is wrongly dropped.
        ro.bounds = rendering::transform_aabb(mesh->bounds(), ro.world);
        ro.sphere = rendering::transform_sphere(mesh->bounding_sphere(), ro.world);
        out.objects.push_back(std::move(ro));
    }
    build_debris_render_world(out);
    build_vehicle_wheels_render_world(out);
    build_combat_feedback_render_world(out);

    // Projected decals (Phase 36). Placement and facing come from the entity
    // Transform; the authored size is baked into the matrix here so the GPU's
    // local-space test stays the unit cube. A disabled decal is not extracted
    // at all: the renderer must not spend a draw on something the author
    // explicitly turned off, and `decal_count()` then reports what will draw.
    for (auto e : world.query<scene::Transform, DecalComponent>()) {
        const auto* tr = world.get<scene::Transform>(e);
        const auto* dc = world.get<DecalComponent>(e);
        if (tr == nullptr || dc == nullptr || !dc->enabled) {
            continue;
        }
        const Mat4 world_matrix =
            rendering::decal_world_matrix(scene::world_matrix(*tr),
                                          Vec3{dc->size_x, dc->size_y, dc->size_z});
        rendering::MaterialHandle mat = material_for_path(dc->material);
        if (!mat.valid()) {
            mat = m_default_material;
        }
        out.decals.push_back(rendering::make_decal(e.id, world_matrix, mat, dc->opacity));
    }

    NF_LOG_TRACE(LogCategory::Core, "Runtime: built render world with {} objects (meshes={})", out.objects.size(),
                 m_mesh_library->size());
}

void Runtime::fallback_clear(rhi::Texture& target, rhi::CommandBuffer& cmd, bool present_source) {
    // Distinct magenta: unmistakably a failure fallback, never a success color.
    // Resources are retained until shutdown() so the recorded command buffer
    // never references destroyed VkRenderPass/VkFramebuffer (the previous bug).
    rhi::ColorAttachment ca{};
    ca.format = target.format();
    const std::array<rhi::ColorAttachment, 1> atts{ca};
    rhi::RenderPassDesc rpd{};
    rpd.color_attachments = std::span<const rhi::ColorAttachment>(atts);
    rpd.present_source = present_source;
    auto rp = m_device.create_render_pass(rpd);
    if (!rp) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: fallback render pass creation failed");
        return;
    }
    const std::array<rhi::Texture*, 1> fb_tex{&target};
    auto fb = m_device.create_framebuffer(*rp, std::span<rhi::Texture* const>(fb_tex), nullptr);
    if (!fb) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: fallback framebuffer creation failed");
        return;
    }
    const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{1.0f, 0.0f, 1.0f, 1.0f}};
    cmd.begin_render_pass(*rp, *fb, std::span<const rhi::ClearValue>(clears));
    cmd.end_render_pass();
    // Retain: destruction happens in shutdown() after wait_idle().
    m_fallback_rps.push_back(std::move(rp));
    m_fallback_fbs.push_back(std::move(fb));
}

void Runtime::render(uint32_t image_index, rhi::CommandBuffer& cmd, u32 frame_slot) {
    NF_PROFILE_SCOPE("Runtime::render");
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: no scene loaded, using fallback clear");
        if (m_swapchain != nullptr) {
            if (rhi::Texture* t = m_swapchain->get_texture(image_index)) {
                fallback_clear(*t, cmd, true);
            }
        }
        return;
    }
    if (m_swapchain == nullptr) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: no swapchain bound");
        return;
    }
    // Keep mesh uploads flowing even if the caller forgot update().
    sync_meshes_from_assets();
    const uint32_t w = m_swapchain->width();
    const uint32_t h = m_swapchain->height();
    if (!ensure_renderer_initialized_for(w, h)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: renderer init failed, using fallback clear");
        if (rhi::Texture* t = m_swapchain->get_texture(image_index)) {
            fallback_clear(*t, cmd, true);
        }
        return;
    }
    if (m_renderer->width() != w || m_renderer->height() != h) {
        if (w != 0 && h != 0) {
            m_renderer->resize(w, h);
        }
    }
    rendering::Camera cam{};
    extract_camera(w, h, cam);
    extract_light();
    extract_local_lights();
    apply_combat_feedback_lights();
    extract_sky();
    extract_post_process();
    // Reused scratch: build_render_world clears it, so the per-object storage
    // survives frames instead of churning the heap each frame.
    build_render_world(m_render_world);
    // Frustum culling happens inside Renderer3D::render (RenderWorld → Visible).
    rhi::Texture* target = m_swapchain->get_texture(image_index);
    if (target == nullptr) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: null swapchain texture {}", image_index);
        return;
    }
    // Scene → AssetManager → Renderer3D: Depth → GBuffer → Lighting → Tonemap.
    if (!m_renderer->render(cmd, m_render_world, cam, *target, true, frame_slot)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: Renderer3D failed, using fallback clear");
        fallback_clear(*target, cmd, true);
        return;
    }
    const auto& stats = m_renderer->last_stats();
    NF_LOG_TRACE(LogCategory::Core, "Runtime::render: extracted={} visible={} draws={}", stats.extracted,
                 stats.visible, stats.draw_calls);
}

bool Runtime::present_viewport(uint32_t image_index, rhi::CommandBuffer& cmd,
                               rhi::Texture& offscreen, rhi::TextureView& offscreen_view,
                               u32 frame_slot) {
    if (m_swapchain == nullptr) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::present_viewport: no swapchain bound");
        return false;
    }
    rhi::Texture* target = m_swapchain->get_texture(image_index);
    if (target == nullptr) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::present_viewport: null swapchain texture {}", image_index);
        return false;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        // No scene: same visible contract as render() — a magenta fallback.
        fallback_clear(*target, cmd, true);
        return false;
    }
    if (!m_renderer || !m_renderer_initialized) {
        fallback_clear(*target, cmd, true);
        return false;
    }
    if (!m_renderer->present_texture(cmd, offscreen, &offscreen_view, *target, true, frame_slot)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::present_viewport: present_texture failed");
        fallback_clear(*target, cmd, true);
        return false;
    }
    return true;
}

rendering::RenderWorld Runtime::render_world_snapshot() {
    rendering::RenderWorld out;
    build_render_world(out);
    return out;
}

void Runtime::render_offscreen(rhi::Texture& target, rhi::CommandBuffer& cmd, u32 frame_slot) {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render_offscreen: no scene loaded, using fallback clear");
        fallback_clear(target, cmd, false);
        return;
    }
    // No swapchain / PRESENT in headless mode: present_source=false.
    const uint32_t w = target.width();
    const uint32_t h = target.height();
    if (!ensure_renderer_initialized_for(w, h)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render_offscreen: renderer init failed");
        fallback_clear(target, cmd, false);
        return;
    }
    if (m_renderer->width() != w || m_renderer->height() != h) {
        if (w != 0 && h != 0) {
            m_renderer->resize(w, h);
        }
    }
    sync_meshes_from_assets();
    rendering::Camera cam{};
    extract_camera(w, h, cam);
    extract_light();
    extract_local_lights();
    apply_combat_feedback_lights();
    extract_sky();
    extract_post_process();
    build_render_world(m_render_world);
    if (!m_renderer->render(cmd, m_render_world, cam, target, false, frame_slot)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render_offscreen: Renderer3D failed");
        fallback_clear(target, cmd, false);
        return;
    }
    const auto& stats = m_renderer->last_stats();
    NF_LOG_TRACE(LogCategory::Core, "Runtime::render_offscreen: extracted={} visible={} draws={}",
                 stats.extracted, stats.visible, stats.draw_calls);
}

bool Runtime::pick_entity_gpu(uint32_t x, uint32_t y, uint32_t& out_entity_id) {
    out_entity_id = 0;
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return false;
    }

    // Pick at the size the frame was rendered at, so pixel coordinates line up
    // with the image the user clicked on.
    uint32_t w = m_renderer ? m_renderer->width() : 0;
    uint32_t h = m_renderer ? m_renderer->height() : 0;
    if (w == 0 || h == 0) {
        w = 1280;
        h = 720;
    }

    if (!m_picker) {
        m_picker = std::make_unique<rendering::GpuPicker>();
        if (!m_picker->init(m_device, resolve_shader_dir(), w, h)) {
            // No pick shaders, no device support, or a failed RHI object: drop
            // the picker and let the caller fall back to CPU picking rather
            // than leaving a half-built one behind.
            m_picker.reset();
            return false;
        }
    } else if (m_picker->width() != w || m_picker->height() != h) {
        m_device.wait_idle();
        if (!m_picker->resize(w, h)) {
            m_picker.reset();
            return false;
        }
    }

    // Rebuild the same render world the frame builds, so what is pickable is
    // exactly what was drawn — culling and upload state included.
    sync_meshes_from_assets();
    rendering::Camera cam{};
    extract_camera(w, h, cam);
    rendering::RenderWorld world{};
    build_render_world(world);

    auto cmd = m_device.create_command_buffer();
    if (!cmd) {
        return false;
    }
    // Same LOD bands the frame was drawn with, so a distant object simplified
    // out of the image is not pickable through the empty pixels either.
    static const std::vector<float> kNoLodBands;
    const std::vector<float>& lod_bands = m_renderer ? m_renderer->lod_max_distances() : kNoLodBands;
    const rendering::PickHit hit = m_picker->pick(*cmd, world.objects, cam, *m_mesh_library, x, y,
                                                   std::span<const float>(lod_bands));
    if (!hit.hit) {
        return false;
    }
    out_entity_id = hit.object_id;
    return true;
}

// --- Shared material assets -------------------------------------------------

std::string Runtime::normalize_material_path(const std::string& p) {
    // Canonical form always carries the extension: extensionless references
    // (older scenes, hand-typed paths) resolve to the same instance key.
    std::string out = p.empty() ? std::string(kDefaultMaterialPath) : p;
    const std::string ext = ".nfmat";
    if (out.size() < ext.size() || out.compare(out.size() - ext.size(), ext.size(), ext) != 0) {
        out += ext;
    }
    return out;
}

rendering::PBRMaterialParams Runtime::default_material_params() {
    rendering::PBRMaterialParams params{};
    params.base_color[0] = 0.8f;
    params.base_color[1] = 0.8f;
    params.base_color[2] = 0.8f;
    params.base_color[3] = 1.0f;
    params.metallic = 0.0f;
    params.roughness = 0.4f;
    params.ao = 1.0f;
    return params;
}

bool Runtime::load_material_file(const std::string& path,
                                 rendering::PBRMaterialParams& out) const {
    auto read = m_vfs.read_text(path);
    if (!read.ok) {
        return false;
    }
    rendering::MaterialAsset asset;
    std::string err;
    if (!rendering::MaterialAsset::load_from_text(read.value, asset, err)) {
        NF_LOG_WARN(LogCategory::Core, "Runtime: material '{}' unparsable ({}), using defaults", path,
                    err);
        return false;
    }
    out = asset.params;
    return true;
}

rendering::MaterialHandle Runtime::material_for_path(const std::string& logical_path) {
    const std::string path = normalize_material_path(logical_path);
    auto it = m_material_instances.find(path);
    if (it != m_material_instances.end()) {
        return it->second;
    }
    // Init-only when down: never resize here. Sizing belongs to the render
    // entry points (they pass the target size); resizing to the no-swapchain
    // default mid-frame would desync viewport/scissor from the target.
    if (!m_renderer_initialized) {
        if (!ensure_renderer_initialized()) {
            return rendering::kInvalidMaterialHandle;
        }
    }
    ensure_default_material();
    rendering::Material* gmat = (m_renderer != nullptr) ? m_renderer->gbuffer_material() : nullptr;
    if (gmat == nullptr || !gmat->valid()) {
        return rendering::kInvalidMaterialHandle;
    }
    // File params win; a missing/unparsable file falls back to gray defaults
    // (same look as the pre-material era). Resolved once and cached.
    rendering::MaterialAsset file_asset;
    rendering::PBRMaterialParams params = default_material_params();
    std::string file_err;
    bool have_file = false;
    {
        auto read = m_vfs.read_text(path);
        if (read.ok && rendering::MaterialAsset::load_from_text(read.value, file_asset, file_err)) {
            params = file_asset.params;
            have_file = true;
        } else if (path != kDefaultMaterialPath) {
            NF_LOG_WARN(LogCategory::Core, "Runtime: material '{}' missing, using gray fallback", path);
        }
    }
    rendering::MaterialHandle h = m_renderer->materials().create_instance(*gmat, params, path);
    if (!h.valid()) {
        return rendering::kInvalidMaterialHandle;
    }
    m_material_instances.emplace(path, h);
    m_material_mip[path] =
        have_file ? file_asset.mip_mode : rhi::MipMapMode::Linear;
    // Map bindings follow the file; failures stay scalar (never fatal).
    // Record first: rebind_material_sampling reads the recorded bindings.
    m_material_albedo[path] = (have_file ? file_asset.albedo : "");
    m_material_normal[path] = (have_file ? file_asset.normal : "");
    m_material_mrough[path] = (have_file ? file_asset.mrough : "");
    m_material_occlusion[path] = (have_file ? file_asset.occlusion : "");
    m_material_emissive_map[path] = (have_file ? file_asset.emissive : "");
    if (have_file && (!file_asset.albedo.empty() || !file_asset.normal.empty() ||
                      !file_asset.mrough.empty() || !file_asset.occlusion.empty() ||
                      !file_asset.emissive.empty())) {
        std::string tex_err;
        if (!rebind_material_sampling(path, h, tex_err)) {
            NF_LOG_WARN(LogCategory::Core, "Runtime: material '{}' maps unreachable ({}), scalar",
                        path, tex_err);
        }
    }
    return h;
}

bool Runtime::set_material_mip_mode(const std::string& material_path, rhi::MipMapMode mode,
                                    std::string& out_error) {
    const std::string path = normalize_material_path(material_path);
    const int idx = static_cast<int>(mode);
    if (idx < 0 || idx > 2) {
        out_error = "Invalid mip mode";
        return false;
    }
    rendering::MaterialHandle h = material_for_path(path);
    if (!h.valid()) {
        out_error = "Renderer unavailable for material '" + path + "'";
        return false;
    }
    m_material_mip[path] = mode;
    if (!rebind_material_sampling(path, h, out_error)) {
        return false;
    }
    m_material_dirty[path] = true;
    return true;
}

rhi::MipMapMode Runtime::material_mip_mode(const std::string& material_path) const {
    auto it = m_material_mip.find(normalize_material_path(material_path));
    return (it != m_material_mip.end()) ? it->second : rhi::MipMapMode::Linear;
}

bool Runtime::set_material_params(const std::string& logical_path,
                                  const rendering::PBRMaterialParams& params, std::string& out_error) {
    const std::string path = normalize_material_path(logical_path);
    rendering::MaterialHandle h = material_for_path(path);
    if (!h.valid()) {
        out_error = "Renderer unavailable for material '" + path + "'";
        return false;
    }
    m_renderer->materials().set_params(h, params);
    m_material_dirty[path] = true;
    return true;
}

bool Runtime::material_params(const std::string& logical_path,
                              rendering::PBRMaterialParams& out) const {
    const std::string path = normalize_material_path(logical_path);
    auto it = m_material_instances.find(path);
    if (it != m_material_instances.end() && m_renderer != nullptr) {
        if (const rendering::PBRMaterialParams* p = m_renderer->materials().params(it->second)) {
            out = *p;
            return true;
        }
    }
    if (load_material_file(path, out)) {
        return true;
    }
    out = default_material_params();
    return true;
}

std::vector<std::string> Runtime::known_material_paths() const {
    std::vector<std::string> paths;
    for (const auto& kv : m_material_instances) {
        paths.push_back(kv.first);
    }
    auto resolved = m_vfs.resolve("content://Materials");
    if (resolved.ok) {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(resolved.value, ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec) || ec) {
                if (ec) {
                    break;
                }
                continue;
            }
            if (it->path().extension() != ".nfmat") {
                continue;
            }
            paths.push_back("content://Materials/" + it->path().filename().generic_string());
        }
    }
    if (paths.empty()) {
        paths.push_back(kDefaultMaterialPath);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

bool Runtime::save_material(const std::string& src_path, const std::string& dst_path,
                            std::string& out_error) {
    const std::string src = normalize_material_path(src_path);
    const std::string dst = normalize_material_path(dst_path);
    rendering::PBRMaterialParams params = default_material_params();
    if (!material_params(src, params)) {
        out_error = "Unknown material '" + src + "'";
        return false;
    }
    rendering::MaterialAsset asset;
    const size_t slash = dst.find_last_of('/');
    asset.name = (slash == std::string::npos) ? dst : dst.substr(slash + 1);
    {
        const size_t dot = asset.name.find_last_of('.');
        if (dot != std::string::npos) {
            asset.name = asset.name.substr(0, dot);
        }
    }
    asset.params = params;
    asset.albedo = material_albedo(src);
    asset.normal = material_normal(src);
    asset.mrough = material_mrough(src);
    asset.occlusion = material_occlusion(src);
    asset.emissive = material_emissive_map(src);
    asset.mip_mode = material_mip_mode(src);
    auto r = m_vfs.write_text(dst, asset.save_to_text());
    if (!r.ok) {
        out_error = r.error;
        NF_LOG_ERROR(LogCategory::Core, "Runtime: failed to save material '{}': {}", dst, out_error);
        return false;
    }
    if (dst == src) {
        m_material_dirty[src] = false;
    }
    NF_LOG_INFO(LogCategory::Core, "Runtime: material '{}' saved to '{}'", src, dst);
    return true;
}

static u32 full_mip_count(u32 w, u32 h) {
    u32 levels = 1;
    while (w > 1 || h > 1) {
        if (w > 1) {
            w /= 2;
        }
        if (h > 1) {
            h /= 2;
        }
        ++levels;
    }
    return levels;
}

rhi::Sampler* Runtime::sampler_for_mode(TextureObjects& entry, rhi::MipMapMode mode) {
    const int idx = static_cast<int>(mode);
    if (idx < 0 || idx > 2) {
        return nullptr;
    }
    if (!entry.samplers[idx]) {
        rhi::SamplerDesc sd{};
        sd.address_u = rhi::AddressMode::ClampToEdge;
        sd.address_v = rhi::AddressMode::ClampToEdge;
        sd.mip = mode;
        sd.min_lod = 0.0f;
        sd.max_lod = (mode == rhi::MipMapMode::None || entry.mip_levels <= 1)
                         ? 0.0f
                         : static_cast<float>(entry.mip_levels - 1);
        entry.samplers[idx] = m_device.create_sampler(sd);
    }
    return entry.samplers[idx].get();
}

bool Runtime::rebind_material_sampling(const std::string& material_path,
                                       rendering::MaterialHandle handle, std::string& out_error) {
    if (m_renderer == nullptr) {
        out_error = "Renderer unavailable";
        return false;
    }
    // One binder for all five map slots: same pre-validation, same sampler
    // lookup, same setter/clearer shape — only the recorded path and the
    // library methods differ. Five copies of this block would drift the way
    // the old albedo-only copy did when the sampler lookup changed.
    using Lib = rendering::MaterialLibrary;
    using SetFn = void (Lib::*)(rendering::MaterialHandle, const rhi::TextureView&,
                                const rhi::Sampler&);
    using ClearFn = void (Lib::*)(rendering::MaterialHandle);
    auto bind_one = [&](const std::string& tex_path, SetFn set, ClearFn clear) -> bool {
        if (tex_path.empty()) {
            (m_renderer->materials().*clear)(handle);
            return true;
        }
        if (!ensure_texture(tex_path, out_error)) {
            return false;
        }
        auto tit = m_textures.find(tex_path);
        if (tit == m_textures.end() || !tit->second->view) {
            out_error = "Texture objects missing for '" + tex_path + "'";
            return false;
        }
        rhi::Sampler* sampler = sampler_for_mode(*tit->second, material_mip_mode(material_path));
        if (sampler == nullptr) {
            out_error = "Sampler creation failed for '" + tex_path + "'";
            return false;
        }
        (m_renderer->materials().*set)(handle, *tit->second->view, *sampler);
        return true;
    };
    if (!bind_one(material_albedo(material_path), &Lib::set_albedo_texture,
                  &Lib::clear_albedo_texture)) {
        return false;
    }
    if (!bind_one(material_normal(material_path), &Lib::set_normal_texture,
                  &Lib::clear_normal_texture)) {
        return false;
    }
    if (!bind_one(material_mrough(material_path), &Lib::set_mrough_texture,
                  &Lib::clear_mrough_texture)) {
        return false;
    }
    if (!bind_one(material_occlusion(material_path), &Lib::set_occlusion_texture,
                  &Lib::clear_occlusion_texture)) {
        return false;
    }
    if (!bind_one(material_emissive_map(material_path), &Lib::set_emissive_texture,
                  &Lib::clear_emissive_texture)) {
        return false;
    }
    return true;
}

std::unique_ptr<Runtime::TextureObjects> Runtime::upload_texture_objects(
    const std::string& logical_path, const rendering::DecodedImage& img, std::string& out_error) {
    const size_t px = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    if (px == 0 || px > 64u * 1024u * 1024u || !img.ok()) {
        out_error = "Texture '" + logical_path + "' has unreasonable dimensions";
        return nullptr;
    }
    const u32 levels = full_mip_count(static_cast<u32>(img.width), static_cast<u32>(img.height));
    rhi::TextureDesc td{};
    td.width = static_cast<u32>(img.width);
    td.height = static_cast<u32>(img.height);
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.mip_levels = levels;
    td.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst | rhi::ImageUsage::TransferSrc;
    auto tex = m_device.create_texture(td);
    rhi::BufferDesc staging_desc{};
    staging_desc.size = img.rgba.size();
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = m_device.create_buffer(staging_desc);
    if (!tex || !staging) {
        out_error = "GPU allocation failed for texture '" + logical_path + "'";
        return nullptr;
    }
    staging->update(img.rgba.data(), 0, img.rgba.size());
    if (auto upload = m_device.create_upload_context()) {
        upload->copy_buffer_to_texture(*staging, *tex, 0, 0, 0, static_cast<u32>(img.width),
                                       static_cast<u32>(img.height));
        if (auto fence = upload->submit()) {
            fence->wait();
        }
    }
    u32 generated_levels = 1;
    if (auto trans_cmd = m_device.create_command_buffer()) {
        if (auto trans_fence = m_device.create_fence(false)) {
            trans_cmd->begin();
            if (levels <= 1) {
                // Single level: plain upload transition (generate_mipmaps is a
                // no-op below one extra level and would skip the transition).
                trans_cmd->transition_texture_for_sampling(*tex);
            } else if (trans_cmd->generate_mipmaps(*tex)) {
                // Full chain live, every level SHADER_READ already.
                generated_levels = levels;
            } else {
                // Blits unsupported (level 0 is still valid uploaded data):
                // finish the standard upload transition.
                NF_LOG_WARN(LogCategory::Core, "Runtime: mip generation failed for '{}'",
                            logical_path);
                trans_cmd->transition_texture_for_sampling(*tex);
            }
            trans_cmd->end();
            m_device.submit(*trans_cmd, rhi::SubmitInfo{.signal_fence = trans_fence.get()});
            trans_fence->wait();
            // If blits were unsupported the upper levels are garbage: sample
            // level 0 only. Samplers read mip_levels for their max_lod.
            auto entry = std::make_unique<TextureObjects>();
            entry->texture = std::move(tex);
            entry->mip_levels = generated_levels;
            rhi::TextureViewDesc vd{};
            vd.texture = entry->texture.get();
            vd.mip_count = entry->mip_levels;
            entry->view = m_device.create_texture_view(vd);
            if (!entry->view) {
                out_error = "View creation failed for texture '" + logical_path + "'";
                return nullptr;
            }
            return entry;
        }
    }
    out_error = "Command recording failed for texture '" + logical_path + "'";
    return nullptr;
}

bool Runtime::ensure_texture(const std::string& logical_path, std::string& out_error) {
    auto it = m_textures.find(logical_path);
    if (it != m_textures.end() && it->second && it->second->texture) {
        return true;
    }
    auto resolved = m_vfs.resolve(logical_path);
    if (!resolved.ok) {
        out_error = "Cannot resolve texture '" + logical_path + "': " + resolved.error;
        return false;
    }
    std::string dec_err;
    rendering::DecodedImage img = rendering::decode_image_file(resolved.value.string(), dec_err);
    if (!img.ok()) {
        out_error = "Cannot decode texture '" + logical_path + "': " + dec_err;
        return false;
    }
    auto entry = upload_texture_objects(logical_path, img, out_error);
    if (!entry) {
        return false;
    }
    NF_LOG_INFO(LogCategory::Core, "Runtime: texture uploaded '{}' ({}x{})", logical_path, img.width,
                img.height);
    m_textures[logical_path] = std::move(entry);
    return true;
}

bool Runtime::reload_texture(const std::string& logical_path, std::string& out_error) {
    // Build the replacement first: any failure keeps the old texture live, so
    // materials never dangle.
    auto resolved = m_vfs.resolve(logical_path);
    if (!resolved.ok) {
        out_error = "Cannot resolve texture '" + logical_path + "': " + resolved.error;
        return false;
    }
    std::string dec_err;
    rendering::DecodedImage img = rendering::decode_image_file(resolved.value.string(), dec_err);
    if (!img.ok()) {
        out_error = "Cannot decode texture '" + logical_path + "': " + dec_err;
        return false;
    }
    auto entry = upload_texture_objects(logical_path, img, out_error);
    if (!entry) {
        return false;
    }
    // Swap under wait_idle: in-flight frames may still reference the old view.
    m_device.wait_idle();
    m_textures[logical_path] = std::move(entry);
    // Rebind every material sampling this path (raw pointers changed), each
    // with its own recorded mip mode. A texture can sit in any of the five
    // map slots, so every slot map is consulted — checking only the albedo
    // map would leave a reloaded normal map bound to freed objects.
    if (m_renderer != nullptr) {
        auto uses_path = [&](const std::string& mat) {
            return material_albedo(mat) == logical_path || material_normal(mat) == logical_path ||
                   material_mrough(mat) == logical_path || material_occlusion(mat) == logical_path ||
                   material_emissive_map(mat) == logical_path;
        };
        for (const auto& mit : m_material_instances) {
            if (!uses_path(mit.first)) {
                continue;
            }
            std::string rerr;
            if (!rebind_material_sampling(mit.first, mit.second, rerr)) {
                NF_LOG_WARN(LogCategory::Core, "Runtime: rebind after reload failed ({})", rerr);
            }
        }
    }
    NF_LOG_INFO(LogCategory::Core, "Runtime: texture reloaded '{}'", logical_path);
    return true;
}

bool Runtime::set_material_albedo(const std::string& material_path, const std::string& texture_path,
                                  std::string& out_error) {
    const std::string path = normalize_material_path(material_path);
    rendering::MaterialHandle h = material_for_path(path);
    if (!h.valid() || m_renderer == nullptr) {
        out_error = "Renderer unavailable for material '" + path + "'";
        return false;
    }
    if (texture_path.empty()) {
        m_renderer->materials().clear_albedo_texture(h);
        m_material_albedo[path] = "";
        m_material_dirty[path] = true;
        return true;
    }
    // Pre-validate (warms the cache): a bad file fails here, before the map
    // or the renderer is touched.
    if (!ensure_texture(texture_path, out_error)) {
        return false;
    }
    m_material_albedo[path] = texture_path;
    if (!rebind_material_sampling(path, h, out_error)) {
        return false;
    }
    m_material_dirty[path] = true;
    return true;
}

std::string Runtime::material_albedo(const std::string& material_path) const {
    auto it = m_material_albedo.find(normalize_material_path(material_path));
    return (it != m_material_albedo.end()) ? it->second : std::string{};
}

// The four PBR map slots share one implementation shape with the albedo
// pair above: record-then-rebind on set, recorded path on get, empty string
// for "scalar". Only the map and the library methods differ.
bool Runtime::set_material_map(const std::string& material_path, const std::string& texture_path,
                               std::unordered_map<std::string, std::string>& slot_map,
                               void (rendering::MaterialLibrary::*clear)(
                                   rendering::MaterialHandle),
                               std::string& out_error) {
    const std::string path = normalize_material_path(material_path);
    rendering::MaterialHandle h = material_for_path(path);
    if (!h.valid() || m_renderer == nullptr) {
        out_error = "Renderer unavailable for material '" + path + "'";
        return false;
    }
    if (texture_path.empty()) {
        (m_renderer->materials().*clear)(h);
        slot_map[path] = "";
        m_material_dirty[path] = true;
        return true;
    }
    if (!ensure_texture(texture_path, out_error)) {
        return false;
    }
    slot_map[path] = texture_path;
    if (!rebind_material_sampling(path, h, out_error)) {
        return false;
    }
    m_material_dirty[path] = true;
    return true;
}

bool Runtime::set_material_normal(const std::string& material_path, const std::string& texture_path,
                                  std::string& out_error) {
    return set_material_map(material_path, texture_path, m_material_normal,
                             &rendering::MaterialLibrary::clear_normal_texture, out_error);
}

std::string Runtime::material_normal(const std::string& material_path) const {
    auto it = m_material_normal.find(normalize_material_path(material_path));
    return (it != m_material_normal.end()) ? it->second : std::string{};
}

bool Runtime::set_material_mrough(const std::string& material_path, const std::string& texture_path,
                                  std::string& out_error) {
    return set_material_map(material_path, texture_path, m_material_mrough,
                             &rendering::MaterialLibrary::clear_mrough_texture, out_error);
}

std::string Runtime::material_mrough(const std::string& material_path) const {
    auto it = m_material_mrough.find(normalize_material_path(material_path));
    return (it != m_material_mrough.end()) ? it->second : std::string{};
}

bool Runtime::set_material_occlusion(const std::string& material_path,
                                     const std::string& texture_path,
                                     std::string& out_error) {
    return set_material_map(material_path, texture_path, m_material_occlusion,
                             &rendering::MaterialLibrary::clear_occlusion_texture, out_error);
}

std::string Runtime::material_occlusion(const std::string& material_path) const {
    auto it = m_material_occlusion.find(normalize_material_path(material_path));
    return (it != m_material_occlusion.end()) ? it->second : std::string{};
}

bool Runtime::set_material_emissive_map(const std::string& material_path,
                                        const std::string& texture_path,
                                        std::string& out_error) {
    return set_material_map(material_path, texture_path, m_material_emissive_map,
                             &rendering::MaterialLibrary::clear_emissive_texture, out_error);
}

std::string Runtime::material_emissive_map(const std::string& material_path) const {
    auto it = m_material_emissive_map.find(normalize_material_path(material_path));
    return (it != m_material_emissive_map.end()) ? it->second : std::string{};
}

std::vector<std::string> Runtime::known_texture_paths() const {
    std::vector<std::string> out;
    auto collect = [&](const std::unordered_map<std::string, std::string>& slot_map) {
        for (const auto& kv : slot_map) {
            if (!kv.second.empty() &&
                std::find(out.begin(), out.end(), kv.second) == out.end()) {
                out.push_back(kv.second);
            }
        }
    };
    collect(m_material_albedo);
    collect(m_material_normal);
    collect(m_material_mrough);
    collect(m_material_occlusion);
    collect(m_material_emissive_map);
    return out;
}

bool Runtime::hot_reload_mesh(const assets::AssetId& id, std::string& out_error) {
    // Clear the failure verdict first: the whole point of a reload is that the
    // bytes changed, so an asset that previously could not be loaded deserves
    // another attempt instead of staying silently skipped.
    m_failed_meshes.erase(id);
    auto mit = m_mesh_handles.find(id);
    if (mit == m_mesh_handles.end()) {
        // Not resident (still loading or never synced): refresh the manager
        // cache so the next sync picks the new bytes up.
        m_manager.unload(id);
        m_manager.load_mesh(id);
        return true;
    }
    m_manager.unload(id);
    auto handle = m_manager.load_mesh_sync(id);
    if (!handle || handle->state != assets::AssetState::Ready || !handle->asset) {
        out_error = "Reloaded mesh unavailable: " + (handle ? handle->error : std::string("no handle"));
        return false;
    }
    auto fresh = rendering::make_static_mesh(*handle->asset, handle->asset->logical_path);
    if (!fresh || !fresh->upload(m_device)) {
        out_error = "Reloaded mesh upload failed";
        return false;
    }
    // The old GPU buffers may still be referenced by the in-flight frame.
    m_device.wait_idle();
    if (!m_mesh_library || !m_mesh_library->replace(mit->second, std::move(fresh))) {
        out_error = "Mesh slot replacement failed";
        return false;
    }
    NF_LOG_INFO(LogCategory::Core, "Runtime: mesh hot-reloaded '{}'", id.to_string());
    return true;
}

bool Runtime::reload_material_file(const std::string& logical_path, std::string& out_error) {
    const std::string path = normalize_material_path(logical_path);
    if (material_dirty(path)) {
        out_error = "Skipped (unsaved editor edits win over disk)";
        NF_LOG_WARN(LogCategory::Core, "Runtime: hot reload skipped dirty material '{}'", path);
        return false;
    }
    rendering::MaterialAsset file_asset;
    {
        auto read = m_vfs.read_text(path);
        std::string perr;
        if (!read.ok || !rendering::MaterialAsset::load_from_text(read.value, file_asset, perr)) {
            out_error = "Unparsable material file '" + path + "'";
            return false;
        }
    }
    auto it = m_material_instances.find(path);
    if (it == m_material_instances.end()) {
        // Not resident yet: nothing live to refresh (next use loads the file).
        return true;
    }
    if (m_renderer == nullptr) {
        out_error = "Renderer unavailable";
        return false;
    }
    m_renderer->materials().set_params(it->second, file_asset.params);
    const std::string bound = material_albedo(path);
    if (bound != file_asset.albedo) {
        if (file_asset.albedo.empty()) {
            m_renderer->materials().clear_albedo_texture(it->second);
        } else if (!set_material_albedo(path, file_asset.albedo, out_error)) {
            return false;
        }
        // set_material_albedo marks dirty; a file reload is not a user edit.
        m_material_dirty[path] = false;
        // set via file: ensure the recorded binding matches the file exactly.
        m_material_albedo[path] = file_asset.albedo;
    }
    if (material_mip_mode(path) != file_asset.mip_mode) {
        if (!set_material_mip_mode(path, file_asset.mip_mode, out_error)) {
            return false;
        }
        // set_material_mip_mode marks dirty; a file reload is not a user edit.
        m_material_dirty[path] = false;
    }
    NF_LOG_INFO(LogCategory::Core, "Runtime: material hot-reloaded '{}'", path);
    return true;
}

bool Runtime::material_dirty(const std::string& logical_path) const {
    auto it = m_material_dirty.find(normalize_material_path(logical_path));
    return it != m_material_dirty.end() && it->second;
}

bool Runtime::any_material_dirty() const {
    for (const auto& kv : m_material_dirty) {
        if (kv.second) {
            return true;
        }
    }
    return false;
}

} // namespace nf::runtime
