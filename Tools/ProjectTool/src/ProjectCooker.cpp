#include <NF/Project/ProjectCooker.hpp>

#include <NF/Assets/MeshAsset.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace nf::project {

namespace {

constexpr std::string_view kContentMount = "content://";

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Extensions the cooker processes. GLSL (.frag/.vert) is deliberately absent:
// it is compiled to SPIR-V by the build, not copied by the cooker, and shipping
// the .spv is packaging's job. Listing it here would produce a registry entry
// pointing at a cooked file the cooker never writes.
constexpr std::string_view kCookableExtensions[] = {
    ".nfmesh", ".nfmat", ".nfscene", ".png", ".jpg", ".jpeg", ".bmp", ".tga",
};

// The registry file itself lives under content:// and must not be cooked.
constexpr std::string_view kRegistryFileName = "AssetRegistry.nfreg";

std::string extension_of(std::string_view path) {
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of("/\\");
    if (dot == std::string_view::npos) return {};
    if (slash != std::string_view::npos && dot < slash) return {};
    return to_lower(path.substr(dot));
}

bool read_whole_file(const std::filesystem::path& p, std::vector<u8>& out, std::string& out_error) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        out_error = "failed to open file: '" + p.string() + "'";
        return false;
    }
    const auto size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    out.resize(size);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        if (!in) {
            out_error = "failed to read file: '" + p.string() + "'";
            return false;
        }
    }
    return true;
}

// Replace forward slashes' native form with '/' so a registry written on
// Windows still matches on any host.
std::string generic(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

} // namespace

std::string fingerprint_bytes(const void* data, size_t size) {
    uint64_t hash = 14695981039346656037ULL;
    const auto* bytes = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

std::string cooked_path_for(std::string_view logical_path, std::string_view cache_mount) {
    if (!logical_path.starts_with(kContentMount)) {
        // Already somewhere else (cache://, project://): leave it alone rather
        // than guessing a mapping the caller did not ask for.
        return std::string(logical_path);
    }
    const std::string rest = generic(std::string(logical_path.substr(kContentMount.size())));
    std::string prefix(cache_mount);
    // Accept both a bare mount ("cache://") and a mount with a path
    // ("cache://Packed"). Without the separator check the second form silently
    // produces "cache://PackedM/x.nfmesh".
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    return prefix + rest;
}

bool is_cookable(std::string_view extension) {
    const std::string ext = to_lower(extension);
    for (auto e : kCookableExtensions) {
        if (ext == e) return true;
    }
    return false;
}

assets::AssetType asset_type_for(std::string_view extension) {
    const std::string ext = to_lower(extension);
    if (ext == ".nfmesh") return assets::AssetType::Mesh;
    if (ext == ".nfscene") return assets::AssetType::Scene;
    if (ext == ".nfmat") return assets::AssetType::Material;
    if (ext == ".spv") return assets::AssetType::Shader;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
        return assets::AssetType::Texture;
    }
    return assets::AssetType::Unknown;
}

std::string format_for(assets::AssetType type) {
    switch (type) {
        case assets::AssetType::Mesh:     return "nfmesh-v1";
        case assets::AssetType::Texture:  return "img-v1";
        case assets::AssetType::Shader:   return "spv-v1";
        case assets::AssetType::Scene:    return "nfscene-v1";
        case assets::AssetType::Material: return "nfmat-v1";
        default:                          return "unknown-v1";
    }
}

bool cook_one(assets::VirtualFileSystem& vfs,
              assets::AssetRegistry& registry,
              const std::string& input,
              const std::string& output,
              std::string& out_error,
              bool& out_skipped) {
    out_skipped = false;

    auto in_resolve = vfs.resolve(input);
    if (!in_resolve.ok) {
        out_error = "failed to resolve input '" + input + "': " + in_resolve.error;
        return false;
    }
    if (!std::filesystem::exists(in_resolve.value)) {
        out_error = "input does not exist: '" + input + "'";
        return false;
    }

    std::vector<u8> bytes;
    if (!read_whole_file(in_resolve.value, bytes, out_error)) {
        return false;
    }
    const std::string fingerprint = fingerprint_bytes(bytes.data(), bytes.size());

    const assets::AssetType type = asset_type_for(extension_of(input));
    if (type == assets::AssetType::Unknown) {
        out_error = "unknown asset type for '" + input + "'";
        return false;
    }

    // Skip when the source is unchanged and the cooked file is still there. The
    // second cook of a project must do no work, which is what makes `nf build`
    // usable in a loop.
    const assets::AssetMetadata* existing = registry.find_by_path(input);
    if (existing != nullptr && existing->fingerprint == fingerprint) {
        auto cooked_exists = vfs.exists(output);
        if (cooked_exists.ok && cooked_exists.value) {
            out_skipped = true;
            return true;
        }
    }

    // A mesh that cannot be parsed must not be published: a corrupt cooked asset
    // is worse than a missing one, because it fails at load time in the game.
    if (type == assets::AssetType::Mesh) {
        std::string parse_err;
        auto asset = assets::MeshAsset::load_from_bytes(std::span<const u8>(bytes), parse_err);
        if (!asset) {
            out_error = "input is not a valid .nfmesh: " + parse_err;
            return false;
        }
    }

    auto write_res = vfs.write_bytes(output, std::span<const u8>(bytes));
    if (!write_res.ok) {
        out_error = "failed to write '" + output + "': " + write_res.error;
        return false;
    }

    assets::AssetMetadata meta;
    if (existing != nullptr) {
        // Preserve the AssetId across re-cooks: stable identity is the whole
        // point of the registry, and scenes reference it.
        meta = *existing;
        meta.fingerprint = fingerprint;
        meta.cooked_path = output;
        registry.remove(existing->id);
    } else {
        meta.id = assets::AssetId::generate();
        meta.type = type;
        meta.logical_path = input;
        meta.cooked_path = output;
        meta.fingerprint = fingerprint;
        meta.format = format_for(type);
        meta.version = 1;
    }
    std::string add_err;
    if (!registry.add(meta, add_err)) {
        out_error = "failed to update registry: " + add_err;
        return false;
    }
    return true;
}

