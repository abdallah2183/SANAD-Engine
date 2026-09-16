// AudioTests — compressed audio import: format sniffing + WAV decode.
//
// Hermetic by design: every positive case synthesizes its WAV bytes in
// memory (tiny PCM writer below), so no binary fixtures and no hardware.
// OGG/MP3/FLAC share the same ma_decoder path; their containers are covered
// by the sniffing tests plus manual verification against real files.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioDecoder.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

// --- Minimal PCM WAV writer (mono/stereo, 16-bit) ---------------------------

void push_u16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
}
void push_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}
void push_tag(std::vector<u8>& b, const char* tag) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(tag[i]));
}

// freqs: one sine per channel, amplitude 0.5.
std::vector<u8> make_wav(u32 sample_rate, const std::vector<f32>& freqs, f32 seconds) {
    const u32 channels = static_cast<u32>(freqs.size());
    const u32 frames = static_cast<u32>(sample_rate * seconds);
    std::vector<u8> b;
    push_tag(b, "RIFF");
    push_u32(b, 36 + frames * channels * 2);
    push_tag(b, "WAVE");
    push_tag(b, "fmt ");
    push_u32(b, 16);
    push_u16(b, 1); // PCM
    push_u16(b, static_cast<u16>(channels));
    push_u32(b, sample_rate);
    push_u32(b, sample_rate * channels * 2);
    push_u16(b, static_cast<u16>(channels * 2));
    push_u16(b, 16);
    push_tag(b, "data");
    push_u32(b, frames * channels * 2);
    constexpr f32 kPi = 3.14159265358979323846f;
    for (u32 i = 0; i < frames; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            const f32 s = 0.5f * std::sin(2.0f * kPi * freqs[c] * static_cast<f32>(i) /
                                          static_cast<f32>(sample_rate));
            const i16 q = static_cast<i16>(std::lround(s * 32767.0f));
            push_u16(b, static_cast<u16>(q));
        }
    }
    return b;
}

f32 buffer_rms(const AudioBuffer& buf) {
    if (buf.samples.empty()) return 0.0f;
    double acc = 0.0;
    for (f32 s : buf.samples) acc += static_cast<double>(s) * s;
    return static_cast<f32>(std::sqrt(acc / buf.samples.size()));
}

} // namespace

// ---------------------------------------------------------------------------
// Format sniffing
// ---------------------------------------------------------------------------

NF_TEST(detect_wav_magic) {
    const u8 wav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    NF_CHECK(detect_audio_format(wav, sizeof(wav)) == AudioFileFormat::Wav);
    // "RIFF" without "WAVE" is not WAV.
    const u8 riff[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'X', 'X', 'X', 'X'};
    NF_CHECK(detect_audio_format(riff, sizeof(riff)) == AudioFileFormat::Unknown);
}

NF_TEST(detect_ogg_flac_magic) {
    const u8 ogg[] = {'O', 'g', 'g', 'S', 0, 0, 0, 0};
    NF_CHECK(detect_audio_format(ogg, sizeof(ogg)) == AudioFileFormat::OggVorbis);
    const u8 flac[] = {'f', 'L', 'a', 'C', 0, 0, 0, 0};
    NF_CHECK(detect_audio_format(flac, sizeof(flac)) == AudioFileFormat::Flac);
}

NF_TEST(detect_mp3_id3_and_frame_sync) {
    const u8 id3[] = {'I', 'D', '3', 4, 0, 0, 0, 0};
    NF_CHECK(detect_audio_format(id3, sizeof(id3)) == AudioFileFormat::Mp3);
    const u8 frame[] = {0xFF, 0xFB, 0x90, 0x00};
    NF_CHECK(detect_audio_format(frame, sizeof(frame)) == AudioFileFormat::Mp3);
}

NF_TEST(detect_rejects_garbage_and_short) {
    NF_CHECK(detect_audio_format(nullptr, 0) == AudioFileFormat::Unknown);
    const u8 tiny[] = {'R', 'I'};
    NF_CHECK(detect_audio_format(tiny, sizeof(tiny)) == AudioFileFormat::Unknown);
    const u8 garbage[] = {'N', 'O', 'T', 'A', 'U', 'D', 'I', 'O', '!', '!', '!', '!'};
    NF_CHECK(detect_audio_format(garbage, sizeof(garbage)) == AudioFileFormat::Unknown);
    NF_CHECK(std::string(audio_format_name(AudioFileFormat::Mp3)) == "mp3");
    NF_CHECK(std::string(audio_format_name(AudioFileFormat::Unknown)) == "unknown");
}

