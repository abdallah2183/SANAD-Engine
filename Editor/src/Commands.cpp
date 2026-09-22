#include <NF/Editor/Commands.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Editor/Prefabs.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Scene/Transform.hpp>

namespace nf::editor {

void CommandStack::push(std::unique_ptr<ICommand> cmd, ecs::World& world) {
    if (!cmd) {
        return;
    }
    cmd->apply(world);
    m_undo.push_back(std::move(cmd));
    m_redo.clear();
}

bool CommandStack::undo(ecs::World& world) {
    if (m_undo.empty()) {
        return false;
    }
    m_undo.back()->undo(world);
    m_redo.push_back(std::move(m_undo.back()));
    m_undo.pop_back();
    return true;
}

bool CommandStack::redo(ecs::World& world) {
    if (m_redo.empty()) {
        return false;
    }
    m_redo.back()->apply(world);
    m_undo.push_back(std::move(m_redo.back()));
    m_redo.pop_back();
    return true;
}

void CommandStack::clear() {
    m_undo.clear();
    m_redo.clear();
}

ecs::Entity CommandStack::last_target() const {
    if (m_undo.empty()) {
        return ecs::kInvalidEntity;
    }
    return m_undo.back()->target();
}

std::string CommandStack::undo_label() const {
    return m_undo.empty() ? std::string{} : m_undo.back()->label();
}

std::string CommandStack::redo_label() const {
    return m_redo.empty() ? std::string{} : m_redo.back()->label();
}

// --- CreateEntityCommand ----------------------------------------------------

CreateEntityCommand::CreateEntityCommand(std::string name, ecs::Entity parent)
    : m_name(std::move(name)), m_parent(parent) {}

void CreateEntityCommand::apply(ecs::World& world) {
    ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    if (!m_name.empty()) {
        world.add<scene::NameComponent>(e, scene::NameComponent{m_name});
    }
    if (m_parent.valid() && world.is_alive(m_parent)) {
        scene::set_parent(world, e, m_parent);
    }
    m_created = e;
}

void CreateEntityCommand::undo(ecs::World& world) {
    if (m_created.valid() && world.is_alive(m_created)) {
        world.destroy_entity(m_created);
    }
    m_created = ecs::kInvalidEntity;
}

std::string CreateEntityCommand::label() const {
    return "Create '" + m_name + "'";
}

// --- DeleteEntityCommand ----------------------------------------------------

std::unique_ptr<DeleteEntityCommand> DeleteEntityCommand::capture(ecs::World& world, ecs::Entity e,
                                                                 std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    auto cmd = std::unique_ptr<DeleteEntityCommand>(new DeleteEntityCommand());
    cmd->m_original = e;
    cmd->m_restored = e;
    if (const auto* t = world.get<scene::Transform>(e)) {
        cmd->m_had_transform = true;
        cmd->m_transform = *t;
        cmd->m_parent = t->parent;
    }
    if (const auto* n = world.get<scene::NameComponent>(e)) {
        cmd->m_had_name = true;
        cmd->m_name = *n;
    }
    if (const auto* m = world.get<runtime::MeshComponent>(e)) {
        cmd->m_had_mesh = true;
        cmd->m_mesh = *m;
    }
    if (const auto* c = world.get<runtime::CameraComponent>(e)) {
        cmd->m_had_camera = true;
        cmd->m_camera = *c;
    }
    if (const auto* l = world.get<runtime::DirectionalLight>(e)) {
        cmd->m_had_light = true;
        cmd->m_light = *l;
    }
    if (const auto* p = world.get<scene::PrefabLinkComponent>(e)) {
        cmd->m_had_prefab = true;
        cmd->m_prefab = *p;
    }
    cmd->m_children = scene::get_children(world, e);
    return cmd;
}

void DeleteEntityCommand::apply(ecs::World& world) {
    // m_restored always names the live handle (original first, then the
    // undo-restored entity), so apply/redo destroys whatever is current.
    ecs::Entity victim = m_restored;
    if (!victim.valid() || !world.is_alive(victim)) {
        return;
    }
    // Re-attach children to our own parent so the subtree survives.
    ecs::Entity new_parent = ecs::kInvalidEntity;
    if (const auto* t = world.get<scene::Transform>(victim)) {
        new_parent = t->parent;
    } else {
        new_parent = m_parent;
    }
    m_children = scene::get_children(world, victim);
    for (ecs::Entity child : m_children) {
        if (world.is_alive(child)) {
            scene::set_parent(world, child, new_parent);
        }
    }
    world.destroy_entity(victim);
    m_restored = ecs::kInvalidEntity;
}

void DeleteEntityCommand::undo(ecs::World& world) {
    ecs::Entity e = world.create_entity();
    if (m_had_transform) {
        scene::Transform t = m_transform;
        t.parent = ecs::kInvalidEntity; // set below via set_parent (cycle-safe)
        t.dirty = true;
        world.add<scene::Transform>(e, t);
    }
    if (m_had_name) {
        world.add<scene::NameComponent>(e, m_name);
    }
    if (m_had_mesh) {
        world.add<runtime::MeshComponent>(e, m_mesh);
    }
    if (m_had_camera) {
        world.add<runtime::CameraComponent>(e, m_camera);
    }
    if (m_had_light) {
        world.add<runtime::DirectionalLight>(e, m_light);
    }
    if (m_had_prefab) {
        world.add<scene::PrefabLinkComponent>(e, m_prefab);
    }
    if (m_parent.valid() && world.is_alive(m_parent)) {
        scene::set_parent(world, e, m_parent);
    }
    for (ecs::Entity child : m_children) {
        if (world.is_alive(child)) {
            scene::set_parent(world, child, e);
        }
    }
    scene::propagate_transforms(world);
    m_restored = e;
}

std::string DeleteEntityCommand::label() const {
    if (m_had_name && !m_name.name.empty()) {
        return "Delete '" + m_name.name + "'";
    }
    return "Delete entity";
}

// --- RenameCommand ----------------------------------------------------------

RenameCommand::RenameCommand(ecs::Entity e, std::string new_name)
    : m_entity(e), m_new_name(std::move(new_name)) {}

bool RenameCommand::capture_old(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return false;
    }
    if (const auto* n = world.get<scene::NameComponent>(m_entity)) {
        m_had_old = true;
        m_old_name = n->name;
    } else {
        m_had_old = false;
        m_old_name.clear();
    }
    m_old_captured = true;
    return true;
}

void RenameCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (!m_old_captured) {
        capture_old(world);
    }
    if (m_new_name.empty()) {
        world.remove<scene::NameComponent>(m_entity);
    } else if (world.has<scene::NameComponent>(m_entity)) {
        world.get<scene::NameComponent>(m_entity)->name = m_new_name;
    } else {
        world.add<scene::NameComponent>(m_entity, scene::NameComponent{m_new_name});
    }
}

void RenameCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (!m_had_old || m_old_name.empty()) {
        world.remove<scene::NameComponent>(m_entity);
    } else if (world.has<scene::NameComponent>(m_entity)) {
        world.get<scene::NameComponent>(m_entity)->name = m_old_name;
    } else {
        world.add<scene::NameComponent>(m_entity, scene::NameComponent{m_old_name});
    }
}

std::string RenameCommand::label() const {
    return "Rename to '" + m_new_name + "'";
}

// --- SetTransformCommand ----------------------------------------------------

SetTransformCommand::SetTransformCommand(ecs::Entity e, const scene::Transform& before,
                                         const scene::Transform& after)
    : m_entity(e), m_before(before), m_after(after) {}

void SetTransformCommand::write_pos_rot_scale(ecs::World& world, ecs::Entity e,
                                              const scene::Transform& v) {
    auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        return;
    }
    t->local_x = v.local_x;
    t->local_y = v.local_y;
    t->local_z = v.local_z;
    t->rot_x = v.rot_x;
    t->rot_y = v.rot_y;
    t->rot_z = v.rot_z;
    t->scale_x = v.scale_x;
    t->scale_y = v.scale_y;
    t->scale_z = v.scale_z;
    t->dirty = true;
}

void SetTransformCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    write_pos_rot_scale(world, m_entity, m_after);
}

void SetTransformCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    write_pos_rot_scale(world, m_entity, m_before);
}

std::string SetTransformCommand::label() const {
    return "Edit transform";
}

// --- SetTransformsCommand ---------------------------------------------------

