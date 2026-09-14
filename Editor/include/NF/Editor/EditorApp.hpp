#pragma once

// NF/Editor/EditorApp.hpp — central editor state machine (window/RHI-free).
//
// EditorApp owns no GPU resources and opens no windows: it edits the Scene
// owned by Runtime (via Runtime::edit_scene()), tracks dirty/selection/undo,
// and exposes panel models (outliner rows, browser entries, console). The
// native shell (main.cpp) drives it once per frame and renders through
// Runtime::render_offscreen(). All structural edits go through CommandStack.

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/AssetBrowser.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/Gizmo.hpp>
#include <NF/Editor/HotReload.hpp>
#include <NF/Editor/ImportQueue.hpp>
#include <NF/Editor/Inspector.hpp>
#include <NF/Editor/Outliner.hpp>
#include <NF/Editor/PlayMode.hpp>
#include <NF/Editor/Selection.hpp>
#include <NF/Editor/Viewport.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/SaveSystem.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Runtime/Runtime.hpp>

#include <string>

namespace nf::editor {

struct EditorStatus {
    std::string scene_label;
    bool dirty = false;
    bool playing = false;
    size_t entity_count = 0;
    size_t selected_count = 0;
    double fps = 0.0;
    double frame_ms = 0.0;
};

class EditorApp {
public:
    EditorApp(assets::VirtualFileSystem& vfs, assets::AssetRegistry& registry,
              assets::AssetManager& manager, ConsoleBuffer& console);

    void attach_runtime(runtime::Runtime* runtime) { m_runtime = runtime; }

    // --- Scene file ops ---
    bool new_scene(std::string& out_err);
    bool open_scene(const std::string& logical_path, std::string& out_err);
    bool save(std::string& out_err);
    bool save_as(const std::string& logical_path, std::string& out_err);
    // Saves a copy without touching path/dirty (automation + tests).
    bool save_copy(const std::string& logical_path, std::string& out_err);
    bool dirty() const { return m_dirty || materials_dirty(); }
    bool scene_dirty() const { return m_dirty; }
    const std::string& scene_path() const { return m_scene_path; }

    // --- Project ---
    //
    // The project the editor was opened in, if any. Empty when the editor was
    // started from the engine tree without --project, which is how it has always
    // worked; the toolbar shows the name and the Build action targets it.
    void set_project(std::string path, std::string name) {
        m_project_path = std::move(path);
        m_project_name = std::move(name);
    }
    const std::string& project_path() const { return m_project_path; }
    const std::string& project_name() const { return m_project_name; }
    bool has_project() const { return !m_project_path.empty(); }

    // --- Structural edits (all undoable, all set dirty) ---
    bool create_entity(const std::string& name, ecs::Entity parent, std::string& out_err);
    void request_delete(ecs::Entity e);
    void cancel_delete();
    bool confirm_delete(std::string& out_err);
    bool delete_entity(ecs::Entity e, std::string& out_err);
    bool rename_entity(ecs::Entity e, const std::string& new_name, std::string& out_err);
    bool set_transform(ecs::Entity e, const TransformEdit& edit, std::string& out_err);
    bool reparent(ecs::Entity e, ecs::Entity new_parent, std::string& out_err);
    bool set_camera(ecs::Entity e, const CameraEdit& edit, std::string& out_err);
    bool set_light(ecs::Entity e, const LightEdit& edit, std::string& out_err);
    bool set_mesh(ecs::Entity e, const std::string& asset_id_text, const std::string& material,
                  std::string& out_err);
    bool drop_mesh_asset(const AssetEntry& entry, std::string& out_err);

    // --- Physics (direct component edit; marks dirty + rebuilds runtime bodies) ---
    bool set_rigid_body(ecs::Entity e, const physics::RigidBodyComponent& rb, std::string& out_err);
    bool set_collider(ecs::Entity e, const physics::ColliderComponent& col, std::string& out_err);

    // --- Animation + Audio (direct component edit, same shape as physics) ---
    // Playback fields only. The clip table and the generated buffer are not
    // editable here: they come from the scene's procedural spec, and inventing
    // them in the inspector would produce a component the save path could not
    // reproduce.
    bool set_animation(ecs::Entity e, const animation::AnimationComponent& anim,
                       std::string& out_err);
    bool set_audio(ecs::Entity e, const audio::AudioComponent& aud, std::string& out_err);

    // --- Gameplay modules (Phase 10) ----------------------------------------
    // Attaches a GameplayModuleComponent naming a registered module to `e`.
    // Re-attaching the module that is already there is a no-op rather than a
    // wipe, so a stray click cannot silently discard the saved state. The
    // component starts with an empty property map: the live module is the source
    // of truth during a session, and Runtime::capture_gameplay_state() fills the
    // snapshot in at save time.
    bool attach_gameplay_module(ecs::Entity e, const std::string& module_name, std::string& out_err);
    bool detach_gameplay_module(ecs::Entity e, std::string& out_err);

    // --- Save slots (Phase 10) ----------------------------------------------
    // Distinct from save()/open_scene(), which persist the *scene* to a path the
    // user names. A save slot is player data: scene + gameplay module state +
    // version metadata, written under `saves://`.
    bool save_game(const std::string& slot, std::string& out_err);
    bool load_game(const std::string& slot, std::string& out_err);
    std::vector<runtime::SaveSystem::SlotInfo> list_saves();
    bool has_save(const std::string& slot);

