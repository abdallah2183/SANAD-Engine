// NF/Runtime/SaveSystem.cpp — save slots, async save, versioning (Phase 10, W4)

#include <NF/Runtime/SaveSystem.hpp>

#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayState.hpp>
#include <NF/Scene/Scene.hpp>

#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifndef NF_ENGINE_VERSION
#define NF_ENGINE_VERSION "0.0.0-dev"
#endif

namespace nf::runtime {

namespace {

// --- Small text helpers -----------------------------------------------------
//
// Save files are line-oriented, unlike the scene format's space-separated
// tokens, so a value here is allowed to contain spaces and only stops at the
// newline. That is why the save system does not reuse the loader's field_value.

std::string line_value(std::string_view text, std::string_view key) {
    usize pos = 0;
    while (pos <= text.size()) {
        usize end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();

        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.size() > key.size() && line.rfind(key, 0) == 0) {
            std::string_view value = line.substr(key.size());
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            return std::string(value);
        }

        if (end >= text.size()) break;
        pos = end + 1;
    }
    return {};
}

std::string now_iso8601() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// --- Migration registry -----------------------------------------------------
//
// Chained by construction: a migration always moves one version forward, so
// bringing a v1 save up to v3 means running 1->2 then 2->3. A single function
// that jumps versions would have to know every intermediate format.

struct Migration {
    u32                   from = 0;
    SaveSystem::MigrationFn fn = nullptr;
};

std::vector<Migration>& migrations() {
    static std::vector<Migration> table;
    return table;
}

/// Rewrites meta's `schema_version:` line to `to_version`. A migration has to
/// leave the stamp consistent with the bytes it produced, otherwise a load that
/// crashed mid-chain would describe itself as newer than it is.
void stamp_schema(SaveSystem::SlotFiles& files, u32 to_version) {
    const std::string key = "schema_version:";
    usize pos = files.meta.find(key);
    if (pos == std::string::npos) {
        files.meta += "schema_version: " + std::to_string(to_version) + "\n";
        return;
    }
    const usize line_end = files.meta.find('\n', pos);
    const usize value_pos = pos + key.size();
    files.meta.replace(value_pos,
                       (line_end == std::string::npos ? files.meta.size() : line_end) - value_pos,
                       " " + std::to_string(to_version));
}

/// Runs the chain from `from` up to `SaveSystem::kSchemaVersion`, handing the
/// slot's bytes to each link. Returns false when a link is missing, rather than
/// loading a save whose format the reader does not actually understand.
bool run_migrations(u32 from, SaveSystem::SlotFiles& files, std::string& out_error) {
    if (from == SaveSystem::kSchemaVersion) return true;

    // A save from a *newer* engine is refused outright. There is no chain that
    // can walk backwards, and loading it would mean reading fields this build
    // does not know about — silently dropping whatever they held.
    if (from > SaveSystem::kSchemaVersion) {
        out_error = "save: slot was written by a newer engine (schema " + std::to_string(from) +
                    ", this build reads " + std::to_string(SaveSystem::kSchemaVersion) + ")";
        return false;
    }

    u32 current = from;
    while (current != SaveSystem::kSchemaVersion) {
        SaveSystem::MigrationFn fn = nullptr;
        for (const Migration& m : migrations()) {
            if (m.from == current) {
                fn = m.fn;
                break;
            }
        }

        if (fn == nullptr) {
            out_error = "save: no migration from schema version " + std::to_string(current) +
                        " to " + std::to_string(current + 1) +
                        " (this build reads schema " + std::to_string(SaveSystem::kSchemaVersion) + ")";
            return false;
        }
        if (!fn(files, out_error)) {
            out_error = "save: migration " + std::to_string(current) + "->" +
                        std::to_string(current + 1) + " failed: " + out_error;
            return false;
        }
        // Each link owns its own stamp: a migration that changed the bytes is
        // the only authority on which version they now describe.
        stamp_schema(files, current + 1);
        ++current;
    }
    return true;
}

} // namespace

// --- Construction -----------------------------------------------------------

SaveSystem::SaveSystem(assets::VirtualFileSystem& vfs, Runtime& runtime)
    : m_vfs(vfs), m_runtime(runtime) {}

const char* SaveSystem::engine_version() {
    return NF_ENGINE_VERSION;
}