SetTransformsCommand::SetTransformsCommand(std::vector<Entry> entries, std::string verb)
    : m_entries(std::move(entries)), m_label(std::move(verb)) {
    // "Move 3 entities" for a group, "Move entity" for one — the undo menu
    // reads this, so a lone selection must not read like a batch.
    if (m_entries.size() == 1) {
        m_label += " entity";
    } else {
        m_label += " " + std::to_string(m_entries.size()) + " entities";
    }
}

void SetTransformsCommand::apply(ecs::World& world) {
    for (const Entry& en : m_entries) {
        if (!en.entity.valid() || !world.is_alive(en.entity)) {
            continue;
        }
        SetTransformCommand::write_pos_rot_scale(world, en.entity, en.after);
    }
}

void SetTransformsCommand::undo(ecs::World& world) {
    for (const Entry& en : m_entries) {
        if (!en.entity.valid() || !world.is_alive(en.entity)) {
            continue;
        }
        SetTransformCommand::write_pos_rot_scale(world, en.entity, en.before);
    }
}

// --- ReparentCommand --------------------------------------------------------

ReparentCommand::ReparentCommand(ecs::Entity e, ecs::Entity old_parent, ecs::Entity new_parent)
    : m_entity(e), m_old_parent(old_parent), m_new_parent(new_parent) {}

void ReparentCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    scene::set_parent(world, m_entity, m_new_parent);
}

void ReparentCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    scene::set_parent(world, m_entity, m_old_parent);
}

std::string ReparentCommand::label() const {
    return "Reparent entity";
}

// --- SetMeshCommand ---------------------------------------------------------

SetMeshCommand::SetMeshCommand(ecs::Entity e, bool had_before, const runtime::MeshComponent& before,
                               const runtime::MeshComponent& after)
    : m_entity(e), m_had_before(had_before), m_before(before), m_after(after) {}

void SetMeshCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    world.add<runtime::MeshComponent>(m_entity, m_after);
}

void SetMeshCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (m_had_before) {
        world.add<runtime::MeshComponent>(m_entity, m_before);
    } else {
        world.remove<runtime::MeshComponent>(m_entity);
    }
}

std::string SetMeshCommand::label() const {
    return "Assign mesh";
}

// --- CreateMeshEntityCommand ------------------------------------------------

CreateMeshEntityCommand::CreateMeshEntityCommand(std::string name, ecs::Entity parent,
                                                 const runtime::MeshComponent& mesh)
    : m_name(std::move(name)), m_parent(parent), m_mesh(mesh) {}

CreateMeshEntityCommand::CreateMeshEntityCommand(std::string name, ecs::Entity parent,
                                                 const runtime::MeshComponent& mesh,
                                                 const scene::Transform& seed)
    : m_name(std::move(name)), m_parent(parent), m_mesh(mesh), m_has_seed(true), m_seed(seed) {}

void CreateMeshEntityCommand::apply(ecs::World& world) {
    ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, m_has_seed ? m_seed : scene::Transform{});
    if (!m_name.empty()) {
        world.add<scene::NameComponent>(e, scene::NameComponent{m_name});
    }
    world.add<runtime::MeshComponent>(e, m_mesh);
    if (m_parent.valid() && world.is_alive(m_parent)) {
        scene::set_parent(world, e, m_parent);
    }
    // The seed is the entity's world placement; a reparent must not silently
    // keep the child's old world position (PropagateFlags), so mark it dirty the
    // same way an inspector edit does.
    if (auto* t = world.get<scene::Transform>(e)) {
        t->dirty = true;
    }
    m_created = e;
}

void CreateMeshEntityCommand::undo(ecs::World& world) {
    if (m_created.valid() && world.is_alive(m_created)) {
        world.destroy_entity(m_created);
    }
    m_created = ecs::kInvalidEntity;
}

std::string CreateMeshEntityCommand::label() const {
    return "Add mesh '" + m_name + "'";
}

// --- SetCameraCommand -------------------------------------------------------

SetCameraCommand::SetCameraCommand(ecs::Entity e, bool had_before,
                                   const runtime::CameraComponent& before,
                                   const runtime::CameraComponent& after)
    : m_entity(e), m_had_before(had_before), m_before(before), m_after(after) {}

void SetCameraCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    world.add<runtime::CameraComponent>(m_entity, m_after);
}

void SetCameraCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (m_had_before) {
        world.add<runtime::CameraComponent>(m_entity, m_before);
    } else {
        world.remove<runtime::CameraComponent>(m_entity);
    }
}