// ---------------------------------------------------------------------------
// WAV decode
// ---------------------------------------------------------------------------

NF_TEST(decode_wav_mono16_sine) {
    auto bytes = make_wav(44100, {440.0f}, 0.1f);
    DecodeResult r = decode_audio_memory(bytes.data(), bytes.size());
    NF_CHECK(r.ok);
    NF_CHECK(r.format == AudioFileFormat::Wav);
    NF_CHECK(r.buffer.channels == 1);
    NF_CHECK(r.buffer.sample_rate == 44100);
    // 4410 frames, exact for PCM WAV.
    NF_CHECK(r.buffer.frame_count() == 4410);
    NF_CHECK_NEAR(r.buffer.duration_seconds(), 0.1f, 1e-4f);
    // First sample is sin(0) == 0; second matches the sine value.
    NF_CHECK_NEAR(r.buffer.samples[0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(r.buffer.samples[1], 0.5f * std::sin(2.0f * 3.14159265f * 440.0f / 44100.0f), 2e-3f);
    // Amplitude 0.5 sine has RMS 0.5/sqrt(2).
    NF_CHECK_NEAR(buffer_rms(r.buffer), 0.5f / 1.41421356f, 2e-2f);
}

NF_TEST(decode_wav_stereo_channels_independent) {
    auto bytes = make_wav(48000, {440.0f, 880.0f}, 0.05f);
    DecodeResult r = decode_audio_memory(bytes.data(), bytes.size());
    NF_CHECK(r.ok);
    NF_CHECK(r.buffer.channels == 2);
    NF_CHECK(r.buffer.sample_rate == 48000);
    NF_CHECK(r.buffer.frame_count() == 2400);
    // Interleaved: left[10] and right[10] follow different frequencies.
    const f32 left = r.buffer.samples[10 * 2 + 0];
    const f32 right = r.buffer.samples[10 * 2 + 1];
    constexpr f32 kPi = 3.14159265358979323846f;
    NF_CHECK_NEAR(left, 0.5f * std::sin(2.0f * kPi * 440.0f * 10.0f / 48000.0f), 2e-3f);
    NF_CHECK_NEAR(right, 0.5f * std::sin(2.0f * kPi * 880.0f * 10.0f / 48000.0f), 2e-3f);
}

NF_TEST(decode_wav_with_resample_and_mixdown) {
    auto bytes = make_wav(44100, {440.0f}, 0.1f);
    DecodeOptions opt;
    opt.target_channels = 1;
    opt.target_sample_rate = 22050;
    DecodeResult r = decode_audio_memory(bytes.data(), bytes.size(), opt);
    NF_CHECK(r.ok);
    NF_CHECK(r.buffer.channels == 1);
    NF_CHECK(r.buffer.sample_rate == 22050);
    // Same duration, half the frames (linear resampler lands within 1 frame).
    NF_CHECK_NEAR(r.buffer.duration_seconds(), 0.1f, 1e-3f);
    NF_CHECK(std::abs(static_cast<long long>(r.buffer.frame_count()) - 2205) <= 2);
    NF_CHECK_NEAR(buffer_rms(r.buffer), 0.5f / 1.41421356f, 5e-2f);
}

// ---------------------------------------------------------------------------
// PCM conversion (channel mix + resample)
// ---------------------------------------------------------------------------

NF_TEST(convert_identity_is_noop) {
    auto bytes = make_wav(44100, {440.0f}, 0.02f);
    DecodeResult r = decode_audio_memory(bytes.data(), bytes.size());
    NF_CHECK(r.ok);
    AudioBuffer c = convert_audio_format(r.buffer, 0, 0);
    NF_CHECK(c.channels == r.buffer.channels);
    NF_CHECK(c.sample_rate == r.buffer.sample_rate);
    NF_CHECK(c.samples.size() == r.buffer.samples.size());
    NF_CHECK(c.samples == r.buffer.samples);
}

NF_TEST(convert_mono_to_stereo_duplicates) {
    AudioBuffer mono;
    mono.channels = 1;
    mono.sample_rate = 44100;
    mono.samples = {0.5f, -0.25f, 0.125f};
    AudioBuffer st = convert_audio_format(mono, 2, 0);
    NF_CHECK(st.channels == 2);
    NF_CHECK(st.sample_rate == 44100);
    NF_CHECK(st.frame_count() == 3);
    NF_CHECK_NEAR(st.samples[0], 0.5f, 1e-6f);
    NF_CHECK_NEAR(st.samples[1], 0.5f, 1e-6f);
    NF_CHECK_NEAR(st.samples[4], 0.125f, 1e-6f);
    NF_CHECK_NEAR(st.samples[5], 0.125f, 1e-6f);
}

NF_TEST(convert_stereo_to_mono_averages) {
    AudioBuffer st;
    st.channels = 2;
    st.sample_rate = 44100;
    st.samples = {1.0f, -1.0f, 0.5f, 0.5f};
    AudioBuffer mono = convert_audio_format(st, 1, 0);
    NF_CHECK(mono.channels == 1);
    NF_CHECK(mono.frame_count() == 2);
    NF_CHECK_NEAR(mono.samples[0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(mono.samples[1], 0.5f, 1e-6f);
}

NF_TEST(convert_resample_halves_frames_keeps_level) {
    auto bytes = make_wav(44100, {440.0f}, 0.1f);
    DecodeResult r = decode_audio_memory(bytes.data(), bytes.size());
    NF_CHECK(r.ok);
    AudioBuffer half = convert_audio_format(r.buffer, 0, 22050);
    NF_CHECK(half.sample_rate == 22050);
    NF_CHECK_NEAR(half.duration_seconds(), 0.1f, 1e-3f);
    NF_CHECK(std::abs(static_cast<long long>(half.frame_count()) - 2205) <= 2);
    NF_CHECK_NEAR(buffer_rms(half), 0.5f / 1.41421356f, 5e-2f);
}

NF_TEST(convert_ignores_unsupported_targets) {
    auto bytes = make_wav(44100, {440.0f}, 0.02f);
    DecodeResult r = decode_audio_memory(bytes.data(), bytes.size());
    NF_CHECK(r.ok);
    AudioBuffer same = convert_audio_format(r.buffer, 7, 0);
    NF_CHECK(same.channels == 1);
    AudioBuffer empty;
    AudioBuffer out = convert_audio_format(empty, 2, 22050);
    NF_CHECK(out.samples.empty());
}

// ---------------------------------------------------------------------------
// Failure paths
// ---------------------------------------------------------------------------

NF_TEST(decode_rejects_empty_and_garbage) {
    DecodeResult e1 = decode_audio_memory(nullptr, 0);
    NF_CHECK(!e1.ok);
    NF_CHECK(!e1.error.empty());

    const char garbage[] = "this is definitely not audio data at all............";
    DecodeResult e2 = decode_audio_memory(garbage, sizeof(garbage));
    NF_CHECK(!e2.ok);
    NF_CHECK(!e2.error.empty());

    // Truncated WAV header sniffs as WAV but cannot decode.
    const u8 trunc[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    DecodeResult e3 = decode_audio_memory(trunc, sizeof(trunc));
    NF_CHECK(!e3.ok);
}

NF_TEST(decode_file_missing_and_roundtrip) {
    DecodeResult m = decode_audio_file("does/not/exist_12345.wav");
    NF_CHECK(!m.ok);
    NF_CHECK(!m.error.empty());

    // Round-trip through a real temp file.
    auto bytes = make_wav(22050, {330.0f}, 0.05f);
    const auto tmp = std::filesystem::temp_directory_path() / "nf_test_decode_roundtrip.wav";
    {
        std::FILE* f = nullptr;
#if defined(_MSC_VER)
        fopen_s(&f, tmp.string().c_str(), "wb");
#else
        f = std::fopen(tmp.string().c_str(), "wb");
#endif
        NF_CHECK(f != nullptr);
        if (f) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    DecodeResult r = decode_audio_file(tmp.string());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    NF_CHECK(r.ok);
    NF_CHECK(r.buffer.channels == 1);
    NF_CHECK(r.buffer.sample_rate == 22050);
    NF_CHECK(r.buffer.frame_count() == 1102); // 22050 * 0.05 = 1102.5 -> 1102
}
