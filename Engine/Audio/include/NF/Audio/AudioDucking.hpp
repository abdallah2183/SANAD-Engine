#pragma once

// NF/Audio/AudioDucking.hpp — Dynamic Bus Attenuation & Sidechain Compression

#include <NF/Core/Types.hpp>
#include <NF/Audio/Buses.hpp>

#include <vector>

namespace nf::audio {

enum class DuckingState : u8 {
    Idle = 0,
    Attacking,
    Holding,
    Releasing
};

/// Authoring definition for dynamic ducking between audio buses.
struct DuckingRule {
    BusId initiator_bus = BusId::Voice;    ///< Bus whose activity triggers ducking (e.g. Dialogue)
    BusId target_bus = BusId::Music;       ///< Bus that gets attenuated (e.g. Background Music)
    f32 duck_gain = 0.25f;                 ///< Attenuated gain floor [0.0, 1.0] (e.g. 0.25 = -12 dB)
    f32 attack_time = 0.1f;                ///< Seconds to transition down from 1.0 to duck_gain
    f32 hold_time = 0.25f;                 ///< Seconds to stay at duck_gain after initiator goes silent
    f32 release_time = 0.5f;               ///< Seconds to ramp back up from duck_gain to 1.0
    f32 threshold = 0.01f;                 ///< Minimum initiator signal level to trigger ducking
};

/// Manages multiple ducking rules across audio buses with deterministic envelope states.
class AudioDuckingManager {
public:
    AudioDuckingManager() = default;

    void add_rule(const DuckingRule& rule);
    void clear_rules();
    [[nodiscard]] size_t rule_count() const noexcept { return m_rules.size(); }
    [[nodiscard]] const DuckingRule& rule(size_t index) const { return m_rules[index].def; }

    /// Advances ducking envelopes given dt seconds and the latest peak levels for each bus.
    void update(f32 dt, const f32 bus_levels[kBusCount]);

    /// Returns the combined ducking multiplier [0.0, 1.0] for a target bus.
    /// If multiple rules target the same bus, the most restrictive (lowest) gain wins.
    [[nodiscard]] f32 get_duck_multiplier(BusId bus) const;

    /// State inspection for diagnostics
    [[nodiscard]] DuckingState get_rule_state(size_t index) const;
    [[nodiscard]] f32 get_rule_current_gain(size_t index) const;

    /// Resets all ducking envelopes back to idle and fully unattenuated (1.0).
    void reset();

private:
    struct ActiveDuckingTrack {
        DuckingRule def;
        DuckingState state = DuckingState::Idle;
        f32 current_gain = 1.0f;
        f32 hold_timer = 0.0f;
    };

    std::vector<ActiveDuckingTrack> m_rules;
};

} // namespace nf::audio
