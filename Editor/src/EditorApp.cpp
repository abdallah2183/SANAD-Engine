#include <NF/Editor/EditorApp.hpp>
#include <NF/Audio/AudioDecoder.hpp>
#include <NF/Editor/ExternalIde.hpp>
#include <NF/Editor/Outliner.hpp>
#include <NF/Editor/Prefabs.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>

// CreateProcessW for launching the game player next to the editor. The defines
// come first: engine headers above do not pull <windows.h>, and NOMINMAX keeps
// the engine's own min/max use unambiguous. Both are already on the compiler
// command line, so the guards only cover a build that stops passing them.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>

namespace nf::editor {

namespace {

// Side of the ground plane the Create menu and File > New lay down, in metres.
// 60 m is a city block scaled down: big enough that a cube dropped anywhere in
// the default view lands on it, small enough to stay inside the shadow
// cascade range at the default camera distance.
constexpr float kGroundSize = 60.0f;

void normalize_direction(float v[3]) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-20f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

// assets::AssetAABB and rendering::AABB are field-for-field the same six
// floats, and writing the conversion at the two use sites instead of here is how
// they drift. Kept next to the other bounds code so the two types can never be
// silently interchanged by a cast that happens to compile.
static rendering::AABB assets_to_render_aabb(const assets::AssetAABB& b) {
    rendering::AABB r;
    r.min_x = b.min_x;
    r.min_y = b.min_y;
    r.min_z = b.min_z;
    r.max_x = b.max_x;
    r.max_y = b.max_y;
    r.max_z = b.max_z;
    return r;
}

static AABB to_editor_aabb(const rendering::AABB& b) {
    AABB e;
    e.min_x = b.min_x;
    e.min_y = b.min_y;
    e.min_z = b.min_z;
    e.max_x = b.max_x;
    e.max_y = b.max_y;
    e.max_z = b.max_z;
    return e;
}

void recompute_asset_bounds(assets::MeshAsset& mesh) {    assets::AssetAABB box;
    assets::AssetSphere sphere;
    if (mesh.vertices.empty()) {
        mesh.bounds = box;
        mesh.sphere = sphere;
        return;
    }
    float big = std::numeric_limits<float>::max();
    box.min_x = box.min_y = box.min_z = big;
    box.max_x = box.max_y = box.max_z = -big;
    for (const assets::AssetVertex& v : mesh.vertices) {
        box.min_x = std::min(box.min_x, v.position[0]);
        box.min_y = std::min(box.min_y, v.position[1]);
        box.min_z = std::min(box.min_z, v.position[2]);
        box.max_x = std::max(box.max_x, v.position[0]);
        box.max_y = std::max(box.max_y, v.position[1]);
        box.max_z = std::max(box.max_z, v.position[2]);
    }
    sphere.cx = (box.min_x + box.max_x) * 0.5f;
    sphere.cy = (box.min_y + box.max_y) * 0.5f;
    sphere.cz = (box.min_z + box.max_z) * 0.5f;
    float r2 = 0.0f;
    for (const assets::AssetVertex& v : mesh.vertices) {
        const float dx = v.position[0] - sphere.cx;
        const float dy = v.position[1] - sphere.cy;
        const float dz = v.position[2] - sphere.cz;
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    sphere.radius = std::sqrt(r2);
    mesh.bounds = box;
    mesh.sphere = sphere;
}

// World-bakes a mesh: position' = R * S * p + T through the engine's own TRS
// helper, so an exported model lands in another tool exactly where the viewport
// drew it (rotated 90 degrees about X is a floor in both places). This glue
// lives in the editor, not in NFAssets, because it needs the scene euler
// convention and NFAssets deliberately does not depend on NFScene — the same
// split as rendering::make_mesh_asset across the mesh boundary.
assets::MeshAsset bake_mesh_world(const assets::MeshAsset& mesh, const scene::Transform& t) {
    assets::MeshAsset out = mesh;
    if (mesh.vertices.empty()) {
        return out;
    }
    const Mat4 m = scene::compose_trs_mat4(t.world_x, t.world_y, t.world_z, t.rot_x, t.rot_y,
                                          t.rot_z, t.scale_x, t.scale_y, t.scale_z);
    const float sx = (t.scale_x != 0.0f) ? t.scale_x : 1.0f;
    const float sy = (t.scale_y != 0.0f) ? t.scale_y : 1.0f;
    const float sz = (t.scale_z != 0.0f) ? t.scale_z : 1.0f;
    for (assets::AssetVertex& v : out.vertices) {
        const Vec3 p = m.transform_point(Vec3{v.position[0], v.position[1], v.position[2]});
        v.position[0] = p.x;
        v.position[1] = p.y;
        v.position[2] = p.z;
        // Normals take the inverse-transpose of R * S, which is R * S^-1:
        // divide by the scale, transform by the rotation only, then re-normalize
        // (a scaled normal is no longer a direction).
        float n[3] = {v.normal[0] / sx, v.normal[1] / sy, v.normal[2] / sz};
        const Vec3 nd = m.transform_direction(Vec3{n[0], n[1], n[2]});
        v.normal[0] = nd.x;
        v.normal[1] = nd.y;
        v.normal[2] = nd.z;
        normalize_direction(v.normal);
        const Vec3 td = m.transform_direction(Vec3{v.tangent[0], v.tangent[1], v.tangent[2]});
        v.tangent[0] = td.x;
        v.tangent[1] = td.y;
        v.tangent[2] = td.z;
        normalize_direction(v.tangent);
    }
    recompute_asset_bounds(out);
    return out;
}

// Concatenates world-baked meshes into one asset, shifting every submesh's
// vertex/index offsets by what came before it. "Export scene" is one file with
// everything the scene draws, which is what a user uploading "the model"
// expects to hand somebody.
assets::MeshAsset merge_mesh_assets(const std::vector<assets::MeshAsset>& meshes) {
    assets::MeshAsset out;
    bool first = true;
    for (const assets::MeshAsset& mesh : meshes) {
        if (first) {
            out.id = mesh.id;
            out.logical_path = mesh.logical_path;
            out.format_version = mesh.format_version;
            first = false;
        }
        const uint32_t vertex_base = static_cast<uint32_t>(out.vertices.size());
        const uint32_t index_base = static_cast<uint32_t>(out.indices.size());
        out.vertices.insert(out.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        out.indices.insert(out.indices.end(), mesh.indices.begin(), mesh.indices.end());
        if (mesh.submeshes.empty()) {
            assets::AssetSubMesh whole;
            whole.index_count = static_cast<uint32_t>(mesh.indices.size());
            whole.vertex_count = static_cast<uint32_t>(mesh.vertices.size());
            whole.vertex_offset = vertex_base;
            whole.index_offset = index_base;
            out.submeshes.push_back(whole);
            continue;
        }
        for (const assets::AssetSubMesh& sub : mesh.submeshes) {
            assets::AssetSubMesh shifted = sub;
            shifted.vertex_offset += vertex_base;
            shifted.index_offset += index_base;
            out.submeshes.push_back(shifted);
        }
    }
    recompute_asset_bounds(out);
    return out;
}

} // namespace

const char* primitive_name(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Ground: return "Ground";
        case PrimitiveKind::Cube: return "Cube";
        case PrimitiveKind::Sphere: return "Sphere";
        case PrimitiveKind::Quad: return "Quad";
    }
    return "Primitive";
}

const char* primitive_asset_stem(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Ground: return "ground";
        case PrimitiveKind::Cube: return "cube";
        case PrimitiveKind::Sphere: return "sphere";
        case PrimitiveKind::Quad: return "quad";
    }
    return "primitive";
}

std::string primitive_asset_path(PrimitiveKind kind) {
    return std::string("content://Meshes/") + primitive_asset_stem(kind) + ".nfmesh";
}

EditorApp::EditorApp(assets::VirtualFileSystem& vfs, assets::AssetRegistry& registry,
                     assets::AssetManager& manager, ConsoleBuffer& console)
    : m_vfs(vfs), m_registry(registry), m_manager(manager), m_console(console) {}

ecs::World* EditorApp::world() {
    if (m_runtime == nullptr) {
        return nullptr;
    }
    scene::Scene* s = m_runtime->edit_scene();
    return (s != nullptr) ? &s->world() : nullptr;
}

const ecs::World* EditorApp::world() const {
    if (m_runtime == nullptr) {
        return nullptr;
    }
    const scene::Scene* s = m_runtime->scene();
    return (s != nullptr) ? &s->world() : nullptr;
}

bool EditorApp::require_editable(std::string& out_err) const {
    if (m_runtime == nullptr || m_runtime->edit_scene() == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (m_play.playing()) {
        out_err = "Structural edits are disabled while playing (press Stop first)";
        return false;
    }
    return true;
}

void EditorApp::after_mutation(ecs::Entity touched) {
    m_outliner_cache_valid = false; // rows may show new/renamed/reparented entities
    if (m_runtime != nullptr) {
        m_runtime->mark_scene_edited();
    }
    m_dirty = true;
    if (ecs::World* w = world()) {
        m_selection.prune(*w);
        // Recompose the world pose immediately. Everything the editor DRAWS
        // reads world_* (the gizmo origin, the grid, the bounds the viewport
        // frames), and the editor mutates local_* from its own command path.
        // Without this, a gizmo drag wrote local_x and the gizmo stayed put
        // until the next runtime tick — a visible one-frame lag between the
        // arrows and the object they are supposed to be dragging.
        //
        // propagate_transforms is a full BFS over the hierarchy, so this is not
        // free; it runs once per committed edit (and once per live drag frame),
        // never once per widget.
        scene::propagate_transforms(*w);
    }
    if (touched.valid()) {
        if (const ecs::World* w = world()) {
            if (w->is_alive(touched)) {
                m_selection.set_single(touched);
            }
        }
    }
}

bool EditorApp::build_default_scene(scene::Scene& scene, std::string& out_err) {
    // A new scene is a room you can work in: ground, something standing on it,
    // a sun, a natural sky and a camera framing the origin. File > New used to
    // produce an empty world, which is why the first question about this editor
    // was always "why is there no floor?".
    auto& w = scene.world();

    assets::AssetId ground_id;
    if (!ensure_primitive_asset(PrimitiveKind::Ground, ground_id, out_err)) {
        return false;
    }
    assets::AssetId cube_id;
    if (!ensure_primitive_asset(PrimitiveKind::Cube, cube_id, out_err)) {
        return false;
    }

    // Light first, so the scene reads in the same order the outliner shows and
    // the renderer finds it on the first pass.
    {
        ecs::Entity e = w.create_entity();
        scene::Transform t{};
        t.dirty = true;
        w.add<scene::Transform>(e, t);
        w.add<scene::NameComponent>(e, scene::NameComponent{"Sun"});
        runtime::DirectionalLight light;
        light.dir_x = -0.45f;
        light.dir_y = -1.0f;
        light.dir_z = -0.35f;
        light.color_r = 1.0f;
        light.color_g = 0.97f;
        light.color_b = 0.92f;
        light.intensity = 1.8f;
        light.cast_shadows = true;
        // Cascades sized for a 60 m ground: the whole floor receives shadows in
        // one cascade band instead of falling out of range halfway across.
        light.shadow_distance = 80.0f;
        w.add<runtime::DirectionalLight>(e, light);
    }
    {
        ecs::Entity e = w.create_entity();
        scene::Transform t{};
        t.dirty = true;
        w.add<scene::Transform>(e, t);
        w.add<scene::NameComponent>(e, scene::NameComponent{"Sky"});
        w.add<runtime::SkyComponent>(e, runtime::SkyComponent{});
    }
    {
        ecs::Entity e = w.create_entity();
        scene::Transform t{};
        t.local_y = -0.05f;
        t.rot_x = -90.0f;
        t.scale_x = kGroundSize;
        t.scale_y = kGroundSize;
        t.scale_z = 1.0f;
        t.dirty = true;
        w.add<scene::Transform>(e, t);
        w.add<scene::NameComponent>(e, scene::NameComponent{"Ground"});
        runtime::MeshComponent mc;
        mc.mesh_id = ground_id;
        mc.material = runtime::kDefaultMaterialPath;
        w.add<runtime::MeshComponent>(e, mc);
        physics::RigidBodyComponent rb;
        rb.type = physics::BodyType::Static;
        w.add<physics::RigidBodyComponent>(e, rb);
        physics::ColliderComponent col;
        col.shape = physics::Shape::make_box(Vec3(kGroundSize * 0.5f, 0.05f, kGroundSize * 0.5f));
        w.add<physics::ColliderComponent>(e, col);
    }
    {
        ecs::Entity e = w.create_entity();
        scene::Transform t{};
        t.local_y = 0.5f; // resting on the ground
        t.dirty = true;
        w.add<scene::Transform>(e, t);
        w.add<scene::NameComponent>(e, scene::NameComponent{"Cube"});
        runtime::MeshComponent mc;
        mc.mesh_id = cube_id;
        mc.material = runtime::kDefaultMaterialPath;
        w.add<runtime::MeshComponent>(e, mc);
        physics::RigidBodyComponent rb;
        rb.type = physics::BodyType::Dynamic;
        w.add<physics::RigidBodyComponent>(e, rb);
        physics::ColliderComponent col;
        col.shape = physics::Shape::make_box(Vec3(0.5f, 0.5f, 0.5f));
        w.add<physics::ColliderComponent>(e, col);
    }
    {
        ecs::Entity e = w.create_entity();
        scene::Transform t{};
        t.local_y = 3.0f;
        t.local_z = 8.0f;
        t.dirty = true;
        w.add<scene::Transform>(e, t);
        w.add<scene::NameComponent>(e, scene::NameComponent{"Camera"});
        runtime::CameraComponent cam;
        cam.fov_y = 60.0f;
        cam.aspect = 16.0f / 9.0f;
        cam.near_plane = 0.1f;
        cam.far_plane = 1000.0f;
        cam.is_active = true;
        w.add<runtime::CameraComponent>(e, cam);
    }
    return true;
}

bool EditorApp::new_scene(std::string& out_err) {
    if (m_runtime == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    if (m_play.playing()) {
        out_err = "Stop play mode before creating a scene";
        return false;
    }
    // A drag armed on the old world must not survive the switch: entity ids
    // recycle, so the stale handle could alias a different entity.
    {
        std::string dummy;
        viewport_abort_drag(dummy);
    }
    // Build the default scene through the loader path: fill a scene, save it to
    // a temp logical path, then load it so Runtime owns it like any other. The
    // content is the authoring baseline — ground, cube, sun, natural sky and a
    // camera — see build_default_scene.
    scene::Scene fresh("Untitled");
    if (!build_default_scene(fresh, out_err)) {
        return false;
    }
    std::string tmp_err;
    if (!runtime::save_scene_to_vfs(m_vfs, "cache://EditorUntitled.nfscene", fresh, tmp_err)) {
        out_err = "Failed to stage the default scene: " + tmp_err;
        return false;
    }
    if (!m_runtime->load_scene("cache://EditorUntitled.nfscene", out_err)) {
        return false;
    }
    m_scene_path.clear();
    if (scene::Scene* s = m_runtime->edit_scene()) {
        s->set_name("Untitled");
    }
    m_stack.clear();
    m_selection.clear();
    m_outliner = OutlinerState{};
    m_dirty = false;
    return true;
}

bool EditorApp::open_scene(const std::string& logical_path, std::string& out_err) {
    if (m_runtime == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    if (m_play.playing()) {
        out_err = "Stop play mode before opening a scene";
        return false;
    }
    {
        std::string dummy;
        viewport_abort_drag(dummy);
    }
    if (!m_runtime->load_scene(logical_path, out_err)) {
        return false;
    }
    m_scene_path = logical_path;
    m_stack.clear();
    m_selection.clear();
    m_outliner = OutlinerState{};
    m_dirty = false;
    rebuild_hot_watch();
    return true;
}

bool EditorApp::save(std::string& out_err) {
    if (m_scene_path.empty()) {
        out_err = "Scene has no path yet (use Save As)";
        return false;
    }
    return save_as(m_scene_path, out_err);
}

bool EditorApp::save_as(const std::string& logical_path, std::string& out_err) {
    if (m_runtime == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    if (!m_runtime->save_scene(logical_path, out_err)) {
        return false;
    }
    m_scene_path = logical_path;
    m_dirty = false;
    return true;
}

bool EditorApp::save_copy(const std::string& logical_path, std::string& out_err) {
    if (m_runtime == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    const scene::Scene* s = m_runtime->scene();
    if (s == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!runtime::save_scene_to_vfs(m_vfs, logical_path, *s, out_err)) {
        return false;
    }
    return true;
}

bool EditorApp::create_entity(const std::string& name, ecs::Entity parent, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (parent.valid() && !w->is_alive(parent)) {
        out_err = "Parent entity is not alive";
        return false;
    }
    auto cmd = std::make_unique<CreateEntityCommand>(name, parent);
    m_stack.push(std::move(cmd), *w);
    after_mutation(m_stack.last_target());
    return true;
}

bool EditorApp::delete_entity(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = DeleteEntityCommand::capture(*w, e, out_err);
    if (!cmd) {
        return false;
    }
    m_selection.remove(e);
    m_stack.push(std::move(cmd), *w);
    after_mutation(ecs::kInvalidEntity);
    return true;
}

bool EditorApp::rename_entity(ecs::Entity e, const std::string& new_name, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_rename_command(*w, e, new_name, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_transform(ecs::Entity e, const TransformEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_transform_command(*w, e, edit, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    // An animated entity's transform is owned by its pose, which is applied as
    // an offset from a placement captured on the first step. Without re-capturing
    // it here, the move the user just made would be silently undone next frame.
    // No-op for entities that are not animated.
    if (m_runtime != nullptr) {
        m_runtime->rebase_animation(e);
    }
    return true;
}

bool EditorApp::reparent(ecs::Entity e, ecs::Entity new_parent, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!e.valid() || !w->is_alive(e)) {
        out_err = "Entity is not alive";
        return false;
    }
    if (new_parent.valid()) {
        if (!w->is_alive(new_parent)) {
            out_err = "New parent is not alive";
            return false;
        }
        if (new_parent == e || is_descendant_of(*w, new_parent, e)) {
            out_err = "Reparent would create a cycle";
            return false;
        }
    }
    ecs::Entity old_parent = ecs::kInvalidEntity;
    if (const auto* t = w->get<scene::Transform>(e)) {
        old_parent = t->parent;
    }
    auto cmd = std::make_unique<ReparentCommand>(e, old_parent, new_parent);
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_camera(ecs::Entity e, const CameraEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_camera_command(*w, e, edit, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_light(ecs::Entity e, const LightEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_light_command(*w, e, edit, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_sky(ecs::Entity e, const SkyEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_sky_command(*w, e, edit, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_time_of_day(ecs::Entity e, const TimeOfDayEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_time_of_day_command(*w, e, edit, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_post_process(ecs::Entity e, const runtime::PostProcessComponent& edit,
                                 std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_post_process_command(*w, e, edit, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_mesh(ecs::Entity e, const std::string& asset_id_text, const std::string& material,
                         std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_mesh_command(*w, e, asset_id_text, material, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    // Kick the asset load so the viewport picks it up next update.
    if (const auto* mc = w->get<runtime::MeshComponent>(e)) {
        if (mc->mesh_id.valid()) {
            m_manager.load_mesh(mc->mesh_id);
        }
    }
    after_mutation(e);
    return true;
}

bool EditorApp::drop_mesh_asset(const AssetEntry& entry, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_drop_mesh_command(entry, "Mesh", ecs::kInvalidEntity, out_err);
    if (!cmd) {
        return false;
    }
    ecs::Entity parent;
    if (m_selection.has_selection() && w->is_alive(m_selection.primary())) {
        parent = m_selection.primary();
    }
    // Rebuild with the selection as parent when valid.
    std::string derr;
    auto with_parent = make_drop_mesh_command(entry, "Mesh", parent, derr);
    if (with_parent) {
        cmd = std::move(with_parent);
    }
    m_stack.push(std::move(cmd), *w);
    // Start the asset load immediately.
    m_manager.load_mesh(entry.id);
    after_mutation(m_stack.last_target());
    return true;
}

bool EditorApp::ensure_primitive_asset(PrimitiveKind kind, assets::AssetId& out_id, std::string& out_err) {
    const std::string logical = primitive_asset_path(kind);
    // Reuse: a scene saved last session already points at this mesh, and a
    // second copy under a second id would fork the asset for no reason.
    if (auto existing = m_registry.find_by_path(logical)) {
        out_id = existing->id;
        m_manager.load_mesh(out_id);
        return true;
    }

    std::unique_ptr<rendering::StaticMesh> mesh;
    switch (kind) {
        case PrimitiveKind::Ground:
        case PrimitiveKind::Quad:
            mesh = rendering::StaticMesh::create_quad(1.0f);
            break;
        case PrimitiveKind::Cube:
            mesh = rendering::StaticMesh::create_cube(1.0f);
            break;
        case PrimitiveKind::Sphere:
            mesh = rendering::StaticMesh::create_sphere(0.5f, 24);
            break;
    }
    if (!mesh) {
        out_err = "Primitive mesh generation failed";
        return false;
    }

    const assets::AssetId id = assets::AssetId::generate();
    auto asset = rendering::make_mesh_asset(*mesh, id, logical);
    if (!asset) {
        out_err = "Primitive mesh cook failed";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!asset->save_to_bytes(bytes)) {
        out_err = "Primitive mesh serialization failed";
        return false;
    }
    // content:// holds the authored copy (what the asset browser lists and the
    // packager ships); cache:// holds the cooked copy the loader reads.
    if (!m_vfs.write_bytes(logical, std::span<const uint8_t>(bytes)).ok) {
        out_err = "Failed to write '" + logical + "'";
        return false;
    }
    const std::string rel = logical.substr(std::string("content://").size());
    const std::string cooked = "cache://" + rel;
    if (!m_vfs.write_bytes(cooked, std::span<const uint8_t>(bytes)).ok) {
        out_err = "Failed to write cooked '" + cooked + "'";
        return false;
    }
    assets::AssetMetadata meta;
    meta.id = id;
    meta.type = assets::AssetType::Mesh;
    meta.logical_path = logical;
    meta.cooked_path = cooked;
    meta.format = "nfmesh-v1";
    meta.version = 1;
    std::string aerr;
    if (!m_registry.add(meta, aerr)) {
        out_err = "Registry rejected primitive mesh: " + aerr;
        return false;
    }
    out_id = id;
    m_manager.load_mesh(out_id);
    return true;
}

bool EditorApp::create_primitive(PrimitiveKind kind, ecs::Entity parent, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    assets::AssetId id;
    if (!ensure_primitive_asset(kind, id, out_err)) {
        return false;
    }

    runtime::MeshComponent mc;
    mc.mesh_id = id;
    mc.material = runtime::kDefaultMaterialPath;

    // Placement per kind: everything lands ON the ground plane (top surface
    // y = 0), which is what makes a freshly created scene navigable without a
    // second edit.
    scene::Transform seed{};
    switch (kind) {
        case PrimitiveKind::Ground:
            // create_quad authors the quad in the XY plane, so -90 degrees about
            // X lays it flat with its +Z normal pointing up (visible from above).
            // The 5 cm drop matches the thin collider below: the surface you see
            // and the surface objects rest on are the same plane.
            seed.local_y = -0.05f;
            seed.rot_x = -90.0f;
            seed.scale_x = kGroundSize;
            seed.scale_y = kGroundSize;
            seed.scale_z = 1.0f;
            break;
        case PrimitiveKind::Sphere:
            seed.local_y = 0.5f; // radius 0.5: touches the ground
            break;
        case PrimitiveKind::Quad:
            seed.local_y = 1.0f; // standing upright, bottom edge on the ground
            break;
        case PrimitiveKind::Cube:
            seed.local_y = 0.5f;
            break;
    }

    // Suffix the name once it is taken, so three cubes read "Cube", "Cube 2",
    // "Cube 3" in the outliner instead of three rows nobody can tell apart.
    int same_kind = 0;
    for (ecs::Entity other : w->query<runtime::MeshComponent>()) {
        const auto* omc = w->get<runtime::MeshComponent>(other);
        if (omc != nullptr && omc->mesh_id == id) {
            ++same_kind;
        }
    }
    std::string name = primitive_name(kind);
    if (same_kind > 0) {
        name += " " + std::to_string(same_kind + 1);
    }

    auto cmd = std::make_unique<CreateMeshEntityCommand>(name, parent, mc, seed);
    m_stack.push(std::move(cmd), *w);
    const ecs::Entity created = m_stack.last_target();

    // The ground is the one primitive that must be solid or Play drops
    // everything through it. Physics components are leaf data (see
    // set_rigid_body): added directly, with no undo entry of their own.
    if (kind == PrimitiveKind::Ground && created.valid() && w->is_alive(created)) {
        physics::RigidBodyComponent rb;
        rb.type = physics::BodyType::Static;
        w->add<physics::RigidBodyComponent>(created, rb);
        physics::ColliderComponent col;
        // World-space half extents: a 60 x 60 plate 10 cm thick with its top
        // surface exactly at y = 0.
        col.shape = physics::Shape::make_box(Vec3(kGroundSize * 0.5f, 0.05f, kGroundSize * 0.5f));
        w->add<physics::ColliderComponent>(created, col);
        if (m_runtime != nullptr) {
            m_runtime->rebuild_physics_from_scene();
        }
    }

    m_manager.load_mesh(id);
    after_mutation(created);
    return true;
}

bool EditorApp::create_directional_light(ecs::Entity parent, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    ecs::Entity e = w->create_entity();
    w->add<scene::Transform>(e, scene::Transform{});
    w->add<scene::NameComponent>(e, scene::NameComponent{"Sun"});
    runtime::DirectionalLight light;
    // Late-morning sun: high enough to light a ground plane, angled enough that
    // surfaces model instead of flattening out under a head-on light.
    light.dir_x = -0.45f;
    light.dir_y = -1.0f;
    light.dir_z = -0.35f;
    light.color_r = 1.0f;
    light.color_g = 0.97f;
    light.color_b = 0.92f;
    light.intensity = 1.8f;
    light.cast_shadows = true;
    w->add<runtime::DirectionalLight>(e, light);
    if (parent.valid() && w->is_alive(parent)) {
        scene::set_parent(*w, e, parent);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::create_camera(ecs::Entity parent, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    ecs::Entity e = w->create_entity();
    scene::Transform t{};
    // Three metres up, eight back: the framing every sample scene uses, and the
    // one the runtime falls back to when a scene has no camera at all.
    t.local_y = 3.0f;
    t.local_z = 8.0f;
    t.dirty = true;
    w->add<scene::Transform>(e, t);
    w->add<scene::NameComponent>(e, scene::NameComponent{"Camera"});
    runtime::CameraComponent cam;
    cam.fov_y = 60.0f;
    cam.aspect = 16.0f / 9.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 1000.0f;
    cam.is_active = true;
    w->add<runtime::CameraComponent>(e, cam);
    if (parent.valid() && w->is_alive(parent)) {
        scene::set_parent(*w, e, parent);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::create_sky_entity(ecs::Entity parent, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    ecs::Entity e = w->create_entity();
    w->add<scene::Transform>(e, scene::Transform{});
    w->add<scene::NameComponent>(e, scene::NameComponent{"Sky"});
    // The component's own defaults are the natural palette (see
    // rendering::SkyParams), so "Add sky" means "the sky a real day has".
    w->add<runtime::SkyComponent>(e, runtime::SkyComponent{});
    if (parent.valid() && w->is_alive(parent)) {
        scene::set_parent(*w, e, parent);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::apply_sky_edit(const SkyEdit& edit, std::string& out_err) {
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    // One sky per scene: the renderer takes the first, so an existing entity is
    // updated in place instead of a second one being stacked on top of it.
    const std::vector<ecs::Entity> skies = w->query<runtime::SkyComponent>();
    ecs::Entity target = skies.empty() ? ecs::kInvalidEntity : skies.front();
    if (!target.valid()) {
        if (!create_sky_entity(ecs::kInvalidEntity, out_err)) {
            return false;
        }
        // Re-query rather than trusting stack().last_target(): create_sky_entity
        // creates the entity directly and pushes no command, so the last command
        // target is whatever the caller did before — not this sky.
        const std::vector<ecs::Entity> created = w->query<runtime::SkyComponent>();
        if (created.empty()) {
            out_err = "Sky entity was not created";
            return false;
        }
        target = created.front();
    }
    return set_sky(target, edit, out_err);
}

size_t EditorApp::export_meshes_to_file(const std::string& physical_path, assets::MeshFormat format,
                                        bool whole_scene, std::string& out_error) {
    ecs::World* w = world();
    if (w == nullptr) {
        out_error = "No scene open";
        return 0;
    }
    if (physical_path.empty()) {
        out_error = "No output path";
        return 0;
    }

    std::vector<ecs::Entity> targets;
    if (whole_scene) {
        for (ecs::Entity e : w->query<runtime::MeshComponent>()) {
            targets.push_back(e);
        }
    } else {
        targets = m_selection.all();
    }
    if (targets.empty()) {
        out_error = whole_scene ? "The scene has no mesh entities" : "Nothing selected";
        return 0;
    }

    // Baked copies own their pixels: the handle's MeshAsset is shared with the
    // live asset cache, and merging must not walk a vector the loader can drop.
    std::vector<assets::MeshAsset> baked;
    baked.reserve(targets.size());
    size_t exported = 0;
    for (ecs::Entity e : targets) {
        const auto* mc = w->get<runtime::MeshComponent>(e);
        const auto* t = w->get<scene::Transform>(e);
        if (mc == nullptr || t == nullptr || !mc->mesh_id.valid()) {
            continue;
        }
        auto handle = m_manager.find(mc->mesh_id);
        if (!handle || handle->state != assets::AssetState::Ready) {
            // An entity that was just created has a handle but no pixels yet;
            // exporting what the viewport already draws would silently skip it.
            handle = m_manager.load_mesh_sync(mc->mesh_id);
        }
        if (!handle || !handle->asset || handle->state != assets::AssetState::Ready) {
            continue;
        }
        baked.push_back(bake_mesh_world(*handle->asset, *t));
        ++exported;
    }
    if (baked.empty()) {
        out_error = "Nothing to export: the target entities have no loaded mesh";
        return 0;
    }

    const assets::MeshAsset merged = merge_mesh_assets(baked);
    std::string err;
    // The dialog's format wins over the typed extension: the user picked the
    // format in a combo, and a stale extension in the path field must not
    // silently produce a different file than the one they chose.
    if (!assets::export_mesh_to_file(merged, physical_path, err, true, format)) {
        out_error = err;
        return 0;
    }
    return exported;
}

bool EditorApp::set_rigid_body(ecs::Entity e, const physics::RigidBodyComponent& rb,
                                std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    // Add or replace the component directly. Physics components are not
    // undo-tracked in this phase — they are leaf data, and the physics body is
    // deliberately not created here: EditorApp::play() rebuilds the physics
    // world from the scene's components, so the body appears when play begins.
    if (auto* existing = w->get<physics::RigidBodyComponent>(e)) {
        *existing = rb;
    } else {
        w->add<physics::RigidBodyComponent>(e, rb);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::set_collider(ecs::Entity e, const physics::ColliderComponent& col,
                              std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (auto* existing = w->get<physics::ColliderComponent>(e)) {
        *existing = col;
    } else {
        w->add<physics::ColliderComponent>(e, col);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::set_destructible(ecs::Entity e, const runtime::DestructibleComponent& d,
                                 std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    // A spec the cooker cannot use is rejected here rather than written into the
    // scene: the artist would save a level whose object silently never breaks,
    // and the only symptom would be a warning in the load log.
    if (d.chunks < 2u) {
        out_err = "Chunks must be at least 2 (one chunk is an object that cannot come apart)";
        return false;
    }
    if (d.chunks > 64u) {
        // FractureParams::max_depth is the real ceiling: a deeper tree is not
        // reachable, so a higher number would silently mean 64 anyway.
        out_err = "Chunks must be at most 64 (the fracture tree's depth cap)";
        return false;
    }
    if (!std::isfinite(d.strength) || d.strength <= 0.0f) {
        out_err = "Strength must be a positive finite number";
        return false;
    }
    if (!std::isfinite(d.damage_threshold) || d.damage_threshold < 0.0f) {
        // 0 is legal: the object breaks on the slightest touch. A negative
        // threshold is not.
        out_err = "Damage threshold must be a non-negative finite number";
        return false;
    }
    if (!std::isfinite(d.blast_radius) || d.blast_radius <= 0.0f) {
        out_err = "Blast radius must be a positive finite number";
        return false;
    }
    if (auto* existing = w->get<runtime::DestructibleComponent>(e)) {
        *existing = d;
    } else {
        w->add<runtime::DestructibleComponent>(e, d);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::set_animation(ecs::Entity e, const animation::AnimationComponent& anim,
                              std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    auto* existing = w->get<animation::AnimationComponent>(e);
    if (existing == nullptr) {
        out_err = "Entity has no AnimationComponent";
        return false;
    }
    // Playback fields only. `clips`, `skeleton` and the procedural spec are the
    // scene's data and are left alone, so an inspector edit can never produce a
    // component that the save path cannot reproduce on the next load.
    existing->speed = anim.speed;
    existing->paused = anim.paused;
    existing->use_state_machine = anim.use_state_machine;
    existing->player.set_speed(anim.speed);
    existing->player.set_loop_mode(anim.player.loop_mode());
    if (!anim.player.clip_name().empty() && anim.player.clip_name() != existing->player.clip_name()) {
        existing->player.set_clip(anim.player.clip_name());
    }
    // Pausing and playing are player state, not just a flag: AnimationPlayer
    // ignores update() unless it is Playing, so the two must move together or
    // the checkbox would appear to do nothing.
    if (existing->paused) {
        existing->player.pause();
    } else {
        existing->player.play();
    }
    // The pose is applied as an offset from the placement captured on the first
    // step, so re-capture it here: otherwise the next frame would undo whatever
    // the user just did to the transform.
    if (m_runtime != nullptr) {
        m_runtime->rebase_animation(e);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::set_audio(ecs::Entity e, const audio::AudioComponent& aud, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    auto* existing = w->get<audio::AudioComponent>(e);
    if (existing == nullptr) {
        out_err = "Entity has no AudioComponent";
        return false;
    }
    // As above: the buffer and the procedural tone spec belong to the scene.
    existing->volume = aud.volume;
    existing->pitch = aud.pitch;
    existing->looping = aud.looping;
    existing->spatial = aud.spatial;
    existing->autoplay = aud.autoplay;
    existing->spatial_settings = aud.spatial_settings;
    if (existing->autoplay) {
        existing->playing = true;
    }
    after_mutation(e);
    return true;
}

// Reads and decodes one logical audio path, so the scene's music/ambience is
// audible the moment it is authored rather than after a save and reload. Same
// route `set_audio_buffer` takes for a per-entity source.
static bool resolve_audio_buffer(assets::VirtualFileSystem& vfs, const std::string& logical_path,
                                 audio::AudioBuffer& out, std::string& out_err) {
    auto bytes = vfs.read_bytes(logical_path);
    if (!bytes.ok) {
        out_err = "Audio file not found: " + logical_path;
        return false;
    }
    audio::DecodeOptions options;
    options.target_sample_rate = audio::kDefaultSampleRate;
    audio::DecodeResult decoded =
        audio::decode_audio_memory(bytes.value.data(), bytes.value.size(), options);
    if (!decoded.ok) {
        out_err = "Audio file could not be decoded (" + decoded.error + "): " + logical_path;
        return false;
    }
    out = std::move(decoded.buffer);
    return true;
}

bool EditorApp::set_reverb_zone(ecs::Entity e, const audio::ReverbZoneComponent& zone,
                                std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_reverb_zone_command(*w, e, zone, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_music(ecs::Entity e, const MusicEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!e.valid() || !w->is_alive(e)) {
        out_err = "Entity is not alive";
        return false;
    }

    audio::MusicComponent after;
    const std::string path = edit.buffer;
    after.buffer_name = path;
    after.volume = edit.volume;
    after.fade_in_seconds = edit.fade_in_seconds;
    after.enabled = edit.enabled;

    const auto* cur = w->get<audio::MusicComponent>(e);
    if (cur != nullptr && cur->buffer_name == path) {
        // The path did not change, so the samples already decoded are still the
        // right ones — carry them over instead of re-reading the file.
        after.owned_buffer = cur->owned_buffer;
    } else if (!resolve_audio_buffer(m_vfs, path, after.owned_buffer, out_err)) {
        // A path that cannot be read is REFUSED rather than stored: a component
        // naming a file nothing can decode is silent, and the author is standing
        // right here to be told. The same rule set_audio_buffer follows.
        return false;
    }

    auto cmd = make_music_command(*w, e, after, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_ambience(ecs::Entity e, const AmbienceEdit& edit, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!e.valid() || !w->is_alive(e)) {
        out_err = "Entity is not alive";
        return false;
    }

    audio::AmbienceComponent after;
    const std::string path = edit.buffer;
    after.buffer_name = path;
    after.fade_in_seconds = edit.fade_in_seconds;
    after.enabled = edit.enabled;

    const auto* cur = w->get<audio::AmbienceComponent>(e);
    if (cur != nullptr && cur->buffer_name == path) {
        after.owned_buffer = cur->owned_buffer;
    } else if (!resolve_audio_buffer(m_vfs, path, after.owned_buffer, out_err)) {
        return false;
    }

    auto cmd = make_ambience_command(*w, e, after, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::attach_gameplay_module(ecs::Entity e, const std::string& module_name,
                                       std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (module_name.empty()) {
        out_err = "Module name is empty";
        return false;
    }
    // Checked against the registry rather than merely non-empty: a component
    // naming a module no build can construct is data the scene can never use, and
    // letting the inspector create one would make that a one-click mistake.
    if (!gameplay::GameplayModuleRegistry::instance().contains(module_name)) {
        out_err = "No gameplay module named '" + module_name + "' is registered in this build";
        return false;
    }

    if (auto* existing = w->get<gameplay::GameplayModuleComponent>(e)) {
        if (existing->module_name == module_name) {
            return true;  // already attached; do not discard the saved state
        }
        existing->module_name = module_name;
        existing->properties.clear();
        existing->enabled = true;
        after_mutation(e);
        return true;
    }

    gameplay::GameplayModuleComponent comp;
    comp.module_name = module_name;
    w->add<gameplay::GameplayModuleComponent>(e, std::move(comp));
    after_mutation(e);
    return true;
}

bool EditorApp::detach_gameplay_module(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!w->has<gameplay::GameplayModuleComponent>(e)) {
        out_err = "Entity has no GameplayModuleComponent";
        return false;
    }
    w->remove<gameplay::GameplayModuleComponent>(e);
    after_mutation(e);
    return true;
}

namespace {

bool valid_script_path(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.find(' ') != std::string::npos) {
        return false;
    }
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".lua") != 0) {
        return false;
    }
    return path.rfind("content://", 0) == 0 || path.rfind("project://", 0) == 0;
}

std::string script_template(const std::string& logical_path) {
    std::string out = "-- " + logical_path + "\n";
    out += "-- Authored in the NOVAForge editor (Inspector > Script).\n";
    out += "-- `self` is this entity; see nf.* in Docs for the host API.\n";
    out += "function update(dt)\n";
    out += "end\n";
    return out;
}

} // namespace

bool EditorApp::create_script_file(const std::string& logical_path, std::string& out_err) {
    if (!valid_script_path(logical_path)) {
        out_err = "Script path must be a content:// or project:// .lua file with no spaces";
        return false;
    }
    auto exists = m_vfs.exists(logical_path);
    if (!exists.ok) {
        out_err = "Failed to check script path: " + exists.error;
        return false;
    }
    if (exists.value) {
        out_err = "Script file already exists: " + logical_path;
        return false;
    }
    auto written = m_vfs.write_text(logical_path, script_template(logical_path));
    if (!written.ok) {
        out_err = "Failed to write script file: " + written.error;
        return false;
    }
    invalidate_browser();
    return true;
}

bool EditorApp::create_unique_script_file(const std::string& dir_logical, const std::string& stem,
                                          std::string& out_path, std::string& out_err) {
    if (dir_logical.empty() || (dir_logical.rfind("content://", 0) != 0 &&
                                dir_logical.rfind("project://", 0) != 0)) {
        out_err = "Script folder must be a content:// or project:// directory";
        return false;
    }
    if (stem.empty() || stem.find_first_of(" /\\:") != std::string::npos) {
        out_err = "Script name must be non-empty with no spaces or separators";
        return false;
    }
    std::string dir = dir_logical;
    if (dir.back() != '/') {
        dir.push_back('/');
    }
    for (int i = 0; i < 100; ++i) {
        const std::string candidate =
            (i == 0) ? (dir + stem + ".lua")
                     : (dir + stem + "_" + (i < 10 ? "0" : "") + std::to_string(i) + ".lua");
        auto exists = m_vfs.exists(candidate);
        if (!exists.ok) {
            out_err = "Failed to check script path: " + exists.error;
            return false;
        }
        if (exists.value) {
            continue;
        }
        if (!create_script_file(candidate, out_err)) {
            return false;
        }
        out_path = candidate;
        return true;
    }
    out_err = "No free script name under " + dir_logical;
    return false;
}

bool EditorApp::attach_script(ecs::Entity e, const std::string& logical_path, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!valid_script_path(logical_path)) {
        out_err = "Script path must be a content:// or project:// .lua file with no spaces";
        return false;
    }
    auto bytes = m_vfs.read_bytes(logical_path);
    if (!bytes.ok) {
        out_err = "Script file not found: " + logical_path;
        return false;
    }
    if (auto* existing = w->get<scripting::ScriptComponent>(e)) {
        if (existing->path == logical_path) {
            return true;
        }
        existing->path = logical_path;
        existing->source.assign(reinterpret_cast<const char*>(bytes.value.data()), bytes.value.size());
        existing->enabled = true;
        if (m_runtime != nullptr) {
            m_runtime->clear_script_cache();
        }
        after_mutation(e);
        return true;
    }
    scripting::ScriptComponent comp;
    comp.path = logical_path;
    comp.source.assign(reinterpret_cast<const char*>(bytes.value.data()), bytes.value.size());
    comp.enabled = true;
    w->add<scripting::ScriptComponent>(e, std::move(comp));
    if (m_runtime != nullptr) {
        m_runtime->clear_script_cache();
    }
    after_mutation(e);
    return true;
}

bool EditorApp::set_script_enabled(ecs::Entity e, bool enabled, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    auto* comp = w->get<scripting::ScriptComponent>(e);
    if (comp == nullptr) {
        out_err = "Entity has no ScriptComponent";
        return false;
    }
    comp->enabled = enabled;
    after_mutation(e);
    return true;
}

bool EditorApp::set_script_path(ecs::Entity e, const std::string& logical_path, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    auto* comp = w->get<scripting::ScriptComponent>(e);
    if (comp == nullptr) {
        out_err = "Entity has no ScriptComponent";
        return false;
    }
    if (!valid_script_path(logical_path)) {
        out_err = "Script path must be a content:// or project:// .lua file with no spaces";
        return false;
    }
    auto bytes = m_vfs.read_bytes(logical_path);
    if (!bytes.ok) {
        out_err = "Script file not found: " + logical_path;
        return false;
    }
    comp->path = logical_path;
    comp->source.assign(reinterpret_cast<const char*>(bytes.value.data()), bytes.value.size());
    if (m_runtime != nullptr) {
        m_runtime->clear_script_cache();
    }
    after_mutation(e);
    return true;
}

bool EditorApp::detach_script(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!w->has<scripting::ScriptComponent>(e)) {
        out_err = "Entity has no ScriptComponent";
        return false;
    }
    w->remove<scripting::ScriptComponent>(e);
    if (m_runtime != nullptr) {
        m_runtime->clear_script_cache();
    }
    after_mutation(e);
    return true;
}

bool EditorApp::detach_destructible(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!w->has<runtime::DestructibleComponent>(e)) {
        out_err = "Entity has no DestructibleComponent";
        return false;
    }
    w->remove<runtime::DestructibleComponent>(e);
    after_mutation(e);
    return true;
}

bool EditorApp::set_audio_buffer(ecs::Entity e, const std::string& logical_path, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (logical_path.empty() || logical_path.find(' ') != std::string::npos) {
        out_err = "Audio path must be non-empty with no spaces";
        return false;
    }
    auto bytes = m_vfs.read_bytes(logical_path);
    if (!bytes.ok) {
        out_err = "Audio file not found: " + logical_path;
        return false;
    }
    audio::DecodeOptions options;
    options.target_sample_rate = audio::kDefaultSampleRate;
    audio::DecodeResult decoded =
        audio::decode_audio_memory(bytes.value.data(), bytes.value.size(), options);
    if (!decoded.ok) {
        out_err = "Audio file could not be decoded (" + decoded.error + ")";
        return false;
    }
    if (auto* existing = w->get<audio::AudioComponent>(e)) {
        existing->buffer_name = logical_path;
        existing->owned_buffer = std::move(decoded.buffer);
        existing->tone_hz = 0.0f;
        existing->tone_duration = 0.0f;
    } else {
        audio::AudioComponent aud;
        aud.buffer_name = logical_path;
        aud.owned_buffer = std::move(decoded.buffer);
        w->add<audio::AudioComponent>(e, std::move(aud));
    }
    after_mutation(e);
    return true;
}

namespace {

bool valid_particle_config(const vfx::EmitterConfig& cfg, std::string& out_err) {
    if (!std::isfinite(cfg.rate) || cfg.rate < 0.0f || cfg.rate > 100000.0f) {
        out_err = "Rate must be within [0, 100000]";
        return false;
    }
    if (!std::isfinite(cfg.lifetime) || cfg.lifetime <= 0.0f || cfg.lifetime > 60.0f) {
        out_err = "Lifetime must be within (0, 60]";
        return false;
    }
    if (!std::isfinite(cfg.lifetime_spread) || cfg.lifetime_spread < 0.0f ||
        cfg.lifetime_spread > 1.0f) {
        out_err = "Lifetime spread must be within [0, 1]";
        return false;
    }
    if (!std::isfinite(cfg.drag) || cfg.drag < 0.0f || cfg.drag > 10.0f) {
        out_err = "Drag must be within [0, 10]";
        return false;
    }
    if (!std::isfinite(cfg.start_size) || cfg.start_size < 0.0f ||
        !std::isfinite(cfg.end_size) || cfg.end_size < 0.0f) {
        out_err = "Sizes must be non-negative finite numbers";
        return false;
    }
    if (cfg.max_particles < 1u || cfg.max_particles > 1048576u) {
        out_err = "Max particles must be within [1, 1048576]";
        return false;
    }
    return true;
}

bool valid_cloth_config(const physics::ClothConfig& cfg, std::string& out_err) {
    if (cfg.res_x < 2 || cfg.res_x > 128 || cfg.res_z < 2 || cfg.res_z > 128) {
        out_err = "Resolution must be within [2, 128] per axis";
        return false;
    }
    if (!std::isfinite(cfg.spacing) || cfg.spacing <= 0.0f || cfg.spacing > 10.0f) {
        out_err = "Spacing must be within (0, 10]";
        return false;
    }
    if (!std::isfinite(cfg.mass) || cfg.mass <= 0.0f || cfg.mass > 1000.0f) {
        out_err = "Mass must be within (0, 1000]";
        return false;
    }
    if (!std::isfinite(cfg.damping) || cfg.damping < 0.0f || cfg.damping >= 1.0f) {
        out_err = "Damping must be within [0, 1)";
        return false;
    }
    if (!std::isfinite(cfg.stiffness) || cfg.stiffness < 0.0f || cfg.stiffness > 1.0f) {
        out_err = "Stiffness must be within [0, 1]";
        return false;
    }
    if (cfg.iterations < 1 || cfg.iterations > 32 || cfg.substeps < 1 || cfg.substeps > 8) {
        out_err = "Iterations must be within [1, 32] and substeps within [1, 8]";
        return false;
    }
    return true;
}

bool valid_character_config(const physics::CharacterConfig& cfg, std::string& out_err) {
    if (!std::isfinite(cfg.radius) || cfg.radius <= 0.0f || cfg.radius > 5.0f) {
        out_err = "Radius must be within (0, 5]";
        return false;
    }
    if (!std::isfinite(cfg.max_speed) || cfg.max_speed < 0.0f || cfg.max_speed > 100.0f) {
        out_err = "Max speed must be within [0, 100]";
        return false;
    }
    if (!std::isfinite(cfg.acceleration) || cfg.acceleration < 0.0f || cfg.acceleration > 1000.0f) {
        out_err = "Acceleration must be within [0, 1000]";
        return false;
    }
    if (!std::isfinite(cfg.air_control) || cfg.air_control < 0.0f || cfg.air_control > 1.0f) {
        out_err = "Air control must be within [0, 1]";
        return false;
    }
    if (!std::isfinite(cfg.jump_speed) || cfg.jump_speed < 0.0f || cfg.jump_speed > 50.0f) {
        out_err = "Jump speed must be within [0, 50]";
        return false;
    }
    if (!std::isfinite(cfg.slope_limit_deg) || cfg.slope_limit_deg < 0.0f ||
        cfg.slope_limit_deg > 90.0f) {
        out_err = "Slope limit must be within [0, 90]";
        return false;
    }
    if (!std::isfinite(cfg.mass) || cfg.mass <= 0.0f || cfg.mass > 10000.0f) {
        out_err = "Mass must be within (0, 10000]";
        return false;
    }
    if (!std::isfinite(cfg.friction) || cfg.friction < 0.0f || cfg.friction > 10.0f) {
        out_err = "Friction must be within [0, 10]";
        return false;
    }
    return true;
}

} // namespace

bool EditorApp::attach_particles(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (w->has<vfx::ParticleComponent>(e)) {
        return true;
    }
    w->add<vfx::ParticleComponent>(e, vfx::ParticleComponent{});
    after_mutation(e);
    return true;
}

bool EditorApp::set_particles(ecs::Entity e, const vfx::ParticleComponent& pc, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!valid_particle_config(pc.config, out_err)) {
        return false;
    }
    if (auto* existing = w->get<vfx::ParticleComponent>(e)) {
        *existing = pc;
    } else {
        w->add<vfx::ParticleComponent>(e, pc);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::detach_particles(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!w->has<vfx::ParticleComponent>(e)) {
        out_err = "Entity has no ParticleComponent";
        return false;
    }
    w->remove<vfx::ParticleComponent>(e);
    after_mutation(e);
    return true;
}

bool EditorApp::attach_cloth(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (w->has<physics::ClothComponent>(e)) {
        return true;
    }
    w->add<physics::ClothComponent>(e, physics::ClothComponent{});
    after_mutation(e);
    return true;
}

bool EditorApp::set_cloth(ecs::Entity e, const physics::ClothComponent& cc, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!valid_cloth_config(cc.config, out_err)) {
        return false;
    }
    if (auto* existing = w->get<physics::ClothComponent>(e)) {
        *existing = cc;
    } else {
        w->add<physics::ClothComponent>(e, cc);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::detach_cloth(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!w->has<physics::ClothComponent>(e)) {
        out_err = "Entity has no ClothComponent";
        return false;
    }
    w->remove<physics::ClothComponent>(e);
    after_mutation(e);
    return true;
}

bool EditorApp::attach_character(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (w->has<physics::CharacterComponent>(e)) {
        return true;
    }
    w->add<physics::CharacterComponent>(e, physics::CharacterComponent{});
    after_mutation(e);
    return true;
}

bool EditorApp::set_character(ecs::Entity e, const physics::CharacterComponent& ch,
                              std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!valid_character_config(ch.config, out_err)) {
        return false;
    }
    // Live input/state ride along structurally but are never authored here:
    // a config edit preserves the running intent rather than inventing it.
    if (auto* existing = w->get<physics::CharacterComponent>(e)) {
        const Vec3 wish = existing->wish_dir;
        const bool jump = existing->jump;
        const bool grounded = existing->grounded;
        *existing = ch;
        existing->wish_dir = wish;
        existing->jump = jump;
        existing->grounded = grounded;
    } else {
        physics::CharacterComponent fresh = ch;
        fresh.wish_dir = Vec3{0.0f, 0.0f, 0.0f};
        fresh.jump = false;
        fresh.grounded = false;
        w->add<physics::CharacterComponent>(e, fresh);
    }
    after_mutation(e);
    return true;
}

bool EditorApp::set_character_input(ecs::Entity e, const Vec3& wish_dir, bool jump,
                                    std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    auto* comp = w->get<physics::CharacterComponent>(e);
    if (comp == nullptr) {
        out_err = "Entity has no CharacterComponent";
        return false;
    }
    comp->wish_dir = wish_dir;
    comp->jump = jump;
    after_mutation(e);
    return true;
}

bool EditorApp::detach_character(ecs::Entity e, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr || !w->is_alive(e)) {
        out_err = "Entity not alive";
        return false;
    }
    if (!w->has<physics::CharacterComponent>(e)) {
        out_err = "Entity has no CharacterComponent";
        return false;
    }
    w->remove<physics::CharacterComponent>(e);
    after_mutation(e);
    return true;
}

bool EditorApp::create_asset_folder(const std::string& logical_dir, std::string& out_err) {
    if (!valid_asset_folder(logical_dir, out_err)) {
        return false;
    }
    auto exists = m_vfs.exists(logical_dir);
    if (!exists.ok) {
        out_err = "Failed to check folder: " + exists.error;
        return false;
    }
    if (exists.value) {
        out_err = "Folder already exists: " + logical_dir;
        return false;
    }
    auto made = m_vfs.create_directories(logical_dir);
    if (!made.ok) {
        out_err = "Failed to create folder: " + made.error;
        return false;
    }
    invalidate_browser();
    return true;
}

bool EditorApp::delete_asset(const std::string& logical_path, bool recursive, std::string& out_err) {
    if (logical_path.empty()) {
        out_err = "Asset path is empty";
        return false;
    }
    // Refuse a bare mount here as well as in the VFS: this is the layer a
    // future "delete by accident" would go through, and a clear refusal with
    // the offending name is worth more than a filesystem error code.
    if (logical_path == "content://" || logical_path == "project://" ||
        logical_path == "cache://") {
        out_err = "Refusing to delete a root: " + logical_path;
        return false;
    }
    auto is_dir = m_vfs.is_directory(logical_path);
    if (!is_dir.ok) {
        out_err = "Cannot inspect asset: " + is_dir.error;
        return false;
    }
    // A scene that is currently OPEN would leave the editor editing a file that
    // no longer exists. Refuse with an explanation rather than delete and
    // leave the session pointing at nothing.
    if (!is_dir.value && m_scene_path == logical_path) {
        out_err = "Cannot delete the open scene — open another scene first";
        return false;
    }
    // `recursive` is honoured rather than inferred: a caller that says
    // "this one file" must get the single-file path, so a future non-recursive
    // delete of a directory fails loudly instead of quietly taking a tree.
    const auto res = recursive ? m_vfs.remove_all(logical_path) : m_vfs.remove(logical_path);
    if (!res.ok) {
        out_err = "Delete failed: " + res.error;
        return false;
    }
    // Drop a selection that pointed at the deleted file, so the inspector does
    // not keep rendering fields for something that is gone.
    if (m_browser.selected_path == logical_path) {
        m_browser.selected_path.clear();
    }
    invalidate_browser();
    return true;
}

bool EditorApp::rename_asset(const std::string& from_logical, const std::string& to_logical,
                             std::string& out_err) {
    if (from_logical.empty() || to_logical.empty()) {
        out_err = "Asset path is empty";
        return false;
    }
    if (from_logical == to_logical) {
        return true; // nothing to do; not an error the user needs to read
    }
    // A rename target is a FILE path, so it inherits the script-path rules:
    // no spaces (a Lua host stops at the first space) and the file extensions
    // the engine actually consumes. A folder rename goes through the same door
    // so "Meshes " cannot be typed.
    if (to_logical.find(' ') != std::string::npos) {
        out_err = "Name must contain no spaces";
        return false;
    }
    const auto renamed = m_vfs.rename(from_logical, to_logical);
    if (!renamed.ok) {
        out_err = "Rename failed: " + renamed.error;
        return false;
    }
    // Keep the session's pointers at the asset's new name, so an open scene
    // that was renamed still saves and reloads.
    if (m_scene_path == from_logical) {
        m_scene_path = to_logical;
    }
    // A folder rename moves everything under it, so any pointer that names a
    // path INSIDE the old tree has to follow it — not just one that names the
    // tree root. A dock selection of "content://Meshes/cube.nfmesh" while
    // "content://Meshes" is renamed is the common case, and leaving it behind
    // means the panel keeps a highlighted row for a file that no longer exists
    // and its context menu opens a path that resolves to nothing.
    auto inside_old = [&from_logical](const std::string& p) {
        return p == from_logical ||
               (p.rfind(from_logical + "/", 0) == 0);
    };
    auto retarget = [&inside_old, &from_logical, &to_logical](std::string& p) {
        if (inside_old(p)) {
            p = to_logical + p.substr(from_logical.size());
        }
    };
    retarget(m_browser.selected_path);
    for (std::string& f : m_browser.favorites) {
        retarget(f);
    }
    for (std::string& b : m_browser.back_stack) {
        retarget(b);
    }
    for (std::string& f : m_browser.forward_stack) {
        retarget(f);
    }
    retarget(m_browser.current_folder);
    invalidate_browser();
    return true;
}

bool EditorApp::open_asset_in_ide(const std::string& logical_path, std::string& out_kind,
                                  std::string& out_err) {
    if (logical_path.empty()) {
        out_err = "Asset path is empty";
        return false;
    }
    auto resolved = m_vfs.resolve(logical_path);
    if (!resolved.ok) {
        out_err = "Cannot resolve asset path: " + resolved.error;
        return false;
    }
    IdeKind kind = IdeKind::ShellDefault;
    if (!open_file_in_ide(resolved.value, default_search_paths(), kind, out_err)) {
        return false;
    }
    switch (kind) {
        case IdeKind::VisualStudio: out_kind = "Visual Studio"; break;
        case IdeKind::VsCode: out_kind = "VS Code"; break;
        case IdeKind::ShellDefault: out_kind = "shell default"; break;
    }
    return true;
}

runtime::SaveSystem* EditorApp::save_system() {
    if (m_save_system == nullptr && m_runtime != nullptr) {
        m_save_system = std::make_unique<runtime::SaveSystem>(m_vfs, *m_runtime);
    }
    return m_save_system.get();
}

bool EditorApp::save_game(const std::string& slot, std::string& out_err) {
    runtime::SaveSystem* saves = save_system();
    if (saves == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    if (!saves->save_game(slot, out_err)) {
        return false;
    }
    // A save slot is not the scene file, so m_dirty is deliberately untouched:
    // saving progress does not mean the scene has been written anywhere.
    return true;
}

bool EditorApp::load_game(const std::string& slot, std::string& out_err) {
    runtime::SaveSystem* saves = save_system();
    if (saves == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    if (!saves->load_game(slot, out_err)) {
        return false;
    }
    // The world was replaced wholesale, so selection and undo history no longer
    // refer to anything that exists.
    m_selection.clear();
    m_dirty = false;
    return true;
}

std::vector<runtime::SaveSystem::SlotInfo> EditorApp::list_saves() {
    runtime::SaveSystem* saves = save_system();
    return saves != nullptr ? saves->list_saves() : std::vector<runtime::SaveSystem::SlotInfo>{};
}

bool EditorApp::has_save(const std::string& slot) {
    runtime::SaveSystem* saves = save_system();
    return saves != nullptr && saves->has_save(slot);
}

void EditorApp::set_autosave(float interval_seconds, const std::string& slot_prefix) {
    if (runtime::SaveSystem* saves = save_system()) {
        // Three explicit args, not two: SaveSystem declares a 2-arg and a
        // 3-arg-with-default `set_autosave` side by side, which makes a 2-arg
        // call ambiguous. Naming the default constant picks the 3-arg overload
        // with the exact value its default would have supplied.
        saves->set_autosave(interval_seconds, slot_prefix,
                            runtime::SaveSystem::kDefaultAutosaveSlots);
    }
}

bool EditorApp::autosave_enabled() {
    runtime::SaveSystem* saves = save_system();
    return saves != nullptr && saves->autosave_enabled();
}

unsigned EditorApp::autosaves_performed() {
    runtime::SaveSystem* saves = save_system();
    return saves != nullptr ? saves->autosaves_performed() : 0u;
}

void EditorApp::tick_autosave(float dt) {
    if (m_save_system != nullptr) {
        m_save_system->tick(dt);
    }
}

bool EditorApp::require_materials(std::string& out_err) const {
    if (m_runtime == nullptr) {        out_err = "Runtime not attached";
        return false;
    }
    if (m_play.playing()) {
        out_err = "Material edits are disabled while playing (press Stop first)";
        return false;
    }
    return true;
}

bool EditorApp::set_entity_material(ecs::Entity e, const std::string& material_path,
                                    std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_material_assignment_command(*w, e, material_path, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    after_mutation(e);
    return true;
}

bool EditorApp::set_material_params(const std::string& material_path, const MaterialEdit& edit,
                                    std::string& out_err) {
    if (!require_materials(out_err)) {
        return false;
    }
    auto cmd = make_material_params_command(*m_runtime, material_path, edit, out_err);
    ecs::World* w = world();
    if (!cmd || w == nullptr) {
        if (!cmd) {
            return false;
        }
        // Material table edits need no scene, but the stack replays against a
        // world — push only when one is open so undo stays symmetric.
        out_err = "No scene open";
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    if (m_runtime != nullptr) {
        m_runtime->mark_scene_edited();
    }
    if (const ecs::World* cw = world()) {
        m_selection.prune(*cw);
    }
    return true;
}

bool EditorApp::preview_material_params(const std::string& material_path, const MaterialEdit& edit,
                                        std::string& out_err) {
    if (!require_materials(out_err)) {
        return false;
    }
    // Same validation as the undoable path (the factory rejects), but the
    // command is applied directly and dropped: no undo entry per tick.
    auto cmd = make_material_params_command(*m_runtime, material_path, edit, out_err);
    if (!cmd) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    cmd->apply(*w);
    return true;
}

bool EditorApp::commit_material_params(const std::string& material_path,
                                       const rendering::PBRMaterialParams& before,
                                       std::string& out_err) {
    if (!require_materials(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    rendering::PBRMaterialParams after = read_material_params(*m_runtime, material_path);
    m_stack.push(std::make_unique<SetMaterialParamsCommand>(m_runtime, material_path, before, after),
                 *w);
    if (m_runtime != nullptr) {
        m_runtime->mark_scene_edited();
    }
    if (const ecs::World* cw = world()) {
        m_selection.prune(*cw);
    }
    return true;
}

bool EditorApp::set_material_albedo(const std::string& material_path, const std::string& texture_path,
                                    std::string& out_err) {
    if (!require_materials(out_err)) {
        return false;
    }
    // Pre-upload: rejects missing/undecodable textures BEFORE any command, so
    // the undo stack never fills with no-ops.
    if (!texture_path.empty() && !m_runtime->ensure_texture(texture_path, out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    const std::string before = m_runtime->material_albedo(material_path);
    m_stack.push(make_material_albedo_command(*m_runtime, material_path, before, texture_path), *w);
    if (const ecs::World* cw = world()) {
        m_selection.prune(*cw);
    }
    return true;
}

std::string EditorApp::material_albedo(const std::string& material_path) const {
    if (m_runtime == nullptr) {
        return {};
    }
    return m_runtime->material_albedo(material_path);
}

bool EditorApp::set_material_mip_mode(const std::string& material_path, rhi::MipMapMode mode,
                                      std::string& out_err) {
    if (!require_materials(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    auto cmd = make_material_mip_command(*m_runtime, material_path, mode, out_err);
    if (!cmd) {
        return false;
    }
    m_stack.push(std::move(cmd), *w);
    if (const ecs::World* cw = world()) {
        m_selection.prune(*cw);
    }
    return true;
}

rhi::MipMapMode EditorApp::material_mip_mode(const std::string& material_path) const {
    if (m_runtime == nullptr) {
        return rhi::MipMapMode::Linear;
    }
    return m_runtime->material_mip_mode(material_path);
}

std::vector<std::string> EditorApp::known_textures() {
    auto all = list_content_assets(m_vfs, m_registry);
    auto textures = filter_assets(all, "", static_cast<int>(assets::AssetType::Texture));
    std::vector<std::string> paths;
    for (const auto& e : textures) {
        paths.push_back(e.logical_path);
    }
    return paths;
}

bool EditorApp::import_file(const std::string& src_absolute, const std::string& dst_dir_logical,
                            bool overwrite, size_t& out_job, std::string& out_err) {
    out_job = m_imports.submit(src_absolute, dst_dir_logical, overwrite, out_err);
    return out_job != 0;
}

const ImportJob* EditorApp::process_one_import() {
    // Async pump: dispatches queued work to JobSystem workers and commits
    // whatever finished, in submit order. The returned job (if any) is the
    // first one that reached a terminal state during this pump; the pointer
    // is valid until the next queue mutation, and callers use it immediately.
    const ImportJob* terminal = m_imports.pump_async(m_vfs, m_registry);
    if (terminal != nullptr && terminal->state == ImportJob::State::Done) {
        watch_imported(*terminal);
    }
    if (terminal != nullptr) {
        invalidate_browser();
    }
    return terminal;
}

size_t EditorApp::process_imports() {
    auto terminal_count = [&]() {
        size_t n = 0;
        for (const auto& j : m_imports.jobs()) {
            if (j.state == ImportJob::State::Done || j.state == ImportJob::State::Failed) {
                ++n;
            }
        }
        return n;
    };
    const size_t before = terminal_count();
    m_imports.process_all(m_vfs, m_registry);
    const size_t done_now = terminal_count() - before;
    for (const auto& j : m_imports.jobs()) {
        if (j.state == ImportJob::State::Done) {
            watch_imported(j);
        }
    }
    if (done_now > 0) {
        invalidate_browser();
    }
    return done_now;
}

void EditorApp::watch_imported(const ImportJob& job) {
    // A model import writes meshes, textures AND materials, so every written
    // file is watched as whatever it landed as. Watching only the primary path
    // would leave an imported model's textures and materials invisible to hot
    // reload — the parts a developer edits most.
    if (!job.written.empty()) {
        for (const ImportWritten& written : job.written) {
            switch (written.type) {
                case assets::AssetType::Mesh:
                    if (const assets::AssetMetadata* meta =
                            m_registry.find_by_path(written.logical_path)) {
                        m_hot.watch_mesh(meta->id, meta->logical_path);
                    }
                    break;
                case assets::AssetType::Texture:
                    m_hot.watch_texture(written.logical_path);
                    break;
                case assets::AssetType::Material:
                    m_hot.watch_material(written.logical_path);
                    break;
                default:
                    break;
            }
        }
        return;
    }
    // No commit landed (a failed job): fall back to the primary destination,
    // which is still the right guess for the single-file types.
    if (job.type == assets::AssetType::Mesh) {
        if (const assets::AssetMetadata* meta = m_registry.find_by_path(job.dst_logical)) {
            m_hot.watch_mesh(meta->id, meta->logical_path);
        }
    } else if (job.type == assets::AssetType::Texture) {
        m_hot.watch_texture(job.dst_logical);
    } else if (job.type == assets::AssetType::Material) {
        m_hot.watch_material(job.dst_logical);
    }
    // Imported scenes are opened explicitly, never auto-reloaded.
}

void EditorApp::rebuild_hot_watch() {
    m_hot.clear();
    const ecs::World* w = world();
    if (w != nullptr) {
        for (ecs::Entity e : w->query<runtime::MeshComponent>()) {
            const auto* mc = w->get<runtime::MeshComponent>(e);
            if (mc == nullptr || !mc->mesh_id.valid()) {
                continue;
            }
            const assets::AssetMetadata* meta = m_registry.find(mc->mesh_id);
            if (meta != nullptr && !meta->logical_path.empty()) {
                m_hot.watch_mesh(mc->mesh_id, meta->logical_path);
            }
        }
    }
    if (m_runtime != nullptr) {
        for (const auto& p : m_runtime->known_material_paths()) {
            m_hot.watch_material(p);
        }
        for (const auto& t : m_runtime->known_texture_paths()) {
            m_hot.watch_texture(t);
        }
    }
    // First poll after a rebuild adopts baselines silently (FileWatcher
    // semantics), so rebuilding never echoes as external changes.
}

size_t EditorApp::poll_hot_reload() {
    if (m_runtime == nullptr) {
        return 0;
    }
    size_t reloaded = 0;
    for (const auto& r : m_hot.poll(m_vfs, m_registry, *m_runtime)) {
        if (r.ok) {
            ++reloaded;
            m_console.push(LogMessage{LogLevel::Info, LogCategory::Editor,
                                      "Hot reload [" + r.kind + "] " + r.path + ": " + r.message,
                                      std::chrono::system_clock::now(), __FILE__, __LINE__});
        } else {
            m_console.push(LogMessage{LogLevel::Warn, LogCategory::Editor,
                                      "Hot reload [" + r.kind + "] " + r.path + ": " + r.message,
                                      std::chrono::system_clock::now(), __FILE__, __LINE__});
        }
    }
    if (reloaded > 0) {
        m_runtime->mark_scene_edited();
    }
    return reloaded;
}

bool EditorApp::save_material(const std::string& src_path, const std::string& dst_path,
                              std::string& out_err) {
    if (m_runtime == nullptr) {
        out_err = "Runtime not attached";
        return false;
    }
    if (!m_runtime->save_material(src_path, dst_path, out_err)) {
        return false;
    }
    // Our own write must not echo back as an external change.
    m_hot.refresh(m_vfs, dst_path);
    return true;
}

std::vector<std::string> EditorApp::known_materials() const {
    if (m_runtime == nullptr) {
        return {runtime::kDefaultMaterialPath};
    }
    return m_runtime->known_material_paths();
}

bool EditorApp::materials_dirty() const {
    return m_runtime != nullptr && m_runtime->any_material_dirty();
}

bool EditorApp::undo(std::string& out_err) {
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (m_play.playing()) {
        out_err = "Undo is disabled while playing";
        return false;
    }
    if (!m_stack.undo(*w)) {
        out_err = "Nothing to undo";
        return false;
    }
    m_runtime->mark_scene_edited();
    m_dirty = true;
    m_selection.prune(*w);
    return true;
}

bool EditorApp::redo(std::string& out_err) {
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (m_play.playing()) {
        out_err = "Redo is disabled while playing";
        return false;
    }
    if (!m_stack.redo(*w)) {
        out_err = "Nothing to redo";
        return false;
    }
    m_runtime->mark_scene_edited();
    m_dirty = true;
    m_selection.prune(*w);
    return true;
}

static bool valid_prefab_path(const std::string& path) {
    if (path.rfind("content://", 0) != 0) {
        return false;
    }
    const std::string ext = ".nfscene";
    return path.size() > ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

bool EditorApp::create_prefab(ecs::Entity root, const std::string& prefab_path, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!root.valid() || !w->is_alive(root)) {
        out_err = "Entity is not alive";
        return false;
    }
    if (!valid_prefab_path(prefab_path)) {
        out_err = "Prefab path must look like content://Prefabs/<name>.nfscene";
        return false;
    }
    // Snapshot the subtree into a template scene (top link stripped: a
    // template root links nowhere; nested links are preserved verbatim).
    scene::Scene tpl(entity_label(*w, root));
    ecs::Entity tpl_root = clone_subtree(*w, root, tpl.world(), ecs::kInvalidEntity);
    if (!tpl_root.valid()) {
        out_err = "Subtree clone failed";
        return false;
    }
    tpl.world().remove<scene::PrefabLinkComponent>(tpl_root);
    if (!runtime::save_scene_to_vfs(m_vfs, prefab_path, tpl, out_err)) {
        return false;
    }
    // Tag the instance root (undoable, like any scene edit).
    const auto* cur = w->get<scene::PrefabLinkComponent>(root);
    const bool had = (cur != nullptr);
    const scene::PrefabLinkComponent before = had ? *cur : scene::PrefabLinkComponent{};
    m_stack.push(std::make_unique<SetPrefabLinkCommand>(
                     root, had, before, scene::PrefabLinkComponent{prefab_path}),
                 *w);
    after_mutation(root);
    m_console.push(LogMessage{LogLevel::Info, LogCategory::Editor, "Prefab saved '" + prefab_path + "'",
                              std::chrono::system_clock::now(), __FILE__, __LINE__});
    invalidate_browser();
    return true;
}

bool EditorApp::instantiate_prefab(const std::string& prefab_path, ecs::Entity parent,
                                   std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (parent.valid() && !w->is_alive(parent)) {
        out_err = "Parent entity is not alive";
        return false;
    }
    auto loaded = runtime::load_scene_from_vfs(m_vfs, prefab_path);
    if (!loaded.success || !loaded.scene) {
        out_err = "Cannot load prefab '" + prefab_path + "': " + loaded.error;
        return false;
    }
    m_stack.push(std::make_unique<InstantiatePrefabCommand>(std::move(*loaded.scene), prefab_path, parent),
                 *w);
    after_mutation(m_stack.last_target());
    return true;
}

bool EditorApp::apply_prefab(ecs::Entity instance_root, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!instance_root.valid() || !w->is_alive(instance_root)) {
        out_err = "Entity is not alive";
        return false;
    }
    const auto* link = w->get<scene::PrefabLinkComponent>(instance_root);
    if (link == nullptr || link->prefab_path.empty()) {
        out_err = "Entity is not a prefab instance root";
        return false;
    }
    scene::Scene tpl(entity_label(*w, instance_root));
    ecs::Entity tpl_root = clone_subtree(*w, instance_root, tpl.world(), ecs::kInvalidEntity);
    if (!tpl_root.valid()) {
        out_err = "Subtree clone failed";
        return false;
    }
    tpl.world().remove<scene::PrefabLinkComponent>(tpl_root);
    if (!runtime::save_scene_to_vfs(m_vfs, link->prefab_path, tpl, out_err)) {
        return false;
    }
    m_console.push(LogMessage{LogLevel::Info, LogCategory::Editor,
                              "Prefab applied '" + link->prefab_path + "'",
                              std::chrono::system_clock::now(), __FILE__, __LINE__});
    return true;
}

bool EditorApp::revert_prefab(ecs::Entity instance_root, std::string& out_err) {
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!instance_root.valid() || !w->is_alive(instance_root)) {
        out_err = "Entity is not alive";
        return false;
    }
    const auto* link = w->get<scene::PrefabLinkComponent>(instance_root);
    if (link == nullptr || link->prefab_path.empty()) {
        out_err = "Entity is not a prefab instance root";
        return false;
    }
    const std::string path = link->prefab_path;
    ecs::Entity parent;
    if (const auto* t = w->get<scene::Transform>(instance_root)) {
        parent = t->parent;
    }
    auto loaded = runtime::load_scene_from_vfs(m_vfs, path);
    if (!loaded.success || !loaded.scene) {
        out_err = "Cannot load prefab '" + path + "': " + loaded.error;
        return false;
    }
    auto del = DeleteSubtreeCommand::capture(*w, instance_root, out_err);
    if (!del) {
        return false;
    }
    auto inst =
        std::make_unique<InstantiatePrefabCommand>(std::move(*loaded.scene), path, parent);
    m_stack.push(std::make_unique<RevertPrefabCommand>(std::move(del), std::move(inst)), *w);
    after_mutation(m_stack.last_target());
    return true;
}

bool EditorApp::play(std::string& out_err) {
    if (m_runtime == nullptr || m_runtime->edit_scene() == nullptr) {
        out_err = "No scene open";
        return false;
    }
    // A drag cannot cross into play mode (edits lock there): fold it away.
    {
        std::string dummy;
        viewport_abort_drag(dummy);
    }
    // Physics bodies are built from the scene when it is *loaded*. A RigidBody
    // and Collider added through the inspector while the scene is already open
    // therefore has no body yet, and Play would simulate the scene as it was on
    // disk — the entity the user just gave physics to would sit perfectly still.
    // Rebuilding here makes Play start from the authored components, which is
    // also the right semantics: a play session begins from the scene state, not
    // from wherever the previous session left the simulation.
    m_runtime->rebuild_physics_from_scene();
    if (!m_play.play(*m_runtime->edit_scene(), out_err)) {
        return false;
    }
    // A trace exported from now on is "this play session", not "everything
    // since the editor started", which is what the Profiler panel's export
    // button promises.
    m_profiler_session.clear();
    return true;
}

bool EditorApp::stop(std::string& out_err) {
    if (!m_play.playing()) {
        return m_play.stop(out_err);
    }
    // Restore the pre-play edit world: physics/animation/gameplay all wrote
    // into the live Transforms during the session (fallen boxes, driven
    // entities). Without this, Stop leaves the play-mutated poses behind and
    // the next Play starts from the wreckage — "Stop does not undo the game".
    // Structural edits are locked while playing, so the snapshot and the live
    // world differ only in values, never in membership; restore is exact.
    if (m_play.snapshot() != nullptr && m_runtime != nullptr &&
        m_runtime->edit_scene() != nullptr) {
        restore_scene(*m_runtime->edit_scene(), *m_play.snapshot());
        m_runtime->rebuild_physics_from_scene();
        m_runtime->mark_scene_edited();
        m_selection.prune(*world());
        {
            std::string dummy;
            viewport_abort_drag(dummy);
        }
    }
    return m_play.stop(out_err);
}

bool EditorApp::launch_game(std::string& out_err) {
    if (m_runtime == nullptr || m_runtime->edit_scene() == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (m_runtime->edit_scene()->world().alive_entity_count() == 0) {
        out_err = "Nothing to play (empty scene)";
        return false;
    }

    // Home for the play-session scene: the first scheme this session's VFS
    // mounts. The player process mounts the same set — the same .nfproj, or
    // the same engine-tree fallback — so the path written here is the path
    // read there, with no second source of truth to drift.
    std::string session_logical;
    for (const char* scheme : {"cache://", "project://", "content://"}) {
        const std::string candidate = std::string(scheme) + "EditorPlaySession.nfscene";
        if (m_vfs.resolve(candidate).ok) {
            session_logical = candidate;
            break;
        }
    }
    if (session_logical.empty()) {
        out_err = "No mounted scheme for the play-session scene (cache://, project://, content://)";
        return false;
    }
    // The live edit scene, not the on-disk file: Play must play exactly what
    // is on screen, saved or not. This writes no dirty flag and moves no
    // editor state — the session file is a handoff, not a save.
    if (!m_runtime->save_scene(session_logical, out_err)) {
        out_err = "Could not write the play session scene: " + out_err;
        return false;
    }

    // The player ships beside the editor in every build layout (both CMake
    // targets land in build/<preset>/bin). Prefer the dedicated player; the
    // runtime sample hosts the same loop as a fallback.
    wchar_t exe_buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) == 0) {
        out_err = "Could not locate the editor executable";
        return false;
    }
    const std::filesystem::path editor_dir = std::filesystem::path(exe_buf).parent_path();
    std::filesystem::path player = editor_dir / L"NFGamePlayer.exe";
    if (!std::filesystem::exists(player)) {
        player = editor_dir / L"NFSampleRuntimeScene.exe";
    }
    if (!std::filesystem::exists(player)) {
        out_err = "Game player not found next to the editor (build the NFGamePlayer target)";
        return false;
    }

    // Wide characters throughout: project paths are user input and may be
    // non-ASCII.
    std::wstring cmd = L"\"" + player.wstring() + L"\" --scene \"" +
                       std::filesystem::path(session_logical).wstring() + L"\"";
    if (!m_project_path.empty()) {
        cmd += L" --project \"" + std::filesystem::path(m_project_path).wstring() + L"\"";
    }

    // Working directory: without a project the player walks up from here
    // looking for Engine/ + Content/, so it gets the engine root (the parent
    // of the content mount). With a project the mounts come from the .nfproj
    // and the project directory is simply the sensible home.
    std::filesystem::path cwd = editor_dir;
    if (!m_project_path.empty()) {
        cwd = std::filesystem::path(m_project_path).parent_path();
    } else if (const auto content = m_vfs.resolve("content://"); content.ok) {
        cwd = content.value.parent_path();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // DETACHED_PROCESS: the player is a console-subsystem binary and must not
    // flash a black console next to its game window. The editor never waits —
    // the game window and the editor are independent from here on, which is
    // the point: closing the game window is the Stop, and editing continues
    // meanwhile.
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                        DETACHED_PROCESS | CREATE_DEFAULT_ERROR_MODE, nullptr,
                                        cwd.c_str(), &si, &pi);
    if (started == 0) {
        out_err = "Could not start the game player (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    m_console.push(LogMessage{LogLevel::Info, LogCategory::Editor,
                              "Game launched from " + session_logical +
                                  " — close the game window to stop",
                              std::chrono::system_clock::now(), __FILE__, __LINE__});
    return true;
}

ecs::Entity EditorApp::pick(const ViewCamera& cam, float ndc_x, float ndc_y) {
    ecs::World* w = world();
    if (w == nullptr) {
        return ecs::kInvalidEntity;
    }

    // GPU path first. It resolves through an id pass, so it honours occlusion
    // and true pixel coverage — the CPU ray/AABB test below picks against world
    // bounds and cannot tell what is in front of what. Falls through when the
    // picker is unavailable (no pick shaders) or the pixel is empty, so the CPU
    // path stays the safety net rather than a dead alternative.
    if (m_runtime != nullptr && m_viewport.width > 0 && m_viewport.height > 0) {
        const auto fw = static_cast<float>(m_viewport.width);
        const auto fh = static_cast<float>(m_viewport.height);
        // NDC (-1..1, +y up) -> pixels (0..size-1, +y down).
        float px = 0.0f, py = 0.0f;
        viewport_ndc_to_pixel(ndc_x, ndc_y, fw, fh, px, py);
        if (px >= 0.0f && py >= 0.0f && px < fw && py < fh) {
            uint32_t picked_id = 0;
            if (m_runtime->pick_entity_gpu(static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                                           picked_id)) {
                // The id pass carries the raw entity id; a usable handle also
                // needs the live generation, which only the world knows.
                for (ecs::Entity e : w->all_entities()) {
                    if (e.id == picked_id) {
                        return e;
                    }
                }
                // Stale id (the entity was destroyed after the frame was drawn):
                // report a miss rather than inventing a handle.
                return ecs::kInvalidEntity;
            }
        }
    }

    // One bounds source: world_bounds() is what the pick ray tests against AND
    // what frame_selection() centres on, so "what you can click" and "what
    // Frame frames" can never describe different boxes.
    auto bounds_of = [this](ecs::Entity e) -> std::optional<AABB> {
        AABB b;
        if (world_bounds(e, b)) {
            return b;
        }
        return std::nullopt;
    };
    return pick_entity(*w, bounds_of, cam, ndc_x, ndc_y);
}

// --- View pivot + framing ---------------------------------------------------
//
// world_bounds() is deliberately the same code the picker uses (the lambda above
// is the same body), so "what the user can click" and "what Frame centres on"
// can never describe different boxes. It is exposed rather than inlined twice
// because framing, the tests, and any future gizmo-scaling logic all need it.

// Radius of the sphere that circumscribes a box. Framing aims the camera with
// this rather than with the half-diagonal in a chosen plane, so a long thin
// object and a cube of the same diagonal get the same treatment.
static float box_radius(const AABB& b) {
    const float rx = (b.max_x - b.min_x) * 0.5f;
    const float ry = (b.max_y - b.min_y) * 0.5f;
    const float rz = (b.max_z - b.min_z) * 0.5f;
    return std::sqrt(rx * rx + ry * ry + rz * rz);
}

bool EditorApp::world_bounds(ecs::Entity e, AABB& out) const {
    const ecs::World* w = m_runtime != nullptr ? &m_runtime->edit_scene()->world() : nullptr;
    if (w == nullptr) {
        return false;
    }
    const auto* mc = w->get<runtime::MeshComponent>(e);
    const auto* tr = w->get<scene::Transform>(e);
    if (mc == nullptr || tr == nullptr || !mc->mesh_id.valid()) {
        return false;
    }
    const auto handle = m_manager.find(mc->mesh_id);
    if (!handle || !handle->asset) {
        return false;
    }
    // Through the FULL world matrix, not a translation. This is the same box the
    // renderer draws and the same one the pick ray tests, which is what makes
    // "click the object" and "Frame the object" agree. Translating only meant a
    // scaled ground plane was clickable in a 1x1 box at its origin, and Frame
    // centred on that phantom cube.
    const rendering::AABB local = assets_to_render_aabb(handle->asset->bounds);
    out = to_editor_aabb(rendering::transform_aabb(local, scene::world_matrix(*tr)));
    return true;
}

bool EditorApp::selection_bounds(AABB& out) const {
    const ecs::World* w = m_runtime != nullptr ? &m_runtime->edit_scene()->world() : nullptr;
    if (w == nullptr) {
        return false;
    }
    bool any = false;
    AABB acc{};
    for (ecs::Entity e : m_selection.all()) {
        AABB b;
        if (!world_bounds(e, b) || !w->is_alive(e)) {
            continue;
        }
        if (!any) {
            acc = b;
            any = true;
            continue;
        }
        acc.min_x = std::min(acc.min_x, b.min_x);
        acc.min_y = std::min(acc.min_y, b.min_y);
        acc.min_z = std::min(acc.min_z, b.min_z);
        acc.max_x = std::max(acc.max_x, b.max_x);
        acc.max_y = std::max(acc.max_y, b.max_y);
        acc.max_z = std::max(acc.max_z, b.max_z);
    }
    if (any) {
        out = acc;
    }
    return any;
}

bool EditorApp::scene_bounds(AABB& out) const {
    const ecs::World* w = m_runtime != nullptr ? &m_runtime->edit_scene()->world() : nullptr;
    if (w == nullptr) {
        return false;
    }
    bool any = false;
    AABB acc{};
    for (auto e : w->query<runtime::MeshComponent>()) {
        AABB b;
        if (!world_bounds(e, b)) {
            continue;
        }
        if (!any) {
            acc = b;
            any = true;
            continue;
        }
        acc.min_x = std::min(acc.min_x, b.min_x);
        acc.min_y = std::min(acc.min_y, b.min_y);
        acc.min_z = std::min(acc.min_z, b.min_z);
        acc.max_x = std::max(acc.max_x, b.max_x);
        acc.max_y = std::max(acc.max_y, b.max_y);
        acc.max_z = std::max(acc.max_z, b.max_z);
    }
    if (any) {
        out = acc;
    }
    return any;
}

bool EditorApp::frame_selection(std::string& out_err) {
    if (!m_selection.has_selection()) {
        out_err = "Nothing selected to frame";
        return false;
    }
    AABB b;
    if (!selection_bounds(b)) {
        // Selected, but nothing in it is a mesh: a camera or a light has no
        // extent to frame, and centring on its origin is the next best thing.
        // Said plainly rather than failing with "no bounds".
        for (ecs::Entity e : m_selection.all()) {
            const ecs::World* w =
                m_runtime != nullptr ? &m_runtime->edit_scene()->world() : nullptr;
            const auto* tr = w != nullptr ? w->get<scene::Transform>(e) : nullptr;
            if (tr != nullptr && w->is_alive(e)) {
                move_view_pivot_to(tr->world_x, tr->world_y, tr->world_z, 0.0f);
                return true;
            }
        }
        out_err = "Selection has nothing to frame";
        return false;
    }
    move_view_pivot_to((b.min_x + b.max_x) * 0.5f, (b.min_y + b.max_y) * 0.5f,
                       (b.min_z + b.max_z) * 0.5f, box_radius(b));
    return true;
}

bool EditorApp::frame_all(std::string& out_err) {
    AABB b;
    if (!scene_bounds(b)) {
        out_err = "Scene has nothing to frame";
        return false;
    }
    move_view_pivot_to((b.min_x + b.max_x) * 0.5f, (b.min_y + b.max_y) * 0.5f,
                       (b.min_z + b.max_z) * 0.5f, box_radius(b));
    return true;
}

float EditorApp::consume_view_fit() {
    const float r = m_view_fit_radius;
    m_view_fit_radius = 0.0f;
    return r;
}

float EditorApp::fit_distance_for(float radius, float fov_y_deg, float margin) {
    // A sphere of radius R is fully inside a vertical half-angle of fov/2 only
    // at distance R / sin(fov/2); the margin backs the camera off so the object
    // does not sit flush against the top and bottom of the viewport. A long thin
    // object (a 60 m ground plane) is the case that matters: using the box
    // diagonal instead of the sphere over-zooms it by a factor of two, and the
    // user has to wheel back out every single time.
    const float half = fov_y_deg * 0.5f * 3.14159265359f / 180.0f;
    const float s = std::sin(half);
    if (s < 1e-4f) {
        return radius * margin * 4.0f; // degenerate fov: a sane, large answer
    }
    return (radius / s) * margin;
}

void EditorApp::move_view_pivot_to(float x, float y, float z, float fit_radius) {
    const float dx = x - m_view_pivot[0];
    const float dy = y - m_view_pivot[1];
    const float dz = z - m_view_pivot[2];
    m_view_pivot[0] = x;
    m_view_pivot[1] = y;
    m_view_pivot[2] = z;
    m_view_fit_radius = fit_radius;
    // Carry the eye with the pivot. Without this the camera would keep its world
    // position and the orbit would swing it to a completely different angle the
    // moment the pivot moved — framing a selection would also rotate the user's
    // view, which reads as the camera glitching rather than as a focus action.
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) {
        return;
    }
    ecs::World* w = m_runtime != nullptr ? &m_runtime->edit_scene()->world() : nullptr;
    if (w == nullptr) {
        return;
    }
    for (auto e : w->query<runtime::CameraComponent>()) {
        const auto* c = w->get<runtime::CameraComponent>(e);
        auto* tr = w->get<scene::Transform>(e);
        if (c == nullptr || tr == nullptr || !c->is_active) {
            continue;
        }
        tr->local_x += dx;
        tr->local_y += dy;
        tr->local_z += dz;
        tr->dirty = true;
        break;
    }
    scene::propagate_transforms(*w);
}

namespace {

constexpr float kDragDeadzoneNdc = 0.004f; // clicks must not become micro-drags
constexpr float kPi = 3.14159265358979323846f;

float drag_distance_to(const ViewCamera& vc, const scene::Transform& t) {
    const float dx = t.world_x - vc.px;
    const float dy = t.world_y - vc.py;
    const float dz = t.world_z - vc.pz;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    return len > 1e-4f ? len : 1e-4f;
}

// World point into the pointer NDC the gesture path consumes (y-up). Mirrors
// main.cpp's world_to_pointer_ndc: look_at * perspective, then negate y for
// the Vulkan flip — one convention shared by pick_ray and the drag math.
Vec3 gizmo_to_pointer_ndc(const ViewCamera& vc, const Vec3& world) {
    const Mat4 view =
        Mat4::look_at(Vec3{vc.px, vc.py, vc.pz}, Vec3{vc.tx, vc.ty, vc.tz}, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = Mat4::perspective(vc.fov_y_deg * kPi / 180.0f, vc.aspect, vc.near_plane,
                                        vc.far_plane);
    const Vec3 ndc = (view * proj).transform_point(world);
    return Vec3{ndc.x, -ndc.y, ndc.z};
}

void gizmo_frame_axes(const scene::Transform& t, GizmoSpace space, float axes[3][3]) {
    axes[0][0] = 1.0f; axes[0][1] = 0.0f; axes[0][2] = 0.0f;
    axes[1][0] = 0.0f; axes[1][1] = 1.0f; axes[1][2] = 0.0f;
    axes[2][0] = 0.0f; axes[2][1] = 0.0f; axes[2][2] = 1.0f;
    if (space != GizmoSpace::Local) {
        return;
    }
    // t.world_rot, NOT a quaternion rebuilt from t.rot_*. The two agree for a
    // root and disagree for a child of a rotated parent — and it is the DRAG
    // mapping, not just the drawing, that depends on this. Using the local angles
    // made a Local-space axis drag on a parented object move it along an axis it
    // was not aligned to: the gizmo drew one thing and the drag did another.
    // The same value the view layer draws with (TransformGizmoView.cpp), so the
    // arrows the user aims at are the arrows the drag follows.
    const Quat q = t.world_rot;
    for (int a = 0; a < 3; ++a) {
        const Vec3 w = q.rotate(Vec3{axes[a][0], axes[a][1], axes[a][2]});
        axes[a][0] = w.x;
        axes[a][1] = w.y;
        axes[a][2] = w.z;
    }
}

} // namespace

bool EditorApp::viewport_press(float ndc_x, float ndc_y, const ViewCamera& vc, bool additive,
                               std::string& out_err) {
    m_drag_handle = GizmoHandle::None; // a pick starts a camera-plane drag, never a handle one
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    const ecs::Entity hit = pick(vc, ndc_x, ndc_y);
    if (!hit.valid()) {
        // Miss: deselect (standard), and make sure no stale drag survives.
        m_selection.clear();
        m_drag.cancel();
        return true;
    }
    if (additive) {
        // Shift-click toggles: a second click on an already-picked entity
        // drops it back out, so a group can be trimmed without restarting.
        if (m_selection.contains(hit)) {
            m_selection.remove(hit);
        } else {
            m_selection.add(hit);
        }
    } else {
        m_selection.set_single(hit);
    }
    if (!m_selection.has_selection()) {
        m_drag.cancel();
        return true;
    }
    // The drag arms on the whole selection (everything with a Transform);
    // entities without one are named in out_err by begin(), not fatal.
    if (!m_drag.begin(*w, m_selection.all(), m_gizmo_mode, m_gizmo_space, m_gizmo_snap,
                      out_err)) {
        return false;
    }
    // Perspective scaling uses the distance to the entity the pointer
    // actually hit (the drag primary): v0.1 has no merged group bounds.
    const auto* t = w->get<scene::Transform>(m_drag.entity());
    if (t == nullptr) {
        m_drag.cancel();
        out_err = "Selected entity has no Transform";
        return false;
    }
    m_drag_vc = vc;
    m_drag_ndc0x = ndc_x;
    m_drag_ndc0y = ndc_y;
    m_drag_lastx = ndc_x;
    m_drag_lasty = ndc_y;
    m_drag_distance = drag_distance_to(vc, *t);
    m_drag_moved = false;
    return true;
}

bool EditorApp::viewport_gizmo_press(GizmoHandle handle, float ndc_x, float ndc_y,
                                     const ViewCamera& vc, std::string& out_err) {
    m_drag_handle = GizmoHandle::None;
    if (handle == GizmoHandle::None) {
        out_err = "No gizmo handle grabbed";
        return false;
    }
    if (!require_editable(out_err)) {
        return false;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        out_err = "No scene open";
        return false;
    }
    if (!m_selection.has_selection()) {
        out_err = "Nothing selected";
        return false;
    }
    // Handle/mode compatibility: planes exist only for translate, and the
    // center box is translate (plane move) or scale (uniform) — never rotate.
    // Axis handles exist in all three modes (arrows / rings / arms).
    const bool is_plane = (handle == GizmoHandle::PlaneXY || handle == GizmoHandle::PlaneXZ ||
                           handle == GizmoHandle::PlaneYZ);
    const bool is_center = (handle == GizmoHandle::Center);
    const bool is_axis = !is_plane && !is_center;
    if ((is_plane && m_gizmo_mode != GizmoMode::Translate) ||
        (is_center && m_gizmo_mode == GizmoMode::Rotate)) {
        out_err = "Handle does not belong to the active gizmo mode";
        m_drag.cancel();
        return false;
    }
    // Geometry source: the primary when it carries a Transform, else the
    // first selected entity that does. The drag itself still arms on the
    // whole selection (same group rule as a pick press).
    ecs::Entity origin_e = m_selection.primary();
    const scene::Transform* ot = nullptr;
    if (origin_e.valid() && w->is_alive(origin_e)) {
        ot = w->get<scene::Transform>(origin_e);
    }
    if (ot == nullptr) {
        for (ecs::Entity e : m_selection.all()) {
            if (!e.valid() || !w->is_alive(e)) {
                continue;
            }
            ot = w->get<scene::Transform>(e);
            if (ot != nullptr) {
                origin_e = e;
                break;
            }
        }
    }
    if (ot == nullptr) {
        out_err = "Selected entities have no Transform";
        return false;
    }
    if (!m_drag.begin(*w, m_selection.all(), m_gizmo_mode, m_gizmo_space, m_gizmo_snap,
                      out_err)) {
        return false;
    }
    float axes[3][3]{};
    gizmo_frame_axes(*ot, m_gizmo_space, axes);
    m_drag_origin[0] = ot->world_x;
    m_drag_origin[1] = ot->world_y;
    m_drag_origin[2] = ot->world_z;
    const Ray ray = pick_ray(vc, ndc_x, ndc_y);
    int axis = -1;
    (void)gizmo_handle_axis(handle, axis);
    if (m_gizmo_mode == GizmoMode::Translate) {
        if (is_axis && axis >= 0) {
            m_drag_axis[0] = axes[axis][0];
            m_drag_axis[1] = axes[axis][1];
            m_drag_axis[2] = axes[axis][2];
            // Edge-on axis (ray parallel to the line): press still arms, the
            // drag events below then contribute ~nothing instead of failing
            // a grab the view already accepted.
            if (!gizmo_axis_param(ray, m_drag_origin[0], m_drag_origin[1], m_drag_origin[2],
                                  m_drag_axis[0], m_drag_axis[1], m_drag_axis[2],
                                  m_drag_tlast)) {
                m_drag_tlast = 0.0f;
            }
        } else {
            // Plane quad, or the center box (camera-plane move): the plane
            // through the origin facing the interaction.
            if (is_center) {
                float fx = vc.tx - vc.px, fy = vc.ty - vc.py, fz = vc.tz - vc.pz;
                const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (fl > 1e-9f) {
                    fx /= fl;
                    fy /= fl;
                    fz /= fl;
                } else {
                    fx = 0.0f;
                    fy = 0.0f;
                    fz = 1.0f;
                }
                m_drag_plane_n[0] = fx;
                m_drag_plane_n[1] = fy;
                m_drag_plane_n[2] = fz;
            } else {
                int aa = 0, bb = 1, nn = 2;
                gizmo_handle_plane(handle, aa, bb, nn);
                m_drag_plane_n[0] = axes[nn][0];
                m_drag_plane_n[1] = axes[nn][1];
                m_drag_plane_n[2] = axes[nn][2];
            }
            if (!gizmo_plane_point(ray, m_drag_origin[0], m_drag_origin[1], m_drag_origin[2],
                                   m_drag_plane_n[0], m_drag_plane_n[1], m_drag_plane_n[2],
                                   m_drag_plast)) {
                m_drag.cancel();
                out_err = "Pointer ray misses the drag plane";
                return false;
            }
        }
    } else if (m_gizmo_mode == GizmoMode::Rotate) {
        if (!is_axis || axis < 0) {
            m_drag.cancel();
            out_err = "Handle does not belong to the active gizmo mode";
            return false;
        }
        // Geometry normal: the drawn (world-frame) axis. Apply-frame axis:
        // the unit axis itself — pre-multiplied for World, post-multiplied
        // for Local, which is exactly the drawn axis in each frame.
        m_drag_plane_n[0] = axes[axis][0];
        m_drag_plane_n[1] = axes[axis][1];
        m_drag_plane_n[2] = axes[axis][2];
        m_drag_axis[0] = (axis == 0) ? 1.0f : 0.0f;
        m_drag_axis[1] = (axis == 1) ? 1.0f : 0.0f;
        m_drag_axis[2] = (axis == 2) ? 1.0f : 0.0f;
        if (!gizmo_plane_point(ray, m_drag_origin[0], m_drag_origin[1], m_drag_origin[2],
                               m_drag_plane_n[0], m_drag_plane_n[1], m_drag_plane_n[2],
                               m_drag_plast)) {
            m_drag.cancel();
            out_err = "Pointer ray misses the rotation plane";
            return false;
        }
    } else { // Scale
        // Screen-space snapshot in pointer-NDC units: direction + length of
        // the grabbed arm, or origin + radius for the uniform box. Any arm
        // length works (only the ratio is used), so a fixed world length is
        // fine even if the view draws a slightly different one.
        const float dist = drag_distance_to(vc, *ot);
        const float snap_len = dist > 1e-4f ? dist * 0.2f : 0.2f;
        const Vec3 ondc =
            gizmo_to_pointer_ndc(vc, Vec3{m_drag_origin[0], m_drag_origin[1], m_drag_origin[2]});
        m_drag_sox = ondc.x;
        m_drag_soy = ondc.y;
        if (is_center) {
            const float v_px =
                static_cast<float>(m_viewport.height > 0 ? m_viewport.height : 9);
            // Radius truthfully matches the drawn gizmo: axis px over
            // viewport height, converted to NDC (full height = 2).
            const float v_h = v_px > 0.0f ? v_px : 1.0f;
            m_drag_srad = (kTransformGizmoAxisPx / v_h) * 2.0f;
            if (m_drag_srad <= 1e-6f) {
                m_drag_srad = 0.1f;
            }
        } else if (is_axis && axis >= 0) {
            const Vec3 tndc = gizmo_to_pointer_ndc(
                vc, Vec3{m_drag_origin[0] + axes[axis][0] * snap_len,
                         m_drag_origin[1] + axes[axis][1] * snap_len,
                         m_drag_origin[2] + axes[axis][2] * snap_len});
            float dx = tndc.x - ondc.x;
            float dy = tndc.y - ondc.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len <= 1e-6f) {
                m_drag.cancel();
                out_err = "Scale axis faces the camera";
                return false;
            }
            m_drag_sdx = dx / len;
            m_drag_sdy = dy / len;
            m_drag_slen = len;
        } else {
            m_drag.cancel();
            out_err = "Handle does not belong to the active gizmo mode";
            return false;
        }
        m_drag_flast = 1.0f;
    }
    m_drag_handle = handle;
    m_drag_vc = vc;
    m_drag_ndc0x = ndc_x;
    m_drag_ndc0y = ndc_y;
    m_drag_lastx = ndc_x;
    m_drag_lasty = ndc_y;
    m_drag_distance = drag_distance_to(vc, *ot);
    m_drag_moved = false;
    return true;
}

bool EditorApp::viewport_drag(float ndc_x, float ndc_y, const ViewCamera& vc, std::string& out_err) {
    (void)vc; // mapping uses the press-time camera: the view must not shift under the gesture
    if (!m_drag.active()) {
        return true;
    }
    ecs::World* w = world();
    if (!require_editable(out_err) || w == nullptr) {
        m_drag.cancel();
        if (w == nullptr) {
            out_err = "No scene open";
        }
        return false;
    }
    if (!m_drag_moved) {
        const float dx = ndc_x - m_drag_ndc0x;
        const float dy = ndc_y - m_drag_ndc0y;
        if (dx * dx + dy * dy < kDragDeadzoneNdc * kDragDeadzoneNdc) {
            return true;
        }
        m_drag_moved = true;
    }
    if (m_drag_handle != GizmoHandle::None) {
        // Handle drag: the mapping is frozen at press (axis line, drag plane,
        // screen snapshot), so the object moves under the gesture without the
        // mapping chasing it. A missed geometric query keeps the last state
        // instead of failing the gesture (parallel ray/plane flicker).
        const Ray ray = pick_ray(m_drag_vc, ndc_x, ndc_y);
        GizmoDelta d;
        bool feed = false;
        if (m_gizmo_mode == GizmoMode::Translate) {
            int axis = -1;
            (void)gizmo_handle_axis(m_drag_handle, axis);
            if (axis >= 0) {
                float t = m_drag_tlast;
                if (gizmo_axis_param(ray, m_drag_origin[0], m_drag_origin[1],
                                     m_drag_origin[2], m_drag_axis[0], m_drag_axis[1],
                                     m_drag_axis[2], t)) {
                    const float dt = t - m_drag_tlast;
                    d.dx = m_drag_axis[0] * dt;
                    d.dy = m_drag_axis[1] * dt;
                    d.dz = m_drag_axis[2] * dt;
                    m_drag_tlast = t;
                    feed = true;
                }
            } else {
                float p[3]{m_drag_plast[0], m_drag_plast[1], m_drag_plast[2]};
                if (gizmo_plane_point(ray, m_drag_origin[0], m_drag_origin[1],
                                      m_drag_origin[2], m_drag_plane_n[0], m_drag_plane_n[1],
                                      m_drag_plane_n[2], p)) {
                    d.dx = p[0] - m_drag_plast[0];
                    d.dy = p[1] - m_drag_plast[1];
                    d.dz = p[2] - m_drag_plast[2];
                    m_drag_plast[0] = p[0];
                    m_drag_plast[1] = p[1];
                    m_drag_plast[2] = p[2];
                    feed = true;
                }
            }
        } else if (m_gizmo_mode == GizmoMode::Rotate) {
            float p[3]{m_drag_plast[0], m_drag_plast[1], m_drag_plast[2]};
            if (gizmo_plane_point(ray, m_drag_origin[0], m_drag_origin[1], m_drag_origin[2],
                                  m_drag_plane_n[0], m_drag_plane_n[1], m_drag_plane_n[2], p)) {
                const float ang = gizmo_ring_angle(
                    m_drag_plast[0] - m_drag_origin[0], m_drag_plast[1] - m_drag_origin[1],
                    m_drag_plast[2] - m_drag_origin[2], p[0] - m_drag_origin[0],
                    p[1] - m_drag_origin[1], p[2] - m_drag_origin[2], m_drag_plane_n[0],
                    m_drag_plane_n[1], m_drag_plane_n[2]);
                d.axis_x = m_drag_axis[0];
                d.axis_y = m_drag_axis[1];
                d.axis_z = m_drag_axis[2];
                d.axis_angle_deg = ang * 180.0f / kPi;
                m_drag_plast[0] = p[0];
                m_drag_plast[1] = p[1];
                m_drag_plast[2] = p[2];
                feed = true;
            }
        } else { // Scale: screen-space factors against the press snapshot.
            float f = 1.0f;
            if (m_drag_handle == GizmoHandle::Center) {
                f = gizmo_uniform_factor(m_drag_sox, m_drag_soy, m_drag_ndc0x, m_drag_ndc0y,
                                         ndc_x, ndc_y, m_drag_srad);
            } else {
                f = gizmo_axis_factor(m_drag_ndc0x, m_drag_ndc0y, ndc_x, ndc_y, m_drag_sdx,
                                      m_drag_sdy, m_drag_slen);
            }
            const float inc = f - m_drag_flast;
            if (m_drag_handle == GizmoHandle::Center) {
                d.dscale = inc;
            } else {
                int axis = -1;
                (void)gizmo_handle_axis(m_drag_handle, axis);
                if (axis == 0) {
                    d.scl_x = inc;
                } else if (axis == 1) {
                    d.scl_y = inc;
                } else if (axis == 2) {
                    d.scl_z = inc;
                }
            }
            m_drag_flast = f;
            feed = true;
        }
        if (feed) {
            m_drag.accumulate(d);
        }
        m_drag_lastx = ndc_x;
        m_drag_lasty = ndc_y;
        if (!m_drag.live_apply(*w)) {
            m_drag.cancel();
            m_drag_handle = GizmoHandle::None;
            out_err = "Drag target lost";
            return false;
        }
        after_mutation(m_drag.entity());
        return true;
    }
    const GizmoDelta d =
        gizmo_delta_for_drag(m_gizmo_mode, m_drag_vc, m_drag_distance, m_drag_lastx, m_drag_lasty,
                             ndc_x, ndc_y);
    m_drag.accumulate(d);
    m_drag_lastx = ndc_x;
    m_drag_lasty = ndc_y;
    if (!m_drag.live_apply(*w)) {
        m_drag.cancel();
        out_err = "Drag target lost";
        return false;
    }
    after_mutation(m_drag.entity());
    return true;
}

bool EditorApp::viewport_release(std::string& out_err) {
    if (!m_drag.active()) {
        m_drag_handle = GizmoHandle::None;
        return true;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        m_drag.cancel();
        m_drag_handle = GizmoHandle::None;
        out_err = "No scene open";
        return false;
    }
    // A click without movement folds into nothing (commit returns nullptr),
    // so selection clicks never pollute the undo stack.
    if (std::unique_ptr<ICommand> cmd = m_drag.commit(*w)) {
        const ecs::Entity target = cmd->target();
        m_stack.push(std::move(cmd), *w);
        after_mutation(target);
    }
    m_drag_handle = GizmoHandle::None;
    (void)out_err;
    return true;
}

bool EditorApp::viewport_abort_drag(std::string& out_err) {
    m_drag_handle = GizmoHandle::None;
    if (!m_drag.active()) {
        return true;
    }
    ecs::World* w = world();
    const ecs::Entity e = m_drag.entity();
    if (w != nullptr && e.valid() && w->is_alive(e)) {
        m_drag.abort(*w);
        after_mutation(e);
    } else {
        m_drag.cancel();
    }
    (void)out_err;
    return true;
}

void EditorApp::tick(float dt) {
    if (dt > 0.0f) {
        m_dt_accum += static_cast<double>(dt);
        ++m_dt_count;
        if (m_dt_accum >= 0.25) {
            m_fps = static_cast<double>(m_dt_count) / m_dt_accum;
            m_frame_ms = (m_fps > 0.0) ? (1000.0 / m_fps) : 0.0;
            m_dt_accum = 0.0;
            m_dt_count = 0;
        }
        m_browser_cache_age += static_cast<double>(dt);
    }
}

EditorStatus EditorApp::status() const {
    EditorStatus s;
    if (m_runtime != nullptr && m_runtime->scene() != nullptr) {
        s.scene_label = m_runtime->scene()->name();
    }
    if (!m_scene_path.empty()) {
        s.scene_label += " (" + m_scene_path + ")";
    }
    if (m_dirty) {
        s.scene_label += " *";
    }
    s.dirty = m_dirty;
    s.playing = m_play.playing();
    if (const ecs::World* w = world()) {
        s.entity_count = w->alive_entity_count();
    }
    s.selected_count = m_selection.all().size();
    s.fps = m_fps;
    s.frame_ms = m_frame_ms;
    return s;
}

std::vector<OutlinerRow> EditorApp::outliner_rows() const {
    if (const ecs::World* w = world()) {
        // Rebuild only when something changed: after_mutation() is the normal
        // invalidation path; the world pointer / alive-count guards also catch
        // scene swaps and play-mode churn that bypass commands.
        const bool world_swapped = (w != m_outliner_cache_world);
        const bool churned = (w->alive_entity_count() != m_outliner_cache_count);
        if (!m_outliner_cache_valid || world_swapped || churned) {
            m_outliner_cache = build_outliner_rows(*w, m_outliner);
            m_outliner_cache_valid = true;
            m_outliner_cache_world = w;
            m_outliner_cache_count = w->alive_entity_count();
        }
        // The search box filters every frame over the cached rows: typing must
        // not wait for a structural mutation to take effect.
        return filter_outliner_rows(m_outliner_cache, m_outliner.filter_text);
    }
    return {};
}

std::vector<AssetEntry> EditorApp::browser_entries() {
    // Unity-style roots: 0 lists the engine Content, 1 the open project's
    // files. The project root with no project mounted is an empty listing
    // (list_project_assets), never a silent fallback to engine content.
    if (m_browser_cache_dirty || m_browser_cache_age > 0.5) {
        const bool project_root = (m_browser.browser_root == 1);
        auto all = project_root ? list_project_assets(m_vfs)
                                : list_content_assets(m_vfs, m_registry);
        if (project_root) {
            // The project scan lists files, not registry records, so mesh entries
            // arrive without an AssetId — and a drop without an ID is refused.
            // Resolve project://Content/... against the registry's content://...
            // records so drag-and-drop works in projects exactly as it does in the
            // engine tree.
            static const std::string kPrefix = "project://Content/";
            for (auto& e : all) {
                if (e.type != assets::AssetType::Mesh || e.has_id) {
                    continue;
                }
                if (e.logical_path.rfind(kPrefix, 0) != 0) {
                    continue;
                }
                const std::string content_path = "content://" + e.logical_path.substr(kPrefix.size());
                if (const assets::AssetMetadata* meta = m_registry.find_by_path(content_path)) {
                    if (meta->id.valid()) {
                        e.id = meta->id;
                        e.has_id = true;
                        e.cooked_path = meta->cooked_path;
                        if (!meta->cooked_path.empty()) {
                            auto ex = m_vfs.exists(meta->cooked_path);
                            e.has_cooked = ex.ok && ex.value;
                        }
                    }
                }
            }
        }
        m_browser_cache = std::move(all);
        m_browser_cache_age = 0.0;
        m_browser_cache_dirty = false;
    }
    return filter_assets(m_browser_cache, m_browser.filter_text, m_browser.filter_type);
}

} // namespace nf::editor
