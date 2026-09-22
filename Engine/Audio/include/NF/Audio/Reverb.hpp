#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <vector>

namespace nf::audio {

/// A spherical reverb zone: anywhere the listener stands inside `inner_radius`
/// gets the full effect; the effect falls off linearly to nothing at `radius`.
///
/// The zone is authored as discrete echoes (a pre-delay tap followed by a
/// feedback comb) rather than a dense convolution tail: every parameter is
/// headless-testable with exact numbers, and the CPU cost is a few adds per
/// frame instead of an FFT. A cave is `pre_delay ~0.03`, `spacing ~0.11`
/// (about a 37 m round trip), `decay ~1.5`; a bathroom is tighter and brighter.
struct ReverbZone {
    Vec3 position{};
    /// Effect reaches zero at this distance from `position`.
    f32 radius = 10.0f;
    /// Full effect anywhere inside this distance (must be <= radius).
    f32 inner_radius = 2.0f;
    /// Wet level (0..1) applied to the echo output at full effect.
    f32 wet_gain = 0.35f;
    /// Seconds for the echo tail to fall 60 dB (drives the comb feedback).
    f32 decay_seconds = 1.5f;
    /// Seconds of silence before the first echo returns.
    f32 pre_delay_seconds = 0.03f;
    /// Seconds between successive echoes.
    f32 echo_spacing_seconds = 0.11f;
};

/// The reverb the listener currently hears — the resolved contribution of the
/// strongest zone containing them, ready to hand to EchoProcessor::configure.
struct ReverbSample {
    f32 wet_gain = 0.0f;
    f32 decay_seconds = 0.0f;
    f32 pre_delay_seconds = 0.0f;
    f32 echo_spacing_seconds = 0.0f;
    /// Index into the caller's zone array of the winning zone (0 when none).
    u32 zone_index = 0;
    /// False when the listener is outside every zone (fully dry).
    bool active = false;
};

/// Sample the zone list at the listener position. Strongest zone wins: each
/// zone's weight is 1 inside `inner_radius`, falling linearly to 0 at
/// `radius`, and the zone with the highest weight supplies the parameters
/// (first-listed wins ties). `wet_gain` in the result is scaled by that
/// weight, so standing near a zone edge is drier than standing at its heart.
ReverbSample compute_reverb_at(const ReverbZone* zones, usize count,
                               const Vec3& listener);

/// Pre-delay + feedback comb echo line. One instance per reverberant source;
/// the engine owns the instances so the tail survives frame to frame.
///
/// Signal path: dry -> comb (delay = echo spacing, feedback from decay) ->
/// pre-delay ring -> wet, scaled by `wet_gain`. The feedback is chosen so the
/// tail is ~60 dB down after `decay_seconds`:
///   feedback = 0.001 ^ (echo_spacing / decay)
/// which makes the impulse response exactly predictable:
///   echo n lands at frame (pre_delay + n * spacing) with gain wet * feedback^(n-1).
class EchoProcessor {
public:
    /// (Re)configure from a listener sample. Safe to call every time the zone
    /// changes; clears any tail built so far.
    void configure(u32 sample_rate, const ReverbSample& reverb);
    /// Drop the tail without changing the configuration.
    void reset();
    /// Read `dry`, write the echo-only wet signal (dry is NOT copied through —
    /// the caller mixes dry + wet). Buffers must be `frames` long.
    void process(const f32* dry, f32* wet, usize frames);

    f32 feedback() const { return m_feedback; }
    f32 wet_gain() const { return m_wet; }
    usize pre_delay_frames() const { return m_pre_delay; }
    usize spacing_frames() const { return m_spacing; }

private:
    std::vector<f32> m_comb;
    std::vector<f32> m_pre;
    usize m_comb_cursor = 0;
    usize m_pre_cursor = 0;
    usize m_spacing = 0;
    usize m_pre_delay = 0;
    f32 m_feedback = 0.0f;
    f32 m_wet = 0.0f;
};

} // namespace nf::audio
