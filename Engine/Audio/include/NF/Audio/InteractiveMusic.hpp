#pragma once

#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/MusicSystem.hpp>
#include <NF/Core/Types.hpp>

#include <string>
#include <vector>

namespace nf::audio {

/// Quantization boundary for musically aligned transitions.
enum class QuantizationMode : u32 {
    Immediate = 0, // Transition begins immediately
    NextBeat,      // Transition begins at the start of the next beat
    NextBar,       // Transition begins at the start of the next bar (measure)
    NextPhrase,    // Transition begins at the next 4-bar phrase
};

/// Tracks musical tempo, beats, bars, and time progression.
class BeatClock {
public:
    BeatClock(f32 bpm = 120.0f, u32 beats_per_bar = 4u, u32 bars_per_phrase = 4u);

    void set_tempo(f32 bpm, u32 beats_per_bar = 4u);
    void reset();

    /// Advances the clock by dt seconds. Returns true if a beat boundary was crossed.
    bool advance(f32 dt);

    f32 bpm() const { return m_bpm; }
    f32 seconds_per_beat() const { return m_seconds_per_beat; }
    f32 seconds_per_bar() const { return m_seconds_per_beat * static_cast<f32>(m_beats_per_bar); }

    u32 beats_per_bar() const { return m_beats_per_bar; }
    u32 current_beat() const { return m_current_beat; }
    u32 current_bar() const { return m_current_bar; }
    f32 beat_fraction() const { return m_beat_fraction; } // [0, 1) through current beat
    f32 song_time() const { return m_song_time; }

    /// Calculates seconds remaining until the next specified quantization boundary.
    f32 time_until(QuantizationMode mode) const;

private:
    f32 m_bpm = 120.0f;
    u32 m_beats_per_bar = 4u;
    u32 m_bars_per_phrase = 4u;
    f32 m_seconds_per_beat = 0.5f;

    f32 m_song_time = 0.0f;
    f32 m_beat_fraction = 0.0f;
    u32 m_current_beat = 0u;
    u32 m_current_bar = 0u;
};

/// A single interactive audio stem (e.g. Drums, Bass, Melodic, Ambience, Solo).
struct MusicStem {
    std::string name;
    const AudioBuffer* buffer = nullptr;
    f32 target_gain = 1.0f;
    f32 current_gain = 0.0f;
    f32 fade_speed = 2.0f; // Gain change per second
    f32 min_intensity = 0.0f; // Intensity threshold [0, 1] where stem activates
    f32 max_intensity = 1.0f;
    f64 cursor = 0.0;
    bool solo = false;
    bool muted = false;
    bool active = true;
};

/// Musical stinger / cue played over the interactive music (e.g. victory flourish, threat alert).
struct MusicalStinger {
    const AudioBuffer* buffer = nullptr;
    f32 volume = 1.0f;
    f64 cursor = 0.0;
    bool active = false;
};

/// Interactive dynamic music engine featuring multi-stem vertical layering,
/// combat/exploration intensity mapping, and beat-synchronized bar transitions.
class InteractiveMusicEngine {
public:
    InteractiveMusicEngine(f32 bpm = 120.0f, u32 beats_per_bar = 4u);

    /// Adds a stem track by name.
    void add_stem(const std::string& name, const AudioBuffer* buffer,
                  f32 min_intensity = 0.0f, f32 max_intensity = 1.0f, f32 initial_gain = 0.0f);

    /// Removes a stem track by name.
    bool remove_stem(const std::string& name);

    /// Sets the dynamic gameplay intensity [0.0 = calm/ambient, 1.0 = maximum combat climax].
    /// Automatically adjusts stem target gains based on each stem's activation threshold.
    void set_intensity(f32 intensity, f32 transition_time = 1.0f);
    f32 intensity() const { return m_intensity; }

    /// Sets explicit target gain for an individual stem.
    bool set_stem_gain(const std::string& name, f32 target_gain, f32 fade_time = 0.5f);
    f32 stem_gain(const std::string& name) const;

    /// Mute or unmute an individual stem.
    bool set_stem_muted(const std::string& name, bool muted);

    /// Queues a musical stinger to be played either immediately or aligned with beat/bar.
    void play_stinger(const AudioBuffer* buffer, f32 volume = 1.0f,
                      QuantizationMode quant = QuantizationMode::NextBeat);

    /// Queues an intensity shift or state transition to trigger on a musical boundary.
    void queue_intensity_transition(f32 target_intensity, QuantizationMode quant, f32 transition_time = 1.0f);

    /// Advances the musical clock, executes queued transitions, and updates stem gain envelopes.
    void update(f32 dt);

    /// Mixes all active stems and stingers into the output stereo buffers at the given sample rate.
    void mix(f32* left, f32* right, usize frames, u32 mix_sample_rate = kDefaultSampleRate);

    BeatClock& clock() { return m_clock; }
    const BeatClock& clock() const { return m_clock; }

    size_t stem_count() const { return m_stems.size(); }
    void clear();

private:
    struct QueuedTransition {
        f32 target_intensity = 0.0f;
        f32 transition_time = 1.0f;
        f32 delay_remaining = 0.0f;
        bool active = false;
    };

    struct QueuedStinger {
        MusicalStinger stinger;
        f32 delay_remaining = 0.0f;
        bool active = false;
    };

    BeatClock m_clock;
    f32 m_intensity = 0.0f;
    std::vector<MusicStem> m_stems;
    std::vector<MusicalStinger> m_active_stingers;
    std::vector<QueuedTransition> m_queued_transitions;
    std::vector<QueuedStinger> m_queued_stingers;
};

} // namespace nf::audio
