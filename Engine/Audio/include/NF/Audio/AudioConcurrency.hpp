#pragma once

// NF/Audio/AudioConcurrency.hpp — Voice Limiting, Prioritization & Stealing System

#include <NF/Core/Types.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace nf::audio {

enum class VoiceStealPolicy : u8 {
    PreventNew = 0,      ///< Reject the new sound if the group is full
    StopOldest,          ///< Steal the voice that has been playing longest
    StopFarthest,        ///< Steal the voice furthest from the listener
    StopQuietest,        ///< Steal the voice with lowest perceived volume
    StopLowestPriority   ///< Steal lowest priority; break ties with oldest
};

struct ConcurrencyGroup {
    u32 id = 0;
    std::string name;
    u32 max_instances = 4;
    VoiceStealPolicy steal_policy = VoiceStealPolicy::StopOldest;
    f32 voice_steal_fade_time = 0.02f; ///< 20ms micro-fade to prevent pops/clicks
    u32 default_priority = 128;        ///< 0 (lowest) to 255 (highest)
};

struct ActiveVoice {
    u64 voice_id = 0;
    std::string sound_name;
    u32 group_id = 0;
    f32 volume = 1.0f;
    f32 distance = 0.0f;
    f32 age_seconds = 0.0f;
    u32 priority = 128;
    bool is_fading_out = false;
    f32 fade_time_remaining = 0.0f;
    f32 fade_total_time = 0.02f;
};

/// Controls simultaneous audio voice limits, prioritization, and smooth voice stealing.
class AudioConcurrencyManager {
public:
    AudioConcurrencyManager() = default;

    /// Registers a new concurrency group and returns its unique ID.
    u32 register_group(const ConcurrencyGroup& group);

    const ConcurrencyGroup* get_group(u32 group_id) const;
    ConcurrencyGroup* get_group(u32 group_id);
    size_t group_count() const noexcept { return m_groups.size(); }

    /// Requests to start playing a voice under a concurrency group.
    /// Returns true if accepted (either under limit or stole an existing voice).
    /// If an existing voice was chosen to be stolen, its ID is written to out_stolen_voice_id.
    bool request_voice(
        u32 group_id,
        u64 voice_id,
        const std::string& sound_name,
        f32 volume,
        f32 distance,
        std::optional<u32> priority,
        u64& out_stolen_voice_id
    );

    /// Releases/stops a voice when playback finishes normally.
    void release_voice(u64 voice_id);

    /// Updates dynamic runtime attributes of an active voice (e.g. moving source).
    void update_voice_state(u64 voice_id, f32 volume, f32 distance);

    /// Advances voice ages and micro-fades by dt seconds. Evicts completed stolen voices.
    void update(f32 dt);

    /// Returns the effective fade gain [0.0, 1.0] for a voice (handles micro-fade out).
    f32 get_voice_fade_gain(u64 voice_id) const;

    /// Checks if a voice is alive and active.
    bool is_voice_active(u64 voice_id) const;

    /// Checks if a voice is in the process of being stolen (fading out).
    bool is_voice_fading_out(u64 voice_id) const;

    /// Returns number of active voices currently playing in a group.
    size_t active_group_voices(u32 group_id) const;

    /// Returns total active voices across all groups.
    size_t total_active_voices() const noexcept { return m_active_voices.size(); }

    void clear_all();

private:
    u32 m_next_group_id = 1;
    std::vector<ConcurrencyGroup> m_groups;
    std::unordered_map<u64, ActiveVoice> m_active_voices;

    u64 select_victim(const ConcurrencyGroup& group, const ActiveVoice& candidate) const;
};

} // namespace nf::audio
