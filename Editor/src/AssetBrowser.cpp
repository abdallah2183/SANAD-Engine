#include <NF/Editor/AssetBrowser.hpp>
#include <NF/Editor/Commands.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace nf::editor {

namespace {

std::string to_lower_str(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

std::vector<AssetEntry> list_content_assets(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg) {
    std::vector<AssetEntry> out;
    for (const auto& kv : reg.entries()) {
        const assets::AssetMetadata& meta = kv.second;
        if (meta.logical_path.rfind("content://", 0) != 0) {
            continue;
        }
        AssetEntry e;
        e.logical_path = meta.logical_path;
        e.type = meta.type;
        e.id = meta.id;
        e.has_id = meta.id.valid();
        e.cooked_path = meta.cooked_path;
        if (!meta.cooked_path.empty()) {
            auto ex = vfs.exists(meta.cooked_path);
            e.has_cooked = ex.ok && ex.value;
        }
        out.push_back(e);
    }
    // Scene files live on disk but are not registry assets: scan content://.
    // NOTE: the relative path is stripped lexically from the same base string
    // the iterator was seeded with (guaranteed prefix), never via
    // std::filesystem::relative(), whose canonicalization can disagree with
    // the mount path on Windows and silently drop every file.
    auto resolved = vfs.resolve("content://");
    if (resolved.ok) {
        const std::string base = resolved.value.generic_string();
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(resolved.value, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec) || ec) {
                continue;
            }
            std::string ext = it->path().extension().string();
            for (char& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool is_scene = (ext == ".nfscene");
            const bool is_material = (ext == ".nfmat");
            const bool is_script = (ext == ".lua");
            const bool is_texture =
                (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga");
            if (!is_scene && !is_material && !is_texture && !is_script) {
                continue;
            }
            const std::string full = it->path().generic_string();
            if (full.size() <= base.size() || full.compare(0, base.size(), base) != 0) {
                continue;
            }
            std::string rel = full.substr(base.size());
            while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
                rel.erase(rel.begin());
            }
            if (rel.empty()) {
                continue;
            }
            const std::string logical = "content://" + rel;
            bool known = false;
            for (const auto& e : out) {
                if (e.logical_path == logical) {
                    known = true;
                    break;
                }
            }
            if (known) {
                continue;
            }
            AssetEntry e;
            e.logical_path = logical;
            e.type = is_material  ? assets::AssetType::Material
                      : is_script ? assets::AssetType::Script
                      : (is_texture ? assets::AssetType::Texture : assets::AssetType::Scene);
            e.is_scene_file = !is_material && !is_texture && !is_script;
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(), [](const AssetEntry& a, const AssetEntry& b) {
        return a.logical_path < b.logical_path;
    });
    return out;
}

std::vector<AssetEntry> filter_assets(const std::vector<AssetEntry>& entries, const std::string& text,
                                      int type_filter) {
    const std::string needle = to_lower_str(text);
    std::vector<AssetEntry> out;
    for (const auto& e : entries) {
        if (type_filter >= 0 && static_cast<int>(e.type) != type_filter) {
            continue;
        }
        if (!needle.empty() && to_lower_str(e.logical_path).find(needle) == std::string::npos) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

AssetOpenAction classify_double_click(const AssetEntry& entry, std::string& out_info) {
    if (entry.type == assets::AssetType::Scene || ends_with(entry.logical_path, ".nfscene")) {
        out_info = entry.logical_path;
        return AssetOpenAction::OpenScene;
    }
    if (entry.type == assets::AssetType::Mesh) {
        out_info = "Mesh " + entry.logical_path + " id=" + (entry.has_id ? entry.id.to_string() : "<none>") +
                   (entry.has_cooked ? " cooked=" + entry.cooked_path : " (no cooked asset)");
        return AssetOpenAction::ShowMeshInfo;
    }
    if (entry.type == assets::AssetType::Material || ends_with(entry.logical_path, ".nfmat")) {
        out_info = entry.logical_path;
        return AssetOpenAction::ShowMaterialInfo;
    }
    if (entry.type == assets::AssetType::Texture) {
        out_info = entry.logical_path;
        return AssetOpenAction::ShowTextureInfo;
    }
    // Scripts open in the external IDE (Visual Studio / VS Code / shell
    // default) — double-click used to do nothing at all for them.
    if (entry.type == assets::AssetType::Script || ends_with(entry.logical_path, ".lua")) {
        out_info = entry.logical_path;
        return AssetOpenAction::OpenInIde;
    }
    out_info = entry.logical_path;
    return AssetOpenAction::None;
}

std::unique_ptr<ICommand> make_drop_mesh_command(const AssetEntry& entry, const std::string& entity_name,
                                                 ecs::Entity parent, std::string& out_err) {
    if (entry.type != assets::AssetType::Mesh || !entry.has_id || !entry.id.valid()) {
        out_err = "Only mesh assets with a valid AssetId can be dropped into the viewport";
        return nullptr;
    }
    runtime::MeshComponent mesh;
    mesh.mesh_id = entry.id;
    // Derive a readable material tag from the mesh path (v0.1 convention).
    mesh.material = "content://Materials/Default";
    std::string name = entity_name.empty() ? "Mesh" : entity_name;
    return std::make_unique<CreateMeshEntityCommand>(name, parent, mesh);
}

std::string mount_of(const std::string& logical_path) {
    for (const char* mount : {"content://", "project://"}) {
        if (logical_path.rfind(mount, 0) == 0) {
            return mount;
        }
    }
    return {};
}

std::string asset_folder_of(const std::string& logical_path) {
    const std::string mount = mount_of(logical_path);
    if (mount.empty()) {
        return {};
    }
    const size_t slash = logical_path.find_last_of('/');
    if (slash == std::string::npos || slash < mount.size()) {
        return mount;
    }
    return logical_path.substr(0, slash);
}

std::vector<std::string> asset_folders(const std::vector<AssetEntry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) {
        const std::string folder = asset_folder_of(e.logical_path);
        if (folder.empty()) {
            continue;
        }
        if (std::find(out.begin(), out.end(), folder) == out.end()) {
            out.push_back(folder);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<AssetEntry> assets_in_folder(const std::vector<AssetEntry>& entries,
                                         const std::string& folder) {
    std::vector<AssetEntry> out;
    for (const auto& e : entries) {
        if (asset_folder_of(e.logical_path) == folder) {
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(), [](const AssetEntry& a, const AssetEntry& b) {
        return a.logical_path < b.logical_path;
    });
    return out;
}

std::vector<std::string> subfolders_in_folder(const std::vector<std::string>& folders,
                                              const std::string& folder) {
    const std::string prefix = folder.empty() ? std::string{} : folder + "/";
    std::vector<std::string> out;
    for (const auto& f : folders) {
        if (f.size() <= prefix.size() || f.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::string rest = f.substr(prefix.size());
        if (rest.empty() || rest.find('/') != std::string::npos) {
            continue;
        }
        out.push_back(f);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string parent_folder_of(const std::string& folder) {
    if (folder.empty()) {
        return {};
    }
    // A mount root ("content://") carries no interior slash: Up stays put.
    const size_t slash = folder.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= folder.size()) {
        return folder;
    }
    // Both mounts are 10 chars ("content://" / "project://"); a slash at or
    // before that is the root's own trailing slash.
    if (slash <= 9) {
        return folder.substr(0, 10);
    }
    return folder.substr(0, slash);
}

std::string folder_display_name(const std::string& folder) {
    if (folder.empty()) {
        return {};
    }
    const size_t slash = folder.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= folder.size()) {
        return folder;
    }
    return folder.substr(slash + 1);
}

bool is_root_folder(const std::string& folder) {
    return folder.empty() || folder == "content://" || folder == "project://";
}

bool valid_asset_folder(const std::string& logical_dir, std::string& out_err) {
    if (logical_dir.empty()) {
        out_err = "Folder path is empty";
        return false;
    }
    if (mount_of(logical_dir).empty()) {
        out_err = "Folder must start with content:// or project://";
        return false;
    }
    if (logical_dir.back() == '/') {
        out_err = "Folder must not end with '/'";
        return false;
    }
    if (logical_dir.find(' ') != std::string::npos) {
        out_err = "Folder must contain no spaces";
        return false;
    }
    return true;
}

// --- FileSystem dock model ---------------------------------------------------

std::string file_name_of(const std::string& logical_path) {
    const size_t slash = logical_path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return logical_path;
    }
    if (slash + 1 >= logical_path.size()) {
        return {};
    }
    return logical_path.substr(slash + 1);
}

std::string file_extension_lower(const std::string& logical_path) {
    const std::string name = file_name_of(logical_path);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return {};
    }
    std::string ext = name.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

std::string file_stem_of(const std::string& logical_path) {
    const std::string name = file_name_of(logical_path);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return name;
    }
    return name.substr(0, dot);
}

AssetIconKind asset_icon_for_folder() {
    return AssetIconKind::Folder;
}

AssetIconKind asset_icon_of(const AssetEntry& entry) {
    if (is_prefab_path(entry.logical_path)) {
        return AssetIconKind::Prefab;
    }
    switch (entry.type) {
        case assets::AssetType::Mesh: return AssetIconKind::Mesh;
        case assets::AssetType::Scene: return AssetIconKind::Scene;
        case assets::AssetType::Material: return AssetIconKind::Material;
        case assets::AssetType::Texture: return AssetIconKind::Texture;
        case assets::AssetType::Script: return AssetIconKind::Script;
        case assets::AssetType::Shader: return AssetIconKind::Shader;
        default: break;
    }
    // Fall back to extension so on-disk-only files (project scan has no
    // registry type for some) still get a sensible icon.
    const std::string ext = file_extension_lower(entry.logical_path);
    if (ext == ".nfmesh" || ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
        return AssetIconKind::Mesh;
    }
    if (ext == ".nfscene" || ext == ".tscn" || ext == ".scn") {
        return AssetIconKind::Scene;
    }
    if (ext == ".nfmat" || ext == ".mat") {
        return AssetIconKind::Material;
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" ||
        ext == ".svg" || ext == ".webp") {
        return AssetIconKind::Texture;
    }
    if (ext == ".lua" || ext == ".gd" || ext == ".cs" || ext == ".js" || ext == ".py") {
        return AssetIconKind::Script;
    }
    if (ext == ".hlsl" || ext == ".glsl" || ext == ".spv" || ext == ".shader") {
        return AssetIconKind::Shader;
    }
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
        return AssetIconKind::Audio;
    }
    return AssetIconKind::Unknown;
}

bool is_favorite(const AssetBrowserState& state, const std::string& folder) {
    return std::find(state.favorites.begin(), state.favorites.end(), folder) !=
           state.favorites.end();
}

void add_favorite(AssetBrowserState& state, const std::string& folder) {
    if (folder.empty() || is_favorite(state, folder)) {
        return;
    }
    state.favorites.push_back(folder);
    std::sort(state.favorites.begin(), state.favorites.end());
}

void remove_favorite(AssetBrowserState& state, const std::string& folder) {
    state.favorites.erase(std::remove(state.favorites.begin(), state.favorites.end(), folder),
                          state.favorites.end());
}

void toggle_favorite(AssetBrowserState& state, const std::string& folder) {
    if (is_favorite(state, folder)) {
        remove_favorite(state, folder);
    } else {
        add_favorite(state, folder);
    }
}

void browser_navigate_to(AssetBrowserState& state, const std::string& folder) {
    if (state.current_folder == folder) {
        return;
    }
    // Cap history so a long session cannot grow it without bound.
    if (state.back_stack.size() > 100) {
        state.back_stack.erase(state.back_stack.begin());
    }
    state.back_stack.push_back(state.current_folder);
    state.current_folder = folder;
    state.forward_stack.clear();
}

bool browser_can_go_back(const AssetBrowserState& state) {
    return !state.back_stack.empty();
}

bool browser_can_go_forward(const AssetBrowserState& state) {
    return !state.forward_stack.empty();
}

bool browser_go_back(AssetBrowserState& state) {
    if (state.back_stack.empty()) {
        return false;
    }
    state.forward_stack.push_back(state.current_folder);
    state.current_folder = state.back_stack.back();
    state.back_stack.pop_back();
    return true;
}

bool browser_go_forward(AssetBrowserState& state) {
    if (state.forward_stack.empty()) {
        return false;
    }
    state.back_stack.push_back(state.current_folder);
    state.current_folder = state.forward_stack.back();
    state.forward_stack.pop_back();
    return true;
}

std::vector<std::string> breadcrumb_parts(const std::string& folder) {
    const std::string mount = mount_of(folder);
    if (mount.empty()) {
        return {folder};
    }
    std::vector<std::string> parts;
    parts.push_back(mount);
    std::string rest = folder.substr(mount.size());
    while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) {
        rest.erase(rest.begin());
    }
    std::string cur;
    for (char c : rest) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        parts.push_back(cur);
    }
    return parts;
}

std::string breadcrumb_path_at(const std::string& folder, size_t index) {
    const auto parts = breadcrumb_parts(folder);
    if (parts.empty() || index >= parts.size()) {
        return folder;
    }
    // parts[0] is the mount ("content://" / "project://") which already ends
    // with '/': appending the segment directly yields "content://Meshes".
    // Deeper segments need one '/' separator each.
    std::string out = parts[0];
    for (size_t i = 1; i <= index; ++i) {
        if (!out.empty() && out.back() != '/') {
            out.push_back('/');
        }
        out += parts[i];
    }
    return out;
}

// --- UX2 item E3: the OPEN PROJECT's files -----------------------------------
//
// The browser used to list content:// unconditionally, so an author opening their
// own game saw the engine's sample content and none of their work. With a project
// open the panel is rooted at project:// instead. Kept beside
// list_content_assets because it is the same scan with a different mount and a
// different extension set — if the scan logic changes, both must change.

assets::AssetType project_asset_type_for(const std::string& extension) {
    std::string ext;
    ext.reserve(extension.size());
    for (char c : extension) {
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (ext == ".nfmesh") {
        return assets::AssetType::Mesh;
    }
    if (ext == ".nfmat") {
        return assets::AssetType::Material;
    }
    if (ext == ".nfscene") {
        return assets::AssetType::Scene;
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
        return assets::AssetType::Texture;
    }
    if (ext == ".lua") {
        return assets::AssetType::Script;
    }
    return assets::AssetType::Unknown;
}

bool is_prefab_path(const std::string& logical_path) {
    // Prefabs are ordinary .nfscene files living under a Prefabs/ directory, so
    // the path is the only thing that distinguishes them. Checked with both
    // separators because a logical path can carry either.
    return logical_path.find("Prefabs/") != std::string::npos ||
           logical_path.find("Prefabs\\") != std::string::npos;
}

std::vector<AssetEntry> list_project_assets(assets::VirtualFileSystem& vfs) {
    std::vector<AssetEntry> out;
    auto resolved = vfs.resolve("project://");
    if (!resolved.ok) {
        return out; // no project mounted: an empty listing, not an error
    }
    const std::string base = resolved.value.generic_string();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(resolved.value, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        // Any dot-directory is tooling/VCS state (".git", ".kilo", ".vs",
        // ".idea", ...), never authored content. A named blocklist could not
        // keep up: one tool directory that is not on it leaks into the listing
        // and into a packaged game, and a real one did -- a worktree under
        // ".kilo/worktrees/<name>/Content/Meshes/cube.nfmesh" was listed as
        // project content and sorted ahead of the genuine Content/ cube, so a
        // drag-and-drop grabbed the tooling copy, which carries no AssetId and
        // was refused. The rule is structural instead: a leading dot means
        // hidden.
        //
        // This must come BEFORE the is_regular_file() test below. A directory
        // entry is not a regular file, so that test `continue`s first and the
        // iterator still descends into the dot-directory -- which is exactly
        // what let the worktree through.
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            if (it->is_directory(ec) && !ec) {
                it.disable_recursion_pending(); // do not walk it at all
            }
            continue;
        }
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }
        // Skip the build output and any generated copy: a packaged game
        // is not the author's source, and listing it buries the real assets.
        // Cache/ holds the cooked copies the registry already points at: listing
        // them would show every mesh twice, with the cooked copy sorting first
        // and carrying no AssetId, so a drag-and-drop would grab the wrong one.
        const std::string dir = it->path().parent_path().generic_string();
        if (dir.find("/dist") != std::string::npos || dir.find("/.git") != std::string::npos ||
            dir.find("/build") != std::string::npos || dir.find("/Cache") != std::string::npos) {
            continue;
        }
        const assets::AssetType type = project_asset_type_for(it->path().extension().string());
        if (type == assets::AssetType::Unknown) {
            continue;
        }
        // Same lexical strip as list_content_assets: never std::filesystem::
        // relative(), whose canonicalization can disagree with the mount path on
        // Windows and silently drop every file.
        const std::string full = it->path().generic_string();
        if (full.size() <= base.size() || full.compare(0, base.size(), base) != 0) {
            continue;
        }
        std::string rel = full.substr(base.size());
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
            rel.erase(rel.begin());
        }
        if (rel.empty()) {
            continue;
        }
        AssetEntry e;
        e.logical_path = "project://" + rel;
        e.type = type;
        e.is_scene_file = (type == assets::AssetType::Scene);
        out.push_back(std::move(e));
    }
    // Deterministic order: directory iteration order is not stable across
    // machines or runs, and the listing is user-visible.
    std::sort(out.begin(), out.end(), [](const AssetEntry& a, const AssetEntry& b) {
        return a.logical_path < b.logical_path;
    });
    return out;
}

} // namespace nf::editor
