#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/MaterialAsset.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/RenderWorld.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <span>

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

} // namespace

Runtime::Runtime(assets::VirtualFileSystem& vfs, assets::AssetRegistry& registry, assets::AssetManager& manager,
                 rhi::IGraphicsDevice& device, rhi::Swapchain* swapchain)
    : m_vfs(vfs), m_registry(registry), m_manager(manager), m_device(device), m_swapchain(swapchain) {
    m_mesh_library = std::make_unique<rendering::MeshLibrary>();
    m_material_library = std::make_unique<rendering::MaterialLibrary>(m_device);
    m_pipeline_cache = std::make_unique<rendering::PipelineCache>(m_device);
    m_renderer = std::make_unique<rendering::Renderer3D>();
}

Runtime::~Runtime() {
    // Best effort: free GPU resources while the device is still alive.
    // If the device was already shut down this is a no-op (guards inside).
    // The explicit shutdown() path is preferred (called before device shutdown).
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
    m_textures.clear();
    m_default_material = rendering::kInvalidMaterialHandle;
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
    // A freshly loaded scene gets a fresh attempt at every mesh. Failure was
    // recorded per-asset, and the asset may have been cooked since it last
    // failed, so the previous scene's verdicts must not carry over.
    m_failed_meshes.clear();
    m_failed_mesh_reports = 0;
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
            } else if (world.has<MeshComponent>(e)) {
                label = "Mesh";
            } else {
                label = "Entity " + std::to_string(e.id);
            }
            world.add<scene::NameComponent>(e, scene::NameComponent{label});
        }
    }
    // Build the physics world from the scene that was just loaded, so a scene
    // containing physics is live as soon as it opens.
    rebuild_physics_from_scene();
    // Gameplay: instantiate the registered modules on the first load, let the
    // scene's components restore their state, then announce the new scene.
    // apply_gameplay_state() must follow init_gameplay() — there is nothing to
    // restore into until the module instances exist.
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
    // Physics runs on its own fixed clock, so the scene's motion is the same
    // whether the frame took 5 ms or 50. The render path then only reads the
    // transforms physics produced.
    step_physics(dt);
    // Animation advances on the frame delta — it is a function of time, not of
    // state, so it needs no fixed clock. Runs after physics, so an entity that
    // carries both is driven by its animation.
    step_animation(dt);
    // Audio mixes after the transforms are final for this frame, so a source on
    // a moving entity is spatialised where it will actually be rendered rather
    // than one frame behind.
    step_audio(dt);
    // Gameplay runs last, so a module observes where this frame actually put
    // everything, and before propagation, so whatever it writes is what gets
    // rendered rather than a frame late.
    step_gameplay(dt);
    auto& world = m_scene_data_ptr->scene->world();
    scene::propagate_transforms(world);
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
        if (transform != nullptr && anim->has_base_transform && !anim->last_local_pose.empty()) {
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

        audio::AudioSource source;
        source.buffer = buffer;
        source.volume = comp->volume;
        source.pitch = comp->pitch;
        source.looping = comp->looping;
        source.playing = true;
        source.sample_cursor = comp->sample_cursor;
        source.spatial = comp->spatial;
        source.position = position;
        source.spatial_settings = comp->spatial_settings;

        m_audio_bus.mix_source(source, m_audio_listener.position, m_audio_listener.forward,
                               m_audio_listener.up, m_audio_left.data(), m_audio_right.data(),
                               frames, audio::kDefaultSampleRate);

        // The cursor is the one piece of playback state that has to survive the
        // frame; everything else is re-derived from the component next time.
        comp->sample_cursor = source.sample_cursor;
        comp->playing = source.playing;
        ++m_audio_sources_mixed;
    }

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
    auto& registry = gameplay::GameplayModuleRegistry::instance();
    for (const std::string& name : registry.names()) {
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
    ctx.input   = m_input_source;
    ctx.audio   = &m_audio_bus;
    ctx.physics = m_physics.get();
    if (m_scene_data_ptr && m_scene_data_ptr->scene) {
        ctx.scene         = m_scene_data_ptr->scene.get();
        ctx.world         = &m_scene_data_ptr->scene->world();
        ctx.scene_version = static_cast<u32>(m_scene_version);
    }
    return ctx;
}

ecs::Entity Runtime::find_gameplay_owner(std::string_view name) const {
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) return ecs::Entity{};
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
    gameplay::GameplayContext ctx = build_gameplay_context(dt);

    u32 stepped = 0;
    for (GameplaySlot& slot : m_gameplay_modules) {
        const auto it = enabled.find(slot.module->name());
        if (it != enabled.end() && !it->second) continue;
        slot.module->on_update(ctx);
        ++stepped;
    }

    ++m_gameplay_frame;
    m_gameplay_updates += stepped;
    return stepped;
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
    (void)ecs::component_type_id<gameplay::GameplayModuleComponent>();
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
        JobSystem::instance().enqueue(std::move(run_parse));
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
    mark_scene_edited();
}

void Runtime::rebuild_physics_from_scene() {
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
    if (created > 0) {
        NF_LOG_INFO(LogCategory::Core, "Runtime: physics world created with {} bodies", created);
    }
}

