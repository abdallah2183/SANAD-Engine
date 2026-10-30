#include <NF/Audio/InteractiveMusic.hpp>
#include <algorithm>
#include <cmath>

namespace nf::audio {

BeatClock::BeatClock(f32 bpm, u32 beats_per_bar, u32 bars_per_phrase)
    : m_bpm((bpm > 0.1f) ? bpm : 120.0f),
      m_beats_per_bar((beats_per_bar > 0u) ? beats_per_bar : 4u),
      m_bars_per_phrase((bars_per_phrase > 0u) ? bars_per_phrase : 4u) {
    m_seconds_per_beat = 60.0f / m_bpm;
}

void BeatClock::set_tempo(f32 bpm, u32 beats_per_bar) {
    m_bpm = (bpm > 0.1f) ? bpm : 120.0f;
    m_beats_per_bar = (beats_per_bar > 0u) ? beats_per_bar : 4u;
    m_seconds_per_beat = 60.0f / m_bpm;
}

void BeatClock::reset() {
    m_song_time = 0.0f;
    m_beat_fraction = 0.0f;
    m_current_beat = 0u;
    m_current_bar = 0u;
}

bool BeatClock::advance(f32 dt) {
    if (dt <= 0.0f) {
        return false;
    }

    m_song_time += dt;
    f32 total_beats = m_song_time / m_seconds_per_beat;
    u32 new_beat = static_cast<u32>(total_beats);
    m_beat_fraction = total_beats - static_cast<f32>(new_beat);

    bool crossed_beat = (new_beat != m_current_beat);
    m_current_beat = new_beat;
    m_current_bar = m_current_beat / m_beats_per_bar;

    return crossed_beat;
}

f32 BeatClock::time_until(QuantizationMode mode) const {
    if (mode == QuantizationMode::Immediate) {
        return 0.0f;
    }

    f32 beat_rem = (1.0f - m_beat_fraction) * m_seconds_per_beat;
    if (mode == QuantizationMode::NextBeat) {
        return beat_rem;
    }

    u32 beat_in_bar = m_current_beat % m_beats_per_bar;
    u32 beats_to_bar = (m_beats_per_bar - 1u) - beat_in_bar;
    f32 bar_rem = beat_rem + static_cast<f32>(beats_to_bar) * m_seconds_per_beat;
    if (mode == QuantizationMode::NextBar) {
        return bar_rem;
    }

    // NextPhrase
    u32 bar_in_phrase = m_current_bar % m_bars_per_phrase;
    u32 bars_to_phrase = (m_bars_per_phrase - 1u) - bar_in_phrase;
    return bar_rem + static_cast<f32>(bars_to_phrase) * seconds_per_bar();
}

InteractiveMusicEngine::InteractiveMusicEngine(f32 bpm, u32 beats_per_bar)
    : m_clock(bpm, beats_per_bar) {}

void InteractiveMusicEngine::add_stem(const std::string& name, const AudioBuffer* buffer,
                                      f32 min_intensity, f32 max_intensity, f32 initial_gain) {
    MusicStem stem;
    stem.name = name;
    stem.buffer = buffer;
    stem.min_intensity = min_intensity;
    stem.max_intensity = max_intensity;
    stem.current_gain = initial_gain;
    stem.target_gain = initial_gain;
    stem.fade_speed = 2.0f;
    m_stems.push_back(stem);
}

bool InteractiveMusicEngine::remove_stem(const std::string& name) {
    auto it = std::find_if(m_stems.begin(), m_stems.end(),
                           [&name](const MusicStem& s) { return s.name == name; });
    if (it != m_stems.end()) {
        m_stems.erase(it);
        return true;
    }
    return false;
}

void InteractiveMusicEngine::set_intensity(f32 intensity, f32 transition_time) {
    m_intensity = std::clamp(intensity, 0.0f, 1.0f);
    f32 speed = (transition_time > 0.01f) ? (1.0f / transition_time) : 100.0f;

    for (auto& stem : m_stems) {
        if (m_intensity >= stem.min_intensity && m_intensity <= stem.max_intensity) {
            stem.target_gain = 1.0f;
        } else {
            stem.target_gain = 0.0f;
        }
        stem.fade_speed = speed;
    }
}

bool InteractiveMusicEngine::set_stem_gain(const std::string& name, f32 target_gain, f32 fade_time) {
    for (auto& stem : m_stems) {
        if (stem.name == name) {
            stem.target_gain = std::clamp(target_gain, 0.0f, 1.0f);
            stem.fade_speed = (fade_time > 0.01f) ? (1.0f / fade_time) : 100.0f;
            return true;
        }
    }
    return false;
}

f32 InteractiveMusicEngine::stem_gain(const std::string& name) const {
    for (const auto& stem : m_stems) {
        if (stem.name == name) {
            return stem.current_gain;
        }
    }
    return 0.0f;
}

bool InteractiveMusicEngine::set_stem_muted(const std::string& name, bool muted) {
    for (auto& stem : m_stems) {
        if (stem.name == name) {
            stem.muted = muted;
            return true;
        }
    }
    return false;
}

void InteractiveMusicEngine::play_stinger(const AudioBuffer* buffer, f32 volume,
                                          QuantizationMode quant) {
    if (buffer == nullptr) {
        return;
    }

    f32 delay = m_clock.time_until(quant);
    if (delay <= 1e-4f) {
        MusicalStinger st;
        st.buffer = buffer;
        st.volume = volume;
        st.cursor = 0.0;
        st.active = true;
        m_active_stingers.push_back(st);
    } else {
        QueuedStinger qs;
        qs.stinger.buffer = buffer;
        qs.stinger.volume = volume;
        qs.stinger.cursor = 0.0;
        qs.stinger.active = true;
        qs.delay_remaining = delay;
        qs.active = true;
        m_queued_stingers.push_back(qs);
    }
}

void InteractiveMusicEngine::queue_intensity_transition(f32 target_intensity,
                                                        QuantizationMode quant,
                                                        f32 transition_time) {
    f32 delay = m_clock.time_until(quant);
    if (delay <= 1e-4f) {
        set_intensity(target_intensity, transition_time);
    } else {
        QueuedTransition qt;
        qt.target_intensity = target_intensity;
        qt.transition_time = transition_time;
        qt.delay_remaining = delay;
        qt.active = true;
        m_queued_transitions.push_back(qt);
    }
}

void InteractiveMusicEngine::update(f32 dt) {
    if (dt <= 0.0f) {
        return;
    }

    m_clock.advance(dt);

    // Process queued transitions
    for (auto& qt : m_queued_transitions) {
        if (qt.active) {
            qt.delay_remaining -= dt;
            if (qt.delay_remaining <= 0.0f) {
                set_intensity(qt.target_intensity, qt.transition_time);
                qt.active = false;
            }
        }
    }
    m_queued_transitions.erase(
        std::remove_if(m_queued_transitions.begin(), m_queued_transitions.end(),
                       [](const QueuedTransition& q) { return !q.active; }),
        m_queued_transitions.end());

    // Process queued stingers
    for (auto& qs : m_queued_stingers) {
        if (qs.active) {
            qs.delay_remaining -= dt;
            if (qs.delay_remaining <= 0.0f) {
                m_active_stingers.push_back(qs.stinger);
                qs.active = false;
            }
        }
    }
    m_queued_stingers.erase(
        std::remove_if(m_queued_stingers.begin(), m_queued_stingers.end(),
                       [](const QueuedStinger& q) { return !q.active; }),
        m_queued_stingers.end());

    // Update stem gains
    for (auto& stem : m_stems) {
        if (stem.current_gain < stem.target_gain) {
            stem.current_gain += stem.fade_speed * dt;
            if (stem.current_gain > stem.target_gain) {
                stem.current_gain = stem.target_gain;
            }
        } else if (stem.current_gain > stem.target_gain) {
            stem.current_gain -= stem.fade_speed * dt;
            if (stem.current_gain < stem.target_gain) {
                stem.current_gain = stem.target_gain;
            }
        }
    }
}

void InteractiveMusicEngine::mix(f32* left, f32* right, usize frames, u32 mix_sample_rate) {
    if (left == nullptr || right == nullptr || frames == 0) {
        return;
    }
    if (mix_sample_rate == 0) {
        mix_sample_rate = kDefaultSampleRate;
    }

    // 1. Mix active stems
    for (auto& stem : m_stems) {
        if (stem.muted || stem.current_gain <= 1e-4f || stem.buffer == nullptr) {
            continue;
        }

        const AudioBuffer& buf = *stem.buffer;
        usize frame_count = buf.frame_count();
        if (frame_count == 0 || buf.channels == 0 || buf.sample_rate == 0) {
            continue;
        }

        f64 source_step = static_cast<f64>(buf.sample_rate) / static_cast<f64>(mix_sample_rate);
        f32 gain = stem.current_gain;

        for (usize i = 0; i < frames; ++i) {
            if (stem.cursor >= static_cast<f64>(frame_count)) {
                stem.cursor = std::fmod(stem.cursor, static_cast<f64>(frame_count));
            }

            usize f0 = static_cast<usize>(stem.cursor);
            f64 frac = stem.cursor - static_cast<f64>(f0);
            usize f1 = (f0 + 1 < frame_count) ? (f0 + 1) : 0u;

            usize idx0 = f0 * buf.channels;
            usize idx1 = f1 * buf.channels;

            f32 l0 = buf.samples[idx0];
            f32 l1 = buf.samples[idx1];
            f32 r0 = (buf.channels >= 2) ? buf.samples[idx0 + 1] : l0;
            f32 r1 = (buf.channels >= 2) ? buf.samples[idx1 + 1] : l1;

            f32 sample_l = static_cast<f32>(static_cast<f64>(l0) + (static_cast<f64>(l1) - static_cast<f64>(l0)) * frac);
            f32 sample_r = static_cast<f32>(static_cast<f64>(r0) + (static_cast<f64>(r1) - static_cast<f64>(r0)) * frac);

            left[i] += sample_l * gain;
            right[i] += sample_r * gain;

            stem.cursor += source_step;
        }
    }

    // 2. Mix active stingers (one-shot)
    for (auto& st : m_active_stingers) {
        if (!st.active || st.buffer == nullptr) {
            continue;
        }

        const AudioBuffer& buf = *st.buffer;
        usize frame_count = buf.frame_count();
        if (frame_count == 0 || buf.channels == 0 || buf.sample_rate == 0) {
            st.active = false;
            continue;
        }

        f64 source_step = static_cast<f64>(buf.sample_rate) / static_cast<f64>(mix_sample_rate);
        f32 gain = st.volume;

        for (usize i = 0; i < frames; ++i) {
            if (st.cursor >= static_cast<f64>(frame_count)) {
                st.active = false;
                break;
            }

            usize f0 = static_cast<usize>(st.cursor);
            f64 frac = st.cursor - static_cast<f64>(f0);
            usize f1 = (f0 + 1 < frame_count) ? (f0 + 1) : f0;

            usize idx0 = f0 * buf.channels;
            usize idx1 = f1 * buf.channels;

            f32 l0 = buf.samples[idx0];
            f32 l1 = buf.samples[idx1];
            f32 r0 = (buf.channels >= 2) ? buf.samples[idx0 + 1] : l0;
            f32 r1 = (buf.channels >= 2) ? buf.samples[idx1 + 1] : l1;

            f32 sample_l = static_cast<f32>(static_cast<f64>(l0) + (static_cast<f64>(l1) - static_cast<f64>(l0)) * frac);
            f32 sample_r = static_cast<f32>(static_cast<f64>(r0) + (static_cast<f64>(r1) - static_cast<f64>(r0)) * frac);

            left[i] += sample_l * gain;
            right[i] += sample_r * gain;

            st.cursor += source_step;
        }
    }

    m_active_stingers.erase(
        std::remove_if(m_active_stingers.begin(), m_active_stingers.end(),
                       [](const MusicalStinger& s) { return !s.active; }),
        m_active_stingers.end());
}

void InteractiveMusicEngine::clear() {
    m_stems.clear();
    m_active_stingers.clear();
    m_queued_transitions.clear();
    m_queued_stingers.clear();
    m_clock.reset();
}

} // namespace nf::audio
