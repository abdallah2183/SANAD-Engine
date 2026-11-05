#pragma once

// NF/Editor/Console.hpp — bounded in-memory log sink for the Console panel.
//
// The buffer keeps the newest kMaxLines messages and counts how many were
// dropped, so a spammy frame can neither leak memory nor stall the runtime.
// Install once via Logger::add_sink(make_console_sink(buffer)).

#include <NF/Core/Logger.hpp>

#include <cstddef>
#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nf::editor {

class ConsoleBuffer {
public:
    static constexpr size_t kMaxLines = 500;

    void push(const LogMessage& msg);
    void clear();
    size_t dropped() const;

    // Snapshot with minimum-level filter (Inspector-style pure read).
    std::vector<LogMessage> filtered(LogLevel min_level) const;

    // The panel's full query: level floor, a category bitmask (a message in
    // none of the selected categories is hidden), and a case-insensitive
    // substring over the message text. An empty substring disables the text
    // filter; LogCategory::All disables the category filter.
    struct Filter {
        LogLevel min_level = LogLevel::Info;
        LogCategory categories = LogCategory::All;
        std::string text;
    };
    std::vector<LogMessage> filtered(const Filter& filter) const;
    struct Snapshot {
        std::vector<LogMessage> lines;
        // Counts at each minimum severity, including messages below the
        // current level floor, but respecting the text/category query.
        std::array<size_t, 6> level_counts{};
        size_t total = 0;
        size_t dropped = 0;
    };
    Snapshot snapshot(const Filter& filter) const;
    size_t size() const;

private:
    mutable std::mutex m_mutex;
    std::deque<LogMessage> m_lines;
    size_t m_dropped = 0;
};

LogSink make_console_sink(ConsoleBuffer& buffer);

const char* console_category_name(LogCategory category);
// Clipboard export keeps logical UTF-8 and multiline text, plus diagnostic tags.
std::string format_console_lines(const std::vector<LogMessage>& lines);

} // namespace nf::editor
