#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Runtime/InputLog.hpp>

#include <string>

namespace nf::runtime {

struct ApplicationConfig;

// The effective run settings after a project (if any) has been applied.
struct ResolvedRunConfig {
    std::string scene_path;   // never empty unless the caller explicitly asked for no scene
    std::string title;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string project_name; // empty when running without a project
};

// Applies a project to the VFS and derives the effective run settings.
//
// Extracted from Application::run() so the precedence rules are assertable
// rather than buried in a 400-line function that also opens a window and a
// device:
//
//   - With a project: the project declares the mounts, and supplies title,
//     window size and startup scene.
//   - An explicitly supplied scene_path always wins over the project's startup
//     scene, so `--scene` keeps working against a project.
//   - Without a project: the legacy walk-up for a directory containing both
//     Engine/ and Content/ still applies, and an unspecified scene falls back
//     to content://Scenes/Example.nfscene. This is what every in-repo sample and
//     test relies on, so it must not change.
//
// `vfs` receives the mounts. Returns false and fills out_error when a project
// was configured but could not be loaded or mounted — never a partial setup.
bool resolve_run_config(const ApplicationConfig& config,
                        assets::VirtualFileSystem& vfs,
                        ResolvedRunConfig& out,
                        std::string& out_error);

// The scene used when nothing specifies one and no project is in play.
inline constexpr const char* kDefaultScenePath = "content://Scenes/Example.nfscene";

/// ---------------------------------------------------------------------------
/// Input record/replay: the validation and file loading behind `--input-log` and
/// `--replay`.
///
/// Split out of `Application::run()` for the same reason `resolve_run_config`
/// exists: a rule that decides whether a run is *allowed to start* must be
/// assertable without opening a window and a Vulkan device. Every rejection
/// here is a case that would otherwise surface as a confusing mid-run failure or,
/// worse, as a silent no-op that reports success.
///
/// Returns false and fills `out_error` when the combination is unusable.
/// `out_log` is only meaningful on success.
/// ---------------------------------------------------------------------------

/// Why a replay produced no gameplay. Reported separately from success/failure
/// because "the log was fine but nothing happened" is the case most likely to
/// be a real bug in the game rather than a typo in a flag.
enum class ReplayProbe {
    NotConfigured,   // no --replay was given
    Loaded,          // frames were served
    EmptyLog,        // the file parsed but recorded no frames
    Unreadable,      // the file could not be opened
    RejectedLog,     // the file was opened but is not a valid log
};

const char* to_string(ReplayProbe probe);

/// Check the configured record/replay combination and, when replaying, load
/// the log.
///
/// Rejects, with a message naming the flag:
///  - both `--input-log` and `--replay` (recording a replay would log the
///    replay back into itself; `InputRecorder`/`InputReplay` are mutually
///    exclusive by construction and doing it anyway nests them),
///  - `--replay` onto a file that cannot be opened,
///  - `--replay` onto a file whose contents are not a well-formed log.
///
/// `out_error` is left untouched on success.
bool prepare_input_replay(const ApplicationConfig& config,
                          InputLog& out_log,
                          ReplayProbe& out_probe,
                          std::string& out_error);

} // namespace nf::runtime
