#include <NF/Audio/SoundCue.hpp>
#include <algorithm>
#include <numeric>

namespace nf::audio {

SoundCue::SoundCue(std::string name)
    : m_name(std::move(name)) {}

void SoundCue::add_variant(std::string sound_name, f32 weight, f32 volume, f32 pitch) {
    SoundVariant v;
    v.sound_name = std::move(sound_name);
    v.weight = std::max(0.001f, weight);
    v.volume_scale = std::max(0.0f, volume);
    v.pitch_scale = std::max(0.01f, pitch);
    m_variants.push_back(std::move(v));
}

void SoundCue::set_pitch_range(f32 min_pitch, f32 max_pitch) {
    m_pitch_min = std::max(0.01f, min_pitch);
    m_pitch_max = std::max(m_pitch_min, max_pitch);
}

void SoundCue::set_volume_range(f32 min_vol, f32 max_vol) {
    m_volume_min = std::max(0.0f, min_vol);
    m_volume_max = std::max(m_volume_min, max_vol);
}

void SoundCue::enable_filter(FilterType type, f32 cutoff_hz, f32 q) {
    m_has_filter = true;
    m_filter_type = type;
    m_filter_cutoff = cutoff_hz;
    m_filter_q = q;
}

SoundCueResult SoundCue::evaluate(f32 rand_variant, f32 rand_vol, f32 rand_pitch) {
    SoundCueResult result;
    if (m_variants.empty()) {
        return result;
    }

    if (m_variants.size() == 1) {
        result.variant_index = 0;
        result.sound_name = m_variants[0].sound_name;
        const f32 v = m_volume_min + (m_volume_max - m_volume_min) * std::clamp(rand_vol, 0.0f, 1.0f);
        const f32 p = m_pitch_min + (m_pitch_max - m_pitch_min) * std::clamp(rand_pitch, 0.0f, 1.0f);
        result.final_volume = m_variants[0].volume_scale * v;
        result.final_pitch = m_variants[0].pitch_scale * p;
        return result;
    }

    // Determine eligible candidates considering anti-repetition history
    std::vector<size_t> eligible;
    eligible.reserve(m_variants.size());

    for (size_t i = 0; i < m_variants.size(); ++i) {
        const bool in_recent = std::find(m_recent_history.begin(), m_recent_history.end(), i) != m_recent_history.end();
        if (!in_recent) {
            eligible.push_back(i);
        }
    }

    // If all variants are in recent history (or history equals/exceeds variant count), allow all
    if (eligible.empty()) {
        for (size_t i = 0; i < m_variants.size(); ++i) {
            eligible.push_back(i);
        }
    }

    // Sum weights of eligible variants
    f32 total_weight = 0.0f;
    for (size_t idx : eligible) {
        total_weight += m_variants[idx].weight;
    }

    // Pick candidate based on rand_variant
    const f32 roll = std::clamp(rand_variant, 0.0f, 0.9999f) * total_weight;
    f32 acc = 0.0f;
    size_t chosen_idx = eligible.front();

    for (size_t idx : eligible) {
        acc += m_variants[idx].weight;
        if (roll < acc) {
            chosen_idx = idx;
            break;
        }
    }

    // Update recent history
    if (m_no_repeat_count > 0) {
        m_recent_history.push_back(chosen_idx);
        while (m_recent_history.size() > m_no_repeat_count) {
            m_recent_history.erase(m_recent_history.begin());
        }
    }

    const auto& chosen = m_variants[chosen_idx];
    result.variant_index = static_cast<u32>(chosen_idx);
    result.sound_name = chosen.sound_name;

    const f32 v = m_volume_min + (m_volume_max - m_volume_min) * std::clamp(rand_vol, 0.0f, 1.0f);
    const f32 p = m_pitch_min + (m_pitch_max - m_pitch_min) * std::clamp(rand_pitch, 0.0f, 1.0f);
    result.final_volume = chosen.volume_scale * v;
    result.final_pitch = chosen.pitch_scale * p;

    return result;
}

void SoundCue::reset_history() {
    m_recent_history.clear();
}

} // namespace nf::audio
