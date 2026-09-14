#include <NF/Assets/VirtualFileSystem.hpp>

#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <fstream>

namespace nf::assets {

static std::string to_forward_slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

static std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}



VFSResult<void> VirtualFileSystem::mount(std::string_view logical, const std::filesystem::path& physical) {
    std::string log_str(logical);
    if (log_str.find("://") == std::string::npos) {
        return VFSResult<void>::failure("Mount logical must contain '://', got '" + log_str + "'");
    }
    if (!log_str.ends_with("://") && log_str.find("://") != log_str.size()-3) {
        // Allow "content://", "content://some/path" is not a mount, only "content://"
        // For v0.1 we only support mounts at the scheme level: "engine://", "project://", etc.
        // So we require exactly "xxx://"
        if (log_str.find('/', log_str.find("://")+3) != std::string::npos) {
            return VFSResult<void>::failure("Mount logical must be exactly 'scheme://', got '" + log_str + "'");
        }
    }
    // Ensure logical ends with ://
    if (!log_str.ends_with("://")) {
        // If it's "content://", it's already correct; if it's "content:///", normalize?
        // For now, require "://"
        return VFSResult<void>::failure("Mount logical must end with '://'");
    }

    std::error_code ec;
    std::filesystem::path phys = physical;
    // Create the directory if it doesn't exist
    std::filesystem::create_directories(phys, ec);
    if (ec) {
        return VFSResult<void>::failure("Failed to create mount directory '" + phys.string() + "': " + ec.message());
    }
    // Canonicalize physical
    std::filesystem::path canonical = std::filesystem::weakly_canonical(phys, ec);
    if (ec) canonical = std::filesystem::absolute(phys, ec);
    if (ec) canonical = phys;

    // Check for duplicate mount
    for (auto& m : m_mounts) if (m.logical == log_str) {
        return VFSResult<void>::failure("Mount already exists for '" + log_str + "'");
    }

    m_mounts.push_back({log_str, canonical});
    // Sort by longest logical first for resolve (so more specific mounts win, though for v0.1 all are scheme://)
    std::sort(m_mounts.begin(), m_mounts.end(), [](const Mount& a, const Mount& b){ return a.logical.size() > b.logical.size(); });
    return VFSResult<void>::success();
}

VFSResult<std::filesystem::path> VirtualFileSystem::resolve(std::string_view logical_path) const {
    return resolve_internal(logical_path);
}

VFSResult<std::filesystem::path> VirtualFileSystem::resolve_internal(std::string_view logical_path) const {
    std::string path_str(logical_path);
    // Find mount
    const Mount* matched = nullptr;
    std::string relative;
    for (auto& m : m_mounts) {
        if (path_str.rfind(m.logical, 0) == 0) {
            matched = &m;
            relative = path_str.substr(m.logical.size());
            break;
        }
    }
    if (!matched) {
        return VFSResult<std::filesystem::path>::failure("No mount found for '" + path_str + "'");
    }

    // Normalize relative path: handle Windows backslashes, ., .., and forbid traversal outside mount
    std::string rel_forward = to_forward_slashes(relative);
    // Remove leading slash if any
    if (!rel_forward.empty() && rel_forward.front()=='/') rel_forward.erase(0,1);

    auto parts = split_path(rel_forward);
    // Check for any ".." that would escape
    std::vector<std::string> normalized_parts;
    for (auto& p : parts) {
        if (p == "..") {
            if (normalized_parts.empty()) {
                return VFSResult<std::filesystem::path>::failure("Path traversal detected (..) in '" + path_str + "'");
            }
            normalized_parts.pop_back();
        } else if (p == "." || p.empty()) {
            continue;
        } else {
            // Forbid any part containing ":" or "\0" or other invalid chars? For now, just forbid ".." already handled
            // Also forbid absolute Windows paths like "C:/"
            if (p.size()>=2 && p[1]==':') {
                return VFSResult<std::filesystem::path>::failure("Absolute path component not allowed in '" + path_str + "'");
            }
            normalized_parts.push_back(p);
        }
    }
    std::string normalized_rel;
    for (size_t i=0;i<normalized_parts.size();++i){
        if (i) normalized_rel += "/";
        normalized_rel += normalized_parts[i];
    }

    std::filesystem::path physical = matched->physical;
    if (!normalized_rel.empty()) physical /= normalized_rel;
    if (normalized_parts.empty()) {
        // The mount root itself ("content://"): it is trivially within the
        // mount. (Previously this fell into the file-parent check below and
        // was wrongly rejected as escaping.)
        return VFSResult<std::filesystem::path>::success(physical);
    }

    // Ensure the resolved path is still within the mount (canonical check)
    std::error_code ec;
    std::filesystem::path canonical_physical = std::filesystem::weakly_canonical(matched->physical, ec);
    if (ec) canonical_physical = matched->physical;
    std::filesystem::path canonical_target = std::filesystem::weakly_canonical(physical, ec);
    if (ec) canonical_target = std::filesystem::absolute(physical, ec);
    // Simple prefix check: canonical_target must start with canonical_physical
    std::string canon_phys_str = to_forward_slashes(canonical_physical.string());
    std::string canon_target_str = to_forward_slashes(canonical_target.string());
    // Normalize both to have trailing slash for directory check
    if (!canon_phys_str.ends_with("/")) canon_phys_str += "/";
    if (!canon_target_str.ends_with("/")) {
        // For file, check its parent is within
        std::string parent = to_forward_slashes(canonical_target.parent_path().string());
        if (!parent.ends_with("/")) parent += "/";
        if (parent.rfind(canon_phys_str, 0) != 0 && canon_target_str.rfind(canon_phys_str, 0) != 0) {
            return VFSResult<std::filesystem::path>::failure("Resolved path escapes mount root for '" + path_str + "'");
        }
    } else {
        if (canon_target_str.rfind(canon_phys_str, 0) != 0) {
            return VFSResult<std::filesystem::path>::failure("Resolved path escapes mount root for '" + path_str + "'");
        }
    }

    return VFSResult<std::filesystem::path>::success(physical);
}

