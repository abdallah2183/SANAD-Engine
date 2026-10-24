#pragma once

// NF/Rendering/DynamicResolution.hpp — frame-time governor for render scale
// (Phase 38).
//
// Phase 37 made the internal 3D resolution independent of the output. This is
// the piece that moves it on purpose: when frames run long the governor steps
// the scale DOWN, and when there is headroom it steps it back UP, within a
// minimum and a maximum. The authored `render_scale` is the ceiling, so a game
// can supersample on a fast machine and fall back toward native (or below) when
// the frame budget is missed.
//
// The governor is deliberately a pure CPU object:
//
//   * It knows nothing about Vulkan or the renderer. It consumes frame times in
//     seconds and returns a scale. That makes every decision testable without a
//     GPU, and keeps the runtime from owning policy.
//   * It reacts to a smoothed LOAD sample, not to one frame. A single shader
//     compile hitch must not drop the resolution for the next half second.
//   * It has a cooldown and a hysteresis band. Without both, a controller
//     oscillates every frame between two scales — and every change rebuilds
//     every render target, which is the most expensive thing the renderer does.
//   * It steps additively (default 0.05) rather than snapping to an extreme, so
//     a transient load produces a small change, not a visible resolution pop.
//
// Phase 40 lets the LOAD sample come from real GPU timestamps while the
// cooldown keeps running on CPU wall time. Those are different clocks: 0.5 s of
// GPU work can span several seconds of wall time, so using the GPU sample for
// both would make the cooldown depend on how fast the GPU happens to be. A GPU
// sample is an OPTION with an automatic fallback — a device without timestamps,
// a disabled profiler, or no fresh sample all fall back to CPU frame time. The
// governor itself still touches no GPU API.
//
// The average is reset to the target after every accepted change: the samples
// that justified the change describe the OLD resolution. Keeping them would
// justify a second immediate step as soon as the cooldown expires.
//
// This is not MSAA and not temporal upscaling. It changes how many pixels the
// deferred chain shades; the tonemap pass still does the resolve, exactly as in
// Phase 37. Below 1.0 the benefit is performance and the cost is sharpness;
// above 1.0 the benefit is edge detail and the cost is quadratic fragment work.

#include <NF/Core/Types.hpp>

namespace nf::rendering {

/// Frame-rate bounds shared by the scene loader, the editor and the runtime.
/// Below 10 fps "hold the target" is not a useful request, and above 360 the
/// frame budget is shorter than the cost of rebuilding a target at any sane
/// resolution, so the governor would only thrash.
inline constexpr f32 kMinTargetFps = 10.0f;
inline constexpr f32 kMaxTargetFps = 360.0f;

/// Which clock the governor uses to decide whether the frame is too expensive.
/// `Cpu` is the historical behaviour: the frame duration the runtime already
/// receives. `Gpu` prefers a fresh timestamp sample and falls back to the CPU
/// value when none is available. It never changes the clock used by the
/// cooldown, which is always CPU wall time.
enum class DynamicResolutionTiming : u8 {
    Cpu = 0,
    Gpu = 1,
};

struct DynamicResolutionConfig {
    /// Off by default. A scene that never asks for this renders exactly as it
    /// did before the governor existed.
    bool enabled = false;
    /// The frame budget, in milliseconds. 16.667 = 60 fps, 8.333 = 120 fps.
    f32 target_frame_ms = 16.6667f;
    /// Load clock. `Gpu` is a preference, not a requirement: any missing or
    /// non-positive GPU sample falls back to the CPU frame time.
    DynamicResolutionTiming timing = DynamicResolutionTiming::Cpu;
    /// Lowest allowed scale. Never above max_scale; configure() swaps an
    /// inverted pair rather than silently producing an empty range.
    f32 min_scale = 0.5f;
    /// Highest allowed scale. The runtime sets this to the authored
    /// `render_scale`, so dynamic resolution only ever gives back quality the
    /// author already asked for.
    f32 max_scale = 1.0f;
    /// How far one accepted adjustment moves the scale.
    f32 step = 0.05f;
    /// Minimum time between two accepted adjustments, in seconds.
    f32 cooldown_s = 0.5f;
    /// Hysteresis as a fraction of the target frame time. At 0.10 a frame time
    /// between 90% and 110% of target is "good enough" and changes nothing.
    f32 tolerance = 0.10f;
};

class DynamicResolution {
public:
    /// Identity clamp bounds for the authored scale. Kept here so a caller
    /// constructing a config by hand cannot ask for an illegal resolution.
    static constexpr f32 kDefaultTargetFrameMs = 16.6667f;
    static constexpr f32 kDefaultStep = 0.05f;
    static constexpr f32 kDefaultCooldownSeconds = 0.5f;
    static constexpr f32 kDefaultTolerance = 0.10f;

    /// Clamps every field into a legal range. An inverted min/max pair is
    /// swapped. A non-finite or zero target uses 60 fps. If the governor was
    /// disabled and becomes enabled, the scale returns to max_scale; a config
    /// update that keeps it enabled only clamps the current scale.
    void configure(const DynamicResolutionConfig& config);

    const DynamicResolutionConfig& config() const { return m_config; }
    bool enabled() const { return m_config.enabled; }
    f32 scale() const { return m_scale; }
    /// The source used by the last accepted update. Equal to the configured
    /// source when it had a usable sample, otherwise `Cpu` (the fallback).
    DynamicResolutionTiming last_timing() const { return m_last_timing; }

    /// Forces the current scale and clears the smoothing/cooldown state.
    /// Clamped to the current config's range.
    void reset(f32 scale);

    /// Feeds one CPU frame duration in seconds and returns the scale to use
    /// for the NEXT frame. This is the CPU-only form; a scene configured for
    /// `Gpu` timing cannot get a GPU sample through it and therefore falls back
    /// to this value. A disabled governor, a non-positive time, or a non-finite
    /// time is ignored and returns the current scale unchanged.
    f32 update(f32 frame_time_s);

    /// Feeds both clocks. When `timing` is `Gpu` and `gpu_frame_time_s` is
    /// positive and finite and `gpu_frame_time_valid` is true, the GPU value is
    /// the load sample. Otherwise the CPU value is used. `cpu_frame_time_s` is
    /// also the wall clock that advances the cooldown, even on a GPU sample:
    /// 0.5 s of GPU work is not 0.5 s of real time. A non-positive CPU time with
    /// a usable GPU sample still advances time by the GPU sample, so a caller
    /// without a wall clock can drive the governor.
    f32 update(f32 cpu_frame_time_s, f32 gpu_frame_time_s, bool gpu_frame_time_valid);

private:
    DynamicResolutionConfig m_config{};
    f32 m_scale = 1.0f;
    /// Exponential moving average of (clamped) load samples, in seconds.
    f32 m_average_frame_s = 0.0f;
    /// Wall time accumulated since the last accepted change, in seconds.
    /// Hitches are clamped before they reach this, so a freeze cannot bypass
    /// cooldown.
    f32 m_time_since_change_s = 0.0f;
    DynamicResolutionTiming m_last_timing = DynamicResolutionTiming::Cpu;
    bool m_has_sample = false;
    bool m_has_scale = false;
};

} // namespace nf::rendering
