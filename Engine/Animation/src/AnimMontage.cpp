#include <NF/Animation/AnimMontage.hpp>
#include <algorithm>

namespace nf::animation {

AnimMontage::AnimMontage(std::string name, f32 total_duration)
    : m_name(std::move(name))
    , m_total_duration(std::max(0.0f, total_duration)) {}

void AnimMontage::add_section(std::string section_name, f32 start_time, f32 duration, std::string next_section) {
    MontageSection s;
    s.name = std::move(section_name);
    s.start_time = std::max(0.0f, start_time);
    s.duration = std::max(0.001f, duration);
    s.next_section = std::move(next_section);

    if (s.start_time + s.duration > m_total_duration) {
        m_total_duration = s.start_time + s.duration;
    }
    m_sections.push_back(std::move(s));
}

const MontageSection* AnimMontage::find_section(const std::string& section_name) const {
    for (const auto& s : m_sections) {
        if (s.name == section_name) {
            return &s;
        }
    }
    return nullptr;
}

bool AnimMontage::set_next_section(const std::string& section_name, const std::string& next_section) {
    for (auto& s : m_sections) {
        if (s.name == section_name) {
            s.next_section = next_section;
            return true;
        }
    }
    return false;
}

void AnimMontage::play(const std::string& start_section) {
    m_is_playing = true;
    if (!start_section.empty()) {
        jump_to_section(start_section);
    } else if (!m_sections.empty()) {
        jump_to_section(m_sections.front().name);
    } else {
        m_current_time = 0.0f;
        m_current_section.clear();
    }
}

void AnimMontage::stop() {
    m_is_playing = false;
    m_current_time = 0.0f;
    m_current_section.clear();
}

void AnimMontage::pause() {
    m_is_playing = false;
}

void AnimMontage::resume() {
    if (!m_sections.empty()) {
        m_is_playing = true;
    }
}

bool AnimMontage::jump_to_section(const std::string& section_name) {
    const auto* sec = find_section(section_name);
    if (!sec) {
        return false;
    }
    m_current_section = sec->name;
    m_current_time = sec->start_time;
    return true;
}

std::vector<TriggeredNotify> AnimMontage::advance(f32 dt) {
    std::vector<TriggeredNotify> all_events;
    if (!m_is_playing || dt <= 0.0f) {
        return all_events;
    }

    if (m_sections.empty()) {
        const f32 prev_t = m_current_time;
        m_current_time = std::min(m_total_duration, m_current_time + dt);
        all_events = m_timeline.advance(prev_t, m_current_time, m_total_duration, false);
        if (m_current_time >= m_total_duration) {
            m_is_playing = false;
        }
        return all_events;
    }

    const auto* sec = find_section(m_current_section);
    if (!sec) {
        sec = &m_sections.front();
        m_current_section = sec->name;
        m_current_time = sec->start_time;
    }

    f32 remaining_dt = dt;
    while (remaining_dt > 0.0f && m_is_playing) {
        sec = find_section(m_current_section);
        if (!sec) {
            m_is_playing = false;
            break;
        }

        const f32 sec_end = sec->start_time + sec->duration;
        const f32 time_left_in_sec = sec_end - m_current_time;

        if (remaining_dt < time_left_in_sec) {
            // Step stays within current section
            const f32 prev_t = m_current_time;
            m_current_time += remaining_dt;
            auto evts = m_timeline.advance(prev_t, m_current_time, m_total_duration, false);
            all_events.insert(all_events.end(), evts.begin(), evts.end());
            remaining_dt = 0.0f;
        } else {
            // Reaches or crosses section end
            const f32 prev_t = m_current_time;
            m_current_time = sec_end;
            auto evts = m_timeline.advance(prev_t, sec_end, m_total_duration, false);
            all_events.insert(all_events.end(), evts.begin(), evts.end());

            remaining_dt -= time_left_in_sec;

            if (sec->next_section.empty()) {
                // Montage finished
                m_is_playing = false;
                break;
            } else {
                const auto* next_sec = find_section(sec->next_section);
                if (!next_sec) {
                    m_is_playing = false;
                    break;
                }
                m_current_section = next_sec->name;
                m_current_time = next_sec->start_time;
            }
        }
    }

    return all_events;
}

} // namespace nf::animation