VFSResult<bool> VirtualFileSystem::exists(std::string_view logical_path) const {
    auto r = resolve_internal(logical_path);
    if (!r.ok) return VFSResult<bool>::failure(r.error);
    std::error_code ec;
    bool ex = std::filesystem::exists(r.value, ec);
    if (ec) return VFSResult<bool>::failure(ec.message());
    return VFSResult<bool>::success(ex);
}

VFSResult<std::vector<u8>> VirtualFileSystem::read_bytes(std::string_view logical_path) const {
    auto r = resolve_internal(logical_path);
    if (!r.ok) return VFSResult<std::vector<u8>>::failure(r.error);
    std::ifstream file(r.value, std::ios::binary);
    if (!file) return VFSResult<std::vector<u8>>::failure("Failed to open file for reading: '" + std::string(logical_path) + "'");
    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    if (size>0) {
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!file) return VFSResult<std::vector<u8>>::failure("Failed to read file: '" + std::string(logical_path) + "'");
    }
    return VFSResult<std::vector<u8>>::success(std::move(data));
}

VFSResult<std::string> VirtualFileSystem::read_text(std::string_view logical_path) const {
    auto r = read_bytes(logical_path);
    if (!r.ok) return VFSResult<std::string>::failure(r.error);
    return VFSResult<std::string>::success(std::string(reinterpret_cast<const char*>(r.value.data()), r.value.size()));
}

VFSResult<void> VirtualFileSystem::write_bytes(std::string_view logical_path, std::span<const u8> data) {
    auto r = resolve_internal(logical_path);
    if (!r.ok) return VFSResult<void>::failure(r.error);
    std::error_code ec;
    std::filesystem::create_directories(r.value.parent_path(), ec);
    if (ec) return VFSResult<void>::failure("Failed to create directories for '" + std::string(logical_path) + "': " + ec.message());
    std::ofstream file(r.value, std::ios::binary | std::ios::trunc);
    if (!file) return VFSResult<void>::failure("Failed to open file for writing: '" + std::string(logical_path) + "'");
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) return VFSResult<void>::failure("Failed to write file: '" + std::string(logical_path) + "'");
    }
    return VFSResult<void>::success();
}

VFSResult<void> VirtualFileSystem::write_text(std::string_view logical_path, std::string_view text) {
    std::span<const u8> span(reinterpret_cast<const u8*>(text.data()), text.size());
    return write_bytes(logical_path, span);
}

VFSResult<void> VirtualFileSystem::create_directories(std::string_view logical_path) {
    auto r = resolve_internal(logical_path);
    if (!r.ok) return VFSResult<void>::failure(r.error);
    std::error_code ec;
    std::filesystem::create_directories(r.value, ec);
    if (ec) return VFSResult<void>::failure(ec.message());
    return VFSResult<void>::success();
}

VFSResult<bool> VirtualFileSystem::is_directory(std::string_view logical_path) const {
    auto r = resolve_internal(logical_path);
    if (!r.ok) return VFSResult<bool>::failure(r.error);
    std::error_code ec;
    bool is_dir = std::filesystem::is_directory(r.value, ec);
    if (ec) return VFSResult<bool>::failure(ec.message());
    return VFSResult<bool>::success(is_dir);
}

std::vector<std::string> VirtualFileSystem::mounts() const {
    std::vector<std::string> out;
    for (auto& m : m_mounts) out.push_back(m.logical);
    return out;
}

} // namespace nf::assets