    /// Autosave interval in seconds. A value <= 0 turns autosave off; "every
    /// frame" is not a save policy.
    void set_autosave(float interval_seconds, const std::string& slot_prefix);
    bool autosave_enabled();
    unsigned autosaves_performed();
    /// Accumulates frame time and autosaves when due. Driven from the editor's
    /// frame, since the Runtime has no business knowing about save slots.
    void tick_autosave(float dt);

    /// The save system, created on first use. Never null after a call.
    runtime::SaveSystem* save_system();

    // --- Shared materials (undoable; visible in the viewport next frame) ---
    bool set_entity_material(ecs::Entity e, const std::string& material_path, std::string& out_err);
    bool set_material_params(const std::string& material_path, const MaterialEdit& edit,
                             std::string& out_err);
    bool set_material_albedo(const std::string& material_path, const std::string& texture_path,
                             std::string& out_err);
    std::string material_albedo(const std::string& material_path) const;
    std::vector<std::string> known_textures();
    // Saves src material to dst (same path = in-place save, clears dirty).
    bool save_material(const std::string& src_path, const std::string& dst_path, std::string& out_err);

    // --- Prefabs (.nfscene templates + linked instances) ---
    bool create_prefab(ecs::Entity root, const std::string& prefab_path, std::string& out_err);
    bool instantiate_prefab(const std::string& prefab_path, ecs::Entity parent, std::string& out_err);
    bool apply_prefab(ecs::Entity instance_root, std::string& out_err);
    bool revert_prefab(ecs::Entity instance_root, std::string& out_err);

    // --- Hot reload (poll-driven; call periodically, not per frame) ---
    HotReload& hot_reload() { return m_hot; }
    // Rebuilds the watch set from the open scene + known assets.
    void rebuild_hot_watch();
    // Polls once; logs every result to the console. Returns reload count.
    size_t poll_hot_reload();

    // --- Import queue (external files -> content://) ---
    ImportQueue& import_queue() { return m_imports; }
    bool import_file(const std::string& src_absolute, const std::string& dst_dir_logical,
                     bool overwrite, size_t& out_job, std::string& out_err);
    // Processes every queued job synchronously; returns jobs completed.
    // Deterministic (no threads); tests use this path.
    size_t process_imports();
    // Async pump for the frame loop (the shell calls this once per frame):
    // dispatches queued jobs to workers and commits finished work in submit
    // order. Returns the first job that reached a terminal state during this
    // pump, or nullptr. The pointer is valid until the next queue mutation.
    const ImportJob* process_one_import();
    std::vector<std::string> known_materials() const;
    bool materials_dirty() const;

    bool undo(std::string& out_err);
    bool redo(std::string& out_err);

    // --- Play mode ---
    bool play(std::string& out_err);
    bool stop(std::string& out_err);
    bool playing() const { return m_play.playing(); }

    // --- Picking (viewport NDC -> selection) ---
    ecs::Entity pick(const ViewCamera& cam, float ndc_x, float ndc_y);

    // --- Frame ---
    void tick(float dt);

    // --- Panel models / state ---
    Selection& selection() { return m_selection; }
    const Selection& selection() const { return m_selection; }
    OutlinerState& outliner() { return m_outliner; }
    CommandStack& stack() { return m_stack; }
    AssetBrowserState& browser() { return m_browser; }
    PlaySession& play_session() { return m_play; }
    ViewportState& viewport() { return m_viewport; }
    GizmoMode gizmo_mode() const { return m_gizmo_mode; }
    void set_gizmo_mode(GizmoMode m) { m_gizmo_mode = m; }
    GizmoSpace gizmo_space() const { return m_gizmo_space; }
    void set_gizmo_space(GizmoSpace s) { m_gizmo_space = s; }
    ConsoleBuffer& console() { return m_console; }

    EditorStatus status() const;
    std::vector<OutlinerRow> outliner_rows() const;
    std::vector<AssetEntry> browser_entries();
    ecs::World* world();
    const ecs::World* world() const;
    assets::AssetManager& asset_manager() { return m_manager; }
    runtime::Runtime* runtime() { return m_runtime; }

private:
    bool require_editable(std::string& out_err) const;
    bool require_materials(std::string& out_err) const;
    void after_mutation(ecs::Entity touched);
    void watch_imported(const ImportJob& job);

    assets::VirtualFileSystem& m_vfs;
    assets::AssetRegistry& m_registry;
    assets::AssetManager& m_manager;
    ConsoleBuffer& m_console;
    runtime::Runtime* m_runtime = nullptr;
    // Created on first use: most editor sessions never touch a save slot, and a
    // SaveSystem needs a mounted `saves://` to be useful.
    std::unique_ptr<runtime::SaveSystem> m_save_system;

    std::string m_scene_path;
    std::string m_project_path;
    std::string m_project_name;
    bool m_dirty = false;
    Selection m_selection;
    OutlinerState m_outliner;
    CommandStack m_stack;
    ImportQueue m_imports;
    HotReload m_hot;
    AssetBrowserState m_browser;
    PlaySession m_play;
    ViewportState m_viewport;
    GizmoMode m_gizmo_mode = GizmoMode::Translate;
    GizmoSpace m_gizmo_space = GizmoSpace::World;

    double m_fps = 0.0;
    double m_frame_ms = 0.0;
    double m_dt_accum = 0.0;
    int m_dt_count = 0;
};

} // namespace nf::editor
