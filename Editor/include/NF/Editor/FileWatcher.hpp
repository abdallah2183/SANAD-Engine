#pragma once

// NF/Editor/FileWatcher.hpp — poll-based content watching (no OS APIs).
//
// Portable and deterministic: the shell polls once in a while, tests poll
// directly. A file is "changed" when its size or last-write time differs from
// the baseline snapshot; vanished files are reported as changed (the caller
// decides: warn-and-keep for live assets, never crash). Baselines refresh
// only through poll() hits or explicit refresh() (used after editor saves so
// our own writes never echo back as external changes).

#include <NF/Assets/VirtualFileSystem.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::editor {

class FileWatcher {
public:
    FileWatcher() = default;

    // Starts watching, baselining immediately (creation after watch() counts
    // as a change; missing files baseline as missing). Re-watching refreshes.
    void watch(assets::VirtualFileSystem& vfs, const std::string& logical_path);
    void unwatch(const std::string& logical_path);
    void clear();

    // Returns watched paths that changed since the last poll (or watch) and
    // re-baselines them. Deletions report exactly once, then go quiet.
    std::vector<std::string> poll(assets::VirtualFileSystem& vfs);

    // Re-baselines without reporting (call after our own saves/imports).
    void refresh(const std::string& logical_path, assets::VirtualFileSystem& vfs);

    size_t watched_count() const { return m_entries.size(); }
    bool is_watched(const std::string& logical_path) const;

private:
    // Files at or below kHashBytes are content-hashed: back-to-back rewrites
    // with identical size can share a timestamp on coarse filesystems, and a
    // missed hot reload is worse than a few kilobytes of reads per poll.
    static constexpr uint64_t kHashBytes = 256u * 1024u;
    struct Baseline {
        bool exists = false;
        uint64_t mtime_ns = 0;
        uint64_t size = 0;
        uint64_t content_hash = 0;
        bool hashed = false;
    };
    static Baseline snapshot(assets::VirtualFileSystem& vfs, const std::string& logical_path);

    std::unordered_map<std::string, Baseline> m_entries;
};

} // namespace nf::editor
