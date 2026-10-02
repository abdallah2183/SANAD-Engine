#include <NF/Audio/MusicSystem.hpp>
#include <algorithm>
#include <cmath>

namespace nf::audio {

namespace {

/// Rate for a full-swing linear fade of `seconds` (positive = towards
/// `target`). A non-positive duration snaps the envelope instead of dividing
/// by zero.
f32 fade_rate_for(f32 seconds) {
    if (seconds <= 0.0f) {
        return 0.0f;
    }
    return 1.0f / seconds;
}

} // namespace

void MusicSystem::hand_over(const MusicTrack& next, f32 fade_out_seconds,
                            f32 fade_in_seconds) {
    // Move the live track to the outgoing slot so it keeps sounding while the
    // new one fades in. A track with no buffer or no fade left is dropped.
    if (m_current.active && m_current.track.buffer != nullptr &&
        fade_out_seconds > 0.0f) {
        m_outgoing = m_current;
        m_outgoing.fade_rate = -(m_outgoing.gain / fade_out_seconds);
        m_outgoing.target = 0.0f;
    } else {
        m_outgoing = Slot{};
    }

    m_current = Slot{};
    m_current.track = next;
    m_current.active = next.buffer != nullptr;
    if (m_current.active) {
        m_current.target = 1.0f;
        m_current.fade_rate = fade_rate_for(fade_in_seconds);
        if (fade_in_seconds <= 0.0f) {
            m_current.gain = 1.0f; // snap: no fade, start fully open
        }
    }
}

void MusicSystem::play(const MusicTrack& track, f32 fade_in_seconds) {
    hand_over(track, fade_in_seconds, fade_in_seconds);
    if (!m_current.active) {
        m_state = m_outgoing.active ? MusicState::FadingOut : MusicState::Stopped;
        return;
    }
    m_state = (m_current.gain >= 1.0f) ? MusicState::Playing
                                       : MusicState::FadingIn;
}

void MusicSystem::crossfade(const MusicTrack& next, f32 fade_out_seconds,
                            f32 fade_in_seconds) {
    hand_over(next, fade_out_seconds, fade_in_seconds);
    if (!m_current.active) {
        m_state = m_outgoing.active ? MusicState::FadingOut : MusicState::Stopped;
        return;
    }
    m_state = (m_current.gain >= 1.0f) ? MusicState::Playing
                                       : MusicState::Crossfading;
}

void MusicSystem::stop(f32 fade_out_seconds) {
    if (m_current.active && m_current.track.buffer != nullptr &&
        fade_out_seconds > 0.0f) {
        m_outgoing = m_current;
        m_outgoing.fade_rate = -(m_outgoing.gain / fade_out_seconds);
        m_outgoing.target = 0.0f;
    }
    m_current = Slot{};
    m_state = m_outgoing.active ? MusicState::FadingOut : MusicState::Stopped;
}

void MusicSystem::update(f32 dt) {
    // Current envelope.
    if (m_current.active && m_current.fade_rate != 0.0f) {
        m_current.gain += m_current.fade_rate * dt;
        if (m_current.gain >= m_current.target) {
            m_current.gain = m_current.target;
            m_current.fade_rate = 0.0f;
            m_state = MusicState::Playing;
        } else if (m_current.gain <= 0.0f) {
            m_current.gain = 0.0f;
            m_current.fade_rate = 0.0f;
        }
    }

    // Outgoing envelope: fades to zero, then the slot dies.
    if (m_outgoing.active) {
        m_outgoing.gain += m_outgoing.fade_rate * dt;
        if (m_outgoing.gain <= 0.0f) {
            m_outgoing.gain = 0.0f;
            m_outgoing.fade_rate = 0.0f;
            m_outgoing.active = false;
            m_outgoing.cursor = 0.0;
        }
        // Once the old track is gone a crossfade is just a fade-in.
        if (m_state == MusicState::Crossfading && !m_outgoing.active) {
            m_state = m_current.fade_rate != 0.0f ? MusicState::FadingIn
                                                  : MusicState::Playing;
        }
        if (m_state == MusicState::FadingOut && !m_outgoing.active &&
            !m_current.active) {
            m_state = MusicState::Stopped;
        }
    }

    // Ambience envelope. Direction comes from the sign of the rate, not from
    // `target`: a fade-out targets 0, and testing `gain >= target` would snap
    // it to silence on the first update instead of fading at all.
    if (m_ambience.active && m_ambience.fade_rate != 0.0f) {
        const f32 rate = m_ambience.fade_rate;
        m_ambience.gain += rate * dt;
        if (rate > 0.0f) {
            if (m_ambience.gain >= m_ambience.target) {
                m_ambience.gain = m_ambience.target;
                m_ambience.fade_rate = 0.0f;
            }
        } else if (m_ambience.gain <= 0.0f) {
            m_ambience.gain = 0.0f;
            m_ambience.fade_rate = 0.0f;
            m_ambience.active = false;
            m_ambience.cursor = 0.0;
        }
    }
}

void MusicSystem::mix_slot(Slot& slot, f32* left, f32* right, usize frames,
                           u32 mix_sample_rate) {
    if (!slot.active || slot.track.buffer == nullptr || slot.gain <= 0.0f ||
        frames == 0) {
        return;
    }
    const AudioBuffer& buf = *slot.track.buffer;
    if (buf.samples.empty() || buf.channels == 0 || buf.sample_rate == 0) {
        return;
    }
    const usize frame_count = buf.frame_count();
    if (frame_count == 0) {
        return;
    }
    if (mix_sample_rate == 0) {
        mix_sample_rate = kDefaultSampleRate;
    }

    if (!std::isfinite(slot.cursor) || slot.cursor < 0.0) {
        slot.cursor = 0.0;
    }
    if (slot.cursor >= static_cast<f64>(frame_count)) {
        slot.cursor = std::fmod(slot.cursor, static_cast<f64>(frame_count));
    }

    const f32 level = slot.track.base_volume * slot.gain;
    const f64 source_step =
        static_cast<f64>(buf.sample_rate) / static_cast<f64>(mix_sample_rate);
    for (usize i = 0; i < frames; ++i) {
        if (slot.cursor >= static_cast<f64>(frame_count)) {
            slot.cursor = std::fmod(slot.cursor,
                                    static_cast<f64>(frame_count)); // loop
        }

        const usize frame0 = static_cast<usize>(slot.cursor);
        const f64 fraction = slot.cursor - static_cast<f64>(frame0);
        const usize frame1 = (frame0 + 1 < frame_count) ? frame0 + 1 : 0u;
        const usize index0 = frame0 * buf.channels;
        const usize index1 = frame1 * buf.channels;
        const f32 left0 = buf.samples[index0];
        const f32 left1 = buf.samples[index1];
        const f32 right0 = buf.channels >= 2 ? buf.samples[index0 + 1] : left0;
        const f32 right1 = buf.channels >= 2 ? buf.samples[index1 + 1] : left1;
        const f32 sample_left = static_cast<f32>(
            static_cast<f64>(left0) +
            (static_cast<f64>(left1) - static_cast<f64>(left0)) * fraction);
        const f32 sample_right = static_cast<f32>(
            static_cast<f64>(right0) +
            (static_cast<f64>(right1) - static_cast<f64>(right0)) * fraction);

        left[i] += sample_left * level;
        right[i] += sample_right * level;
        slot.cursor += source_step;
    }
}

void MusicSystem::mix_music(f32* left, f32* right, usize frames) {
    mix_music(left, right, frames, kDefaultSampleRate);
}

void MusicSystem::mix_music(f32* left, f32* right, usize frames,
                            u32 mix_sample_rate) {
    mix_slot(m_current, left, right, frames, mix_sample_rate);
    mix_slot(m_outgoing, left, right, frames, mix_sample_rate);
}

void MusicSystem::mix_ambience(f32* left, f32* right, usize frames) {
    mix_ambience(left, right, frames, kDefaultSampleRate);
}

void MusicSystem::mix_ambience(f32* left, f32* right, usize frames,
                               u32 mix_sample_rate) {
    mix_slot(m_ambience, left, right, frames, mix_sample_rate);
}

void MusicSystem::set_ambience(const AudioBuffer* buffer,
                               f32 fade_in_seconds) {
    m_ambience = Slot{};
    m_ambience.track.buffer = buffer;
    m_ambience.track.base_volume = 1.0f;
    m_ambience.active = buffer != nullptr;
    m_ambience.target = 1.0f;
    if (m_ambience.active) {
        if (fade_in_seconds > 0.0f) {
            m_ambience.fade_rate = 1.0f / fade_in_seconds;
        } else {
            m_ambience.gain = 1.0f;
        }
    }
}

void MusicSystem::clear_ambience(f32 fade_out_seconds) {
    if (!m_ambience.active) {
        return;
    }
    if (fade_out_seconds > 0.0f) {
        m_ambience.fade_rate = -(m_ambience.gain / fade_out_seconds);
        m_ambience.target = 0.0f;
    } else {
        m_ambience = Slot{};
    }
}

} // namespace nf::audio
