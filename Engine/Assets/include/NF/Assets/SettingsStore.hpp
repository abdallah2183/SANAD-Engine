#pragma once
// NF/Runtime/SettingsStore.hpp — per-project local settings under the user profile.
//
// Why not VFS saves:// (see SaveSystem)? saves:// defaults to a Saves folder
// next to the project — inside a packaged dist/ that folder is read-only when
// the package sits under Program Files or a non-writable directory, and the
// task requires persistence from a non-writable exe folder. Settings therefore
// live under %LOCALAPPDATA%\SANAD\<ProjectName>\ (Windows), keyed by the
// sanitized project name so two games never share a file. The atomic
// write-temp-then-rename pattern is reused from ProjectDescriptor.
//
// File format (tolerant, versioned):
//   version: 1
//   key = value         (value is raw UTF-8, may contain spaces; '#' comments)
// Limits: file <= 1 MiB, <= 512 keys, key <= 64 chars [A-Za-z0-9_.-],
// value <= 4096 bytes. Lua can only name keys — never paths.

#include <NF/Core/Types.hpp>

#include <filesystem>
#include <map>
#include <string>

namespace nf::assets {

class SettingsStore {
public:
    static constexpr u32 kSchemaVersion = 1;
    static constexpr size_t kMaxFileBytes = 1024 * 1024;
    static constexpr size_t kMaxKeys = 512;

    SettingsStore() = default;
    explicit SettingsStore(std::string project_name) : m_project_name(std::move(project_name)) {}
    // For tests / portable runs: force the base directory (LOCALAPPDATA root).
    SettingsStore(std::string project_name, std::filesystem::path base_dir_override)
        : m_project_name(std::move(project_name)), m_base_override(std::move(base_dir_override)) {}

    void set_project_name(std::string name) { m_project_name = std::move(name); }
    const std::string& project_name() const { return m_project_name; }

    /// Directory for this project: <LOCALAPPDATA>/SANAD/<safe-name>/.
    /// Creates nothing; use ensure_dir() before writing. Empty on failure.
    std::filesystem::path settings_dir() const;
    std::filesystem::path settings_file() const;
    bool ensure_dir(std::string& out_error) const;

    using Map = std::map<std::string, std::string>;
    static bool valid_key(const std::string& key);
    static std::string safe_project_segment(const std::string& project_name);
    static std::filesystem::path user_data_root(); // LOCALAPPDATA or empty

    bool save(const Map& values, std::string& out_error) const;
    // Missing file -> empty map + ok=true ("no settings yet" is not an error).
    // Corrupt/oversized/unknown-version -> ok=false with error; map untouched.
    bool load(Map& out, std::string& out_error) const;
    bool clear(std::string& out_error) const;
    bool exists() const;

private:
    std::string m_project_name;
    std::filesystem::path m_base_override;
};

} // namespace nf::assets
