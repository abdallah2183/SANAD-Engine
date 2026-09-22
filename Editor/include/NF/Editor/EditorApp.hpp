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
#include <NF/Assets/MeshExport.hpp>
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
#include <NF/Editor/ProfilerSession.hpp>
#include <NF/Editor/Selection.hpp>
#include <NF/Editor/Viewport.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/SaveSystem.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Runtime/Runtime.hpp>

#include <cstdint>
#include <functional>
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

// The building blocks the Create menu offers (Phase 21). Each one is a cooked
// mesh asset plus an entity that draws it — the same two steps drag & drop
// takes — so a scene authored from the menu is complete on disk, not a set of
// ids that resolve to nothing when the file is reopened.
enum class PrimitiveKind : uint8_t {
    Ground = 0, // 60 x 60 plane, static, with a thin box collider to stand on
    Cube = 1,   // 1 m cube
    Sphere = 2, // 1 m diameter
    Quad = 3,   // 1 m square, upright (what create_quad authors)
};

/// Display name of a primitive ("Ground", "Cube", ...).
const char* primitive_name(PrimitiveKind kind);
/// Asset file stem, e.g. "ground" -> content://Meshes/ground.nfmesh.
const char* primitive_asset_stem(PrimitiveKind kind);
/// Logical path of the primitive's mesh asset: content://Meshes/<stem>.nfmesh.
std::string primitive_asset_path(PrimitiveKind kind);

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
    // A4: delete is immediate and undoable. There is deliberately no
    // request/confirm/cancel pair any more — the two-step flow made Delete look
    // broken (the Confirm button was easy to miss) while Ctrl+Z was already
    // there. Callers use delete_entity directly.
    bool delete_entity(ecs::Entity e, std::string& out_err);
    bool rename_entity(ecs::Entity e, const std::string& new_name, std::string& out_err);
    bool set_transform(ecs::Entity e, const TransformEdit& edit, std::string& out_err);
    bool reparent(ecs::Entity e, ecs::Entity new_parent, std::string& out_err);
    bool set_camera(ecs::Entity e, const CameraEdit& edit, std::string& out_err);
    bool set_light(ecs::Entity e, const LightEdit& edit, std::string& out_err);
    bool set_sky(ecs::Entity e, const SkyEdit& edit, std::string& out_err);
    /// Find the scene's sky entity (the renderer takes the first) and apply
    /// `edit` to it, creating one first when the scene has none. BOTH paths end
    /// in the same edit — a caller must never have to know whether a sky already
    /// existed. Skipping the apply on the create path is exactly what made the
    /// sky preset dropdown look inert: the new sky kept its default palette.
    bool apply_sky_edit(const SkyEdit& edit, std::string& out_err);
    bool set_mesh(ecs::Entity e, const std::string& asset_id_text, const std::string& material,
                  std::string& out_err);
    bool drop_mesh_asset(const AssetEntry& entry, std::string& out_err);

    // --- Scene authoring (Phase 21) -----------------------------------------
    // File > New builds a scene you can actually work in: a 60 m ground plane,
    // a cube resting on it, a sun, the procedural sky and a camera framing the
    // origin. "Why is there no floor" was the single most repeated question
    // about the empty scene, so the default scene has one.
    //
    // ensure_primitive_asset() cooks the mesh on first use: bytes to
    // content://Meshes/<stem>.nfmesh, a copy under cache://, and a registry
    // entry, so the id an entity stores still resolves next session. Existing
    // assets are reused by path, never duplicated.
    bool ensure_primitive_asset(PrimitiveKind kind, assets::AssetId& out_id, std::string& out_err);
    // Asset + entity in ONE undo step (create and place together).
    bool create_primitive(PrimitiveKind kind, ecs::Entity parent, std::string& out_err);
    bool create_directional_light(ecs::Entity parent, std::string& out_err);
    bool create_camera(ecs::Entity parent, std::string& out_err);
    // Attaches the natural-palette procedural sky to its own entity. The
    // renderer takes the first sky in the scene, so callers use this only when
    // the scene has none.
    bool create_sky_entity(ecs::Entity parent, std::string& out_err);

    /// Exports meshes as one file. `whole_scene` exports every mesh entity,
    /// otherwise only the selection; each entity's transform is baked into the
    /// vertices, so the file stands alone. Returns the number of entities
    /// exported (0 with out_error set = nothing to export).
    size_t export_meshes_to_file(const std::string& physical_path, assets::MeshFormat format,
                                 bool whole_scene, std::string& out_error);

    // --- Physics (direct component edit; marks dirty + rebuilds runtime bodies) ---
    bool set_rigid_body(ecs::Entity e, const physics::RigidBodyComponent& rb, std::string& out_err);
    bool set_collider(ecs::Entity e, const physics::ColliderComponent& col, std::string& out_err);

    // --- Destruction (Phase 19; same shape as the physics edits above) --------
    // Adds or replaces a DestructibleComponent. The fracture asset is never
    // built here: it is cooked from this spec when the scene is adopted, so the
    // inspector cannot produce a component the save path cannot reproduce. A
    // spec without a box collider is still accepted — the runtime skips the
    // binding with a warning — because "breakable" and "collides" are two
    // separate decisions the artist makes in two separate sections.
    bool set_destructible(ecs::Entity e, const runtime::DestructibleComponent& d,
                          std::string& out_err);

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
    // Live material preview: validates exactly like set_material_params and
    // writes through immediately, but pushes no undo entry. The panel calls
    // this on every slider tick and commit_material_params() once when the
    // drag ends, so feedback is instant and undo stays one step per gesture.
    bool preview_material_params(const std::string& material_path, const MaterialEdit& edit,
                                 std::string& out_err);
    // Folds preceding preview writes into a single undo step: before is the
    // snapshot taken when the gesture started, after is the live value now.
    bool commit_material_params(const std::string& material_path,
                                const rendering::PBRMaterialParams& before,
                                std::string& out_err);
    bool set_material_mip_mode(const std::string& material_path, rhi::MipMapMode mode,
                               std::string& out_err);
    rhi::MipMapMode material_mip_mode(const std::string& material_path) const;
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

    // --- Viewport mouse drag (move/rotate/scale with the pointer) ---
    // press() hit-tests, selects, and arms a drag (no scene change yet);
    // drag() live-applies pointer movement (no undo entry yet); release()
    // folds the whole gesture into ONE undoable command (a click without
    // movement folds into nothing). abort() restores the start transform
    // with no undo entry (Escape / scene switch). All respect the play lock.
    // `additive` (shift-click) toggles the hit entity into the existing
    // selection instead of replacing it, so a group drags together and the
    // whole gesture still folds into one undo step.
    bool viewport_press(float ndc_x, float ndc_y, const ViewCamera& vc, bool additive,
                        std::string& out_err);
    bool viewport_drag(float ndc_x, float ndc_y, const ViewCamera& vc, std::string& out_err);
    bool viewport_release(std::string& out_err);
    bool viewport_abort_drag(std::string& out_err);
    bool viewport_dragging() const { return m_drag.active(); }

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
    GizmoSnap& gizmo_snap() { return m_gizmo_snap; }
    const GizmoSnap& gizmo_snap() const { return m_gizmo_snap; }
    ConsoleBuffer& console() { return m_console; }

    // P3 asset previews: panels pass a content:// (or any VFS-resolvable)
    // path and get back an ImGui texture id, or 0 when the shell has no GPU
    // thumbnail cache (headless/tests) — 0 is the panel placeholder. The hook
    // is a std::function precisely so EditorApp links no RHI: only the native
    // shell owns a device, so only it can upload.
    std::function<std::uintptr_t(const std::string&)> preview_texture;

    // P4 profiler: a rolling copy of the engine profiler's frames. The engine
    // profiler merges and clears itself every end_frame(), so this is where a
    // session-spanning trace lives. The shell calls capture_frame() once per
    // frame (right after Profiler::end_frame) and clear() when a session begins.
    ProfilerSession& profiler_session() { return m_profiler_session; }
    const ProfilerSession& profiler_session() const { return m_profiler_session; }

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
    // Fills a fresh scene with the default authoring content (ground, cube, sun,
    // sky, camera). Used by new_scene(); separate so tests can assert on the
    // scene it produces without going through the VFS round trip.
    bool build_default_scene(scene::Scene& scene, std::string& out_err);

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
    ProfilerSession m_profiler_session;
    ViewportState m_viewport;
    GizmoMode m_gizmo_mode = GizmoMode::Translate;
    GizmoSpace m_gizmo_space = GizmoSpace::World;
    GizmoSnap m_gizmo_snap{}; // grid snapping; 0 steps = unsnapped
    // Active pointer drag state (viewport mouse move). Not part of the undo
    // stack until release() folds it; aborted (not committed) on scene switch.
    GizmoDrag m_drag;
    ViewCamera m_drag_vc{};
    float m_drag_ndc0x = 0.0f;
    float m_drag_ndc0y = 0.0f;
    float m_drag_lastx = 0.0f;
    float m_drag_lasty = 0.0f;
    float m_drag_distance = 1.0f;
    bool m_drag_moved = false;

    double m_fps = 0.0;
    double m_frame_ms = 0.0;
    double m_dt_accum = 0.0;
    int m_dt_count = 0;
};

} // namespace nf::editor
