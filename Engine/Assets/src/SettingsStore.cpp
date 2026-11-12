#define _CRT_SECURE_NO_WARNINGS
// NF/Runtime/SettingsStore.cpp — per-project settings in the user profile.

#include <NF/Assets/SettingsStore.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shlobj.h>
#endif

namespace nf::assets {
namespace {

std::string trim(std::string_view s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

bool key_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

} // namespace

bool SettingsStore::valid_key(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    if (key == "version") return false; // reserved: the schema stamp
    for (char c : key) if (!key_char_ok(c)) return false;
    return true;
}

std::string SettingsStore::safe_project_segment(const std::string& project_name) {
    std::string out;
    for (char c : project_name) {
        if (key_char_ok(c)) out.push_back(c);
        else out.push_back('_');
    }
    // Collapse empties; a traversal like "../../" becomes "______" (safe).
    if (out.empty()) out = "SANADProject";
    if (out == "." || out == "..") out = "SANADProject";
    if (out.size() > 64) out.resize(64);
    return out;
}

std::filesystem::path SettingsStore::user_data_root() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw != nullptr) {
        int len = ::WideCharToMultiByte(CP_UTF8, 0, raw, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string tmp(static_cast<size_t>(len - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, raw, -1, tmp.data(), len, nullptr, nullptr);
            out = std::move(tmp);
        }
    }
    if (raw != nullptr) ::CoTaskMemFree(raw);
    if (!out.empty()) return std::filesystem::path(out) / "SANAD";
#endif
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && local[0] != '\0') return std::filesystem::path(local) / "SANAD";
    const char* profile = std::getenv("USERPROFILE");
    if (profile && profile[0] != '\0') {
        return std::filesystem::path(profile) / "AppData" / "Local" / "SANAD";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "SANAD";
    }
    return {};
}

std::filesystem::path SettingsStore::settings_dir() const {
    std::filesystem::path root = m_base_override.empty() ? user_data_root() : m_base_override;
    if (root.empty()) return {};
    // Callers that inject a base override pass the LOCALAPPDATA-equivalent
    // root; the SANAD/<project> suffix is still applied so tests cannot leak
    // outside the temp root.
    return root / safe_project_segment(m_project_name);
}

std::filesystem::path SettingsStore::settings_file() const {
    auto dir = settings_dir();
    if (dir.empty()) return {};
    return dir / "settings.txt";
}

bool SettingsStore::ensure_dir(std::string& out_error) const {
    auto dir = settings_dir();
    if (dir.empty()) {
        out_error = "settings: user data folder is unavailable (LOCALAPPDATA not set)";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        out_error = "settings: could not create '" + dir.string() + "': " + ec.message();
        return false;
    }
    return true;
}

bool SettingsStore::exists() const {
    auto f = settings_file();
    if (f.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(f, ec) && !ec;
}

bool SettingsStore::save(const Map& values, std::string& out_error) const {
    if (values.size() > kMaxKeys) {
        out_error = "settings: too many keys (" + std::to_string(values.size()) + " > " + std::to_string(kMaxKeys) + ")";
        return false;
    }
    for (const auto& [k, v] : values) {
        if (!valid_key(k)) {
            out_error = "settings: invalid key '" + k + "' (use [A-Za-z0-9_.-], max 64 chars)";
            return false;
        }
        if (v.size() > 4096) {
            out_error = "settings: value for '" + k + "' exceeds 4096 bytes";
            return false;
        }
        if (v.find('\n') != std::string::npos || v.find('\r') != std::string::npos) {
            out_error = "settings: value for '" + k + "' must be single-line";
            return false;
        }
    }
    std::string body = "version: " + std::to_string(kSchemaVersion) + "\n";
    for (const auto& [k, v] : values) body += k + " = " + v + "\n";
    if (body.size() > kMaxFileBytes) {
        out_error = "settings: data exceeds 1 MiB";
        return false;
    }
    if (!ensure_dir(out_error)) return false;
    auto file = settings_file();
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            out_error = "settings: could not open '" + tmp.string() + "' for writing";
            return false;
        }
        out << body;
        out.close();
        if (!out) {
            out_error = "settings: write failed for '" + tmp.string() + "'";
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        // Windows rename does not overwrite an existing file: remove + retry.
        std::error_code ec2;
        std::filesystem::remove(file, ec2);
        ec.clear();
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            out_error = "settings: could not replace '" + file.string() + "': " + ec.message();
            std::filesystem::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

bool SettingsStore::load(Map& out, std::string& out_error) const {
    auto file = settings_file();
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec)) {
        out.clear();
        return true; // first run: no settings yet
    }
    if (!std::filesystem::is_regular_file(file, ec)) {
        out_error = "settings: '" + file.string() + "' is not a file";
        return false;
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        out_error = "settings: could not stat '" + file.string() + "'";
        return false;
    }
    if (size > kMaxFileBytes) {
        out_error = "settings: file exceeds 1 MiB, refusing to load";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        out_error = "settings: could not open '" + file.string() + "'";
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();

    Map parsed;
    bool saw_version = false;
    u32 version = 0;
    std::istringstream lines(text);
    std::string raw;
    size_t line_no = 0;
    while (std::getline(lines, raw)) {
        ++line_no;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("version:", 0) == 0) {
            try {
                version = static_cast<u32>(std::stoul(trim(line.substr(8))));
            } catch (...) {
                out_error = "settings: bad version line " + std::to_string(line_no);
                return false;
            }
            saw_version = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            out_error = "settings: line " + std::to_string(line_no) + ": expected 'key = value'";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (!valid_key(key)) {
            out_error = "settings: line " + std::to_string(line_no) + ": invalid key '" + key + "'";
            return false;
        }
        if (parsed.size() >= kMaxKeys) {
            out_error = "settings: too many keys";
            return false;
        }
        parsed[key] = value;
    }
    if (!saw_version) {
        out_error = "settings: missing 'version:' line";
        return false;
    }
    if (version > kSchemaVersion) {
        out_error = "settings: file was written by a newer engine (version " +
                    std::to_string(version) + ", this build reads " + std::to_string(kSchemaVersion) + ")";
        return false;
    }
    // version < current: this schema has no migrations yet; accept v1 only.
    if (version != kSchemaVersion) {
        out_error = "settings: unsupported version " + std::to_string(version);
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool SettingsStore::clear(std::string& out_error) const {
    auto file = settings_file();
    if (file.empty()) {
        out_error = "settings: user data folder is unavailable";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (ec) {
        out_error = "settings: could not remove '" + file.string() + "': " + ec.message();
        return false;
    }
    return true;
}

} // namespace nf::assets