std::string SetCameraCommand::label() const {
    return "Edit camera";
}

// --- SetLightCommand --------------------------------------------------------

SetLightCommand::SetLightCommand(ecs::Entity e, bool had_before,
                                 const runtime::DirectionalLight& before,
                                 const runtime::DirectionalLight& after)
    : m_entity(e), m_had_before(had_before), m_before(before), m_after(after) {}

void SetLightCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    world.add<runtime::DirectionalLight>(m_entity, m_after);
}

void SetLightCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (m_had_before) {
        world.add<runtime::DirectionalLight>(m_entity, m_before);
    } else {
        world.remove<runtime::DirectionalLight>(m_entity);
    }
}

std::string SetLightCommand::label() const {
    return "Edit light";
}

// --- SetSkyCommand ----------------------------------------------------------

SetSkyCommand::SetSkyCommand(ecs::Entity e, bool had_before,
                             const runtime::SkyComponent& before,
                             const runtime::SkyComponent& after)
    : m_entity(e), m_had_before(had_before), m_before(before), m_after(after) {}

void SetSkyCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    world.add<runtime::SkyComponent>(m_entity, m_after);
}

void SetSkyCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (m_had_before) {
        world.add<runtime::SkyComponent>(m_entity, m_before);
    } else {
        world.remove<runtime::SkyComponent>(m_entity);
    }
}

std::string SetSkyCommand::label() const {
    return "Edit sky";
}

// --- SetMaterialAssignmentCommand -------------------------------------------

SetMaterialAssignmentCommand::SetMaterialAssignmentCommand(ecs::Entity e, std::string before_path,
                                                           std::string after_path)
    : m_entity(e), m_before(std::move(before_path)), m_after(std::move(after_path)) {}

void SetMaterialAssignmentCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (auto* mc = world.get<runtime::MeshComponent>(m_entity)) {
        mc->material = m_after;
    }
}

void SetMaterialAssignmentCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (auto* mc = world.get<runtime::MeshComponent>(m_entity)) {
        mc->material = m_before;
    }
}

std::string SetMaterialAssignmentCommand::label() const {
    return "Assign material '" + m_after + "'";
}

// --- SetMaterialParamsCommand -----------------------------------------------

SetMaterialParamsCommand::SetMaterialParamsCommand(runtime::Runtime* runtime, std::string path,
                                                   rendering::PBRMaterialParams before,
                                                   rendering::PBRMaterialParams after)
    : m_runtime(runtime), m_path(std::move(path)), m_before(before), m_after(after) {}

bool SetMaterialParamsCommand::write_through(const rendering::PBRMaterialParams& params) {
    if (m_runtime == nullptr) {
        return false;
    }
    std::string err;
    if (!m_runtime->set_material_params(m_path, params, err)) {
        NF_LOG_WARN(LogCategory::Editor, "SetMaterialParamsCommand: {}", err);
        return false;
    }
    return true;
}

void SetMaterialParamsCommand::apply(ecs::World& world) {
    (void)world;
    write_through(m_after);
}

void SetMaterialParamsCommand::undo(ecs::World& world) {
    (void)world;
    write_through(m_before);
}

std::string SetMaterialParamsCommand::label() const {
    return "Edit material '" + m_path + "'";
}

// --- SetMaterialAlbedoCommand -------------------------------------------------

SetMaterialAlbedoCommand::SetMaterialAlbedoCommand(runtime::Runtime* runtime, std::string material_path,
                                                   std::string before_tex, std::string after_tex)
    : m_runtime(runtime),
      m_material(std::move(material_path)),
      m_before(std::move(before_tex)),
      m_after(std::move(after_tex)) {}

void SetMaterialAlbedoCommand::apply(ecs::World& world) {
    (void)world;
    if (m_runtime == nullptr) {
        return;
    }
    std::string err;
    if (!m_runtime->set_material_albedo(m_material, m_after, err)) {
        NF_LOG_WARN(LogCategory::Editor, "SetMaterialAlbedoCommand: {}", err);
    }
}

void SetMaterialAlbedoCommand::undo(ecs::World& world) {
    (void)world;
    if (m_runtime == nullptr) {
        return;
    }
    std::string err;
    if (!m_runtime->set_material_albedo(m_material, m_before, err)) {
        NF_LOG_WARN(LogCategory::Editor, "SetMaterialAlbedoCommand: {}", err);
    }
}

