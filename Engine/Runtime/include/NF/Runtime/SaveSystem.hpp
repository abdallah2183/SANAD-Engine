#pragma once

// NF/Runtime/SaveSystem.hpp — save slots, async save, versioning (Phase 10, W4)
//
// Design doc §70. A slot is a directory under `saves://` holding three files:
//
//   scene.nfscene   the entity/component state, in the standard scene format
//   modules.txt     gameplay module state, one block per module
//   meta.txt        schema version, engine version, scene name, timestamp
//
// The format is text, consistent with .nfscene / .nfproj / .nfmat. There is no
// binary format yet and no JSON parser in the project, so a text format is the
// one that can actually be read back by the code that writes it.
//
// Why the payload is built on the calling thread even for the async path: the
// scene is being mutated by the frame loop. Serializing it on a worker would
// race with that. Taking a snapshot in memory is cheap; the expensive part is
// the disk write, and that is what goes to a worker.

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Jobs/JobSystem.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace nf::runtime {

class Runtime;

class SaveSystem {
public:
    static constexpr u32 kSchemaVersion = 1;
    static constexpr const char* kSavesMount  = "saves://";
    static constexpr const char* kSceneFile   = "scene.nfscene";
    static constexpr const char* kModulesFile = "modules.txt";
    static constexpr const char* kMetaFile    = "meta.txt";

    struct SlotInfo {
        std::string name;
        std::string scene_name;
        std::string engine_version;
        std::string saved_at;
        u32         schema_version = 0;
    };

    SaveSystem(assets::VirtualFileSystem& vfs, Runtime& runtime);

    SaveSystem(const SaveSystem&) = delete;
    SaveSystem& operator=(const SaveSystem&) = delete;

    // --- Mount --------------------------------------------------------------
    //
    // `saves://` is declared by ProjectDescriptor::make_default, so a normal run
    // already has it. These exist for a host that mounts content itself and for
    // tests, which need the slot root to be a temp directory.

    bool ensure_mount(const std::filesystem::path& physical, std::string& out_error);
    /// Mounts `saves://` as a sibling of the content mount. Returns false with a
    /// clear error when neither is available, rather than silently saving into
    /// the current working directory.
    bool ensure_mount(std::string& out_error);
    [[nodiscard]] bool mounted() const;

    // --- Synchronous --------------------------------------------------------

    /// Writes a complete slot. Returns false and leaves the previous slot
    /// contents untouched on any failure: the new slot is built in a sibling
    /// directory and renamed into place only once it is complete, so a failed
    /// save cannot destroy a good one.
    bool save_game(const std::string& slot, std::string& out_error);

    /// Restores a slot into the live scene. A schema mismatch runs the
    /// registered migration path before the data is read.
    bool load_game(const std::string& slot, std::string& out_error);

    [[nodiscard]] bool has_save(const std::string& slot) const;
    [[nodiscard]] std::vector<SlotInfo> list_saves() const;
    bool delete_save(const std::string& slot, std::string& out_error);

    // --- Asynchronous -------------------------------------------------------

    /// Snapshots the state now and writes it on a worker. Returns false if a
    /// save is already in flight — overlapping saves to one slot would race on
    /// the rename, and queueing them silently would make the second one's
    /// ordering depend on the job system.
    bool save_game_async(const std::string& slot, std::string& out_error);

    [[nodiscard]] bool async_save_pending() const;

    /// Blocks until the in-flight save finishes. Returns false if it failed;
    /// `out_error` then carries the worker's error.
    bool wait_for_async_save(std::string& out_error);

    [[nodiscard]] u32 async_saves_dispatched() const { return m_async_dispatched; }
    [[nodiscard]] u32 async_saves_completed() const { return m_async_completed; }

    // --- Autosave -----------------------------------------------------------

    /// How many rotating autosave slots are kept before the oldest is replaced.
    /// Three is the usual answer for a game a player can save manually: enough
    /// to undo a mistake, few enough that the disk does not fill over a long
    /// session. `set_autosave` accepts a smaller cap but not a zero one — a cap
    /// of zero would mean "save and immediately delete".
    static constexpr u32 kDefaultAutosaveSlots = 3;

    /// `max_slots` defaults to the ring above; pass a smaller number for a
    /// platform with less disk. A non-positive interval still means off.
    void set_autosave(f32 interval_seconds, const std::string& slot_prefix,
                      u32 max_slots = kDefaultAutosaveSlots);
    void disable_autosave();
    [[nodiscard]] bool autosave_enabled() const { return m_autosave_interval > 0.0f; }
    [[nodiscard]] u32  autosaves_performed() const { return m_autosaves; }
    [[nodiscard]] f32  autosave_elapsed() const { return m_autosave_elapsed; }
    [[nodiscard]] const std::string& autosave_slot_prefix() const { return m_autosave_prefix; }
    [[nodiscard]] u32  autosave_max_slots() const { return m_autosave_max_slots; }

    /// Accumulates `dt` and saves when the interval elapses. Driven from
    /// Runtime::update. A non-positive interval is ignored rather than treated
    /// as "save every frame".
    void tick(f32 dt);

    /// The engine version stamped into meta.txt. Injected at build time from the
    /// CMake project version so it cannot drift from the binary's own version.
    static const char* engine_version();

    // --- Migration ----------------------------------------------------------
    //
    // A migration moves one schema version forward, so the chain from v1 to v3
    // is 1->2 then 2->3. Registering a single function that jumps versions would
    // force it to know every intermediate format, which is the thing migrations
    // exist to avoid.
    //
    // The migration sees the slot's own bytes and may rewrite them: a version
    // bump that changes nothing is a no-op, and a migration that cannot touch
    // the data cannot fix an old save either. The chain stamps meta's
    // schema_version forward as it goes, so a partially-migrated slot can never
    // be mistaken for a current one.

    /// The three files a slot holds, mutable. `meta` is parsed by the save
    /// system itself; `scene` and `modules` are read by the loader after the
    /// chain finishes, so what the reader sees is what the chain produced.
    struct SlotFiles {
        std::string meta;
        std::string scene;
        std::string modules;
    };

    using MigrationFn = bool (*)(SlotFiles& files, std::string& out_error);

    /// Registers (or replaces) the migration from `from_version` to
    /// `from_version + 1`.
    static void register_migration(u32 from_version, MigrationFn fn);

    /// Returned by `last_migration_from` when a load matched the current schema
    /// and no chain ran. It is a sentinel rather than 0 because 0 is also a real
    /// schema version a save can legitimately have migrated *from* — without it,
    /// "nothing happened" and "the oldest save we support was upgraded" report
    /// the same number and are indistinguishable.
    static constexpr u32 kNoMigration = 0xFFFFFFFFu;

    /// Schema version the last successful load had to migrate from, or
    /// `kNoMigration` when it did not. The observable that separates "the
    /// migration path ran" from "the version happened to match".
    [[nodiscard]] u32 last_migration_from() const { return m_last_migration_from; }

private:
    struct Payload {
        std::string slot;
        std::string scene_text;
        std::string modules_text;
        std::string meta_text;
    };

    bool build_payload(const std::string& slot, Payload& out, std::string& out_error);
    bool write_payload(const Payload& payload, std::string& out_error);

    [[nodiscard]] std::string slot_logical_path(const std::string& slot) const;
    [[nodiscard]] static bool is_valid_slot_name(const std::string& slot);

    assets::VirtualFileSystem& m_vfs;
    Runtime&                   m_runtime;

    // Async bookkeeping. The worker writes `m_async_error` and `m_async_failed`;
    // the mutex is there so the main thread reading them after wait() does not
    // depend on the job system's memory ordering being understood correctly.
    JobGroup        m_async_group;
    mutable std::mutex m_async_mutex;
    std::string     m_async_error;
    bool            m_async_failed = false;
    u32             m_async_dispatched = 0;
    u32             m_async_completed = 0;

    f32         m_autosave_interval = 0.0f;
    f32         m_autosave_elapsed = 0.0f;
    std::string m_autosave_prefix;
    u32         m_autosaves = 0;
    u32         m_autosave_max_slots = kDefaultAutosaveSlots;
    u32         m_last_migration_from = kNoMigration;
};

} // namespace nf::runtime
