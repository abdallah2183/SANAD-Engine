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
            const bool is_texture =
                (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga");
            if (!is_scene && !is_material && !is_texture) {
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
            e.type = is_material ? assets::AssetType::Material
                                 : (is_texture ? assets::AssetType::Texture : assets::AssetType::Scene);
            e.is_scene_file = !is_material && !is_texture;
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
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }
        // Skip the build output and any VCS/tooling directory: a packaged game
        // is not the author's source, and listing it buries the real assets.
        const std::string dir = it->path().parent_path().generic_string();
        if (dir.find("/dist") != std::string::npos || dir.find("/.git") != std::string::npos ||
            dir.find("/build") != std::string::npos) {
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