void SaveSystem::register_migration(u32 from_version, MigrationFn fn) {
    if (fn == nullptr) return;
    for (Migration& m : migrations()) {
        if (m.from == from_version) {
            m.fn = fn;  // last registration wins; tooling re-registers freely
            return;
        }
    }
    migrations().push_back(Migration{from_version, fn});
}

// --- Mount ------------------------------------------------------------------

bool SaveSystem::mounted() const {
    for (const std::string& logical : m_vfs.mounts()) {
        if (logical == kSavesMount) return true;
    }
    return false;
}

bool SaveSystem::ensure_mount(const std::filesystem::path& physical, std::string& out_error) {
    if (mounted()) return true;
    const auto result = m_vfs.mount(kSavesMount, physical);
    if (!result.ok) {
        out_error = "save: cannot mount " + std::string(kSavesMount) + ": " + result.error;
        return false;
    }
    return true;
}

bool SaveSystem::ensure_mount(std::string& out_error) {
    if (mounted()) return true;

    // Derived from the content mount so the engine tree and a packaged game both
    // work. The alternative — defaulting to the working directory — would scatter
    // saves wherever the process happened to be launched from.
    const auto content = m_vfs.resolve("content://");
    if (!content.ok) {
        out_error = "save: saves:// is not mounted and content:// is not either, "
                    "so there is nowhere to derive a save directory from";
        return false;
    }
    return ensure_mount(content.value.parent_path() / "Saves", out_error);
}

// --- Slot names -------------------------------------------------------------

bool SaveSystem::is_valid_slot_name(const std::string& slot) {
    if (slot.empty() || slot.size() > 64) return false;
    for (char c : slot) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    // The character whitelist already excludes `/` and `\`, but `.` and `..`
    // pass it, and either would resolve to a directory outside the slot root.
    if (slot == "." || slot == "..") return false;
    return true;
}

std::string SaveSystem::slot_logical_path(const std::string& slot) const {
    return std::string(kSavesMount) + slot;
}

// --- Payload ----------------------------------------------------------------

bool SaveSystem::build_payload(const std::string& slot, Payload& out, std::string& out_error) {
    scene::Scene* scene_ptr = m_runtime.edit_scene();
    if (scene_ptr == nullptr) {
        out_error = "save: no scene is loaded";
        return false;
    }

    // During a session the module owns its state and the component is the
    // snapshot, so capture first: this makes scene.nfscene and modules.txt
    // describe the same instant rather than two instants one frame apart.
    m_runtime.capture_gameplay_state();

    out.slot       = slot;
    out.scene_text = serialize_scene_to_text(*scene_ptr);

    std::ostringstream blocks;
    u32 module_count = 0;
    for (ecs::Entity e : scene_ptr->world().query<gameplay::GameplayModuleComponent>()) {
        const auto* comp = scene_ptr->world().get<gameplay::GameplayModuleComponent>(e);
        if (comp == nullptr || comp->module_name.empty()) continue;

        blocks << "---\n";
        blocks << "module: " << comp->module_name << "\n";
        blocks << "enabled: " << (comp->enabled ? "true" : "false") << "\n";
        // Encoded rather than one key per line so a value containing a newline
        // cannot forge a new entry. Same encoding as the scene format, so a
        // value means the same thing in both files.
        blocks << "props: " << gameplay::encode_properties(comp->properties) << "\n";
        ++module_count;
    }

    std::ostringstream modules;
    modules << "# NOVAForge Save Modules v1\n";
    modules << "schema_version: " << kSchemaVersion << "\n";
    modules << "module_count: " << module_count << "\n";
    modules << blocks.str();
    out.modules_text = modules.str();

    std::ostringstream meta;
    meta << "# NOVAForge Save v1\n";
    meta << "schema_version: " << kSchemaVersion << "\n";
    meta << "engine_version: " << engine_version() << "\n";
    meta << "scene: " << scene_ptr->name() << "\n";
    meta << "saved_at: " << now_iso8601() << "\n";
    out.meta_text = meta.str();
    return true;
}

