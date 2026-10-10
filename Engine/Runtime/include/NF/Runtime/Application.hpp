#pragma once

#include <NF/Core/Types.hpp>

#include <string>

namespace nf::runtime {

struct ApplicationConfig {
    std::string title = "SANAD Runtime";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    bool validation = false;
    bool headless = false;
    // A windowed run is a played run: gameplay drives the scene and reads the
    // keyboard (KeyboardInputSource) unless the embedder opts out. Headless
    // runs ignore this and keep the editor-safe defaults, so test harnesses
    // see exactly the behaviour they saw before.
    bool enable_input = true;
    // Empty means "not specified": a project's startup_scene is used when one is
    // given, otherwise content://Scenes/Example.nfscene. Empty rather than a
    // default path so a caller can distinguish "I did not choose" from "I chose
    // the same path the default would have picked".
    std::string scene_path;
    uint32_t max_frames = 0; // 0 = run until close
    // Path to a .nfproj. When set, the project supplies the VFS mounts, window
    // title and size, and startup scene. When empty the runtime falls back to
    // walking up the filesystem for a directory containing both Engine/ and
    // Content/ — the engine source tree layout, which is what every in-repo
    // sample and test relies on. A shipped game has neither directory, so it
    // must pass a project.
    std::string project_path;

    // --- Deterministic input (record / replay) ------------------------------
    //
    // A save restores *state*; a log restores *behaviour*. Without a route from
    // the command line into `Runtime::play_input_log`, neither existed for a
    // shipped game: the recorder, the log format and the replay source were all
    // built and tested, but only a C++ host could reach them, so no headless
    // run could ever demonstrate "the player moved because of this input" and
    // no automated test could assert it.
    //
    // Both are plain filesystem paths, not logical ones: they name a file on
    // the machine that ran the game, which is the only thing that can read a
    // run's own recording. `saves://` would be the wrong layer — it resolves
    // through the game's asset mounts, which a CI machine may not have.

    /// Record every input gameplay polls to this file, written when the run
    /// ends. Capture starts with the scene and covers the whole frame loop.
    /// Empty = do not record.
    std::string input_log_path;

    /// Replay a previously recorded log instead of reading the keyboard.
    /// Frame `n` of the log answers every poll during step `n`. Mutually
    /// exclusive with `input_log_path`: recording a replay would log the
    //  replay back into itself.
    std::string input_replay_path;

    /// Exit non-zero when a replay never ran any frames. Off by default so a
    /// missing or empty log is not silently a successful no-op run; a CI job
    /// that means to prove gameplay drives the entity turns it on.
    bool input_replay_strict = false;
};

class Application {
public:
    explicit Application(const ApplicationConfig& config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Returns exit code (0 on success)
    int run();

private:
    ApplicationConfig m_config;
};

} // namespace nf::runtime
