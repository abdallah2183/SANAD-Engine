#include <NF/Assets/ProjectDescriptor.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace nf::assets {

namespace {

std::string trim(std::string_view s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// True when `child` is `parent` or lives underneath it. Used to reject a
// relative mount that climbs out of the project with "..".
bool path_is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto c = child.lexically_normal();
    const auto p = parent.lexically_normal();
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

// The standard mounts every project gets unless it overrides them. `engine://`
// is deliberately absent: it only means anything inside the engine source tree,
// so a project that needs it declares it.
struct DefaultMount {
    const char* logical;
    const char* relative;
};
constexpr DefaultMount kDefaultMounts[] = {
    {"project://", "."},
    {"content://", "Content"},
    {"cache://", "Cache"},
    {"shaders://", "Shaders"},
    // Save slots. Declared by default so a packaged game can write saves
    // without the engine guessing a location, and kept out of `content://`
    // because a save is player data while content is shipped data.
    {"saves://", "Saves"},
};

bool is_known_logical(std::string_view logical) {
    if (logical.size() < 4) return false;
    return logical.ends_with("://");
}

} // namespace

std::optional<ProjectDescriptor> ProjectDescriptor::parse(std::string_view text,
                                                          const std::filesystem::path& base_dir,
                                                          std::string& out_error) {
    ProjectDescriptor desc;
    desc.m_root = base_dir.lexically_normal();

    bool saw_version = false;
    uint32_t version = 0;
    bool saw_name = false;

    // Explicit mounts from the file, in declaration order. Defaults are applied
    // afterwards so an explicit declaration always wins.
    std::vector<ProjectMount> declared;

    std::istringstream in{std::string(text)};
    std::string raw;
    size_t line_no = 0;
    while (std::getline(in, raw)) {
        ++line_no;
        // Tolerate CRLF: a project file written on Windows must parse anywhere.
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            out_error = "line " + std::to_string(line_no) + ": expected 'key: value', got '" + line + "'";
            return std::nullopt;
        }
        const std::string key = trim(std::string_view(line).substr(0, colon));
        const std::string value = trim(std::string_view(line).substr(colon + 1));

        if (key == "mount") {
            const auto arrow = value.find("->");
            if (arrow == std::string::npos) {
                out_error = "line " + std::to_string(line_no) +
                            ": mount needs '<logical> -> <path>', got '" + value + "'";
                return std::nullopt;
            }
            const std::string logical = trim(std::string_view(value).substr(0, arrow));
            const std::string raw_path = trim(std::string_view(value).substr(arrow + 2));
            if (!is_known_logical(logical)) {
                out_error = "line " + std::to_string(line_no) +
                            ": mount prefix must end with '://', got '" + logical + "'";
                return std::nullopt;
            }
            if (raw_path.empty()) {
                out_error = "line " + std::to_string(line_no) + ": mount '" + logical + "' has no path";
                return std::nullopt;
            }
            if (std::any_of(declared.begin(), declared.end(),
                            [&](const ProjectMount& m) { return m.logical == logical; })) {
                out_error = "line " + std::to_string(line_no) + ": duplicate mount '" + logical + "'";
                return std::nullopt;
            }

            std::filesystem::path physical;
            const std::filesystem::path as_written(raw_path);
            if (as_written.is_relative()) {
                physical = (desc.m_root / as_written).lexically_normal();
                // A project file must not be able to reach outside its own
                // directory. Absolute paths are allowed (engine:// points at an
                // install) but they are never *derived* from a traversal.
                if (!path_is_within(physical, desc.m_root)) {
                    out_error = "line " + std::to_string(line_no) + ": mount '" + logical +
                                "' escapes the project root: " + raw_path;
                    return std::nullopt;
                }
            } else {
                physical = as_written.lexically_normal();
            }
            declared.push_back(ProjectMount{logical, std::move(physical)});
            continue;
        }

        if (key == "version") {
            try {
                version = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
                out_error = "line " + std::to_string(line_no) + ": invalid version '" + value + "'";
                return std::nullopt;
            }
            saw_version = true;
        } else if (key == "name") {
            desc.m_name = value;
            saw_name = true;
        } else if (key == "title") {
            desc.m_title = value;
        } else if (key == "startup_scene") {
            desc.m_startup_scene = value;
        } else if (key == "window_width") {
            try {
                desc.m_window_width = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
                out_error = "line " + std::to_string(line_no) + ": invalid window_width '" + value + "'";
                return std::nullopt;
            }
        } else if (key == "window_height") {
            try {
                desc.m_window_height = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
                out_error = "line " + std::to_string(line_no) + ": invalid window_height '" + value + "'";
                return std::nullopt;
            }
        }
        // Anything else is ignored on purpose: a newer engine may write fields
        // this build does not know, and refusing to open the file would be worse
        // than ignoring them.
    }

