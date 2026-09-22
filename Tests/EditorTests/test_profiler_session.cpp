// ProfilerSession (P4): the editor's rolling copy of the engine profiler's
// frames, and the Chrome trace export the Profiler panel writes.
//
// Why this object exists: nf::Profiler::end_frame() merges the per-thread
// buffers and then clears them, so Profiler::save_chrome_trace() can only ever
// export the last frame. A session that ran for an hour would export 1/60s of
// itself. These tests pin the behaviour that makes the export meaningful — and
// they are pure CPU, so the DoD "export a real trace" is provable without a
// GPU.

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Profiler.hpp>
#include <NF/Editor/ProfilerSession.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace nf;

namespace {

// RAII zone + explicit end_frame, mirroring what the editor loop does each
// frame: record the frame's zones, then end_frame() once, then capture.
// reset_for_tests first: a prior test (or the runner itself) may have left
// pending buffers, and this test asserts exact counts.
struct Frame {
    Frame() {
        Profiler::instance().reset_for_tests();
    }
    ~Frame() {
        Profiler::instance().reset_for_tests();
    }
    // Records one zone with enough work that the duration is non-zero on any
    // machine these tests run on — the export must preserve that.
    void zone(const char* name) {
        ProfileZone z(name);
        volatile double sink = 0.0;
        for (int i = 0; i < 2000; ++i) {
            sink += static_cast<double>(i) * 0.5;
        }
    }
    // The editor calls this once per frame, after every zone of that frame.
    void close() {
        Profiler::instance().end_frame();
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

NF_TEST(profiler_session_starts_empty) {
    editor::ProfilerSession session;
    NF_CHECK(session.empty());
    NF_CHECK_EQ(session.event_count(), 0u);
}

NF_TEST(profiler_session_captures_frames) {
    Frame frame;
    editor::ProfilerSession session;
    frame.zone("Test::zone_a");
    frame.close();
    session.capture_frame();
    NF_CHECK(!session.empty());
    NF_CHECK_EQ(session.event_count(), 1u);

    // A second frame appends rather than replacing — that is the whole point
    // of the session: the export spans the run, not one frame. Two zones in
    // that frame both survive, because end_frame() is called once per frame,
    // after all of the frame's zones.
    frame.zone("Test::zone_a");
    frame.zone("Test::zone_b");
    frame.close();
    session.capture_frame();
    NF_CHECK_EQ(session.event_count(), 3u);
}

NF_TEST(profiler_session_clear_empties_and_releases) {
    Frame frame;
    editor::ProfilerSession session;
    frame.zone("Test::zone_a");
    frame.close();
    session.capture_frame();
    NF_CHECK(!session.empty());
    session.clear();
    NF_CHECK(session.empty());
    NF_CHECK_EQ(session.event_count(), 0u);
    // Clear is idempotent and safe on an already-empty session.
    session.clear();
    NF_CHECK(session.empty());
}

NF_TEST(profiler_session_export_is_chrome_trace_json) {
    Frame frame;
    editor::ProfilerSession session;
    frame.zone("Test::render");
    frame.close();
    session.capture_frame();

    const auto path = std::filesystem::temp_directory_path() / "nf_test_profile_session.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::string err;
    const bool saved = session.save_chrome_trace(path.string(), err);
    NF_CHECK(saved);
    if (!saved) {
        return;
    }

    const std::string json = read_file(path);
    NF_CHECK(!json.empty());
    // Chrome Trace Event envelope: the array is opened and closed.
    NF_CHECK(json.find("{\"traceEvents\":[") != std::string::npos);
    NF_CHECK(json.back() == '}');
    // One complete duration event.
    NF_CHECK(json.find("\"name\":\"Test::render\"") != std::string::npos);
    NF_CHECK(json.find("\"ph\":\"X\"") != std::string::npos);
    NF_CHECK(json.find("\"pid\":1") != std::string::npos);
    NF_CHECK(json.find("\"tid\":") != std::string::npos);
    // A duration field is present and carries a real measurement.
    const size_t dur = json.find("\"dur\":");
    NF_CHECK(dur != std::string::npos);
    bool nonzero = false;
    for (size_t i = dur + 6; i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i) {
        if (json[i] != '0') {
            nonzero = true;
        }
    }
    NF_CHECK(nonzero);

    std::filesystem::remove(path, ec);
}

// The panel's button is offered before any frame has been captured (the
// Profiler window opens at startup). That must write a valid, event-less trace
// rather than fail or crash.
NF_TEST(profiler_session_empty_export_is_valid_trace) {
    editor::ProfilerSession session;
    const auto path = std::filesystem::temp_directory_path() / "nf_test_profile_empty.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::string err;
    NF_CHECK(session.save_chrome_trace(path.string(), err));
    const std::string json = read_file(path);
    NF_CHECK(json == "{\"traceEvents\":[]}");
    std::filesystem::remove(path, ec);
}

// Zone names come from C string literals, but the writer escapes the JSON
// specials anyway: a name built at runtime (or a future ::--style label) must
// not produce a file that fails to parse.
NF_TEST(profiler_session_escapes_json_specials) {
    Frame frame;
    editor::ProfilerSession session;
    // A quote in a zone name would terminate the JSON string early.
    frame.zone("zone \"quoted\" \\ path\n");
    frame.close();
    session.capture_frame();

    const auto path = std::filesystem::temp_directory_path() / "nf_test_profile_esc.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::string err;
    NF_CHECK(session.save_chrome_trace(path.string(), err));
    const std::string json = read_file(path);
    NF_CHECK(json.find("\\\"quoted\\\"") != std::string::npos);
    NF_CHECK(json.find("\\\\") != std::string::npos);
    NF_CHECK(json.find("\\n") != std::string::npos);
    // No raw control characters leaked into the file.
    NF_CHECK(json.find('\n') == std::string::npos || json.find("\\n") < json.find('\n'));
    std::filesystem::remove(path, ec);
}

// EditorApp::play() clears the session, so an export taken during a play
// session is that session and not the whole editor run. This pins the
// invariant without needing a Runtime: the clear is what makes it true.
NF_TEST(profiler_session_clear_after_capture_resets_count) {
    Frame frame;
    editor::ProfilerSession session;
    frame.zone("Test::zone_a");
    frame.close();
    session.capture_frame();
    NF_CHECK_EQ(session.event_count(), 1u);
    session.clear();
    frame.zone("Test::zone_b");
    // close() before capture_frame(): last_events() holds the previous frame
    // until end_frame() merges the new one, so capturing too early would
    // re-export the cleared session's zones.
    frame.close();
    session.capture_frame();
    NF_CHECK_EQ(session.event_count(), 1u);
    // The surviving event is the new session's, not the old one's.
    NF_CHECK(session.events().front().name == std::string("Test::zone_b"));
}
