#pragma once

#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <string>
#include <string_view>

namespace nf::project {

// Result of a cook run.
struct CookReport {
    size_t cooked = 0;
    size_t skipped = 0; // unchanged since the last cook (fingerprint match)
    size_t failed = 0;
    // Registry entries whose source asset no longer exists. Removed from the
    // registry and their cooked files deleted, so a rebuild does not ship an
    // asset the project no longer contains.
    size_t pruned = 0;
    size_t total() const { return cooked + skipped + failed; }
    bool ok() const { return failed == 0; }
};

// Result of a registry verification pass.
struct VerifyReport {
    size_t total = 0;
    size_t ok_count = 0;   // verified present and parseable
    size_t missing = 0;    // cooked file absent
    size_t bad_format = 0; // cooked file present but unparseable
    bool ok() const { return missing == 0 && bad_format == 0; }
};

// FNV-1a 64 over the source bytes, hex encoded. The cook cache key.
std::string fingerprint_bytes(const void* data, size_t size);

// The cooked counterpart of a content path: content://Meshes/cube.nfmesh ->
// cache://Meshes/cube.nfmesh. Exposed because packaging and verification both
// need to agree with cooking about where an asset lands.
std::string cooked_path_for(std::string_view logical_path, std::string_view cache_mount = "cache://");

// True when the cooker knows how to process this file extension (lowercase,
// with the dot). GLSL sources are NOT cookable: they are compiled to SPIR-V by
// the build, and shipping those is packaging's job.
bool is_cookable(std::string_view extension);

// The asset type implied by an extension, or Unknown.
assets::AssetType asset_type_for(std::string_view extension);

// The registry's `format` string for a type. Centralised because the existing
// convention is not derivable from the enum name: meshes are "nfmesh-v1" and
// textures are "img-v1", which the editor's import queue already writes. A
// cooked entry must agree with an imported one or the two look like different
// formats for the same bytes.
std::string format_for(assets::AssetType type);

// Cook one asset: fingerprint, validate (meshes must parse), write to `output`,
// update the registry. `out_skipped` is set when the fingerprint matches the
// existing entry and the cooked file is already present.
bool cook_one(assets::VirtualFileSystem& vfs,
              assets::AssetRegistry& registry,
              const std::string& input,
              const std::string& output,
              std::string& out_error,
              bool& out_skipped);

// Cook every supported asset under the content mount, mirroring the tree into
// the cache mount. Returns false only on a hard failure (content root
// unreadable); per-asset problems are counted in the report, not fatal.
bool cook_all(assets::VirtualFileSystem& vfs,
              assets::AssetRegistry& registry,
              const std::string& content_mount,
              const std::string& cache_mount,
              CookReport& out_report,
              std::string& out_error);

// Verify every registry entry: the cooked file exists, and a mesh parses.
bool verify_registry(assets::VirtualFileSystem& vfs,
                     const assets::AssetRegistry& registry,
                     VerifyReport& out_report,
                     std::string& out_error);

} // namespace nf::project
