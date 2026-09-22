#pragma once

#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/Buses.hpp>
#include <NF/Audio/MusicSystem.hpp>
#include <NF/Audio/Occlusion.hpp>
#include <NF/Audio/Reverb.hpp>
#include <vector>

namespace nf::audio {

/// One sound the scene mixes: a buffer, where it is, and which bus it feeds.
///
/// The caller owns the struct and keeps it alive across blocks — it carries
/// the playback cursor and the occlusion filter's state, which is exactly what
/// makes a wall's muffle continuous instead of resetting every block. This is
/// the same ownership shape as `AudioComponent`, one level lower.
struct Emitter {
    // --- authored -----------------------------------------------------------
    const AudioBuffer* buffer = nullptr;
    Vec3 position{};
    f32 volume = 1.0f;
    bool playing = false;
    bool looping = false;
    bool spatial = false;
    /// Run the wall test between the listener and `position`. Turn it off for
    /// sounds that are meant to ignore geometry (a UI blip, narration).
    bool occluded = true;
    BusId bus = BusId::Sfx;
    SpatialSettings spatial_settings;

    // --- state the scene keeps for you (do not reset between blocks) --------
    usize sample_cursor = 0;
    LowPassFilter filter;

    // --- written by mix_emitter, for HUDs, logs and tests -------------------
    /// Walls between the listener and `position` in the last block.
    u32 walls_last = 0;
    /// 1 - openness for `walls_last`; 0 in open air.
    f32 occlusion_last = 0.0f;
};

/// ---------------------------------------------------------------------------
/// AudioScene — everything a level's audio needs, mixed in device blocks.
/// ---------------------------------------------------------------------------
///
/// The engine's audio module ships the pieces (reverb zones, occlusion, a
/// music/ambience system, buses and volume settings) but a game needs them
/// composed in the right order, per block:
///
///   scene.begin_block(frames);            // once per block
///   scene.mix_emitter(footstep);          // once per emitter, any order
///   scene.mix_emitter(dripping_water);
///   scene.finalize(out_left, out_right);  // music + ambience + reverb + buses
///
/// Signal order inside one block:
///   emitter -> 3D pan/attenuation -> wall low-pass -> its bus
///   music / ambience -> their buses
///   (sfx + ambience) -> echo tail -> wet back into the sfx bus
///   buses * bus volumes * master -> out
///
/// `finalize` ADDS onto the output buffers, matching `BusMixer::finalize`, so
/// a caller can prime them with the device's own base mix first.
///
/// The volumes live in `settings()`, which is THE object a Settings window
/// binds (see the contract on `AudioVolumeSettings`).
class AudioScene {
public:
    AudioScene();

    /// --- Settings-window binding point -------------------------------------
    /// One slider per bus; see AudioVolumeSettings for the read/write/persist
    /// recipe. The mixer is already pointed at this object, so a slider takes
    /// effect on the next block with no reconfiguration.
    AudioVolumeSettings& settings() { return m_settings; }
    const AudioVolumeSettings& settings() const { return m_settings; }

    /// Menu music and the ambience bed. Envelopes advance inside
    /// begin_block(), so a fade is measured in the block clock.
    MusicSystem& music() { return m_music; }
    const MusicSystem& music() const { return m_music; }

    /// --- World authoring ----------------------------------------------------
    void set_listener(const AudioListener& listener) { m_listener = listener; }
    const AudioListener& listener() const { return m_listener; }

    /// Add a reverb zone; returns its index. Zones are searched in order and
    /// the strongest one containing the listener wins (see compute_reverb_at).
    u32 add_zone(const ReverbZone& zone);
    void clear_zones();
    usize zone_count() const { return m_zones.size(); }
    const ReverbZone& zone(usize index) const { return m_zones[index]; }

    /// Add a wall the wall test can see; returns its index.
    u32 add_occluder(const OccluderAabb& box);
    void clear_occluders();
    usize occluder_count() const { return m_occluders.size(); }

    /// --- Mixing -------------------------------------------------------------
    /// Size the block. `sample_rate` drives the music envelopes, the occlusion
    /// filter and the reverb tail; zero falls back to kDefaultSampleRate.
    void begin_block(usize frames, u32 sample_rate = kDefaultSampleRate);
    /// Mix one emitter for this block: 3D pan/attenuation, then the wall
    /// low-pass, then into `emitter.bus`. Safe to call with a stopped or
    /// bufferless emitter (contributes silence, still updates diagnostics).
    void mix_emitter(Emitter& emitter);
    /// Mix music and ambience, run the reverb tail, and sum the buses onto
    /// `out_left`/`out_right` (each `block_frames()` long). ADDS.
    void finalize(f32* out_left, f32* out_right);

    usize block_frames() const { return m_frames; }
    u32 sample_rate() const { return m_sample_rate; }
    /// The reverb resolved at the listener for the block just mixed.
    const ReverbSample& reverb() const { return m_reverb; }

private:
    void apply_reverb();

    AudioVolumeSettings m_settings;
    BusMixer m_mixer;
    MusicSystem m_music;
    AudioListener m_listener;
    std::vector<ReverbZone> m_zones;
    std::vector<OccluderAabb> m_occluders;

    // Scratch blocks, all `m_frames` long. Reused every block so a steady
    // frame allocates nothing.
    std::vector<f32> m_scratch_left;
    std::vector<f32> m_scratch_right;
    std::vector<f32> m_send_left;   // reverb send: the dry world, mono
    std::vector<f32> m_send_right;
    std::vector<f32> m_tail_left;   // reverb tail out, mono
    std::vector<f32> m_tail_right;

    EchoProcessor m_echo;
    ReverbSample m_reverb;
    /// What m_echo is currently configured for (its tail shape, not its level).
    ReverbSample m_echo_params;
    bool m_echo_configured = false;

    usize m_frames = 0;
    u32 m_sample_rate = kDefaultSampleRate;
};

} // namespace nf::audio
