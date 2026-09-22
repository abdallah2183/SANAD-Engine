// Samples/AudioWalkthrough/main.cpp — G5 acceptance demo: AUDIO THAT SHIPS.
//
// The walkthrough, in one runnable binary, using only public Engine/Audio API
// (no engine source is modified):
//
//   1. MENU   — menu music fades in over an ambience bed, each on its own bus.
//   2. WALL   — a source behind a wall comes through muffled: a 6 kHz hiss is
//               dimmed, a 200 Hz rumble is not.
//   3. CAVE   — a click walks into a reverb zone and the cave answers with
//               discrete echoes at the zone's spacing.
//   4. SLIDERS— bus volumes come from a settings file (`audio.volume.<bus>=v`)
//               and dragging one changes the very next block.
//
// It renders the whole sequence to a 16-bit stereo WAV you can play, prints the
// measured numbers, and exits 0 only when every check holds — so it doubles as
// a smoke test on a machine with no sound card. `--play` pushes the render to
// the real device for the checklist in README.md.
//
// Usage:
//   NFSampleAudioWalkthrough [--settings FILE] [--out FILE.wav] [--play]

#include <NF/Audio/AudioScene.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

// --- report ----------------------------------------------------------------

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// --- arguments -------------------------------------------------------------

struct Args {
    std::string settings_path;              // --settings FILE
    std::string out_path = "audio_walkthrough.wav"; // --out FILE
    bool play = false;                      // --play
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--settings" && i + 1 < argc) {
            args.settings_path = argv[++i];
        } else if (flag == "--out" && i + 1 < argc) {
            args.out_path = argv[++i];
        } else if (flag == "--play") {
            args.play = true;
        } else {
            std::printf("ignoring unknown argument '%s'\n", flag.c_str());
        }
    }
    return args;
}

// --- content ---------------------------------------------------------------

/// A single full-scale frame: the click the cave answers.
AudioBuffer make_click() {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = kDefaultSampleRate;
    buf.samples.assign(1, 1.0f);
    return buf;
}

AudioBuffer make_menu_music() {
    return make_tone_buffer(220.0f, 2.0f, kDefaultSampleRate, 1);
}

AudioBuffer make_ambience_bed() {
    return make_tone_buffer(55.0f, 2.0f, kDefaultSampleRate, 1);
}

AudioBuffer make_hiss() {
    return make_tone_buffer(6000.0f, 1.0f, kDefaultSampleRate, 1);
}

AudioBuffer make_rumble() {
    return make_tone_buffer(200.0f, 1.0f, kDefaultSampleRate, 1);
}

/// The cave the walkthrough walks into.
ReverbZone make_cave() {
    ReverbZone cave;
    cave.position = {0, 0, 100};
    cave.radius = 25.0f;
    cave.inner_radius = 6.0f;
    cave.wet_gain = 0.35f;
    cave.decay_seconds = 1.5f;
    cave.pre_delay_seconds = 0.03f;
    cave.echo_spacing_seconds = 0.11f;
    return cave;
}

OccluderAabb make_wall() {
    OccluderAabb wall;
    wall.min = {-4.0f, -4.0f, 7.9f};
    wall.max = {4.0f, 4.0f, 8.1f};
    return wall;
}

AudioListener listener_at(const Vec3& position) {
    AudioListener listener;
    listener.position = position;
    listener.forward = {0, 0, -1};
    listener.up = {0, 1, 0};
    return listener;
}

// --- measurement -----------------------------------------------------------

/// Output RMS over input RMS, second half of the block so the filter transient
/// is excluded and the window error cancels.
f32 measure_gain(AudioScene& scene, Emitter& emitter,
                 const std::vector<f32>& reference, usize frames) {
    emitter.playing = true;
    emitter.sample_cursor = 0;

    scene.begin_block(frames);
    scene.mix_emitter(emitter);

    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    scene.finalize(left.data(), right.data());

    f64 sum_in = 0.0;
    f64 sum_out = 0.0;
    for (usize i = frames / 2; i < frames; ++i) {
        sum_in += static_cast<f64>(reference[i]) * static_cast<f64>(reference[i]);
        sum_out += static_cast<f64>(left[i]) * static_cast<f64>(left[i]);
    }
    return sum_in > 0.0 ? static_cast<f32>(std::sqrt(sum_out / sum_in)) : 0.0f;
}

// --- WAV output ------------------------------------------------------------

