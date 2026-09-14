#pragma once

// NF/Editor/ImportQueue.hpp — external file import into content:// (Phase 5,
// async since Phase 6).
//
// Brings files from OUTSIDE the project (an absolute source path) into the
// content tree through the same validate -> copy -> register pipeline the
// AssetCooker uses, so imported assets are immediately usable (browser,
// drag-drop, albedo picker) and cooker --verify stays green:
//
//   .nfmesh -> content://Meshes/<name> (+ cache copy + registry entry)
//   .png/.jpg/.jpeg/.bmp/.tga -> content://Textures/<name> (+ cache copy +
//     registry entry, cooked next to the mesh convention)
//   .nfmat -> content://Materials/<name> (parsed, no registry — like scenes)
//   .nfscene -> content://Scenes/<name> (parsed, no registry)
//
// Threading model: submit() queues; pump_async() dispatches Queued jobs to
// JobSystem workers and commits finished work on the calling thread, in
// submit order. Workers touch NO shared state — they read the source file
// with plain std::ifstream and validate/parse into a per-job staged block;
// VFS writes, registry mutation and Runtime invalidation all happen on the
// calling (main) thread during commit. A worker therefore can never race the
// UI, the registry, or another worker. When JobSystem is not initialized,
// workers run inline (fully deterministic — same results, no threads).
// process_next()/process_all() keep their fully-synchronous semantics for
// tests and one-shot tools.

#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetTypes.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nf::editor {

// CPU-side result staged by a worker; owned jointly by the queue entry and
// the worker lambda so an in-flight job can never dangle, even if the queue
// is destroyed first.
struct ImportStaged {
    std::mutex mutex;
    bool finished = false; // worker done (success or failure)
    bool ok = false;
    std::string error;
    float progress = 0.0f; // worker-side mirror (0.3 read, 0.55 validated)
    std::vector<uint8_t> bytes;
    assets::AssetId file_id; // mesh files: embedded id, when valid
    std::string fingerprint; // FNV-1a hex of the source bytes
};

struct ImportJob {
    size_t id = 0;
    std::string src_absolute;
    std::string dst_logical;
    bool overwrite = false;
    assets::AssetType type = assets::AssetType::Unknown;
    enum class State : uint8_t { Queued, Working, Done, Failed };
    State state = State::Queued;
    float progress = 0.0f; // 0..1, staged within process_next()
    std::string error;
    assets::AssetId assigned_id; // valid for registered Mesh/Texture imports
    std::shared_ptr<ImportStaged> staged; // set at dispatch, cleared on commit
};

class ImportQueue {
public:
    ImportQueue() = default;

    // Validates the request (source exists, supported extension, destination
    // free unless overwrite) and queues it. Returns the job id, or 0 + err
    // when the request itself is invalid (0 is never a valid job id).
    size_t submit(const std::string& src_absolute, const std::string& dst_dir_logical,
                  bool overwrite, std::string& out_err);

    // Fully synchronous: completes the oldest queued job inline on the calling
    // thread (worker stages + commit). Returns false when the queue is empty.
    bool process_next(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg);
    void process_all(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg);

    // Async pump for the frame loop: dispatches every Queued job to workers,
    // mirrors worker progress, then commits finished jobs in submit order
    // (stops at the first unfinished one). Returns the first job that reached
    // a terminal state during this pump, or nullptr.
    const ImportJob* pump_async(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg);

    const std::vector<ImportJob>& jobs() const { return m_jobs; }
    void clear_finished(); // drops Done/Failed jobs, keeps Queued/Working
    bool has_pending() const;

private:
    static assets::AssetType type_for_extension(const std::string& ext_lower);
    // Pure-CPU stages (worker-safe): read + validate + fingerprint.
    static void run_cpu_stages(const std::string& src_absolute, assets::AssetType type, size_t job_id,
                               const std::shared_ptr<ImportStaged>& staged);
    // Main-thread commit: copy + register from a finished staged block.
    // Returns false only for an unfinished block (caller bug).
    static bool commit_staged(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg,
                              ImportJob& job);
    std::vector<ImportJob> m_jobs;
    size_t m_next_id = 1;
};

} // namespace nf::editor
