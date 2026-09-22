#include <NF/Project/ProjectPackager.hpp>

#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include "PathUtils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nf::project {

namespace {

constexpr const char* kRegistryLogical = "content://AssetRegistry.nfreg";
constexpr const char* kManifestName = "manifest.txt";

// The files `--shipping` stages. They are this tool's own output, so a rebuild
// owns them the same way it owns Content/, Cache/ and Shaders/.
constexpr const char* kShippingNotes[] = {"VERSION.txt", "README.txt", "REDIST.txt",
                                          "UNINSTALL.txt"};

// Collect "<fingerprint>  <relative path>" for everything under `dir`, sorted so
// two builds can be compared with diff rather than by eye.
bool collect_manifest(const std::filesystem::path& dir,
                      const std::filesystem::path& base,
                      std::vector<std::string>& out_lines,
                      size_t& out_count,
                      std::string& out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return true; // an absent optional subtree is not an error
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto rel = std::filesystem::relative(it->path(), base, ec);
        if (ec) continue;
        std::string fp_err;
        const auto fp = detail::fingerprint_file(it->path(), fp_err);
        if (fp.empty()) {
            out_error = fp_err;
            return false;
        }
        out_lines.push_back(fp + "  " + rel.generic_string());
        ++out_count;
    }
    if (ec) {
        out_error = "failed to enumerate '" + dir.string() + "': " + ec.message();
        return false;
    }
    return true;
}

// --- shipping notes ---------------------------------------------------------

// UTC build time, ISO 8601. Only the version stamp carries a timestamp; the
// manifest deliberately does not, so two identical builds still diff clean.
std::string utc_stamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_s(&tm_utc, &t);
    char stamp[32]{};
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min,
                  tm_utc.tm_sec);
    return stamp;
}

bool stage_shipping_notes(const std::filesystem::path& dir,
                          const std::string& project_name,
                          const std::string& version,
                          std::string& out_error) {
    const std::string version_txt =
        "project:    " + project_name + "\n"
        "tool:       " + version + "\n"
        "engine:     NOVAForge/SANAD 0.1.0\n"
        "target:     windows-x64\n"
        "built:      " + utc_stamp() + "\n";
    if (!detail::write_text(dir / "VERSION.txt", version_txt, out_error)) return false;

    const std::string readme_txt =
        project_name + "\n"
        "=============\n"
        "\n"
        "Run\n"
        "  Double-click NFPlayer.exe. It looks for the single .nfproj beside it,\n"
        "  so no arguments are needed. From a console you can be explicit:\n"
        "\n"
        "    NFPlayer.exe --project " + project_name + ".nfproj\n"
        "\n"
        "  Flags: --frames N (run N frames then exit), --headless (no window),\n"
        "  --validation (Vulkan validation layers), --scene <logical>.\n"
        "\n"
        "Crashes\n"
        "  If the game crashes, a minidump (.dmp) and a human-readable crash\n"
        "  report (.txt) appear in Crashes/ next to NFPlayer.exe. Send BOTH with\n"
        "  your bug report — the .dmp gives developers a stack trace, the .txt\n"
        "  says which build crashed.\n"
        "\n"
        "Removal\n"
        "  See UNINSTALL.txt: delete this folder. That is the whole uninstall.\n";
    if (!detail::write_text(dir / "README.txt", readme_txt, out_error)) return false;

    const std::string redist_txt =
        "Redistribution notes\n"
        "====================\n"
        "Runtime requirements\n"
        "  - Windows x64 (Windows 10 or newer).\n"
        "  - A GPU driver with Vulkan 1.x support (the driver provides\n"
        "    vulkan-1.dll; nothing to bundle).\n"
        "  - No Visual C++ redistributable is needed: a RELEASE build of the\n"
        "    player statically links the CRT. Debug builds do link the debug CRT\n"
        "    and only run on machines with Visual Studio installed — ship\n"
        "    Release builds to players.\n"
        "\n"
        "Signing\n"
        "  This package is NOT code-signed. Windows SmartScreen may show\n"
        "  'Windows protected your PC' on first run: 'More info' -> 'Run\n"
        "  anyway'. Before distributing widely, sign the executables with your\n"
        "  own certificate:\n"
        "\n"
        "    signtool sign /fd SHA256 /tr <timestamp-url> NFPlayer.exe\n"
        "\n"
        "  The engine ships no store signing certificates.\n"
        "\n"
        "Self-containment\n"
        "  The game writes nothing outside this folder (crash artifacts go to\n"
        "  ./Crashes, see UNINSTALL.txt).\n";
    if (!detail::write_text(dir / "REDIST.txt", redist_txt, out_error)) return false;

    const std::string uninstall_txt =
        "Uninstalling " + project_name + "\n"
        "================================\n"
        "Delete this folder. That is the entire uninstall.\n"
        "\n"
        "What the game writes, and where:\n"
        "  ./Crashes/*.dmp, *.txt   crash artifacts (created on a crash)\n"
        "\n"
        "What the game never writes:\n"
        "  - registry entries\n"
        "  - services or scheduled tasks\n"
        "  - Start Menu / Desktop shortcuts (zip distribution is portable)\n"
        "  - anything under %APPDATA% or %LOCALAPPDATA%\n"
        "\n"
        "After deleting the folder, nothing of the game remains on the machine.\n";
    if (!detail::write_text(dir / "UNINSTALL.txt", uninstall_txt, out_error)) return false;
    return true;
}

