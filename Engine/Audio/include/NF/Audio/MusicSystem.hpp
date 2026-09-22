#pragma once

#include <NF/Audio/AudioEngine.hpp>
#include <NF/Core/Types.hpp>

namespace nf::audio {

/// High-level state of the music layer, readable by HUD/editor/debug UIs.
enum class MusicState : u32 {
    Stopped = 0,
    FadingIn,
    Playing,
    Crossfading,
    FadingOut,
};

/// One music selection: a buffer plus its authored level. The system loops
/// music by design — a menu track that runs out mid-credits is a bug, and a
/// deliberate gap needs an authored fade-out instead.
struct MusicTrack {
    const AudioBuffer* buffer = nullptr;
    f32 base_volume = 1.0f;
};

/// Menu music + ambience, with deterministic linear envelopes.
///
/// Two independent layers:
///  * music — one current track (plus a fading-out predecessor while a
///    crossfade is running), driven by play/crossfade/stop;
///  * ambience — one looped bed (wind, cave drips) with its own fade.
///
/// Both layers mix 2D into whatever accumulators the caller routes to the
/// Music and Ambience buses; bus volumes (AudioVolumeSettings) are applied
/// downstream by the BusMixer, so the sliders and the envelopes multiply.
/// All envelope math is a linear ramp at 1/fade_seconds gain per second —
/// exact, deterministic, testable.
class MusicSystem {
public:
    /// Start `track` with a linear fade-in. If a track is already playing it
    /// moves to the fading-out slot over the same duration (play is therefore
    /// a crossfade when music is live, and an instant start when it is not).
    void play(const MusicTrack& track, f32 fade_in_seconds);
    /// Fade the current track out over `fade_out_seconds` while `next` fades
    /// in over `fade_in_seconds`.
    void crossfade(const MusicTrack& next, f32 fade_out_seconds,
                   f32 fade_in_seconds);
    /// Fade out and stop.
    void stop(f32 fade_out_seconds);
    /// Advance envelopes by `dt` seconds. Call once per frame before mixing.
    void update(f32 dt);

    MusicState state() const { return m_state; }
    f32 current_gain() const { return m_current.gain; }
    f32 outgoing_gain() const { return m_outgoing.gain; }
    bool has_outgoing() const { return m_outgoing.active; }

    /// Mix the active music track(s) into `left`/`right` (both `frames` long).
    /// Advances the sample cursors — call exactly once per block.
    void mix_music(f32* left, f32* right, usize frames);

    /// Set the ambience bed to `buffer` (looped) with a fade-in; use
    /// clear_ambience for a fade-OUT. Replacing a live bed is an instant swap
    /// (there is one ambience slot, unlike music's crossfading pair), so a
    /// hand-off between two beds should be clear_ambience + set_ambience.
    void set_ambience(const AudioBuffer* buffer, f32 fade_in_seconds);
    void clear_ambience(f32 fade_out_seconds);
    f32 ambience_gain() const { return m_ambience.gain; }
    bool has_ambience() const { return m_ambience.active; }
    /// Mix the ambience bed into `left`/`right`. Advances its cursor.
    void mix_ambience(f32* left, f32* right, usize frames);

private:
    struct Slot {
        MusicTrack track{};
        f32 gain = 0.0f;
        /// Gain change per second; positive fades in, negative fades out.
        f32 fade_rate = 0.0f;
        /// Envelope ceiling (1.0 for music/ambience fades in, 0 when leaving).
        f32 target = 0.0f;
        usize cursor = 0;
        bool active = false;
    };

    /// Retire `slot` into the fading-out position (or drop it if there is no
    /// fade left to do) and put `next` in the current position.
    void hand_over(const MusicTrack& next, f32 fade_out_seconds,
                   f32 fade_in_seconds);
    void mix_slot(Slot& slot, f32* left, f32* right, usize frames);

    Slot m_current;
    Slot m_outgoing;
    Slot m_ambience;
    MusicState m_state = MusicState::Stopped;
};

} // namespace nf::audio