std::string SetMaterialAlbedoCommand::label() const {
    return "Set material albedo '" + m_material + "'";
}

// --- SetMaterialMipModeCommand ------------------------------------------------

SetMaterialMipModeCommand::SetMaterialMipModeCommand(runtime::Runtime* runtime,
                                                     std::string material_path,
                                                     rhi::MipMapMode before_mode,
                                                     rhi::MipMapMode after_mode)
    : m_runtime(runtime),
      m_material(std::move(material_path)),
      m_before(before_mode),
      m_after(after_mode) {}

bool SetMaterialMipModeCommand::write_through(rhi::MipMapMode mode) {
    if (m_runtime == nullptr) {
        return false;
    }
    std::string err;
    if (!m_runtime->set_material_mip_mode(m_material, mode, err)) {
        NF_LOG_WARN(LogCategory::Editor, "SetMaterialMipModeCommand: {}", err);
        return false;
    }
    return true;
}

void SetMaterialMipModeCommand::apply(ecs::World& world) {
    (void)world;
    write_through(m_after);
}

void SetMaterialMipModeCommand::undo(ecs::World& world) {
    (void)world;
    write_through(m_before);
}

std::string SetMaterialMipModeCommand::label() const {
    return "Set material mip filter '" + m_material + "'";
}

// --- SetPrefabLinkCommand -----------------------------------------------------

SetPrefabLinkCommand::SetPrefabLinkCommand(ecs::Entity e, bool had_before,
                                           const scene::PrefabLinkComponent& before,
                                           const scene::PrefabLinkComponent& after)
    : m_entity(e), m_had_before(had_before), m_before(before), m_after(after) {}

void SetPrefabLinkCommand::apply(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    world.add<scene::PrefabLinkComponent>(m_entity, m_after);
}

void SetPrefabLinkCommand::undo(ecs::World& world) {
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        return;
    }
    if (m_had_before) {
        world.add<scene::PrefabLinkComponent>(m_entity, m_before);
    } else {
        world.remove<scene::PrefabLinkComponent>(m_entity);
    }
}

std::string SetPrefabLinkCommand::label() const {
    return "Tag prefab instance";
}

// --- DeleteSubtreeCommand -----------------------------------------------------