bool SaveSystem::write_payload(const Payload& payload, std::string& out_error) {
    const auto resolved = m_vfs.resolve(slot_logical_path(payload.slot));
    if (!resolved.ok) {
        out_error = "save: " + resolved.error;
        return false;
    }
    const std::filesystem::path dir = resolved.value;
    std::filesystem::path staging = dir;
    staging += ".staging";
    std::filesystem::path previous = dir;
    previous += ".previous";

    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        out_error = "save: cannot create the staging directory: " + ec.message();
        return false;
    }

    struct Entry {
        const char*        name;
        const std::string* text;
    };
    const Entry entries[] = {
        {kSceneFile,   &payload.scene_text},
        {kModulesFile, &payload.modules_text},
        {kMetaFile,    &payload.meta_text},
    };

    for (const Entry& entry : entries) {
        std::ofstream file(staging / entry.name, std::ios::binary | std::ios::trunc);
        if (!file) {
            out_error = std::string("save: cannot open ") + entry.name + " for writing";
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        file.write(entry.text->data(), static_cast<std::streamsize>(entry.text->size()));
        file.close();
        if (!file) {
            out_error = std::string("save: failed writing ") + entry.name;
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    }

    // Swap, with rollback. The old slot is moved aside rather than deleted, so a
    // failure between here and the publish can put it back — a save that fails
    // must never cost the player the save they already had.
    std::filesystem::remove_all(previous, ec);
    const bool had_previous = std::filesystem::exists(dir, ec);
    if (had_previous) {
        std::filesystem::rename(dir, previous, ec);
        if (ec) {
            out_error = "save: cannot move the existing slot aside: " + ec.message();
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    }

    std::filesystem::rename(staging, dir, ec);
    if (ec) {
        out_error = "save: cannot publish the slot: " + ec.message();
        std::filesystem::remove_all(staging, ec);
        if (had_previous) {
            std::error_code restore_ec;
            std::filesystem::rename(previous, dir, restore_ec);
        }
        return false;
    }

    std::filesystem::remove_all(previous, ec);
    return true;
}

// --- Save / load ------------------------------------------------------------

bool SaveSystem::save_game(const std::string& slot, std::string& out_error) {
    if (!ensure_mount(out_error)) return false;
    if (!is_valid_slot_name(slot)) {
        out_error = "save: invalid slot name '" + slot + "'";
        return false;
    }

    Payload payload;
    if (!build_payload(slot, payload, out_error)) return false;
    if (!write_payload(payload, out_error)) return false;

    NF_LOG_INFO(LogCategory::Core, "SaveSystem: wrote slot '{}'", slot);
    return true;
}

bool SaveSystem::has_save(const std::string& slot) const {
    if (!is_valid_slot_name(slot)) return false;
    const auto exists = m_vfs.exists(slot_logical_path(slot) + "/" + kMetaFile);
    return exists.ok && exists.value;
}

bool SaveSystem::load_game(const std::string& slot, std::string& out_error) {
    if (!ensure_mount(out_error)) return false;
    if (!is_valid_slot_name(slot)) {
        out_error = "load: invalid slot name '" + slot + "'";
        return false;
    }

    const std::string base = slot_logical_path(slot);
    const auto meta_file = m_vfs.read_text(base + "/" + kMetaFile);
    if (!meta_file.ok) {
        out_error = "load: slot '" + slot + "' has no readable " + kMetaFile;
        return false;
    }

    SlotFiles files;
    files.meta = meta_file.value;

    // The scene and the modules are read *before* the chain runs so a migration
    // can rewrite them. Reading first and migrating second would be the same
    // thing for a link that touches nothing, and the only thing a link that
    // touches nothing is good for.
    const auto scene_file = m_vfs.read_text(base + "/" + kSceneFile);
    if (!scene_file.ok) {
        out_error = "load: slot '" + slot + "' has no readable " + kSceneFile;
        return false;
    }
    files.scene = scene_file.value;

    const auto modules_file = m_vfs.read_text(base + "/" + kModulesFile);
    files.modules = modules_file.ok ? modules_file.value : std::string{};

    const std::string schema_text = line_value(files.meta, "schema_version:");
    const u32 schema = schema_text.empty()
                           ? 0u
                           : static_cast<u32>(std::strtoul(schema_text.c_str(), nullptr, 10));

    if (!run_migrations(schema, files, out_error)) return false;
    m_last_migration_from = (schema == kSchemaVersion) ? kNoMigration : schema;

    // Reuse the scene loader rather than a second parser, so a save cannot drift
    // from the format the editor writes.
    SceneLoadResult loaded = load_scene_from_text(files.scene);
    if (!loaded.success || loaded.scene == nullptr) {
        out_error = "load: scene in slot '" + slot + "' did not load: " + loaded.error;
        return false;
    }

    if (!files.modules.empty()) {
        ecs::World& world = loaded.scene->world();

        // Blocks are `---`-separated; each names a module and carries its state.
        std::istringstream stream(files.modules);
        std::string line;
        std::string module_name;
        bool enabled = true;
        std::string props;
        bool in_block = false;

        auto flush = [&]() {
            if (!in_block || module_name.empty()) return;
            ecs::Entity owner = world.create_entity();
            gameplay::GameplayModuleComponent comp;
            comp.module_name = module_name;
            comp.enabled = enabled;
            if (!props.empty()) gameplay::decode_properties(props, comp.properties);
            world.add<gameplay::GameplayModuleComponent>(owner, std::move(comp));
        };

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("---", 0) == 0) {
                flush();
                module_name.clear();
                props.clear();
                enabled = true;
                in_block = true;
                continue;
            }
            if (line.rfind("module:", 0) == 0)  { module_name = line_value(line, "module:"); continue; }
            if (line.rfind("enabled:", 0) == 0) { enabled = line_value(line, "enabled:") == "true"; continue; }
            if (line.rfind("props:", 0) == 0)   { props = line_value(line, "props:"); continue; }
        }
        flush();
    }

    // Swap the loaded scene in and let the modules pick their state up. Doing
    // this through load_scene_from_physical + assignment rather than
    // Runtime::load_scene keeps the slot's own path out of the scene's
    // "last loaded from" bookkeeping — a save is not where the scene lives.
    m_runtime.adopt_scene(std::move(loaded.scene), base + "/" + kSceneFile);

    // A migrated slot is written back with the new schema stamp, so the next
    // load takes the no-migration path. Without this the chain would run on
    // every load forever — and a link that rewrites bytes would rewrite them
    // again on a slot it already rewrote.
    //
    // All three files go back, not just meta: a property rename lands in the
    // scene's component properties *and* in modules.txt's live state, so a slot
    // that persisted only the stamp would be half v0, half v1 — the chain would
    // stop running while un-migrated module bytes were still what the runtime
    // loaded. `files.modules` is guarded on non-empty so a slot that never had a
    // modules file does not gain a zero-byte one.
    if (m_last_migration_from != kNoMigration) {
        const std::string stamped[] = {files.meta, files.scene, files.modules};
        const char* const names[]   = {kMetaFile, kSceneFile, kModulesFile};
        for (usize i = 0; i < 3u; ++i) {
            if (stamped[i].empty()) continue;
            const auto write = m_vfs.write_text(base + "/" + names[i], stamped[i]);
            if (!write.ok) {
                out_error = "load: slot '" + slot + "' migrated but its " +
                            std::string(names[i]) + " could not be written back";
                return false;
            }
        }
    }

    NF_LOG_INFO(LogCategory::Core, "SaveSystem: loaded slot '{}'", slot);
    return true;
}

