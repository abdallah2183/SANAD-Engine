#pragma once

// NF/Editor/ProfilerSession.hpp — a play-session trace recorder.
//
// nf::Profiler merges and clears its per-thread buffers every end_frame(), so
// Profiler::save_chrome_trace() can only ever export the last frame — 1/60s of
// a session. A profiler whose export shows a single frame is not a profiler,
// so the editor keeps its own rolling copy: capture_frame() is called once per
// editor frame right after end_frame() and appends the just-merged events;
// save_chrome_trace() then writes the whole recorded window.
//
// Clear it when a session begins (e.g. on Play) so an export is genuinely
// "this session" rather than "everything since the editor started".
//
// The format matches nf::Profiler::save_chrome_trace byte for byte
// ({"traceEvents":[{...}]}), so a file written by either one loads in
// chrome://tracing or Perfetto.

#include <NF/Core/Profiler.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace nf::editor {

class ProfilerSession {
public:
    // Appends the profiler's last merged frame. Call after
    // Profiler::end_frame(); before it, last_events() is the previous frame.
    void capture_frame();

    void clear();
    bool empty() const;
    std::size_t event_count() const;

    // Writes Chrome Trace Event JSON. Returns false and sets out_error when the
    // file cannot be written. An empty session still writes a valid
    // (event-less) trace rather than failing.
    bool save_chrome_trace(const std::string& path, std::string& out_error) const;

    // Read access for tests and the panel: the recorded events, in the
    // profiler's deterministic order.
    const std::vector<nf::ProfileEvent>& events() const;

private:
    std::vector<nf::ProfileEvent> m_events;
};

} // namespace nf::editor
