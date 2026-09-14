#include <NF/Editor/Console.hpp>

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

size_t ConsoleBuffer::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lines.size();
}

LogSink make_console_sink(ConsoleBuffer& buffer) {
    return [&buffer](const LogMessage& msg) { buffer.push(msg); };
}

} // namespace nf::editor
