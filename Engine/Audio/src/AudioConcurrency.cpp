// NF/Audio/AudioConcurrency.cpp — Voice Limiting, Prioritization & Stealing System

#include <NF/Audio/AudioConcurrency.hpp>
#include <algorithm>

namespace nf::audio {

u32 AudioConcurrencyManager::register_group(const ConcurrencyGroup& group) {
    ConcurrencyGroup g = group;
    g.id = m_next_group_id++;
    m_groups.push_back(g);
    return g.id;
}

const ConcurrencyGroup* AudioConcurrencyManager::get_group(u32 group_id) const {
    for (const auto& g : m_groups) {
        if (g.id == group_id) return &g;
    }
    return nullptr;
}

ConcurrencyGroup* AudioConcurrencyManager::get_group(u32 group_id) {
    for (auto& g : m_groups) {
        if (g.id == group_id) return &g;
    }
    return nullptr;
}

u64 AudioConcurrencyManager::select_victim(const ConcurrencyGroup& group, const ActiveVoice& candidate) const {
    std::vector<const ActiveVoice*> candidates;
    for (const auto& pair : m_active_voices) {
        if (pair.second.group_id == group.id && !pair.second.is_fading_out) {
            candidates.push_back(&pair.second);
        }
    }

    if (candidates.empty()) return 0;

    switch (group.steal_policy) {
        case VoiceStealPolicy::PreventNew:
            return 0; // Reject candidate

        case VoiceStealPolicy::StopOldest: {
            auto oldest = std::max_element(candidates.begin(), candidates.end(), [](const ActiveVoice* a, const ActiveVoice* b) {
                return a->age_seconds < b->age_seconds;
            });
            return (*oldest)->voice_id;
        }

        case VoiceStealPolicy::StopFarthest: {
            auto farthest = std::max_element(candidates.begin(), candidates.end(), [](const ActiveVoice* a, const ActiveVoice* b) {
                return a->distance < b->distance;
            });
            return (*farthest)->voice_id;
        }

        case VoiceStealPolicy::StopQuietest: {
            auto quietest = std::min_element(candidates.begin(), candidates.end(), [](const ActiveVoice* a, const ActiveVoice* b) {
                return a->volume < b->volume;
            });
            return (*quietest)->voice_id;
        }

        case VoiceStealPolicy::StopLowestPriority: {
            auto lowest = std::min_element(candidates.begin(), candidates.end(), [](const ActiveVoice* a, const ActiveVoice* b) {
                if (a->priority != b->priority) return a->priority < b->priority;
                return a->age_seconds > b->age_seconds; // Older is less preferred if tie
            });
            if ((*lowest)->priority > candidate.priority) {
                return 0; // All active voices have strictly higher priority than candidate
            }
            return (*lowest)->voice_id;
        }
    }

    return 0;
}

bool AudioConcurrencyManager::request_voice(
    u32 group_id,
    u64 voice_id,
    const std::string& sound_name,
    f32 volume,
    f32 distance,
    std::optional<u32> priority,
    u64& out_stolen_voice_id
) {
    out_stolen_voice_id = 0;
    const ConcurrencyGroup* group = get_group(group_id);
    if (!group) {
        // Unmanaged or default group: always allow
        ActiveVoice v{};
        v.voice_id = voice_id;
        v.sound_name = sound_name;
        v.group_id = 0;
        v.volume = volume;
        v.distance = distance;
        v.priority = priority.value_or(128);
        m_active_voices[voice_id] = v;
        return true;
    }

    // Count currently audible (non-fading) voices in this group
    u32 active_count = 0;
    for (const auto& pair : m_active_voices) {
        if (pair.second.group_id == group_id && !pair.second.is_fading_out) {
            active_count++;
        }
    }

    ActiveVoice candidate{};
    candidate.voice_id = voice_id;
    candidate.sound_name = sound_name;
    candidate.group_id = group_id;
    candidate.volume = volume;
    candidate.distance = distance;
    candidate.priority = priority.value_or(group->default_priority);

    if (active_count >= group->max_instances) {
        const u64 victim_id = select_victim(*group, candidate);
        if (victim_id == 0) {
            return false; // Rejected by concurrency policy
        }

        // Trigger micro-fade on the victim
        auto victim_it = m_active_voices.find(victim_id);
        if (victim_it != m_active_voices.end()) {
            victim_it->second.is_fading_out = true;
            victim_it->second.fade_total_time = std::max(0.001f, group->voice_steal_fade_time);
            victim_it->second.fade_time_remaining = victim_it->second.fade_total_time;
            out_stolen_voice_id = victim_id;
        }
    }

    m_active_voices[voice_id] = candidate;
    return true;
}

void AudioConcurrencyManager::release_voice(u64 voice_id) {
    m_active_voices.erase(voice_id);
}

void AudioConcurrencyManager::update_voice_state(u64 voice_id, f32 volume, f32 distance) {
    auto it = m_active_voices.find(voice_id);
    if (it != m_active_voices.end()) {
        it->second.volume = volume;
        it->second.distance = distance;
    }
}

void AudioConcurrencyManager::update(f32 dt) {
    std::vector<u64> to_remove;

    for (auto& pair : m_active_voices) {
        pair.second.age_seconds += dt;
        if (pair.second.is_fading_out) {
            pair.second.fade_time_remaining -= dt;
            if (pair.second.fade_time_remaining <= 0.0f) {
                to_remove.push_back(pair.first);
            }
        }
    }

    for (u64 id : to_remove) {
        m_active_voices.erase(id);
    }
}

f32 AudioConcurrencyManager::get_voice_fade_gain(u64 voice_id) const {
    auto it = m_active_voices.find(voice_id);
    if (it == m_active_voices.end()) return 0.0f;
    if (!it->second.is_fading_out) return 1.0f;
    if (it->second.fade_total_time <= 1e-6f) return 0.0f;
    return std::clamp(it->second.fade_time_remaining / it->second.fade_total_time, 0.0f, 1.0f);
}

bool AudioConcurrencyManager::is_voice_active(u64 voice_id) const {
    return m_active_voices.find(voice_id) != m_active_voices.end();
}

bool AudioConcurrencyManager::is_voice_fading_out(u64 voice_id) const {
    auto it = m_active_voices.find(voice_id);
    if (it == m_active_voices.end()) return false;
    return it->second.is_fading_out;
}

size_t AudioConcurrencyManager::active_group_voices(u32 group_id) const {
    size_t count = 0;
    for (const auto& pair : m_active_voices) {
        if (pair.second.group_id == group_id && !pair.second.is_fading_out) {
            count++;
        }
    }
    return count;
}

void AudioConcurrencyManager::clear_all() {
    m_active_voices.clear();
    m_groups.clear();
}

} // namespace nf::audio
