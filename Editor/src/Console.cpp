#include <NF/Editor/Console.hpp>

#include <algorithm>
#include <cctype>

namespace nf::editor {

void ConsoleBuffer::push(const LogMessage& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lines.size() >= kMaxLines) {
        m_lines.pop_front();
        ++m_dropped;
    }
    m_lines.push_back(msg);
}

void ConsoleBuffer::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lines.clear();
    m_dropped = 0;
}

size_t ConsoleBuffer::dropped() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
}

std::vector<LogMessage> ConsoleBuffer::filtered(LogLevel min_level) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<LogMessage> out;
    for (const auto& m : m_lines) {
        if (static_cast<uint8_t>(m.level) >= static_cast<uint8_t>(min_level)) {
            out.push_back(m);
        }
    }
    return out;
}

std::vector<LogMessage> ConsoleBuffer::filtered(const Filter& filter) const {
    return snapshot(filter).lines;
}

ConsoleBuffer::Snapshot ConsoleBuffer::snapshot(const Filter& filter) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot out;
    out.total = m_lines.size();
    out.dropped = m_dropped;
    // The substring comparison is ASCII-case-insensitive: log text mixes
    // engine identifiers and user content (asset paths, entity names), and a
    // search that ignored case would hide "Runtime:" from someone typing
    // "runtime". Arabic text has no case, so this cannot corrupt shaping.
    std::string needle;
    needle.reserve(filter.text.size());
    for (char c : filter.text) {
        needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const auto& m : m_lines) {
        // All means every category, including uncategorised (None) messages —
        // has_category(All, None) is false, so a bitmask check alone would
        // drop them while the panel claims to show everything.
        if (filter.categories != LogCategory::All &&
            !has_category(filter.categories, m.category)) {
            continue;
        }
        if (!needle.empty()) {
            // tolower per byte, not the whole string first: message text is
            // UTF-8 and byte-wise lowercasing is the safe subset for ASCII.
            bool found = false;
            for (size_t i = 0; i + needle.size() <= m.text.size(); ++i) {
                size_t j = 0;
                for (; j < needle.size(); ++j) {
                    const char a =
                        static_cast<char>(std::tolower(static_cast<unsigned char>(m.text[i + j])));
                    if (a != needle[j]) {
                        break;
                    }
                }
                if (j == needle.size()) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        }
        for (size_t i = 0; i < out.level_counts.size(); ++i) {
            if (static_cast<size_t>(m.level) >= i) ++out.level_counts[i];
        }
        if (static_cast<uint8_t>(m.level) >= static_cast<uint8_t>(filter.min_level)) {
            out.lines.push_back(m);
        }
    }
    return out;
}

size_t ConsoleBuffer::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lines.size();
}

LogSink make_console_sink(ConsoleBuffer& buffer) {
    return [&buffer](const LogMessage& msg) { buffer.push(msg); };
}

const char* console_category_name(LogCategory c) {
    switch (c) {
        case LogCategory::None: return "-";
        case LogCategory::Core: return "Core";
        case LogCategory::Render: return "Render";
        case LogCategory::Physics: return "Physics";
        case LogCategory::Audio: return "Audio";
        case LogCategory::Network: return "Network";
        case LogCategory::Asset: return "Asset";
        case LogCategory::Editor: return "Editor";
        case LogCategory::Script: return "Script";
        case LogCategory::ECS: return "ECS";
        case LogCategory::Platform: return "Platform";
        case LogCategory::RHI: return "RHI";
        case LogCategory::Jobs: return "Jobs";
        case LogCategory::Scene: return "Scene";
        default: return "?";
    }
}

std::string format_console_lines(const std::vector<LogMessage>& lines) {
    static constexpr const char* levels[] = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal"};
    std::string out;
    for (const auto& line : lines) {
        const size_t level = static_cast<size_t>(line.level);
        out += "[";
        out += level < std::size(levels) ? levels[level] : "?";
        out += "][";
        out += console_category_name(line.category);
        out += "] ";
        out += line.text;
        out += '\n';
    }
    return out;
}

} // namespace nf::editor