// --- zip --------------------------------------------------------------------

void put_u16(std::ofstream& out, uint16_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
}

void put_u32(std::ofstream& out, uint32_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
    out.put(static_cast<char>((v >> 16) & 0xFF));
    out.put(static_cast<char>((v >> 24) & 0xFF));
}

struct ZipEntry {
    std::string name; // forward slashes, relative to the archive root
    uint32_t crc = 0;
    uint32_t size = 0;
    uint32_t offset = 0; // local file header position in the archive
};

// DOS date/time has a 2-second granularity and starts in 1984; that is fine for
// a build artifact, and it is what the container format mandates.
void dos_datetime(uint16_t& out_time, uint16_t& out_date) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_value{};
    localtime_s(&tm_value, &t);
    out_time = static_cast<uint16_t>((tm_value.tm_hour << 11) | (tm_value.tm_min << 5) |
                                     (tm_value.tm_sec / 2));
    out_date = static_cast<uint16_t>(((tm_value.tm_year + 1900 - 1980) << 9) |
                                     ((tm_value.tm_mon + 1) << 5) | tm_value.tm_mday);
}

} // namespace

uint32_t crc32_of(const uint8_t* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool zip_directory(const std::filesystem::path& dir,
                   const std::filesystem::path& zip_file,
                   std::string& out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        out_error = "zip: source directory does not exist: '" + dir.string() + "'";
        return false;
    }

    // Enumerate first and sort: the archive layout is deterministic, so two
    // zips of identical content are byte-identical apart from timestamps.
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec) files.push_back(it->path());
    }
    if (ec) {
        out_error = "zip: failed to enumerate '" + dir.string() + "': " + ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());

    uint16_t dos_time = 0, dos_date = 0;
    dos_datetime(dos_time, dos_date);

    std::ofstream out(zip_file, std::ios::binary | std::ios::trunc);
    if (!out) {
        out_error = "zip: failed to create '" + zip_file.string() + "'";
        return false;
    }

    std::vector<ZipEntry> entries;
    entries.reserve(files.size());
    for (const auto& f : files) {
        const auto rel = std::filesystem::relative(f, dir, ec);
        if (ec) {
            out_error = "zip: failed to relativize '" + f.string() + "'";
            return false;
        }
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            out_error = "zip: failed to read '" + f.string() + "'";
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        if (bytes.size() > 0xFFFFFFFFull) {
            out_error = "zip: file too large for a store-only archive: '" + f.string() + "'";
            return false;
        }

        ZipEntry e;
        e.name = rel.generic_string();
        e.size = static_cast<uint32_t>(bytes.size());
        e.crc = crc32_of(bytes.data(), bytes.size());
        e.offset = static_cast<uint32_t>(out.tellp());

        put_u32(out, 0x04034b50); // local file header
        put_u16(out, 20);         // version needed
        put_u16(out, 0);          // flags
        put_u16(out, 0);          // method: stored
        put_u16(out, dos_time);
        put_u16(out, dos_date);
        put_u32(out, e.crc);
        put_u32(out, e.size); // compressed == stored size
        put_u32(out, e.size);
        put_u16(out, static_cast<uint16_t>(e.name.size()));
        put_u16(out, 0); // extra length
        out.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out_error = "zip: write failed for '" + e.name + "'";
            return false;
        }
        entries.push_back(std::move(e));
    }

    const uint32_t cd_offset = static_cast<uint32_t>(out.tellp());
    for (const auto& e : entries) {
        put_u32(out, 0x02014b50); // central directory header
        put_u16(out, 20);         // version made by
        put_u16(out, 20);         // version needed
        put_u16(out, 0);          // flags
        put_u16(out, 0);          // method: stored
        put_u16(out, dos_time);
        put_u16(out, dos_date);
        put_u32(out, e.crc);
        put_u32(out, e.size);
        put_u32(out, e.size);
        put_u16(out, static_cast<uint16_t>(e.name.size()));
        put_u16(out, 0); // extra
        put_u16(out, 0); // comment
        put_u16(out, 0); // disk number start
        put_u16(out, 0); // internal attrs
        put_u32(out, 0); // external attrs
        put_u32(out, e.offset);
        out.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
    }
    const uint32_t cd_size = static_cast<uint32_t>(out.tellp()) - cd_offset;

    put_u32(out, 0x06054b50); // end of central directory
    put_u16(out, 0);          // this disk
    put_u16(out, 0);          // cd disk
    put_u16(out, static_cast<uint16_t>(entries.size()));
    put_u16(out, static_cast<uint16_t>(entries.size()));
    put_u32(out, cd_size);
    put_u32(out, cd_offset);
    put_u16(out, 0); // comment length
    out.flush();
    if (!out) {
        out_error = "zip: failed to finalize '" + zip_file.string() + "'";
        return false;
    }
    return true;
}