std::unique_ptr<DeleteSubtreeCommand> DeleteSubtreeCommand::capture(ecs::World& world, ecs::Entity root,
                                                                    std::string& out_err) {
    if (!root.valid() || !world.is_alive(root)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    auto cmd = std::unique_ptr<DeleteSubtreeCommand>(new DeleteSubtreeCommand());
    // Root-first order (parents before children) for undo; apply destroys in
    // reverse.
    std::vector<ecs::Entity> stack{root};
    std::vector<ecs::Entity> order;
    while (!stack.empty()) {
        ecs::Entity cur = stack.back();
        stack.pop_back();
        if (!world.is_alive(cur)) {
            continue;
        }
        order.push_back(cur);
        auto kids = scene::get_children(world, cur);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    for (ecs::Entity e : order) {
        Entry en;
        en.original = e;
        if (const auto* t = world.get<scene::Transform>(e)) {
            en.had_transform = true;
            en.transform = *t;
            en.orig_parent = t->parent;
        }
        if (const auto* n = world.get<scene::NameComponent>(e)) {
            en.had_name = true;
            en.name = *n;
        }
        if (const auto* m = world.get<runtime::MeshComponent>(e)) {
            en.had_mesh = true;
            en.mesh = *m;
        }
        if (const auto* c = world.get<runtime::CameraComponent>(e)) {
            en.had_camera = true;
            en.camera = *c;
        }
        if (const auto* l = world.get<runtime::DirectionalLight>(e)) {
            en.had_light = true;
            en.light = *l;
        }
        if (const auto* p = world.get<scene::PrefabLinkComponent>(e)) {
            en.had_prefab = true;
            en.prefab = *p;
        }
        cmd->m_entries.push_back(en);
    }
    return cmd;
}

void DeleteSubtreeCommand::apply(ecs::World& world) {
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (world.is_alive(it->original)) {
            world.destroy_entity(it->original);
        } else if (it->restored.valid() && world.is_alive(it->restored)) {
            // Redo after undo: destroy the restored copies.
            world.destroy_entity(it->restored);
        }
        it->restored = ecs::kInvalidEntity;
    }
    m_restored_root = ecs::kInvalidEntity;
}

void DeleteSubtreeCommand::undo(ecs::World& world) {
    // Recreate root-first, remapping internal parents to the fresh handles.
    std::unordered_map<uint32_t, ecs::Entity> remap;
    for (auto& en : m_entries) {
        ecs::Entity ne = world.create_entity();
        remap[en.original.id] = ne;
        en.restored = ne;
        if (en.had_transform) {
            scene::Transform t = en.transform;
            t.parent = ecs::kInvalidEntity;
            t.dirty = true;
            world.add<scene::Transform>(ne, t);
        }
        if (en.had_name) {
            world.add<scene::NameComponent>(ne, en.name);
        }
        if (en.had_mesh) {
            world.add<runtime::MeshComponent>(ne, en.mesh);
        }
        if (en.had_camera) {
            world.add<runtime::CameraComponent>(ne, en.camera);
        }
        if (en.had_light) {
            world.add<runtime::DirectionalLight>(ne, en.light);
        }
        if (en.had_prefab) {
            world.add<scene::PrefabLinkComponent>(ne, en.prefab);
        }
    }
    for (auto& en : m_entries) {
        if (!en.had_transform) {
            continue;
        }
        auto it = remap.find(en.orig_parent.id);
        if (it != remap.end()) {
            scene::set_parent(world, en.restored, it->second);
        } else if (en.orig_parent.valid() && world.is_alive(en.orig_parent)) {
            scene::set_parent(world, en.restored, en.orig_parent);
        }
    }
    scene::propagate_transforms(world);
    if (!m_entries.empty()) {
        m_restored_root = m_entries.front().restored;
    }
}

std::string DeleteSubtreeCommand::label() const {
    return "Delete subtree";
}

// --- InstantiatePrefabCommand ---------------------------------------------------

InstantiatePrefabCommand::InstantiatePrefabCommand(scene::Scene prefab_template, std::string prefab_path,
                                                   ecs::Entity parent)
    : m_template(std::move(prefab_template)), m_path(std::move(prefab_path)), m_parent(parent) {}

void InstantiatePrefabCommand::apply(ecs::World& world) {
    m_created.clear();
    ecs::Entity parent =
        (m_parent.valid() && world.is_alive(m_parent)) ? m_parent : ecs::kInvalidEntity;
    // Transplant every template root under the parent, tagging the link
    // (nested links inside the template are preserved verbatim). Track the
    // FULL subtrees (not just roots) so undo destroys every copy.
    const std::vector<ecs::Entity> roots = clone_prefab_roots(m_template, world, parent);
    for (ecs::Entity r : roots) {
        world.add<scene::PrefabLinkComponent>(r, scene::PrefabLinkComponent{m_path});
        const std::vector<ecs::Entity> members = collect_subtree(world, r);
        m_created.insert(m_created.end(), members.begin(), members.end());
    }
    scene::propagate_transforms(world);
}

void InstantiatePrefabCommand::undo(ecs::World& world) {
    for (auto it = m_created.rbegin(); it != m_created.rend(); ++it) {
        if (world.is_alive(*it)) {
            world.destroy_entity(*it);
        }
    }
    m_created.clear();
}

std::string InstantiatePrefabCommand::label() const {
    return "Instantiate prefab '" + m_path + "'";
}

// --- RevertPrefabCommand ----------------------------------------------------------

RevertPrefabCommand::RevertPrefabCommand(std::unique_ptr<DeleteSubtreeCommand> del,
                                         std::unique_ptr<InstantiatePrefabCommand> inst)
    : m_del(std::move(del)), m_inst(std::move(inst)) {}

void RevertPrefabCommand::apply(ecs::World& world) {
    if (m_del) {
        m_del->apply(world);
    }
    if (m_inst) {
        m_inst->apply(world);
    }
}

void RevertPrefabCommand::undo(ecs::World& world) {
    if (m_inst) {
        m_inst->undo(world);
    }
    if (m_del) {
        m_del->undo(world);
    }
}

std::string RevertPrefabCommand::label() const {
    return "Revert prefab instance";
}

ecs::Entity RevertPrefabCommand::target() const {
    return (m_inst != nullptr) ? m_inst->target() : ecs::kInvalidEntity;
}

} // namespace nf::editor
