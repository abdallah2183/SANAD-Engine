#include <NF/Audio/AudioScene.hpp>
#include <algorithm>

namespace nf::audio {

AudioScene::AudioScene() {
    // The mixer reads the settings every finalize(), so a slider move needs no
    // reconfiguration — this one binding is the whole Settings integration.
    m_mixer.set_settings(&m_settings);
}

u32 AudioScene::add_zone(const ReverbZone& zone) {
    m_zones.push_back(zone);
    return static_cast<u32>(m_zones.size() - 1);
}

void AudioScene::clear_zones() {
    m_zones.clear();
}

u32 AudioScene::add_occluder(const OccluderAabb& box) {
    m_occluders.push_back(box);
    return static_cast<u32>(m_occluders.size() - 1);
}

void AudioScene::clear_occluders() {
    m_occluders.clear();
}

void AudioScene::begin_block(usize frames, u32 sample_rate) {
    m_frames = frames;
    m_sample_rate = sample_rate == 0 ? kDefaultSampleRate : sample_rate;

    m_mixer.begin_block(frames);
    m_scratch_left.assign(frames, 0.0f);
    m_scratch_right.assign(frames, 0.0f);
    m_send_left.assign(frames, 0.0f);
    m_send_right.assign(frames, 0.0f);
    m_tail_left.assign(frames, 0.0f);
    m_tail_right.assign(frames, 0.0f);

    // Music envelopes advance on the block clock, which is the same clock the
    // audio is mixed on — a fade measured against wall time would drift.
    if (frames > 0) {
        m_music.update(static_cast<f32>(frames) /
                       static_cast<f32>(m_sample_rate));
    }
}

void AudioScene::mix_emitter(Emitter& emitter) {
    emitter.walls_last = 0;
    emitter.occlusion_last = 0.0f;

    if (m_frames == 0 || emitter.buffer == nullptr) {
        return;
    }

    // 1. How many walls are between the listener and this sound?
    f32 cutoff = 0.0f; // 0 means "no filter", not "silent"
    if (emitter.occluded && !m_occluders.empty()) {
        emitter.walls_last =
            count_occluders_crossed(m_listener.position, emitter.position,
                                    m_occluders.data(), m_occluders.size());
        if (emitter.walls_last > 0) {
            emitter.occlusion_last = occlusion_amount(emitter.walls_last);
            cutoff = occlusion_lowpass_cutoff(emitter.occlusion_last);
        }
    }

    // 2. Mix the source into a scratch block: AudioBus does the cursor, the
    //    loop and the 3D pan/attenuation.
    std::fill(m_scratch_left.begin(), m_scratch_left.end(), 0.0f);
    std::fill(m_scratch_right.begin(), m_scratch_right.end(), 0.0f);

    AudioSource source;
    source.buffer = emitter.buffer;
    source.volume = emitter.volume;
    source.looping = emitter.looping;
    source.playing = emitter.playing;
    source.sample_cursor = emitter.sample_cursor;
    source.spatial = emitter.spatial;
    source.position = emitter.position;
    source.spatial_settings = emitter.spatial_settings;

    AudioBus router; // volume is applied once, by the BusMixer
    router.mix_source(source, m_listener.position, m_listener.forward,
                      m_listener.up, m_scratch_left.data(),
                      m_scratch_right.data(), m_frames, m_sample_rate);

    emitter.sample_cursor = source.sample_cursor;
    emitter.playing = source.playing;

    // 3. Muffle it. Filtering after the mix is what keeps this per source: a
    //    filter on the whole bus would dim every other sound on that bus too.
    if (cutoff > 0.0f) {
        emitter.filter.set_cutoff(cutoff, m_sample_rate);
        emitter.filter.process_buffer(m_scratch_left.data(),
                                      m_scratch_left.data(), m_frames);
        emitter.filter.process_buffer(m_scratch_right.data(),
                                      m_scratch_right.data(), m_frames);
    } else {
        // Open air is bypass, not a 20 kHz one-pole (which would still colour
        // the sound). Drop any state so a stale tail cannot leak into the dry
        // signal when the next wall appears.
        emitter.filter.reset();
    }

    m_mixer.mix_buffer(m_scratch_left.data(), m_scratch_right.data(), m_frames,
                       emitter.bus);
}

void AudioScene::apply_reverb() {
    // Configure only when the *shape* of the tail changes: configure() clears
    // the comb, so calling it every block would restart the tail each frame.
    // The zone weight rides on wet_gain, which is a per-block multiplier here,
    // so walking around inside a zone does not disturb the tail.
    ReverbSample params = m_reverb;
    params.wet_gain = 1.0f;
    if (!m_echo_configured ||
        params.decay_seconds != m_echo_params.decay_seconds ||
        params.pre_delay_seconds != m_echo_params.pre_delay_seconds ||
        params.echo_spacing_seconds != m_echo_params.echo_spacing_seconds) {
        m_echo.configure(m_sample_rate, params);
        m_echo_params = params;
        m_echo_configured = true;
    }

    // Feed the dry world (sfx + ambience) through the echo. The wet tail sums
    // back into the sfx bus, so the sfx slider fades the tail along with the
    // sound that caused it. A mono send keeps the tail from smearing the
    // stereo image it came from.
    m_mixer.read_bus(BusId::Sfx, m_send_left.data(), m_send_right.data(),
                     m_frames);
    m_mixer.read_bus(BusId::Ambience, m_tail_left.data(), m_tail_right.data(),
                     m_frames);
    for (usize i = 0; i < m_frames; ++i) {
        m_send_left[i] += m_tail_left[i];
    }

    // Reuse the tail buffers for the echo output: the ambience read above has
    // already been folded into the send.
    m_echo.process(m_send_left.data(), m_tail_left.data(), m_frames);
    for (usize i = 0; i < m_frames; ++i) {
        m_tail_left[i] *= m_reverb.wet_gain;
    }
    m_mixer.mix_buffer(m_tail_left.data(), m_tail_left.data(), m_frames,
                       BusId::Sfx);
}

void AudioScene::finalize(f32* out_left, f32* out_right) {
    if (m_frames == 0) {
        return;
    }

    // --- music: into the Music bus, so the music slider owns it -------------
    std::fill(m_scratch_left.begin(), m_scratch_left.end(), 0.0f);
    std::fill(m_scratch_right.begin(), m_scratch_right.end(), 0.0f);
    m_music.mix_music(m_scratch_left.data(), m_scratch_right.data(), m_frames);
    m_mixer.mix_buffer(m_scratch_left.data(), m_scratch_right.data(), m_frames,
                       BusId::Music);

    // --- ambience: its own bus ---------------------------------------------
    std::fill(m_scratch_left.begin(), m_scratch_left.end(), 0.0f);
    std::fill(m_scratch_right.begin(), m_scratch_right.end(), 0.0f);
    m_music.mix_ambience(m_scratch_left.data(), m_scratch_right.data(),
                         m_frames);
    m_mixer.mix_buffer(m_scratch_left.data(), m_scratch_right.data(), m_frames,
                       BusId::Ambience);

    // --- reverb: the space the listener is standing in ----------------------
    m_reverb = compute_reverb_at(m_zones.data(), m_zones.size(),
                                 m_listener.position);
    if (m_reverb.active && m_reverb.wet_gain > 0.0f) {
        apply_reverb();
    } else if (m_echo_configured) {
        // Left the space: drop the tail rather than let it ring into a room
        // that is no longer there.
        m_echo.reset();
        m_echo_configured = false;
    }

    m_mixer.finalize(out_left, out_right, m_frames);
}

} // namespace nf::audio
