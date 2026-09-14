// NF/Editor/HotReload.cpp — live refresh (see header).

#include <NF/Editor/HotReload.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Core/Hash.hpp>
#include <NF/Runtime/Runtime.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>

namespace nf::editor {

namespace {

std::string fingerprint_of(const std::vector<uint8_t>& bytes) {
    const u64 h = fnv1a_64(bytes.data(), bytes.size());
    char buf[17]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

bool read_physical(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const auto size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    out.resize(size);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        if (!in) {
            return false;
        }
    }
    return true;
}

} // namespace

void HotReload::watch_mesh(const assets::AssetId& id, const std::string& source_logical) {
    for (const auto& m : m_meshes) {
        if (m.id == id) {
            return;
        }
    }
    m_meshes.push_back(MeshWatch{id, source_logical});
}

void HotReload::watch_texture(const std::string& logical_path) {
    if (!contains(m_textures, logical_path)) {
        m_textures.push_back(logical_path);
    }
}

void HotReload::watch_material(const std::string& logical_path) {
    if (!contains(m_materials, logical_path)) {
        m_materials.push_back(logical_path);
    }
}

void HotReload::clear() {
    m_watcher.clear();
    m_meshes.clear();
    m_textures.clear();
    m_materials.clear();
}

void HotReload::refresh(assets::VirtualFileSystem& vfs, const std::string& logical_path) {
    m_watcher.refresh(logical_path, vfs);
}

size_t HotReload::watched_count() const {
    return m_meshes.size() + m_textures.size() + m_materials.size();
}

bool HotReload::contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& e : v) {
        if (e == s) {
            return true;
        }
    }
    return false;
}

std::vector<ReloadResult> HotReload::poll(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg,
                                          runtime::Runtime& runtime) {
    // Ensure every dependency is watched (idempotent), then poll once.
    for (const auto& m : m_meshes) {
        if (!m_watcher.is_watched(m.source)) {
            m_watcher.watch(vfs, m.source);
        }
    }
    for (const auto& t : m_textures) {
        if (!m_watcher.is_watched(t)) {
            m_watcher.watch(vfs, t);
        }
    }
    for (const auto& m : m_materials) {
        if (!m_watcher.is_watched(m)) {
            m_watcher.watch(vfs, m);
        }
    }
    std::vector<ReloadResult> out;
    for (const std::string& changed : m_watcher.poll(vfs)) {
        ReloadResult r;
        r.path = changed;
        // Mesh source?
        const MeshWatch* mesh = nullptr;
        for (const auto& m : m_meshes) {
            if (m.source == changed) {
                mesh = &m;
                break;
            }
        }
        if (mesh != nullptr) {
            r.kind = "mesh";
            // Re-cook: source -> cooked + fingerprint, then live rebuild.
            const assets::AssetMetadata* meta = reg.find(mesh->id);
            if (meta == nullptr) {
                r.message = "no registry entry; skipped";
                out.push_back(r);
                continue;
            }
            auto src = vfs.resolve(changed);
            auto dst = vfs.resolve(meta->cooked_path);
            std::vector<uint8_t> bytes;
            if (!src.ok || !dst.ok || !read_physical(src.value, bytes) || bytes.empty()) {
                r.message = "source unreadable; kept last good copy";
                out.push_back(r);
                continue;
            }
            // Validate before touching anything live.
            std::string perr;
            if (!assets::MeshAsset::load_from_bytes(std::span<const uint8_t>(bytes), perr)) {
                r.message = std::string("invalid mesh, kept last good copy: ") + perr;
                out.push_back(r);
                continue;
            }
            std::error_code ec;
            std::filesystem::create_directories(dst.value.parent_path(), ec);
            {
                std::ofstream o(dst.value, std::ios::binary | std::ios::trunc);
                if (!o) {
                    r.message = "cooked write failed; kept last good copy";
                    out.push_back(r);
                    continue;
                }
                o.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                if (!o) {
                    r.message = "cooked write failed; kept last good copy";
                    out.push_back(r);
                    continue;
                }
            }
            if (auto* mut = reg.find(mesh->id)) {
                mut->fingerprint = fingerprint_of(bytes);
            }
            std::string rerr;
            if (!runtime.hot_reload_mesh(mesh->id, rerr)) {
                r.message = "rebuild failed (" + rerr + "); kept last good copy";
                out.push_back(r);
                continue;
            }
            r.ok = true;
            r.message = "rebuilt live";
            out.push_back(r);
            continue;
        }
        if (contains(m_textures, changed)) {
            r.kind = "texture";
            std::string terr;
            if (!runtime.reload_texture(changed, terr)) {
                r.message = "kept last good copy (" + terr + ")";
                out.push_back(r);
                continue;
            }
            r.ok = true;
            r.message = "rebuilt live";
            out.push_back(r);
            continue;
        }
        if (contains(m_materials, changed)) {
            r.kind = "material";
            std::string merr;
            if (!runtime.reload_material_file(changed, merr)) {
                r.message = merr; // includes the dirty-skip case
                out.push_back(r);
                continue;
            }
            r.ok = true;
            r.message = "rebuilt live";
            out.push_back(r);
            continue;
        }
        r.kind = "unknown";
        r.message = "watched but unclassified; ignored";
        out.push_back(r);
    }
    return out;
}

} // namespace nf::editor
