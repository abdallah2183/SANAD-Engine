#include <NF/Rendering/FramePacingPolicy.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

const char* frame_policy_reason_name(FramePolicyReason reason) {
    switch (reason) {
    case FramePolicyReason::HoldNoSamples: return "hold-no-samples";
    case FramePolicyReason::HoldCooldown: return "hold-cooldown";
    case FramePolicyReason::HoldInBand: return "hold-in-band";
    case FramePolicyReason::HoldAtFloor: return "hold-at-floor";
    case FramePolicyReason::HoldAtCeiling: return "hold-at-ceiling";
    case FramePolicyReason::StepDown: return "step-down";
    case FramePolicyReason::StepUp: return "step-up";
    }
    return "hold-no-samples";
}

namespace {

/// Clamps every field into a legal range. Duplicated from the governor's own
/// rule on purpose: the policy must be safe to call with whatever a scene or a
/// config file contained, and a shared "sanitize" that silently differs from
/// the other component's would be worse than two small copies.
FramePolicyConfig sanitize(const FramePolicyConfig& in) {
    FramePolicyConfig out = in;
    if (!std::isfinite(out.target_fps) || out.target_fps <= 0.0f) {
        out.target_fps = 60.0f;
    }
    out.target_fps = std::clamp(out.target_fps, 10.0f, 360.0f);
    if (!std::isfinite(out.tolerance)) {
        out.tolerance = 0.10f;
    }
    out.tolerance = std::clamp(out.tolerance, 0.0f, 0.5f);
    if (!std::isfinite(out.step) || out.step <= 0.0f) {
        out.step = 0.05f;
    }
    out.step = std::clamp(out.step, 0.01f, 0.5f);
    if (!std::isfinite(out.min_scale)) {
        out.min_scale = 0.5f;
    }
    if (!std::isfinite(out.max_scale)) {
        out.max_scale = 1.0f;
    }
    out.min_scale = std::clamp(out.min_scale, 0.25f, 2.0f);
    out.max_scale = std::clamp(out.max_scale, 0.25f, 2.0f);
    if (out.min_scale > out.max_scale) {
        std::swap(out.min_scale, out.max_scale);
    }
    if (!std::isfinite(out.cooldown_seconds) || out.cooldown_seconds < 0.0f) {
        out.cooldown_seconds = 0.5f;
    }
    return out;
}

} // namespace

FramePolicyResult frame_policy_step(const FramePacingStats& stats, const FramePolicyConfig& config,
                                    FramePolicyState& state, f32 dt_seconds) {
    const FramePolicyConfig cfg = sanitize(config);

    FramePolicyResult result;
    result.scale = std::clamp(state.scale, cfg.min_scale, cfg.max_scale);
    state.scale = result.scale;

    // A paused frame advances nothing. It can still be asked what it thinks,
    // but a decision that "the cooldown expired because the game was paused"
    // would change the resolution the moment the player alt-tabs back in.
    const bool clock_advanced = std::isfinite(dt_seconds) && dt_seconds > 0.0f;
    if (clock_advanced) {
        state.since_change_seconds += dt_seconds;
    }

    if (stats.samples < cfg.min_samples) {
        result.reason = FramePolicyReason::HoldNoSamples;
        return result;
    }
    if (state.since_change_seconds < cfg.cooldown_seconds) {
        result.reason = FramePolicyReason::HoldCooldown;
        return result;
    }
    if (!std::isfinite(stats.low_1pct_fps) || !std::isfinite(stats.average_fps) ||
        stats.low_1pct_fps <= 0.0 || stats.average_fps <= 0.0) {
        // A window with zeroed statistics (a device that reported nothing)
        // must not be read as "0 fps, drop everything".
        result.reason = FramePolicyReason::HoldNoSamples;
        return result;
    }

    const f32 low_fps = static_cast<f32>(stats.low_1pct_fps);
    const f32 avg_fps = static_cast<f32>(stats.average_fps);
    const f32 floor_fps = cfg.target_fps * (1.0f - cfg.tolerance);
    const f32 ceiling_fps = cfg.target_fps * (1.0f + cfg.tolerance);

    // Down: the bad frames are below the band. This is the rule that makes the
    // policy worth having — an average-only governor waits for the stutter to
    // become the norm.
    if (low_fps < floor_fps) {
        if (result.scale <= cfg.min_scale + 1e-6f) {
            result.reason = FramePolicyReason::HoldAtFloor;
            return result;
        }
        result.scale = std::max(cfg.min_scale, result.scale - cfg.step);
        result.changed = true;
        result.reason = FramePolicyReason::StepDown;
    } else if (avg_fps > ceiling_fps && low_fps >= cfg.target_fps) {
        // Up: BOTH statistics must clear the target. The average alone would
        // climb on a window whose worst frames are still bad, which is how a
        // resolution oscillates around a stutter instead of settling.
        if (result.scale >= cfg.max_scale - 1e-6f) {
            result.reason = FramePolicyReason::HoldAtCeiling;
            return result;
        }
        result.scale = std::min(cfg.max_scale, result.scale + cfg.step);
        result.changed = true;
        result.reason = FramePolicyReason::StepUp;
    } else {
        result.reason = FramePolicyReason::HoldInBand;
    }

    if (result.changed) {
        state.scale = result.scale;
        state.since_change_seconds = 0.0f;
    }
    return result;
}

} // namespace nf::rendering
