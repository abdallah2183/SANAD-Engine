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
#include <NF/Editor/TransformGizmo.hpp>
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
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Vfx/Components.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/InputBindings.hpp>

#include <cstdint>
#include <filesystem>
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
    // Recovery: a draft written before a scene switch (and by the shell before
    // the close prompt). `recover_last_draft()` opens it as an UNTITLED dirty
    // scene, so the user can Save As instead of overwriting a named scene.
    bool write_recovery_copy(std::string& out_err);
    bool has_recovery_copy() const;
    bool recover_last_draft(std::string& out_err);
    bool dirty() const { return m_dirty || materials_dirty(); }
    bool scene_dirty() const { return m_dirty; }
    const std::string& scene_path() const { return m_scene_path; }
    /// Monotonic identity of the open scene. Host loops keep per-scene state
    /// (frame pacing, for example) and must reset it when this changes. Ordinary
    /// edits do not advance it; a successful new/open/recovery does.
    uint64_t scene_serial() const { return m_scene_serial; }
    assets::VirtualFileSystem& vfs() { return m_vfs; }
    const assets::VirtualFileSystem& vfs() const { return m_vfs; }

    // --- Project ---
    //
    // The project the editor was opened in, if any. Empty when the editor was
    // started from the engine tree without --project, which is how it has always
    // worked; the toolbar shows the name and the Build action targets it.
    void set_project(std::string path, std::string name) {
        m_project_path = std::move(path);
        m_project_name = std::move(name);
        // Opening a project navigates the browser to its files (Unity shows
        // your game, not the engine install); the user can flip back to
        // Content with the root combo.
        m_browser.browser_root = 1;
        m_browser.current_folder.clear();
        m_browser_cache_dirty = true;
        // The import scratch file lands inside the user's own project folder
        // (its Cache folder, which is git-ignored and holds cooked output):
        // a sandboxed process may not write to the system temp directory, and
        // the scene validation then failed for a file the user never touched.
        if (!m_project_path.empty()) {
            m_imports.set_scratch_dir(std::filesystem::path(m_project_path) / "Cache");
        }
    }
    const std::string& project_path() const { return m_project_path; }
    const std::string& project_name() const { return m_project_name; }
    bool has_project() const { return !m_project_path.empty(); }

    // --- Dimension Mode (2D / 3D) ---
    bool is_2d_mode() const { return m_2d_mode; }
    void set_2d_mode(bool enable) { m_2d_mode = enable; }

    // --- Structural edits (all undoable, all set dirty) ---
    bool create_entity(const std::string& name, ecs::Entity parent, std::string& out_err);
    // A4: delete is immediate and undoable. There is deliberately no
    // request/confirm/cancel pair any more — the two-step flow made Delete look
    // broken (the Confirm button was easy to miss) while Ctrl+Z was already
    // there. Callers use delete_entity directly.
    bool delete_entity(ecs::Entity e, std::string& out_err);
    /// Duplicates every selected root and its children as one undo step. The
    /// root copies are offset slightly and selected; their names get a unique
    /// "(n)" suffix. Fails while playing, like every structural edit.
    bool duplicate_selection(std::string& out_err);
    bool rename_entity(ecs::Entity e, const std::string& new_name, std::string& out_err);
    bool set_transform(ecs::Entity e, const TransformEdit& edit, std::string& out_err);
    bool reparent(ecs::Entity e, ecs::Entity new_parent, std::string& out_err);
    bool set_camera(ecs::Entity e, const CameraEdit& edit, std::string& out_err);
    bool set_light(ecs::Entity e, const LightEdit& edit, std::string& out_err);
    bool set_sky(ecs::Entity e, const SkyEdit& edit, std::string& out_err);
    /// Attach or edit the entity's day/night cycle. Goes through the same
    /// validated command path as every other Inspector edit, so the change is
    /// undoable and cannot write a value the runtime would reject.
    bool set_time_of_day(ecs::Entity e, const TimeOfDayEdit& edit, std::string& out_err);
    /// Attach or edit the entity's post-processing block (design §206). Same
    /// validated-command path as every other Inspector edit: undoable, and
    /// unable to store a value the scene loader would then refuse to read back.
    bool set_post_process(ecs::Entity e, const runtime::PostProcessComponent& edit,
                          std::string& out_err);
    /// Attach or edit the entity's projected decal (Phase 36). Transform owns
    /// placement and facing; this edit owns the material, box extent, blend trim
    /// and enable flag, all through the same undoable validated path.
    bool set_decal(ecs::Entity e, const DecalEdit& edit, std::string& out_err);
    /// Attach or edit the entity's weapon (Phase 44). The profile is the scene
    /// data; the live state is rebuilt from it, so an authored magazine starts
    /// full. Undo restores the component the entity had, state included.
    bool set_weapon(ecs::Entity e, const runtime::WeaponProfile& edit, std::string& out_err);
    /// Attach or edit the entity's hit points (Phase 44). Same validated,
    /// undoable path; `health` is clamped into [0, max] rather than refused.
    bool set_health(ecs::Entity e, const runtime::HealthComponent& edit, std::string& out_err);
    /// Attach or edit a combat agent (Phase 46 authoring). The live aim state is
    /// rebuilt by the factory; undo restores the component the entity had.
    bool set_combat_agent(ecs::Entity e, const runtime::CombatAgentComponent& edit,
                          std::string& out_err);
    /// Attach or edit an entity's affiliation (Phase 46).
    bool set_faction(ecs::Entity e, const runtime::FactionComponent& edit,
                     std::string& out_err);
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
    // The neutral ground material, written to content://Materials/Ground.nfmat
    // on first use. The Default material is authored red (it exists for the
    // material-edit demo), and a 60 m red floor reads as a rendering bug to a
    // first-time user. Returns Default if the content tree is not writable.
    std::string ensure_ground_material();
    // Asset + entity in ONE undo step (create and place together).
    bool create_primitive(PrimitiveKind kind, ecs::Entity parent, std::string& out_err);
    bool create_directional_light(ecs::Entity parent, std::string& out_err);
    bool create_point_light(ecs::Entity parent, std::string& out_err);
    bool create_spot_light(ecs::Entity parent, std::string& out_err);
    bool create_camera(ecs::Entity parent, std::string& out_err);
    bool create_sprite(ecs::Entity parent, std::string& out_err);
    bool create_tilemap(ecs::Entity parent, std::string& out_err);
    bool create_audio_emitter(ecs::Entity parent, std::string& out_err);
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

    // --- Scene audio environment --------------------------------------------
    // The level's reverb zone, music and ambience bed. Separate setters from
    // set_audio because they are separate components: a scene can have a cave
    // that echoes and no music. Each is add-or-replace with undo, and undo of
    // the edit that FIRST created the component removes it, so a scene that
    // never had the line round-trips to a file with no line.
    bool set_reverb_zone(ecs::Entity e, const audio::ReverbZoneComponent& zone,
                         std::string& out_err);
    bool set_music(ecs::Entity e, const MusicEdit& edit, std::string& out_err);
    bool set_ambience(ecs::Entity e, const AmbienceEdit& edit, std::string& out_err);

    // --- Gameplay modules (Phase 10) ----------------------------------------
    // Attaches a GameplayModuleComponent naming a registered module to `e`.
    // Re-attaching the module that is already there is a no-op rather than a
    // wipe, so a stray click cannot silently discard the saved state. The
    // component starts with an empty property map: the live module is the source
    // of truth during a session, and Runtime::capture_gameplay_state() fills the
    // snapshot in at save time.
    bool attach_gameplay_module(ecs::Entity e, const std::string& module_name, std::string& out_err);
    bool detach_gameplay_module(ecs::Entity e, std::string& out_err);

    // --- Lua scripts (Phase 24: file-backed ScriptComponent authoring) ------
    // A script is a content://Scripts/*.lua file plus a component naming it.
    // The file is the source of truth (the scene persists the path, never the
    // source inline); the component's source is the file's bytes, loaded at
    // attach time and re-resolved by the Runtime on scene adopt. All three
    // are direct edits like the gameplay module attach above, not undoable
    // commands: they are covered by save/load round-trip tests instead.
    //
    // create_script_file() writes a minimal update(dt) template; it refuses
    // to clobber an existing file. attach/set read the file through the VFS
    // immediately, so Play works without a scene reload.
    bool create_script_file(const std::string& logical_path, std::string& out_err);
    // Unique variant for the "New script" button: tries stem.lua, stem_02.lua,
    // … inside dir and creates the first free one, returning its path. The
    // fixed-name create above refuses to clobber by design, which left the
    // button failing forever once script.lua existed — this is the button's
    // answer (never overwrite, never fail spuriously).
    bool create_unique_script_file(const std::string& dir_logical, const std::string& stem,
                                   std::string& out_path, std::string& out_err);
    bool attach_script(ecs::Entity e, const std::string& logical_path, std::string& out_err);
    bool set_script_enabled(ecs::Entity e, bool enabled, std::string& out_err);
    bool set_script_path(ecs::Entity e, const std::string& logical_path, std::string& out_err);
    bool detach_script(ecs::Entity e, std::string& out_err);

    // --- Destructible / particles / cloth / character (Phase 25) -----------
    // Same direct-edit shape as the script methods above: validated at the
    // door, add-or-replace, covered by save/load round-trip tests instead of
    // undo commands. The setters take full components so the inspector can
    // forward its cached fields without the App knowing widget layout.
    bool detach_destructible(ecs::Entity e, std::string& out_err);

    // --- Combat feedback (Phase 59) -----------------------------------------
    // The `Feedback:` line is the only authoring path until now, which meant a
    // level designer had to hand-edit the scene file to tune a muzzle flash.
    // `set_feedback` is add-or-replace like the other full-component setters and
    // validates every number against the loader's own ranges, so a value the
    // inspector accepts is a value the scene file can reproduce.
    bool set_feedback(ecs::Entity e, const runtime::FeedbackComponent& fb, std::string& out_err);
    bool detach_feedback(ecs::Entity e, std::string& out_err);

    // --- Perception (Phase 60) ----------------------------------------------
    // An NPC's senses were authorable only by hand-editing the scene file. The
    // setter validates against the loader's own ranges, so a value the inspector
    // accepts is a value the `Perception:` line can carry back.
    bool set_perception(ecs::Entity e, const runtime::PerceptionComponent& pc,
                        std::string& out_err);
    bool detach_perception(ecs::Entity e, std::string& out_err);

    // --- Decision layer (Phase 61) ------------------------------------------
    // The considerations an NPC weighs, with the same ranges the loader and the
    // validator enforce. The setter refuses an empty name and a duplicate one,
    // because a decision nobody can name is a decision nobody can act on.
    bool set_brain(ecs::Entity e, const runtime::BrainComponent& brain, std::string& out_err);
    bool detach_brain(ecs::Entity e, std::string& out_err);

    // --- State machines (Phase 62) ------------------------------------------
    // States and guarded edges. The setter refuses what the validator calls an
    // error — an edge to a state nothing declares — because a machine that drops
    // an edge is an NPC that never leaves a state.
    bool set_state_machine(ecs::Entity e, const runtime::StateMachineComponent& machine,
                           std::string& out_err);
    bool detach_state_machine(ecs::Entity e, std::string& out_err);
    // Binds a VFS audio file to the entity (add-or-replace): reads and
    // decodes it now, so the waveform preview and Play work without a scene
    // reload. A file-backed buffer clears any procedural tone on the same
    // entity — the file wins, documented, never mixed silently.
    bool set_audio_buffer(ecs::Entity e, const std::string& logical_path, std::string& out_err);
    bool attach_particles(ecs::Entity e, std::string& out_err);
    bool set_particles(ecs::Entity e, const vfx::ParticleComponent& pc, std::string& out_err);
    bool detach_particles(ecs::Entity e, std::string& out_err);
    bool attach_cloth(ecs::Entity e, std::string& out_err);
    bool set_cloth(ecs::Entity e, const physics::ClothComponent& cc, std::string& out_err);
    bool detach_cloth(ecs::Entity e, std::string& out_err);
    bool attach_character(ecs::Entity e, std::string& out_err);
    bool set_character(ecs::Entity e, const physics::CharacterComponent& ch, std::string& out_err);
    // Live input only (never persisted): what gameplay, AI or a debug panel
    // writes every tick. Separated from set_character so a config edit can
    // never wipe or invent intent.
    bool set_character_input(ecs::Entity e, const Vec3& wish_dir, bool jump, std::string& out_err);
    bool detach_character(ecs::Entity e, std::string& out_err);
    // --- Navigation (Phase 28) ----------------------------------------------
    // Same direct-edit shape as the systems above: validated at the door,
    // add-or-replace, covered by save/load round-trip tests. A volume has no
    // live counterpart in the editor (the mesh is baked by the Runtime on
    // adopt), so authoring one is a component write like any other.
    bool attach_navmesh(ecs::Entity e, std::string& out_err);
    bool set_navmesh(ecs::Entity e, const runtime::NavMeshComponent& nm, std::string& out_err);
    bool detach_navmesh(ecs::Entity e, std::string& out_err);
    /// Terrain (Phase 63): the ground the navigation mesh walks on. Editing it
    /// re-runs the terrain build AND the bake, because the mesh the author sees
    /// and the surface the agents path on are the same field — a rebuild that
    /// touched only one of them would be a lie.
    bool attach_terrain(ecs::Entity e, std::string& out_err);
    bool set_terrain(ecs::Entity e, const runtime::TerrainComponent& terrain,
                     std::string& out_err);
    bool detach_terrain(ecs::Entity e, std::string& out_err);
    bool attach_nav_agent(ecs::Entity e, std::string& out_err);
    bool set_nav_agent(ecs::Entity e, const runtime::NavAgentComponent& na, std::string& out_err);
    bool detach_nav_agent(ecs::Entity e, std::string& out_err);
    /// Patrol routes (Phase 64): an optional list of destinations for the
    /// NavAgent on the same entity. Attaching authors a two-point route around
    /// the entity so the inspector has something to edit; the live point index
    /// is runtime state and is never part of the component.
    bool attach_nav_route(ecs::Entity e, std::string& out_err);
    bool set_nav_route(ecs::Entity e, const runtime::NavRouteComponent& route,
                       std::string& out_err);
    bool detach_nav_route(ecs::Entity e, std::string& out_err);
    /// Squad radio (Phase 68): the optional `Squad:` line that shares personally
    /// sensed contacts with same-token members. Attaching authors a default
    /// token and range so the component round-trips instead of saving as an
    /// inert, empty line. Applying validates the token and range against the
    /// loader's own rules.
    bool attach_squad(ecs::Entity e, std::string& out_err);
    bool set_squad(ecs::Entity e, const runtime::SquadComponent& squad, std::string& out_err);
    bool detach_squad(ecs::Entity e, std::string& out_err);
    /// Radio jammer (Phase 70): the optional `Jammer:` field that degrades
    /// squad radios inside its radius. Attaching authors the default 12 m,
    /// full-strength field; applying validates radius and strength against the
    /// loader's own bounds.
    bool attach_jammer(ecs::Entity e, std::string& out_err);
    bool set_jammer(ecs::Entity e, const runtime::JammerComponent& jammer,
                    std::string& out_err);
    bool detach_jammer(ecs::Entity e, std::string& out_err);
    // --- Vehicles (Step 11) ----------------------------------------------------
    // Same direct-edit shape as navigation: validated at the door, add-or-replace,
    // and a synchronous vehicle re-bind so the authored car simulates without a
    // reopen. set_vehicle scrubs live inputs and the Jolt view (session state,
    // never scene data); set_vehicle_input writes live inputs only, separated
    // so a tune edit can never wipe or invent intent — same split as character.
    bool attach_vehicle(ecs::Entity e, std::string& out_err);
    bool set_vehicle(ecs::Entity e, const physics::VehicleComponent& vc, std::string& out_err);
    bool detach_vehicle(ecs::Entity e, std::string& out_err);
    bool set_vehicle_input(ecs::Entity e, f32 throttle, f32 steer, f32 brake, f32 handbrake,
                           std::string& out_err);
    bool reset_vehicle_pose(ecs::Entity e, std::string& out_err);

    // --- Asset folders (Unity-style Project panel) --------------------------
    // Creates a VFS directory for the browser (content:// or project://, no
    // spaces — script paths stop at the first space). The folder appears in
    // the tree once it holds a file; the panel stays navigated into it so a
    // script created right after lands where the user is looking.
    bool create_asset_folder(const std::string& logical_dir, std::string& out_err);

    // --- Asset delete / rename (the FileSystem dock's destructive verbs) ---
    //
    // Both validate BEFORE touching the disk and both refuse a mount root, so
    // the panel cannot offer a Delete on "content://" that would take the tree
    // with it. `recursive` is what a FOLDER needs; a file is deleted either
    // way. A scene/prefab deletion additionally offers a re-open of the scene
    // currently open if that scene is the one being removed, because leaving
    // the editor pointed at a file that no longer exists is the confusing
    // half of the operation.
    bool delete_asset(const std::string& logical_path, bool recursive, std::string& out_err);
    bool rename_asset(const std::string& from_logical, const std::string& to_logical,
                      std::string& out_err);

    // --- External IDE (Unity-style "open the script in Visual Studio") -----
    // Resolves a logical asset (usually content://Scripts/*.lua) to its file
    // and opens it detached in Visual Studio / VS Code / the shell default,
    // in that order. Reports which one it picked in out_kind ("Visual
    // Studio", "VS Code", "shell default") so the panel can say what
    // happened instead of opening silently.
    bool open_asset_in_ide(const std::string& logical_path, std::string& out_kind,
                           std::string& out_err);

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
    // The canonical prefab path for an entity: "content://Prefabs/<Name>.nfscene".
    //
    // The name comes from the entity's NameComponent where it has one, and
    // from its id where it does not. The id alone is not enough: the ECS
    // RECYCLES ids, so a prefab named after an id is overwritten by the next
    // entity that gets the same id — a different prefab, silently gone, with
    // no undo entry.
    std::string prefab_path_for(ecs::Entity root) const;
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
    // Launches the current scene in a standalone game window (NFGamePlayer,
    // spawned next to the editor). The live edit scene is written to a
    // play-session file first, so Play plays exactly what is on screen,
    // saved or not. The editor stays fully editable while the game runs —
    // closing the game window is the Stop.
    bool launch_game(std::string& out_err);

    // --- Picking (viewport NDC -> selection) ---
    ecs::Entity pick(const ViewCamera& cam, float ndc_x, float ndc_y);

    // --- View pivot + framing ---------------------------------------------
    //
    // The viewport camera orbits a PIVOT, not the world origin. That used to be
    // hardcoded to (0,0,0), which made any content away from the origin
    // unreachable: a level authored at x = 50 could not be orbited, zoomed to,
    // or looked at, no matter how the user dragged. The pivot is the fix, and
    // framing is how the user sets it without typing coordinates.
    //
    // The pivot is VIEW state, not scene state: it is never written to the
    // scene, never undoable, and never saved. Moving it cannot dirty a scene.
    const float* view_pivot() const { return &m_view_pivot[0]; }
    void set_view_pivot(float x, float y, float z) {
        m_view_pivot[0] = x;
        m_view_pivot[1] = y;
        m_view_pivot[2] = z;
    }

    /// World-space AABB of one entity, or false when it has no renderable mesh
    /// (a camera, a light, a bare transform). This is the SINGLE source of
    /// truth for "where is this entity": picking ray-tests it and framing
    /// centres on it, so the two can never disagree about an object's extent.
    bool world_bounds(ecs::Entity e, AABB& out) const;
    /// Union of world_bounds over the current selection. False when nothing in
    /// the selection is renderable.
    bool selection_bounds(AABB& out) const;
    /// Union over every renderable entity in the edit scene.
    bool scene_bounds(AABB& out) const;

    /// Centre the view on the selection. Moves the pivot AND carries the camera
    /// eye along with it, so the view direction the user had is preserved
    /// instead of swinging to a new angle. The matching distance is NOT set
    /// here (the fov lives in the view layer); instead the framed radius is
    /// handed to consume_view_fit() and applied by the caller on the same
    /// frame. Fails with a reason when there is nothing to frame, so a toolbar
    /// button can say why it did nothing.
    bool frame_selection(std::string& out_err);
    /// As above, over everything in the scene. The "I have lost the level"
    /// escape hatch.
    bool frame_all(std::string& out_err);
    /// Takes the radius of the last frame_* call, or 0 when there was none.
    /// The caller turns it into an eye distance (it owns the fov) and must
    /// call this every frame — it is a one-shot queue, not a state flag.
    float consume_view_fit();
    /// Aiming distance for a world-space radius at the given vertical fov, with
    /// a margin so the object is not flush against the viewport edge. Pure, so
    /// the "does framing actually put the thing on screen" property is testable
    /// without a window.
    static float fit_distance_for(float radius, float fov_y_deg, float margin = 1.35f);

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
    // --- Gizmo-handle press (the 3D arrows/rings/boxes) ---
    // Unlike press(), this never re-picks: it arms the drag on the CURRENT
    // selection with the grabbed handle, so grabbing an arrow moves instead
    // of re-selecting. The drag after it is axis/plane/ring-constrained
    // (viewport_drag branches on gizmo_drag_handle()); release/abort are the
    // same as for a camera-plane drag — one undo step or a clean restore.
    bool viewport_gizmo_press(GizmoHandle handle, float ndc_x, float ndc_y, const ViewCamera& vc,
                              std::string& out_err);
    GizmoHandle gizmo_drag_handle() const { return m_drag_handle; }

    // --- Frame ---
    void tick(float dt);

    // --- Panel models / state ---
    Selection& selection() { return m_selection; }
    const Selection& selection() const { return m_selection; }
    OutlinerState& outliner() { return m_outliner; }
    CommandStack& stack() { return m_stack; }
    AssetBrowserState& browser() { return m_browser; }
    /// The asset panel rescans the project tree through this cache (500 ms
    /// TTL): a full recursive walk plus per-mesh registry lookups every frame
    /// costs real milliseconds on large projects and shows nothing new.
    void invalidate_browser() { m_browser_cache_dirty = true; }
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

    // --- Player settings (the overrides the PLAYER owns) -------------------
    //
    // Sensitivity, inverted-Y, FOV, the ADS trim and the shake trim are the
    // player's taste, not the level's. They live in a file outside the scene
    // (`content://Settings/player.nfsettings`), and this is the editor's view
    // of it: the panel edits `player_settings()`, `apply` pushes it into the
    // live runtime, `save` writes the file. A value of 1.0 (or FOV 0) means
    // "the scene's", so an untouched panel changes nothing.
    runtime::PlayerSettings& player_settings() { return m_player_settings; }
    const runtime::PlayerSettings& player_settings() const { return m_player_settings; }
    /// Reads the file into `player_settings()`. False (with a reason) when the
    /// file is missing or does not parse; the current values then survive.
    bool reload_player_settings(std::string& out_err);
    /// Writes `player_settings()` to the file and applies it to the runtime.
    bool save_player_settings(std::string& out_err);
    /// Pushes the current values into the live runtime, if one exists. Play
    /// mode also does this on entry, so a saved file takes effect on the next
    /// session even when the panel was never opened.
    void apply_player_settings();
    bool player_settings_file_exists() const;
    /// True once the file has been read (or attempted) this session, so the
    /// panel can load it lazily instead of doing file I/O in the constructor.
    bool player_settings_loaded() const { return m_player_settings_loaded; }

    // --- Player profiles --------------------------------------------------
    //
    // One machine, more than one player. A profile is a settings file under a
    // name; switching COPIES it over the active file the runtime reads, so the
    // runtime never has to know profiles exist. The index is a small text file
    // because the VFS has no directory listing.
    static constexpr const char* kProfileIndexPath = "content://Settings/players/index.nfindex";

    [[nodiscard]] std::vector<std::string> player_profiles() const;
    /// Writes the current settings as `name`, adding it to the index.
    bool save_player_profile(const std::string& name, std::string& out_err);
    /// Makes `name` the active profile: loads it, applies it, and writes it to
    /// the active file so a runtime started later reads the same thing.
    bool load_player_profile(const std::string& name, std::string& out_err);

    // --- Input bindings ---------------------------------------------------
    //
    // The table the player's controls come from. It is data (`InputBindings`),
    // the editor is where it gets EDITED, and the file it round-trips through
    // is the one the shipped player reads — so a rebind done here is a rebind
    // in the game, not a setting that only exists while the editor is open.
    static constexpr const char* kInputBindingsPath = "content://Input/bindings.nfinput";

    runtime::InputBindings& input_bindings() { return m_input_bindings; }
    const runtime::InputBindings& input_bindings() const { return m_input_bindings; }
    /// Reads the file over the shipped defaults. A missing file is NOT an
    /// error (the defaults are the answer); a file that does not parse leaves
    /// the current table alone and reports why.
    bool load_input_bindings(std::string& out_err);
    bool save_input_bindings(std::string& out_err);
    bool input_bindings_loaded() const { return m_input_bindings_loaded; }
    bool input_bindings_file_exists() const;
    /// Puts one action's binding back to what the engine ships. `false` when
    /// the action is not in the defaults at all.
    bool reset_binding(std::string_view action);

