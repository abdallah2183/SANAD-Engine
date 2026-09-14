// NF/Editor/FileWatcher.cpp — poll-based watching (see header).

#include <NF/Editor/FileWatcher.hpp>
#include <NF/Core/Hash.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace nf::editor {

FileWatcher::Baseline FileWatcher::snapshot(assets::VirtualFileSystem& vfs,
                                            const std::string& logical_path) {
    Baseline b;
    auto resolved = vfs.resolve(logical_path);
    if (!resolved.ok) {
        return b;
    }
    std::error_code ec;
    if (!std::filesystem::exists(resolved.value, ec) || ec) {
        return b;
    }
    b.exists = true;
    auto ft = std::filesystem::last_write_time(resolved.value, ec);
    if (!ec) {
        b.mtime_ns = static_cast<uint64_t>(ft.time_since_epoch().count());
    }
    b.size = static_cast<uint64_t>(std::filesystem::file_size(resolved.value, ec));
    if (ec) {
        b.size = 0;
    }
    if (b.size <= kHashBytes) {
        std::ifstream in(resolved.value, std::ios::binary);
        if (in) {
            std::vector<uint8_t> bytes(b.size);
            if (b.size == 0 || (in.read(reinterpret_cast<char*>(bytes.data()),
                                        static_cast<std::streamsize>(b.size)) &&
                                static_cast<size_t>(in.gcount()) == b.size)) {
                b.content_hash = fnv1a_64(bytes.data(), bytes.size());
                b.hashed = true;
            }
        }
    }
    return b;
}

void FileWatcher::watch(assets::VirtualFileSystem& vfs, const std::string& logical_path) {
    m_entries[logical_path] = snapshot(vfs, logical_path);
}

void FileWatcher::unwatch(const std::string& logical_path) {
    m_entries.erase(logical_path);
}

void FileWatcher::clear() {
    m_entries.clear();
}

bool FileWatcher::is_watched(const std::string& logical_path) const {
    return m_entries.find(logical_path) != m_entries.end();
}

std::vector<std::string> FileWatcher::poll(assets::VirtualFileSystem& vfs) {
    std::vector<std::string> changed;
    // Baselines are real snapshots (taken by watch()/refresh()), so any
    // divergence — content edit, create-after-watch, or delete — reports
    // exactly once and re-baselines. The content hash arbitrates same-size
    // rapid rewrites that coarse timestamps cannot distinguish.
    for (auto& kv : m_entries) {
        const Baseline now = snapshot(vfs, kv.first);
        const Baseline& base = kv.second;
        bool differs = (base.exists != now.exists || base.mtime_ns != now.mtime_ns ||
                        base.size != now.size);
        if (!differs && base.hashed && now.hashed && base.content_hash != now.content_hash) {
            differs = true;
        }
        if (differs) {
            changed.push_back(kv.first);
            kv.second = now;
        }
    }
    return changed;
}

void FileWatcher::refresh(const std::string& logical_path, assets::VirtualFileSystem& vfs) {
    auto it = m_entries.find(logical_path);
    if (it == m_entries.end()) {
        return;
    }
    it->second = snapshot(vfs, logical_path);
}

} // namespace nf::editor
