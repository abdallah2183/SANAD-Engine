#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Audio/AudioDSP.hpp>
#include <string>
#include <vector>
#include <optional>

namespace nf::audio {

struct SoundVariant {
    std::string sound_name;
    f32 weight = 1.0f;
    f32 volume_scale = 1.0f;
    f32 pitch_scale = 1.0f;
};

struct SoundCueResult {
    std::string sound_name;
    f32 final_volume = 1.0f;
    f32 final_pitch = 1.0f;
    u32 variant_index = 0;
};

/// SoundCue: Multi-variant procedural sound definition with pitch/volume randomization
/// and anti-repetition history.
class SoundCue {
public:
    SoundCue() = default;
    explicit SoundCue(std::string name);

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void set_name(std::string name) { m_name = std::move(name); }

    void add_variant(std::string sound_name, f32 weight = 1.0f, f32 volume = 1.0f, f32 pitch = 1.0f);
    [[nodiscard]] size_t variant_count() const noexcept { return m_variants.size(); }
    [[nodiscard]] const SoundVariant& variant(size_t index) const { return m_variants[index]; }

    void set_pitch_range(f32 min_pitch, f32 max_pitch);
    [[nodiscard]] f32 pitch_min() const noexcept { return m_pitch_min; }
    [[nodiscard]] f32 pitch_max() const noexcept { return m_pitch_max; }

    void set_volume_range(f32 min_vol, f32 max_vol);
    [[nodiscard]] f32 volume_min() const noexcept { return m_volume_min; }
    [[nodiscard]] f32 volume_max() const noexcept { return m_volume_max; }

    /// Anti-repetition: don't replay the last K picked variants (default 1)
    void set_no_repeat_count(size_t count) { m_no_repeat_count = count; }
    [[nodiscard]] size_t no_repeat_count() const noexcept { return m_no_repeat_count; }

    /// Optional DSP filter configuration for this cue
    void enable_filter(FilterType type, f32 cutoff_hz, f32 q = 0.7071f);
    void disable_filter() { m_has_filter = false; }
    [[nodiscard]] bool has_filter() const noexcept { return m_has_filter; }
    [[nodiscard]] FilterType filter_type() const noexcept { return m_filter_type; }
    [[nodiscard]] f32 filter_cutoff() const noexcept { return m_filter_cutoff; }
    [[nodiscard]] f32 filter_q() const noexcept { return m_filter_q; }

    /// Evaluate the cue using normalized [0, 1] random parameters
    SoundCueResult evaluate(f32 rand_variant, f32 rand_vol = 0.5f, f32 rand_pitch = 0.5f);

    /// Reset playback history
    void reset_history();

private:
    std::string m_name;
    std::vector<SoundVariant> m_variants;
    f32 m_pitch_min = 1.0f;
    f32 m_pitch_max = 1.0f;
    f32 m_volume_min = 1.0f;
    f32 m_volume_max = 1.0f;
    size_t m_no_repeat_count = 1;
    std::vector<size_t> m_recent_history;

    bool m_has_filter = false;
    FilterType m_filter_type = FilterType::LowPass;
    f32 m_filter_cutoff = 1000.0f;
    f32 m_filter_q = 0.7071f;
};

} // namespace nf::audio
