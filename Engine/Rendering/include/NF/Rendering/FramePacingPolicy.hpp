#pragma once

// NF/Rendering/FramePacingPolicy.hpp — render scale decided from the frame
// WINDOW, not from the last frame (Phase 42).
//
// `DynamicResolution` (Phase 38) reacts to a smoothed per-frame load. That is
// the right shape for a governor that runs every frame, but it answers the
// wrong question: an average of 60 fps can hide a run that stutters once a
// second, and the stutter is what the player feels. `FramePacing` (Phase 41)
// already measures the window that can answer it — the 1% low.
//
// So this is the policy half: it consumes a finished `FramePacingStats` window
// and says whether the scale should move. It is deliberately a pure function of
// (stats, config, state, dt):
//
//   * No GPU, no renderer, no clock of its own. Every rule below is testable
//     without a device, and the same call can drive the editor and the player.
//   * The 1% LOW decides to go down; the AVERAGE decides to come back up. A
//     single stutter should lower the resolution, but a single good second
//     should not raise it — so recovery also requires the 1% low to clear the
//     target. That asymmetry is the whole policy.
//   * A cooldown and a hysteresis band, for the same reason the governor has
//     them: every scale change rebuilds every render target, and an oscillator
//     costs more than the frames it saves.
//   * Too few samples is "hold", not "act". Eight frames is one bad compile
//     hitch away from a wrong verdict.
//
// It does not replace `DynamicResolution`; a host picks one. Keeping both is
// deliberate: the per-frame governor is the low-latency option, this is the
// "judge the window" option, and which one feels better is a per-game decision.

#include <NF/Core/Types.hpp>
#include <NF/Rendering/FramePacing.hpp>

namespace nf::rendering {

struct FramePolicyConfig {
    /// What the game wants. The 1% low and the average are both compared
    /// against it; 0 or a non-finite value falls back to 60.
    f32 target_fps = 60.0f;

    /// The band around the target where nothing moves. 0.10 means "within 10%
    /// of target is good enough", which is what stops a 59 fps window from
    /// triggering a step on a 60 fps target.
    f32 tolerance = 0.10f;

    /// How far one accepted decision moves the scale.
    f32 step = 0.05f;

    /// The legal range. The authored `render_scale` is normally the max.
    f32 min_scale = 0.5f;
    f32 max_scale = 1.0f;

    /// Wall time that must pass between accepted changes.
    f32 cooldown_seconds = 0.5f;

    /// A window shorter than this cannot justify a change.
    u32 min_samples = 8;
};

struct FramePolicyState {
    f32 scale = 1.0f;
    /// Wall time since the last accepted change. Advanced every call, cleared
    /// only when a change is accepted.
    f32 since_change_seconds = 0.0f;
};

/// Why the policy returned what it did. Logged and shown in the editor; the
/// scale alone cannot explain a decision to a person looking at a number.
enum class FramePolicyReason : u8 {
    HoldNoSamples = 0, ///< The window is too short to judge.
    HoldCooldown = 1,  ///< A change happened recently.
    HoldInBand = 2,    ///< Both statistics are within tolerance.
    HoldAtFloor = 3,   ///< The policy wants to go down but cannot.
    HoldAtCeiling = 4, ///< The policy wants to go up but cannot.
    StepDown = 5,      ///< The 1% low missed the target.
    StepUp = 6,        ///< The average and the 1% low both cleared it.
};

const char* frame_policy_reason_name(FramePolicyReason reason);

struct FramePolicyResult {
    f32 scale = 1.0f;
    bool changed = false;
    FramePolicyReason reason = FramePolicyReason::HoldNoSamples;
};

/// Decides the scale for the next window.
///
/// `dt_seconds` advances the cooldown and is the host's wall clock, not the
/// frame time — a paused frame (dt <= 0) advances nothing and can only hold.
/// The state is updated in place: an accepted change resets the cooldown and
/// stores the new scale, a hold stores only the advanced clock.
FramePolicyResult frame_policy_step(const FramePacingStats& stats, const FramePolicyConfig& config,
                                    FramePolicyState& state, f32 dt_seconds);

} // namespace nf::rendering