bool cook_all(assets::VirtualFileSystem& vfs,
              assets::AssetRegistry& registry,
              const std::string& content_mount,
              const std::string& cache_mount,
              CookReport& out_report,
              std::string& out_error) {
    out_report = CookReport{};

    auto root = vfs.resolve(content_mount);
    if (!root.ok) {
        out_error = "cannot resolve content mount '" + content_mount + "': " + root.error;
        return false;
    }
    if (!std::filesystem::exists(root.value)) {
        out_error = "content root does not exist: '" + root.value.string() + "'";
        return false;
    }

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root.value, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;

        const std::filesystem::path file = it->path();
        if (generic(file.filename().string()) == kRegistryFileName) continue;
        if (!is_cookable(extension_of(file.string()))) continue;

        std::error_code rel_ec;
        auto rel = std::filesystem::relative(file, root.value, rel_ec);
        if (rel_ec) continue;

        const std::string logical = content_mount + generic(rel.generic_string());
        const std::string cooked = cooked_path_for(logical, cache_mount);

        std::string cook_err;
        bool skipped = false;
        if (cook_one(vfs, registry, logical, cooked, cook_err, skipped)) {
            if (skipped) {
                ++out_report.skipped;
            } else {
                ++out_report.cooked;
            }
        } else {
            ++out_report.failed;
            // Keep the message: a bare count tells the user nothing about which
            // asset is broken.
            out_error += std::string(out_error.empty() ? "" : "\n") + logical + ": " + cook_err;
        }
    }

    // A failure to enumerate is a hard error; individual cook failures are not.
    if (ec) {
        out_error += std::string(out_error.empty() ? "" : "\n") +
                     "directory enumeration failed: " + ec.message();
        return false;
    }

    // Prune: a registry entry whose source asset is gone must not survive. The
    // walk above only visits files that exist, so without this a deleted asset
    // keeps its registry entry and its cooked output, and the next package ships
    // data the project no longer contains.
    std::vector<assets::AssetId> stale;
    for (const auto& [id, meta] : registry.entries()) {
        if (!meta.logical_path.starts_with(content_mount)) continue;
        auto source = vfs.exists(meta.logical_path);
        if (source.ok && source.value) continue;
        stale.push_back(id);
    }
    for (const auto& id : stale) {
        const auto* meta = registry.find(id);
        if (meta != nullptr) {
            std::error_code del_ec;
            auto cooked = vfs.resolve(meta->cooked_path);
            if (cooked.ok) {
                std::filesystem::remove(cooked.value, del_ec);
            }
        }
        registry.remove(id);
        ++out_report.pruned;
    }

    return true;
}

bool verify_registry(assets::VirtualFileSystem& vfs,
                     const assets::AssetRegistry& registry,
                     VerifyReport& out_report,
                     std::string& out_error) {
    out_report = VerifyReport{};
    out_report.total = registry.size();
    out_error.clear();

    for (const auto& [id, meta] : registry.entries()) {
        (void)id;
        auto exists = vfs.exists(meta.cooked_path);
        if (!exists.ok || !exists.value) {
            ++out_report.missing;
            out_error += std::string(out_error.empty() ? "" : "\n") + "missing cooked file for '" +
                         meta.logical_path + "' -> '" + meta.cooked_path + "'";
            continue;
        }
        if (meta.type == assets::AssetType::Mesh) {
            auto data = vfs.read_bytes(meta.cooked_path);
            if (!data.ok) {
                ++out_report.bad_format;
                out_error += std::string(out_error.empty() ? "" : "\n") + "cannot read '" + meta.cooked_path +
                             "': " + data.error;
                continue;
            }
            std::string parse_err;
            auto asset = assets::MeshAsset::load_from_bytes(std::span<const u8>(data.value), parse_err);
            if (!asset) {
                ++out_report.bad_format;
                out_error += std::string(out_error.empty() ? "" : "\n") + "corrupt .nfmesh '" +
                             meta.cooked_path + "': " + parse_err;
                continue;
            }
        }
        ++out_report.ok_count;
    }
    return out_report.ok();
}

} // namespace nf::project
