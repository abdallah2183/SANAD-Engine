// NF/Core/Profiler.cpp — hierarchical CPU profiler.

#include <NF/Core/Profiler.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace nf {

namespace {

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

// All live thread buffers, for merging. Entries are never removed (a thread
// buffer outlives its thread via thread_local destruction ordering only if
// the thread exits — instead each entry is heap-owned and leaked on purpose:
// profiler buffers are tiny and must stay valid for a merge that can happen
// after a worker thread exits).
std::vector<Profiler::ThreadBuffer*>& buffer_registry() {
    static std::vector<Profiler::ThreadBuffer*> registry;
    return registry;
}

} // namespace

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

Profiler::ThreadBuffer& Profiler::thread_buffer() {
    thread_local ThreadBuffer* buffer = nullptr;
    if (!buffer) {
        buffer = new ThreadBuffer(); // see registry note above
        std::lock_guard lock(registry_mutex());
        buffer_registry().push_back(buffer);
    }
    return *buffer;
}

u64 Profiler::now_us() {
    using clock = std::chrono::steady_clock;
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
            .count());
}

u64 Profiler::this_thread_hash() {
    return static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void Profiler::begin_zone(const char* name) {
#if !NF_PROFILE_ENABLED
    (void)name;
    return;
#else
    ThreadBuffer& buf = thread_buffer();
    ProfileEvent ev;
    ev.name = name ? name : "?";
    ev.start_us = now_us();
    ev.thread_hash = this_thread_hash();
    ev.depth = static_cast<u32>(buf.open.size());
    buf.open.push_back(std::move(ev));
#endif
}

void Profiler::end_zone() {
#if !NF_PROFILE_ENABLED
    return;
#else
    ThreadBuffer& buf = thread_buffer();
    if (buf.open.empty()) return; // unbalanced end: ignore, never crash
    ProfileEvent ev = std::move(buf.open.back());
    buf.open.pop_back();
    ev.end_us = now_us();
    buf.done.push_back(std::move(ev));
#endif
}

usize Profiler::end_frame() {
    std::vector<ProfileEvent> merged;
    {
        std::lock_guard lock(registry_mutex());
        for (ThreadBuffer* buf : buffer_registry()) {
            merged.insert(merged.end(), buf->done.begin(), buf->done.end());
            buf->done.clear();
            // Zones left open across the frame boundary are dropped: a frame
            // must own its zones, and leaking an open zone would corrupt
            // every later frame's depths.
            buf->open.clear();
        }
    }
    // Deterministic order: start time, then name, then thread.
    std::sort(merged.begin(), merged.end(), [](const ProfileEvent& a, const ProfileEvent& b) {
        if (a.start_us != b.start_us) return a.start_us < b.start_us;
        if (a.name != b.name) return a.name < b.name;
        return a.thread_hash < b.thread_hash;
    });
    m_last_events = merged;

    // Aggregates: inclusive sums, then exclusive = inclusive - children.
    // Children are found by containment within the same thread (events are
    // properly nested per thread because zones stack).
    std::unordered_map<std::string, ProfileAggregate> by_name;
    for (const auto& ev : m_last_events) {
        auto& agg = by_name[ev.name];
        agg.name = ev.name;
        agg.calls += 1;
        const u64 dur = ev.end_us >= ev.start_us ? ev.end_us - ev.start_us : 0;
        agg.inclusive_us += dur;
        agg.exclusive_us += dur;
    }
    for (const auto& ev : m_last_events) {
        // Subtract this event's duration from its parent's exclusive time.
        // Parent = the smallest same-thread event strictly containing it.
        const ProfileEvent* parent = nullptr;
        for (const auto& cand : m_last_events) {
            if (cand.thread_hash != ev.thread_hash || &cand == &ev) continue;
            if (cand.start_us <= ev.start_us && cand.end_us >= ev.end_us) {
                if (!parent || (cand.end_us - cand.start_us) < (parent->end_us - parent->start_us)) {
                    parent = &cand;
                }
            }
        }
        if (parent) {
            const u64 dur = ev.end_us >= ev.start_us ? ev.end_us - ev.start_us : 0;
            auto it = by_name.find(parent->name);
            if (it != by_name.end()) {
                it->second.exclusive_us =
                    it->second.exclusive_us >= dur ? it->second.exclusive_us - dur : 0;
            }
        }
    }
    m_last_aggregates.clear();
    for (auto& [name, agg] : by_name) m_last_aggregates.push_back(agg);
    std::sort(m_last_aggregates.begin(), m_last_aggregates.end(),
              [](const ProfileAggregate& a, const ProfileAggregate& b) { return a.name < b.name; });

    ++m_frame_index;
    return m_last_events.size();
}

bool Profiler::save_chrome_trace(const std::string& path, std::string& out_error) const {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "w") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "w");
#endif
    if (!f) {
        out_error = std::string("cannot write trace file: ") + path;
        return false;
    }
    std::fputs("{\"traceEvents\":[", f);
    bool first = true;
    for (const auto& ev : m_last_events) {
        // Escape the small set JSON cannot carry raw.
        std::string name;
        for (char c : ev.name) {
            if (c == '"') name += "\\\"";
            else if (c == '\\') name += "\\\\";
            else if (c == '\n') name += "\\n";
            else name += c;
        }
        const u64 dur = ev.end_us >= ev.start_us ? ev.end_us - ev.start_us : 0;
        std::fprintf(f, "%s{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%llu,\"dur\":%llu,\"pid\":1,\"tid\":%llu}",
                     first ? "" : ",", name.c_str(), (unsigned long long)ev.start_us,
                     (unsigned long long)dur, (unsigned long long)ev.thread_hash);
        first = false;
    }
    std::fputs("]}", f);
    std::fclose(f);
    return true;
}

void Profiler::reset_for_tests() {
    std::lock_guard lock(registry_mutex());
    for (ThreadBuffer* buf : buffer_registry()) {
        buf->open.clear();
        buf->done.clear();
    }
    m_last_events.clear();
    m_last_aggregates.clear();
    m_frame_index = 0;
}

} // namespace nf
