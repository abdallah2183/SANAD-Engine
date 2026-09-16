// NF/Audio/AudioDecoder.cpp — WAV/OGG/MP3/FLAC -> AudioBuffer.
//
// Backends: miniaudio decodes WAV/MP3/FLAC (decode-only, never hardware);
// OGG-Vorbis goes through the vendored stb_vorbis directly because
// miniaudio 0.11.25's bundled vorbis fork rejects known-good Vorbis files
// (verified against stock stb_vorbis v1.22 — see ThirdParty/stb).
// Channel/resample conversion is one shared songsheets: every backend
// decodes native, then convert_audio_format() applies DecodeOptions.

#include <NF/Audio/AudioDecoder.hpp>
#include <NF/Core/Logger.hpp>

#include <miniaudio.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace nf::audio {

// --- OGG Vorbis backend (stb_vorbis) ----------------------------------------
// Kept in an anonymous namespace: only decode_audio_memory routes here.

namespace {

bool decode_ogg_memory(const void* data, usize size, AudioBuffer& out_buf, std::string& out_error) {
    if (size > static_cast<usize>((std::numeric_limits<int>::max)())) {
        out_error = "ogg data exceeds decoder limits";
        return false;
    }
    int stb_error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(static_cast<const unsigned char*>(data),
                                           static_cast<int>(size), &stb_error, nullptr);
    if (!v) {
        out_error = std::string("invalid ogg-vorbis stream (stb_vorbis error ") +
                    std::to_string(stb_error) + ")";
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    if (info.channels <= 0 || info.sample_rate == 0) {
        stb_vorbis_close(v);
        out_error = "ogg-vorbis stream has no channels";
        return false;
    }
    const u32 channels = static_cast<u32>(info.channels);
    std::vector<f32> pcm;
    std::vector<f32> chunk(4096 * channels);
    for (;;) {
        // Returns samples decoded PER CHANNEL (0 = end of stream).
        const int got = stb_vorbis_get_samples_float_interleaved(
            v, static_cast<int>(channels), chunk.data(), static_cast<int>(chunk.size()));
        if (got <= 0) break;
        pcm.insert(pcm.end(), chunk.data(),
                   chunk.data() + static_cast<usize>(got) * channels);
    }
    stb_vorbis_close(v);
    if (pcm.empty()) {
        out_error = "ogg-vorbis stream decoded to zero frames";
        return false;
    }
    out_buf.samples = std::move(pcm);
    out_buf.channels = channels;
    out_buf.sample_rate = info.sample_rate;
    return true;
}

} // namespace

AudioBuffer convert_audio_format(const AudioBuffer& src, u32 target_channels,
                                 u32 target_sample_rate) {
    u32 dst_channels = (target_channels == 1 || target_channels == 2)
                           ? target_channels
                           : src.channels;
    u32 dst_rate = target_sample_rate != 0 ? target_sample_rate : src.sample_rate;
    if (src.channels == 0 || src.sample_rate == 0 || src.samples.empty()) return {};
    if (dst_channels == src.channels && dst_rate == src.sample_rate) return src;

    const usize src_frames = src.frame_count();
    const usize dst_frames = dst_rate == src.sample_rate
                                 ? src_frames
                                 : static_cast<usize>((static_cast<u64>(src_frames) * dst_rate +
                                                       src.sample_rate / 2) /
                                                      src.sample_rate);
    AudioBuffer dst;
    dst.channels = dst_channels;
    dst.sample_rate = dst_rate;
    dst.samples.resize(dst_frames * dst_channels);

    // Resample position in source-frame units, then mix channels.
    for (usize i = 0; i < dst_frames; ++i) {
        const f32 src_pos = dst_rate == src.sample_rate
                                ? static_cast<f32>(i)
                                : static_cast<f32>(i) * static_cast<f32>(src.sample_rate) /
                                      static_cast<f32>(dst_rate);
        usize i0 = static_cast<usize>(src_pos);
        f32 frac = src_pos - static_cast<f32>(i0);
        if (i0 >= src_frames) {
            i0 = src_frames - 1;
            frac = 0.0f;
        }
        usize i1 = i0 + 1 < src_frames ? i0 + 1 : i0;
        for (u32 c = 0; c < dst_channels; ++c) {
            // Map destination channel to source channel(s).
            f32 a = 0.0f, b = 0.0f;
            if (src.channels == 1) {
                a = src.samples[i0];
                b = src.samples[i1];
            } else if (dst_channels == 1) {
                // Mixdown: average all source channels.
                for (u32 s = 0; s < src.channels; ++s) {
                    a += src.samples[i0 * src.channels + s];
                    b += src.samples[i1 * src.channels + s];
                }
                a /= static_cast<f32>(src.channels);
                b /= static_cast<f32>(src.channels);
            } else {
                const u32 s = c < src.channels ? c : src.channels - 1;
                a = src.samples[i0 * src.channels + s];
                b = src.samples[i1 * src.channels + s];
            }
            dst.samples[i * dst_channels + c] = a + (b - a) * frac;
        }
    }
    return dst;
}

AudioFileFormat detect_audio_format(const void* data, usize size) {
    if (data == nullptr || size < 4) return AudioFileFormat::Unknown;
    const auto* b = static_cast<const u8*>(data);
    // WAV: "RIFF"...."WAVE"
    if (size >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVE", 4) == 0) {
        return AudioFileFormat::Wav;
    }
    // OGG: "OggS"
    if (std::memcmp(b, "OggS", 4) == 0) return AudioFileFormat::OggVorbis;
    // FLAC: "fLaC"
    if (std::memcmp(b, "fLaC", 4) == 0) return AudioFileFormat::Flac;
    // MP3: ID3 tag, or a frame sync (0xFF + top 3 bits set), possibly after
    // leading padding. Scan the first 4KB for either marker.
    const usize scan = size < 4096 ? size : 4096;
    if (scan >= 3 && std::memcmp(b, "ID3", 3) == 0) return AudioFileFormat::Mp3;
    for (usize i = 0; i + 1 < scan; ++i) {
        if (b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0) return AudioFileFormat::Mp3;
    }
    return AudioFileFormat::Unknown;
}

const char* audio_format_name(AudioFileFormat format) {
    switch (format) {
        case AudioFileFormat::Wav: return "wav";
        case AudioFileFormat::OggVorbis: return "ogg";
        case AudioFileFormat::Mp3: return "mp3";
        case AudioFileFormat::Flac: return "flac";
        case AudioFileFormat::Unknown: break;
    }
    return "unknown";
}

DecodeResult decode_audio_memory(const void* data, usize size, const DecodeOptions& options) {
    DecodeResult out;
    if (data == nullptr || size == 0) {
        out.error = "decode_audio_memory: empty input";
        return out;
    }
    out.format = detect_audio_format(data, size);

    // OGG takes the stb_vorbis path (see file header); everything else goes
    // through miniaudio, always decoding native (conversion is shared below).
    if (out.format == AudioFileFormat::OggVorbis) {
        if (!decode_ogg_memory(data, size, out.buffer, out.error)) {
            NF_LOG_WARN(LogCategory::Audio, "AudioDecoder: {}", out.error);
            return out;
        }
    } else {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);

    ma_decoder decoder;
    if (ma_decoder_init_memory(data, size, &config, &decoder) != MA_SUCCESS) {
        out.error = std::string("unsupported or corrupt audio data (sniffed: ") +
                    audio_format_name(out.format) + ")";
        NF_LOG_WARN(LogCategory::Audio, "AudioDecoder: {}", out.error);
        return out;
    }

    // Chunked reads: works for every container (some, e.g. MP3 streams,
    // cannot report a length up front), and bounds a single allocation.
    constexpr ma_uint64 kChunkFrames = 4096;
    std::vector<f32> pcm;
    pcm.reserve(static_cast<usize>(kChunkFrames) * (decoder.outputChannels > 0 ? decoder.outputChannels : 1));
    std::vector<f32> chunk(static_cast<usize>(kChunkFrames) * decoder.outputChannels);
    for (;;) {
        ma_uint64 read = 0;
        ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &read);
        if (read > 0) {
            pcm.insert(pcm.end(), chunk.data(),
                       chunk.data() + static_cast<usize>(read) * decoder.outputChannels);
        }
        if (r != MA_SUCCESS || read == 0) break;
    }
    const u32 channels = decoder.outputChannels;
    const u32 rate = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);

    if (pcm.empty() || channels == 0 || rate == 0) {
        out.error = "audio data decoded to zero frames";
        NF_LOG_WARN(LogCategory::Audio, "AudioDecoder: {}", out.error);
        return out;
    }
    out.buffer.samples = std::move(pcm);
    out.buffer.channels = channels;
    out.buffer.sample_rate = rate;
    }

    out.buffer = convert_audio_format(out.buffer, options.target_channels,
                                      options.target_sample_rate);
    out.ok = true;
    NF_LOG_INFO(LogCategory::Audio, "AudioDecoder: decoded {} ({} ch, {} Hz, {:.2f}s)",
                audio_format_name(out.format), out.buffer.channels,
                out.buffer.sample_rate, out.buffer.duration_seconds());
    return out;
}

DecodeResult decode_audio_file(const std::string& path, const DecodeOptions& options) {
    DecodeResult out;
    if (path.empty()) {
        out.error = "decode_audio_file: empty path";
        return out;
    }
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        out.error = std::string("cannot open audio file: ") + path;
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(f);
        out.error = std::string("audio file is empty: ") + path;
        return out;
    }
    std::vector<u8> bytes(static_cast<usize>(len));
    const usize got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) {
        out.error = std::string("short read on audio file: ") + path;
        return out;
    }
    out = decode_audio_memory(bytes.data(), bytes.size(), options);
    if (!out.ok) out.error += std::string(" [") + path + "]";
    return out;
}

} // namespace nf::audio