    if (!saw_version) {
        out_error = "missing required 'version' line";
        return std::nullopt;
    }
    if (version != kCurrentVersion) {
        out_error = "unsupported project version " + std::to_string(version) + " (expected " +
                    std::to_string(kCurrentVersion) + ")";
        return std::nullopt;
    }
    if (!saw_name || desc.m_name.empty()) {
        out_error = "missing required 'name' line";
        return std::nullopt;
    }

    // Defaults first, then the file's own declarations override them.
    for (const auto& def : kDefaultMounts) {
        desc.m_mounts.push_back(
            ProjectMount{def.logical, (desc.m_root / def.relative).lexically_normal()});
    }
    for (auto& m : declared) {
        desc.add_mount(std::move(m.logical), std::move(m.physical));
    }

    if (desc.m_title.empty()) desc.m_title = desc.m_name;
    return desc;
}

std::optional<ProjectDescriptor> ProjectDescriptor::load_from_file(const std::filesystem::path& file,
                                                                   std::string& out_error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        out_error = "failed to open project file: '" + file.string() + "'";
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (!in.good() && !in.eof()) {
        out_error = "failed to read project file: '" + file.string() + "'";
        return std::nullopt;
    }
    // Relative mounts resolve against the descriptor's own directory, which is
    // what makes a project directory relocatable.
    return parse(buf.str(), file.parent_path(), out_error);
}

ProjectDescriptor ProjectDescriptor::make_default(const std::filesystem::path& root,
                                                  const std::string& name) {
    ProjectDescriptor desc;
    desc.m_root = root.lexically_normal();
    desc.m_name = name;
    desc.m_title = name;
    for (const auto& def : kDefaultMounts) {
        desc.add_mount(def.logical, (desc.m_root / def.relative).lexically_normal());
    }
    return desc;
}

void ProjectDescriptor::add_mount(std::string logical, std::filesystem::path physical) {
    for (auto& m : m_mounts) {
        if (m.logical == logical) {
            m.physical = std::move(physical);
            return;
        }
    }
    m_mounts.push_back(ProjectMount{std::move(logical), std::move(physical)});
}

std::filesystem::path ProjectDescriptor::mount_path(std::string_view logical) const {
    for (const auto& m : m_mounts) {
        if (m.logical == logical) return m.physical;
    }
    return {};
}

bool ProjectDescriptor::apply_mounts(VirtualFileSystem& vfs, std::string& out_error) const {
    for (const auto& m : m_mounts) {
        auto r = vfs.mount(m.logical, m.physical);
        if (!r.ok) {
            // Name the mount: "VFS setup failed" is useless when a project
            // declares several and only one of them is wrong.
            out_error = "failed to mount '" + m.logical + "' at '" + m.physical.string() +
                        "': " + r.error;
            return false;
        }
    }
    return true;
}

bool ProjectDescriptor::save_to_file(const std::filesystem::path& file,
                                     std::string& out_error) const {
    if (m_name.empty()) {
        out_error = "refusing to save a project with no name";
        return false;
    }

    std::filesystem::path tmp = file;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(tmp.parent_path(), ec);
    if (ec) {
        out_error = "failed to create directories for '" + file.string() + "': " + ec.message();
        return false;
    }

    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        out_error = "failed to open project file for writing: '" + tmp.string() + "'";
        return false;
    }

    out << "# NOVAForge Project\n";
    out << "version: " << kCurrentVersion << "\n";
    out << "name: " << m_name << "\n";
    out << "title: " << m_title << "\n";
    out << "startup_scene: " << m_startup_scene << "\n";
    out << "window_width: " << m_window_width << "\n";
    out << "window_height: " << m_window_height << "\n";
    out << "\n# Mounts. Relative paths resolve against this file's directory.\n";
    for (const auto& m : m_mounts) {
        // Write relative when the mount lives inside the project, so moving the
        // project directory keeps it working. Absolute stays absolute.
        std::string written = m.physical.string();
        if (path_is_within(m.physical, m_root)) {
            std::error_code rel_ec;
            auto rel = std::filesystem::relative(m.physical, m_root, rel_ec);
            if (!rel_ec) {
                written = rel.empty() ? std::string(".") : rel.generic_string();
            }
        }
        out << "mount: " << m.logical << " -> " << written << "\n";
    }
    out.close();
    if (!out) {
        out_error = "failed to write project file: '" + tmp.string() + "'";
        std::filesystem::remove(tmp, ec);
        return false;
    }

    // Atomic replace, matching AssetRegistry::save_to_physical.
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(file, ec);
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            out_error = "failed to atomically replace '" + file.string() + "': " + ec.message();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

} // namespace nf::assets
