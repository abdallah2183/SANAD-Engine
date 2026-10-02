#pragma once

// NF/Editor/ImportQueue.hpp — external file import into content:// (Phase 5,
// async since Phase 6).
//
// Brings files from OUTSIDE the project (an absolute source path) into the
// content tree through the same validate -> copy -> register pipeline the
// AssetCooker uses, so imported assets are immediately usable (browser,
// drag-drop, albedo picker) and cooker --verify stays green:
//
//   MODELS — .gltf/.glb/.obj/.stl/.ply (and .nfmesh) go through
//     nf::assets::import_mesh_file and land as cooked .nfmesh:
//       <dir>/<stem>.nfmesh          (mesh 0)
//       <dir>/<stem>_1.nfmesh ...    (every further mesh in the source)
//     A multi-mesh source therefore imports as several registered assets
//     rather than one asset plus a silent loss of the rest. The original
//     model file is NOT copied: the cooked mesh is the asset, and keeping the
//     uncooked source around would invite editing the wrong one.
//     A model also brings its MATERIALS and TEXTURES, because geometry alone is
//     not an import a developer can use:
//       content://Textures/<stem>_<image>.<ext>   (registered texture assets)
//       content://Materials/<stem>_<material>.nfmat
//     Each .nfmat carries the imported base colour and points its `albedo` at
//     the written texture, which is the only route a material has into the
//     engine: .nfmesh cannot store one. What the source declared and the
//     engine's material block has no room for is reported in `warnings`.
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
#include <NF/Assets/MeshImport.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nf::editor {

/// One cooked output file staged by a worker. A model source produces one per
/// mesh, one per extracted texture and one per material; every other type
/// produces exactly one, holding its own bytes.
struct ImportStagedFile {
    /// Full logical destination. A model import writes into several directories
    /// (Meshes, Textures, Materials), so the destination is per FILE, not per
    /// job.
    std::string logical_path;
    std::vector<uint8_t> bytes;
    assets::AssetId file_id; // a .nfmesh's embedded id, when it has a valid one
    /// Meshes and textures get a registry entry plus a cache copy; text assets
    /// (materials, scenes) bypass the registry, as they always have.
    bool registered = false;
    assets::AssetType type = assets::AssetType::Unknown;
};

// CPU-side result staged by a worker; owned jointly by the queue entry and
// the worker lambda so an in-flight job can never dangle, even if the queue
// is destroyed first.
struct ImportStaged {
    std::mutex mutex;
    bool finished = false; // worker done (success or failure)
    bool ok = false;
    std::string error;
    float progress = 0.0f; // worker-side mirror (0.3 read, 0.55 validated)
    std::vector<ImportStagedFile> files; // >= 1 on success
    /// Everything the reader could not carry over (STL's missing UVs, computed
    /// normals, a skipped face, a material slot this pipeline has no room for).
    /// Surfaced to the user instead of being dropped: the same
    /// no-silent-substitution ledger the importers publish.
    std::vector<std::string> warnings;
    std::string fingerprint; // FNV-1a hex of the SOURCE bytes
};

/// One file a commit wrote, with the type it landed as. A model import writes
/// meshes, textures and materials, and each has to be watched the right way.
struct ImportWritten {
    std::string logical_path;
    assets::AssetType type = assets::AssetType::Unknown;
};

struct ImportJob {
    size_t id = 0;
    std::string src_absolute;
    /// Destination directory, e.g. content://Meshes. Kept alongside
    /// dst_logical because a multi-mesh import writes several files into it.
    std::string dst_dir;
    /// The primary destination: what the UI names and what a caller opens.
    /// For a model source this is `<dir>/<stem>.nfmesh`, not the source name.
    std::string dst_logical;
    bool overwrite = false;
    assets::AssetType type = assets::AssetType::Unknown;
    /// Which reader the worker must use for a mesh source. Unknown means the
    /// file is already cooked (.nfmesh) or is not a mesh at all.
    assets::MeshImportFormat source_format = assets::MeshImportFormat::Unknown;
    enum class State : uint8_t { Queued, Working, Done, Failed };
    State state = State::Queued;
    float progress = 0.0f; // 0..1, staged within process_next()
    std::string error;
    assets::AssetId assigned_id; // valid for registered Mesh/Texture imports
    /// Every file the commit wrote, primary first. One entry for most imports;
    /// a model import contributes its meshes, then its textures, then its
    /// materials.
    std::vector<ImportWritten> written;
    std::vector<std::string> warnings;
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

    /// Extensions a file dialog should offer, as a semicolon-separated glob
    /// ("*.gltf;*.glb;*.obj;..."), derived from the readers themselves so the
    /// dialog can never advertise a format submit() would reject. This is
    /// exactly the bug this list exists to prevent: the dialog used to offer
    /// *.gltf/*.glb while submit() answered "Unsupported extension".
    static std::string accepted_extension_glob();

private:
    static assets::AssetType type_for_extension(const std::string& ext_lower);
    // Pure-CPU stages (worker-safe): read + validate/cook + fingerprint.
    static void run_cpu_stages(const std::string& src_absolute, const std::string& dst_dir,
                               assets::AssetType type, assets::MeshImportFormat source_format,
                               size_t job_id, const std::shared_ptr<ImportStaged>& staged);
    // Main-thread commit: copy + register from a finished staged block.
    // Returns false only for an unfinished block (caller bug).
    static bool commit_staged(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg,
                              ImportJob& job);
    std::vector<ImportJob> m_jobs;
    size_t m_next_id = 1;
};

} // namespace nf::editor
