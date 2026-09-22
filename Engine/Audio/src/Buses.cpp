#include <NF/Audio/Buses.hpp>
#include <NF/Audio/AudioEngine.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nf::audio {

const char* bus_name(BusId id) {
    switch (id) {
        case BusId::Master: return "master";
        case BusId::Music: return "music";
        case BusId::Sfx: return "sfx";
        case BusId::Ambience: return "ambience";
        case BusId::Voice: return "voice";
    }
    return "master";
}

bool bus_id_from_name(std::string_view name, BusId& out) {
    for (u32 i = 0; i < kBusCount; ++i) {
        const BusId id = static_cast<BusId>(i);
        if (name == bus_name(id)) {
            out = id;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// AudioVolumeSettings
// ---------------------------------------------------------------------------

f32 AudioVolumeSettings::volume(BusId id) const {
    const u32 index = static_cast<u32>(id);
    if (index >= kBusCount) {
        return 1.0f;
    }
    return volumes[index];
}

bool AudioVolumeSettings::set_volume(BusId id, f32 value) {
    const u32 index = static_cast<u32>(id);
    if (index >= kBusCount) {
        return false;
    }
    const f32 clamped = std::clamp(value, 0.0f, 1.0f);
    if (volumes[index] == clamped) {
        return false;
    }
    volumes[index] = clamped;
    return true;
}

void AudioVolumeSettings::reset_to_defaults() {
    const f32 defaults[kBusCount] = {1.0f, 0.8f, 1.0f, 0.7f, 1.0f};
    for (u32 i = 0; i < kBusCount; ++i) {
        volumes[i] = defaults[i];
    }
}

void AudioVolumeSettings::append_settings_text(std::string& out) const {
    // 9 significant digits: a stored f32 re-reads bit-exactly (the engine
    // serializer convention). Slider values are hand-tuned anyway; exactness
    // beats prettiness here.
    char line[64] = {};
    for (u32 i = 0; i < kBusCount; ++i) {
        std::snprintf(line, sizeof(line), "audio.volume.%s=%.9g\n",
                      bus_name(static_cast<BusId>(i)),
                      static_cast<double>(volumes[i]));
        out += line;
    }
}

bool AudioVolumeSettings::apply_setting(std::string_view key,
                                        std::string_view value) {
    constexpr std::string_view kPrefix = "audio.volume.";
    if (key.size() <= kPrefix.size() ||
        key.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }

    BusId id = BusId::Master;
    if (!bus_id_from_name(key.substr(kPrefix.size()), id)) {
        return false;
    }

    // strtof wants a NUL-terminated string; settings values are short.
    char buffer[64] = {};
    const usize len = std::min(value.size(), sizeof(buffer) - 1);
    std::memcpy(buffer, value.data(), len);

    char* end = nullptr;
    const f32 parsed = std::strtof(buffer, &end);
    if (end == buffer) {
        return false; // not a number — leave the key for whoever owns it
    }
    set_volume(id, parsed);
    return true;
}

// ---------------------------------------------------------------------------
// BusMixer
// ---------------------------------------------------------------------------

void BusMixer::begin_block(usize num_frames) {
    m_frames = num_frames;
    for (u32 b = 0; b < kBusCount; ++b) {
        m_left[b].assign(num_frames, 0.0f);
        m_right[b].assign(num_frames, 0.0f);
    }
}

void BusMixer::mix_source(AudioSource& source,
                          const Vec3& listener_pos,
                          const Vec3& listener_forward,
                          const Vec3& listener_up,
                          usize num_frames,
                          u32 sample_rate,
                          BusId bus) {
    const u32 index = static_cast<u32>(bus);
    if (index >= kBusCount || num_frames == 0) {
        return;
    }
    // Contract is begin_block first; resize defensively so a mis-sized block
    // degrades to correct output instead of writing out of bounds.
    if (m_left[index].size() < num_frames) {
        m_left[index].assign(num_frames, 0.0f);
        m_right[index].assign(num_frames, 0.0f);
        m_frames = num_frames;
    }

    AudioBus router;
    router.volume = 1.0f; // bus volume is applied once, in finalize()
    router.mix_source(source, listener_pos, listener_forward, listener_up,
                      m_left[index].data(), m_right[index].data(),
                      num_frames, sample_rate);
}

void BusMixer::mix_buffer(const f32* left, const f32* right, usize num_frames,
                          BusId bus) {
    const u32 index = static_cast<u32>(bus);
    if (index >= kBusCount || num_frames == 0 || left == nullptr ||
        right == nullptr) {
        return;
    }
    if (m_left[index].size() < num_frames) {
        m_left[index].assign(num_frames, 0.0f);
        m_right[index].assign(num_frames, 0.0f);
        m_frames = num_frames;
    }
    const usize frames = std::min(num_frames, m_left[index].size());
    for (usize i = 0; i < frames; ++i) {
        m_left[index][i] += left[i];
        m_right[index][i] += right[i];
    }
}

void BusMixer::read_bus(BusId bus, f32* out_left, f32* out_right,
                        usize num_frames) const {
    const u32 index = static_cast<u32>(bus);
    if (index >= kBusCount) {
        return;
    }
    const usize frames = std::min(num_frames, m_left[index].size());
    for (usize i = 0; i < frames; ++i) {
        out_left[i] = m_left[index][i];
        out_right[i] = m_right[index][i];
    }
    for (usize i = frames; i < num_frames; ++i) {
        out_left[i] = 0.0f;
        out_right[i] = 0.0f;
    }
}

void BusMixer::finalize(f32* out_left, f32* out_right, usize num_frames) {
    const f32 master =
        m_settings != nullptr ? m_settings->volume(BusId::Master) : 1.0f;

    const usize frames = std::min(m_frames, num_frames);
    for (u32 b = 0; b < kBusCount; ++b) {
        const f32 bus_vol =
            m_settings != nullptr ? m_settings->volume(static_cast<BusId>(b))
                                  : 1.0f;
        const f32 scale = bus_vol * master;
        if (scale == 0.0f) {
            continue; // muted bus: skip the sweep, accumulators clear below
        }
        for (usize i = 0; i < frames; ++i) {
            out_left[i] += m_left[b][i] * scale;
            out_right[i] += m_right[b][i] * scale;
        }
    }

    for (u32 b = 0; b < kBusCount; ++b) {
        std::fill(m_left[b].begin(), m_left[b].end(), 0.0f);
        std::fill(m_right[b].begin(), m_right[b].end(), 0.0f);
    }
}

f32 BusMixer::bus_peak(BusId bus) const {
    const u32 index = static_cast<u32>(bus);
    if (index >= kBusCount) {
        return 0.0f;
    }
    f32 peak = 0.0f;
    for (usize i = 0; i < m_frames; ++i) {
        peak = std::max(peak, std::fabs(m_left[index][i]));
        peak = std::max(peak, std::fabs(m_right[index][i]));
    }
    return peak;
}

} // namespace nf::audio
