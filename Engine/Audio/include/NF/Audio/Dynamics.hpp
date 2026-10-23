#pragma once

// NF/Audio/Dynamics.hpp — master-bus dynamics: a soft-knee compressor and a
// lookahead peak limiter.
//
// WHY THIS IS A PLAYER SETTING AND NOT SCENE DATA
//
// A level's mix (how loud the music is against the guns, how much headroom the
// master leaves) is authored. How much a particular player can tolerate is not:
// the same explosion that is exciting on speakers is painful on headphones at
// midnight, and no author can ship one master that fits both. So the venue is
// `PlayerSettings`, the file that already owns volume, sensitivity and shake.
//
// Both stages are OFF by default and their neutral settings are EXACTLY
// identity, which is the contract every optional stage in this engine follows:
// a player who never writes a settings file hears the mix the author shipped,
// bit for bit.
//
// SIGNAL ORDER
//
//   buses * bus volumes * master -> compressor -> limiter -> the device buffer
//
// The limiter is the last thing before the device for one reason: nothing after
// it may add gain. A compressor with makeup gain in front of it is fine; a
// compressor after it would re-introduce the overshoot the limiter just
// removed.

#include <NF/Core/Types.hpp>

#include <vector>

namespace nf::audio {

/// Peak compressor with a soft knee, stereo-linked.
///
/// "Stereo-linked" is not a detail: a per-channel detector shifts the stereo
/// image whenever one side is louder (a hard-panned gunshot pulls only that
/// channel down, and the mix walks left). One detector over `max(|L|, |R|)`
/// turns both channels by the same gain, so the image stays where the mixer
/// put it.
struct CompressorSettings {
    bool enabled = false;
    /// Level above which gain reduction begins, in dBFS. 0 is full scale.
    f32 threshold_db = -18.0f;
    /// Input/output ratio above the threshold. 1 is a no-op; 4 means 4 dB of
    /// input over the threshold becomes 1 dB of output.
    f32 ratio = 3.0f;
    /// Time to reach ~63% of a newly-louder reduction. Short enough to catch a
    /// transient, long enough not to click.
    f32 attack_ms = 10.0f;
    /// Time to give the gain back. This is the audible half: too short and the
    /// mix "breathes" on every beat.
    f32 release_ms = 150.0f;
    /// Width of the quadratic transition around the threshold. 0 is a hard
    /// corner (audible on sustained material); 6-12 dB is musical.
    f32 knee_db = 6.0f;
    /// Static gain added after compression, in dB. The point of compression is
    /// usually to make the quiet part louder, and without makeup it only makes
    /// the loud part quieter.
    f32 makeup_db = 0.0f;

    bool operator==(const CompressorSettings&) const = default;
};

/// Lookahead brickwall limiter.
///
/// The lookahead is what makes it a limiter rather than a clipper: the detector
/// sees a peak `lookahead_ms` before that peak reaches the output, so the gain
/// is already down when the transient arrives, and the waveform is turned down
/// instead of chopped off. The cost is latency, which is why the delay is
/// exposed (`MasterDynamics::limiter_delay_samples`) and why 5 ms is the
/// default rather than a huge safety window.
struct LimiterSettings {
    bool enabled = false;
    /// Maximum output level in dBFS. -0.3 is a common delivery ceiling: a
    /// little headroom for the lossy encoder a game's audio may pass through.
    f32 ceiling_db = -0.3f;
    /// Time to restore gain after the peak has passed. Short rules make the
    /// level pump; long ones make the next quiet moment dull.
    f32 release_ms = 80.0f;
    /// Preview window, in milliseconds. 0 is allowed (no latency, instant
    /// attack from the current sample) but cannot anticipate a transient.
    f32 lookahead_ms = 5.0f;

    bool operator==(const LimiterSettings&) const = default;
};

struct MasterDynamicsSettings {
    CompressorSettings compressor{};
    LimiterSettings limiter{};

    bool operator==(const MasterDynamicsSettings&) const = default;
};

/// The processor. State lives across blocks: a settings change mid-run keeps
/// the current envelope (a slider must not click), and only a sample-rate or
/// lookahead change rebuilds the delay lines.
class MasterDynamics {
public:
    /// Replaces the settings. No state is rebuilt here: the next process()
    /// reconfigures only if the lookahead or sample rate actually changed,
    /// so dragging a threshold slider never clicks.
    void set_settings(const MasterDynamicsSettings& settings) { m_settings = settings; }
    const MasterDynamicsSettings& settings() const { return m_settings; }

    /// Processes `frames` stereo frames IN PLACE. A disabled stage does not
    /// touch a single sample; both disabled is a bit-exact passthrough.
    /// Non-finite settings are replaced by their neutral values rather than
    /// poisoning the output with NaN.
    void process(f32* left, f32* right, usize frames, u32 sample_rate);

    /// Clears the envelopes, meters and delay lines. Call on a scene change.
    void reset();

    /// Current gain reduction of each stage at the end of the last block, in
    /// dB (0 = no reduction). Read by a meter or a test, never by the mix.
    f32 compressor_gain_reduction_db() const { return m_compressor_gr_db; }
    f32 limiter_gain_reduction_db() const { return m_limiter_gr_db; }

    /// Latency the limiter adds, in frames, for the CURRENT sample rate. A
    /// caller that mixes with a lookahead must delay everything else by the
    /// same amount to keep the tracks aligned; the runtime's audio scene mixes
    /// one block at a time, so it only reports this.
    u32 limiter_delay_samples() const { return m_lookahead; }

private:
    void configure(u32 sample_rate, u32 lookahead);

    MasterDynamicsSettings m_settings{};

    // Compressor envelope, in dB of gain reduction (positive = quieter).
    f32 m_compressor_gr_db = 0.0f;

    // Limiter envelope (linear gain) and the window it was derived from.
    f32 m_limiter_gain = 1.0f;
    f32 m_limiter_gr_db = 0.0f;
    u32 m_sample_rate = 0;
    u32 m_lookahead = 0;

    // Sliding maximum over the lookahead window. A monotonic deque stored in a
    // ring: values are decreasing from front to back, so the front is the
    // window's peak. Everything here is preallocated and reused per block.
    struct MaxEntry {
        u64 sequence = 0;
        f32 value = 0.0f;
    };
    std::vector<MaxEntry> m_max_ring;
    usize m_max_head = 0;
    usize m_max_size = 0;

    // Delay line, one frame wide, `m_lookahead` long.
    std::vector<f32> m_delay_left;
    std::vector<f32> m_delay_right;
    usize m_delay_pos = 0;
    u64 m_sequence = 0;
};

} // namespace nf::audio