u32 Runtime::step_physics(float frame_delta) {
    if (!m_physics || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return 0;
    }

    const u32 steps = m_physics_clock.advance(frame_delta);
    for (u32 i = 0; i < steps; ++i) {
        m_physics->step(m_physics_clock.step());
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
        out.fov_y_rad = c->fov_y * 3.14159265359f / 180.0f;
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
        rendering::update_camera(out);
        return true;
    }
    // No active camera: keep the default orbit camera.
    rendering::update_camera(out);
    return false;
}

void Runtime::extract_light() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    for (auto e : world.query<DirectionalLight>()) {
        const auto* l = world.get<DirectionalLight>(e);
        if (l == nullptr) {
            continue;
        }
        rendering::DirectionalLight rl{};
        rl.direction = {l->dir_x, l->dir_y, l->dir_z};
        rl.color = {l->color_r, l->color_g, l->color_b};
        rl.intensity = l->intensity;
        rl.enabled = true;
        rl.shadows_enabled = l->cast_shadows;
        rl.shadow_strength = l->shadow_strength;
        rl.shadow_bias = l->shadow_bias;
        m_renderer->set_directional_light(rl);
        return;
    }
    // No light in the scene: keep the renderer's default directional light.
}

void Runtime::extract_sky() {
    if (!m_renderer || !m_renderer_initialized) {
        return;
    }
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
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
        if (tr == nullptr || mc == nullptr || !mc->mesh_id.valid()) {
            continue;
        }
        auto it = m_mesh_handles.find(mc->mesh_id);
        if (it == m_mesh_handles.end()) {
            // Asset still loading or failed: skip safely (no crash).
            continue;
        }
        const rendering::StaticMesh* mesh = m_mesh_library->get(it->second);
        if (mesh == nullptr || !mesh->is_uploaded()) {
            continue;
        }
        rendering::RenderObject ro{};
        ro.id = e.id;
        ro.visible = true;
        ro.transform.x = tr->world_x;
        ro.transform.y = tr->world_y;
        ro.transform.z = tr->world_z;
        // Full TRS matrix: translation from the propagated world position,
        // rotation/scale from the local transform.
        ro.world = scene::compose_trs_mat4(tr->world_x, tr->world_y, tr->world_z, tr->rot_x,
                                           tr->rot_y, tr->rot_z, tr->scale_x, tr->scale_y,
                                           tr->scale_z);
        ro.mesh_handle = it->second;
        // Shared material assignment: same path shares one renderer instance.
        ro.material_handle = material_for_path(mc->material);
        if (!ro.material_handle.valid()) {
            ro.material_handle = m_default_material;
        }
        ro.lod = 0;
        // World-space bounds for frustum culling (translation only).
        ro.bounds = rendering::transform_aabb(mesh->bounds(), tr->world_x, tr->world_y, tr->world_z);
        ro.sphere = rendering::transform_sphere(mesh->bounding_sphere(), tr->world_x, tr->world_y, tr->world_z);
        out.objects.push_back(std::move(ro));
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

void Runtime::render(uint32_t image_index, rhi::CommandBuffer& cmd) {
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
    extract_sky();
    rendering::RenderWorld render_world{};
    build_render_world(render_world);
    // Frustum culling happens inside Renderer3D::render (RenderWorld → Visible).
    rhi::Texture* target = m_swapchain->get_texture(image_index);
    if (target == nullptr) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: null swapchain texture {}", image_index);
        return;
    }
    // Scene → AssetManager → Renderer3D: Depth → GBuffer → Lighting → Tonemap.
    if (!m_renderer->render(cmd, render_world, cam, *target, true)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime::render: Renderer3D failed, using fallback clear");
        fallback_clear(*target, cmd, true);
        return;
    }
    const auto& stats = m_renderer->last_stats();
    NF_LOG_TRACE(LogCategory::Core, "Runtime::render: extracted={} visible={} draws={}", stats.extracted,
                 stats.visible, stats.draw_calls);
}

void Runtime::render_offscreen(rhi::Texture& target, rhi::CommandBuffer& cmd) {
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
    extract_sky();
    rendering::RenderWorld render_world{};
    build_render_world(render_world);
    if (!m_renderer->render(cmd, render_world, cam, target, false)) {
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
    // Albedo binding follows the file; failures stay scalar (never fatal).
    // Record first: rebind_material_sampling reads the recorded binding.
    m_material_albedo[path] = (have_file ? file_asset.albedo : "");
    if (have_file && !file_asset.albedo.empty()) {
        std::string tex_err;
        if (!rebind_material_sampling(path, h, tex_err)) {
            NF_LOG_WARN(LogCategory::Core, "Runtime: material '{}' albedo unreachable ({}), scalar",
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
    const std::string tex_path = material_albedo(material_path);
    if (tex_path.empty()) {
        m_renderer->materials().clear_albedo_texture(handle);
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
    m_renderer->materials().set_albedo_texture(handle, *tit->second->view, *sampler);
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
    // with its own recorded mip mode.
    if (m_renderer != nullptr) {
        for (const auto& kv : m_material_albedo) {
            if (kv.second != logical_path) {
                continue;
            }
            auto mit = m_material_instances.find(kv.first);
            if (mit == m_material_instances.end()) {
                continue;
            }
            std::string rerr;
            if (!rebind_material_sampling(kv.first, mit->second, rerr)) {
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

std::vector<std::string> Runtime::known_texture_paths() const {
    std::vector<std::string> out;
    for (const auto& kv : m_material_albedo) {
        if (!kv.second.empty() &&
            std::find(out.begin(), out.end(), kv.second) == out.end()) {
            out.push_back(kv.second);
        }
    }
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
