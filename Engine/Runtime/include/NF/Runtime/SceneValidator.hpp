#pragma once

// NF/Runtime/SceneValidator.hpp — the rules a scene has to satisfy to be
// playable, checked BEFORE it is played.
//
// The loader already refuses a line it cannot parse, and the editor refuses a
// value its command rejects. Neither can see the problems that only exist
// between entities:
//
//   * two entities with the same name, which makes `follow=` and
//     `nf.find_entity` answer an arbitrary one of them;
//   * a camera that follows a name nothing has, so it sits where it was and
//     the player sees a frozen view;
//   * a render scale outside the range the renderer accepts, which the loader
//     accepts because the field itself is well-formed;
//   * a scene with no active camera at all.
//
// Those are exactly the failures that cost an hour to find by playing and
// seconds to find by reading. So this is a pure pass over a loaded scene: no
// device, no files, no clock. The CLI (`nf validate`) runs it over every scene
// in a project; a test runs it over a scene it builds in memory.
//
// It reports; it does not repair. A validator that silently fixes a scene is a
// validator whose output nobody reads.

#include <NF/Core/Types.hpp>
#include <NF/Scene/Scene.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nf::runtime {

enum class ValidationSeverity : u8 {
    Info = 0,    ///< Worth knowing; the scene still plays.
    Warning = 1, ///< It will play, but probably not as intended.
    Error = 2,   ///< It will not play correctly; fix before shipping.
};

/// Stable technical name for logs and CLI output ("info"/"warning"/"error").
const char* validation_severity_name(ValidationSeverity severity);

struct ValidationIssue {
    ValidationSeverity severity = ValidationSeverity::Info;
    /// The entity the issue is about; empty for a scene-level issue.
    std::string entity;
    std::string message;
};

struct ValidationReport {
    std::string scene_path;
    std::vector<ValidationIssue> issues;
    u32 errors = 0;
    u32 warnings = 0;
    u32 infos = 0;

    void add(ValidationSeverity severity, std::string entity, std::string message);
    [[nodiscard]] bool ok() const { return errors == 0; }
};

/// Answers "does this logical path exist?".
///
/// The validator stays free of the filesystem by taking the question instead of
/// asking it. A test passes a set of names; the CLI passes the project's VFS.
using AssetExists = std::function<bool(std::string_view)>;

/// Every rule, over one loaded scene.
///
/// `scene_path` only labels the report. `asset_exists` is optional: when it is
/// empty the asset rules are skipped, which is the honest behaviour for a
/// caller with no filesystem (and keeps the pure rules pure). Only paths that
/// LOOK like paths (`content://…`) are checked — `material` may also be a bare
/// name, and a name is resolved by the asset system, not by a file lookup.
[[nodiscard]] ValidationReport validate_scene(const scene::Scene& scene,
                                              std::string_view scene_path,
                                              const AssetExists& asset_exists = {});

/// Human-readable form: one line per issue, then a summary. Errors first —
/// a long list of infos must not push the one line that matters off the top.
[[nodiscard]] std::string format_validation_report(const ValidationReport& report);

/// Machine-readable form, for a CI gate or an editor panel.
///
/// One object per scene:
///   {"scene":"…","errors":N,"warnings":N,"infos":N,
///    "issues":[{"severity":"error","entity":"…","message":"…"}]}
/// Strings are escaped (quotes, backslashes, control characters), so a scene
/// whose entity name contains a quote cannot produce a file no parser reads.
[[nodiscard]] std::string format_validation_json(const ValidationReport& report);

} // namespace nf::runtime