std::vector<SaveSystem::SlotInfo> SaveSystem::list_saves() const {
    std::vector<SlotInfo> slots;

    const auto root = m_vfs.resolve(kSavesMount);
    if (!root.ok) return slots;

    std::error_code ec;
    if (!std::filesystem::is_directory(root.value, ec)) return slots;

    for (const auto& entry : std::filesystem::directory_iterator(root.value, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        SlotInfo info;
        info.name = entry.path().filename().string();

        std::ifstream meta(entry.path() / kMetaFile, std::ios::binary);
        if (!meta) continue;
        const std::string text((std::istreambuf_iterator<char>(meta)), std::istreambuf_iterator<char>());

        info.scene_name     = line_value(text, "scene:");
        info.engine_version = line_value(text, "engine_version:");
        info.saved_at       = line_value(text, "saved_at:");
        const std::string schema = line_value(text, "schema_version:");
        info.schema_version = schema.empty()
                                  ? 0u
                                  : static_cast<u32>(std::strtoul(schema.c_str(), nullptr, 10));

        slots.push_back(std::move(info));
    }

    std::sort(slots.begin(), slots.end(),
              [](const SlotInfo& a, const SlotInfo& b) { return a.name < b.name; });
    return slots;
}

bool SaveSystem::delete_save(const std::string& slot, std::string& out_error) {
    if (!ensure_mount(out_error)) return false;
    if (!is_valid_slot_name(slot)) {
        out_error = "delete: invalid slot name '" + slot + "'";
        return false;
    }

    const auto resolved = m_vfs.resolve(slot_logical_path(slot));
    if (!resolved.ok) {
        out_error = "delete: " + resolved.error;
        return false;
    }

    std::error_code ec;
    const auto removed = std::filesystem::remove_all(resolved.value, ec);
    if (ec) {
        out_error = "delete: " + ec.message();
        return false;
    }
    if (removed == 0) {
        out_error = "delete: slot '" + slot + "' does not exist";
        return false;
    }
    return true;
}

// --- Async ------------------------------------------------------------------

bool SaveSystem::async_save_pending() const {
    return m_async_group.pending() != 0;
}

bool SaveSystem::save_game_async(const std::string& slot, std::string& out_error) {
    if (!ensure_mount(out_error)) return false;
    if (!is_valid_slot_name(slot)) {
        out_error = "save: invalid slot name '" + slot + "'";
        return false;
    }
    if (async_save_pending()) {
        out_error = "save: a save is already in flight";
        return false;
    }

    // Snapshot on this thread — see the header for why the serialization cannot
    // move to the worker.
    Payload payload;
    if (!build_payload(slot, payload, out_error)) return false;

    {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        m_async_error.clear();
        m_async_failed = false;
    }

    ++m_async_dispatched;

    // The payload is copied into the job, not captured by reference: the caller's
    // local dies as soon as this returns, and the worker outlives it.
    m_async_group.add([this, payload]() {
        std::string error;
        const bool ok = write_payload(payload, error);
        {
            std::lock_guard<std::mutex> lock(m_async_mutex);
            m_async_failed = !ok;
            m_async_error = error;
        }
        if (!ok) {
            NF_LOG_ERROR(LogCategory::Core, "SaveSystem: async save of '{}' failed: {}",
                         payload.slot, error);
        }
    });

    return true;
}

bool SaveSystem::wait_for_async_save(std::string& out_error) {
    m_async_group.wait();

    std::lock_guard<std::mutex> lock(m_async_mutex);
    if (m_async_failed) {
        out_error = m_async_error;
        return false;
    }
    ++m_async_completed;
    return true;
}

// --- Autosave ---------------------------------------------------------------

void SaveSystem::set_autosave(f32 interval_seconds, const std::string& slot_prefix,
                              u32 max_slots) {
    if (interval_seconds <= 0.0f) {
        disable_autosave();
        return;
    }
    // A cap below one means "keep no slots", which is a delete policy wearing a
    // save policy's clothes. Clamp rather than honour it.
    m_autosave_max_slots = max_slots < 1u ? 1u : max_slots;
    m_autosave_interval = interval_seconds;
    m_autosave_prefix = slot_prefix.empty() ? std::string("autosave_") : slot_prefix;
    m_autosave_elapsed = 0.0f;
}

void SaveSystem::disable_autosave() {
    m_autosave_interval = 0.0f;
    m_autosave_elapsed = 0.0f;
}

void SaveSystem::tick(f32 dt) {
    if (m_autosave_interval <= 0.0f) return;
    if (!(dt > 0.0f)) return;  // NaN or a negative delta must not rewind the timer

    m_autosave_elapsed += dt;
    if (m_autosave_elapsed < m_autosave_interval) return;
    m_autosave_elapsed = 0.0f;

    // Skipped, not queued: losing one autosave is recoverable, and two writers
    // interleaving into one slot is not.
    if (async_save_pending()) {
        NF_LOG_WARN(LogCategory::Core, "SaveSystem: autosave skipped, a save is already in flight");
        return;
    }

    // Rotating ring: slot N+1 lands on slot (N mod max) + 1, so a long session
    // reuses a fixed handful of directories instead of growing forever. The
    // write is a staging-directory swap, so landing on an occupied slot leaves
    // none of the previous occupant's files behind.
    const u32 index = (m_autosaves % m_autosave_max_slots) + 1u;
    const std::string slot = m_autosave_prefix + std::to_string(index);
    std::string error;
    if (!save_game(slot, error)) {
        NF_LOG_ERROR(LogCategory::Core, "SaveSystem: autosave to '{}' failed: {}", slot, error);
        return;
    }
    ++m_autosaves;
}

} // namespace nf::runtime
