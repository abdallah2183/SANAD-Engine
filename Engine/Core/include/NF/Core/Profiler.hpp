#pragma once

// NF/Core/Profiler.hpp — hierarchical CPU profiler (design doc Section 84-85).
//
// Zones nest per thread; end_frame() aggregates inclusive/exclusive times and
// call counts per zone, and save_chrome_trace() exports the frame's events in
// Chrome Trace Event format (open in chrome://tracing or Perfetto).
//
// Cost when idle: one thread_local stack push/pop per zone (~tens of ns).
// Disabled builds: NF_PROFILE_ENABLED=0 compiles every macro to nothing.
//
// Threading: each thread records into its own buffer; end_frame() merges all
// buffers that reported since the previous end_frame. Export ordering is
// deterministic (sorted by start time, then name) so traces diff cleanly.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <string>
#include <vector>

#ifndef NF_PROFILE_ENABLED
#define NF_PROFILE_ENABLED 1
#endif

namespace nf {

struct ProfileEvent {
    std::string name;
    u64 start_us = 0; // microseconds since an unspecified profiler epoch
    u64 end_us = 0;
    u64 thread_hash = 0;
    u32 depth = 0;
};

struct ProfileAggregate {
    std::string name;
    u64 calls = 0;
    u64 inclusive_us = 0;
    u64 exclusive_us = 0; // minus children's inclusive time
};

class Profiler {
public:
    /// Per-thread record. Public only so the .cpp's helpers can name it;
    /// not part of the API.
    struct ThreadBuffer {
        std::vector<ProfileEvent> open; // stack of open zones (back = innermost)
        std::vector<ProfileEvent> done; // closed events awaiting merge
    };

    static Profiler& instance();

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    /// Opens a zone on the calling thread. Every begin matches one end;
    /// use Zone RAII instead of calling these directly.
    void begin_zone(const char* name);
    void end_zone();

    /// Closes the current frame: merges thread buffers, builds aggregates,
    /// and clears for the next frame. Returns the number of events merged.
    usize end_frame();

    /// Events merged by the last end_frame (sorted deterministically).
    const std::vector<ProfileEvent>& last_events() const { return m_last_events; }
    /// Per-zone aggregates from the last end_frame (sorted by name).
    const std::vector<ProfileAggregate>& last_aggregates() const { return m_last_aggregates; }
    u64 frame_index() const { return m_frame_index; }

    /// Writes last_events() as Chrome Trace Event JSON. Returns false +
    /// err when the file cannot be written.
    bool save_chrome_trace(const std::string& path, std::string& out_error) const;

    /// Test seam: drops all state including other threads' pending buffers
    /// (call only when no other thread is profiling).
    void reset_for_tests();

private:
    Profiler() = default;

    static ThreadBuffer& thread_buffer();
    static u64 now_us();
    static u64 this_thread_hash();

    std::vector<ProfileEvent> m_last_events;
    std::vector<ProfileAggregate> m_last_aggregates;
    u64 m_frame_index = 0;
};

/// RAII zone: times from construction to destruction.
class ProfileZone {
public:
    explicit ProfileZone(const char* name) {
#if NF_PROFILE_ENABLED
        Profiler::instance().begin_zone(name);
#else
        (void)name;
#endif
    }
    ~ProfileZone() {
#if NF_PROFILE_ENABLED
        Profiler::instance().end_zone();
#endif
    }
    ProfileZone(const ProfileZone&) = delete;
    ProfileZone& operator=(const ProfileZone&) = delete;
};

} // namespace nf

#if NF_PROFILE_ENABLED
#define NF_PROFILE_SCOPE(name) ::nf::ProfileZone NF_PROFILE_CONCAT(_nf_zone_, __LINE__)(name)
#define NF_PROFILE_FUNCTION() NF_PROFILE_SCOPE(__FUNCTION__)
#else
#define NF_PROFILE_SCOPE(name) ((void)0)
#define NF_PROFILE_FUNCTION() ((void)0)
#endif
#define NF_PROFILE_CONCAT(a, b) NF_PROFILE_CONCAT_INNER(a, b)
#define NF_PROFILE_CONCAT_INNER(a, b) a##b
