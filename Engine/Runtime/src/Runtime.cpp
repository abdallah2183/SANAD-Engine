#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/MaterialAsset.hpp>
#include <NF/Scene/NameComponent.hpp>
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
    m_device.wait_idle();
    if (m_renderer && m_renderer_initialized) {
        m_renderer->shutdown();
    }
    m_renderer_initialized = false;
    m_fallback_fbs.clear();
    m_fallback_rps.clear();
    m_mesh_handles.clear();
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
    std::filesystem::path shader_dir;
#ifdef NF_BASIC3D_SHADER_DIR
    {
        std::filesystem::path p = NF_BASIC3D_SHADER_DIR;
        if (std::filesystem::exists(p)) {
            shader_dir = p;
        }
    }
#endif
    if (shader_dir.empty()) {
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
                shader_dir = cand;
                break;
            }
        }
    }
    if (shader_dir.empty()) {
        auto r = m_vfs.resolve("content://Shaders/Basic3D");
        if (r.ok && std::filesystem::exists(r.value)) {
            shader_dir = r.value;
        }
    }
    if (shader_dir.empty() || !std::filesystem::exists(shader_dir)) {
        shader_dir = std::filesystem::path("build/DebugNinja/Shaders/Basic3D");
        if (!std::filesystem::exists(shader_dir)) {
            shader_dir = std::filesystem::path("build/debug/Shaders/Basic3D");
        }
    }
    if (!m_renderer->init(m_device, shader_dir, width, height)) {
        NF_LOG_ERROR(LogCategory::Core, "Runtime: failed to init Renderer3D (shader_dir='{}')", shader_dir.string());
        return false;
    }
    m_renderer->set_mesh_library(m_mesh_library.get());
    // Sensible defaults; per-frame extraction overrides the directional light.
    m_renderer->set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{-0.5f, -1.0f, -0.3f},
        rendering::Vec3{1.0f, 1.0f, 1.0f},
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
            // Missing/corrupt asset: warn once per frame is too noisy, so only
            // warn here; render proceeds without this object (safe fallback).
            NF_LOG_WARN(LogCategory::Core, "Runtime: mesh asset {} failed: {}", comp->mesh_id.to_string(),
                        handle->error);
            continue;
        }
        if (handle->state == assets::AssetState::Ready && handle->asset) {
            auto static_mesh = handle->asset->to_static_mesh(handle->asset->logical_path);
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
    m_scene_data_ptr = std::make_unique<SceneRuntimeData>();
    m_scene_data_ptr->scene = std::move(result.scene);
    m_loaded_scene_path = logical_path;
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
    return true;
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
    (void)dt;
    // AssetManager.update(): async CPU → GPU upload on this (render) thread.
    m_manager.update();
    sync_meshes_from_assets();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    auto& world = m_scene_data_ptr->scene->world();
    scene::propagate_transforms(world);
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
        m_renderer->set_directional_light(rl);
        return;
    }
    // No light in the scene: keep the renderer's default directional light.
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
        // rotation/scale from the local transform. Matches compose_trs().
        float m[16]{};
        scene::compose_trs(tr->world_x, tr->world_y, tr->world_z, tr->rot_x, tr->rot_y, tr->rot_z,
                           tr->scale_x, tr->scale_y, tr->scale_z, m);
        for (int i = 0; i < 16; ++i) {
            ro.world.m[i] = m[i];
        }
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
    // Albedo binding follows the file; failures stay scalar (never fatal).
    if (have_file && !file_asset.albedo.empty()) {
        std::string tex_err;
        if (ensure_texture(file_asset.albedo, tex_err)) {
            auto tit = m_textures.find(file_asset.albedo);
            if (tit != m_textures.end() && tit->second->view && tit->second->sampler) {
                m_renderer->materials().set_albedo_texture(h, *tit->second->view,
                                                           *tit->second->sampler);
            }
        } else {
            NF_LOG_WARN(LogCategory::Core, "Runtime: material '{}' albedo unreachable ({}), scalar",
                        path, tex_err);
        }
        m_material_albedo[path] = file_asset.albedo;
    } else {
        m_material_albedo[path] = "";
    }
    return h;
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

std::unique_ptr<Runtime::TextureObjects> Runtime::upload_texture_objects(
    const std::string& logical_path, const rendering::DecodedImage& img, std::string& out_error) {
    const size_t px = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    if (px == 0 || px > 64u * 1024u * 1024u || !img.ok()) {
        out_error = "Texture '" + logical_path + "' has unreasonable dimensions";
        return nullptr;
    }
    rhi::TextureDesc td{};
    td.width = static_cast<u32>(img.width);
    td.height = static_cast<u32>(img.height);
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
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
    if (auto trans_cmd = m_device.create_command_buffer()) {
        if (auto trans_fence = m_device.create_fence(false)) {
            trans_cmd->begin();
            trans_cmd->transition_texture_for_sampling(*tex);
            trans_cmd->end();
            m_device.submit(*trans_cmd, rhi::SubmitInfo{.signal_fence = trans_fence.get()});
            trans_fence->wait();
        }
    }
    rhi::TextureViewDesc vd{};
    vd.texture = tex.get();
    auto view = m_device.create_texture_view(vd);
    rhi::SamplerDesc sd{};
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    auto sampler = m_device.create_sampler(sd);
    if (!view || !sampler) {
        out_error = "View/sampler creation failed for texture '" + logical_path + "'";
        return nullptr;
    }
    auto entry = std::make_unique<TextureObjects>();
    entry->texture = std::move(tex);
    entry->view = std::move(view);
    entry->sampler = std::move(sampler);
    return entry;
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
    // Rebind every material sampling this path (raw pointers changed).
    if (m_renderer != nullptr) {
        for (const auto& kv : m_material_albedo) {
            if (kv.second != logical_path) {
                continue;
            }
            auto mit = m_material_instances.find(kv.first);
            if (mit == m_material_instances.end()) {
                continue;
            }
            m_renderer->materials().set_albedo_texture(mit->second, *m_textures[logical_path]->view,
                                                       *m_textures[logical_path]->sampler);
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
    if (!ensure_texture(texture_path, out_error)) {
        return false;
    }
    auto tit = m_textures.find(texture_path);
    if (tit == m_textures.end() || !tit->second->view || !tit->second->sampler) {
        out_error = "Texture objects missing for '" + texture_path + "'";
        return false;
    }
    m_renderer->materials().set_albedo_texture(h, *tit->second->view, *tit->second->sampler);
    m_material_albedo[path] = texture_path;
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
    auto fresh = handle->asset->to_static_mesh(handle->asset->logical_path);
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
