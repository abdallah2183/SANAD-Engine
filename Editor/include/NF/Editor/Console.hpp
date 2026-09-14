#pragma once

// NF/Editor/Console.hpp — bounded in-memory log sink for the Console panel.
//
// The buffer keeps the newest kMaxLines messages and counts how many were
// dropped, so a spammy frame can neither leak memory nor stall the runtime.
// Install once via Logger::add_sink(make_console_sink(buffer)).

#include <NF/Core/Logger.hpp>

#include <cstddef>
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
    size_t size() const;

private:
    mutable std::mutex m_mutex;
    std::deque<LogMessage> m_lines;
    size_t m_dropped = 0;
};

LogSink make_console_sink(ConsoleBuffer& buffer);

} // namespace nf::editor
