#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace nf::audio {

struct AudioSource; // defined in AudioEngine.hpp

/// Mix buses. Every source routes into exactly one bus; buses sum into the
/// master, and each bus's volume — plus the master's — is a Settings slider.
enum class BusId : u32 {
    Master = 0, // final sum; its volume is the master slider
    Music,      // music + ambience system output
    Sfx,        // world sounds: weapons, footsteps, vehicles
    Ambience,   // non-music beds routed here explicitly (weather, crowds)
    Voice,      // dialogue
};

inline constexpr u32 kBusCount = 5;

/// Snake-case bus name, as used in settings keys and logs.
const char* bus_name(BusId id);
/// Reverse of bus_name; false when `name` is not a bus.
bool bus_id_from_name(std::string_view name, BusId& out);

/// ---------------------------------------------------------------------------
/// AudioVolumeSettings — THE SETTINGS-WINDOW BINDING CONTRACT (G3/G9)
/// ---------------------------------------------------------------------------
///
/// A plain value type with no engine dependencies. A Settings window binds a
/// slider per bus:
///
///   read:    f32 v = settings.volume(BusId::Music);
///   write:   settings.set_volume(BusId::Music, slider_value); // clamps 0..1
///            -> true when the value changed (enables Apply buttons cheaply)
///   persist: settings.append_settings_text(text) appends one line per bus,
///            `audio.volume.<bus>=<value>`, e.g. `audio.volume.music=0.8`.
///   load:    feed every `key=value` pair from the settings file to
///            settings.apply_setting(key, value); pairs it does not own
///            return false and must be passed on to other systems.
///
/// Defaults (0..1 scale): master 1.0, music 0.8, sfx 1.0, ambience 0.7,
/// voice 1.0. Values are never negative and never above 1 — amplification
/// belongs to the source, not to a slider that also feeds clip-prone sums.
struct AudioVolumeSettings {
    f32 volumes[kBusCount] = {1.0f, 0.8f, 1.0f, 0.7f, 1.0f};

    f32 volume(BusId id) const;
    /// Clamp to [0, 1] and store. Returns true when the stored value changed.
    bool set_volume(BusId id, f32 value);
    /// Reset every bus to its shipped default.
    void reset_to_defaults();

    /// Append `audio.volume.<bus>=<value>` lines (one per bus) to `out`.
    void append_settings_text(std::string& out) const;
    /// Try to consume one `key=value` pair; true = it was ours and is applied.
    bool apply_setting(std::string_view key, std::string_view value);
};

/// ---------------------------------------------------------------------------
/// BusMixer — routes sources into buses and applies the settings volumes.
/// ---------------------------------------------------------------------------
///
/// Per block: begin_block(frames) -> N x mix_source(...) -> finalize(out_l,
/// out_r). finalize ADDS the fully scaled bus sum onto the output buffers, so
/// the caller can prime them with the device's base mix first. The scaling is
///   out += sum_over_buses( bus_accumulator * bus_volume * master_volume )
/// with volumes taken from the bound AudioVolumeSettings every finalize, so
/// moving a slider takes effect on the next block with no reconfiguration.
class BusMixer {
public:
    /// Size and zero the per-bus accumulators for a block of `num_frames`.
    void begin_block(usize num_frames);
    /// Mix one source into its bus accumulator. `bus` selects the bus (the
    /// source's own spatialisation still applies — this only routes).
    void mix_source(AudioSource& source,
                    const Vec3& listener_pos,
                    const Vec3& listener_forward,
                    const Vec3& listener_up,
                    usize num_frames,
                    u32 sample_rate,
                    BusId bus);
    /// Sum an already-mixed stereo block into a bus accumulator. The music and
    /// ambience layers produce their own stereo output (MusicSystem) rather
    /// than routing one AudioSource at a time, so this is how they reach a bus
    /// and therefore the sliders. `left`/`right` may be null (treated as
    /// silence); `num_frames` beyond the block is ignored.
    void mix_buffer(const f32* left, const f32* right, usize num_frames,
                    BusId bus);
    /// Copy a bus's accumulated block out without clearing it — the effects
    /// send (e.g. the reverb feed) and diagnostics read this. Missing frames
    /// read as silence, so a caller may pass a larger `num_frames` than the
    /// block without reading past the accumulator.
    void read_bus(BusId bus, f32* out_left, f32* out_right,
                  usize num_frames) const;

    /// Sum the buses onto `out_left`/`out_right` with the settings volumes,
    /// then clear the accumulators for the next block.
    void finalize(f32* out_left, f32* out_right, usize num_frames);

    void set_settings(const AudioVolumeSettings* settings) { m_settings = settings; }
    const AudioVolumeSettings* settings() const { return m_settings; }
    /// Frames of the current block (valid between begin_block and finalize).
    usize block_frames() const { return m_frames; }
    /// Peak absolute sample accumulated on a bus during this block (diagnostics).
    f32 bus_peak(BusId bus) const;

private:
    std::vector<f32> m_left[kBusCount];
    std::vector<f32> m_right[kBusCount];
    usize m_frames = 0;
    const AudioVolumeSettings* m_settings = nullptr;
};

} // namespace nf::audio
