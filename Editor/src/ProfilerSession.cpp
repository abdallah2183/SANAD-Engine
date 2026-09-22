#include <NF/Editor/ProfilerSession.hpp>

#include <cstdio>

namespace nf::editor {

namespace {

// The same escape set nf::Profiler applies: these are the only characters JSON
// cannot carry raw, and zone names are C string literals from the codebase, so
// nothing else can appear in practice.
std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c; break;
        }
    }
    return out;
}

} // namespace

void ProfilerSession::capture_frame() {
    const std::vector<nf::ProfileEvent>& frame = nf::Profiler::instance().last_events();
    // Reserve once at the start so a long session appends in amortised O(1)
    // instead of reallocating every frame.
    if (m_events.empty()) {
        m_events.reserve(frame.size() * 8);
    }
    m_events.insert(m_events.end(), frame.begin(), frame.end());
}

void ProfilerSession::clear() {
    m_events.clear();
    // Drop the capacity too: a session that ran for an hour should not keep a
    // million-event buffer allocated through the next one.
    std::vector<nf::ProfileEvent>().swap(m_events);
}

bool ProfilerSession::empty() const {
    return m_events.empty();
}

std::size_t ProfilerSession::event_count() const {
    return m_events.size();
}

const std::vector<nf::ProfileEvent>& ProfilerSession::events() const {
    return m_events;
}

bool ProfilerSession::save_chrome_trace(const std::string& path, std::string& out_error) const {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "w") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(path.c_str(), "w");
#endif
    if (f == nullptr) {
        out_error = std::string("cannot write trace file: ") + path;
        return false;
    }
    std::fputs("{\"traceEvents\":[", f);
    bool first = true;
    for (const nf::ProfileEvent& ev : m_events) {
        const std::uint64_t dur = (ev.end_us >= ev.start_us) ? (ev.end_us - ev.start_us) : 0;
        std::fprintf(f,
                     "%s{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%llu,\"dur\":%llu,\"pid\":1,\"tid\":%llu}",
                     first ? "" : ",", escape_json(ev.name).c_str(),
                     static_cast<unsigned long long>(ev.start_us),
                     static_cast<unsigned long long>(dur),
                     static_cast<unsigned long long>(ev.thread_hash));
        first = false;
    }
    std::fputs("]}", f);
    std::fclose(f);
    return true;
}

} // namespace nf::editor