bool build_project(const BuildOptions& opts, BuildReport& out, std::string& out_error) {
    out = BuildReport{};

    if (opts.project_file.empty()) {
        out_error = "no project file given";
        return false;
    }

    auto desc = assets::ProjectDescriptor::load_from_file(opts.project_file, out_error);
    if (!desc) {
        return false;
    }
    const auto root = desc->root();

    // --- cook ---------------------------------------------------------------
    assets::VirtualFileSystem vfs;
    if (!desc->apply_mounts(vfs, out_error)) {
        return false;
    }

    assets::AssetRegistry registry;
    {
        auto exists = vfs.exists(kRegistryLogical);
        if (exists.ok && exists.value) {
            if (!registry.load(vfs, kRegistryLogical, out_error)) {
                return false;
            }
        } else {
            registry.set_version(assets::AssetRegistry::kCurrentVersion);
        }
    }

    if (!cook_all(vfs, registry, "content://", "cache://", out.cook, out_error)) {
        return false;
    }
    if (!out.cook.ok()) {
        // Do not package a project with a broken asset: the failure would show
        // up as a missing mesh in the shipped game instead of here.
        return false;
    }
    if (out.cook.cooked > 0 || out.cook.pruned > 0) {
        if (!registry.save(vfs, kRegistryLogical, out_error)) {
            return false;
        }
    }

    // --- stage shaders ------------------------------------------------------
    // Copied into the project so the package carries them. Without this the
    // runtime falls back to searching build/DebugNinja/..., which does not exist
    // outside the engine tree.
    const auto project_shaders = root / "Shaders" / "Basic3D";
    if (!opts.shader_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(opts.shader_dir, ec)) {
            if (!detail::copy_tree(opts.shader_dir, project_shaders, out_error)) {
                return false;
            }
            for (auto it = std::filesystem::recursive_directory_iterator(project_shaders, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec) ++out.shaders_copied;
            }
        } else {
            out_error = "shader directory does not exist: '" + opts.shader_dir.string() +
                        "' (build the engine first)";
            return false;
        }
    }

    // --- assemble the package ----------------------------------------------
    const auto out_dir = opts.output_dir.empty() ? (root / "dist") : opts.output_dir;
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        out_error = "failed to create '" + out_dir.string() + "': " + ec.message();
        return false;
    }

    // Replace only the subtrees this tool owns, so a removed asset does not
    // linger in the package and anything else the user keeps there survives.
    for (const char* sub : {"Content", "Cache", "Shaders"}) {
        std::filesystem::remove_all(out_dir / sub, ec);
    }
    std::filesystem::remove(out_dir / kManifestName, ec);
    // The shipping notes go too. A plain rebuild after a --shipping build must
    // not leave a VERSION.txt claiming a version the package no longer is.
    for (const char* note : kShippingNotes) {
        std::filesystem::remove(out_dir / note, ec);
    }

    for (const char* sub : {"Content", "Cache", "Shaders"}) {
        const auto src = root / sub;
        if (!std::filesystem::exists(src, ec)) continue;
        if (!detail::copy_tree(src, out_dir / sub, out_error)) {
            return false;
        }
    }

    // The descriptor declares relative mounts, so copying it into the package is
    // what makes the package relocatable.
    const auto packaged_project = out_dir / opts.project_file.filename();
    std::filesystem::copy_file(opts.project_file, packaged_project,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        out_error = "failed to copy project file into the package: " + ec.message();
        return false;
    }

    if (!opts.player_exe.empty()) {
        if (!std::filesystem::exists(opts.player_exe, ec)) {
            out_error = "player executable does not exist: '" + opts.player_exe.string() + "'";
            return false;
        }
        std::filesystem::copy_file(opts.player_exe, out_dir / opts.player_exe.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            out_error = "failed to copy player into the package: " + ec.message();
            return false;
        }
    }

    // --- manifest -----------------------------------------------------------
    std::vector<std::string> lines;
    size_t count = 0;
    for (const char* sub : {"Content", "Cache", "Shaders"}) {
        if (!collect_manifest(out_dir / sub, out_dir, lines, count, out_error)) {
            return false;
        }
    }
    std::sort(lines.begin(), lines.end());

    std::string manifest = "# NOVAForge package manifest\n";
    manifest += "# fingerprint       relative path\n";
    for (const auto& l : lines) {
        manifest += l;
        manifest += "\n";
    }
    if (!detail::write_text(out_dir / kManifestName, manifest, out_error)) {
        return false;
    }

    out.manifest_entries = count;
    out.files_packaged = count;
    out.output_dir = out_dir;
    out.packaged_project = packaged_project;

    // --- shipping extras ----------------------------------------------------
    const auto zip_path = std::filesystem::path(out_dir.string() + ".zip");
    std::filesystem::remove(zip_path, ec); // a stale zip must not survive a rebuild
    if (opts.shipping) {
        if (opts.version.empty()) {
            out_error = "shipping package requires a version string (BuildOptions::version)";
            return false;
        }
        if (!stage_shipping_notes(out_dir, desc->name(), opts.version, out_error)) {
            return false;
        }
        // Written NEXT TO the package, never inside it, so re-zipping never
        // nests and the folder itself stays launchable as-is.
        if (!zip_directory(out_dir, zip_path, out_error)) {
            return false;
        }
        out.zip_path = zip_path;
    }
    return true;
}

} // namespace nf::project
