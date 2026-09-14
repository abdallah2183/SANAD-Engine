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

} // namespace nf::editor
