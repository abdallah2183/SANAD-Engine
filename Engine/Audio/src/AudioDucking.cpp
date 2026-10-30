// NF/Audio/AudioDucking.cpp — Dynamic Bus Attenuation & Sidechain Compression

#include <NF/Audio/AudioDucking.hpp>
#include <algorithm>

namespace nf::audio {

void AudioDuckingManager::add_rule(const DuckingRule& rule) {
    ActiveDuckingTrack track;
    track.def = rule;
    track.state = DuckingState::Idle;
    track.current_gain = 1.0f;
    track.hold_timer = 0.0f;
    m_rules.push_back(track);
}

void AudioDuckingManager::clear_rules() {
    m_rules.clear();
}

void AudioDuckingManager::update(f32 dt, const f32 bus_levels[kBusCount]) {
    for (auto& track : m_rules) {
        const u32 initiator_idx = static_cast<u32>(track.def.initiator_bus);
        const f32 signal = (initiator_idx < kBusCount) ? bus_levels[initiator_idx] : 0.0f;
        const bool active = signal >= track.def.threshold;

        if (active) {
            track.hold_timer = track.def.hold_time;
            if (track.current_gain > track.def.duck_gain) {
                track.state = DuckingState::Attacking;
                const f32 rate = (1.0f - track.def.duck_gain) / std::max(0.001f, track.def.attack_time);
                track.current_gain = std::max(track.def.duck_gain, track.current_gain - rate * dt);
                if (track.current_gain <= track.def.duck_gain + 1e-4f) {
                    track.current_gain = track.def.duck_gain;
                    track.state = DuckingState::Holding;
                }
            } else {
                track.current_gain = track.def.duck_gain;
                track.state = DuckingState::Holding;
            }
        } else {
            // Signal absent
            f32 remaining_dt = dt;
            if (track.hold_timer > 0.0f) {
                if (track.hold_timer >= remaining_dt) {
                    track.hold_timer -= remaining_dt;
                    track.state = DuckingState::Holding;
                    continue;
                } else {
                    remaining_dt -= track.hold_timer;
                    track.hold_timer = 0.0f;
                }
            }

            // Hold elapsed, release back to normal 1.0
            if (track.current_gain < 1.0f) {
                track.state = DuckingState::Releasing;
                const f32 rate = (1.0f - track.def.duck_gain) / std::max(0.001f, track.def.release_time);
                track.current_gain = std::min(1.0f, track.current_gain + rate * remaining_dt);
                if (track.current_gain >= 0.9999f) {
                    track.current_gain = 1.0f;
                    track.state = DuckingState::Idle;
                }
            } else {
                track.current_gain = 1.0f;
                track.state = DuckingState::Idle;
            }
        }
    }
}

f32 AudioDuckingManager::get_duck_multiplier(BusId bus) const {
    f32 multiplier = 1.0f;
    for (const auto& track : m_rules) {
        if (track.def.target_bus == bus) {
            multiplier = std::min(multiplier, track.current_gain);
        }
    }
    return multiplier;
}

DuckingState AudioDuckingManager::get_rule_state(size_t index) const {
    if (index >= m_rules.size()) return DuckingState::Idle;
    return m_rules[index].state;
}

f32 AudioDuckingManager::get_rule_current_gain(size_t index) const {
    if (index >= m_rules.size()) return 1.0f;
    return m_rules[index].current_gain;
}

void AudioDuckingManager::reset() {
    for (auto& track : m_rules) {
        track.state = DuckingState::Idle;
        track.current_gain = 1.0f;
        track.hold_timer = 0.0f;
    }
}

} // namespace nf::audio
