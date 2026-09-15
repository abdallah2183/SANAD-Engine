#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Outliner.hpp>
#include <NF/Editor/Prefabs.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>

#include <cmath>

namespace nf::editor {

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
    if (m_runtime != nullptr) {
        m_runtime->mark_scene_edited();
    }
    m_dirty = true;
    if (const ecs::World* w = world()) {
        m_selection.prune(*w);
    }
    if (touched.valid()) {
        if (const ecs::World* w = world()) {
            if (w->is_alive(touched)) {
                m_selection.set_single(touched);
            }
        }
    }
    m_outliner.pending_delete = ecs::kInvalidEntity;
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
    // Build an empty scene through the loader path: save an empty scene to a
    // temp logical path, then load it so Runtime owns it like any other scene.
    scene::Scene empty("Untitled");
    std::string tmp_err;
    if (!runtime::save_scene_to_vfs(m_vfs, "cache://EditorUntitled.nfscene", empty, tmp_err)) {
        out_err = "Failed to stage empty scene: " + tmp_err;
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

void EditorApp::request_delete(ecs::Entity e) {
    m_outliner.pending_delete = e;
}

void EditorApp::cancel_delete() {
    m_outliner.pending_delete = ecs::kInvalidEntity;
}

bool EditorApp::confirm_delete(std::string& out_err) {
    ecs::Entity e = m_outliner.pending_delete;
    m_outliner.pending_delete = ecs::kInvalidEntity;
    if (!e.valid()) {
        out_err = "Nothing pending deletion";
        return false;
    }
    return delete_entity(e, out_err);
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
        saves->set_autosave(interval_seconds, slot_prefix);
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
    return done_now;
}

void EditorApp::watch_imported(const ImportJob& job) {
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
    m_outliner.pending_delete = ecs::kInvalidEntity;
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
    m_outliner.pending_delete = ecs::kInvalidEntity;
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
    return true;
}

bool EditorApp::stop(std::string& out_err) {
    // Integrity: the edit scene must still match the pre-play snapshot in
    // structure (edits are locked during play, so this always holds; the
    // check turns a silent-loss bug into a loud error).
    if (m_play.playing() && m_play.snapshot() != nullptr && m_runtime != nullptr &&
        m_runtime->edit_scene() != nullptr) {
        std::string diff;
        if (!scenes_equal_structure(*m_play.snapshot(), *m_runtime->edit_scene(), diff)) {
            // Locked UI prevents this; report but still stop cleanly.
            m_console.push(LogMessage{LogLevel::Warn, LogCategory::Editor,
                                      "Play session ended with edit-world drift: " + diff,
                                      std::chrono::system_clock::now(), __FILE__, __LINE__});
        }
    }
    return m_play.stop(out_err);
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

    auto bounds_of = [this, w](ecs::Entity e) -> std::optional<AABB> {
        const auto* mc = w->get<runtime::MeshComponent>(e);
        const auto* tr = w->get<scene::Transform>(e);
        if (mc == nullptr || tr == nullptr || !mc->mesh_id.valid()) {
            return std::nullopt;
        }
        auto handle = m_manager.find(mc->mesh_id);
        if (!handle || !handle->asset) {
            return std::nullopt;
        }
        const auto& b = handle->asset->bounds;
        AABB box;
        box.min_x = b.min_x + tr->world_x;
        box.min_y = b.min_y + tr->world_y;
        box.min_z = b.min_z + tr->world_z;
        box.max_x = b.max_x + tr->world_x;
        box.max_y = b.max_y + tr->world_y;
        box.max_z = b.max_z + tr->world_z;
        return box;
    };
    return pick_entity(*w, bounds_of, cam, ndc_x, ndc_y);
}

namespace {

constexpr float kDragDeadzoneNdc = 0.004f; // clicks must not become micro-drags

float drag_distance_to(const ViewCamera& vc, const scene::Transform& t) {
    const float dx = t.world_x - vc.px;
    const float dy = t.world_y - vc.py;
    const float dz = t.world_z - vc.pz;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    return len > 1e-4f ? len : 1e-4f;
}

} // namespace

bool EditorApp::viewport_press(float ndc_x, float ndc_y, const ViewCamera& vc, std::string& out_err) {
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
    m_selection.set_single(hit);
    const auto* t = w->get<scene::Transform>(hit);
    if (t == nullptr) {
        m_drag.cancel();
        out_err = "Selected entity has no Transform";
        return false;
    }
    if (!m_drag.begin(*w, hit, m_gizmo_mode, m_gizmo_space, out_err)) {
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
        return true;
    }
    ecs::World* w = world();
    if (w == nullptr) {
        m_drag.cancel();
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
    (void)out_err;
    return true;
}

bool EditorApp::viewport_abort_drag(std::string& out_err) {
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
        return build_outliner_rows(*w, m_outliner);
    }
    return {};
}

std::vector<AssetEntry> EditorApp::browser_entries() {
    auto all = list_content_assets(m_vfs, m_registry);
    return filter_assets(all, m_browser.filter_text, m_browser.filter_type);
}

} // namespace nf::editor
