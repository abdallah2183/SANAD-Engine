#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Animation/AnimNotify.hpp>
#include <string>
#include <vector>

namespace nf::animation {

struct MontageSection {
    std::string name;
    f32 start_time = 0.0f;
    f32 duration = 0.0f;
    std::string next_section; // Section to chain to automatically upon completion (empty = stop)
};

/// Composite animation montage with branching combo sections and notify timelines
class AnimMontage {
public:
    explicit AnimMontage(std::string name = "Montage", f32 total_duration = 0.0f);

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void set_name(std::string name) { m_name = std::move(name); }

    [[nodiscard]] f32 total_duration() const noexcept { return m_total_duration; }
    void set_total_duration(f32 duration) { m_total_duration = std::max(0.0f, duration); }

    void add_section(std::string section_name, f32 start_time, f32 duration, std::string next_section = "");
    [[nodiscard]] size_t section_count() const noexcept { return m_sections.size(); }
    [[nodiscard]] const MontageSection* find_section(const std::string& section_name) const;

    /// Set or change the linkage between sections
    bool set_next_section(const std::string& section_name, const std::string& next_section);

    /// Access the embedded timeline
    AnimTimeline& timeline() noexcept { return m_timeline; }
    [[nodiscard]] const AnimTimeline& timeline() const noexcept { return m_timeline; }

    // Playback controls
    void play(const std::string& start_section = "");
    void stop();
    void pause();
    void resume();

    [[nodiscard]] bool is_playing() const noexcept { return m_is_playing; }
    [[nodiscard]] f32 current_time() const noexcept { return m_current_time; }
    [[nodiscard]] const std::string& current_section() const noexcept { return m_current_section; }

    /// Jump directly to a named section
    bool jump_to_section(const std::string& section_name);

    /// Advance montage playback and evaluate triggered notifies
    std::vector<TriggeredNotify> advance(f32 dt);

private:
    std::string m_name;
    f32 m_total_duration = 0.0f;
    std::vector<MontageSection> m_sections;
    AnimTimeline m_timeline;

    bool m_is_playing = false;
    f32 m_current_time = 0.0f;
    std::string m_current_section;
};

} // namespace nf::animation
