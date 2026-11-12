#include "WindowsGameExport.hpp"

#if defined(_WIN32)
#include <NF/Project/EmbeddedGameResources.hpp>

#include <fstream>
#include <set>
#include <vector>

namespace nf::project {
namespace {

namespace fs = std::filesystem;
namespace game = embedded_game;

bool windows_error(std::string& error, const char* operation) {
    const DWORD code = GetLastError();
    error = std::string(operation) + " (Windows error " + std::to_string(code) + ")";
    return false;
}

// A copy of the project with the same name has the same save directory. The
// build ID, separately, identifies each exported copy of the game data.
std::wstring save_id(const assets::ProjectDescriptor& project, const fs::path& descriptor) {
    const auto name = descriptor.filename().u8string();
    uint64_t hash = 14695981039346656037ull;
    for (const auto ch : name) {
        hash = (hash ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
    }
    for (const auto ch : project.name()) {
        hash = (hash ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
    }
    wchar_t result[17]{};
    swprintf_s(result, L"%016llx", static_cast<unsigned long long>(hash));
    return result;
}

bool collect_files(const fs::path& root, const fs::path& mount, const fs::path& saves,
                   std::set<fs::path>& files, std::string& error) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(mount, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const DWORD attrs = GetFileAttributesW(it->path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return windows_error(error, "Could not inspect a game asset");
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            error = "Cannot export a linked game asset: " + it->path().string();
            return false;
        }
        if (it->path() == saves) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) files.insert(it->path().lexically_relative(root));
    }
    if (!ec) return true;
    error = "Could not enumerate '" + mount.string() + "': " + ec.message();
    return false;
}

bool embed_files(const fs::path& exe, const fs::path& root, const fs::path& descriptor,
                 const fs::path& descriptor_source, const std::set<fs::path>& files,
                 std::wstring& index, std::string& error) {
    HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
    if (!update) return windows_error(error, "Could not open the game executable for export");

    // Windows maps the resources as part of the PE image. Bound the combined
    // data below the Windows image limit and report oversized exports early.
    constexpr uint64_t max_bytes = 1024ull * 1024 * 1024;
    uint64_t total = 0;
    std::size_t file_index = 0;
    for (const auto& relative : files) {
        const auto source = relative == descriptor ? descriptor_source : root / relative;
        std::ifstream file(source, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (size < 0) {
            error = "Could not read game asset '" + source.string() + "'";
            break;
        }
        total += static_cast<uint64_t>(size);
        if (total > max_bytes) {
            error = "Windows EXE export supports up to 1 GiB of game data";
            break;
        }
        index += size == 0 ? L"0" : L"1";
        index += relative.generic_wstring() + L'\n';
        if (size != 0) {
            std::vector<char> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
                error = "Could not read game asset '" + source.string() + "'";
                break;
            }
            if (!UpdateResourceW(update, MAKEINTRESOURCEW(10), game::file_resource_name(file_index).c_str(),
                                 game::kLanguage, bytes.data(), static_cast<DWORD>(bytes.size()))) {
                windows_error(error, "Could not embed a game asset");
                break;
            }
        }
        ++file_index;
    }

    if (error.empty() &&
        !UpdateResourceW(update, MAKEINTRESOURCEW(10), game::kIndex, game::kLanguage,
                         index.data(), static_cast<DWORD>(index.size() * sizeof(wchar_t)))) {
        windows_error(error, "Could not embed the game index");
    }
    if (!EndUpdateResourceW(update, !error.empty()) && error.empty()) {
        return windows_error(error, "Could not finish writing the game executable");
    }
    return error.empty();
}

} // namespace

bool write_windows_game(const BuildOptions& opts, const assets::ProjectDescriptor& project,
                        const std::filesystem::path& output_dir, BuildReport& report,
                        std::string& error) {
    std::error_code ec;
    const auto descriptor = opts.project_file.filename();
    auto exe_name = descriptor;
    exe_name.replace_extension(L".exe");
    if (!game::valid_file_path(exe_name) || !game::valid_file_path(descriptor)) {
        error = "The project filename cannot be used as a Windows executable name";
        return false;
    }
    const auto destination = output_dir / exe_name;
    fs::create_directories(output_dir, ec);
    if (!ec) {
        for (auto it = fs::directory_iterator(output_dir, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->path() != destination || !it->is_regular_file(ec)) {
                error = "Choose an export directory containing only " + exe_name.string() +
                        " or an empty directory";
                return false;
            }
        }
    }
    if (ec) {
        error = "Could not prepare export directory '" + output_dir.string() + "': " + ec.message();
        return false;
    }

    const auto id = game::unique_id();
    if (id.empty()) {
        error = "Could not create a Windows export ID";
        return false;
    }
    const auto staging_path = output_dir.parent_path() / (L".sanad-export-" + id);
    if (!fs::create_directory(staging_path, ec)) {
        error = "Could not create the export staging directory: " + ec.message();
        return false;
    }
    const game::StagingDirectory staging{staging_path};

    const auto exe = staging.path / exe_name;
    if (!fs::copy_file(opts.player_exe, exe, ec)) {
        error = "Could not copy the Windows player template '" + opts.player_exe.string() +
                "': " + ec.message();
        return false;
    }
    HMODULE image = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    if (!image) return windows_error(error, "Could not read the Windows player template");
    const bool valid_template =
        FindResourceW(image, game::kTemplate, MAKEINTRESOURCEW(10)) != nullptr &&
        FindResourceW(image, game::kIndex, MAKEINTRESOURCEW(10)) == nullptr;
    FreeLibrary(image);
    if (!valid_template) {
        error = "The Windows export player template is missing or already contains a game";
        return false;
    }

    std::set<fs::path> files{descriptor};
    const auto root = project.root();
    auto packaged = project;
    const auto saves = project.mount_path("saves://");
    for (const auto& mount : project.mounts()) {
        if (mount.logical == "saves://" || mount.physical == root) continue;
        if (!collect_files(root, mount.physical, saves, files, error)) return false;
    }
    for (const auto& path : files) {
        if (!game::valid_file_path(path)) {
            error = "Invalid Windows game asset path: " + path.string();
            return false;
        }
    }
    packaged.add_mount("saves://", root / "Saves");
    const auto descriptor_source = staging.path / descriptor;
    if (!packaged.save_to_file(descriptor_source, error)) return false;

    std::wstring index = std::wstring(game::kVersion) + L'\n' + id + L'\n' +
                         save_id(project, descriptor) + L'\n' + descriptor.native() + L'\n';
    if (!embed_files(exe, root, descriptor, descriptor_source, files, index, error)) return false;
    if (!MoveFileExW(exe.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return windows_error(error, "Could not replace the exported game executable");
    }

    report.output_dir = output_dir;
    report.game_exe = destination;
    report.files_packaged = files.size();
    return true;
}

} // namespace nf::project
#else
namespace nf::project {
bool write_windows_game(const BuildOptions&, const assets::ProjectDescriptor&,
                        const std::filesystem::path&, BuildReport&, std::string& error) {
    error = "Windows EXE export requires Windows";
    return false;
}
} // namespace nf::project
#endif
