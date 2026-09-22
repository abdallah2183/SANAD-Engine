#pragma once

#include <NF/Project/ProjectCooker.hpp>

#include <filesystem>
#include <string>

namespace nf::project {

struct BuildOptions {
    std::filesystem::path project_file; // the .nfproj to build
    std::filesystem::path player_exe;   // copied into the package when non-empty
    std::filesystem::path shader_dir;   // compiled SPIR-V, copied to <project>/Shaders/Basic3D
    std::filesystem::path output_dir;   // empty means <project>/dist
    bool shipping = false;              // stage the shipping extras + emit the zip
    std::string version;                // stamped into VERSION.txt ("nf 0.1 (NOVAForge Phase 7)")
};

struct BuildReport {
    CookReport cook;
    size_t shaders_copied = 0;
    size_t files_packaged = 0;
    size_t manifest_entries = 0;
    std::filesystem::path output_dir;
    std::filesystem::path packaged_project; // the .nfproj inside the package
    std::filesystem::path zip_path;         // the .zip archive (shipping builds)
};

// Cooks the project, stages its shaders, and writes a self-contained package.
//
// The package is a plain directory — deliberately not an archive. A container
// format would add something to maintain and debug for no user-visible benefit
// at this stage, and "it shipped and it runs" is provable either way.
//
//   dist/
//     <Name>.nfproj      mounts are relative, so the package is relocatable
//     <player>.exe
//     Content/           the source assets, mirrored
//     Cache/             the cooked assets the runtime actually reads
//     Shaders/Basic3D/   SPIR-V, so the hardcoded build-tree search is not load-bearing
//     manifest.txt       sorted "<fingerprint>  <relative path>", diffable between builds
//
// Re-running replaces Content/, Cache/ and Shaders/ inside the output directory
// so a removed asset does not linger in the package, but leaves anything else in
// that directory alone.
bool build_project(const BuildOptions& opts, BuildReport& out, std::string& out_error);

// --- shipping staging -------------------------------------------------------
//
// `opts.shipping = true` adds the files that turn a folder of engine output
// into a package a stranger can evaluate, run, and remove:
//
//   VERSION.txt    project name, tool/engine version, UTC build time, target
//   README.txt     how to run, command-line flags, where crashes go
//   REDIST.txt     unsigned-build note (SmartScreen), CRT/Vulkan facts
//   UNINSTALL.txt  exactly what the package writes and how to remove it
//
// and archives the whole folder to <output_dir>.zip. The zip is deliberately
// written NEXT TO the package (not inside it) so re-zipping never nests.

// CRC-32 (IEEE 0xEDB88320, the variant every zip container uses). Exposed so
// tests can verify archive entries against the bytes they claim to contain.
uint32_t crc32_of(const uint8_t* data, size_t size);

// Archives `dir` (recursively, all regular files) to `zip_file` using stored
// (uncompressed) entries. Store-only is deliberate: it is ~120 lines instead of
// a vendored zlib, and the archives are small at this stage. Returns false with
// out_error set on the first unreadable file or failed write.
bool zip_directory(const std::filesystem::path& dir,
                   const std::filesystem::path& zip_file,
                   std::string& out_error);

} // namespace nf::project