bool write_wav16(const std::string& path, const std::vector<f32>& left,
                 const std::vector<f32>& right, u32 sample_rate) {
    if (left.size() != right.size() || left.empty()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const u16 channels = 2;
    const u16 bits = 16;
    const u32 frames = static_cast<u32>(left.size());
    const u32 data_bytes = frames * channels * (bits / 8u);
    const u32 byte_rate = sample_rate * channels * (bits / 8u);
    const u16 block_align = static_cast<u16>(channels * (bits / 8u));

    auto put_u16 = [&out](u16 v) {
        const char bytes[2] = {static_cast<char>(v & 0xFFu),
                               static_cast<char>((v >> 8) & 0xFFu)};
        out.write(bytes, 2);
    };
    auto put_u32 = [&out](u32 v) {
        const char bytes[4] = {static_cast<char>(v & 0xFFu),
                               static_cast<char>((v >> 8) & 0xFFu),
                               static_cast<char>((v >> 16) & 0xFFu),
                               static_cast<char>((v >> 24) & 0xFFu)};
        out.write(bytes, 4);
    };

    out.write("RIFF", 4);
    put_u32(36u + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(16u);                       // PCM header size
    put_u16(static_cast<u16>(1u));      // format: PCM
    put_u16(channels);
    put_u32(sample_rate);
    put_u32(byte_rate);
    put_u16(block_align);
    put_u16(bits);
    out.write("data", 4);
    put_u32(data_bytes);

    for (usize i = 0; i < left.size(); ++i) {
        const f32 l = std::clamp(left[i], -1.0f, 1.0f);
        const f32 r = std::clamp(right[i], -1.0f, 1.0f);
        put_u16(static_cast<u16>(static_cast<std::int16_t>(std::lround(l * 32767.0f))));
        put_u16(static_cast<u16>(static_cast<std::int16_t>(std::lround(r * 32767.0f))));
    }
    return out.good();
}

// --- settings file ---------------------------------------------------------

/// Read `key=value` lines and hand every one to the settings object. Returns
/// how many were ours; the rest belong to other systems and are reported.
usize load_settings_file(const std::string& path, AudioVolumeSettings& settings,
                         usize& foreign) {
    std::ifstream in(path);
    if (!in) {
        return 0;
    }
    usize applied = 0;
    foreign = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const usize eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (settings.apply_setting(line.substr(0, eq), line.substr(eq + 1))) {
            ++applied;
        } else {
            ++foreign;
        }
    }
    return applied;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    constexpr usize kBlock = 4410; // 0.1 s at 44100
    const u32 rate = kDefaultSampleRate;

    // ---------------------------------------------------------------------
    // Settings — the sliders a Settings window binds (see AudioVolumeSettings)
    // ---------------------------------------------------------------------
    std::printf("== sliders ==\n");
    AudioVolumeSettings settings;
    if (!args.settings_path.empty()) {
        usize foreign = 0;
        const usize applied = load_settings_file(args.settings_path, settings, foreign);
        if (applied == 0 && foreign == 0) {
            std::printf("  settings file '%s' not readable — writing the defaults\n",
                        args.settings_path.c_str());
            std::ofstream out(args.settings_path);
            std::string text;
            settings.append_settings_text(text);
            out << text;
        } else {
            std::printf("  loaded %zu audio slider(s) from '%s' (%zu other key(s) ignored)\n",
                        applied, args.settings_path.c_str(), foreign);
        }
    }
    for (u32 i = 0; i < kBusCount; ++i) {
        std::printf("  %-9s = %.3f\n", bus_name(static_cast<BusId>(i)),
                    static_cast<double>(settings.volume(static_cast<BusId>(i))));
    }
    std::string settings_text;
    settings.append_settings_text(settings_text);
    std::printf("  persisted form:\n");
    std::printf("%s", settings_text.c_str());

    // ---------------------------------------------------------------------
    // The walkthrough render
    // ---------------------------------------------------------------------
    const AudioBuffer menu = make_menu_music();
    const AudioBuffer bed = make_ambience_bed();
    const AudioBuffer hiss = make_hiss();
    const AudioBuffer rumble = make_rumble();
    const AudioBuffer click = make_click();

    AudioScene scene;
    scene.settings() = settings;
    scene.add_zone(make_cave());

    std::vector<f32> left;
    std::vector<f32> right;

    auto render_block = [&]() {
        std::vector<f32> l(scene.block_frames(), 0.0f);
        std::vector<f32> r(scene.block_frames(), 0.0f);
        scene.finalize(l.data(), r.data());
        left.insert(left.end(), l.begin(), l.end());
        right.insert(right.end(), r.begin(), r.end());
    };

    // --- 1. MENU: music fades in over the ambience bed, in open air. --------
    constexpr int kMenuBlocks = 10;
    scene.set_listener(listener_at({0, 0, -30})); // far from the cave: dry
    MusicTrack menu_track;
    menu_track.buffer = &menu;
    menu_track.base_volume = 0.5f;
    scene.music().play(menu_track, 0.5f);
    scene.music().set_ambience(&bed, 1.0f);
    for (int b = 0; b < kMenuBlocks; ++b) {
        scene.begin_block(kBlock, rate);
        render_block();
    }

    // --- 2. WALL: a source behind a wall, then the level goes quiet. --------
    constexpr int kWallBlocks = 8;
    scene.set_listener(listener_at({0, 0, 0}));
    scene.add_occluder(make_wall());
    Emitter radio;
    radio.buffer = &hiss;
    radio.position = {0, 0, 14}; // behind the wall at z = 8
    radio.volume = 0.6f;
    radio.looping = true;
    radio.playing = true;
    Emitter engine;
    engine.buffer = &rumble;
    engine.position = {0, 0, 14};
    engine.volume = 0.6f;
    engine.looping = true;
    engine.playing = true;
    for (int b = 0; b < kWallBlocks; ++b) {
        scene.begin_block(kBlock, rate);
        if (b < 4) { // fade the menu out while still in open air: no echo residue
            scene.mix_emitter(radio);
            scene.mix_emitter(engine);
        } else if (b == 4) {
            scene.music().stop(0.3f);
            scene.music().clear_ambience(0.3f);
        }
        render_block();
    }

    // --- 3. CAVE: a click, and the cave answers. ---------------------------
    constexpr int kCaveBlocks = 14;
    constexpr int kClickBlock = 1; // second block of the cave phase
    scene.clear_occluders();
    scene.set_listener(listener_at({0, 0, 100})); // the heart of the cave
    for (int b = 0; b < kCaveBlocks; ++b) {
        scene.begin_block(kBlock, rate);
        if (b == kClickBlock) {
            Emitter step;
            step.buffer = &click;
            step.playing = true;
            scene.mix_emitter(step);
        }
        render_block();
    }

    const usize click_frame = static_cast<usize>(kMenuBlocks + kWallBlocks +
                                                 kClickBlock) * kBlock;

    // ---------------------------------------------------------------------
    // Checks
    // ---------------------------------------------------------------------
    std::printf("\n== checks ==\n");

    // The rendered artifact itself: the first thing after the click is the
    // cave's first echo, exactly pre_delay + spacing frames later. Amplitudes
    // are scaled by the sfx x master sliders, so a settings file that turns
    // the world down still passes the timing check.
    const f32 sfx_scale = scene.settings().volume(BusId::Sfx) *
                          scene.settings().volume(BusId::Master);
    if (sfx_scale > 0.01f) {
        usize first_echo = left.size();
        for (usize i = click_frame + 1; i < left.size(); ++i) {
            if (std::fabs(left[i]) > 1e-4f) {
                first_echo = i;
                break;
            }
        }
        const usize echo_offset = first_echo - click_frame;
        std::printf("  click at frame %zu, first echo at frame %zu (offset %zu)\n",
                    click_frame, first_echo, echo_offset);
        check(std::fabs(left[click_frame] - sfx_scale) < 1e-4f,
              "the click reaches the output at the sfx x master level");
        check(echo_offset == 6174u,
              "the cave answers at 0.03 s + 0.11 s (6174 frames)");
        check(std::fabs(left[first_echo] - 0.35f * sfx_scale) < 1e-3f,
              "the echo returns at the zone's wet gain (0.35)");
    } else {
        std::printf("  sfx bus muted by your settings: artifact echo check skipped\n");
    }

    // Wall muffle, measured on its own scene so nothing else colours it.
    {
        AudioScene wall_scene;
        wall_scene.set_listener(listener_at({0, 0, 0}));
        const usize frames = 44100;
        Emitter behind;
        behind.buffer = &hiss;
        behind.position = {0, 0, 14};

        const f32 open_hiss = measure_gain(wall_scene, behind, hiss.samples, frames);
        check(behind.walls_last == 0u, "open air crosses no wall");

        wall_scene.add_occluder(make_wall());
        const f32 walled_hiss = measure_gain(wall_scene, behind, hiss.samples, frames);
        check(behind.walls_last == 1u, "the wall test sees exactly one wall");
        check(std::fabs(behind.occlusion_last - 0.5f) < 1e-6f,
              "one wall is half the openness");

        behind.buffer = &rumble;
        const f32 walled_rumble = measure_gain(wall_scene, behind, rumble.samples, frames);

        std::printf("  wall muffle: 6 kHz %.3f open -> %.3f behind a wall; 200 Hz %.3f\n",
                    static_cast<double>(open_hiss), static_cast<double>(walled_hiss),
                    static_cast<double>(walled_rumble));
        check(walled_hiss < 0.45f, "a 6 kHz hiss behind one wall is dimmed below half");
        check(walled_rumble > 0.95f, "a 200 Hz rumble behind the same wall still comes through");
    }

    // The slider is a real, immediate control on the mix. Measured as the
    // output/input RMS ratio over the block, which is base_volume x bus volume
    // — a single sample would only tell us the sine's instantaneous phase.
    {
        AudioScene slider_scene;
        slider_scene.set_listener(listener_at({0, 0, 0}));
        MusicTrack track;
        track.buffer = &menu;
        track.base_volume = 0.5f;
        slider_scene.music().play(track, 0.0f);

        auto mix_ratio = [&](usize cursor) {
            std::vector<f32> l(kBlock, 0.0f);
            std::vector<f32> r(kBlock, 0.0f);
            slider_scene.begin_block(kBlock, rate);
            slider_scene.finalize(l.data(), r.data());
            f64 sum_in = 0.0;
            f64 sum_out = 0.0;
            for (usize i = kBlock / 2; i < kBlock; ++i) {
                // The music cursor starts at 0 and advances one block per mix.
                const f64 in = static_cast<f64>(menu.samples[cursor + i]);
                sum_in += in * in;
                sum_out += static_cast<f64>(l[i]) * static_cast<f64>(l[i]);
            }
            return sum_in > 0.0 ? static_cast<f32>(std::sqrt(sum_out / sum_in)) : 0.0f;
        };

        const f32 at_default = mix_ratio(0);
        slider_scene.settings().set_volume(BusId::Music, 0.4f);
        const f32 at_half = mix_ratio(kBlock);

        std::printf("  music slider: gain %.3f at the 0.8 default -> %.3f at 0.4\n",
                    static_cast<double>(at_default), static_cast<double>(at_half));
        check(std::fabs(at_default - 0.4f) < 2e-3f, "music 0.5 x bus 0.8 = 0.4");
        check(std::fabs(at_half - 0.2f) < 2e-3f, "dragging the slider to 0.4 halves it");
    }

    // ---------------------------------------------------------------------
    // Output
    // ---------------------------------------------------------------------
    const f32 seconds = static_cast<f32>(left.size()) / static_cast<f32>(rate);
    if (write_wav16(args.out_path, left, right, rate)) {
        std::printf("\nwrote %s (%.2f s, %zu frames)\n", args.out_path.c_str(),
                    static_cast<double>(seconds), left.size());
    } else {
        std::printf("\nFAILED to write %s\n", args.out_path.c_str());
        ++g_failures;
    }

    if (args.play) {
        auto device = create_output_device();
        if (device && device->accepts_push()) {
            std::printf("playing on %s ...\n", device->backend_name());
            constexpr usize kChunk = 8192;
            usize cursor = 0;
            auto push_upto = [&](usize end) {
                while (cursor < end) {
                    const usize n = std::min(kChunk, end - cursor);
                    device->submit_mix(left.data() + cursor, right.data() + cursor, n);
                    cursor += n;
                }
            };
            push_upto(std::min<usize>(32768, left.size()));
            while (cursor < left.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(1000.0 * static_cast<double>(kChunk) /
                                     static_cast<double>(rate))));
                push_upto(std::min<usize>(cursor + kChunk, left.size()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            device->shutdown();
        } else {
            std::printf("--play: no push-capable device on this machine; %s is the artifact.\n",
                        args.out_path.c_str());
        }
    }

    std::printf("\n%s\n", g_failures == 0 ? "DEMO PASSED" : "DEMO FAILED");
    return g_failures == 0 ? 0 : 1;
}