private:
    bool require_editable(std::string& out_err) const;
    bool require_materials(std::string& out_err) const;
    void after_mutation(ecs::Entity touched);
    /// Re-runs the terrain build and the navigation bake (Phase 63). The mesh
    /// the author sees and the surface the agents path on come from one field,
    /// so an edit that rebuilt only one of them would be a lie.
    void rebuild_terrain();
    void watch_imported(const ImportJob& job);
    // Moves the pivot to (x,y,z) and carries the active camera eye by the same
    // delta, so a framing action changes WHERE the user is looking without
    // changing the angle they are looking from. `fit_radius` is queued for
    // consume_view_fit(); pass 0 for "no distance change wanted". Private
    // because the public set_view_pivot() without the eye move leaves the
    // camera swinging on the next orbit — not a state anything should build.
    void move_view_pivot_to(float x, float y, float z, float fit_radius);
    // Fills a fresh scene with the default authoring content (ground, cube, sun,
    // sky, camera). Used by new_scene(); separate so tests can assert on the
    // scene it produces without going through the VFS round trip.
    bool build_default_scene(scene::Scene& scene, std::string& out_err);

    assets::VirtualFileSystem& m_vfs;
    assets::AssetRegistry& m_registry;
    assets::AssetManager& m_manager;
    ConsoleBuffer& m_console;
    runtime::Runtime* m_runtime = nullptr;
    /// The player overrides the panel edits. Mirrored into the runtime on
    /// `apply` and on play, never written into the scene.
    runtime::PlayerSettings m_player_settings;
    bool m_player_settings_loaded = false;
    runtime::InputBindings m_input_bindings = runtime::InputBindings::defaults();
    bool m_input_bindings_loaded = false;
    // Created on first use: most editor sessions never touch a save slot, and a
    // SaveSystem needs a mounted `saves://` to be useful.
    std::unique_ptr<runtime::SaveSystem> m_save_system;

    std::string m_scene_path;
    uint64_t m_scene_serial = 0;
    std::string m_project_path;
    std::string m_project_name;
    bool m_2d_mode = false;
    bool m_dirty = false;
    Selection m_selection;
    OutlinerState m_outliner;
    // Outliner rows rebuilt on mutation, not per frame: building 2000 rows of
    // std::string labels every frame to display a clipped few dozen is the
    // kind of cost that hides until a big scene opens. after_mutation() is
    // the invalidation point; the world pointer + alive count guard against
    // paths that bypass it (scene swap, play-mode entity churn).
    mutable std::vector<OutlinerRow> m_outliner_cache;
    mutable bool m_outliner_cache_valid = false;
    mutable const ecs::World* m_outliner_cache_world = nullptr;
    mutable std::size_t m_outliner_cache_count = 0;
    CommandStack m_stack;
    ImportQueue m_imports;
    HotReload m_hot;
    AssetBrowserState m_browser;
    std::vector<AssetEntry> m_browser_cache;
    double m_browser_cache_age = 1e9;
    bool m_browser_cache_dirty = true;
    PlaySession m_play;
    ProfilerSession m_profiler_session;
    ViewportState m_viewport;
    // Orbit pivot in world space. VIEW state only: not scene data, never
    // undoable, never saved. See view_pivot() for why it is not the origin.
    float m_view_pivot[3] = {0.0f, 0.0f, 0.0f};
    // Radius of the last frame_* call, waiting for the view layer to turn it
    // into an eye distance. 0 = nothing pending. One-shot, not a mode.
    float m_view_fit_radius = 0.0f;
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
    // Gizmo-handle drag snapshot (None = plain camera-plane drag). The
    // geometry is frozen at press: an axis drag slides along a fixed line,
    // a plane/ring drag tracks a fixed plane, and a scale drag measures
    // screen motion against a fixed press-time layout — so the object can
    // move under the gesture without the mapping chasing it.
    GizmoHandle m_drag_handle = GizmoHandle::None;
    float m_drag_origin[3] = {0.0f, 0.0f, 0.0f};
    float m_drag_axis[3] = {1.0f, 0.0f, 0.0f}; // apply-frame axis (see press)
    float m_drag_plane_n[3] = {0.0f, 1.0f, 0.0f}; // world plane normal / ring normal
    float m_drag_tlast = 0.0f; // axis param at the last event
    float m_drag_plast[3] = {0.0f, 0.0f, 0.0f}; // plane hit at the last event
    float m_drag_flast = 1.0f; // scale factor at the last event
    float m_drag_sox = 0.0f, m_drag_soy = 0.0f; // press-time origin, pointer NDC
    float m_drag_sdx = 1.0f, m_drag_sdy = 0.0f; // press-time axis screen dir, NDC
    float m_drag_slen = 1.0f; // press-time axis screen length, NDC units
    float m_drag_srad = 1.0f; // press-time gizmo radius, NDC units

    double m_fps = 0.0;
    double m_frame_ms = 0.0;
    double m_dt_accum = 0.0;
    int m_dt_count = 0;
};

} // namespace nf::editor
